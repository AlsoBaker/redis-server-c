#include "resp.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

/* ---- helpers ---- */
static void run_section(const char *name) {
    printf("-- %s\n", name);
}

/* ---------------------------------------------------------------------
 * 1. Happy path: a well-formed multibulk command parses in one shot.
 * ------------------------------------------------------------------- */
static void test_multibulk_happy_path(void) {
    run_section("multibulk happy path");
    const char *buf = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
    size_t len = strlen(buf);

    resp_command_t cmd;
    size_t consumed = 0;
    const char *err = NULL;
    resp_status_t st = resp_parse(buf, len, &cmd, &consumed, &err);

    CHECK(st == RESP_PARSE_OK, "status should be OK");
    CHECK(consumed == len, "should consume the entire buffer");
    CHECK(cmd.argc == 3, "argc should be 3");
    if (cmd.argc == 3) {
        CHECK(cmd.argv[0].len == 3 && memcmp(cmd.argv[0].data, "SET", 3) == 0, "argv[0] == SET");
        CHECK(cmd.argv[1].len == 3 && memcmp(cmd.argv[1].data, "foo", 3) == 0, "argv[1] == foo");
        CHECK(cmd.argv[2].len == 3 && memcmp(cmd.argv[2].data, "bar", 3) == 0, "argv[2] == bar");
    }
    resp_command_free(&cmd);
}

/* ---------------------------------------------------------------------
 * 2. Pipelining: two commands back to back in one buffer. Parse the
 *    first, advance by bytes_consumed, parse the second from what's left.
 * ------------------------------------------------------------------- */
static void test_pipelined_commands(void) {
    run_section("pipelined commands");
    const char *buf = "*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n";
    size_t len = strlen(buf);

    resp_command_t cmd1, cmd2;
    size_t c1 = 0, c2 = 0;
    const char *err = NULL;

    resp_status_t st1 = resp_parse(buf, len, &cmd1, &c1, &err);
    CHECK(st1 == RESP_PARSE_OK, "first command parses OK");
    CHECK(cmd1.argc == 1 && memcmp(cmd1.argv[0].data, "PING", 4) == 0, "first command is PING");

    resp_status_t st2 = resp_parse(buf + c1, len - c1, &cmd2, &c2, &err);
    CHECK(st2 == RESP_PARSE_OK, "second command parses OK");
    CHECK(cmd2.argc == 2 && memcmp(cmd2.argv[0].data, "GET", 3) == 0, "second command is GET");
    CHECK(c1 + c2 == len, "combined consumed bytes equal total buffer length");

    resp_command_free(&cmd1);
    resp_command_free(&cmd2);
}

/* ---------------------------------------------------------------------
 * 3. Truncation: feed the parser every possible prefix of a valid command
 *    (length 0 up to len-1). Every single one must return INCOMPLETE --
 *    never ERROR, never a crash, never a false OK. Only the full buffer
 *    should succeed. This is the core guarantee that lets the network
 *    layer call resp_parse() after every partial recv() without special-
 *    casing anything.
 * ------------------------------------------------------------------- */
static void test_truncation_never_errors(void) {
    run_section("truncation never produces a false ERROR or false OK");
    const char *buf = "*2\r\n$3\r\nGET\r\n$5\r\nhello\r\n";
    size_t len = strlen(buf);

    int saw_bad_status = 0;
    for (size_t prefix = 0; prefix < len; prefix++) {
        resp_command_t cmd;
        size_t consumed = 0;
        const char *err = NULL;
        resp_status_t st = resp_parse(buf, prefix, &cmd, &consumed, &err);
        if (st != RESP_PARSE_INCOMPLETE) {
            saw_bad_status = 1;
            printf("  prefix len %zu produced status %d instead of INCOMPLETE\n", prefix, (int)st);
        }
        if (st == RESP_PARSE_OK) resp_command_free(&cmd);
    }
    CHECK(!saw_bad_status, "all proper prefixes report INCOMPLETE");

    resp_command_t cmd;
    size_t consumed = 0;
    const char *err = NULL;
    resp_status_t st = resp_parse(buf, len, &cmd, &consumed, &err);
    CHECK(st == RESP_PARSE_OK && consumed == len, "full buffer finally parses OK");
    resp_command_free(&cmd);
}

/* ---------------------------------------------------------------------
 * 4. Malformed input: each of these must return RESP_PARSE_ERROR, not a
 *    crash and not a misparse.
 * ------------------------------------------------------------------- */
