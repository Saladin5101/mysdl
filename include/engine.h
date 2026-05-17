/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "plugin.h"

/*
 * Bitcask-style storage engine.
 *
 * On-disk log record layout (binary, little-endian):
 *   [crc32:4][expire_ms:8][key_len:2][val_len:4][key:key_len][val:val_len]
 *
 * A tombstone is a record with val_len == 0.
 * expire_ms == 0 means no expiry.
 *
 * In-memory index: hash table mapping key → (file_offset, val_len, expire_ms).
 */

#define ENGINE_KEY_MAX  256
#define ENGINE_VAL_MAX  (1 << 20)   /* 1 MiB */
#define ENGINE_BUCKETS  4096

typedef struct eng_idx {
    char     key[ENGINE_KEY_MAX];
    uint32_t key_len;
    uint64_t offset;     /* byte offset of record start in log */
    uint32_t val_len;
    uint64_t expire_ms;
    int      used;
    struct eng_idx *next; /* chaining for collisions */
} eng_idx_t;

typedef struct engine {
    int               log_fd;
    int               lock_fd;
    int               flags;
    char              path[256];
    eng_idx_t        *idx[ENGINE_BUCKETS];
    uint64_t          log_size;
    uint64_t          dead_bytes;
    plugin_registry_t plugins;
} engine_t;

/* Register/unregister a plugin on this engine instance. */
static inline int eng_plugin_register(engine_t *e, mysdl_plugin_t *p) {
    return plugin_register(&e->plugins, p);
}
static inline int eng_plugin_unregister(engine_t *e, const char *name) {
    return plugin_unregister(&e->plugins, name);
}

engine_t   *eng_open(const char *dir, int flags);
void        eng_close(engine_t *e);
void        eng_sync(engine_t *e);

int         eng_put(engine_t *e, const char *key, uint32_t klen,
                    const char *val, uint32_t vlen, uint64_t ttl_ms);
/* Returns val in heap-allocated buffer; caller must free(). NULL = not found. */
char       *eng_get(engine_t *e, const char *key, uint32_t klen,
                    uint32_t *vlen_out);
int         eng_del(engine_t *e, const char *key, uint32_t klen);
int         eng_exists(engine_t *e, const char *key, uint32_t klen);
int64_t     eng_ttl(engine_t *e, const char *key, uint32_t klen);

/* Iterate all live keys. cb returns non-zero to stop. */
void        eng_scan(engine_t *e,
                     int (*cb)(const char *key, uint32_t klen,
                               const char *val, uint32_t vlen, void *arg),
                     void *arg);
int         eng_count(engine_t *e);

/* Rewrite log, dropping dead records. */
int         eng_compact(engine_t *e);

#define ENG_CREAT   1
#define ENG_SYNC    2   /* fsync on every write */
#define ENG_RDONLY  4   /* shared read lock, no writes */
#define ENG_COMPACT_RATIO 50  /* auto-compact when dead% >= this */
