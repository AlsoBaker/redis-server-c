#include "resp.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ---------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------- */

/* Find "\r\n" starting at buf[from], searching within buf[0..len).
 * Returns the index of '\r', or -1 if no complete CRLF is present yet. */
static long find_crlf(const char *buf, size_t len, size_t from) {
    if (from >= len) return -1;
    for (size_t i = from; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') return (long)i;
    }
    return -1;
}

/* Parse a (possibly negative) base-10 integer from s[0..len). No leading
 * '+', no whitespace, no leading zeros tolerance issues -- we're strict
 * because this is reading a length prefix, not user-facing input. */
static int parse_ll(const char *s, size_t len, long long *out) {
    if (len == 0) return 0;
    size_t i = 0;
    int neg = 0;
    if (s[0] == '-') {
        neg = 1;
        i = 1;
        if (len == 1) return 0;
    }
    long long v = 0;
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0');
        if (v > RESP_MAX_BULK_LEN * 2) return 0; /* guard against overflow on garbage input */
    }
    *out = neg ? -v : v;
    return 1;
}

static void free_argv_partial(resp_arg_t *argv, size_t filled) {
    for (size_t i = 0; i < filled; i++) free(argv[i].data);
    free(argv);
}

/* ---------------------------------------------------------------------
 * Multibulk parsing: *<argc>\r\n ($<len>\r\n<bytes>\r\n){argc}
 * This is what every real client (redis-cli, redis-benchmark, hiredis) sends.
 * ------------------------------------------------------------------- */
static resp_status_t parse_multibulk(const char *buf, size_t buf_len,
                                      resp_command_t *cmd, size_t *consumed,
                                      const char **err) {
    size_t pos = 0;

    long line_end = find_crlf(buf, buf_len, pos);
    if (line_end < 0) {
        if (buf_len > RESP_MAX_INLINE_LEN) {
            *err = "ERR Protocol error: too big mbulk count string";
            return RESP_PARSE_ERROR;
        }
        return RESP_PARSE_INCOMPLETE;
    }

    long long argc;
    if (!parse_ll(buf + pos + 1, (size_t)line_end - (pos + 1), &argc)) {
        *err = "ERR Protocol error: invalid multibulk length";
        return RESP_PARSE_ERROR;
    }
    pos = (size_t)line_end + 2;

    if (argc <= 0 || argc > RESP_MAX_ARGC) {
        *err = "ERR Protocol error: invalid multibulk length";
        return RESP_PARSE_ERROR;
    }

    resp_arg_t *argv = calloc((size_t)argc, sizeof(resp_arg_t));
    if (!argv) {
        *err = "ERR out of memory";
        return RESP_PARSE_ERROR;
    }
    size_t filled = 0;

    for (long long i = 0; i < argc; i++) {
        long hdr_end = find_crlf(buf, buf_len, pos);
        if (hdr_end < 0) {
            if (buf_len - pos > (size_t)RESP_MAX_INLINE_LEN) {
                free_argv_partial(argv, filled);
                *err = "ERR Protocol error: too big bulk count string";
                return RESP_PARSE_ERROR;
            }
            free_argv_partial(argv, filled);
            return RESP_PARSE_INCOMPLETE;
        }
        if (buf[pos] != '$') {
            free_argv_partial(argv, filled);
            *err = "ERR Protocol error: expected '$', got something else";
            return RESP_PARSE_ERROR;
        }
        long long blen;
        if (!parse_ll(buf + pos + 1, (size_t)hdr_end - (pos + 1), &blen)) {
            free_argv_partial(argv, filled);
            *err = "ERR Protocol error: invalid bulk length";
            return RESP_PARSE_ERROR;
        }
        if (blen < 0 || blen > RESP_MAX_BULK_LEN) {
            free_argv_partial(argv, filled);
            *err = "ERR Protocol error: invalid bulk length";
            return RESP_PARSE_ERROR;
        }
        pos = (size_t)hdr_end + 2;

        /* Need blen bytes of body plus the trailing CRLF. */
        if (buf_len < pos + (size_t)blen + 2) {
            free_argv_partial(argv, filled);
            return RESP_PARSE_INCOMPLETE;
        }
        if (buf[pos + (size_t)blen] != '\r' || buf[pos + (size_t)blen + 1] != '\n') {
            free_argv_partial(argv, filled);
            *err = "ERR Protocol error: expected CRLF after bulk string body";
            return RESP_PARSE_ERROR;
        }

        char *data = malloc((size_t)blen + 1);
        if (!data) {
            free_argv_partial(argv, filled);
            *err = "ERR out of memory";
            return RESP_PARSE_ERROR;
        }
        memcpy(data, buf + pos, (size_t)blen);
        data[blen] = '\0';

        argv[i].data = data;
        argv[i].len = (size_t)blen;
        filled++;
        pos += (size_t)blen + 2;
    }

    cmd->argv = argv;
    cmd->argc = (size_t)argc;
    *consumed = pos;
    return RESP_PARSE_OK;
}

/* ---------------------------------------------------------------------
 * Inline parsing: a single line of whitespace-separated tokens, terminated
 * by "\r\n" or a bare "\n". Real Redis supports this for telnet-style
 * debugging (`nc localhost 6379`, then type `PING`). Quoting isn't
 * supported yet -- plain whitespace split -- noted as a v1 simplification.
 * ------------------------------------------------------------------- */
