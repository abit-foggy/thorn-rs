/*
 * rust.h - the RustCrate model and shared helpers.
 *
 * Part of thorn-rs, the zero-cargo Rust staticlib companion for thorn.
 * Copyright (c) 2026 abit-foggy
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef THORN_RS_RUST_H
#define THORN_RS_RUST_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>

#define THORN_RS_VERSION "0.2.0"

/* Exit codes. */
enum {
    THORN_RS_EXIT_OK    = 0,
    THORN_RS_EXIT_SCAN  = 1,  /* scanner / validation / planning */
    THORN_RS_EXIT_IO    = 2,  /* file I/O                        */
    THORN_RS_EXIT_USAGE = 64  /* command line                    */
};

/* Crate kinds; Phase 0 plans staticlib only. */
enum { RUST_CRATE_STATICLIB = 1 };

/* opt_level: RUST_OPT_UNSET, or '0'..'3', 's', 'z'. */
#define RUST_OPT_UNSET (-1)

/* Growable string list, shaped after thorn's engine.h StrList. */
typedef struct StrList {
    char **items;
    size_t count;
    size_t cap;
} StrList;

int  strlist_push(StrList *l, const char *s);
void strlist_free(StrList *l);

/* One planned Rust crate. */
typedef struct RustCrate {
    char name[128];         /* --crate-name; artifact is lib<name>.a */
    char root[1024];        /* crate root source file                */
    char out[1024];         /* archive path                          */
    char frag[1024];        /* fragment destination                  */
    int  kind;
    char edition[16];       /* "2015"|"2018"|"2021"|"2024"           */
    char target[128];       /* --target triple; "" = host            */
    int  opt_level;
    int  panic_abort;       /* -1 unspecified; 1 => -C panic=abort   */
    int  debuginfo;         /* 0..2, -1 omit                         */
    int  no_std;            /* scanner: #![no_std] seen              */
    int  has_panic_handler; /* scanner: #[panic_handler] seen        */
    char rustc[1024];       /* tool name to emit; never an abs path  */
    char header[1024];      /* C-ABI contract header for verify edge */
    StrList cfgs;           /* --cfg specs                           */
    StrList rustflags;      /* extra -C/... passes                   */
    StrList externs;        /* --extern name=path                    */
    StrList search_paths;   /* -L dirs                               */
    StrList capi_syms;      /* reserved for Phase 2                  */
} RustCrate;

void rust_crate_init(RustCrate *c);
void rust_crate_free(RustCrate *c);

void diag(const char *fmt, ...);

/* thorn graph.c invariant: depfiles cannot carry whitespace paths. */
int has_whitespace(const char *s);

/* Rejects characters syntactically active in Ninja/Make fragments. */
int path_safe(const char *s);

#endif /* THORN_RS_RUST_H */
