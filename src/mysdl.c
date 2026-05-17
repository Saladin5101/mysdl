/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mysdl.h"
#include <string.h>
#include <stdlib.h>

uint64_t mysdl_parse_ttl(const char *s) {
    char *end;
    uint64_t n = strtoull(s, &end, 10);
    if      (!strcmp(end, "ms")) return n;
    else if (!strcmp(end, "s"))  return n * 1000;
    else if (!strcmp(end, "m"))  return n * 60000;
    return 0;
}

void mysdl_flush(mysdl_db_t *db) {
    /* collect all keys then delete each one */
    typedef struct knode { char *k; struct knode *next; } knode_t;
    knode_t *head = NULL;

    for (int i = 0; i < ENGINE_BUCKETS; i++) {
        for (eng_idx_t *n = db->idx[i]; n; n = n->next) {
            if (!n->used) continue;
            knode_t *kn = malloc(sizeof(*kn));
            if (!kn) continue;
            kn->k = malloc(n->key_len + 1);
            if (!kn->k) { free(kn); continue; }
            memcpy(kn->k, n->key, n->key_len);
            kn->k[n->key_len] = '\0';
            kn->next = head;
            head = kn;
        }
    }
    for (knode_t *kn = head; kn; ) {
        eng_del(db, kn->k, (uint32_t)strlen(kn->k));
        knode_t *nx = kn->next;
        free(kn->k); free(kn);
        kn = nx;
    }
    eng_compact(db);
}

int mysdl_rename(mysdl_db_t *db, const char *old_key, const char *new_key) {
    uint32_t klen = (uint32_t)strlen(old_key);
    uint32_t vlen;
    char *val = eng_get(db, old_key, klen, &vlen);
    if (!val) return -1;
    int64_t ttl_left = eng_ttl(db, old_key, klen);
    uint64_t ttl_ms = (ttl_left > 0) ? (uint64_t)ttl_left : 0;
    int ret = eng_put(db, new_key, (uint32_t)strlen(new_key), val, vlen, ttl_ms);
    if (ret == 0) eng_del(db, old_key, klen);
    free(val);
    return ret;
}
