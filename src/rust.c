/* rust.c - crate model lifecycle, string lists, diagnostics, and the
 * whitespace invariant shared with thorn's graph.c. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/rust.h"

void diag(const char *fmt, ...)
{
    va_list ap;
    fputs("thorn-rs: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int strlist_push(StrList *l, const char *s)
{
    if (!l || !s)
        return -1;
    if (l->count == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 8;
        char **ni = realloc(l->items, ncap * sizeof(*ni));
        if (!ni)
            return -1;
        l->items = ni;
        l->cap = ncap;
    }
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (!copy)
        return -1;
    memcpy(copy, s, len);
    l->items[l->count++] = copy;
    return 0;
}

void strlist_free(StrList *l)
{
    if (!l)
        return;
    for (size_t i = 0; i < l->count; i++)
        free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->count = l->cap = 0;
}

int has_whitespace(const char *s)
{
    if (!s)
        return 0;
    for (const char *p = s; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n' ||
            *p == '\v' || *p == '\f' || *p == '\r')
            return 1;
    }
    return 0;
}

int path_safe(const char *s)
{
    if (!s || !*s || has_whitespace(s))
        return 0;
    for (const char *p = s; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < 0x20 || ch == 0x7f)
            return 0;
        switch (ch) {
        case '$': case '%': case ':': case ';': case '&':
        case '|': case '<': case '>': case '(': case ')':
        case '#': case '"': case '\'': case '\\': case '`':
        case '*': case '?': case '[': case ']': case '!': case '~':
            return 0;
        default:
            break;
        }
    }
    return 1;
}

void rust_crate_init(RustCrate *c)
{
    memset(c, 0, sizeof(*c));
    c->kind = RUST_CRATE_STATICLIB;
    snprintf(c->edition, sizeof(c->edition), "2021");
    c->opt_level = RUST_OPT_UNSET;
    c->panic_abort = 0;
    c->debuginfo = -1;
}

void rust_crate_free(RustCrate *c)
{
    if (!c)
        return;
    strlist_free(&c->cfgs);
    strlist_free(&c->rustflags);
    strlist_free(&c->externs);
    strlist_free(&c->search_paths);
    strlist_free(&c->capi_syms);
}
