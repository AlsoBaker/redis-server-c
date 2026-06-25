/* clock_gettime/CLOCK_REALTIME are POSIX, not ISO C -- see store.c for the
 * full explanation. Must come before any system header. */
#define _POSIX_C_SOURCE 200809L

#include "cmd.h"
#include "persist.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

/* -----------------------------------------------------------------------
 * Persistence state: save path and last-save timestamp.
 * Set by cmd_set_save_path() from the server's main() after arg parsing.
 * --------------------------------------------------------------------- */
static char    g_save_path[4096];  /* empty string = no persistence configured */
static int64_t g_lastsave_time = 0; /* Unix seconds of last successful save */

void cmd_set_save_path(const char *path) {
    if (!path || path[0] == '\0') { g_save_path[0] = '\0'; return; }
    size_t n = strlen(path);
    if (n >= sizeof(g_save_path) - 1) n = sizeof(g_save_path) - 1;
    memcpy(g_save_path, path, n);
    g_save_path[n] = '\0';
}

/* -----------------------------------------------------------------------
 * Small helpers
 * --------------------------------------------------------------------- */
static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Case-insensitive, length-bound comparison of a command argument against
 * a C string literal. Bound by arg->len (not strlen on arg->data) so an
 * argument with an embedded NUL can never accidentally "match" a literal
 * via early string termination -- same binary-safety discipline resp.c
 * and store.c already follow for argument contents. */
static int arg_eq_ci(const resp_arg_t *arg, const char *lit) {
    size_t lit_len = strlen(lit);
    if (arg->len != lit_len) return 0;
    for (size_t i = 0; i < lit_len; i++) {
        if (tolower((unsigned char)arg->data[i]) != tolower((unsigned char)lit[i])) {
            return 0;
        }
    }
    return 1;
}

/* Copy an argument into a small fixed buffer for safe embedding inside a
 * human-readable error message: truncated to dst_cap-1 bytes, lowercased,
 * and with any byte that could break RESP framing or terminal output
 * (CR, LF, other control bytes) replaced with '?'. Without this, a client
 * sending e.g. "FOO\r\n$6\r\nFAKE..." as a "command name" could splice
 * extra bytes into our reply stream via an echoed error message --
 * exactly the kind of RESP-injection-via-error-text bug real Redis has to
 * guard against too. */
static void safe_token(char *dst, size_t dst_cap, const resp_arg_t *arg) {
    size_t n = arg->len < dst_cap - 1 ? arg->len : dst_cap - 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)arg->data[i];
        dst[i] = (c < 0x20 || c == 0x7f) ? '?' : (char)tolower(c);
    }
    dst[n] = '\0';
}

/* Strict base-10 int64 parse: the whole argument must be a valid integer,
 * no surrounding whitespace, no trailing garbage. Mirrors store.c's
 * internal parse_strict_ll (duplicated rather than shared since that one
 * is private to store.c and operates on the same binary-safe contract). */
static int parse_strict_ll(const resp_arg_t *arg, int64_t *out) {
    if (arg->len == 0 || arg->len > 20) return 0; /* INT64_MIN is 20 chars incl. sign */
    char buf[21];
    memcpy(buf, arg->data, arg->len);
    buf[arg->len] = '\0';
    if (isspace((unsigned char)buf[0])) return 0;

    errno = 0;
    char *endptr = NULL;
    long long v = strtoll(buf, &endptr, 10);
    if (errno == ERANGE) return 0;
    if (endptr != buf + arg->len) return 0; /* trailing junk or nothing parsed */
    *out = (int64_t)v;
    return 1;
}

static void reply_wrong_arity(resp_buf_t *out, const resp_command_t *cmd) {
    char name[32];
    safe_token(name, sizeof(name), &cmd->argv[0]);
    char msg[80];
    snprintf(msg, sizeof(msg), "ERR wrong number of arguments for '%s' command", name);
    resp_reply_error(out, msg);
}

/* ---------------------------------------------------------------------
 * Command handlers
 * ------------------------------------------------------------------- */
static void cmd_ping(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    (void)s;
    if (cmd->argc > 2) { reply_wrong_arity(out, cmd); return; }
    if (cmd->argc == 2) {
        resp_reply_bulk_string(out, cmd->argv[1].data, cmd->argv[1].len);
    } else {
        resp_reply_simple_string(out, "PONG");
    }
}

static void cmd_echo(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    (void)s;
    if (cmd->argc != 2) { reply_wrong_arity(out, cmd); return; }
    resp_reply_bulk_string(out, cmd->argv[1].data, cmd->argv[1].len);
}

