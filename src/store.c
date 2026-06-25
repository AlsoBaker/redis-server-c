/* clock_gettime/CLOCK_REALTIME are POSIX, not ISO C -- under strict
 * -std=c11 glibc hides them unless we explicitly ask for POSIX visibility.
 * Must come before any system header is pulled in (including transitively
 * via store.h), hence first line of the file. We'll need this same macro
 * in every file that touches sockets/threads later for the same reason. */
#define _POSIX_C_SOURCE 200809L

#include "store.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

#define STORE_INITIAL_SIZE 4          /* small on purpose: makes growth/rehashing
                                        * exercise easily in tests, matches real
                                        * Redis's DICT_HT_INITIAL_SIZE */
#define REHASH_EMPTY_VISITS_LIMIT 10  /* bound on consecutive empty buckets we'll
                                        * skip over in a single rehash step, so a
                                        * very sparse table can't turn one "step"
                                        * into an unbounded scan */

typedef struct store_entry {
    char   *key;
    size_t  key_len;
    char   *val;
    size_t  val_len;
    int64_t expire_at_ms;      /* 0 = no expiry */
    struct store_entry *next;  /* separate chaining */
} store_entry_t;

typedef struct {
    store_entry_t **buckets;
    size_t size;   /* power of two; 0 means unallocated */
    size_t mask;   /* size - 1 */
    size_t used;
} store_table_t;

struct store {
    store_table_t ht[2];  /* ht[0]: primary table. ht[1]: target table while
                            * rehashing, unallocated (size 0) otherwise. */
    long rehash_idx;      /* -1 when not rehashing; else next ht[0] bucket
                            * index to migrate. */
};

/* ---------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------- */
static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t fnv1a_hash(const char *data, size_t len) {
    uint64_t hash = 0xcbf29ce484222325ULL; /* FNV-1a 64-bit offset basis */
    for (size_t i = 0; i < len; i++) {
        hash ^= (unsigned char)data[i];
        hash *= 0x100000001b3ULL;          /* FNV prime */
    }
    return hash;
}

static size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

static void table_init(store_table_t *t, size_t size) {
    t->buckets = size ? calloc(size, sizeof(store_entry_t *)) : NULL;
    t->size = size;
    t->mask = size ? size - 1 : 0;
    t->used = 0;
}

static int is_rehashing(const store_t *s) {
    return s->rehash_idx != -1;
}

/* Find the slot (pointer-to-pointer, so callers can unlink/relink in place)
 * holding `key` within a single table. Returns NULL if not present in t. */
static store_entry_t **find_in_table(store_table_t *t, const char *key,
                                      size_t keylen, uint64_t h) {
    if (t->size == 0) return NULL;
    store_entry_t **slot = &t->buckets[h & t->mask];
    while (*slot) {
        if ((*slot)->key_len == keylen && memcmp((*slot)->key, key, keylen) == 0) {
            return slot;
        }
        slot = &(*slot)->next;
    }
    return NULL;
}

/* Search ht[0], then ht[1] if a rehash is in progress. Sets *table_out to
 * whichever table the key was actually found in (needed so callers can
 * correctly adjust that table's `used` count). */
static store_entry_t **find_slot_either(store_t *s, const char *key, size_t keylen,
                                         uint64_t h, store_table_t **table_out) {
    store_table_t *t = &s->ht[0];
    store_entry_t **slot = find_in_table(t, key, keylen, h);
    if (!slot && is_rehashing(s)) {
        t = &s->ht[1];
        slot = find_in_table(t, key, keylen, h);
    }
    if (slot) *table_out = t;
    return slot;
}

static void free_entry(store_entry_t *e) {
    free(e->key);
    free(e->val);
    free(e);
}

/* Look up a key and lazily delete it if its TTL has passed. Returns NULL
 * for both "doesn't exist" and "just expired". */
static store_entry_t *locate_live(store_t *s, const char *key, size_t keylen) {
    uint64_t h = fnv1a_hash(key, keylen);
    store_table_t *t = NULL;
    store_entry_t **slot = find_slot_either(s, key, keylen, h, &t);
    if (!slot) return NULL;

    store_entry_t *e = *slot;
    if (e->expire_at_ms != 0 && e->expire_at_ms <= now_ms()) {
        *slot = e->next;
        t->used--;
        free_entry(e);
        return NULL;
    }
    return e;
}

/* ---------------------------------------------------------------------
 * Incremental rehashing
 * ------------------------------------------------------------------- */

