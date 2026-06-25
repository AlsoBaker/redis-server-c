/* server_threaded.c -- thread-pool RESP server.
 *
 * Architecture:
 *
 *   main thread        worker thread 0    worker thread 1    ...
 *   ───────────        ───────────────    ───────────────
 *   accept loop  ───▶  work queue  ◀────  work queue
 *                          │                  │
 *                    handle_conn()      handle_conn()
 *                    (blocking I/O)     (blocking I/O)
 *                          │                  │
 *                    lock(store_mu)     lock(store_mu)
 *                    cmd_dispatch()     cmd_dispatch()
 *                    unlock(store_mu)   unlock(store_mu)
 *
 * Every accepted socket goes non-blocking → blocking conversion: workers
 * use plain blocking read()/write(), which is the right tradeoff here.
 * Epoll is a per-thread mechanism and running an epoll loop per worker
 * thread adds complexity with no benefit when each thread is already
 * dedicated to exactly one connection at a time.
 *
 * Locking strategy: one coarse mutex around every cmd_dispatch() call.
 * This serializes store access globally, which is correct because store.h
 * documents "no internal locking -- that's the server's job". A per-key
 * or per-bucket lock would require invasive changes to store.c for gains
 * that only matter at very high core counts; this version is the correct
 * baseline and the right thing to profile before complicating.
 *
 * Unlike server.c (epoll, one thread, many connections), this file's
 * concurrency model means TSan is actually useful -- `make tsan` will
 * catch any data race if the locking ever drifts out of sync.
 */
#define _GNU_SOURCE

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
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define DEFAULT_PORT       6380
#define DEFAULT_WORKERS    4
#define QUEUE_CAPACITY     512   /* max accepted fds waiting in queue */
#define READ_CHUNK         16384
#define LISTEN_BACKLOG     511
#define MAX_INPUT_BUF      (16 * 1024 * 1024) /* per-connection, same as server.c */

/* -----------------------------------------------------------------------
 * Work queue: a fixed-capacity circular buffer of accepted fds. The main
 * (accept) thread pushes; worker threads pop. A condvar on each side
 * avoids busy-waiting whether the queue is empty or full.
 * --------------------------------------------------------------------- */
typedef struct {
    int             fds[QUEUE_CAPACITY];
    int             head, tail, count;
    int             shutdown;       /* set before broadcasting to wake workers */
    pthread_mutex_t mu;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} work_queue_t;

static void wq_init(work_queue_t *q) {
    q->head = q->tail = q->count = q->shutdown = 0;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

/* Push fd onto the queue. Blocks if the queue is full (backpressure: the
 * accept loop naturally slows down rather than losing connections). */
static void wq_push(work_queue_t *q, int fd) {
    pthread_mutex_lock(&q->mu);
    while (q->count == QUEUE_CAPACITY && !q->shutdown)
        pthread_cond_wait(&q->not_full, &q->mu);
    if (!q->shutdown) {
        q->fds[q->tail] = fd;
        q->tail = (q->tail + 1) % QUEUE_CAPACITY;
        q->count++;
        pthread_cond_signal(&q->not_empty);
    } else {
        close(fd); /* shutting down; nobody will handle this fd */
    }
    pthread_mutex_unlock(&q->mu);
}

/* Pop an fd. Returns -1 with shutdown set; returns a valid fd otherwise.
 * Blocks on the not_empty condvar until one is available. */
static int wq_pop(work_queue_t *q) {
    pthread_mutex_lock(&q->mu);
    while (q->count == 0 && !q->shutdown)
        pthread_cond_wait(&q->not_empty, &q->mu);
    int fd = -1;
    if (q->count > 0) {
        fd = q->fds[q->head];
        q->head = (q->head + 1) % QUEUE_CAPACITY;
        q->count--;
        pthread_cond_signal(&q->not_full);
    }
    pthread_mutex_unlock(&q->mu);
    return fd;
}

static void wq_shutdown(work_queue_t *q) {
    pthread_mutex_lock(&q->mu);
    q->shutdown = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mu);
}

