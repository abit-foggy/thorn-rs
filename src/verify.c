/*
 * verify.c - nm-based C-ABI audit and header synchronization.
 *
 * Defined symbols come from `nm -g --defined-only`; one leading
 * underscore is stripped so Mach-O archives match their C names.
 * Header expectations come from a bounded declaration scan (comments,
 * strings, and preprocessor lines blanked; keywords, typedefs, and
 * function bodies skipped by shape).
 *
 * Part of thorn-rs, the zero-cargo Rust staticlib companion for thorn.
 * Copyright (c) 2026 abit-foggy
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "include/verify.h"

#define HEADER_MAX_BYTES (1u << 20)

#define is_ident_start(c) \
    (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z') || (c) == '_')
#define is_ident(c) (is_ident_start(c) || ((c) >= '0' && (c) <= '9'))

static void
blank2(char *out, size_t *o)
{
    out[*o] = ' ';
    out[*o + 1] = ' ';
    *o += 2;
}

static int
collect_defined(const char *archive, StrList *syms)
{
    char cmd[4200];

    snprintf(cmd, sizeof(cmd), "nm -g --defined-only '%s' 2>/dev/null",
             archive);
    FILE *pp = popen(cmd, "r");
    if (pp == NULL) {
        diag("cannot run nm (is binutils installed?)");
        return THORN_RS_EXIT_IO;
    }
    char line[1024];
    while (fgets(line, sizeof(line), pp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\n' || *p == '\0')
            continue;
        /* shape: <address> <type> <name> */
        while (*p && *p != ' ' && *p != '\t')
            p++;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '\n' || (p[1] != ' ' && p[1] != '\t'))
            continue;
        p += 2;
        while (*p == ' ' || *p == '\t')
            p++;
        char *e = p + strlen(p);
        while (e > p && (e[-1] == '\n' || e[-1] == '\r' ||
                         e[-1] == ' ' || e[-1] == '\t'))
            *--e = '\0';
        if (*p == '\0')
            continue;
        if (*p == '_')
            p++;  /* Mach-O C symbol prefix */
        int dup = 0;
        for (size_t i = 0; i < syms->count; i++) {
            if (strcmp(syms->items[i], p) == 0) {
                dup = 1;
                break;
            }
        }
        if (!dup)
            strlist_push(syms, p);
    }
    pclose(pp);
    return 0;
}

/* Blank comments, string/char contents, and preprocessor lines. */
static void
blank_noncode(const char *src, size_t len, char *out)
{
    int in_block = 0, in_line = 0, in_str = 0, in_chr = 0;
    int line_start = 1;
    size_t i = 0, o = 0;

    while (i < len) {
        char c = src[i];
        char n = (i + 1 < len) ? src[i + 1] : '\0';

        if (in_block) {
            if (c == '*' && n == '/') {
                blank2(out, &o);
                in_block = 0;
                i += 2;
                continue;
            }
            out[o++] = (c == '\n') ? '\n' : ' ';
            i++;
            continue;
        }
        if (in_line) {
            out[o++] = (c == '\n') ? '\n' : ' ';
            if (c == '\n')
                in_line = 0;
            i++;
            continue;
        }
        if (in_str || in_chr) {
            if (c == '\\' && i + 1 < len) {
                blank2(out, &o);
                i += 2;
                continue;
            }
            out[o++] = (c == '\n') ? '\n' : ' ';
            if ((in_str && c == '"') || (in_chr && c == '\''))
                in_str = in_chr = 0;
            i++;
            continue;
        }
        if (c == '/' && n == '/') {
            blank2(out, &o);
            in_line = 1;
            i += 2;
            continue;
        }
        if (c == '/' && n == '*') {
            blank2(out, &o);
            in_block = 1;
            i += 2;
            continue;
        }
        if (c == '"') {
            out[o++] = ' ';
            in_str = 1;
            i++;
            continue;
        }
        if (c == '\'') {
            out[o++] = ' ';
            in_chr = 1;
            i++;
            continue;
        }
        if (c == '#' && line_start) {
            while (i < len) {
                out[o++] = (src[i] == '\n') ? '\n' : ' ';
                if (src[i] == '\n')
                    break;
                i++;
            }
            line_start = 1;
            continue;
        }
        out[o++] = c;
        line_start = (c == '\n');
        i++;
    }
    out[o] = '\0';
}

static int
is_skip_keyword(const char *w, size_t n)
{
    static const char *const kw[] = {
        "if", "while", "for", "switch", "return", "sizeof", "else",
        "do", "case", "typedef", "static", "goto", NULL
    };
    for (size_t i = 0; kw[i]; i++) {
        if (strlen(kw[i]) == n && strncmp(w, kw[i], n) == 0)
            return 1;
    }
    return 0;
}

/* Does the declaration segment leading to `pos` contain `kw` as a
   standalone word? */
static int
segment_has_keyword(const char *s, const char *pos, const char *kw)
{
    size_t kn = strlen(kw);
    const char *seg = pos;

    while (seg > s && seg[-1] != ';' && seg[-1] != '{' && seg[-1] != '}')
        seg--;
    for (const char *p = seg; p + kn <= pos; p++) {
        if (strncmp(p, kw, kn) == 0 &&
            (p == seg || !is_ident(p[-1])) && !is_ident(p[kn]))
            return 1;
    }
    return 0;
}

