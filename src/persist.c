#define _POSIX_C_SOURCE 200809L

#include "persist.h"
#include "store.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

/* -----------------------------------------------------------------------
 * Wire helpers: explicit little-endian so snapshots are portable.
 * --------------------------------------------------------------------- */
static void write_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) { p[i] = (uint8_t)(v & 0xff); v >>= 8; }
}
static uint64_t read_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static void write_u32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) { p[i] = (uint8_t)(v & 0xff); v >>= 8; }
}
static uint32_t read_u32(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static uint32_t checksum(const uint8_t *buf, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += buf[i];
    return sum;
}

#define MAGIC "KVSTORE1"
#define MAGIC_LEN 8

/* -----------------------------------------------------------------------
 * Growable byte buffer used during save.
 * --------------------------------------------------------------------- */
typedef struct {
    uint8_t *buf;
    size_t   len, cap;
    size_t   count;   /* number of entries serialized */
    int      err;
} save_ctx_t;

static void ctx_append(save_ctx_t *ctx, const void *data, size_t n) {
    if (ctx->err) return;
    if (ctx->len + n > ctx->cap) {
        size_t newcap = ctx->cap ? ctx->cap * 2 : 65536;
        while (newcap < ctx->len + n) newcap *= 2;
        uint8_t *g = realloc(ctx->buf, newcap);
        if (!g) { ctx->err = 1; return; }
        ctx->buf = g; ctx->cap = newcap;
    }
    memcpy(ctx->buf + ctx->len, data, n);
    ctx->len += n;
}

/* store_foreach callback: serialize one entry and increment the count. */
static void save_cb(const char *key, size_t keylen,
                    const char *val, size_t vallen,
                    int64_t expire_at_ms, void *ud) {
    save_ctx_t *ctx = (save_ctx_t *)ud;
    uint8_t hdr[8];

    write_u64(hdr, (uint64_t)keylen);
    ctx_append(ctx, hdr, 8);
    ctx_append(ctx, key, keylen);

    write_u64(hdr, (uint64_t)vallen);
    ctx_append(ctx, hdr, 8);
    ctx_append(ctx, val, vallen);

    write_u64(hdr, (uint64_t)(int64_t)expire_at_ms);
    ctx_append(ctx, hdr, 8);

    ctx->count++;
}

persist_status_t persist_save(store_t *s, const char *path) {
    /* -- Build the whole file in memory first --
     * This way we either write a complete valid snapshot or nothing at all.
     * Layout: [magic 8][count 8][entries ...][checksum 4] */

    save_ctx_t ctx = {NULL, 0, 0, 0, 0};

    /* Magic (8 bytes) then a placeholder for the count (8 bytes). We will
     * backfill the count after store_foreach tells us how many entries
     * were actually serialized (expired-but-not-yet-deleted entries are
     * skipped by store_foreach, so store_size() would be an overcount). */
    uint8_t header[MAGIC_LEN + 8] = {0};
    memcpy(header, MAGIC, MAGIC_LEN);
    ctx_append(&ctx, header, sizeof(header));

    /* Serialize all live entries. */
    store_foreach(s, save_cb, &ctx);
    if (ctx.err) { free(ctx.buf); return PERSIST_ERR_IO; }

    /* Backfill the real entry count. */
    write_u64(ctx.buf + MAGIC_LEN, (uint64_t)ctx.count);

    /* Checksum of everything so far. */
    uint8_t csum[4];
    write_u32(csum, checksum(ctx.buf, ctx.len));
    ctx_append(&ctx, csum, 4);
    if (ctx.err) { free(ctx.buf); return PERSIST_ERR_IO; }

    /* -- Atomic write: write to <path>.tmp then rename -- */
    size_t pathlen = strlen(path);
    char *tmppath = malloc(pathlen + 5);
    if (!tmppath) { free(ctx.buf); return PERSIST_ERR_IO; }
    memcpy(tmppath, path, pathlen);
    memcpy(tmppath + pathlen, ".tmp", 5);

    FILE *f = fopen(tmppath, "wb");
    if (!f) { free(ctx.buf); free(tmppath); return PERSIST_ERR_IO; }

    int ok = (fwrite(ctx.buf, 1, ctx.len, f) == ctx.len);
    ok &= (fflush(f) == 0);
    fclose(f);
    free(ctx.buf);

    if (!ok || rename(tmppath, path) != 0) {
        remove(tmppath);
        free(tmppath);
        return PERSIST_ERR_IO;
    }
    free(tmppath);
    return PERSIST_OK;
}

/* -----------------------------------------------------------------------
 * Loading
 * --------------------------------------------------------------------- */
persist_status_t persist_load(store_t *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return PERSIST_ERR_IO;

    /* Read the entire file into memory. */
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);

    if (fsz < (long)(MAGIC_LEN + 8 + 4)) {
        /* Too small to be a valid snapshot even with zero entries. */
        fclose(f);
        return PERSIST_ERR_FORMAT;
    }

    uint8_t *data = malloc((size_t)fsz);
    if (!data) { fclose(f); return PERSIST_ERR_IO; }

    if ((long)fread(data, 1, (size_t)fsz, f) != fsz) {
        fclose(f); free(data); return PERSIST_ERR_IO;
    }
    fclose(f);

    /* Verify magic. */
    if (memcmp(data, MAGIC, MAGIC_LEN) != 0) {
        free(data); return PERSIST_ERR_FORMAT;
    }

    /* Verify checksum: the stored checksum is the last 4 bytes; everything
     * before it should sum to that value. */
    uint32_t stored_csum  = read_u32(data + (size_t)fsz - 4);
    uint32_t computed_csum = checksum(data, (size_t)fsz - 4);
    if (stored_csum != computed_csum) {
        free(data); return PERSIST_ERR_CORRUPT;
    }

    uint64_t count = read_u64(data + MAGIC_LEN);
    size_t   pos   = MAGIC_LEN + 8;
    size_t   limit = (size_t)fsz - 4; /* stop before the checksum field */

    /* Get current time once so we can filter entries that have already
     * expired since the snapshot was taken. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    for (uint64_t i = 0; i < count; i++) {
        if (pos + 8 > limit) { free(data); return PERSIST_ERR_CORRUPT; }
        uint64_t keylen = read_u64(data + pos); pos += 8;

        if (pos + keylen > limit) { free(data); return PERSIST_ERR_CORRUPT; }
        const char *key = (const char *)(data + pos); pos += keylen;

        if (pos + 8 > limit) { free(data); return PERSIST_ERR_CORRUPT; }
        uint64_t vallen = read_u64(data + pos); pos += 8;

        if (pos + vallen > limit) { free(data); return PERSIST_ERR_CORRUPT; }
        const char *val = (const char *)(data + pos); pos += vallen;

        if (pos + 8 > limit) { free(data); return PERSIST_ERR_CORRUPT; }
        int64_t expire_at_ms = (int64_t)read_u64(data + pos); pos += 8;

        /* Skip entries that already expired while the server was down. */
        if (expire_at_ms != 0 && expire_at_ms <= now) continue;

        store_set(s, key, (size_t)keylen, val, (size_t)vallen, expire_at_ms);
    }

    free(data);
    return PERSIST_OK;
}

const char *persist_strerror(persist_status_t st) {
    switch (st) {
        case PERSIST_OK:          return "OK";
        case PERSIST_ERR_IO:      return "I/O error";
        case PERSIST_ERR_FORMAT:  return "unrecognized file format";
        case PERSIST_ERR_CORRUPT: return "checksum mismatch (file corrupted)";
        default:                  return "unknown error";
    }
}
