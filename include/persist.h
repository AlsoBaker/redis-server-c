#ifndef PERSIST_H
#define PERSIST_H

#include "store.h"

/*
 * Simple binary snapshot persistence, modelled loosely on Redis RDB.
 *
 * File format (all integers little-endian):
 *
 *   [8 bytes]  Magic: "KVSTORE1"
 *   [8 bytes]  Entry count (uint64_t)
 *   For each entry:
 *     [8 bytes]  key_len  (uint64_t)
 *     [key_len]  key bytes
 *     [8 bytes]  val_len  (uint64_t)
 *     [val_len]  val bytes
 *     [8 bytes]  expire_at_ms (int64_t; 0 = no expiry)
 *   [4 bytes]  Checksum: uint32_t sum of every preceding byte mod 2^32
 *
 * Saves are atomic: persist_save writes to <path>.tmp and then calls
 * rename(2), which is atomic on POSIX. A crash mid-save therefore never
 * corrupts the last good snapshot.
 *
 * persist_load skips entries whose expire_at_ms is in the past, so a
 * snapshot taken just before shutdown doesn't resurrect already-dead keys.
 */

typedef enum {
    PERSIST_OK = 0,
    PERSIST_ERR_IO,       /* file open / read / write / rename failed */
    PERSIST_ERR_FORMAT,   /* wrong magic bytes or unsupported version    */
    PERSIST_ERR_CORRUPT,  /* checksum mismatch -- file is damaged        */
} persist_status_t;

persist_status_t persist_save(store_t *s, const char *path);
persist_status_t persist_load(store_t *s, const char *path);

/* Human-readable description of a status code, suitable for error logs. */
const char *persist_strerror(persist_status_t st);

#endif /* PERSIST_H */
