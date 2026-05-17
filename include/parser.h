/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>

typedef enum {
    SDL_WRITE,   /* write [if (...)] key = 'val' [ttl = <ttl>] */
    SDL_READ,    /* read <key>  |  read * */
    SDL_ERASE,   /* erase <key> */
    SDL_FLUSH,   /* flush */
    SDL_EXISTS,  /* exists <key> */
    SDL_RENAME,  /* rename <old> <new> */
    SDL_COUNT,   /* count */
    SDL_TTL,     /* ttl <key> */
    SDL_COMPACT, /* compact */
    SDL_ERR,
} sdl_op_t;

#define SDL_COND_MAX 8

typedef struct {
    char cond_key[64];
    char cond_val[256];
    int  neq;        /* 0 = ==, 1 = != */
} sdl_cond_t;

typedef struct {
    sdl_op_t  op;
    char      key[64];
    char      key2[64];   /* rename: new name */
    char      val[256];
    uint64_t  ttl_ms;
    int       nconds;
    sdl_cond_t conds[SDL_COND_MAX];
} sdl_stmt_t;

/* Parse one SDL statement. Returns 0 on success, -1 on syntax error. */
int sdl_parse(const char *src, sdl_stmt_t *out);
