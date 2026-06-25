/* server.c -- single-threaded, epoll-driven RESP server.
 *
 * One event loop, one store_t shared by every connection -- safe with no
 * locking precisely because everything happens on this one thread,
 * matching store.h's documented "no internal locking, that's the
 * server's job" contract. Each connection gets a small growable input
 * buffer (bytes off the wire not yet fully parsed) and a resp_buf_t
 * output buffer (encoded replies not yet fully written back). Every
 * socket is non-blocking; a read or write that would block just gets
 * revisited the next time epoll says the fd is ready.
 *
 * This is deliberately the *only* server variant for now. The Makefile's
 * tsan target exists for a future server_threaded.c -- that's meant to
 * be a separate file dropped into src/, not a retrofit of this one.
 */
#define _GNU_SOURCE /* accept4() */

#include "resp.h"
#include "store.h"
#include "cmd.h"
#include "persist.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define DEFAULT_PORT     6380
#define MAX_EVENTS       64
#define READ_CHUNK       16384
#define LISTEN_BACKLOG   511

/* Hard cap on how many bytes of *unparsed* input we'll buffer for a
 * single connection. resp.c already bounds any individual argument
 * (RESP_MAX_BULK_LEN) and the argument count (RESP_MAX_ARGC), but a
 * client could still pipeline an enormous number of moderately-sized,
 * still-incomplete commands. This is the server-level backstop on top of
 * that, in the same "don't let one client make us allocate unboundedly"
 * spirit as resp.h's own limits. */
#define MAX_CONN_INPUT_BUF (16 * 1024 * 1024) /* 16 MiB */

typedef struct {
    int        fd;
    char      *in_buf;
    size_t     in_len;
    size_t     in_cap;
    resp_buf_t out;
    size_t     out_sent;
    int        closing;     /* protocol error or input-cap hit: flush
                              * whatever's queued, then close */
    int        want_write;  /* whether EPOLLOUT is currently armed, so we
                              * only touch epoll_ctl when this changes */
} conn_t;

static store_t              *g_store;
static conn_t               **g_conns;   /* indexed by fd; NULL = no connection */
static size_t                 g_conns_cap;
static int                    g_epfd;
static volatile sig_atomic_t  g_shutdown_requested = 0;

