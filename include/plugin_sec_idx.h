/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * secondary index plugin
 *
 * Maintains a prefix-based secondary index inside the same engine:
 *   idx:<field>:<value>  →  <primary_key>
 *
 * The plugin watches for keys of the form  <ns>:<id>  and extracts
 * a field from the value using a user-supplied extractor function.
 *
 * Usage:
 *   sec_idx_cfg_t cfg = {
 *       .field    = "email",
 *       .extract  = my_extract_fn,   // pulls field value from record val
 *   };
 *   mysdl_plugin_t *p = sec_idx_plugin_new(&cfg);
 *   eng_plugin_register(eng, p);
 */
#pragma once
#include "plugin.h"
#include "engine.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    const char *field;
    /* Extract the indexed field value from a record value.
     * Write result into out (max out_sz bytes). Return 0 on success. */
    int (*extract)(const char *val, uint32_t vlen,
                   char *out, size_t out_sz);
} sec_idx_cfg_t;

static int _sidx_put(struct engine *e, const char *key, uint32_t klen,
                     const char *val, uint32_t vlen,
                     uint64_t expire_ms, void *udata) {
    (void)expire_ms;
    sec_idx_cfg_t *cfg = udata;
    char field_val[256];
    if (cfg->extract(val, vlen, field_val, sizeof(field_val)) != 0) return 0;

    char idx_key[512];
    snprintf(idx_key, sizeof(idx_key), "idx:%s:%s", cfg->field, field_val);
    return eng_put(e, idx_key, (uint32_t)strlen(idx_key), key, klen, 0);
}

static int _sidx_del(struct engine *e, const char *key, uint32_t klen,
                     void *udata) {
    /* To remove the index entry we need the old value — read it first */
    sec_idx_cfg_t *cfg = udata;
    uint32_t vlen;
    char *val = eng_get(e, key, klen, &vlen);
    if (!val) return 0;

    char field_val[256];
    int ok = cfg->extract(val, vlen, field_val, sizeof(field_val));
    free(val);
    if (ok != 0) return 0;

    char idx_key[512];
    snprintf(idx_key, sizeof(idx_key), "idx:%s:%s", cfg->field, field_val);
    eng_del(e, idx_key, (uint32_t)strlen(idx_key));
    return 0;
}

static mysdl_plugin_t _sidx_tmpl = {
    .name   = "sec_idx",
    .on_put = _sidx_put,
    .on_del = _sidx_del,
    .on_get = NULL,
};

static inline mysdl_plugin_t *sec_idx_plugin_new(sec_idx_cfg_t *cfg) {
    mysdl_plugin_t *p = malloc(sizeof(*p));
    if (!p) return NULL;
    *p = _sidx_tmpl;
    p->udata = cfg;
    return p;
}

static inline void sec_idx_plugin_free(mysdl_plugin_t *p) { free(p); }
