# Redis Clone — From Scratch in C

A Redis-compatible in-memory key-value server built entirely from scratch in C, with no external library dependencies beyond the C standard library and POSIX.

![C](https://img.shields.io/badge/C-99%2F11-blue?style=flat-square)
![Linux](https://img.shields.io/badge/Platform-Linux-orange?style=flat-square)
![Tests](https://img.shields.io/badge/Tests-1%2C065%20passing-brightgreen?style=flat-square)
![License](https://img.shields.io/badge/License-MIT-yellow?style=flat-square)

---

## Architecture

The project is deliberately layered — no layer knows about the one above it. Each module can be understood, tested, and reasoned about independently.

```
Client (redis-cli / any RESP client)
         │  TCP bytes
         ▼
┌─────────────────────────────────┐
│  server.c / server_threaded.c   │  Owns the socket. Reads bytes into a
│                                 │  per-connection buffer. Drives the
│                                 │  parse loop. Two variants — see below.
└─────────────────────────────────┘
         │  raw bytes + length
         ▼
┌─────────────────────────────────┐
│  resp.c                         │  Turns bytes into resp_command_t structs.
│  (resp_parse / resp_reply_*)    │  Turns results back into RESP bytes.
└─────────────────────────────────┘
         │  resp_command_t
         ▼
┌─────────────────────────────────┐
│  cmd.c                          │  Maps command names to handlers. Each
│  (cmd_dispatch)                 │  handler calls into store.c and writes
│                                 │  a reply into a resp_buf_t.
└─────────────────────────────────┘
         │  store_* calls
         ▼
┌─────────────────────────────────┐
│  store.c                        │  Hash table, TTL, INCR/DECR,
│  (store_set / get / del / ...)  │  incremental rehashing.
└─────────────────────────────────┘
         │  store_foreach
         ▼
┌─────────────────────────────────┐
│  persist.c                      │  Binary snapshot. Atomic via rename().
│  (persist_save / persist_load)  │  BGSAVE via fork() + copy-on-write.
└─────────────────────────────────┘
```

---

## File Structure

```
redis-server-c/
├── Makefile
├── include/
│   ├── resp.h          Wire protocol interface
│   ├── store.h         Storage engine interface
│   ├── cmd.h           Command dispatcher interface
│   └── persist.h       Persistence interface
├── src/
│   ├── resp.c          RESP parser + reply serializer
│   ├── store.c         Hash table KV store
│   ├── cmd.c           Command dispatcher (21 commands)
│   ├── persist.c       Binary snapshot save/load
│   ├── server.c        Single-threaded epoll server
│   └── server_threaded.c   Thread-pool server
└── tests/
    ├── test_resp.c     38 checks
    ├── test_store.c    958 checks
    ├── test_cmd.c      46 checks
    └── test_persist.c  23 checks
```

---

## Build

```bash
make test          # build + run all 1,065 checks
make server        # build both server binaries
make asan          # rebuild with AddressSanitizer + UBSan, run tests
make tsan          # rebuild with ThreadSanitizer (targets server_threaded)
make clean         # remove build/ directory
```

Requires: GCC, GNU Make, Linux (epoll).

## Run

```bash
# Single-threaded epoll server
./build/server 6380 --save data.kvs

# Thread-pool server (8 workers)
./build/server_threaded 6380 8 --save data.kvs

# Connect with redis-cli (fully RESP-compatible)
redis-cli -p 6380 PING
redis-cli -p 6380 SET foo bar EX 60
redis-cli -p 6380 BGSAVE

# Or telnet-style via netcat (inline protocol)
nc localhost 6380
PING
SET foo bar
KEYS *
```

---

## Supported Commands

`PING` `ECHO` `SET` (with `EX`/`PX`) `GET` `MGET` `MSET` `DEL` `EXISTS` `KEYS`
`EXPIRE` `TTL` `PTTL` `INCR` `DECR` `INCRBY` `DECRBY` `DBSIZE` `FLUSHDB`
`SAVE` `BGSAVE` `LASTSAVE`

---

## Design: The Interesting Parts

### RESP Parser — Truncation Guarantee

The parser (`resp_parse`) returns one of three statuses: `OK`, `INCOMPLETE`, or `ERROR`. The critical correctness property: **every prefix of a valid command, no matter how short, must return `INCOMPLETE`** — never a false `OK`, never a false `ERROR`.

This is what lets the server call `resp_parse` after every single `recv()` without any special-casing for partial reads. The test suite verifies this property exhaustively: it feeds every possible prefix length of several known-valid commands and asserts `INCOMPLETE` on each one.

The parser also never retains pointers into the input buffer — every parsed argument is a freshly `malloc`'d copy. This means the server can free or reuse the read buffer the instant `resp_parse` returns, with no lifetime coupling between the parser and the I/O layer.

### Hash Table — Incremental Rehashing

When a hash table's load factor reaches 1.0, a naive implementation would stop the world, allocate a new table twice the size, and move every entry. On a table with millions of keys, that's a multi-millisecond stall on whichever client request happened to trigger it.

Instead, the store keeps **two tables simultaneously**: `ht[0]` (old) and `ht[1]` (new, larger). A `rehash_idx` cursor tracks which bucket in `ht[0]` needs migrating next.

`rehash_step()` is called at the top of every public `store_*` function. It migrates exactly one non-empty bucket per call, distributing the migration cost across many ordinary operations rather than concentrating it in one pause. While a rehash is in progress:
- All reads check both tables (a key might be in either)
- All inserts go to `ht[1]` only
- When `ht[0]` is fully drained, `ht[1]` becomes the new `ht[0]`

The test suite verifies mid-rehash correctness explicitly — during a bulk insert that triggers a rehash, it spot-checks both an early key (likely migrated) and the most recent key (likely in `ht[1]`) on every iteration.

This is the same design real Redis uses internally (`dict.c`).

### Separate Chaining — Why Not Open Addressing

Collisions are resolved by a linked list at each bucket. The reason this was chosen over open addressing: **incremental rehashing is tractable**. A bucket's entire chain can be relinked to the new table by pointer manipulation, with no tombstones and no probe-sequence corruption. Open addressing makes incremental rehashing effectively impossible.

Bucket indices use `hash & (size - 1)` (bitwise AND) rather than `hash % size` (division) — power-of-two table sizes make this a meaningful speedup on hot lookup paths.

### RESP Injection Guard

When an unknown command is received, the server embeds the command name in an error reply. If that name contains raw `\r\n` bytes (legal under RESP's binary-safety rules), naively embedding it would inject extra CRLF sequences into the reply stream, corrupting whatever the client reads next.

`safe_token()` sanitizes any argument before it touches an error message: truncates to a safe length and replaces any control byte (CR, LF, or anything below `0x20`) with `?`. The test suite includes a specific test that constructs a command name containing embedded CRLF and counts `\r\n` occurrences in the reply — confirming only the real terminator survives.

### Atomic Snapshots + BGSAVE

`persist_save()` writes the entire snapshot to `<path>.tmp`, then calls `rename(2)` to replace the target file. `rename()` is guaranteed atomic by POSIX — a crash mid-save leaves the previous good snapshot intact, never a partial file.

`BGSAVE` uses `fork()`. The child inherits a copy-on-write snapshot of the entire store's memory. Because the child only reads (iterating via `store_foreach`), the OS never actually copies any pages — it shares them read-only until the parent writes something. This gives a consistent point-in-time snapshot at essentially zero cost. The parent replies immediately and keeps serving clients.

### TTL — Lazy Expiry

Each entry stores an `expire_at_ms` field: an absolute Unix-epoch millisecond timestamp, or `0` for no expiry. When any lookup (`store_get`, `store_exists`, etc.) finds an entry whose expiry has passed, it deletes the entry in place and returns "not found". No background thread needed.

Expired keys are also filtered on both save (never written to a snapshot) and load (skipped if already expired by the time the server reads them back).

---

## Two Server Architectures

The same four library modules (`resp.c`, `store.c`, `cmd.c`, `persist.c`) back two fundamentally different server implementations, demonstrating two approaches to concurrency:

|  | `server.c` (epoll) | `server_threaded.c` (thread pool) |
|---|---|---|
| **Concurrency model** | I/O multiplexing (one thread) | True parallelism (N threads) |
| **Store locking** | None needed (single thread) | Coarse `pthread_mutex_t` around every `cmd_dispatch` |
| **I/O style** | Non-blocking + `EPOLLOUT` backpressure | Blocking `read`/`write` per worker |
| **Connections** | Thousands (limited by fds, not threads) | Bounded by thread pool size |
| **Sanitizer target** | ASan + UBSan | TSan |
| **Best for** | Many idle connections, low latency | CPU-bound workloads, simpler code |

**Why real Redis is single-threaded**: an event loop avoids the memory overhead (~8MB stack) and context-switch cost of one-thread-per-connection, while a well-written single-threaded server saturates the network interface before CPU becomes the bottleneck. Most Redis latency is I/O wait, not computation — the epoll model is the right fit.

**Why the coarse mutex in `server_threaded`**: per-key or per-bucket locking would require invasive changes to `store.c` for gains that only matter at very high core counts. The coarse mutex is the correct baseline — simple, correct, and TSan-verifiable. Verified: zero data races detected.

---

## Test Coverage

All tests use a hand-rolled harness with no dependencies: a `CHECK(condition, message)` macro that prints failures with file and line number and exits non-zero if any check fails — CI-friendly out of the box.

| File | Checks | What it covers |
|---|---|---|
| `test_resp.c` | 38 | Multibulk happy path, pipelining, **truncation-guarantee** (every prefix of valid commands asserts `INCOMPLETE`), 7 malformed-input cases |
| `test_store.c` | 958 | Basic CRUD, TTL sentinels (`-1`/`-2`), lazy expiry actually removes entries, `INCR`/`DECR` on missing/non-integer/overflow keys, **mid-rehash correctness** (spot-checks both tables on every insert during an active rehash) |
| `test_cmd.c` | 46 | All 21 commands, wrong arity, unknown commands, **RESP injection guard** (CRLF-in-command-name can't corrupt the reply stream) |
| `test_persist.c` | 23 | Save/load roundtrip for string/numeric/binary-safe data, TTL preservation, empty store, **atomic save** (`.tmp` rename), corrupt-checksum detection |
| **Total** | **1,065** | Zero failures. Zero compiler warnings. Clean under ASan/UBSan and TSan. |


---

## About

Redis-compatible in-memory key-value server built from scratch in C — RESP protocol, hash table with incremental rehashing, TTL, binary persistence with atomic saves, and two server architectures: single-threaded epoll and a thread-pool with mutex-guarded shared state.

1,065 automated checks. Zero warnings. Clean under AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer.
