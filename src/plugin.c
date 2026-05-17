/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "plugin.h"
#include "engine.h"
#include <string.h>

int plugin_register(plugin_registry_t *r, mysdl_plugin_t *p) {
    if (r->count >= MYSDL_PLUGIN_MAX) return -1;
    r->plugins[r->count++] = p;
    return 0;
}

int plugin_unregister(plugin_registry_t *r, const char *name) {
    for (int i = 0; i < r->count; i++) {
        if (!strcmp(r->plugins[i]->name, name)) {
            r->plugins[i] = r->plugins[--r->count];
            r->plugins[r->count] = NULL;
            return 0;
        }
    }
    return -1;
}

int plugin_fire_put(plugin_registry_t *r, struct engine *e,
                    const char *key, uint32_t klen,
                    const char *val, uint32_t vlen, uint64_t expire_ms) {
    for (int i = 0; i < r->count; i++) {
        mysdl_plugin_t *p = r->plugins[i];
        if (p->on_put && p->on_put(e, key, klen, val, vlen, expire_ms, p->udata) < 0)
            return -1;
    }
    return 0;
}

void plugin_fire_del(plugin_registry_t *r, struct engine *e,
                     const char *key, uint32_t klen) {
    for (int i = 0; i < r->count; i++) {
        mysdl_plugin_t *p = r->plugins[i];
        if (p->on_del) p->on_del(e, key, klen, p->udata);
    }
}

void plugin_fire_get(plugin_registry_t *r, struct engine *e,
                     const char *key, uint32_t klen,
                     const char *val, uint32_t vlen) {
    for (int i = 0; i < r->count; i++) {
        mysdl_plugin_t *p = r->plugins[i];
        if (p->on_get) p->on_get(e, key, klen, val, vlen, p->udata);
    }
}
