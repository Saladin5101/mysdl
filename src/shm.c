/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mysdl.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>
#include <errno.h>

/* Use CLOCK_REALTIME so expire_ms survives process restart */
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static unsigned int hash_key(const char *key) {
    unsigned int h = 5381;
    while (*key) h = h * 33 ^ (unsigned char)*key++;
    return h % MYSDL_BUCKETS;
}

static const char *db_path(void) {
    const char *e = getenv("MYSDL_DB");
    return e ? e : MYSDL_DB_PATH;
}

/* ── persistence ───────────────────────────────────────────────── */

static void load(mysdl_db_t *db) {
    FILE *f = fopen(db_path(), "r");
    if (!f) return;
    uint64_t now = now_ms();
    char line[MYSDL_KEY_MAX + MYSDL_VAL_MAX + 32];
    while (fgets(line, sizeof(line), f)) {
        char key[MYSDL_KEY_MAX], val[MYSDL_VAL_MAX];
        unsigned long long exp;
        if (sscanf(line, "%63s\t%255[^\t]\t%llu", key, val, &exp) != 3) continue;
        if (exp && now > (uint64_t)exp) continue;   /* already expired */
        unsigned int i = hash_key(key);
        for (unsigned int n = 0; n < MYSDL_BUCKETS; n++) {
            mysdl_entry_t *e = &db->buckets[(i + n) % MYSDL_BUCKETS];
            if (!e->used) {
                memcpy(e->key, key, MYSDL_KEY_MAX);
                memcpy(e->val, val, MYSDL_VAL_MAX);
                e->expire_ms = (uint64_t)exp;
                e->used = 1;
                break;
            }
        }
    }
    fclose(f);
}

static void save(mysdl_db_t *db) {
    const char *path = db_path();
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    /* ensure parent directory exists */
    char dir[256];
    strncpy(dir, path, sizeof(dir) - 1);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (mkdir(dir, 0755) < 0 && errno != EEXIST) return;
    }

    FILE *f = fopen(tmp, "w");
    if (!f) return;
    uint64_t now = now_ms();
    for (int i = 0; i < MYSDL_BUCKETS; i++) {
        mysdl_entry_t *e = &db->buckets[i];
        if (!e->used) continue;
        if (e->expire_ms && now > e->expire_ms) continue;
        fprintf(f, "%s\t%s\t%llu\n",
                e->key, e->val, (unsigned long long)e->expire_ms);
    }
    fclose(f);
    rename(tmp, path);
}

/* ── misc ──────────────────────────────────────────────────────── */

uint64_t mysdl_parse_ttl(const char *s) {
    char *end;
    uint64_t n = strtoull(s, &end, 10);
    if      (!strcmp(end, "ms")) return n;
    else if (!strcmp(end, "s"))  return n * 1000;
    else if (!strcmp(end, "m"))  return n * 60000;
    return 0;
}

/* ── open / close ──────────────────────────────────────────────── */

