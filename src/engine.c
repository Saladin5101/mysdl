/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine.h"
#include "plugin.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/uio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>

/* ── time ──────────────────────────────────────────────────────── */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ── CRC-32 (ISO 3309) ─────────────────────────────────────────── */

static uint32_t crc32_update(uint32_t crc, const void *buf, size_t len) {
    static const uint32_t tbl[16] = {
        0x00000000,0x1db71064,0x3b6e20c8,0x26d930ac,
        0x76dc4190,0x6b6b51f4,0x4db26158,0x5005713c,
        0xedb88320,0xf00f9344,0xd6d6a3e8,0xcb61b38c,
        0x9b64c2b0,0x86d3d2d4,0xa00ae278,0xbdbdf21c,
    };
    const uint8_t *p = buf;
    crc = ~crc;
    while (len--) {
        crc = (crc >> 4) ^ tbl[(crc ^ *p)       & 0xf];
        crc = (crc >> 4) ^ tbl[(crc ^ (*p >> 4)) & 0xf];
        p++;
    }
    return ~crc;
}

/* ── on-disk record ────────────────────────────────────────────── */
/*
 * [crc32:4][expire_ms:8][key_len:2][val_len:4][key][val]
 * val_len == 0  →  tombstone (delete marker)
 */
#define HDR_SIZE (4 + 8 + 2 + 4)