static void cmd_set(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    if (cmd->argc != 3 && cmd->argc != 5) { reply_wrong_arity(out, cmd); return; }

    int64_t expire_at_ms = 0;
    if (cmd->argc == 5) {
        int is_ex = arg_eq_ci(&cmd->argv[3], "EX");
        int is_px = arg_eq_ci(&cmd->argv[3], "PX");
        if (!is_ex && !is_px) { resp_reply_error(out, "ERR syntax error"); return; }

        int64_t n;
        if (!parse_strict_ll(&cmd->argv[4], &n) || n <= 0) {
            resp_reply_error(out, "ERR invalid expire time in 'set' command");
            return;
        }
        expire_at_ms = now_ms() + (is_ex ? n * 1000 : n);
    }

    store_set(s, cmd->argv[1].data, cmd->argv[1].len,
              cmd->argv[2].data, cmd->argv[2].len, expire_at_ms);
    resp_reply_simple_string(out, "OK");
}

static void cmd_get(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    if (cmd->argc != 2) { reply_wrong_arity(out, cmd); return; }
    const char *val; size_t vlen;
    if (store_get(s, cmd->argv[1].data, cmd->argv[1].len, &val, &vlen)) {
        resp_reply_bulk_string(out, val, vlen);
    } else {
        resp_reply_null_bulk(out);
    }
}

static void cmd_del(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    if (cmd->argc < 2) { reply_wrong_arity(out, cmd); return; }
    long long count = 0;
    for (size_t i = 1; i < cmd->argc; i++) {
        count += store_del(s, cmd->argv[i].data, cmd->argv[i].len);
    }
    resp_reply_integer(out, count);
}

static void cmd_exists(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    if (cmd->argc < 2) { reply_wrong_arity(out, cmd); return; }
    long long count = 0;
    for (size_t i = 1; i < cmd->argc; i++) {
        count += store_exists(s, cmd->argv[i].data, cmd->argv[i].len);
    }
    resp_reply_integer(out, count);
}

static void cmd_expire(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    if (cmd->argc != 3) { reply_wrong_arity(out, cmd); return; }
    int64_t seconds;
    if (!parse_strict_ll(&cmd->argv[2], &seconds)) {
        resp_reply_error(out, "ERR value is not an integer or out of range");
        return;
    }
    int ok = store_expire(s, cmd->argv[1].data, cmd->argv[1].len,
                           now_ms() + seconds * 1000);
    resp_reply_integer(out, ok ? 1 : 0);
}

static void cmd_ttl_generic(store_t *s, const resp_command_t *cmd, resp_buf_t *out, int want_ms) {
    if (cmd->argc != 2) { reply_wrong_arity(out, cmd); return; }
    int64_t ttl_ms = store_ttl_ms(s, cmd->argv[1].data, cmd->argv[1].len);
    if (ttl_ms == STORE_TTL_NO_KEY) { resp_reply_integer(out, -2); return; }
    if (ttl_ms == STORE_TTL_NO_EXPIRY) { resp_reply_integer(out, -1); return; }
    resp_reply_integer(out, want_ms ? ttl_ms : (ttl_ms + 999) / 1000 /* ceil to seconds */);
}

static void cmd_ttl(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    cmd_ttl_generic(s, cmd, out, 0);
}

static void cmd_pttl(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    cmd_ttl_generic(s, cmd, out, 1);
}

static void reply_incr_result(resp_buf_t *out, store_incr_status_t st, int64_t val) {
    switch (st) {
        case STORE_INCR_OK:
            resp_reply_integer(out, val);
            break;
        case STORE_INCR_NOT_INTEGER:
            resp_reply_error(out, "ERR value is not an integer or out of range");
            break;
        case STORE_INCR_OVERFLOW:
            resp_reply_error(out, "ERR increment or decrement would overflow");
            break;
    }
}

static void cmd_incr(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    if (cmd->argc != 2) { reply_wrong_arity(out, cmd); return; }
    int64_t val;
    store_incr_status_t st = store_incrby(s, cmd->argv[1].data, cmd->argv[1].len, 1, &val);
    reply_incr_result(out, st, val);
}

static void cmd_decr(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    if (cmd->argc != 2) { reply_wrong_arity(out, cmd); return; }
    int64_t val;
    store_incr_status_t st = store_incrby(s, cmd->argv[1].data, cmd->argv[1].len, -1, &val);
    reply_incr_result(out, st, val);
}

