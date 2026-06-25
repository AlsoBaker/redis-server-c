#include "cmd.h"
#include "resp.h"
#include "store.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

static void run_section(const char *name) { printf("-- %s\n", name); }

/* Parse a full inline or multibulk command string (must resolve to exactly
 * one complete command -- RESP_PARSE_OK) and assert that. Saves every test
 * below from hand-building resp_command_t structs. */
static void parse_ok(const char *line, resp_command_t *cmd) {
    size_t consumed = 0;
    const char *err = NULL;
    resp_status_t st = resp_parse(line, strlen(line), cmd, &consumed, &err);
    if (st != RESP_PARSE_OK) {
        printf("  parse_ok() helper failed to parse: %s\n", line);
        exit(1);
    }
}

/* Dispatch one inline command string against a fresh resp_buf_t and hand
 * back both the command (so callers can free it) and the reply bytes. */
static void dispatch_line(store_t *s, const char *line, resp_command_t *cmd, resp_buf_t *out) {
    parse_ok(line, cmd);
    resp_buf_init(out);
    cmd_dispatch(s, cmd, out);
}

static int reply_eq(const resp_buf_t *out, const char *expected) {
    size_t elen = strlen(expected);
    return out->len == elen && memcmp(out->data, expected, elen) == 0;
}

/* resp_buf_t is an append-only byte buffer, NOT NUL-terminated -- using
 * strstr() directly on out->data would read past the valid region (and
 * trip ASan). Make an explicitly NUL-terminated copy for substring checks
 * in these tests; caller must free() it. */
static char *nul_terminated_copy(const resp_buf_t *out) {
    char *s = malloc(out->len + 1);
    memcpy(s, out->data, out->len);
    s[out->len] = '\0';
    return s;
}

/* ---------------------------------------------------------------------
 * 1. PING / ECHO
 * ------------------------------------------------------------------- */
static void test_ping_echo(void) {
    run_section("PING / ECHO");
    store_t *s = store_create();
    resp_command_t cmd; resp_buf_t out;

    dispatch_line(s, "PING\r\n", &cmd, &out);
    CHECK(reply_eq(&out, "+PONG\r\n"), "bare PING -> +PONG");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "ping hello\r\n", &cmd, &out); /* lowercase, case-insensitive */
    CHECK(reply_eq(&out, "$5\r\nhello\r\n"), "PING with arg echoes it as bulk string");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "PING a b\r\n", &cmd, &out);
    CHECK(out.len > 0 && out.data[0] == '-', "PING with 2 args is wrong arity -> error reply");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "ECHO hi\r\n", &cmd, &out);
    CHECK(reply_eq(&out, "$2\r\nhi\r\n"), "ECHO returns its argument as bulk string");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "ECHO\r\n", &cmd, &out);
    CHECK(out.len > 0 && out.data[0] == '-', "ECHO with no args is wrong arity -> error reply");
    resp_command_free(&cmd); resp_buf_free(&out);

    store_destroy(s);
}

/* ---------------------------------------------------------------------
 * 2. SET / GET roundtrip, including missing keys and overwrite.
 * ------------------------------------------------------------------- */
static void test_set_get(void) {
    run_section("SET / GET");
    store_t *s = store_create();
    resp_command_t cmd; resp_buf_t out;

    dispatch_line(s, "SET foo bar\r\n", &cmd, &out);
    CHECK(reply_eq(&out, "+OK\r\n"), "SET replies +OK");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "GET foo\r\n", &cmd, &out);
    CHECK(reply_eq(&out, "$3\r\nbar\r\n"), "GET returns the value just set");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "GET missing\r\n", &cmd, &out);
    CHECK(reply_eq(&out, "$-1\r\n"), "GET on missing key returns null bulk");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "SET foo baz\r\n", &cmd, &out);
    CHECK(reply_eq(&out, "+OK\r\n"), "overwrite SET also replies +OK");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "GET foo\r\n", &cmd, &out);
    CHECK(reply_eq(&out, "$3\r\nbaz\r\n"), "GET reflects the overwrite");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "SET onlyone\r\n", &cmd, &out);
    CHECK(out.data[0] == '-', "SET with wrong arity is an error");
    resp_command_free(&cmd); resp_buf_free(&out);

    store_destroy(s);
}

/* ---------------------------------------------------------------------
 * 3. SET with EX/PX expiry options.
 * ------------------------------------------------------------------- */
