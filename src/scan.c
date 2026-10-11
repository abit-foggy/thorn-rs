/* scan.c - C99 crate inspector for thorn-rs.
 *
 * Reads a crate root once (bounded), then normalizes it twice:
 *   - `clean`:     comments AND string/char literal contents blanked
 *                  (structure and newlines kept) - used for attribute
 *                  matching, so a `#[panic_handler]` hidden in a comment
 *                  or string can never satisfy the #![no_std] contract
 *                  (see tests/fixtures/missing-ph)
 *   - `nocomment`: comments blanked, string contents kept verbatim -
 *                  used to extract #![crate_name = "..."] values
 *
 * Trust model (docs/SCOPING.md section 2.2): the crate root is a trusted
 * project input; this scanner is a bounded advisor that fails fast
 * before rustc, which remains the authority. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/scan.h"

/* ------------------------------------------------------------------ */
/* Bounded read                                                        */
/* ------------------------------------------------------------------ */

static int read_file(const char *path, char **out_buf, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
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
    if (!buf) {
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

/* ------------------------------------------------------------------ */
/* Normalizer                                                          */
/* ------------------------------------------------------------------ */

/* Newlines always survive (structure visibility); other characters are
 * kept verbatim in keep mode and blanked otherwise. */
static void emit1(char *dst, size_t *o, char c, int keep)
{
    if (c == '\n')
        dst[(*o)++] = '\n';
    else
        dst[(*o)++] = keep ? c : ' ';
}

/* Two-character sequences (comment delimiters) are always blanked. */
static void blank2(char *dst, size_t *o)
{
    dst[*o] = ' ';
    dst[*o + 1] = ' ';
    *o += 2;
}

static void normalize(const char *src, size_t n, char *dst, int keep)
{
    size_t i = 0, o = 0;
    enum { S_CODE, S_LINE, S_BLOCK, S_STR, S_CHAR } st = S_CODE;
    int block_depth = 0;
    int raw_hashes = 0;   /* raw string delimiter width, S_STR only */

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

        /* S_CODE - code characters are always emitted verbatim; the
           keep flag only governs string/char literal CONTENT. */
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
        /* raw strings: r"...", r#"..."#, br##"..."## (hash count kept) */
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
                    size_t span = (k - i) + 1; /* prefix + opening quote */
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
            /* char literal vs lifetime: '\'' and 'x' are literals,
               'a as a lifetime prefix is a single code character. */
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

/* ------------------------------------------------------------------ */
/* Whitespace-tolerant attribute matching                              */
/* ------------------------------------------------------------------ */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

/* Find `#[name]` (inner=0) or `#![name]` (inner=1) in normalized text. */
static int find_attr(const char *s, const char *name, int inner)
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

/* Extract the string value of `#![name = "value"]` (needs a buffer in
 * which string contents survived: the `nocomment` pass). */
static int find_attr_value(const char *s, const char *name, char *out,
                           size_t outsz)
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

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int scan_crate(const char *path, RustCrate *out)
{
    if (!path || !out)
        return SCAN_INVALID;

    /* Local copy: `out->root` may alias `path` (the CLI stores the
       positional argument there); never self-assign through snprintf. */
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
             "the file (thorn has_whitespace invariant)");
        return SCAN_INVALID;
    }

    char *src = NULL;
    size_t len = 0;
    int rc = read_file(path, &src, &len);
    if (rc != SCAN_OK)
        return rc;

    char *clean = malloc(len + 1);
    char *nocomment = malloc(len + 1);
    if (!clean || !nocomment) {
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

    /* no_std defaults (docs/SCOPING.md section 3.2 flag table):
       abort + opt-level=z unless the CLI already set them explicitly.
       panic_abort -1 = unspecified; an explicit --panic unwind (0) is
       never overridden. */
    if (out->no_std) {
        if (out->panic_abort < 0)
            out->panic_abort = 1;
        if (out->opt_level == RUST_OPT_UNSET)
            out->opt_level = 'z';
    } else if (out->panic_abort < 0) {
        out->panic_abort = 0;
    }

    /* crate name: CLI flag wins; #![crate_name = "..."] fills the gap. */
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

    free(clean);
    free(nocomment);

    if (out->no_std && !out->has_panic_handler) {
        diag("%s declares #![no_std] but no in-tree #[panic_handler] was "
             "found", path);
        diag("Phase 0 has no Cargo to supply one; add a handler to the "
             "crate root (see tests/fixtures/probe/lib.rs)");
        return SCAN_INVALID;
    }

    if (uses_alloc && !has_global_allocator)
        diag("warning: %s uses `extern crate alloc` without an in-tree "
             "#[global_allocator]; linking will fail without one", path);

    if (out->name[0] == '\0') {
        diag("no crate name: pass --name or add "
             "#![crate_name = \"...\"] to %s", path);
        return SCAN_INVALID;
    }

    snprintf(out->root, sizeof(out->root), "%s", path);
    return SCAN_OK;
}
