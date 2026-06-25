#define _POSIX_C_SOURCE 200809L

#include "persist.h"
#include "store.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

static void run_section(const char *name) { printf("-- %s\n", name); }

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* -----------------------------------------------------------------------
 * 1. Happy-path roundtrip: fill a store, save it, load into a fresh
 *    store, confirm every key and value survives intact.
 * --------------------------------------------------------------------- */
static void test_roundtrip(void) {
    run_section("save/load roundtrip");

    store_t *src = store_create();
    store_set(src, "hello", 5, "world", 5, 0);
    store_set(src, "num",   3, "42",    2, 0);
    /* binary-safe key and value with embedded NULs */
    char bkey[4] = {'k', '\0', 'e', 'y'};
    char bval[4] = {'v', '\r', '\n', 'l'};
    store_set(src, bkey, sizeof(bkey), bval, sizeof(bval), 0);

    const char *path = "/tmp/test_persist_roundtrip.kvs";
    persist_status_t st = persist_save(src, path);
    CHECK(st == PERSIST_OK, "persist_save returns OK");

    store_t *dst = store_create();
    st = persist_load(dst, path);
    CHECK(st == PERSIST_OK, "persist_load returns OK");

    CHECK(store_size(dst) == 3, "loaded store has all 3 keys");

    const char *val; size_t vlen;
    CHECK(store_get(dst, "hello", 5, &val, &vlen) == 1 &&
          vlen == 5 && memcmp(val, "world", 5) == 0,
          "string key/value round-trips exactly");

    CHECK(store_get(dst, "num", 3, &val, &vlen) == 1 &&
          vlen == 2 && memcmp(val, "42", 2) == 0,
          "numeric string value round-trips exactly");

    CHECK(store_get(dst, bkey, sizeof(bkey), &val, &vlen) == 1 &&
          vlen == sizeof(bval) && memcmp(val, bval, sizeof(bval)) == 0,
          "binary-safe key/value with embedded NULs/CRLF round-trips exactly");

    store_destroy(src);
    store_destroy(dst);
    unlink(path);
}

/* -----------------------------------------------------------------------
 * 2. TTL preservation: keys saved with a future expiry should load with
 *    that expiry still set; keys saved with a past expiry should NOT
 *    reappear in the loaded store.
 * --------------------------------------------------------------------- */
static void test_ttl_roundtrip(void) {
    run_section("TTL preserved across save/load");

    store_t *src = store_create();

    /* Key with a future TTL (10 seconds from now). */
    int64_t future = now_ms() + 10000;
    store_set(src, "alive", 5, "yes", 3, future);

    /* Key whose TTL has already passed -- should be skipped on save
     * (store_foreach skips expired entries) and never appear in dst. */
    store_set(src, "dead", 4, "no", 2, 0);
    store_expire(src, "dead", 4, now_ms() - 500);

    const char *path = "/tmp/test_persist_ttl.kvs";
    persist_save(src, path);

    store_t *dst = store_create();
    persist_load(dst, path);

    CHECK(store_size(dst) == 1, "already-expired key is not saved/loaded");
    CHECK(store_exists(dst, "alive", 5) == 1, "key with future TTL survives");
    CHECK(store_exists(dst, "dead",  4) == 0, "key with past TTL does not appear");

    int64_t ttl = store_ttl_ms(dst, "alive", 5);
    CHECK(ttl > 0 && ttl <= 10000,
          "loaded key retains its future expiry (TTL is positive and reasonable)");

    store_destroy(src);
    store_destroy(dst);
    unlink(path);
}

/* -----------------------------------------------------------------------
 * 3. Empty store saves and loads cleanly.
 * --------------------------------------------------------------------- */
static void test_empty_store(void) {
    run_section("empty store round-trips cleanly");

    store_t *src = store_create();
    const char *path = "/tmp/test_persist_empty.kvs";
    persist_status_t st = persist_save(src, path);
    CHECK(st == PERSIST_OK, "saving an empty store succeeds");

    store_t *dst = store_create();
    st = persist_load(dst, path);
    CHECK(st == PERSIST_OK, "loading an empty snapshot succeeds");
    CHECK(store_size(dst) == 0, "loaded store is empty");

    store_destroy(src);
    store_destroy(dst);
    unlink(path);
}

/* -----------------------------------------------------------------------
 * 4. Save is atomic: the final file either appears intact or not at all.
 *    We verify this by checking that a .tmp file is NOT left behind after
 *    a successful save -- it was renamed to the real path.
 * --------------------------------------------------------------------- */