static void cmd_incrby_generic(store_t *s, const resp_command_t *cmd, resp_buf_t *out, int negate) {
    if (cmd->argc != 3) { reply_wrong_arity(out, cmd); return; }
    int64_t delta;
    if (!parse_strict_ll(&cmd->argv[2], &delta)) {
        resp_reply_error(out, "ERR value is not an integer or out of range");
        return;
    }
    /* INT64_MIN has no positive counterpart, so negating it would overflow
     * silently -- report it the same way store_incrby reports any other
     * overflow rather than letting UB sneak in here in cmd.c. */
    if (negate && delta == INT64_MIN) {
        resp_reply_error(out, "ERR increment or decrement would overflow");
        return;
    }
    int64_t val;
    store_incr_status_t st = store_incrby(s, cmd->argv[1].data, cmd->argv[1].len,
                                           negate ? -delta : delta, &val);
    reply_incr_result(out, st, val);
}

static void cmd_incrby(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    cmd_incrby_generic(s, cmd, out, 0);
}

static void cmd_decrby(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    cmd_incrby_generic(s, cmd, out, 1);
}

/* -----------------------------------------------------------------------
 * Persistence and introspection commands
 * --------------------------------------------------------------------- */
static void cmd_dbsize(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    if (cmd->argc != 1) { reply_wrong_arity(out, cmd); return; }
    resp_reply_integer(out, (long long)store_size(s));
}

static void cmd_flushdb(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    if (cmd->argc != 1) { reply_wrong_arity(out, cmd); return; }
    store_flush(s);
    resp_reply_simple_string(out, "OK");
}

static void cmd_lastsave(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    (void)s;
    if (cmd->argc != 1) { reply_wrong_arity(out, cmd); return; }
    resp_reply_integer(out, (long long)g_lastsave_time);
}

