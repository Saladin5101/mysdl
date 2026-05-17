/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine.h"
#include "plugin_snapshot.h"
#include "plugin_sec_idx.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* extract: value IS the field (trivial for this test) */
static int extract_val(const char *val, uint32_t vlen,
                       char *out, size_t out_sz) {
    if (vlen >= out_sz) return -1;
    memcpy(out, val, vlen);
    out[vlen] = '\0';
    return 0;
}

int main(void) {
    setenv("MYSDL_DB", "/tmp/mysdl_plugin_test", 1);
    if (system("rm -rf /tmp/mysdl_plugin_test /tmp/mysdl_snap_test")) {}

    engine_t *eng  = eng_open("/tmp/mysdl_plugin_test", ENG_CREAT);
    engine_t *snap = eng_open("/tmp/mysdl_snap_test",   ENG_CREAT);

    /* register snapshot plugin */
    mysdl_plugin_t *sp = snapshot_plugin_new(snap);
    eng_plugin_register(eng, sp);

    /* register secondary index plugin */
    static sec_idx_cfg_t cfg = { .field = "role", .extract = extract_val };
    mysdl_plugin_t *ip = sec_idx_plugin_new(&cfg);
    eng_plugin_register(eng, ip);

    /* write some records */
    eng_put(eng, "user:1", 6, "admin", 5, 0);
    eng_put(eng, "user:2", 6, "guest", 5, 0);
    eng_put(eng, "user:3", 6, "admin", 5, 0);

    /* verify secondary index */
    printf("=== secondary index (role=admin) ===\n");
    char *v = eng_get(eng, "idx:role:admin", 14, NULL);
    printf("idx:role:admin → %s\n", v ? v : "(nil)");
    free(v);

    /* verify snapshot has the data */
    printf("=== snapshot ===\n");
    char *sv = eng_get(snap, "user:1", 6, NULL);
    printf("snap user:1 → %s\n", sv ? sv : "(nil)");
    free(sv);

    /* delete and check index cleanup */
    eng_del(eng, "user:1", 6);
    printf("=== after delete user:1 ===\n");
    v = eng_get(eng, "idx:role:admin", 14, NULL);
    /* index now points to user:3 (last writer wins) */
    printf("idx:role:admin → %s\n", v ? v : "(nil)");
    free(v);

    eng_close(snap);
    eng_close(eng);
    snapshot_plugin_free(sp);
    sec_idx_plugin_free(ip);
    printf("=== done ===\n");
    return 0;
}
