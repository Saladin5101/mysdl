/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mysdl.h"
#include "parser.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int print_entry(const char *key, const char *val, void *arg) {
    (void)arg;
    printf("%s = '%s'\n", key, val);
    return 0;
}

static int eval_conds(mysdl_db_t *db, const sdl_stmt_t *s) {
    for (int i = 0; i < s->nconds; i++) {
        const sdl_cond_t *c = &s->conds[i];
        char *cur = mysdl_get(db, c->cond_key);
        int eq = cur && !strcmp(cur, c->cond_val);
        free(cur);
        if (c->neq ? eq : !eq) return 0;
    }
    return 1;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fputs("usage: mysdl '<sdl statement>'\n", stderr);
        fputs("  write [if (k=='v' [&& k2!='v2'])] key = 'val' [ttl = 300ms]\n", stderr);
        fputs("  read <key> | read *\n", stderr);
        fputs("  erase <key>\n", stderr);
        fputs("  exists <key>\n", stderr);
        fputs("  rename <old> <new>\n", stderr);
        fputs("  ttl <key>\n", stderr);
        fputs("  count\n", stderr);
        fputs("  flush\n", stderr);
        fputs("  compact\n", stderr);
        return 1;
    }

    char src[512] = {0};
    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(src, " ", sizeof(src) - strlen(src) - 1);
        strncat(src, argv[i], sizeof(src) - strlen(src) - 1);
    }

    sdl_stmt_t stmt;
    if (sdl_parse(src, &stmt) < 0) return 1;

    mysdl_db_t *db = mysdl_open();
    if (!db) { fputs("error: cannot open database\n", stderr); return 1; }

    int ret = 0;
    switch (stmt.op) {
    case SDL_WRITE:
        if (stmt.nconds && !eval_conds(db, &stmt)) {
            fputs("(condition not met, skipped)\n", stdout);
            break;
        }
        ret = mysdl_set(db, stmt.key, stmt.val, stmt.ttl_ms);
        if (ret) fputs("error: write failed\n", stderr);
        break;

    case SDL_READ:
        if (!strcmp(stmt.key, "*")) {
            mysdl_scan(db, print_entry, NULL);
        } else {
            char *val = mysdl_get(db, stmt.key);
            if (val) { puts(val); free(val); }
            else { fputs("(nil)\n", stdout); ret = 1; }
        }
        break;

    case SDL_ERASE:
        ret = mysdl_del(db, stmt.key);
        if (ret) fputs("(not found)\n", stdout);
        break;

    case SDL_EXISTS:
        puts(mysdl_exists(db, stmt.key) ? "1" : "0");
        break;

    case SDL_RENAME:
        ret = mysdl_rename(db, stmt.key, stmt.key2);
        if (ret) fputs("(not found)\n", stdout);
        break;

    case SDL_TTL: {
        int64_t ms = mysdl_ttl(db, stmt.key);
        if      (ms == -2) fputs("(nil)\n", stdout);
        else if (ms == -1) fputs("(no ttl)\n", stdout);
        else               printf("%lldms\n", (long long)ms);
        break;
    }

    case SDL_COUNT:
        printf("%d\n", mysdl_count(db));
        break;

    case SDL_FLUSH:
        mysdl_flush(db);
        break;

    case SDL_COMPACT:
        ret = mysdl_compact(db);
        if (ret) fputs("error: compaction failed\n", stderr);
        break;

    default:
        ret = 1;
    }

    mysdl_close(db);
    return ret;
}