/* Do a single bounded unit of rehashing work: migrate the next non-empty
 * ht[0] bucket's whole chain into ht[1], skipping up to
 * REHASH_EMPTY_VISITS_LIMIT empty buckets along the way. Called at the top
 * of every public API call, so a rehash always completes within roughly
 * (ht[0].size / 1) calls in the worst case, with no single call doing
 * more than one bucket's worth of migration work. */
static void rehash_step(store_t *s) {
    if (!is_rehashing(s)) return;

    store_table_t *src = &s->ht[0];
    store_table_t *dst = &s->ht[1];

    size_t empty_visits = 0;
    while (empty_visits < REHASH_EMPTY_VISITS_LIMIT && (size_t)s->rehash_idx < src->size) {
        store_entry_t *e = src->buckets[s->rehash_idx];
        if (!e) {
            empty_visits++;
            s->rehash_idx++;
            continue;
        }
        while (e) {
            store_entry_t *next = e->next;
            uint64_t h = fnv1a_hash(e->key, e->key_len);
            size_t didx = h & dst->mask;
            e->next = dst->buckets[didx];
            dst->buckets[didx] = e;
            dst->used++;
            src->used--;
            e = next;
        }
        src->buckets[s->rehash_idx] = NULL;
        s->rehash_idx++;
        break; /* one non-empty bucket migrated == one unit of work */
    }

    if ((size_t)s->rehash_idx >= src->size) {
        free(src->buckets);
        s->ht[0] = s->ht[1];
        table_init(&s->ht[1], 0);
        s->rehash_idx = -1;
    }
}

/* Begin a rehash (grow ht[0] into a fresh, larger ht[1]) if the load factor
 * warrants it and we're not already mid-rehash. Checked after an insert, so
 * `used` reflects the entry that just triggered it. */
static void maybe_start_rehash(store_t *s) {
    if (is_rehashing(s)) return;
    store_table_t *t = &s->ht[0];
    if (t->used < t->size) return; /* load factor < 1.0, nothing to do */

    size_t new_size = next_pow2(t->size * 2);
    table_init(&s->ht[1], new_size);
    s->rehash_idx = 0;
}

/* ---------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------- */
store_t *store_create(void) {
    store_t *s = malloc(sizeof(*s));
    table_init(&s->ht[0], STORE_INITIAL_SIZE);
    table_init(&s->ht[1], 0);
    s->rehash_idx = -1;
    return s;
}

static void table_free_entries(store_table_t *t) {
    if (!t->buckets) return;
    for (size_t i = 0; i < t->size; i++) {
        store_entry_t *e = t->buckets[i];
        while (e) {
            store_entry_t *next = e->next;
            free_entry(e);
            e = next;
        }
    }
    free(t->buckets);
}

void store_destroy(store_t *s) {
    if (!s) return;
    table_free_entries(&s->ht[0]);
    table_free_entries(&s->ht[1]);
    free(s);
}

size_t store_size(const store_t *s) {
    return s->ht[0].used + s->ht[1].used;
}

int store_is_rehashing(const store_t *s) {
    return is_rehashing(s);
}

int store_set(store_t *s, const char *key, size_t keylen,
              const char *val, size_t vallen, int64_t expire_at_ms) {
    rehash_step(s);
    uint64_t h = fnv1a_hash(key, keylen);

    store_table_t *t = NULL;
    store_entry_t **slot = find_slot_either(s, key, keylen, h, &t);

    if (slot) {
        store_entry_t *e = *slot;
        char *newval = malloc(vallen + 1);
        memcpy(newval, val, vallen);
        newval[vallen] = '\0';
        free(e->val);
        e->val = newval;
        e->val_len = vallen;
        e->expire_at_ms = expire_at_ms;
        return 0;
    }

    store_entry_t *e = malloc(sizeof(*e));
    e->key = malloc(keylen + 1);
    memcpy(e->key, key, keylen);
    e->key[keylen] = '\0';
    e->key_len = keylen;

    e->val = malloc(vallen + 1);
    memcpy(e->val, val, vallen);
    e->val[vallen] = '\0';
    e->val_len = vallen;

    e->expire_at_ms = expire_at_ms;

    store_table_t *target = is_rehashing(s) ? &s->ht[1] : &s->ht[0];
    size_t idx = h & target->mask;
    e->next = target->buckets[idx];
    target->buckets[idx] = e;
    target->used++;

    if (!is_rehashing(s)) maybe_start_rehash(s);

    return 1;
}

int store_get(store_t *s, const char *key, size_t keylen,
              const char **val_out, size_t *vallen_out) {
    rehash_step(s);
    store_entry_t *e = locate_live(s, key, keylen);
    if (!e) return 0;
    *val_out = e->val;
    *vallen_out = e->val_len;
    return 1;
}