mysdl_db_t *mysdl_open(void) {
    int fd = shm_open(MYSDL_SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { perror("shm_open"); return NULL; }

    struct stat st;
    int is_new = (fstat(fd, &st) == 0 && st.st_size < (off_t)sizeof(mysdl_db_t));
    if (is_new) {
        if (ftruncate(fd, sizeof(mysdl_db_t)) < 0) {
            perror("ftruncate"); close(fd); return NULL;
        }
    }

    mysdl_db_t *db = mmap(NULL, sizeof(mysdl_db_t),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (db == MAP_FAILED) return NULL;

    if (is_new) {
        memset(db, 0, sizeof(mysdl_db_t));
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&db->lock, &attr);
        pthread_mutexattr_destroy(&attr);
        load(db);
    }
    return db;
}

void mysdl_close(mysdl_db_t *db) {
    pthread_mutex_lock(&db->lock);
    save(db);
    pthread_mutex_unlock(&db->lock);
    munmap(db, sizeof(mysdl_db_t));
}

/* ── write ─────────────────────────────────────────────────────── */

int mysdl_set(mysdl_db_t *db, const char *key, const char *val, uint64_t ttl_ms) {
    pthread_mutex_lock(&db->lock);
    unsigned int i = hash_key(key);
    int ret = -1;
    for (unsigned int n = 0; n < MYSDL_BUCKETS; n++) {
        mysdl_entry_t *e = &db->buckets[(i + n) % MYSDL_BUCKETS];
        if (!e->used || !strcmp(e->key, key)) {
            strncpy(e->key, key, MYSDL_KEY_MAX - 1);
            strncpy(e->val, val, MYSDL_VAL_MAX - 1);
            e->expire_ms = ttl_ms ? now_ms() + ttl_ms : 0;
            e->used = 1;
            ret = 0;
            break;
        }
    }
    pthread_mutex_unlock(&db->lock);
    return ret;
}

/* ── read ──────────────────────────────────────────────────────── */

const char *mysdl_get(mysdl_db_t *db, const char *key) {
    pthread_mutex_lock(&db->lock);
    const char *ret = NULL;
    unsigned int i = hash_key(key);
    for (unsigned int n = 0; n < MYSDL_BUCKETS; n++) {
        mysdl_entry_t *e = &db->buckets[(i + n) % MYSDL_BUCKETS];
        if (!e->used) break;
        if (!strcmp(e->key, key)) {
            if (e->expire_ms && now_ms() > e->expire_ms)
                e->used = 0;
            else
                ret = e->val;
            break;
        }
    }
    pthread_mutex_unlock(&db->lock);
    return ret;
}

int mysdl_exists(mysdl_db_t *db, const char *key) {
    pthread_mutex_lock(&db->lock);
    int found = 0;
    unsigned int i = hash_key(key);
    for (unsigned int n = 0; n < MYSDL_BUCKETS; n++) {
        mysdl_entry_t *e = &db->buckets[(i + n) % MYSDL_BUCKETS];
        if (!e->used) break;
        if (!strcmp(e->key, key)) {
            found = !(e->expire_ms && now_ms() > e->expire_ms);
            if (!found) e->used = 0;
            break;
        }
    }
    pthread_mutex_unlock(&db->lock);
    return found;
}

int64_t mysdl_ttl(mysdl_db_t *db, const char *key) {
    pthread_mutex_lock(&db->lock);
    int64_t ret = -2;
    unsigned int i = hash_key(key);
    for (unsigned int n = 0; n < MYSDL_BUCKETS; n++) {
        mysdl_entry_t *e = &db->buckets[(i + n) % MYSDL_BUCKETS];
        if (!e->used) break;
        if (!strcmp(e->key, key)) {
            if (e->expire_ms) {
                int64_t left = (int64_t)e->expire_ms - (int64_t)now_ms();
                ret = left > 0 ? left : -2;
            } else {
                ret = -1;
            }
            break;
        }
    }
    pthread_mutex_unlock(&db->lock);
    return ret;
}

int mysdl_count(mysdl_db_t *db) {
    pthread_mutex_lock(&db->lock);
    uint64_t now = now_ms();
    int c = 0;
    for (int i = 0; i < MYSDL_BUCKETS; i++) {
        mysdl_entry_t *e = &db->buckets[i];
        if (!e->used) continue;
        if (e->expire_ms && now > e->expire_ms) { e->used = 0; continue; }
        c++;
    }
    pthread_mutex_unlock(&db->lock);
    return c;
}

void mysdl_scan(mysdl_db_t *db,
                int (*cb)(const char *key, const char *val, void *arg),
                void *arg) {
    pthread_mutex_lock(&db->lock);
    uint64_t now = now_ms();
    for (int i = 0; i < MYSDL_BUCKETS; i++) {
        mysdl_entry_t *e = &db->buckets[i];
        if (!e->used) continue;
        if (e->expire_ms && now > e->expire_ms) { e->used = 0; continue; }
        if (cb(e->key, e->val, arg)) break;
    }
    pthread_mutex_unlock(&db->lock);
}

/* ── delete ────────────────────────────────────────────────────── */

int mysdl_del(mysdl_db_t *db, const char *key) {
    pthread_mutex_lock(&db->lock);
    int ret = -1;
    unsigned int i = hash_key(key);
    for (unsigned int n = 0; n < MYSDL_BUCKETS; n++) {
        unsigned int idx = (i + n) % MYSDL_BUCKETS;
        mysdl_entry_t *e = &db->buckets[idx];
        if (!e->used) break;
        if (!strcmp(e->key, key)) {
            e->used = 0;
            for (unsigned int m = 1; m < MYSDL_BUCKETS; m++) {
                unsigned int cur  = (idx + m)     % MYSDL_BUCKETS;
                unsigned int prev = (idx + m - 1) % MYSDL_BUCKETS;
                if (!db->buckets[cur].used) break;
                if (hash_key(db->buckets[cur].key) == (idx + m) % MYSDL_BUCKETS) break;
                db->buckets[prev] = db->buckets[cur];
                db->buckets[cur].used = 0;
            }
            ret = 0;
            break;
        }
    }
    pthread_mutex_unlock(&db->lock);
    return ret;
}

void mysdl_flush(mysdl_db_t *db) {
    pthread_mutex_lock(&db->lock);
    memset(db->buckets, 0, sizeof(db->buckets));
    save(db);
    pthread_mutex_unlock(&db->lock);
}

int mysdl_rename(mysdl_db_t *db, const char *old, const char *new_key) {
    pthread_mutex_lock(&db->lock);
    int ret = -1;
    unsigned int i = hash_key(old);
    for (unsigned int n = 0; n < MYSDL_BUCKETS; n++) {
        mysdl_entry_t *e = &db->buckets[(i + n) % MYSDL_BUCKETS];
        if (!e->used) break;
        if (!strcmp(e->key, old)) {
            if (e->expire_ms && now_ms() > e->expire_ms) { e->used = 0; break; }
            char     tmp_val[MYSDL_VAL_MAX];
            uint64_t tmp_exp = e->expire_ms;
            memcpy(tmp_val, e->val, MYSDL_VAL_MAX);
            e->used = 0;
            unsigned int idx = (i + n) % MYSDL_BUCKETS;
            for (unsigned int m = 1; m < MYSDL_BUCKETS; m++) {
                unsigned int cur  = (idx + m)     % MYSDL_BUCKETS;
                unsigned int prev = (idx + m - 1) % MYSDL_BUCKETS;
                if (!db->buckets[cur].used) break;
                if (hash_key(db->buckets[cur].key) == (idx + m) % MYSDL_BUCKETS) break;
                db->buckets[prev] = db->buckets[cur];
                db->buckets[cur].used = 0;
            }
            unsigned int j = hash_key(new_key);
            for (unsigned int m = 0; m < MYSDL_BUCKETS; m++) {
                mysdl_entry_t *ne = &db->buckets[(j + m) % MYSDL_BUCKETS];
                if (!ne->used) {
                    strncpy(ne->key, new_key, MYSDL_KEY_MAX - 1);
                    memcpy(ne->val, tmp_val, MYSDL_VAL_MAX);
                    ne->expire_ms = tmp_exp;
                    ne->used = 1;
                    ret = 0;
                    break;
                }
            }
            break;
        }
    }
    pthread_mutex_unlock(&db->lock);
    return ret;
}