static resp_status_t parse_inline(const char *buf, size_t buf_len,
                                   resp_command_t *cmd, size_t *consumed,
                                   const char **err) {
    size_t i = 0;
    while (i < buf_len && buf[i] != '\n') i++;
    if (i >= buf_len) {
        if (buf_len > RESP_MAX_INLINE_LEN) {
            *err = "ERR Protocol error: too big inline request";
            return RESP_PARSE_ERROR;
        }
        return RESP_PARSE_INCOMPLETE;
    }

    size_t line_len = i; /* excludes the '\n' */
    if (line_len > 0 && buf[line_len - 1] == '\r') line_len--;
    *consumed = i + 1;

    /* Tokenize buf[0..line_len) on whitespace into a growable argv. */
    size_t cap = 8, argc = 0;
    resp_arg_t *argv = malloc(cap * sizeof(resp_arg_t));
    if (!argv) {
        *err = "ERR out of memory";
        return RESP_PARSE_ERROR;
    }

    size_t p = 0;
    while (p < line_len) {
        while (p < line_len && isspace((unsigned char)buf[p])) p++;
        if (p >= line_len) break;
        size_t start = p;
        while (p < line_len && !isspace((unsigned char)buf[p])) p++;
        size_t tok_len = p - start;

        if (argc == cap) {
            cap *= 2;
            resp_arg_t *grown = realloc(argv, cap * sizeof(resp_arg_t));
            if (!grown) {
                free_argv_partial(argv, argc);
                *err = "ERR out of memory";
                return RESP_PARSE_ERROR;
            }
            argv = grown;
        }
        char *data = malloc(tok_len + 1);
        if (!data) {
            free_argv_partial(argv, argc);
            *err = "ERR out of memory";
            return RESP_PARSE_ERROR;
        }
        memcpy(data, buf + start, tok_len);
        data[tok_len] = '\0';
        argv[argc].data = data;
        argv[argc].len = tok_len;
        argc++;
    }

    /* argc == 0 means a blank line -- a valid no-op, not an error. The
     * caller's command dispatch loop should just skip empty commands and
     * keep reading, mirroring real Redis behavior on blank inline input. */
    cmd->argv = argc ? argv : (free(argv), NULL);
    cmd->argc = argc;
    return RESP_PARSE_OK;
}

/* ---------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------- */
resp_status_t resp_parse(const char *buf, size_t buf_len,
                          resp_command_t *cmd, size_t *bytes_consumed,
                          const char **err_msg) {
    *bytes_consumed = 0;
    cmd->argv = NULL;
    cmd->argc = 0;

    if (buf_len == 0) return RESP_PARSE_INCOMPLETE;

    if (buf[0] == '*') {
        return parse_multibulk(buf, buf_len, cmd, bytes_consumed, err_msg);
    }
    return parse_inline(buf, buf_len, cmd, bytes_consumed, err_msg);
}

void resp_command_free(resp_command_t *cmd) {
    if (!cmd || !cmd->argv) return;
    for (size_t i = 0; i < cmd->argc; i++) free(cmd->argv[i].data);
    free(cmd->argv);
    cmd->argv = NULL;
    cmd->argc = 0;
}

/* ---------------------------------------------------------------------
 * Reply serialization
 * ------------------------------------------------------------------- */
void resp_buf_init(resp_buf_t *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void resp_buf_free(resp_buf_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static void resp_buf_ensure(resp_buf_t *b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    size_t newcap = b->cap ? b->cap * 2 : 64;
    while (newcap < b->len + extra) newcap *= 2;
    b->data = realloc(b->data, newcap);
    b->cap = newcap;
}

static void resp_buf_append(resp_buf_t *b, const char *data, size_t len) {
    resp_buf_ensure(b, len);
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

void resp_reply_simple_string(resp_buf_t *b, const char *s) {
    resp_buf_append(b, "+", 1);
    resp_buf_append(b, s, strlen(s));
    resp_buf_append(b, "\r\n", 2);
}

void resp_reply_error(resp_buf_t *b, const char *s) {
    resp_buf_append(b, "-", 1);
    resp_buf_append(b, s, strlen(s));
    resp_buf_append(b, "\r\n", 2);
}

void resp_reply_integer(resp_buf_t *b, long long v) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), ":%lld\r\n", v);
    resp_buf_append(b, tmp, (size_t)n);
}

void resp_reply_bulk_string(resp_buf_t *b, const char *s, size_t len) {
    char hdr[32];
    int n = snprintf(hdr, sizeof(hdr), "$%zu\r\n", len);
    resp_buf_append(b, hdr, (size_t)n);
    resp_buf_append(b, s, len);
    resp_buf_append(b, "\r\n", 2);
}

void resp_reply_null_bulk(resp_buf_t *b) {
    resp_buf_append(b, "$-1\r\n", 5);
}

void resp_reply_array_header(resp_buf_t *b, size_t count) {
    char hdr[32];
    int n = snprintf(hdr, sizeof(hdr), "*%zu\r\n", count);
    resp_buf_append(b, hdr, (size_t)n);
}

void resp_reply_null_array(resp_buf_t *b) {
    resp_buf_append(b, "*-1\r\n", 5);
}