static void test_set_with_expiry(void) {
    run_section("SET with EX/PX");
    store_t *s = store_create();
    resp_command_t cmd; resp_buf_t out;

    dispatch_line(s, "SET foo bar EX 10\r\n", &cmd, &out);
    CHECK(reply_eq(&out, "+OK\r\n"), "SET ... EX replies +OK");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "PTTL foo\r\n", &cmd, &out);
    /* expect ":<n>\r\n" with 0 < n <= 10000 */
    CHECK(out.len >= 4 && out.data[0] == ':', "PTTL returns an integer reply");
    long long ms = atoll(out.data + 1);
    CHECK(ms > 0 && ms <= 10000, "PTTL reflects the EX 10 we set (in ms)");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "SET baz qux PX 5000\r\n", &cmd, &out);
    CHECK(reply_eq(&out, "+OK\r\n"), "SET ... PX replies +OK");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "TTL baz\r\n", &cmd, &out);
    CHECK(out.len >= 4 && out.data[0] == ':', "TTL returns an integer reply");
    long long sec = atoll(out.data + 1);
    CHECK(sec > 0 && sec <= 5, "TTL reflects PX 5000 rounded up to seconds");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "SET foo bar XX 10\r\n", &cmd, &out);
    CHECK(out.data[0] == '-', "SET with unrecognized option is a syntax error");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "SET foo bar EX -5\r\n", &cmd, &out);
    CHECK(out.data[0] == '-', "SET ... EX with non-positive seconds is rejected");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "SET foo bar EX notanumber\r\n", &cmd, &out);
    CHECK(out.data[0] == '-', "SET ... EX with non-numeric seconds is rejected");
    resp_command_free(&cmd); resp_buf_free(&out);

    store_destroy(s);
}

/* ---------------------------------------------------------------------
 * 4. DEL / EXISTS over multiple keys.
 * ------------------------------------------------------------------- */
static void test_del_exists(void) {
    run_section("DEL / EXISTS");
    store_t *s = store_create();
    resp_command_t cmd; resp_buf_t out;

    dispatch_line(s, "SET a 1\r\n", &cmd, &out); resp_command_free(&cmd); resp_buf_free(&out);
    dispatch_line(s, "SET b 2\r\n", &cmd, &out); resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "EXISTS a b c\r\n", &cmd, &out);
    CHECK(reply_eq(&out, ":2\r\n"), "EXISTS counts only the keys that are present");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "DEL a b c\r\n", &cmd, &out);
    CHECK(reply_eq(&out, ":2\r\n"), "DEL returns count of keys actually removed");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "EXISTS a b c\r\n", &cmd, &out);
    CHECK(reply_eq(&out, ":0\r\n"), "EXISTS is 0 after DEL");
    resp_command_free(&cmd); resp_buf_free(&out);

    store_destroy(s);
}

/* ---------------------------------------------------------------------
 * 5. EXPIRE / TTL.
 * ------------------------------------------------------------------- */
static void test_expire_ttl(void) {
    run_section("EXPIRE / TTL");
    store_t *s = store_create();
    resp_command_t cmd; resp_buf_t out;

    dispatch_line(s, "TTL ghost\r\n", &cmd, &out);
    CHECK(reply_eq(&out, ":-2\r\n"), "TTL on missing key is -2");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "SET k v\r\n", &cmd, &out); resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "TTL k\r\n", &cmd, &out);
    CHECK(reply_eq(&out, ":-1\r\n"), "TTL on key with no expiry is -1");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "EXPIRE ghost 10\r\n", &cmd, &out);
    CHECK(reply_eq(&out, ":0\r\n"), "EXPIRE on missing key returns 0");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "EXPIRE k 10\r\n", &cmd, &out);
    CHECK(reply_eq(&out, ":1\r\n"), "EXPIRE on existing key returns 1");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "TTL k\r\n", &cmd, &out);
    long long sec = atoll(out.data + 1);
    CHECK(sec > 0 && sec <= 10, "TTL now reflects the EXPIRE we just set");
    resp_command_free(&cmd); resp_buf_free(&out);

    store_destroy(s);
}

/* ---------------------------------------------------------------------
 * 6. INCR / DECR / INCRBY / DECRBY, including error paths.
 * ------------------------------------------------------------------- */
static void test_incr_decr(void) {
    run_section("INCR / DECR / INCRBY / DECRBY");
    store_t *s = store_create();
    resp_command_t cmd; resp_buf_t out;

    dispatch_line(s, "INCR counter\r\n", &cmd, &out);
    CHECK(reply_eq(&out, ":1\r\n"), "INCR on missing key starts at 0 -> 1");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "INCRBY counter 41\r\n", &cmd, &out);
    CHECK(reply_eq(&out, ":42\r\n"), "INCRBY adds the given delta");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "DECR counter\r\n", &cmd, &out);
    CHECK(reply_eq(&out, ":41\r\n"), "DECR subtracts one");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "DECRBY counter 41\r\n", &cmd, &out);
    CHECK(reply_eq(&out, ":0\r\n"), "DECRBY subtracts the given delta");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "SET word hello\r\n", &cmd, &out); resp_command_free(&cmd); resp_buf_free(&out);
    dispatch_line(s, "INCR word\r\n", &cmd, &out);
    CHECK(out.data[0] == '-', "INCR on a non-integer value is an error");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "SET atmax 9223372036854775807\r\n", &cmd, &out);
    resp_command_free(&cmd); resp_buf_free(&out);
    dispatch_line(s, "INCR atmax\r\n", &cmd, &out);
    CHECK(out.data[0] == '-', "INCR past INT64_MAX is an error");
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "INCRBY counter notanumber\r\n", &cmd, &out);
    CHECK(out.data[0] == '-', "INCRBY with a non-numeric delta is an error");
    resp_command_free(&cmd); resp_buf_free(&out);

    /* DECRBY by INT64_MIN can't be negated without overflow -- must be
     * caught explicitly rather than triggering signed-overflow UB. */
    dispatch_line(s, "DECRBY counter -9223372036854775808\r\n", &cmd, &out);
    CHECK(out.data[0] == '-', "DECRBY INT64_MIN is rejected, not UB");
    resp_command_free(&cmd); resp_buf_free(&out);

    store_destroy(s);
}

