#define _POSIX_C_SOURCE 200809L

#include "store.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

static void run_section(const char *name) { printf("-- %s\n", name); }

/* ---------------------------------------------------------------------
 * 1. Basic set/get/exists/del roundtrip.
 * ------------------------------------------------------------------- */
static void test_basic_crud(void) {
    run_section("basic set/get/exists/del");
    store_t *s = store_create();

    CHECK(store_exists(s, "foo", 3) == 0, "key absent before insert");

    int inserted = store_set(s, "foo", 3, "bar", 3, 0);
    CHECK(inserted == 1, "set on new key reports insert");
    CHECK(store_size(s) == 1, "size is 1 after one insert");

    const char *val; size_t vlen;
    CHECK(store_get(s, "foo", 3, &val, &vlen) == 1, "get finds the key");
    CHECK(vlen == 3 && memcmp(val, "bar", 3) == 0, "value is correct");
    CHECK(store_exists(s, "foo", 3) == 1, "exists is true after insert");

    int updated = store_set(s, "foo", 3, "baz", 3, 0);
    CHECK(updated == 0, "set on existing key reports update, not insert");
    CHECK(store_size(s) == 1, "size stays 1 after overwrite");
    store_get(s, "foo", 3, &val, &vlen);
    CHECK(vlen == 3 && memcmp(val, "baz", 3) == 0, "value reflects the overwrite");

    CHECK(store_get(s, "missing", 7, &val, &vlen) == 0, "get on missing key fails");
    CHECK(store_del(s, "missing", 7) == 0, "del on missing key returns 0");

    CHECK(store_del(s, "foo", 3) == 1, "del on existing key returns 1");
    CHECK(store_size(s) == 0, "size is 0 after delete");
    CHECK(store_exists(s, "foo", 3) == 0, "key gone after delete");

    store_destroy(s);
}

/* ---------------------------------------------------------------------
 * 2. TTL / lazy expiry.
 * ------------------------------------------------------------------- */
