#ifndef RESP_H
#define RESP_H

#include <stddef.h>

/*
 * Hard limits bound how much memory a single (possibly malicious or buggy)
 * client can make us allocate before we've even validated the command.
 * Stock Redis has equivalents (proto-max-bulk-len, etc). Values chosen to be
 * generous for real use but not unbounded.
 */
#define RESP_MAX_ARGC        (1024 * 1024)        /* max elements in a multibulk array */
#define RESP_MAX_BULK_LEN    (512 * 1024 * 1024)  /* max length of one bulk string */
#define RESP_MAX_INLINE_LEN  65536                /* max length of an inline command line */

typedef enum {
    RESP_PARSE_OK = 0,          /* a complete command was parsed */
    RESP_PARSE_INCOMPLETE = 1,  /* not enough bytes yet; caller should read more and retry */
    RESP_PARSE_ERROR = 2        /* protocol violation; caller should reply with an error
                                  * (if it can) and close the connection */
} resp_status_t;

/* One command argument. Binary-safe: len is authoritative, data is NOT
 * guaranteed to be free of embedded NULs. We do append one extra NUL byte
 * after data for convenience (e.g. so you can strcmp a command name without
 * a length check), but never rely on it for argument *contents*. */
typedef struct {
    char   *data;
    size_t  len;
} resp_arg_t;

typedef struct {
    resp_arg_t *argv;
    size_t      argc;
} resp_command_t;

/*
 * Try to parse exactly one command from the front of buf[0..buf_len).
 *
 * RESP_PARSE_OK:
 *   *cmd is populated. All argv[i].data are freshly malloc'd copies owned by
 *   the caller -- call resp_command_free(cmd) when done. *bytes_consumed is
 *   how many bytes from the front of buf this command used; the caller
 *   should drop that many bytes from its connection input buffer.
 *
 * RESP_PARSE_INCOMPLETE:
 *   buf doesn't yet hold a full command. *bytes_consumed is always 0.
 *   Caller should read more bytes (appending to buf) and call again.
 *
 * RESP_PARSE_ERROR:
 *   Malformed input. *err_msg points to a static (non-owned) string
 *   describing the problem, suitable for sending back as a RESP error.
 *
 * resp_parse never retains a pointer into buf and never blocks; buf can be
 * freed or reused the instant this call returns. This is what makes it
 * usable from both a blocking-recv loop and a non-blocking epoll loop with
 * no changes.
 */
resp_status_t resp_parse(const char *buf, size_t buf_len,
                          resp_command_t *cmd, size_t *bytes_consumed,
                          const char **err_msg);

void resp_command_free(resp_command_t *cmd);

/* ---- Reply serialization ----
 * Append-only growable buffer. A command handler builds a reply with a
 * handful of resp_reply_* calls (e.g. array_header then N bulk_strings) and
 * the caller writes the whole buffer to the socket / output queue in one
 * shot, rather than issuing a write() per RESP token. */
typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
} resp_buf_t;

void resp_buf_init(resp_buf_t *b);
void resp_buf_free(resp_buf_t *b);

void resp_reply_simple_string(resp_buf_t *b, const char *s);            /* +OK\r\n */
void resp_reply_error(resp_buf_t *b, const char *s);                    /* -ERR ...\r\n */
void resp_reply_integer(resp_buf_t *b, long long v);                    /* :123\r\n */
void resp_reply_bulk_string(resp_buf_t *b, const char *s, size_t len);  /* $N\r\n...\r\n */
void resp_reply_null_bulk(resp_buf_t *b);                               /* $-1\r\n */
void resp_reply_array_header(resp_buf_t *b, size_t count);              /* *N\r\n */
void resp_reply_null_array(resp_buf_t *b);                              /* *-1\r\n */

#endif /* RESP_H */