static void on_shutdown_signal(int sig) {
    (void)sig;
    g_shutdown_requested = 1;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ---------------------------------------------------------------------
 * Connection table: a plain array indexed by fd. fds are small, dense,
 * non-negative integers handed out by the kernel, so this is simpler and
 * faster than a hash table and never needs lookup -- O(1) by construction.
 * ------------------------------------------------------------------- */
static void conns_table_ensure(size_t fd) {
    if (fd < g_conns_cap) return;
    size_t newcap = g_conns_cap ? g_conns_cap * 2 : 64;
    while (newcap <= fd) newcap *= 2;
    conn_t **grown = realloc(g_conns, newcap * sizeof(conn_t *));
    if (!grown) { perror("realloc conns table"); exit(1); }
    for (size_t i = g_conns_cap; i < newcap; i++) grown[i] = NULL;
    g_conns = grown;
    g_conns_cap = newcap;
}

static conn_t *conn_create(int fd) {
    conn_t *c = calloc(1, sizeof(*c));
    if (!c) { perror("calloc conn"); exit(1); }
    c->fd = fd;
    resp_buf_init(&c->out);
    conns_table_ensure((size_t)fd);
    g_conns[fd] = c;
    return c;
}

static void conn_close(conn_t *c) {
    epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    g_conns[c->fd] = NULL;
    free(c->in_buf);
    resp_buf_free(&c->out);
    free(c);
}

static void conn_in_append(conn_t *c, const char *data, size_t len) {
    if (c->in_len + len > c->in_cap) {
        size_t newcap = c->in_cap ? c->in_cap * 2 : 4096;
        while (newcap < c->in_len + len) newcap *= 2;
        char *grown = realloc(c->in_buf, newcap);
        if (!grown) { perror("realloc conn input"); exit(1); }
        c->in_buf = grown;
        c->in_cap = newcap;
    }
    memcpy(c->in_buf + c->in_len, data, len);
    c->in_len += len;
}

/* Drop the first n bytes of the input buffer (a command resp_parse just
 * consumed), sliding the remainder down to the front. */
static void conn_in_consume(conn_t *c, size_t n) {
    memmove(c->in_buf, c->in_buf + n, c->in_len - n);
    c->in_len -= n;
    /* If the buffer drained back to empty and had grown large (say, from
     * one big pipelined burst), release it rather than holding that
     * memory for the rest of an otherwise-idle connection's lifetime. */
    if (c->in_len == 0 && c->in_cap > 65536) {
        free(c->in_buf);
        c->in_buf = NULL;
        c->in_cap = 0;
    }
}

/* Parse and dispatch every complete command currently sitting in the
 * connection's input buffer, appending each reply to c->out. Stops at the
 * first incomplete command (normal: wait for more bytes) or the first
 * protocol error (queue the error reply and mark for close -- resp.h is
 * explicit that a parse error isn't recoverable within the same stream). */
static void conn_process_input(conn_t *c) {
    while (c->in_len > 0 && !c->closing) {
        resp_command_t cmd;
        size_t consumed = 0;
        const char *err = NULL;
        resp_status_t st = resp_parse(c->in_buf, c->in_len, &cmd, &consumed, &err);

        if (st == RESP_PARSE_INCOMPLETE) return;

        if (st == RESP_PARSE_ERROR) {
            resp_reply_error(&c->out, err);
            c->closing = 1;
            return;
        }

        cmd_dispatch(g_store, &cmd, &c->out);
        resp_command_free(&cmd);
        conn_in_consume(c, consumed);
    }
}

/* Returns 1 if all queued output was flushed, 0 if some remains (caller
 * should make sure EPOLLOUT is armed), -1 on a real write error (caller
 * should close the connection). */
static int conn_flush_output(conn_t *c) {
    while (c->out_sent < c->out.len) {
        ssize_t n = write(c->fd, c->out.data + c->out_sent, c->out.len - c->out_sent);
        if (n > 0) {
            c->out_sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -1;
    }
    c->out.len = 0;
    c->out_sent = 0;
    return 1;
}

static void conn_arm_write_interest(conn_t *c, int want) {
    if (want == c->want_write) return;
    struct epoll_event ev;
    ev.data.fd = c->fd;
    ev.events = (uint32_t)(EPOLLIN | (want ? EPOLLOUT : 0));
    if (epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev) < 0) {
        perror("epoll_ctl mod");
    }
    c->want_write = want;
}

/* Try to drain c->out and decide what happens next: keep the connection
 * open and waiting for more input, arm EPOLLOUT because the socket's
 * send buffer is full, or close outright (write error, or a queued error
 * reply that has now been fully flushed). */
static void conn_service_output(conn_t *c) {
    int rc = conn_flush_output(c);
    if (rc < 0) { conn_close(c); return; }
    if (rc == 0) { conn_arm_write_interest(c, 1); return; }

    if (c->closing) { conn_close(c); return; }
    conn_arm_write_interest(c, 0);
}

static void handle_client_readable(conn_t *c) {
    char buf[READ_CHUNK];
    for (;;) {
        ssize_t n = read(c->fd, buf, sizeof(buf));
        if (n > 0) {
            if (c->in_len + (size_t)n > MAX_CONN_INPUT_BUF) {
                /* More unparsed data than we're willing to buffer for one
                 * connection. Same treatment as a protocol error: tell
                 * them, then close once that's flushed. */
                resp_reply_error(&c->out, "ERR input buffer limit exceeded");
                c->closing = 1;
                break;
            }
            conn_in_append(c, buf, (size_t)n);
            if ((size_t)n < sizeof(buf)) break; /* drained the socket for now */
            continue;
        }
        if (n == 0) {
            conn_close(c); /* peer closed; any unflushed output is moot */
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        conn_close(c); /* real read error */
        return;
    }

    if (!c->closing) conn_process_input(c);
    conn_service_output(c);
}

static void handle_listen_readable(int listen_fd) {
    for (;;) {
        int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            if (errno == EMFILE || errno == ENFILE) {
                fprintf(stderr, "accept: out of file descriptors, backing off\n");
                return;
            }
            perror("accept4");
            return;
        }

        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        conn_t *c = conn_create(fd);
        struct epoll_event ev;
        ev.data.fd = fd;
        ev.events = EPOLLIN;
        if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            perror("epoll_ctl add");
            conn_close(c);
        }
    }
}