static void test_atomic_save(void) {
    run_section("save is atomic (no leftover .tmp file)");

    store_t *s = store_create();
    store_set(s, "k", 1, "v", 1, 0);

    const char *path = "/tmp/test_persist_atomic.kvs";
    char tmppath[256];
    snprintf(tmppath, sizeof(tmppath), "%s.tmp", path);

    persist_save(s, path);

    CHECK(access(path,    F_OK) == 0, "final file exists after save");
    CHECK(access(tmppath, F_OK) != 0, ".tmp file is NOT left behind (rename succeeded)");

    store_destroy(s);
    unlink(path);
}

/* -----------------------------------------------------------------------
 * 5. Checksum: flipping a byte in the saved file is detected on load.
 * --------------------------------------------------------------------- */
static void test_checksum_detection(void) {
    run_section("checksum detects corruption");

    store_t *s = store_create();
    store_set(s, "key", 3, "val", 3, 0);

    const char *path = "/tmp/test_persist_corrupt.kvs";
    persist_save(s, path);

    /* Flip a byte in the middle of the file (in the key/value payload). */
    FILE *f = fopen(path, "r+b");
    CHECK(f != NULL, "can open saved file for modification");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        /* Corrupt a byte roughly in the middle -- away from header and
         * checksum at the very end. */
        fseek(f, sz / 2, SEEK_SET);
        int c = fgetc(f);
        fseek(f, sz / 2, SEEK_SET);
        fputc(c ^ 0xff, f);
        fclose(f);

        store_t *dst = store_create();
        persist_status_t st = persist_load(dst, path);
        CHECK(st == PERSIST_ERR_CORRUPT,
              "corrupted file is detected by checksum (PERSIST_ERR_CORRUPT)");
        store_destroy(dst);
    }

    store_destroy(s);
    unlink(path);
}

/* -----------------------------------------------------------------------
 * 6. Missing file returns ERR_IO (not a crash).
 * 7. Wrong magic returns ERR_FORMAT (not a crash).
 * --------------------------------------------------------------------- */
static void test_error_cases(void) {
    run_section("error cases: missing file, wrong magic");

    store_t *s = store_create();

    persist_status_t st = persist_load(s, "/tmp/no_such_file_kjdhfkjds.kvs");
    CHECK(st == PERSIST_ERR_IO, "loading a missing file returns ERR_IO");

    /* Write garbage to a temp file and try loading it. */
    const char *bad = "/tmp/test_persist_bad_magic.kvs";
    FILE *f = fopen(bad, "wb");
    if (f) {
        fwrite("NOTMAGIC!!!!", 1, 12, f);
        fclose(f);
        st = persist_load(s, bad);
        CHECK(st == PERSIST_ERR_FORMAT,
              "file with wrong magic returns ERR_FORMAT");
        unlink(bad);
    }

    store_destroy(s);
}

/* -----------------------------------------------------------------------
 * 8. Large snapshot: save and reload 10,000 keys, confirm all survive.
 * --------------------------------------------------------------------- */
#define BIG_N 10000
static void test_large_snapshot(void) {
    run_section("large snapshot (10k keys)");

    store_t *src = store_create();
    char keybuf[32], valbuf[32];
    for (int i = 0; i < BIG_N; i++) {
        snprintf(keybuf, sizeof(keybuf), "bigkey:%d", i);
        snprintf(valbuf, sizeof(valbuf), "bigval:%d", i);
        store_set(src, keybuf, strlen(keybuf), valbuf, strlen(valbuf), 0);
    }

    const char *path = "/tmp/test_persist_large.kvs";
    persist_status_t st = persist_save(src, path);
    CHECK(st == PERSIST_OK, "saving 10k keys succeeds");

    store_t *dst = store_create();
    st = persist_load(dst, path);
    CHECK(st == PERSIST_OK, "loading 10k keys succeeds");
    CHECK(store_size(dst) == BIG_N, "all 10k keys present after load");

    int all_ok = 1;
    for (int i = 0; i < BIG_N; i++) {
        snprintf(keybuf, sizeof(keybuf), "bigkey:%d", i);
        snprintf(valbuf, sizeof(valbuf), "bigval:%d", i);
        const char *val; size_t vlen;
        if (!store_get(dst, keybuf, strlen(keybuf), &val, &vlen) ||
            vlen != strlen(valbuf) || memcmp(val, valbuf, vlen) != 0) {
            all_ok = 0; break;
        }
    }
    CHECK(all_ok, "all 10k key/value pairs are byte-perfect after load");

    store_destroy(src);
    store_destroy(dst);
    unlink(path);
}

int main(void) {
    test_roundtrip();
    test_ttl_roundtrip();
    test_empty_store();
    test_atomic_save();
    test_checksum_detection();
    test_error_cases();
    test_large_snapshot();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