static int64_t now_ms_for_test(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void test_ttl_and_expiry(void) {
    run_section("TTL and lazy expiry");
    store_t *s = store_create();

    store_set(s, "nottl", 5, "v", 1, 0);
    CHECK(store_ttl_ms(s, "nottl", 5) == STORE_TTL_NO_EXPIRY, "no TTL set -> NO_EXPIRY sentinel");
    CHECK(store_ttl_ms(s, "ghost", 5) == STORE_TTL_NO_KEY, "missing key -> NO_KEY sentinel");

    store_set(s, "soon", 4, "v", 1, 0);
    int64_t future = now_ms_for_test() + 10000;
    CHECK(store_expire(s, "soon", 4, future) == 1, "expire on existing key succeeds");
    CHECK(store_expire(s, "ghost", 5, future) == 0, "expire on missing key fails");

    int64_t ttl = store_ttl_ms(s, "soon", 4);
    CHECK(ttl > 0 && ttl <= 10000, "TTL reflects the future expiry we set");

    /* Already-past expiry: key should read as gone, and actually be freed
     * (size drops), not just hidden. */
    store_set(s, "already", 7, "v", 1, 0);
    store_expire(s, "already", 7, now_ms_for_test() - 1000);
    size_t before = store_size(s);
    CHECK(store_exists(s, "already", 7) == 0, "past-expiry key reads as absent");
    CHECK(store_size(s) == before - 1, "lazily-expired key is actually removed");
    CHECK(store_ttl_ms(s, "already", 7) == STORE_TTL_NO_KEY, "expired key is fully gone, not just hidden");

    store_destroy(s);
}

/* ---------------------------------------------------------------------
 * 3. INCR / DECR via store_incrby.
 * ------------------------------------------------------------------- */
static void test_incrby(void) {
    run_section("INCR/DECR");
    store_t *s = store_create();
    int64_t result;

    CHECK(store_incrby(s, "counter", 7, 1, &result) == STORE_INCR_OK, "incr on missing key succeeds");
    CHECK(result == 1, "missing key treated as 0, incr by 1 -> 1");

    CHECK(store_incrby(s, "counter", 7, 5, &result) == STORE_INCR_OK, "incr on existing numeric key succeeds");
    CHECK(result == 6, "1 + 5 == 6");

    CHECK(store_incrby(s, "counter", 7, -10, &result) == STORE_INCR_OK, "negative delta (DECR) succeeds");
    CHECK(result == -4, "6 - 10 == -4");

    store_set(s, "notanum", 7, "hello", 5, 0);
    CHECK(store_incrby(s, "notanum", 7, 1, &result) == STORE_INCR_NOT_INTEGER,
          "incr on non-numeric value is rejected");

    store_set(s, "atmax", 5, "9223372036854775807", 19, 0); /* INT64_MAX */
    CHECK(store_incrby(s, "atmax", 5, 1, &result) == STORE_INCR_OVERFLOW, "incr past INT64_MAX overflows");

    store_set(s, "atmin", 5, "-9223372036854775808", 20, 0); /* INT64_MIN */
    CHECK(store_incrby(s, "atmin", 5, -1, &result) == STORE_INCR_OVERFLOW, "decr past INT64_MIN overflows");

    store_destroy(s);
}

/* ---------------------------------------------------------------------
 * 4. Binary safety: embedded NULs and CRLF-looking bytes in keys/values.
 * ------------------------------------------------------------------- */
static void test_binary_safety(void) {
    run_section("binary-safe keys and values");
    store_t *s = store_create();

    char key[5]  = { 'k', '\0', 'e', 'y', '\xff' };
    char val[6]  = { 'v', '\r', '\n', '\0', 'a', 'l' };

    store_set(s, key, sizeof(key), val, sizeof(val), 0);
    const char *got_val; size_t got_len;
    CHECK(store_get(s, key, sizeof(key), &got_val, &got_len) == 1, "binary key is found");
    CHECK(got_len == sizeof(val) && memcmp(got_val, val, sizeof(val)) == 0,
          "binary value preserved exactly, length-driven not NUL-driven");

    store_destroy(s);
}

/* ---------------------------------------------------------------------
 * 5. Growth and incremental rehashing: the main event. We insert enough
 *    keys to force at least one rehash, confirm it actually started, drain
 *    it via ordinary API calls (mirroring how it'll happen in real use --
 *    rehash_step rides along on every call, nothing special invoked), and
 *    confirm every single key is still correctly retrievable both DURING
 *    the rehash and after it completes. Then we do it again to make sure
 *    repeated rehash cycles work, not just a one-shot.
 * ------------------------------------------------------------------- */
#define N_KEYS 2000

static void make_key(char *buf, size_t bufsz, int i) {
    snprintf(buf, bufsz, "key:%d", i);
}

/* Hammer the store with no-op lookups on a key that doesn't exist, purely
 * to ride rehash_step() forward, until rehashing finishes or we give up.
 * Returns 1 if it finished, 0 if the iteration cap was hit. */
static int drain_rehash(store_t *s, int max_iters) {
    for (int i = 0; i < max_iters && store_is_rehashing(s); i++) {
        store_exists(s, "__rehash_probe__", 17);
    }
    return !store_is_rehashing(s);
}

static void test_growth_and_rehashing(void) {
    run_section("growth and incremental rehashing");
    store_t *s = store_create();

    int saw_rehashing_mid_insert = 0;
    char keybuf[32], valbuf[32];

    for (int i = 0; i < N_KEYS; i++) {
        make_key(keybuf, sizeof(keybuf), i);
        snprintf(valbuf, sizeof(valbuf), "val:%d", i);
        store_set(s, keybuf, strlen(keybuf), valbuf, strlen(valbuf), 0);

        if (store_is_rehashing(s)) {
            saw_rehashing_mid_insert = 1;

            /* While mid-rehash, spot-check both an early key (likely
             * already migrated to ht[1]) and the key we just inserted
             * (likely still sitting in whichever table store_set picked).
             * Both must be found correctly -- this is the actual point of
             * incremental rehashing: reads during the migration must never
             * see a hole. */
            const char *val; size_t vlen;
            char early_key[32];
            make_key(early_key, sizeof(early_key), 0);
            CHECK(store_get(s, early_key, strlen(early_key), &val, &vlen) == 1,
                  "early key still found mid-rehash");

            CHECK(store_get(s, keybuf, strlen(keybuf), &val, &vlen) == 1,
                  "just-inserted key found mid-rehash");
        }
    }
    CHECK(saw_rehashing_mid_insert == 1, "inserting N_KEYS actually triggered a rehash");
    CHECK(store_size(s) == (size_t)N_KEYS, "size is exactly N_KEYS, no duplicates/drops from rehashing");

    int drained = drain_rehash(s, 50000);
    CHECK(drained == 1, "rehash fully drains within a generous iteration budget");
    CHECK(store_is_rehashing(s) == 0, "not rehashing once drained");

    /* Every key must still be there with the right value after rehashing
     * has fully completed. */
    int all_ok = 1;
    for (int i = 0; i < N_KEYS; i++) {
        make_key(keybuf, sizeof(keybuf), i);
        snprintf(valbuf, sizeof(valbuf), "val:%d", i);
        const char *val; size_t vlen;
        if (!store_get(s, keybuf, strlen(keybuf), &val, &vlen) ||
            vlen != strlen(valbuf) || memcmp(val, valbuf, vlen) != 0) {
            all_ok = 0;
            break;
        }
    }
    CHECK(all_ok == 1, "all N_KEYS survive a full rehash cycle with correct values");
    CHECK(store_size(s) == (size_t)N_KEYS, "size still exactly N_KEYS post-rehash");

    /* Delete half the keys, then insert a fresh batch to force a SECOND
     * rehash cycle, making sure the table isn't a one-shot. */
    for (int i = 0; i < N_KEYS / 2; i++) {
        make_key(keybuf, sizeof(keybuf), i);
        store_del(s, keybuf, strlen(keybuf));
    }
    for (int i = N_KEYS; i < N_KEYS + N_KEYS / 2; i++) {
        make_key(keybuf, sizeof(keybuf), i);
        snprintf(valbuf, sizeof(valbuf), "val:%d", i);
        store_set(s, keybuf, strlen(keybuf), valbuf, strlen(valbuf), 0);
    }
    drain_rehash(s, 50000);
    CHECK(store_size(s) == (size_t)N_KEYS, "size correct after a second churn+rehash cycle");

    store_destroy(s);
}

int main(void) {
    test_basic_crud();
    test_ttl_and_expiry();
    test_incrby();
    test_binary_safety();
    test_growth_and_rehashing();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