/* ---------------------------------------------------------------------
 * 7. Unknown commands and wrong-arity error formatting.
 * ------------------------------------------------------------------- */
static void test_unknown_and_arity_errors(void) {
    run_section("unknown command / wrong arity errors");
    store_t *s = store_create();
    resp_command_t cmd; resp_buf_t out;

    dispatch_line(s, "FROBNICATE x y\r\n", &cmd, &out);
    CHECK(out.data[0] == '-', "unknown command is an error reply");
    char *msg1 = nul_terminated_copy(&out);
    CHECK(strstr(msg1, "unknown command") != NULL, "error message says 'unknown command'");
    CHECK(strstr(msg1, "frobnicate") != NULL, "error message includes the (lowercased) command name");
    free(msg1);
    resp_command_free(&cmd); resp_buf_free(&out);

    dispatch_line(s, "GET\r\n", &cmd, &out);
    CHECK(out.data[0] == '-', "GET with no key is wrong arity");
    char *msg2 = nul_terminated_copy(&out);
    CHECK(strstr(msg2, "wrong number of arguments") != NULL, "arity error message is descriptive");
    free(msg2);
    resp_command_free(&cmd); resp_buf_free(&out);

    store_destroy(s);
}

/* ---------------------------------------------------------------------
 * 8. Empty command (blank inline line) is a true no-op: nothing at all
 *    gets appended to the reply buffer, not even an empty reply token.
 * ------------------------------------------------------------------- */
static void test_empty_command_is_noop(void) {
    run_section("empty command is a no-op");
    store_t *s = store_create();
    resp_command_t cmd; resp_buf_t out;

    dispatch_line(s, "\r\n", &cmd, &out);
    CHECK(cmd.argc == 0, "blank line parses to argc == 0");
    CHECK(out.len == 0, "dispatching an empty command appends nothing to the reply buffer");
    resp_command_free(&cmd); resp_buf_free(&out);

    store_destroy(s);
}

/* ---------------------------------------------------------------------
 * 9. RESP-injection guard: a multibulk "command name" containing raw
 *    CRLF bytes (legal per RESP's binary-safety rules) must not let those
 *    bytes leak unescaped into our error reply and corrupt the framing
 *    for whatever the client reads next.
 * ------------------------------------------------------------------- */
static void test_error_message_sanitizes_command_name(void) {
    run_section("error messages sanitize binary-unsafe command names");
    store_t *s = store_create();

    /* A multibulk command whose single argument is the 6 bytes "FO\r\nBA",
     * which is a perfectly valid (if odd) binary-safe bulk string per
     * resp.c's contract, but would corrupt RESP framing if blindly
     * snprintf'd into a "-ERR unknown command '...'" reply with %s. */
    const char *buf = "*1\r\n$6\r\nFO\r\nBA\r\n";
    resp_command_t cmd;
    size_t consumed = 0;
    const char *err = NULL;
    resp_status_t st = resp_parse(buf, strlen(buf), &cmd, &consumed, &err);
    CHECK(st == RESP_PARSE_OK, "binary command name with embedded CRLF parses fine at the resp.c layer");

    resp_buf_t out;
    resp_buf_init(&out);
    cmd_dispatch(s, &cmd, &out);

    CHECK(out.len > 0 && out.data[0] == '-', "still produces an error reply");

    /* Count occurrences of the literal two-byte sequence "\r\n" in the
     * reply. A well-formed single RESP error line has exactly one --
     * the terminator. If the embedded CRLF leaked through unescaped,
     * there would be two or more. */
    int crlf_count = 0;
    for (size_t i = 0; i + 1 < out.len; i++) {
        if (out.data[i] == '\r' && out.data[i + 1] == '\n') crlf_count++;
    }
    CHECK(crlf_count == 1, "embedded CRLF in the command name does not leak into reply framing");

    resp_command_free(&cmd);
    resp_buf_free(&out);
    store_destroy(s);
}

int main(void) {
    test_ping_echo();
    test_set_get();
    test_set_with_expiry();
    test_del_exists();
    test_expire_ttl();
    test_incr_decr();
    test_unknown_and_arity_errors();
    test_empty_command_is_noop();
    test_error_message_sanitizes_command_name();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
