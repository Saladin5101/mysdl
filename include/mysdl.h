/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "engine.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MYSDL_DB_DIR  "/var/lib/mysdl"

/* SDL-level handle is just the engine */
typedef engine_t mysdl_db_t;

/* Parse "300ms" / "5s" / "1m" → milliseconds. Returns 0 on error. */
uint64_t mysdl_parse_ttl(const char *s);

static inline const char *mysdl_db_dir(void) {
    const char *e = getenv("MYSDL_DB");
    return e ? e : MYSDL_DB_DIR;
}

static inline mysdl_db_t *mysdl_open(void) {
    return eng_open(mysdl_db_dir(), ENG_CREAT);
}
static inline mysdl_db_t *mysdl_open_ro(void) {
    return eng_open(mysdl_db_dir(), ENG_RDONLY);
}
static inline void mysdl_close(mysdl_db_t *db) { eng_close(db); }
static inline void mysdl_sync(mysdl_db_t *db)  { eng_sync(db);  }

static inline int mysdl_set(mysdl_db_t *db, const char *key,
                             const char *val, uint64_t ttl_ms) {
    return eng_put(db, key, (uint32_t)strlen(key),
                   val, (uint32_t)strlen(val), ttl_ms);
}
static inline char *mysdl_get(mysdl_db_t *db, const char *key) {
    return eng_get(db, key, (uint32_t)strlen(key), NULL);
}
static inline int mysdl_del(mysdl_db_t *db, const char *key) {
    return eng_del(db, key, (uint32_t)strlen(key));
}
static inline int mysdl_exists(mysdl_db_t *db, const char *key) {
    return eng_exists(db, key, (uint32_t)strlen(key));
}
static inline int64_t mysdl_ttl(mysdl_db_t *db, const char *key) {
    return eng_ttl(db, key, (uint32_t)strlen(key));
}
static inline int mysdl_count(mysdl_db_t *db) {
    return eng_count(db);
}
static inline int mysdl_compact(mysdl_db_t *db) {
    return eng_compact(db);
}

/* flush = delete all keys by rebuilding an empty log */
void mysdl_flush(mysdl_db_t *db);

/* rename old key to new key, preserving value and TTL */
int mysdl_rename(mysdl_db_t *db, const char *old_key, const char *new_key);

typedef struct {
    int (*cb)(const char *key, const char *val, void *arg);
    void *arg;
} _scan_wrap_t;

static int _scan_cb(const char *key, uint32_t klen,
                    const char *val, uint32_t vlen, void *arg) {
    (void)klen; (void)vlen;
    _scan_wrap_t *w = arg;
    return w->cb(key, val, w->arg);
}
static inline void mysdl_scan(mysdl_db_t *db,
                               int (*cb)(const char *key, const char *val,
                                         void *arg),
                               void *arg) {
    _scan_wrap_t w = { cb, arg };
    eng_scan(db, _scan_cb, &w);
}
