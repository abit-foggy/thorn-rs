#ifndef THORN_RS_RUST_H
#define THORN_RS_RUST_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>

#define THORN_RS_VERSION "0.1.0"

/* Exit codes (README "Exit codes"; distinct classes per the Phase 0 task) */
enum {
    THORN_RS_EXIT_OK    = 0,  /* success                              */
    THORN_RS_EXIT_SCAN  = 1,  /* scanner / validation / planning      */
    THORN_RS_EXIT_IO    = 2,  /* file I/O                             */
    THORN_RS_EXIT_USAGE = 64  /* command-line misuse                  */
};

/* Crate kinds (docs/SCOPING.md section 3.1; Phase 0: staticlib only) */
enum { RUST_CRATE_STATICLIB = 1 };

/* opt_level encoding (docs/SCOPING.md section 3.1):
   RUST_OPT_UNSET, or '0'..'3', 's', 'z' as character values. */
#define RUST_OPT_UNSET (-1)

/* Growable string list - shape mirrors thorn's engine.h StrList. */
typedef struct StrList {
    char **items;
    size_t count;
    size_t cap;
} StrList;

int  strlist_push(StrList *l, const char *s);
void strlist_free(StrList *l);

/* The crate model (docs/SCOPING.md section 3.1, Phase 0 subset). */
typedef struct RustCrate {
    char name[128];        /* --crate-name; artifact is lib<name>.a     */
    char root[1024];       /* crate root source file (lib.rs)           */
    char out[1024];        /* canonical archive path (see section 3.3)  */
    char frag[1024];       /* fragment destination (--out)              */
    int  kind;             /* RUST_CRATE_STATICLIB                      */
    char edition[16];      /* "2015"|"2018"|"2021"|"2024"               */
    char target[128];      /* --target triple; "" = host                */
    int  opt_level;        /* RUST_OPT_UNSET or '0'..'3'/'s'/'z'        */
    int  panic_abort;      /* 1 => -C panic=abort                       */
    int  debuginfo;        /* 0..2, -1 = omit                           */
    int  no_std;           /* scanner: #![no_std] seen                  */
    int  has_panic_handler;/* scanner: #[panic_handler] seen            */
    char rustc[1024];      /* tool name to emit (never an injected path)*/
    StrList cfgs;          /* --cfg items, single-quote wrapped on emit */
    StrList rustflags;     /* extra raw -C/... passes (validated)       */
    StrList externs;       /* --extern name=path                        */
    StrList search_paths;  /* -L dir                                    */
    StrList capi_syms;     /* expected extern "C" exports (Phase 2)     */
} RustCrate;

void rust_crate_init(RustCrate *c);
void rust_crate_free(RustCrate *c);

/* Diagnostics: every failure goes to stderr as "thorn-rs: ..." */
void diag(const char *fmt, ...);

/* thorn graph.c invariant: whitespace paths are unrepresentable in
   Make-style depfiles and are rejected at plan time. */
int has_whitespace(const char *s);

/* Fragment-path safety: rejects empty strings, control characters,
   whitespace, and every character that is syntactically active in
   Ninja or Make fragments ($ % : ; & | < > ( ) # " ' \ ` * ? [ ] ! ~).
   Emitted paths are plain project-relative names. */
int path_safe(const char *s);

#endif /* THORN_RS_RUST_H */
