#ifndef STORE_H
#define STORE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Key-value store: separate-chaining hash table with power-of-two sizing
 * and incremental rehashing.
 *
 * Design notes:
 *
 * - Separate chaining (linked list per bucket) instead of open addressing.
 *   This is what makes incremental rehashing tractable: a bucket's chain
 *   can be migrated to the new table independently of every other bucket,
 *   with no tombstones and no probe-sequence corruption to worry about.
 *
 * - Power-of-two table sizes. Bucket index is `hash & (size - 1)` instead
 *   of `hash % size` -- a mask-and-AND instead of a division on every
 *   single lookup.
 *
 * - Incremental rehashing. When the table needs to grow, we do NOT stop
 *   and rehash every key in one pass -- on a table with millions of keys
 *   that's a multi-millisecond stall on whichever request happens to
 *   trigger it. Instead we keep two tables (ht[0] = old, ht[1] = new) and
 *   migrate a bounded number of buckets on every store_* call until ht[0]
 *   is empty, then ht[1] becomes the new ht[0]. While a rehash is in
 *   progress, reads/writes/deletes have to consider both tables, since a
 *   key may or may not have been migrated yet.
 *
 * - No internal locking. This module is not thread-safe by itself --
 *   concurrency strategy (one coarse mutex around the whole store vs.
 *   finer-grained locking) is decided where this gets wired into a server,
 *   not in here.
 *
 * - Keys and values are binary-safe byte strings (explicit length, no NUL
 *   assumptions), matching how resp.c hands us arguments.
 */

typedef struct store store_t;

store_t *store_create(void);
void     store_destroy(store_t *s);

/* Total number of live keys across both tables (meaningful even mid-rehash). */
size_t store_size(const store_t *s);

/* For tests/diagnostics: is a rehash currently in progress? */
int store_is_rehashing(const store_t *s);

/*
 * Insert or overwrite a key. Both key and val are copied; the caller keeps
 * ownership of the buffers passed in. expire_at_ms is an absolute
 * Unix-epoch millisecond timestamp, or 0 for "no expiry". As in real Redis,
 * a plain SET (expire_at_ms == 0) clears any TTL the key previously had --
 * pass the key's existing TTL back in if you want to preserve it.
 *
 * Returns 1 if a new key was inserted, 0 if an existing key was updated.
 */
int store_set(store_t *s, const char *key, size_t keylen,
              const char *val, size_t vallen, int64_t expire_at_ms);

/*
 * Look up a key. On success, *val_out and *vallen_out point at the store's
 * internal copy of the value -- valid until the next store_set/store_del
 * call on that same key (or store_destroy), do not free it. A key whose
 * TTL has passed is treated as absent (and is lazily deleted right here).
 *
 * Returns 1 if found, 0 if not found (or expired).
 */
int store_get(store_t *s, const char *key, size_t keylen,
              const char **val_out, size_t *vallen_out);

/* Returns 1 if the key exists (and isn't expired), 0 otherwise. */
int store_exists(store_t *s, const char *key, size_t keylen);

/* Returns 1 if the key existed and was removed, 0 if it didn't exist. */
int store_del(store_t *s, const char *key, size_t keylen);

/*
 * Set an absolute expiry (Unix-epoch ms) on an existing key.
 * Returns 1 if the key exists, 0 if it doesn't (no-op in that case).
 */
int store_expire(store_t *s, const char *key, size_t keylen, int64_t expire_at_ms);

#define STORE_TTL_NO_KEY     (-2) /* key does not exist */
#define STORE_TTL_NO_EXPIRY  (-1) /* key exists but has no TTL set */

/* Remaining TTL in milliseconds, or one of the sentinels above.
 * Mirrors real Redis's TTL command's -2/-1 convention so the eventual
 * command dispatcher can map this straight onto a RESP integer reply. */
int64_t store_ttl_ms(store_t *s, const char *key, size_t keylen);

typedef enum {
    STORE_INCR_OK = 0,
    STORE_INCR_NOT_INTEGER,  /* existing value isn't a valid base-10 int64 */
    STORE_INCR_OVERFLOW      /* result would overflow/underflow int64 */
} store_incr_status_t;

/*
 * Atomic-within-this-call increment/decrement (pass a negative delta for
 * DECR). A missing key is treated as 0 before applying delta, matching
 * real Redis. On success *new_value_out holds the result and any existing
 * TTL on the key is left untouched.
 */
store_incr_status_t store_incrby(store_t *s, const char *key, size_t keylen,
                                  int64_t delta, int64_t *new_value_out);

/*
 * Remove every key from the store, resetting it to an empty state.
 * Returns the number of keys that were removed.
 */
size_t store_flush(store_t *s);

/*
 * Iterate over every live (non-expired) entry in the store.
 * The callback receives read-only pointers into the store's internal
 * buffers -- valid only for the duration of that single callback
 * invocation. Do NOT call any store_* mutating function from inside fn;
 * the store's internal state is not safe to modify during iteration.
 */
typedef void (*store_visit_fn)(const char *key,  size_t keylen,
                               const char *val,  size_t vallen,
                               int64_t expire_at_ms, void *ud);
void store_foreach(store_t *s, store_visit_fn fn, void *ud);

#endif /* STORE_H */
