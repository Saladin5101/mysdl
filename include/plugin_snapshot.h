/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * snapshot plugin
 *
 * Mirrors every put/del into a separate snapshot directory.
 * The snapshot is a normal engine opened with ENG_RDONLY by readers —
 * no extra file format needed.
 *
 * Usage:
 *   engine_t *snap_eng = eng_open("/var/lib/mysdl/snap", ENG_CREAT);
 *   mysdl_plugin_t *p  = snapshot_plugin_new(snap_eng);
 *   eng_plugin_register(main_eng, p);
 *   ...
 *   snapshot_plugin_free(p);
 */
#pragma once
#include "plugin.h"
#include "engine.h"
#include <time.h>
#include <stdlib.h>

static int _snap_put(struct engine *e, const char *key, uint32_t klen,
                     const char *val, uint32_t vlen,
                     uint64_t expire_ms, void *udata) {
    (void)e;
    engine_t *snap = udata;
    /* store with absolute expire_ms directly — pass ttl=0 and fix up */
    uint64_t ttl = 0;
    if (expire_ms) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        ttl = expire_ms > now ? expire_ms - now : 1;
    }
    return eng_put(snap, key, klen, val, vlen, ttl);
}

static int _snap_del(struct engine *e, const char *key, uint32_t klen,
                     void *udata) {
    (void)e;
    return eng_del(udata, key, klen);
}

static mysdl_plugin_t _snap_plugin_tmpl = {
    .name   = "snapshot",
    .on_put = _snap_put,
    .on_del = _snap_del,
    .on_get = NULL,
};

static inline mysdl_plugin_t *snapshot_plugin_new(engine_t *snap_engine) {
    mysdl_plugin_t *p = malloc(sizeof(*p));
    if (!p) return NULL;
    *p = _snap_plugin_tmpl;
    p->udata = snap_engine;
    return p;
}

static inline void snapshot_plugin_free(mysdl_plugin_t *p) { free(p); }
