/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "parser.h"
#include "mysdl.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ── tokeniser ─────────────────────────────────────────────────── */

static const char *skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static const char *read_word(const char *p, char *buf, size_t sz) {
    size_t i = 0;
    while (*p && !isspace((unsigned char)*p) &&
           *p != '=' && *p != '!' && *p != '(' && *p != ')' && *p != '&') {
        if (i + 1 < sz) buf[i++] = *p;
        p++;
    }
    buf[i] = '\0';
    return p;
}

static const char *read_quoted(const char *p, char *buf, size_t sz) {
    if (*p != '\'') return NULL;
    p++;
    size_t i = 0;
    while (*p && *p != '\'') {
        if (i + 1 < sz) buf[i++] = *p;
        p++;
    }
    if (*p != '\'') return NULL;
    buf[i] = '\0';
    return p + 1;
}

static const char *read_value(const char *p, char *buf, size_t sz) {
    p = skip_ws(p);
    return (*p == '\'') ? read_quoted(p, buf, sz) : read_word(p, buf, sz);
}

/* ── condition parser ──────────────────────────────────────────── */
/*  ( key == 'val' [&& key2 != 'val2' ...] )  */

static const char *parse_conds(const char *p, sdl_stmt_t *out) {
    if (*p != '(') { fputs("parse error: expected '('\n", stderr); return NULL; }
    p++; p = skip_ws(p);

    while (out->nconds < SDL_COND_MAX) {
        sdl_cond_t *c = &out->conds[out->nconds];

        p = read_word(p, c->cond_key, sizeof(c->cond_key));
        if (!c->cond_key[0]) { fputs("parse error: empty condition key\n", stderr); return NULL; }
        p = skip_ws(p);

        if (p[0] == '!' && p[1] == '=')      { c->neq = 1; p += 2; }
        else if (p[0] == '=' && p[1] == '=') { c->neq = 0; p += 2; }
        else { fputs("parse error: expected == or !=\n", stderr); return NULL; }

        p = skip_ws(p);
        p = read_quoted(p, c->cond_val, sizeof(c->cond_val));
        if (!p) { fputs("parse error: expected quoted condition value\n", stderr); return NULL; }
        out->nconds++;

        p = skip_ws(p);
        if (*p == ')') { p++; break; }
        /* expect && */
        if (p[0] == '&' && p[1] == '&') { p += 2; p = skip_ws(p); continue; }
        fputs("parse error: expected '&&' or ')'\n", stderr);
        return NULL;
    }
    return p;
}

/* ── grammar ───────────────────────────────────────────────────── */

int sdl_parse(const char *src, sdl_stmt_t *out) {
    memset(out, 0, sizeof(*out));
    out->op = SDL_ERR;

    const char *p = skip_ws(src);
    char kw[32];
    p = read_word(p, kw, sizeof(kw));

    if (!strcmp(kw, "flush"))   { out->op = SDL_FLUSH;   return 0; }
    if (!strcmp(kw, "count"))   { out->op = SDL_COUNT;   return 0; }
    if (!strcmp(kw, "compact")) { out->op = SDL_COMPACT; return 0; }

    if (!strcmp(kw, "read")) {
        p = skip_ws(p);
        p = read_word(p, out->key, sizeof(out->key));
        if (!out->key[0]) { fputs("parse error: read needs a key or *\n", stderr); return -1; }
        out->op = SDL_READ;
        return 0;
    }

    if (!strcmp(kw, "erase")) {
        p = skip_ws(p);
        p = read_word(p, out->key, sizeof(out->key));
        if (!out->key[0]) { fputs("parse error: erase needs a key\n", stderr); return -1; }
        out->op = SDL_ERASE;
        return 0;
    }

    if (!strcmp(kw, "exists")) {
        p = skip_ws(p);
        p = read_word(p, out->key, sizeof(out->key));
        if (!out->key[0]) { fputs("parse error: exists needs a key\n", stderr); return -1; }
        out->op = SDL_EXISTS;
        return 0;
    }

    if (!strcmp(kw, "ttl")) {
        p = skip_ws(p);
        p = read_word(p, out->key, sizeof(out->key));
        if (!out->key[0]) { fputs("parse error: ttl needs a key\n", stderr); return -1; }
        out->op = SDL_TTL;
        return 0;
    }

    if (!strcmp(kw, "rename")) {
        p = skip_ws(p);
        p = read_word(p, out->key, sizeof(out->key));
        p = skip_ws(p);
        p = read_word(p, out->key2, sizeof(out->key2));
        if (!out->key[0] || !out->key2[0]) {
            fputs("parse error: rename needs two keys\n", stderr); return -1;
        }
        out->op = SDL_RENAME;
        return 0;
    }

    if (!strcmp(kw, "write")) {
        p = skip_ws(p);

        /* optional: if ( <cond> [&& <cond> ...] ) [then] */
        if (!strncmp(p, "if", 2) && isspace((unsigned char)p[2])) {
            p += 2; p = skip_ws(p);
            p = parse_conds(p, out);
            if (!p) return -1;
            p = skip_ws(p);
            if (!strncmp(p, "then", 4) && isspace((unsigned char)p[4]))
                { p += 4; p = skip_ws(p); }
        }

        /* <key> = '<val>' */
        p = read_word(p, out->key, sizeof(out->key));
        if (!out->key[0]) { fputs("parse error: write needs a key\n", stderr); return -1; }
        p = skip_ws(p);
        if (*p != '=') { fputs("parse error: expected '=' after key\n", stderr); return -1; }
        p++; p = skip_ws(p);
        p = read_value(p, out->val, sizeof(out->val));
        if (!p || !out->val[0]) { fputs("parse error: write needs a value\n", stderr); return -1; }

        /* optional: ttl = <ttl> */
        p = skip_ws(p);
        if (!strncmp(p, "ttl", 3) && isspace((unsigned char)p[3])) {
            p += 3; p = skip_ws(p);
            if (*p != '=') { fputs("parse error: expected '=' after ttl\n", stderr); return -1; }
            p++; p = skip_ws(p);
            char ttl_str[32];
            p = read_word(p, ttl_str, sizeof(ttl_str));
            out->ttl_ms = mysdl_parse_ttl(ttl_str);
        }

        out->op = SDL_WRITE;
        return 0;
    }

    fprintf(stderr, "parse error: unknown keyword '%s'\n", kw);
    return -1;
}
