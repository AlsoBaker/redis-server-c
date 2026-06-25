# Redis Clone — From Scratch in C

A Redis-compatible in-memory key-value server built entirely from scratch in C, with no external library dependencies beyond the C standard library and POSIX.

## What's implemented

| Layer | File | Description |
|---|---|---|
| Wire protocol | `src/resp.c` | RESP parser + reply serializer |
| Storage engine | `src/store.c` | Hash table with incremental rehashing, TTL, INCR/DECR |
| Command dispatcher | `src/cmd.c` | 21 commands |
| Persistence | `src/persist.c` | Binary snapshots, atomic save, BGSAVE via fork() |
| epoll server | `src/server.c` | Single-threaded, non-blocking, many concurrent connections |
| Thread-pool server | `src/server_threaded.c` | N worker threads, mutex-guarded shared store |

## Supported commands

`PING` `ECHO` `SET` (with `EX`/`PX`) `GET` `MGET` `MSET` `DEL` `EXISTS` `KEYS` `EXPIRE` `TTL` `PTTL` `INCR` `DECR` `INCRBY` `DECRBY` `DBSIZE` `FLUSHDB` `SAVE` `BGSAVE` `LASTSAVE`

## Build

```bash
make test          # build + run all 1,065 checks
make server        # build both server binaries
make asan          # rebuild with AddressSanitizer + UBSan
make tsan          # rebuild with ThreadSanitizer
```

Requires: GCC, GNU Make, Linux (epoll).

## Run

```bash
# Single-threaded epoll server
./build/server 6380 --save data.kvs

# Thread-pool server (8 workers)
./build/server_threaded 6380 8 --save data.kvs

# Connect with redis-cli
redis-cli -p 6380 PING
redis-cli -p 6380 SET foo bar EX 60
redis-cli -p 6380 BGSAVE
```

## Design highlights

- **Incremental rehashing** — hash table grows without any stop-the-world pause; migration is bounded per operation
- **Binary-safe throughout** — keys and values can contain null bytes, CRLF, or any byte value
- **RESP injection guard** — binary-unsafe command names are sanitized before appearing in error replies
- **Atomic snapshots** — saves write to a `.tmp` file then `rename(2)`, so a crash never corrupts the last good snapshot
- **BGSAVE via fork()** — child inherits a copy-on-write snapshot; parent keeps serving clients immediately
- **Zero data races** — verified with ThreadSanitizer across all concurrent paths in `server_threaded`
- **1,065 automated checks** — zero warnings, clean under ASan/UBSan and TSan