int store_exists(store_t *s, const char *key, size_t keylen) {
    rehash_step(s);
    return locate_live(s, key, keylen) != NULL;
}

int store_del(store_t *s, const char *key, size_t keylen) {
    rehash_step(s);
    uint64_t h = fnv1a_hash(key, keylen);
    store_table_t *t = NULL;
    store_entry_t **slot = find_slot_either(s, key, keylen, h, &t);
    if (!slot) return 0;

    store_entry_t *e = *slot;
    *slot = e->next;
    t->used--;
    free_entry(e);
    return 1;
}

int store_expire(store_t *s, const char *key, size_t keylen, int64_t expire_at_ms) {
    rehash_step(s);
    store_entry_t *e = locate_live(s, key, keylen);
    if (!e) return 0;
    e->expire_at_ms = expire_at_ms;
    return 1;
}

int64_t store_ttl_ms(store_t *s, const char *key, size_t keylen) {
    rehash_step(s);
    store_entry_t *e = locate_live(s, key, keylen);
    if (!e) return STORE_TTL_NO_KEY;
    if (e->expire_at_ms == 0) return STORE_TTL_NO_EXPIRY;
    int64_t remaining = e->expire_at_ms - now_ms();
    return remaining > 0 ? remaining : 0;
}

/* Strict base-10 int64 parse: the whole span must be a valid integer, no
 * surrounding whitespace, no trailing garbage. Used by INCR/DECR to decide
 * whether an existing value is usable as a number. */
static int parse_strict_ll(const char *s, size_t len, int64_t *out) {
    if (len == 0 || len > 20) return 0; /* INT64_MIN is 20 chars incl. sign */
    char buf[21];
    memcpy(buf, s, len);
    buf[len] = '\0';
    if (isspace((unsigned char)buf[0])) return 0;

    errno = 0;
    char *endptr = NULL;
    long long v = strtoll(buf, &endptr, 10);
    if (errno == ERANGE) return 0;
    if (endptr != buf + len) return 0; /* trailing junk or nothing parsed */
    *out = (int64_t)v;
    return 1;
}

store_incr_status_t store_incrby(store_t *s, const char *key, size_t keylen,
                                  int64_t delta, int64_t *new_value_out) {
    rehash_step(s);
    store_entry_t *e = locate_live(s, key, keylen);

    int64_t current = 0;
    if (e) {
        if (!parse_strict_ll(e->val, e->val_len, &current)) {
            return STORE_INCR_NOT_INTEGER;
        }
    }

    if ((delta > 0 && current > INT64_MAX - delta) ||
        (delta < 0 && current < INT64_MIN - delta)) {
        return STORE_INCR_OVERFLOW;
    }
    int64_t result = current + delta;

    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)result);

    if (e) {
        char *newval = malloc((size_t)n + 1);
        memcpy(newval, buf, (size_t)n + 1);
        free(e->val);
        e->val = newval;
        e->val_len = (size_t)n;
        /* TTL intentionally untouched, matching real Redis INCR/DECR. */
    } else {
        store_set(s, key, keylen, buf, (size_t)n, 0);
    }

    *new_value_out = result;
    return STORE_INCR_OK;
}

/* -----------------------------------------------------------------------
 * Bulk operations
 * --------------------------------------------------------------------- */
size_t store_flush(store_t *s) {
    size_t removed = store_size(s);
    table_free_entries(&s->ht[0]);
    table_free_entries(&s->ht[1]);
    table_init(&s->ht[0], STORE_INITIAL_SIZE);
    table_init(&s->ht[1], 0);
    s->rehash_idx = -1;
    return removed;
}

void store_foreach(store_t *s, store_visit_fn fn, void *ud) {
    int64_t now = now_ms();
    /* Walk both tables. During a rehash keys sit in EITHER ht[0] or ht[1]
     * but never in both, so iterating both gives exactly the full key-set
     * with no duplicates. */
    for (int t = 0; t < 2; t++) {
        store_table_t *tbl = &s->ht[t];
        for (size_t i = 0; i < tbl->size; i++) {
            store_entry_t *e = tbl->buckets[i];
            while (e) {
                /* Skip expired entries -- they're dead even if not yet
                 * lazily removed. Saving them would be wrong: they'd
                 * appear live again when reloaded after a restart. */
                if (e->expire_at_ms == 0 || e->expire_at_ms > now) {
                    fn(e->key, e->key_len, e->val, e->val_len,
                       e->expire_at_ms, ud);
                }
                e = e->next;
            }
        }
    }
}