static void test_malformed_inputs(void) {
    run_section("malformed input is rejected");
    struct { const char *buf; const char *desc; } cases[] = {
        { "*abc\r\n", "non-numeric multibulk count" },
        { "*-5\r\n", "negative multibulk count" },
        { "*0\r\n", "zero-length multibulk count" },
        { "*1\r\n@3\r\nfoo\r\n", "bad bulk type byte (expected '$')" },
        { "*1\r\n$abc\r\nfoo\r\n", "non-numeric bulk length" },
        { "*1\r\n$-5\r\nfoo\r\n", "negative bulk length" },
        { "*1\r\n$3\r\nfooXX", "missing trailing CRLF after bulk body" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        resp_command_t cmd;
        size_t consumed = 0;
        const char *err = NULL;
        resp_status_t st = resp_parse(cases[i].buf, strlen(cases[i].buf), &cmd, &consumed, &err);
        char msg[256];
        snprintf(msg, sizeof(msg), "%s -> expected ERROR", cases[i].desc);
        CHECK(st == RESP_PARSE_ERROR, msg);
        if (st == RESP_PARSE_ERROR) {
            CHECK(err != NULL, "error case sets err_msg");
        }
        if (st == RESP_PARSE_OK) resp_command_free(&cmd);
    }
}

/* ---------------------------------------------------------------------
 * 5. Inline commands (telnet-style): "PING\r\n", whitespace splitting,
 *    a bare \n terminator, and a blank line being a harmless no-op.
 * ------------------------------------------------------------------- */
static void test_inline_commands(void) {
    run_section("inline commands");

    {
        const char *buf = "PING\r\n";
        resp_command_t cmd;
        size_t consumed = 0;
        const char *err = NULL;
        resp_status_t st = resp_parse(buf, strlen(buf), &cmd, &consumed, &err);
        CHECK(st == RESP_PARSE_OK, "PING\\r\\n parses OK");
        CHECK(cmd.argc == 1 && memcmp(cmd.argv[0].data, "PING", 4) == 0, "argv[0] == PING");
        resp_command_free(&cmd);
    }
    {
        const char *buf = "SET  foo   bar\n"; /* bare \n, irregular spacing */
        resp_command_t cmd;
        size_t consumed = 0;
        const char *err = NULL;
        resp_status_t st = resp_parse(buf, strlen(buf), &cmd, &consumed, &err);
        CHECK(st == RESP_PARSE_OK, "bare-\\n inline command parses OK");
        CHECK(cmd.argc == 3, "extra whitespace doesn't create empty tokens");
        resp_command_free(&cmd);
    }
    {
        const char *buf = "\r\n"; /* blank line */
        resp_command_t cmd;
        size_t consumed = 0;
        const char *err = NULL;
        resp_status_t st = resp_parse(buf, strlen(buf), &cmd, &consumed, &err);
        CHECK(st == RESP_PARSE_OK, "blank line is OK, not ERROR");
        CHECK(cmd.argc == 0, "blank line yields an empty (no-op) command");
        resp_command_free(&cmd);
    }
}

/* ---------------------------------------------------------------------
 * 6. Binary safety: a bulk string's length is authoritative, even if it
 *    contains embedded NUL bytes or byte sequences that look like CRLF.
 * ------------------------------------------------------------------- */
static void test_binary_safety(void) {
    run_section("binary safety of bulk strings");
    char buf[64];
    /* $6\r\n  then 6 raw bytes including an embedded NUL and an embedded
     * CRLF-looking pair, then the real trailing CRLF. */
    const char body[6] = { 'a', '\0', 'b', '\r', '\n', 'c' };
    int n = snprintf(buf, sizeof(buf), "*1\r\n$6\r\n");
    memcpy(buf + n, body, 6);
    n += 6;
    buf[n++] = '\r';
    buf[n++] = '\n';

    resp_command_t cmd;
    size_t consumed = 0;
    const char *err = NULL;
    resp_status_t st = resp_parse(buf, (size_t)n, &cmd, &consumed, &err);
    CHECK(st == RESP_PARSE_OK, "embedded NUL/CRLF body still parses OK");
    CHECK(cmd.argc == 1 && cmd.argv[0].len == 6, "length is authoritative, not strlen-based");
    CHECK(memcmp(cmd.argv[0].data, body, 6) == 0, "raw bytes preserved exactly");
    resp_command_free(&cmd);
}

/* ---------------------------------------------------------------------
 * 7. Reply serialization: exact byte-for-byte output.
 * ------------------------------------------------------------------- */
static void test_reply_serialization(void) {
    run_section("reply serialization");
    resp_buf_t b;
    resp_buf_init(&b);

    resp_reply_simple_string(&b, "OK");
    resp_reply_error(&b, "ERR bad thing");
    resp_reply_integer(&b, -42);
    resp_reply_bulk_string(&b, "hello", 5);
    resp_reply_null_bulk(&b);
    resp_reply_array_header(&b, 2);
    resp_reply_bulk_string(&b, "a", 1);
    resp_reply_bulk_string(&b, "b", 1);
    resp_reply_null_array(&b);

    const char *expected =
        "+OK\r\n"
        "-ERR bad thing\r\n"
        ":-42\r\n"
        "$5\r\nhello\r\n"
        "$-1\r\n"
        "*2\r\n$1\r\na\r\n$1\r\nb\r\n"
        "*-1\r\n";

    CHECK(b.len == strlen(expected), "serialized length matches expected");
    CHECK(b.len == strlen(expected) && memcmp(b.data, expected, b.len) == 0,
          "serialized bytes match expected exactly");

    resp_buf_free(&b);
}

int main(void) {
    test_multibulk_happy_path();
    test_pipelined_commands();
    test_truncation_never_errors();
    test_malformed_inputs();
    test_inline_commands();
    test_binary_safety();
    test_reply_serialization();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