static void do_save(store_t *s, resp_buf_t *out) {
    if (g_save_path[0] == '\0') {
        resp_reply_error(out, "ERR no save path configured (start server with --save <file>)");
        return;
    }
    persist_status_t st = persist_save(s, g_save_path);
    if (st != PERSIST_OK) {
        char msg[128];
        snprintf(msg, sizeof(msg), "ERR save failed: %s", persist_strerror(st));
        resp_reply_error(out, msg);
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    g_lastsave_time = (int64_t)ts.tv_sec;
    resp_reply_simple_string(out, "OK");
}

static void cmd_save(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    if (cmd->argc != 1) { reply_wrong_arity(out, cmd); return; }
    do_save(s, out);
}

static void cmd_bgsave(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    if (cmd->argc != 1) { reply_wrong_arity(out, cmd); return; }
    if (g_save_path[0] == '\0') {
        resp_reply_error(out, "ERR no save path configured (start server with --save <file>)");
        return;
    }

    /* fork(): the child inherits a copy-on-write snapshot of the store.
     * Because the child only reads (iterates via store_foreach) and never
     * modifies, no pages are actually copied -- the OS shares them as
     * read-only until the parent writes something. This gives a consistent
     * point-in-time snapshot at essentially zero cost, exactly how real
     * Redis implements BGSAVE. SIGCHLD is expected to be SIG_IGN in the
     * server so that zombie reaping is automatic. */
    pid_t pid = fork();
    if (pid < 0) {
        resp_reply_error(out, "ERR fork failed");
        return;
    }
    if (pid == 0) {
        /* Child: save and exit. No need to update g_lastsave_time here --
         * the parent's timestamp tracks when the save was *requested*, not
         * when it completed, matching real Redis LASTSAVE semantics. */
        persist_status_t st = persist_save(s, g_save_path);
        _exit(st == PERSIST_OK ? 0 : 1);
    }
    /* Parent: update lastsave time and reply immediately. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    g_lastsave_time = (int64_t)ts.tv_sec;
    resp_reply_simple_string(out, "Background saving started");
}

static void cmd_mget(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    if (cmd->argc < 2) { reply_wrong_arity(out, cmd); return; }
    size_t nkeys = cmd->argc - 1;
    resp_reply_array_header(out, nkeys);
    for (size_t i = 1; i <= nkeys; i++) {
        const char *val; size_t vlen;
        if (store_get(s, cmd->argv[i].data, cmd->argv[i].len, &val, &vlen)) {
            resp_reply_bulk_string(out, val, vlen);
        } else {
            resp_reply_null_bulk(out);
        }
    }
}

static void cmd_mset(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    /* Must have an even number of key-value pairs after the command name. */
    if (cmd->argc < 3 || (cmd->argc % 2) == 0) {
        reply_wrong_arity(out, cmd); return;
    }
    for (size_t i = 1; i < cmd->argc; i += 2) {
        store_set(s, cmd->argv[i].data,   cmd->argv[i].len,
                     cmd->argv[i+1].data, cmd->argv[i+1].len, 0);
    }
    resp_reply_simple_string(out, "OK");
}

/* KEYS pattern -- we support only "*" (all keys) for now, matching how
 * real Redis documents KEYS as an O(N) danger: "never use in production".
 * Groundwork for glob matching lives here; the pattern check is isolated
 * so adding '?' and '[...]' later is a one-function change. */
typedef struct { resp_buf_t *out; size_t count; } keys_ctx_t;

static void keys_collect_cb(const char *key, size_t keylen,
                             const char *val,  size_t vallen,
                             int64_t expire_at_ms, void *ud) {
    (void)val; (void)vallen; (void)expire_at_ms;
    keys_ctx_t *ctx = (keys_ctx_t *)ud;
    resp_reply_bulk_string(ctx->out, key, keylen);
    ctx->count++;
}

static void cmd_keys(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    if (cmd->argc != 2) { reply_wrong_arity(out, cmd); return; }

    /* Only "*" is supported right now. Any other pattern gets an error
     * rather than silently returning a wrong result. */
    if (cmd->argv[1].len != 1 || cmd->argv[1].data[0] != '*') {
        resp_reply_error(out, "ERR only '*' pattern supported in KEYS");
        return;
    }

    /* We don't know the count before iterating, so we emit a placeholder
     * array header, collect the bulk strings into a scratch buffer, then
     * prepend the real count and concatenate. */
    resp_buf_t scratch;
    resp_buf_init(&scratch);
    keys_ctx_t ctx = { &scratch, 0 };
    store_foreach(s, keys_collect_cb, &ctx);

    resp_reply_array_header(out, ctx.count);
    if (scratch.len > 0) {
        /* resp_buf_t has no public append-from-buf API, so reach into the
         * internals just this once. The alternative is a second iteration
         * or a two-pass design; this is simpler and stays internal. */
        if (out->len + scratch.len > out->cap) {
            size_t newcap = out->cap ? out->cap * 2 : 64;
            while (newcap < out->len + scratch.len) newcap *= 2;
            out->data = realloc(out->data, newcap);
            out->cap  = newcap;
        }
        memcpy(out->data + out->len, scratch.data, scratch.len);
        out->len += scratch.len;
    }
    resp_buf_free(&scratch);
}

/* -----------------------------------------------------------------------
 * Dispatch table
 * --------------------------------------------------------------------- */
typedef void (*cmd_handler_t)(store_t *, const resp_command_t *, resp_buf_t *);

typedef struct {
    const char    *name;
    cmd_handler_t  handler;
} cmd_entry_t;

static const cmd_entry_t COMMAND_TABLE[] = {
    { "PING",     cmd_ping     },
    { "ECHO",     cmd_echo     },
    { "SET",      cmd_set      },
    { "GET",      cmd_get      },
    { "MGET",     cmd_mget     },
    { "MSET",     cmd_mset     },
    { "DEL",      cmd_del      },
    { "EXISTS",   cmd_exists   },
    { "KEYS",     cmd_keys     },
    { "EXPIRE",   cmd_expire   },
    { "TTL",      cmd_ttl      },
    { "PTTL",     cmd_pttl     },
    { "INCR",     cmd_incr     },
    { "DECR",     cmd_decr     },
    { "INCRBY",   cmd_incrby   },
    { "DECRBY",   cmd_decrby   },
    { "DBSIZE",   cmd_dbsize   },
    { "FLUSHDB",  cmd_flushdb  },
    { "SAVE",     cmd_save     },
    { "BGSAVE",   cmd_bgsave   },
    { "LASTSAVE", cmd_lastsave },
};
#define COMMAND_TABLE_LEN (sizeof(COMMAND_TABLE) / sizeof(COMMAND_TABLE[0]))

void cmd_dispatch(store_t *s, const resp_command_t *cmd, resp_buf_t *out) {
    if (cmd->argc == 0) return; /* blank inline line: documented no-op */

    for (size_t i = 0; i < COMMAND_TABLE_LEN; i++) {
        if (arg_eq_ci(&cmd->argv[0], COMMAND_TABLE[i].name)) {
            COMMAND_TABLE[i].handler(s, cmd, out);
            return;
        }
    }

    char name[32];
    safe_token(name, sizeof(name), &cmd->argv[0]);
    char msg[80];
    snprintf(msg, sizeof(msg), "ERR unknown command '%s'", name);
    resp_reply_error(out, msg);
}
