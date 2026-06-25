#ifndef CMD_H
#define CMD_H

#include "resp.h"
#include "store.h"

/*
 * Command dispatch: the layer between the wire protocol (resp.c) and the
 * storage engine (store.c). Pure logic, no I/O -- it never touches a
 * socket, which is what makes it unit-testable on its own and reusable
 * across every server variant (blocking, epoll, threaded) that gets built
 * on top of it later.
 *
 * cmd_dispatch executes exactly one already-parsed command against `s` and
 * appends its RESP-encoded reply to `out`. It always appends *something*
 * for a non-empty command -- unknown command names, wrong arity, bad
 * argument values, etc. all become a RESP error reply rather than a
 * return code the caller has to check. This matches what a real client
 * expects: one reply in, one reply out, never silence.
 *
 * The one exception is cmd->argc == 0 (an empty/blank inline command, per
 * resp_parse's documented no-op case): cmd_dispatch appends nothing and
 * returns immediately, mirroring real Redis's behavior of silently
 * ignoring blank lines on the inline protocol.
 */
void cmd_dispatch(store_t *s, const resp_command_t *cmd, resp_buf_t *out);

/* Call once from main() after argument parsing to enable persistence.
 * Pass NULL or "" to disable (SAVE/BGSAVE will return an error). */
void cmd_set_save_path(const char *path);

#endif /* CMD_H */
