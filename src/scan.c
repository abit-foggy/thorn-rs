/*
 * scan.c - C99 crate inspector.
 *
 * Comments and string/char literal contents are blanked before
 * attribute matching, so a commented-out #[panic_handler] can never
 * satisfy #![no_std].  A second pass keeps string contents for
 * #![crate_name = "..."] extraction.
 *
 * Part of thorn-rs, the zero-cargo Rust staticlib companion for thorn.
 * Copyright (c) 2026 abit-foggy
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "include/scan.h"

static int
read_file(const char *path, char **out_buf, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        diag("cannot open crate root '%s'", path);
        return SCAN_IO;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        diag("cannot seek crate root '%s'", path);
        fclose(fp);
        return SCAN_IO;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        diag("cannot size crate root '%s'", path);
        fclose(fp);
        return SCAN_IO;
    }
    if ((unsigned long)sz > SCAN_MAX_BYTES) {
        diag("crate root '%s' exceeds the %u byte scan cap", path,
             (unsigned)SCAN_MAX_BYTES);
        fclose(fp);
        return SCAN_INVALID;
    }
    rewind(fp);
    char *buf = malloc((size_t)sz + 1);
    if (buf == NULL) {
        diag("out of memory reading '%s'", path);
        fclose(fp);
        return SCAN_IO;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        diag("short read on crate root '%s'", path);
        free(buf);
        fclose(fp);
        return SCAN_IO;
    }
    fclose(fp);
    buf[sz] = '\0';
    *out_buf = buf;
    *out_len = (size_t)sz;
    return SCAN_OK;
}

/* Newlines always survive; other characters depend on `keep`. */
static void
emit1(char *dst, size_t *o, char c, int keep)
{
    if (c == '\n')
        dst[(*o)++] = '\n';
    else
        dst[(*o)++] = keep ? c : ' ';
}

static void
blank2(char *dst, size_t *o)
{
    dst[*o] = ' ';
    dst[*o + 1] = ' ';
    *o += 2;
}

static void
normalize(const char *src, size_t n, char *dst, int keep)
{
    size_t i = 0, o = 0;
    enum { S_CODE, S_LINE, S_BLOCK, S_STR, S_CHAR } st = S_CODE;
    int block_depth = 0;
    int raw_hashes = 0;    /* raw string delimiter width, S_STR only */

    while (i < n) {
        char c = src[i];

        if (st == S_LINE) {
            emit1(dst, &o, c, 0);
            if (c == '\n')
                st = S_CODE;
            i++;
            continue;
        }
        if (st == S_BLOCK) {
            if (c == '/' && i + 1 < n && src[i + 1] == '*') {
                block_depth++;
                blank2(dst, &o);
                i += 2;
                continue;
            }
            if (c == '*' && i + 1 < n && src[i + 1] == '/') {
                block_depth--;
                blank2(dst, &o);
                i += 2;
                if (block_depth == 0)
                    st = S_CODE;
                continue;
            }
            emit1(dst, &o, c, 0);
            i++;
            continue;
        }
        if (st == S_STR) {
            if (c == '\\' && i + 1 < n) {
                emit1(dst, &o, src[i], keep);
                emit1(dst, &o, src[i + 1], keep);
                i += 2;
                continue;
            }
            if (c == '"') {
                /* close only when followed by the opening hash run */
                size_t h = 0;
                while (h < (size_t)raw_hashes && i + 1 + h < n &&
                       src[i + 1 + h] == '#')
                    h++;
                if ((size_t)raw_hashes == h) {
                    size_t span = 1 + (size_t)h;
                    for (size_t k = 0; k < span; k++)
                        emit1(dst, &o, src[i + k], keep);
                    i += span;
                    st = S_CODE;
                    continue;
                }
            }
            emit1(dst, &o, c, keep);
            i++;
            continue;
        }
        if (st == S_CHAR) {
            if (c == '\\' && i + 1 < n) {
                emit1(dst, &o, src[i], keep);
                emit1(dst, &o, src[i + 1], keep);
                i += 2;
                continue;
            }
            if (c == '\'') {
                emit1(dst, &o, c, keep);
                st = S_CODE;
                i++;
                continue;
            }
            emit1(dst, &o, c, keep);
            i++;
            continue;
        }

        /* S_CODE: code characters are always verbatim; `keep` only
           governs literal contents. */
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            st = S_LINE;
            blank2(dst, &o);
            i += 2;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            st = S_BLOCK;
            block_depth = 1;
            blank2(dst, &o);
            i += 2;
            continue;
        }
        /* raw strings: r"...", r#"..."#, br##"..."## */
        if ((c == 'r' || c == 'b') && i + 1 < n) {
            size_t j = i + (c == 'b' ? 1 : 0);
            if (j < n && src[j] == 'r') {
                size_t h = 0;
                size_t k = j + 1;
                while (k < n && src[k] == '#') {
                    h++;
                    k++;
                }
                if (k < n && src[k] == '"') {
                    size_t span = (k - i) + 1;
                    raw_hashes = (int)h;
                    for (size_t q = 0; q < span; q++)
                        emit1(dst, &o, src[i + q], 1);
                    i += span;
                    st = S_STR;
                    continue;
                }
            }
        }
        if (c == '"') {
            raw_hashes = 0;
            emit1(dst, &o, c, keep);
            st = S_STR;
            i++;
            continue;
        }
        if (c == '\'') {
            /* 'x' and '\'' are literals; 'a as a lifetime is not. */
            if (i + 1 < n && (src[i + 1] == '\\' ||
                              (i + 2 < n && src[i + 2] == '\''))) {
                emit1(dst, &o, c, keep);
                st = S_CHAR;
                i++;
                continue;
            }
            emit1(dst, &o, c, 1);
            i++;
            continue;
        }
        emit1(dst, &o, c, 1);
        i++;
    }
    dst[o] = '\0';
}