static void write_le32(uint8_t *p, uint32_t v) {
    p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24;
}
static void write_le64(uint8_t *p, uint64_t v) {
    for (int i=0;i<8;i++) { p[i]=v&0xff; v>>=8; }
}
static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|
           ((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static uint64_t read_le64(const uint8_t *p) {
    uint64_t v=0;
    for (int i=7;i>=0;i--) v=(v<<8)|p[i];
    return v;
}

/* Write one record; returns bytes written or -1. */
static ssize_t write_record(int fd, const char *key, uint32_t klen,
                             const char *val, uint32_t vlen,
                             uint64_t expire_ms) {
    uint8_t hdr[HDR_SIZE];
    write_le64(hdr + 4, expire_ms);
    hdr[12] = klen & 0xff; hdr[13] = (klen >> 8) & 0xff;
    write_le32(hdr + 14, vlen);

    uint32_t crc = crc32_update(0, hdr + 4, HDR_SIZE - 4);
    crc = crc32_update(crc, key, klen);
    if (vlen) crc = crc32_update(crc, val, vlen);
    write_le32(hdr, crc);

    struct iovec iov[3];
    iov[0].iov_base = hdr;  iov[0].iov_len = HDR_SIZE;
    iov[1].iov_base = (void*)key; iov[1].iov_len = klen;
    iov[2].iov_base = (void*)val; iov[2].iov_len = vlen;
    int iovcnt = vlen ? 3 : 2;

    ssize_t total = HDR_SIZE + klen + vlen;
    if (writev(fd, iov, iovcnt) != total) return -1;
    return total;
}

/* ── in-memory index ───────────────────────────────────────────── */

static unsigned int idx_hash(const char *key, uint32_t klen) {
    unsigned int h = 5381;
    for (uint32_t i = 0; i < klen; i++)
        h = h * 33 ^ (unsigned char)key[i];
    return h % ENGINE_BUCKETS;
}

static eng_idx_t *idx_find(engine_t *e, const char *key, uint32_t klen) {
    unsigned int h = idx_hash(key, klen);
    for (eng_idx_t *n = e->idx[h]; n; n = n->next)
        if (n->key_len == klen && !memcmp(n->key, key, klen))
            return n;
    return NULL;
}

static eng_idx_t *idx_upsert(engine_t *e, const char *key, uint32_t klen) {
    unsigned int h = idx_hash(key, klen);
    for (eng_idx_t *n = e->idx[h]; n; n = n->next)
        if (n->key_len == klen && !memcmp(n->key, key, klen))
            return n;
    eng_idx_t *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    memcpy(n->key, key, klen < ENGINE_KEY_MAX ? klen : ENGINE_KEY_MAX - 1);
    n->key_len = klen;
    n->next = e->idx[h];
    e->idx[h] = n;
    return n;
}

static void idx_remove(engine_t *e, const char *key, uint32_t klen) {
    unsigned int h = idx_hash(key, klen);
    eng_idx_t **pp = &e->idx[h];
    while (*pp) {
        if ((*pp)->key_len == klen && !memcmp((*pp)->key, key, klen)) {
            eng_idx_t *dead = *pp;
            *pp = dead->next;
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
}

static void idx_free_all(engine_t *e) {
    for (int i = 0; i < ENGINE_BUCKETS; i++) {
        eng_idx_t *n = e->idx[i];
        while (n) { eng_idx_t *nx = n->next; free(n); n = nx; }
        e->idx[i] = NULL;
    }
}

/* ── crash recovery: rebuild index by scanning log ────────────── */

static int rebuild_index(engine_t *e) {
    if (lseek(e->log_fd, 0, SEEK_SET) < 0) return -1;
    uint64_t off = 0;
    uint8_t hdr[HDR_SIZE];
    uint64_t now = now_ms();

    while (1) {
        ssize_t r = read(e->log_fd, hdr, HDR_SIZE);
        if (r == 0) break;
        if (r != HDR_SIZE) break;   /* truncated record → stop */

        uint32_t stored_crc = read_le32(hdr);
        uint64_t expire_ms  = read_le64(hdr + 4);
        uint32_t klen       = (uint32_t)hdr[12] | ((uint32_t)hdr[13] << 8);
        uint32_t vlen       = read_le32(hdr + 14);

        if (klen == 0 || klen > ENGINE_KEY_MAX) break;
        if (vlen > (uint32_t)ENGINE_VAL_MAX)    break;

        char *key = malloc(klen + vlen);
        if (!key) break;
        if (read(e->log_fd, key, klen + vlen) != (ssize_t)(klen + vlen)) {
            free(key); break;
        }
        char *val = key + klen;

        /* verify CRC */
        uint32_t crc = crc32_update(0, hdr + 4, HDR_SIZE - 4);
        crc = crc32_update(crc, key, klen);
        if (vlen) crc = crc32_update(crc, val, vlen);
        if (crc != stored_crc) { free(key); break; }

        uint64_t rec_size = HDR_SIZE + klen + vlen;

        if (vlen == 0) {
            /* tombstone */
            eng_idx_t *old = idx_find(e, key, klen);
            if (old) { e->dead_bytes += HDR_SIZE + klen + old->val_len; }
            e->dead_bytes += rec_size;
            idx_remove(e, key, klen);
        } else if (expire_ms && now > expire_ms) {
            /* expired */
            e->dead_bytes += rec_size;
            idx_remove(e, key, klen);
        } else {
            eng_idx_t *old = idx_find(e, key, klen);
            if (old) e->dead_bytes += HDR_SIZE + klen + old->val_len;
            eng_idx_t *n = idx_upsert(e, key, klen);
            if (n) {
                n->offset    = off;
                n->val_len   = vlen;
                n->expire_ms = expire_ms;
                n->used      = 1;
            }
        }

        free(key);
        off += rec_size;
    }

    e->log_size = off;
    /* seek to end for appending */
    lseek(e->log_fd, 0, SEEK_END);
    return 0;
}

/* ── open / close ──────────────────────────────────────────────── */

engine_t *eng_open(const char *dir, int flags) {
    engine_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;

    e->flags = flags;
    strncpy(e->path, dir, sizeof(e->path) - 1);
    if (mkdir(dir, 0755) < 0 && errno != EEXIST) { free(e); return NULL; }

    char lockpath[280];
    snprintf(lockpath, sizeof(lockpath), "%s/lock", dir);
    e->lock_fd = open(lockpath, O_CREAT | O_RDWR, 0600);
    if (e->lock_fd < 0) { free(e); return NULL; }

    /* readers share the lock; writers take it exclusively */
    int lkmode = (flags & ENG_RDONLY) ? LOCK_SH : LOCK_EX;
    if (flock(e->lock_fd, lkmode | LOCK_NB) < 0) {
        close(e->lock_fd); free(e); return NULL;
    }

    char logpath[280];
    snprintf(logpath, sizeof(logpath), "%s/data.log", dir);
    int oflags = (flags & ENG_RDONLY)
                 ? O_RDONLY
                 : (O_RDWR | O_CREAT | ((flags & ENG_SYNC) ? O_SYNC : 0));
    e->log_fd = open(logpath, oflags, 0600);
    if (e->log_fd < 0) {
        flock(e->lock_fd, LOCK_UN); close(e->lock_fd); free(e); return NULL;
    }

    rebuild_index(e);
    return e;
}

void eng_sync(engine_t *e) { fsync(e->log_fd); }

void eng_close(engine_t *e) {
    fsync(e->log_fd);
    close(e->log_fd);
    flock(e->lock_fd, LOCK_UN);
    close(e->lock_fd);
    idx_free_all(e);
    free(e);
}

/* ── put ───────────────────────────────────────────────────────── */

/* Auto-compact when dead bytes exceed ENG_COMPACT_RATIO% of log size. */
static void maybe_compact(engine_t *e) {
    if (e->log_size == 0) return;
    if (e->dead_bytes * 100 / e->log_size >= ENG_COMPACT_RATIO)
        eng_compact(e);
}

int eng_put(engine_t *e, const char *key, uint32_t klen,
            const char *val, uint32_t vlen, uint64_t ttl_ms) {
    if (e->flags & ENG_RDONLY) return -1;
    if (!klen || klen > ENGINE_KEY_MAX) return -1;
    if (vlen > (uint32_t)ENGINE_VAL_MAX) return -1;

    uint64_t expire_ms = ttl_ms ? now_ms() + ttl_ms : 0;
    uint64_t off = e->log_size;
    ssize_t written = write_record(e->log_fd, key, klen, val, vlen, expire_ms);
    if (written < 0) return -1;

    eng_idx_t *old = idx_find(e, key, klen);
    if (old) e->dead_bytes += HDR_SIZE + klen + old->val_len;

    eng_idx_t *n = idx_upsert(e, key, klen);
    if (!n) return -1;
    n->offset    = off;
    n->val_len   = vlen;
    n->expire_ms = expire_ms;
    n->used      = 1;
    e->log_size += written;
    maybe_compact(e);
    plugin_fire_put(&e->plugins, e, key, klen, val, vlen, expire_ms);
    return 0;
}

/* ── get ───────────────────────────────────────────────────────── */

char *eng_get(engine_t *e, const char *key, uint32_t klen, uint32_t *vlen_out) {
    eng_idx_t *n = idx_find(e, key, klen);
    if (!n || !n->used) return NULL;
    if (n->expire_ms && now_ms() > n->expire_ms) {
        idx_remove(e, key, klen);
        return NULL;
    }
    if (n->val_len == 0) return NULL;

    char *buf = malloc(n->val_len + 1);
    if (!buf) return NULL;

    uint64_t val_off = n->offset + HDR_SIZE + klen;
    if (pread(e->log_fd, buf, n->val_len, (off_t)val_off) != (ssize_t)n->val_len) {
        free(buf); return NULL;
    }
    buf[n->val_len] = '\0';
    if (vlen_out) *vlen_out = n->val_len;
    plugin_fire_get(&e->plugins, e, key, klen, buf, n->val_len);
    return buf;
}

/* ── del ───────────────────────────────────────────────────────── */

int eng_del(engine_t *e, const char *key, uint32_t klen) {
    if (e->flags & ENG_RDONLY) return -1;
    eng_idx_t *n = idx_find(e, key, klen);
    if (!n || !n->used) return -1;

    ssize_t written = write_record(e->log_fd, key, klen, NULL, 0, 0);
    if (written < 0) return -1;
    e->dead_bytes += HDR_SIZE + klen + n->val_len;
    e->dead_bytes += written;
    e->log_size   += written;
    idx_remove(e, key, klen);
    maybe_compact(e);
    plugin_fire_del(&e->plugins, e, key, klen);
    return 0;
}

/* ── exists / ttl ──────────────────────────────────────────────── */

int eng_exists(engine_t *e, const char *key, uint32_t klen) {
    eng_idx_t *n = idx_find(e, key, klen);
    if (!n || !n->used) return 0;
    if (n->expire_ms && now_ms() > n->expire_ms) {
        idx_remove(e, key, klen); return 0;
    }
    return 1;
}

int64_t eng_ttl(engine_t *e, const char *key, uint32_t klen) {
    eng_idx_t *n = idx_find(e, key, klen);
    if (!n || !n->used) return -2;
    if (!n->expire_ms) return -1;
    int64_t left = (int64_t)n->expire_ms - (int64_t)now_ms();
    return left > 0 ? left : -2;
}

/* ── scan / count ──────────────────────────────────────────────── */

void eng_scan(engine_t *e,
              int (*cb)(const char *key, uint32_t klen,
                        const char *val, uint32_t vlen, void *arg),
              void *arg) {
    uint64_t now = now_ms();
    for (int i = 0; i < ENGINE_BUCKETS; i++) {
        for (eng_idx_t *n = e->idx[i]; n; n = n->next) {
            if (!n->used) continue;
            if (n->expire_ms && now > n->expire_ms) { n->used = 0; continue; }
            uint32_t vlen;
            char *val = eng_get(e, n->key, n->key_len, &vlen);
            if (!val) continue;
            int stop = cb(n->key, n->key_len, val, vlen, arg);
            free(val);
            if (stop) return;
        }
    }
}

int eng_count(engine_t *e) {
    uint64_t now = now_ms();
    int c = 0;
    for (int i = 0; i < ENGINE_BUCKETS; i++)
        for (eng_idx_t *n = e->idx[i]; n; n = n->next) {
            if (!n->used) continue;
            if (n->expire_ms && now > n->expire_ms) { n->used = 0; continue; }
            c++;
        }
    return c;
}

/* ── compaction ────────────────────────────────────────────────── */

int eng_compact(engine_t *e) {
    char newpath[280];
    snprintf(newpath, sizeof(newpath), "%s/data.log.compact", e->path);
    int nfd = open(newpath, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (nfd < 0) return -1;

    uint64_t now = now_ms();
    uint64_t new_off = 0;

    for (int i = 0; i < ENGINE_BUCKETS; i++) {
        for (eng_idx_t *n = e->idx[i]; n; n = n->next) {
            if (!n->used) continue;
            if (n->expire_ms && now > n->expire_ms) { n->used = 0; continue; }

            /* read value from old log */
            char *val = malloc(n->val_len);
            if (!val) { close(nfd); unlink(newpath); return -1; }
            uint64_t val_off = n->offset + HDR_SIZE + n->key_len;
            if (pread(e->log_fd, val, n->val_len, (off_t)val_off)
                    != (ssize_t)n->val_len) {
                free(val); close(nfd); unlink(newpath); return -1;
            }

            ssize_t written = write_record(nfd, n->key, n->key_len,
                                           val, n->val_len, n->expire_ms);
            free(val);
            if (written < 0) { close(nfd); unlink(newpath); return -1; }
            n->offset = new_off;
            new_off  += written;
        }
    }

    fsync(nfd);

    /* atomic swap */
    char logpath[280];
    snprintf(logpath, sizeof(logpath), "%s/data.log", e->path);
    if (rename(newpath, logpath) < 0) { close(nfd); unlink(newpath); return -1; }

    close(e->log_fd);
    e->log_fd    = nfd;
    e->log_size  = new_off;
    e->dead_bytes = 0;
    return 0;
}