static void wq_destroy(work_queue_t *q) {
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

/* -----------------------------------------------------------------------
 * Globals shared across threads.
 * --------------------------------------------------------------------- */
static store_t         *g_store;
static pthread_mutex_t  g_store_mu;   /* guards ALL store_* calls */
static work_queue_t     g_queue;

/* -----------------------------------------------------------------------
 * Per-connection state (stack-allocated per worker invocation, not global).
 * --------------------------------------------------------------------- */
typedef struct {
    char      *buf;
    size_t     len;
    size_t     cap;
} dyn_buf_t;

static void dyn_buf_append(dyn_buf_t *b, const char *data, size_t n) {
    if (b->len + n > b->cap) {
        size_t newcap = b->cap ? b->cap * 2 : 4096;
        while (newcap < b->len + n) newcap *= 2;
        b->buf = realloc(b->buf, newcap);
        b->cap = newcap;
    }
    memcpy(b->buf + b->len, data, n);
    b->len += n;
}

static void dyn_buf_consume(dyn_buf_t *b, size_t n) {
    memmove(b->buf, b->buf + n, b->len - n);
    b->len -= n;
    if (b->len == 0 && b->cap > 65536) {
        free(b->buf);
        b->buf = NULL;
        b->cap = 0;
    }
}

/* Write all `len` bytes to `fd`, looping on EINTR and partial writes.
 * Returns 0 on success, -1 if the connection is gone. */
static int write_all(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n > 0) { sent += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

/* Handle one accepted connection to completion (close). Called by a
 * worker thread; blocks for the lifetime of the connection. */
static void handle_conn(int fd) {
    dyn_buf_t in  = {NULL, 0, 0};
    resp_buf_t out;
    resp_buf_init(&out);

    char chunk[READ_CHUNK];
    for (;;) {
        /* Read whatever the socket has right now. */
        ssize_t nr = read(fd, chunk, sizeof(chunk));
        if (nr == 0) break;           /* peer closed */
        if (nr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (in.len + (size_t)nr > MAX_INPUT_BUF) {
            /* Input cap exceeded -- same policy as server.c */
            pthread_mutex_lock(&g_store_mu);
            resp_reply_error(&out, "ERR input buffer limit exceeded");
            pthread_mutex_unlock(&g_store_mu);
            write_all(fd, out.data, out.len);
            break;
        }
        dyn_buf_append(&in, chunk, (size_t)nr);

        /* Parse and dispatch every complete command in the buffer. */
        int fatal = 0;
        while (in.len > 0 && !fatal) {
            resp_command_t cmd;
            size_t consumed = 0;
            const char *err = NULL;
            resp_status_t st = resp_parse(in.buf, in.len, &cmd, &consumed, &err);

            if (st == RESP_PARSE_INCOMPLETE) break;

            if (st == RESP_PARSE_ERROR) {
                resp_reply_error(&out, err);
                fatal = 1;
            } else {
                /* Lock only for the store-touching part, not for I/O. */
                pthread_mutex_lock(&g_store_mu);
                cmd_dispatch(g_store, &cmd, &out);
                pthread_mutex_unlock(&g_store_mu);
                resp_command_free(&cmd);
                dyn_buf_consume(&in, consumed);
            }

            /* Flush the reply buffer after each command (or error). This
             * keeps per-command latency low for non-pipelined clients and
             * is safe even for pipelined ones -- write_all returns quickly
             * when the socket buffer has room, which it almost always will
             * since the client is busy sending the next command. */
            if (out.len > 0) {
                if (write_all(fd, out.data, out.len) < 0) { fatal = 1; }
                out.len = 0;
            }
            if (fatal) break;
        }
        if (fatal) break;
    }

    free(in.buf);
    resp_buf_free(&out);
    close(fd);
}

/* -----------------------------------------------------------------------
 * Worker thread entry point.
 * --------------------------------------------------------------------- */
static void *worker_thread(void *arg) {
    (void)arg;
    for (;;) {
        int fd = wq_pop(&g_queue);
        if (fd < 0) return NULL; /* shutdown */
        handle_conn(fd);
    }
}

/* -----------------------------------------------------------------------
 * Listening socket.
 * --------------------------------------------------------------------- */
static int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(fd, LISTEN_BACKLOG) < 0) {
        perror("listen"); exit(1);
    }
    return fd;
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
static volatile sig_atomic_t g_shutdown = 0;
static void on_signal(int s) { (void)s; g_shutdown = 1; }

int main(int argc, char **argv) {
    int         port        = DEFAULT_PORT;
    int         num_workers = DEFAULT_WORKERS;
    const char *save_path   = NULL;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--save") == 0 || strcmp(argv[i], "-s") == 0)
                && i + 1 < argc) {
            save_path = argv[++i];
        } else if (port == DEFAULT_PORT && atoi(argv[i]) > 0) {
            port = atoi(argv[i]);
        } else if (num_workers == DEFAULT_WORKERS && atoi(argv[i]) > 0) {
            num_workers = atoi(argv[i]);
        } else {
            fprintf(stderr, "usage: %s [port] [workers] [--save <file>]\n",
                    argv[0]);
            return 1;
        }
    }

    if (port <= 0 || port > 65535 || num_workers <= 0) {
        fprintf(stderr, "usage: %s [port] [workers] [--save <file>]\n", argv[0]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    pthread_mutex_init(&g_store_mu, NULL);
    g_store = store_create();

    if (save_path) {
        cmd_set_save_path(save_path);
        persist_status_t st = persist_load(g_store, save_path);
        if (st == PERSIST_OK) {
            printf("loaded snapshot from '%s' (%zu keys)\n",
                   save_path, store_size(g_store));
        } else if (st != PERSIST_ERR_IO) {
            fprintf(stderr, "warning: snapshot load failed: %s\n",
                    persist_strerror(st));
        }
    }

    wq_init(&g_queue);

    pthread_t *threads = malloc((size_t)num_workers * sizeof(pthread_t));
    for (int i = 0; i < num_workers; i++) {
        if (pthread_create(&threads[i], NULL, worker_thread, NULL) != 0) {
            perror("pthread_create"); exit(1);
        }
    }

    int listen_fd = create_listen_socket(port);
    printf("listening on 0.0.0.0:%d  (workers: %d%s)\n", port, num_workers,
           save_path ? ", persistence enabled" : "");
    fflush(stdout);

    while (!g_shutdown) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addrlen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EMFILE || errno == ENFILE) {
                fprintf(stderr, "accept: out of fds, backing off 10ms\n");
                usleep(10000);
                continue;
            }
            perror("accept");
            break;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        wq_push(&g_queue, fd);
    }

    printf("\nshutting down");
    wq_shutdown(&g_queue);
    for (int i = 0; i < num_workers; i++) pthread_join(threads[i], NULL);
    free(threads);
    close(listen_fd);

    /* Save snapshot after all workers have exited -- no locking needed. */
    if (save_path) {
        persist_status_t st = persist_save(g_store, save_path);
        if (st == PERSIST_OK)
            printf(" -- snapshot saved to '%s'", save_path);
        else
            fprintf(stderr, "\nwarning: snapshot save failed: %s\n",
                    persist_strerror(st));
    }
    printf("\n");

    wq_destroy(&g_queue);
    store_destroy(g_store);
    pthread_mutex_destroy(&g_store_mu);
    return 0;
}