static const char *
skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

/* Find `#[name]` (inner=0) or `#![name]` (inner=1). */
static int
find_attr(const char *s, const char *name, int inner)
{
    size_t nl = strlen(name);

    for (const char *p = strchr(s, '#'); p; p = strchr(p + 1, '#')) {
        const char *q = p + 1;
        if (inner) {
            if (*q != '!')
                continue;
            q++;
        }
        if (*q != '[')
            continue;
        q = skip_ws(q + 1);
        if (strncmp(q, name, nl) != 0)
            continue;
        q = skip_ws(q + nl);
        if (*q == ']')
            return 1;
    }
    return 0;
}

/* Value of `#![name = "value"]`; needs string contents kept. */
static int
find_attr_value(const char *s, const char *name, char *out, size_t outsz)
{
    size_t nl = strlen(name);

    for (const char *p = strchr(s, '#'); p; p = strchr(p + 1, '#')) {
        const char *q = p + 1;
        if (*q != '!')
            continue;
        q++;
        if (*q != '[')
            continue;
        q = skip_ws(q + 1);
        if (strncmp(q, name, nl) != 0)
            continue;
        q = skip_ws(q + nl);
        if (*q != '=')
            continue;
        q = skip_ws(q + 1);
        if (*q != '"')
            continue;
        q++;
        size_t k = 0;
        while (*q && *q != '"' && k + 1 < outsz)
            out[k++] = *q++;
        out[k] = '\0';
        return (*q == '"') ? 1 : 0;
    }
    return 0;
}