static int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); exit(1); }
    if (listen(fd, LISTEN_BACKLOG) < 0) { perror("listen"); exit(1); }
    if (set_nonblocking(fd) < 0) { perror("set_nonblocking(listen)"); exit(1); }
    return fd;
}

static void shutdown_cleanly(int listen_fd) {
    for (size_t fd = 0; fd < g_conns_cap; fd++) {
        if (g_conns[fd]) conn_close(g_conns[fd]);
    }
    free(g_conns);
    close(listen_fd);
    close(g_epfd);
    store_destroy(g_store);
}

int main(int argc, char **argv) {
    int         port      = DEFAULT_PORT;
    const char *save_path = NULL;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--save") == 0 || strcmp(argv[i], "-s") == 0)
                && i + 1 < argc) {
            save_path = argv[++i];
        } else {
            port = atoi(argv[i]);
            if (port <= 0 || port > 65535) {
                fprintf(stderr, "usage: %s [port] [--save <file>]\n", argv[0]);
                return 1;
            }
        }
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  on_shutdown_signal);
    signal(SIGTERM, on_shutdown_signal);

    g_store = store_create();

    /* Load snapshot before accepting any connections. */
    if (save_path) {
        cmd_set_save_path(save_path);
        persist_status_t st = persist_load(g_store, save_path);
        if (st == PERSIST_OK) {
            printf("loaded snapshot from '%s' (%zu keys)\n",
                   save_path, store_size(g_store));
        } else if (st != PERSIST_ERR_IO) {
            /* IO error just means no prior snapshot exists -- that's fine.
             * Format or corrupt errors mean the file is damaged -- warn. */
            fprintf(stderr, "warning: snapshot load failed: %s\n",
                    persist_strerror(st));
        }
    }

    int listen_fd = create_listen_socket(port);

    g_epfd = epoll_create1(0);
    if (g_epfd < 0) { perror("epoll_create1"); return 1; }

    struct epoll_event ev;
    ev.data.fd = listen_fd;
    ev.events = EPOLLIN;
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        perror("epoll_ctl add listen_fd");
        return 1;
    }

    printf("listening on 0.0.0.0:%d%s\n", port,
           save_path ? " (persistence enabled)" : "");
    fflush(stdout);

    struct epoll_event events[MAX_EVENTS];
    while (!g_shutdown_requested) {
        int n = epoll_wait(g_epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                handle_listen_readable(listen_fd);
                continue;
            }

            conn_t *c = ((size_t)fd < g_conns_cap) ? g_conns[fd] : NULL;
            if (!c) continue;

            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                conn_close(c);
                continue;
            }
            if (events[i].events & EPOLLIN) {
                handle_client_readable(c);
                c = ((size_t)fd < g_conns_cap) ? g_conns[fd] : NULL;
                if (!c) continue;
            }
            if (c && (events[i].events & EPOLLOUT)) {
                conn_service_output(c);
            }
        }
    }

    printf("\nshutting down");
    /* Save snapshot before tearing down the store. */
    if (save_path) {
        persist_status_t st = persist_save(g_store, save_path);
        if (st == PERSIST_OK)
            printf(" -- snapshot saved to '%s'", save_path);
        else
            fprintf(stderr, "\nwarning: snapshot save failed: %s\n",
                    persist_strerror(st));
    }
    printf("\n");
    shutdown_cleanly(listen_fd);
    return 0;
}