static int
collect_header_syms(const char *path, StrList *expected)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        diag("cannot open header '%s'", path);
        return THORN_RS_EXIT_IO;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return THORN_RS_EXIT_IO;
    }
    long sz = ftell(fp);
    if (sz < 0 || (unsigned long)sz > HEADER_MAX_BYTES) {
        diag("header '%s' is empty or exceeds the %u byte cap", path,
             (unsigned)HEADER_MAX_BYTES);
        fclose(fp);
        return THORN_RS_EXIT_IO;
    }
    rewind(fp);
    char *src = malloc((size_t)sz + 1);
    char *clean = malloc((size_t)sz + 1);
    if (src == NULL || clean == NULL) {
        free(src);
        free(clean);
        fclose(fp);
        return THORN_RS_EXIT_IO;
    }
    if (fread(src, 1, (size_t)sz, fp) != (size_t)sz) {
        diag("short read on header '%s'", path);
        free(src);
        free(clean);
        fclose(fp);
        return THORN_RS_EXIT_IO;
    }
    fclose(fp);
    blank_noncode(src, (size_t)sz, clean);
    free(src);

    for (const char *p = clean; *p;) {
        if (!is_ident_start(*p)) {
            p++;
            continue;
        }
        const char *name = p;
        while (is_ident(*p))
            p++;
        size_t nlen = (size_t)(p - name);
        const char *q = p;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
            q++;
        /* an identifier followed by '(' is a call, a declaration, or
           a control keyword; declarations end in ');' */
        if (*q != '(' || is_skip_keyword(name, nlen) ||
            segment_has_keyword(clean, name, "static") ||
            segment_has_keyword(clean, name, "typedef"))
            continue;
        int depth = 0;
        const char *r = q;
        for (; *r; r++) {
            if (*r == '(')
                depth++;
            else if (*r == ')') {
                depth--;
                if (depth == 0)
                    break;
            }
        }
        if (*r == '\0')
            break;
        r++;
        while (*r == ' ' || *r == '\t' || *r == '\n' || *r == '\r')
            r++;
        if (*r != ';')
            continue;
        if (nlen < 256) {
            char sym[256];
            memcpy(sym, name, nlen);
            sym[nlen] = '\0';
            strlist_push(expected, sym);
        }
    }
    free(clean);
    return 0;
}

int
verify_command(const char *archive, const char *header,
               const StrList *expected, char *buf, size_t n)
{
    size_t len = 0;

    int w = snprintf(buf, n, "check %s", archive);
    if (w < 0 || (size_t)w >= n)
        return 1;
    len = (size_t)w;
    for (size_t i = 0; i < expected->count; i++) {
        w = snprintf(buf + len, n - len, " --expect %s",
                     expected->items[i]);
        if (w < 0 || (size_t)w >= n - len)
            return 1;
        len += (size_t)w;
    }
    if (header) {
        w = snprintf(buf + len, n - len, " --header %s", header);
        if (w < 0 || (size_t)w >= n - len)
            return 1;
    }
    return 0;
}

int
verify_archive(const char *archive, const StrList *expected,
               const char *header)
{
    if (access(archive, R_OK) != 0) {
        diag("cannot read archive '%s'", archive);
        return THORN_RS_EXIT_IO;
    }

    StrList defined = {0};
    StrList want = {0};
    int rc = collect_defined(archive, &defined);
    if (rc != 0) {
        strlist_free(&defined);
        strlist_free(&want);
        return rc;
    }
    if (header) {
        rc = collect_header_syms(header, &want);
        if (rc != 0) {
            strlist_free(&defined);
            strlist_free(&want);
            return rc;
        }
    }
    for (size_t i = 0; i < expected->count; i++)
        strlist_push(&want, expected->items[i]);

    size_t missing = 0;
    for (size_t i = 0; i < want.count; i++) {
        int found = 0;
        for (size_t k = 0; k < defined.count; k++) {
            if (strcmp(defined.items[k], want.items[i]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            diag("expected C-ABI symbol '%s' is not exported by %s",
                 want.items[i], archive);
            missing++;
        }
    }

    size_t extra = 0;
    for (size_t i = 0; i < defined.count; i++) {
        int known = 0;
        for (size_t k = 0; k < want.count; k++) {
            if (strcmp(want.items[k], defined.items[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known)
            extra++;
    }

    size_t total = want.count;
    strlist_free(&defined);
    strlist_free(&want);

    if (missing > 0) {
        diag("C-ABI verification failed: %zu missing symbol%s, %zu "
             "extra export%s", missing, missing == 1 ? "" : "s",
             extra, extra == 1 ? "" : "s");
        return THORN_RS_EXIT_SCAN;
    }
    diag("C-ABI verified: %zu expected symbol%s, %zu extra export%s",
         total, total == 1 ? "" : "s",
         extra, extra == 1 ? "" : "s");
    return 0;
}
