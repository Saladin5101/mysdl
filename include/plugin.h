/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>

/*
 * MySDL plugin system.
 *
 * A plugin registers a set of hooks that the engine calls after each
 * operation.  All hooks are optional — set unused ones to NULL.
 *
 * Hook return values:
 *   0  = ok, continue calling remaining plugins
 *  -1  = signal error back to the caller (put/del will return -1)
 */

struct engine;   /* forward declaration */

typedef struct mysdl_plugin {
    const char *name;

    /* Called after a successful eng_put().
     * expire_ms is the absolute expiry (0 = no TTL). */
    int (*on_put)(struct engine *e, const char *key, uint32_t klen,
                  const char *val, uint32_t vlen,
                  uint64_t expire_ms, void *udata);

    /* Called after a successful eng_del(). */
    int (*on_del)(struct engine *e, const char *key, uint32_t klen,
                  void *udata);

    /* Called after a successful eng_get().
     * val/vlen are the returned data (read-only). */
    void (*on_get)(struct engine *e, const char *key, uint32_t klen,
                   const char *val, uint32_t vlen, void *udata);

    void *udata;   /* passed back to every hook */
} mysdl_plugin_t;

#define MYSDL_PLUGIN_MAX 8

typedef struct plugin_registry {
    mysdl_plugin_t *plugins[MYSDL_PLUGIN_MAX];
    int             count;
} plugin_registry_t;

/* Register / unregister a plugin. Returns 0 on success, -1 on error. */
int  plugin_register  (plugin_registry_t *r, mysdl_plugin_t *p);
int  plugin_unregister(plugin_registry_t *r, const char *name);

/* Called by the engine — do not call directly. */
int  plugin_fire_put(plugin_registry_t *r, struct engine *e,
                     const char *key, uint32_t klen,
                     const char *val, uint32_t vlen, uint64_t expire_ms);
void plugin_fire_del(plugin_registry_t *r, struct engine *e,
                     const char *key, uint32_t klen);
void plugin_fire_get(plugin_registry_t *r, struct engine *e,
                     const char *key, uint32_t klen,
                     const char *val, uint32_t vlen);