static int
is_ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int
is_ident(char c)
{
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

/* Position just past the `]` of the next `#[name]` (outer attribute)
   at or after `from`, or NULL. */
static const char *
find_attr_end(const char *from, const char *name)
{
    size_t nl = strlen(name);

    for (const char *p = strchr(from, '#'); p; p = strchr(p + 1, '#')) {
        const char *q = p + 1;
        if (*q != '[')
            continue;
        q = skip_ws(q + 1);
        if (strncmp(q, name, nl) != 0)
            continue;
        q = skip_ws(q + nl);
        if (*q == ']')
            return q + 1;
    }
    return NULL;
}

/* Collect `#[export_name = "sym"]` values; string contents kept. */
static void
collect_export_names(const char *s, StrList *out)
{
    for (const char *p = strchr(s, '#'); p; p = strchr(p + 1, '#')) {
        const char *q = p + 1;
        if (*q != '!')
            continue;
        q++;
        if (*q != '[')
            continue;
        q = skip_ws(q + 1);
        if (strncmp(q, "export_name", 11) != 0)
            continue;
        q = skip_ws(q + 11);
        if (*q != '=')
            continue;
        q = skip_ws(q + 1);
        if (*q != '"')
            continue;
        q++;
        char sym[256];
        size_t k = 0;
        while (*q && *q != '"' && k + 1 < sizeof(sym))
            sym[k++] = *q++;
        sym[k] = '\0';
        if (*q == '"' && sym[0])
            strlist_push(out, sym);
    }
}

/* Collect the function names `#[no_mangle] ... fn <name>` names. */
static void
collect_no_mangle_fns(const char *s, StrList *out)
{
    const char *p = s;

    for (;;) {
        p = find_attr_end(p, "no_mangle");
        if (p == NULL)
            return;
        /* the attributed item follows; find its `fn` keyword */
        const char *limit = p + 256;
        for (const char *r = p; r < limit && *r && *r != ';'; r++) {
            if (r[0] == 'f' && r[1] == 'n' &&
                (r == s || !is_ident(r[-1])) && !is_ident(r[2])) {
                const char *n = skip_ws(r + 2);
                char sym[256];
                size_t k = 0;
                while (is_ident(n[k]) && k + 1 < sizeof(sym)) {
                    sym[k] = n[k];
                    k++;
                }
                sym[k] = '\0';
                if (k)
                    strlist_push(out, sym);
                p = n + k;
                break;
            }
        }
    }
}

/* dirname helper for resolving sibling module files from the crate root. */
static int
crate_dir_name(const char *p, char *buf, size_t n)
{
    const char *slash = strrchr(p, '/');
    if (!slash) {
        snprintf(buf, n, ".");
        return 0;
    }
    size_t d = (size_t)(slash - p);
    if (d == 0)
        d = 1;
    if (d >= n)
        d = n - 1;
    memcpy(buf, p, d);
    buf[d] = '\0';
    return 0;
}

/* Scan a sibling module file for panic_handler / no_mangle / export_name.
   Does not touch no_std or crate_name (those come from the root).  Tracks
   total bytes scanned across modules to enforce SCAN_MAX_MODULE_BYTES.
   Returns SCAN_OK / SCAN_IO / SCAN_INVALID. */
static int
scan_file_attrs(const char *path, RustCrate *out, size_t *module_bytes)
{
    char *src = NULL;
    size_t len = 0;
    int rc = read_file(path, &src, &len);
    if (rc != SCAN_OK)
        return rc;
    if (*module_bytes + len > SCAN_MAX_MODULE_BYTES) {
        free(src);
        diag("module bytes exceed the %zu byte scan cap",
             (unsigned long)SCAN_MAX_MODULE_BYTES);
        return SCAN_INVALID;
    }
    *module_bytes += len;

    char *clean = malloc(len + 1);
    char *nocomment = malloc(len + 1);
    if (!clean || !nocomment) {
        free(clean);
        free(nocomment);
        free(src);
        diag("out of memory normalizing '%s'", path);
        return SCAN_IO;
    }
    normalize(src, len, clean, 0);
    normalize(src, len, nocomment, 1);
    free(src);

    if (find_attr(clean, "panic_handler", 0))
        out->has_panic_handler = 1;
    collect_no_mangle_fns(clean, &out->capi_syms);
    collect_export_names(nocomment, &out->capi_syms);

    free(clean);
    free(nocomment);
    return SCAN_OK;
}

/* Collect `mod name;` declarations from normalized source text.
   Fills names[] with up to max entries, returns count.  Whitespace
   tolerant; only top-level declarations are reported. */
static int
find_mod_decls(const char *s, char names[][128], int max)
{
    int count = 0;
    for (const char *p = strstr(s, "mod "); p && count < max;
         p = strstr(p + 4, "mod ")) {
        p += 4;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (!is_ident_start(*p))
            continue;
        const char *start = p;
        while (is_ident(*p))
            p++;
        size_t len = (size_t)(p - start);
        if (len >= 128)
            len = 127;
        memcpy(names[count], start, len);
        names[count][len] = '\0';
        count++;
        /* skip to after ';' or '{' or end of line */
        while (*p && *p != ';' && *p != '{' && *p != '\n')
            p++;
        if (*p)
            p++;
    }
    return count;
}

int
scan_crate(const char *path, RustCrate *out)
{
    if (path == NULL || out == NULL)
        return SCAN_INVALID;

    /* `out->root` may alias `path`; never self-assign via snprintf. */
    char pathbuf[1024];
    if (strlen(path) >= sizeof(pathbuf)) {
        diag("crate root path exceeds %zu bytes", sizeof(pathbuf) - 1);
        return SCAN_INVALID;
    }
    snprintf(pathbuf, sizeof(pathbuf), "%s", path);
    path = pathbuf;

    if (has_whitespace(path)) {
        diag("crate root path contains whitespace: '%s'", path);
        diag("whitespace paths are unrepresentable in depfiles; rename "
             "the file");
        return SCAN_INVALID;
    }

    char *src = NULL;
    size_t len = 0;
    int rc = read_file(path, &src, &len);
    if (rc != SCAN_OK)
        return rc;

    char *clean = malloc(len + 1);
    char *nocomment = malloc(len + 1);
    if (clean == NULL || nocomment == NULL) {
        diag("out of memory normalizing '%s'", path);
        free(clean);
        free(nocomment);
        free(src);
        return SCAN_IO;
    }
    normalize(src, len, clean, 0);
    normalize(src, len, nocomment, 1);
    free(src);

    out->no_std = find_attr(clean, "no_std", 1);
    out->has_panic_handler = find_attr(clean, "panic_handler", 0);
    int has_global_allocator = find_attr(clean, "global_allocator", 0);
    int uses_alloc = strstr(clean, "extern crate alloc") != NULL;

    if (out->no_std) {
        if (out->panic_abort < 0)
            out->panic_abort = 1;
        if (out->opt_level == RUST_OPT_UNSET)
            out->opt_level = 'z';
    } else if (out->panic_abort < 0) {
        out->panic_abort = 0;
    }

    char attr_name[128];
    if (find_attr_value(nocomment, "crate_name", attr_name,
                        sizeof(attr_name))) {
        if (out->name[0] == '\0') {
            snprintf(out->name, sizeof(out->name), "%s", attr_name);
        } else if (strcmp(out->name, attr_name) != 0) {
            diag("note: --name '%s' overrides #![crate_name = \"%s\"]",
                 out->name, attr_name);
        }
    }

    collect_no_mangle_fns(clean, &out->capi_syms);
    collect_export_names(nocomment, &out->capi_syms);

    /* Scan sibling module files for panic_handler / no_mangle / export_name.
       A crate root like `mod math;` pulls in math.rs or math/mod.rs.
       Only top-level `mod` declarations are followed; deeper nesting is
       the module's own responsibility. */
    if (out->no_std) {
        char dir[1024];
        crate_dir_name(path, dir, sizeof(dir));
        char mods[SCAN_MAX_MODULES][128];
        int nmods = find_mod_decls(clean, mods, SCAN_MAX_MODULES);
        size_t module_bytes = 0;
        for (int i = 0; i < nmods; i++) {
            char cand1[1024], cand2[1024];
            snprintf(cand1, sizeof(cand1), "%s/%s.rs", dir, mods[i]);
            snprintf(cand2, sizeof(cand2), "%s/%s/mod.rs", dir, mods[i]);
            int rc = SCAN_OK;
            if (access(cand1, F_OK) == 0)
                rc = scan_file_attrs(cand1, out, &module_bytes);
            else if (access(cand2, F_OK) == 0)
                rc = scan_file_attrs(cand2, out, &module_bytes);
            if (rc != SCAN_OK) {
                free(clean);
                free(nocomment);
                return rc;
            }
        }
    }

    free(clean);
    free(nocomment);

    if (out->no_std && !out->has_panic_handler) {
        diag("%s declares #![no_std] but no in-tree #[panic_handler] "
             "was found", path);
        diag("there is no Cargo to supply one; add a handler to the "
             "crate root (see tests/fixtures/probe/lib.rs)");
        return SCAN_INVALID;
    }

    if (uses_alloc && !has_global_allocator)
        diag("warning: %s uses `extern crate alloc` without an "
             "in-tree #[global_allocator]", path);

    if (out->name[0] == '\0') {
        diag("no crate name: pass --name or add "
             "#![crate_name = \"...\"] to %s", path);
        return SCAN_INVALID;
    }

    snprintf(out->root, sizeof(out->root), "%s", path);
    return SCAN_OK;
}
