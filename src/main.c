/*
 * main.c - thorn-rs CLI driver.
 *
 * `emit` writes a build fragment; `plan` prints the rustc command.
 * Pipeline: scrub env -> parse -> scan -> resolve -> plan -> emit.
 * Exit codes: 0 ok, 1 scan/plan, 2 I/O, 64 usage.
 *
 * Part of thorn-rs, the zero-cargo Rust staticlib companion for thorn.
 * Copyright (c) 2026 abit-foggy
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "include/emit.h"
#include "include/plan.h"
#include "include/rust.h"
#include "include/scan.h"
#include "include/verify.h"

enum { FMT_NONE = 0, FMT_NINJA = 1, FMT_MAKE = 2 };

static void
usage(FILE *out)
{
    fprintf(out,
        "thorn-rs " THORN_RS_VERSION " - zero-cargo #![no_std] Rust "
        "static libraries for thorn\n\n"
        "USAGE:\n"
        "    thorn-rs [crate-root ...] [flags]   build (default root: "
        "src/lib.rs)\n"
        "    thorn-rs build [-f root ...] [-o dir] [-e ninja|make|both]\n"
        "                    [--rustc bin] [--header FILE] [crate "
        "flags]\n"
        "    thorn-rs emit <crate-root.rs> [flags]   write a single "
        "fragment\n"
        "    thorn-rs plan <crate-root.rs> [flags]   print the planned "
        "rustc command\n"
        "    thorn-rs check <archive.a> [--expect SYM] [--header FILE]\n"
        "    thorn-rs version | help\n\n"
        "BUILD FLAGS:\n"
        "    -f, --file ROOT       crate root (repeatable, with "
        "positionals)\n"
        "    -o, --out-dir DIR     destination directory (default: .; "
        "created)\n"
        "    -e, --engine ENGINE   ninja | make | both (default: ninja)\n"
        "    --rustc BIN           rustc binary (mirrors thorn's "
        "--compiler;\n"
        "                          default: RUSTC env, then PATH)\n"
        "    --header FILE         C-ABI contract; stamps a "
        ".thorn-rs/<name>.verified\n"
        "                          audit into the fragment\n\n"
        "CRATE FLAGS (apply to every crate):\n"
        "    --name NAME           crate name (default: "
        "#![crate_name] attribute)\n"
        "    --edition EDITION     2015 | 2018 | 2021 | 2024 "
        "(default: 2021)\n"
        "    --target TRIPLE       rustc --target (default: host)\n"
        "    --opt-level LEVEL     0 | 1 | 2 | 3 | s | z\n"
        "    --panic STRATEGY      abort | unwind "
        "(default: abort for #![no_std])\n"
        "    --extern NAME=PATH    repeatable\n"
        "    -L DIR                repeatable dependency search path\n"
        "    --cfg SPEC            repeatable, e.g. 'feature=\"std\"'\n"
        "    --rustflag FLAG       repeatable extra rustc pass\n\n"
        "EMIT FLAGS (low-level single-fragment plumbing):\n"
        "    --format ninja|make   fragment format (default: infer "
        "from --out)\n"
        "    --out FILE            fragment destination\n"
        "    --artifact PATH       archive path override\n\n"
        "EXIT CODES: 0 ok; 1 scan/plan failure; 2 file I/O; 64 usage\n");
}

static int
bad_flag_ret(const char *flag)
{
    diag("missing value for %s", flag);
    return THORN_RS_EXIT_USAGE;
}

static int
ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

static int
infer_format(const char *out)
{
    if (ends_with(out, ".ninja"))
        return FMT_NINJA;
    if (ends_with(out, ".mk"))
        return FMT_MAKE;
    const char *b = strrchr(out, '/');
    b = b ? b + 1 : out;
    if (strcmp(b, "Makefile") == 0 || strcmp(b, "makefile") == 0 ||
        strcmp(b, "GNUmakefile") == 0)
        return FMT_MAKE;
    return FMT_NONE;
}

static const char *
dir_name(const char *p, char *buf, size_t n)
{
    const char *slash = strrchr(p, '/');

    if (slash == NULL) {
        snprintf(buf, n, ".");
        return buf;
    }
    size_t d = (size_t)(slash - p);
    if (d == 0)
        d = 1; /* "/file" -> "/" */
    if (d >= n)
        d = n - 1;
    memcpy(buf, p, d);
    buf[d] = '\0';
    return buf;
}

static int
ensure_parent_dir(const char *path)
{
    char dir[1024];

    dir_name(path, dir, sizeof(dir));
    if (strcmp(dir, ".") == 0)
        return 0;
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
            diag("cannot create directory '%s'", dir);
            return -1;
        }
        *p = '/';
    }
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        diag("cannot create directory '%s'", dir);
        return -1;
    }
    return 0;
}

/* The relative path from a fragment sitting in `dir` back to the
   emission working directory: "out/rs" -> "../..", "." -> "". */
static const char *
compute_ups(const char *dir, char *buf, size_t n)
{
    char tmp[1024];

    snprintf(tmp, sizeof(tmp), "%s", dir);
    size_t l = strlen(tmp);
    while (l > 1 && tmp[l - 1] == '/')
        tmp[--l] = '\0';
    if (tmp[0] == '\0' || strcmp(tmp, ".") == 0 || tmp[0] == '/') {
        buf[0] = '\0';
        return buf;
    }
    const char *p = tmp;
    while (p[0] == '.' && p[1] == '/')
        p += 2;
    size_t depth = 1;
    for (const char *q = p; *q; q++)
        if (*q == '/')
            depth++;
    buf[0] = '\0';
    for (size_t i = 0; i < depth; i++) {
        if (strlen(buf) + 3 >= n)
            break;
        strcat(buf, "../");
    }
    return buf;
}

static int
set_edition(char *dst, size_t n, const char *v)
{
    if (strcmp(v, "2015") && strcmp(v, "2018") &&
        strcmp(v, "2021") && strcmp(v, "2024"))
        return -1;
    snprintf(dst, n, "%s", v);
    return 0;
}

static int
parse_opt_level(const char *v)
{
    if (strlen(v) != 1)
        return -1;
    switch (v[0]) {
    case '0': case '1': case '2': case '3': case 's': case 'z':
        return (int)v[0];
    default:
        return -1;
    }
}

static int
parse_args(int argc, char **argv, RustCrate *c, int *format,
           int *rustc_given, char *rustc_flag, size_t rsz)
{
    *format = FMT_NONE;
    *rustc_given = 0;
    int root_seen = 0;

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        int consumes = 0;

        if (strcmp(a, "--name") == 0) {
            if (v == NULL)
                return bad_flag_ret(a);
            snprintf(c->name, sizeof(c->name), "%s", v);
            consumes = 1;
        } else if (strcmp(a, "--format") == 0) {
            if (v == NULL)
                return bad_flag_ret(a);
            if (strcmp(v, "ninja") == 0)
                *format = FMT_NINJA;
            else if (strcmp(v, "make") == 0)
                *format = FMT_MAKE;
            else {
                diag("unknown --format '%s' (expected ninja or make)", v);
                return THORN_RS_EXIT_USAGE;
            }
            consumes = 1;
        } else if (strcmp(a, "--out") == 0) {
            if (v == NULL)
                return bad_flag_ret(a);
            snprintf(c->frag, sizeof(c->frag), "%s", v);
            consumes = 1;
        } else if (strcmp(a, "--artifact") == 0) {
            if (v == NULL)
                return bad_flag_ret(a);
            snprintf(c->out, sizeof(c->out), "%s", v);
            consumes = 1;
        } else if (strcmp(a, "--edition") == 0) {
            if (v == NULL)
                return bad_flag_ret(a);
            if (set_edition(c->edition, sizeof(c->edition), v) != 0) {
                diag("unknown --edition '%s' (expected 2015, 2018, "
                     "2021, or 2024)", v);
                return THORN_RS_EXIT_USAGE;
            }
            consumes = 1;
        } else if (strcmp(a, "--target") == 0) {
            if (v == NULL)
                return bad_flag_ret(a);
            snprintf(c->target, sizeof(c->target), "%s", v);
            consumes = 1;
        } else if (strcmp(a, "--opt-level") == 0) {
            if (v == NULL)
                return bad_flag_ret(a);
            int lv = parse_opt_level(v);
            if (lv < 0) {
                diag("unknown --opt-level '%s' (expected 0-3, s, or z)",
                     v);
                return THORN_RS_EXIT_USAGE;
            }
            c->opt_level = lv;
            consumes = 1;
        } else if (strcmp(a, "--panic") == 0) {
            if (v == NULL)
                return bad_flag_ret(a);
            if (strcmp(v, "abort") == 0)
                c->panic_abort = 1;
            else if (strcmp(v, "unwind") == 0)
                c->panic_abort = 0;
            else {
                diag("unknown --panic '%s' (expected abort or unwind)",
                     v);
                return THORN_RS_EXIT_USAGE;
            }
            consumes = 1;
        } else if (strcmp(a, "--header") == 0) {
            if (v == NULL)
                return bad_flag_ret(a);
            snprintf(c->header, sizeof(c->header), "%s", v);
            consumes = 1;
        } else if (strcmp(a, "--extern") == 0) {
            if (v == NULL)
                return bad_flag_ret(a);
            strlist_push(&c->externs, v);
            consumes = 1;
        } else if (strcmp(a, "-L") == 0 ||
                   strcmp(a, "--search-path") == 0) {
            if (v == NULL)
                return bad_flag_ret(a);
            strlist_push(&c->search_paths, v);
            consumes = 1;
        } else if (strcmp(a, "--cfg") == 0) {
            if (v == NULL)
                return bad_flag_ret(a);
            strlist_push(&c->cfgs, v);
            consumes = 1;
        } else if (strcmp(a, "--rustflag") == 0) {
            if (v == NULL)
                return bad_flag_ret(a);
            strlist_push(&c->rustflags, v);
            consumes = 1;
        } else if (strcmp(a, "--rustc") == 0) {
            if (v == NULL)
                return bad_flag_ret(a);
            snprintf(rustc_flag, rsz, "%s", v);
            *rustc_given = 1;
            consumes = 1;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            exit(THORN_RS_EXIT_OK);
        } else if (a[0] == '-' && a[1] != '\0') {
            diag("unknown flag '%s'", a);
            return THORN_RS_EXIT_USAGE;
        } else {
            if (root_seen) {
                diag("unexpected extra argument '%s'", a);
                return THORN_RS_EXIT_USAGE;
            }
            root_seen = 1;
            snprintf(c->root, sizeof(c->root), "%s", a);
        }
        i += consumes;
    }

    if (!root_seen) {
        diag("missing crate root argument");
        return THORN_RS_EXIT_USAGE;
    }
    return 0;
}

/* thorn-rs check <archive.a> [--expect SYM]... [--header FILE] */
static int
cmd_check(int argc, char **argv)
{
    const char *archive = NULL;
    const char *header = NULL;
    StrList expects = {0};
    int archive_seen = 0;

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (strcmp(a, "--expect") == 0) {
            if (v == NULL) {
                strlist_free(&expects);
                return bad_flag_ret(a);
            }
            strlist_push(&expects, v);
            i++;
        } else if (strcmp(a, "--header") == 0) {
            if (v == NULL) {
                strlist_free(&expects);
                return bad_flag_ret(a);
            }
            header = v;
            i++;
        } else if (a[0] == '-' && a[1] != '\0') {
            strlist_free(&expects);
            diag("unknown flag '%s'", a);
            return THORN_RS_EXIT_USAGE;
        } else {
            if (archive_seen) {
                strlist_free(&expects);
                diag("unexpected extra argument '%s'", a);
                return THORN_RS_EXIT_USAGE;
            }
            archive_seen = 1;
            archive = a;
        }
    }

    if (!archive_seen) {
        strlist_free(&expects);
        diag("check expects an archive path");
        return THORN_RS_EXIT_USAGE;
    }
    if (header && has_whitespace(header)) {
        strlist_free(&expects);
        diag("header path contains whitespace: '%s'", header);
        return THORN_RS_EXIT_SCAN;
    }
    int rc = verify_archive(archive, &expects, header);
    strlist_free(&expects);
    return rc;
}

/* thorn-rs build [-f root ...] [-o dir] [-e engine] [flags] [root ...]
 *
 * The thorn-parity front-end: scans one or more crate roots, plans
 * deterministic rustc invocations, and writes backend fragments into
 * --out-dir.  Pure meta-build generation; no backend is invoked. */
struct SharedFlags {
    char edition[16];
    char target[128];
    int opt_level;
    int panic_abort;
    char header[1024];
    StrList cfgs;
    StrList rustflags;
    StrList externs;
    StrList search_paths;
};

static int
cmd_build(int argc, char **argv)
{
    StrList roots = {0};
    struct SharedFlags sf;
    char out_dir[1024];
    char rustc_flag[1024];
    int engine = FMT_NINJA;
    int rustc_given = 0;
    int rc = 0;

    memset(&sf, 0, sizeof(sf));
    snprintf(sf.edition, sizeof(sf.edition), "2021");
    sf.opt_level = RUST_OPT_UNSET;
    sf.panic_abort = -1;
    out_dir[0] = '\0';
    rustc_flag[0] = '\0';

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        int consumes = 0;

        if (strcmp(a, "-f") == 0 || strcmp(a, "--file") == 0) {
            if (v == NULL) {
                rc = bad_flag_ret(a);
                goto out;
            }
            strlist_push(&roots, v);
            consumes = 1;
        } else if (strcmp(a, "-o") == 0 || strcmp(a, "--out-dir") == 0) {
            if (v == NULL) {
                rc = bad_flag_ret(a);
                goto out;
            }
            snprintf(out_dir, sizeof(out_dir), "%s", v);
            consumes = 1;
        } else if (strcmp(a, "-e") == 0 || strcmp(a, "--engine") == 0) {
            if (v == NULL) {
                rc = bad_flag_ret(a);
                goto out;
            }
            if (strcmp(v, "ninja") == 0)
                engine = FMT_NINJA;
            else if (strcmp(v, "make") == 0)
                engine = FMT_MAKE;
            else if (strcmp(v, "both") == 0)
                engine = FMT_NINJA | FMT_MAKE;
            else {
                diag("unknown --engine '%s' (expected ninja, make, or "
                     "both)", v);
                rc = THORN_RS_EXIT_USAGE;
                goto out;
            }
            consumes = 1;
        } else if (strcmp(a, "--rustc") == 0) {
            if (v == NULL) {
                rc = bad_flag_ret(a);
                goto out;
            }
            snprintf(rustc_flag, sizeof(rustc_flag), "%s", v);
            rustc_given = 1;
            consumes = 1;
        } else if (strcmp(a, "--header") == 0) {
            if (v == NULL) {
                rc = bad_flag_ret(a);
                goto out;
            }
            snprintf(sf.header, sizeof(sf.header), "%s", v);
            consumes = 1;
        } else if (strcmp(a, "--edition") == 0) {
            if (v == NULL) {
                rc = bad_flag_ret(a);
                goto out;
            }
            if (set_edition(sf.edition, sizeof(sf.edition), v) != 0) {
                diag("unknown --edition '%s'", v);
                rc = THORN_RS_EXIT_USAGE;
                goto out;
            }
            consumes = 1;
        } else if (strcmp(a, "--target") == 0) {
            if (v == NULL) {
                rc = bad_flag_ret(a);
                goto out;
            }
            snprintf(sf.target, sizeof(sf.target), "%s", v);
            consumes = 1;
        } else if (strcmp(a, "--opt-level") == 0) {
            if (v == NULL) {
                rc = bad_flag_ret(a);
                goto out;
            }
            int lv = parse_opt_level(v);
            if (lv < 0) {
                diag("unknown --opt-level '%s'", v);
                rc = THORN_RS_EXIT_USAGE;
                goto out;
            }
            sf.opt_level = lv;
            consumes = 1;
        } else if (strcmp(a, "--panic") == 0) {
            if (v == NULL) {
                rc = bad_flag_ret(a);
                goto out;
            }
            if (strcmp(v, "abort") == 0)
                sf.panic_abort = 1;
            else if (strcmp(v, "unwind") == 0)
                sf.panic_abort = 0;
            else {
                diag("unknown --panic '%s'", v);
                rc = THORN_RS_EXIT_USAGE;
                goto out;
            }
            consumes = 1;
        } else if (strcmp(a, "--extern") == 0) {
            if (v == NULL) {
                rc = bad_flag_ret(a);
                goto out;
            }
            strlist_push(&sf.externs, v);
            consumes = 1;
        } else if (strcmp(a, "-L") == 0 ||
                   strcmp(a, "--search-path") == 0) {
            if (v == NULL) {
                rc = bad_flag_ret(a);
                goto out;
            }
            strlist_push(&sf.search_paths, v);
            consumes = 1;
        } else if (strcmp(a, "--cfg") == 0) {
            if (v == NULL) {
                rc = bad_flag_ret(a);
                goto out;
            }
            strlist_push(&sf.cfgs, v);
            consumes = 1;
        } else if (strcmp(a, "--rustflag") == 0) {
            if (v == NULL) {
                rc = bad_flag_ret(a);
                goto out;
            }
            strlist_push(&sf.rustflags, v);
            consumes = 1;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            goto out;
        } else if (a[0] == '-' && a[1] != '\0') {
            diag("unknown flag '%s'", a);
            rc = THORN_RS_EXIT_USAGE;
            goto out;
        } else {
            strlist_push(&roots, a);
        }
        i += consumes;
    }

    if (roots.count == 0)
        strlist_push(&roots, "src/lib.rs");
    if (out_dir[0] == '\0')
        snprintf(out_dir, sizeof(out_dir), ".");

    RustCrate *crates = calloc(roots.count, sizeof(RustCrate));
    const RustCrate **ptrs = calloc(roots.count, sizeof(*ptrs));
    if (crates == NULL || ptrs == NULL) {
        diag("out of memory");
        free(crates);
        free(ptrs);
        rc = THORN_RS_EXIT_IO;
        goto out;
    }

    for (size_t i = 0; i < roots.count; i++) {
        RustCrate *c = &crates[i];
        ptrs[i] = c;
        rust_crate_init(c);
        snprintf(c->edition, sizeof(c->edition), "%s", sf.edition);
        snprintf(c->target, sizeof(c->target), "%s", sf.target);
        c->opt_level = sf.opt_level;
        c->panic_abort = sf.panic_abort;
        snprintf(c->header, sizeof(c->header), "%s", sf.header);
        for (size_t k = 0; k < sf.cfgs.count; k++)
            strlist_push(&c->cfgs, sf.cfgs.items[k]);
        for (size_t k = 0; k < sf.rustflags.count; k++)
            strlist_push(&c->rustflags, sf.rustflags.items[k]);
        for (size_t k = 0; k < sf.externs.count; k++)
            strlist_push(&c->externs, sf.externs.items[k]);
        for (size_t k = 0; k < sf.search_paths.count; k++)
            strlist_push(&c->search_paths, sf.search_paths.items[k]);

        rc = scan_crate(roots.items[i], c);
        if (rc != SCAN_OK) {
            rc = rc == SCAN_IO ? THORN_RS_EXIT_IO : THORN_RS_EXIT_SCAN;
            goto out_free;
        }
        if (c->header[0] && !path_safe(c->header)) {
            diag("header path is unsafe in build fragments: '%s'",
                 c->header);
            rc = THORN_RS_EXIT_SCAN;
            goto out_free;
        }
        int w = (strcmp(out_dir, ".") == 0)
                    ? snprintf(c->out, sizeof(c->out), "lib%s.a", c->name)
                    : snprintf(c->out, sizeof(c->out), "%s/lib%s.a",
                               out_dir, c->name);
        if (w < 0 || (size_t)w >= sizeof(c->out)) {
            diag("artifact path for crate '%s' exceeds the path limit",
                 c->name);
            rc = THORN_RS_EXIT_SCAN;
            goto out_free;
        }
        if (!path_safe(c->out)) {
            diag("artifact path is unsafe in build fragments: '%s'",
                 c->out);
            rc = THORN_RS_EXIT_SCAN;
            goto out_free;
        }
        for (size_t k = 0; k < i; k++) {
            if (strcmp(crates[k].name, c->name) == 0) {
                diag("duplicate crate name '%s' (one archive per name; "
                     "see docs/SCOPING.md section 5.1)", c->name);
                rc = THORN_RS_EXIT_SCAN;
                goto out_free;
            }
        }
    }

    char tool[1024], resolved[4096];
    rc = plan_locate_rustc(rustc_given ? rustc_flag : NULL, tool,
                           sizeof(tool), resolved, sizeof(resolved));
    if (rc != 0)
        goto out_free;
    plan_probe_version(resolved);

    char ups[1024];
    compute_ups(out_dir, ups, sizeof(ups));
    int ninja_wanted = (engine & FMT_NINJA) != 0;
    int make_wanted = (engine & FMT_MAKE) != 0;

    if (ninja_wanted) {
        char frag[1200];
        int w = (strcmp(out_dir, ".") == 0)
                    ? snprintf(frag, sizeof(frag), "build-rs.ninja")
                    : snprintf(frag, sizeof(frag), "%s/build-rs.ninja",
                               out_dir);
        if (w < 0 || (size_t)w >= sizeof(frag)) {
            diag("fragment path exceeds the path limit");
            rc = THORN_RS_EXIT_SCAN;
            goto out_free;
        }
        for (size_t i = 0; i < roots.count; i++) {
            int fw = snprintf(crates[i].frag, sizeof(crates[i].frag),
                              "%s", frag);
            if (fw < 0 || (size_t)fw >= sizeof(crates[i].frag)) {
                diag("fragment path exceeds the crate path limit");
                rc = THORN_RS_EXIT_SCAN;
                goto out_free;
            }
        }
        if (ensure_parent_dir(frag) != 0) {
            rc = THORN_RS_EXIT_IO;
            goto out_free;
        }
        FILE *fp = fopen(frag, "w");
        if (fp == NULL) {
            diag("cannot write fragment '%s'", frag);
            rc = THORN_RS_EXIT_IO;
            goto out_free;
        }
        int erc = emit_ninja_set(ptrs, roots.count, tool, fp);
        if (fclose(fp) != 0) {
            diag("write error on fragment '%s'", frag);
            erc = THORN_RS_EXIT_IO;
        }
        if (erc != 0) {
            rc = erc;
            goto out_free;
        }
        diag("wrote %s (ninja) for %zu crate%s", frag, roots.count,
             roots.count == 1 ? "" : "s");
    }

    if (make_wanted) {
        char frag[1200];
        int w = (strcmp(out_dir, ".") == 0)
                    ? snprintf(frag, sizeof(frag), "build-rs.mk")
                    : snprintf(frag, sizeof(frag), "%s/Makefile",
                               out_dir);
        if (w < 0 || (size_t)w >= sizeof(frag)) {
            diag("fragment path exceeds the path limit");
            rc = THORN_RS_EXIT_SCAN;
            goto out_free;
        }
        for (size_t i = 0; i < roots.count; i++) {
            int fw = snprintf(crates[i].frag, sizeof(crates[i].frag),
                              "%s", frag);
            if (fw < 0 || (size_t)fw >= sizeof(crates[i].frag)) {
                diag("fragment path exceeds the crate path limit");
                rc = THORN_RS_EXIT_SCAN;
                goto out_free;
            }
        }
        if (ensure_parent_dir(frag) != 0) {
            rc = THORN_RS_EXIT_IO;
            goto out_free;
        }
        FILE *fp = fopen(frag, "w");
        if (fp == NULL) {
            diag("cannot write fragment '%s'", frag);
            rc = THORN_RS_EXIT_IO;
            goto out_free;
        }
        int erc = emit_make_set(ptrs, roots.count, tool, ups, fp);
        if (fclose(fp) != 0) {
            diag("write error on fragment '%s'", frag);
            erc = THORN_RS_EXIT_IO;
        }
        if (erc != 0) {
            rc = erc;
            goto out_free;
        }
        diag("wrote %s (make) for %zu crate%s", frag, roots.count,
             roots.count == 1 ? "" : "s");
    }

out_free:
    for (size_t i = 0; i < roots.count; i++)
        rust_crate_free(&crates[i]);
    free(crates);
    free(ptrs);
out:
    strlist_free(&roots);
    strlist_free(&sf.cfgs);
    strlist_free(&sf.rustflags);
    strlist_free(&sf.externs);
    strlist_free(&sf.search_paths);
    return rc;
}

static int
cmd_run(int emit_mode, int argc, char **argv)
{
    RustCrate crate;
    rust_crate_init(&crate);
    int format = FMT_NONE;
    int rustc_given = 0;
    char rustc_flag[1024];
    rustc_flag[0] = '\0';

    int rc = parse_args(argc, argv, &crate, &format, &rustc_given,
                        rustc_flag, sizeof(rustc_flag));
    if (rc != 0) {
        rust_crate_free(&crate);
        return rc;
    }

    if (crate.frag[0] == '\0') {
        if (format == FMT_MAKE)
            snprintf(crate.frag, sizeof(crate.frag), "build-rs.mk");
        else
            snprintf(crate.frag, sizeof(crate.frag), "build-rs.ninja");
    }
    if (format == FMT_NONE) {
        format = infer_format(crate.frag);
        if (format == FMT_NONE) {
            if (emit_mode) {
                diag("cannot infer format from '%s'; pass --format "
                     "ninja|make", crate.frag);
                rust_crate_free(&crate);
                return THORN_RS_EXIT_USAGE;
            }
            /* plan never writes a fragment; the default above only
               fixes the artifact-path base. */
            format = FMT_NINJA;
        }
    }

    rc = scan_crate(crate.root, &crate);
    if (rc != SCAN_OK) {
        rust_crate_free(&crate);
        return rc == SCAN_IO ? THORN_RS_EXIT_IO : THORN_RS_EXIT_SCAN;
    }

    if (crate.out[0] == '\0') {
        char dir[1024];
        dir_name(crate.frag, dir, sizeof(dir));
        int w = (strcmp(dir, ".") == 0)
                    ? snprintf(crate.out, sizeof(crate.out), "lib%s.a",
                               crate.name)
                    : snprintf(crate.out, sizeof(crate.out), "%s/lib%s.a",
                               dir, crate.name);
        if (w < 0 || (size_t)w >= sizeof(crate.out)) {
            diag("artifact path for crate '%s' exceeds the path limit",
                 crate.name);
            rust_crate_free(&crate);
            return THORN_RS_EXIT_SCAN;
        }
    }
    if (!path_safe(crate.frag) || !path_safe(crate.out)) {
        diag("fragment/artifact paths contain characters that are "
             "unsafe in build fragments");
        rust_crate_free(&crate);
        return THORN_RS_EXIT_SCAN;
    }

    char tool[1024], resolved[4096];
    rc = plan_locate_rustc(rustc_given ? rustc_flag : NULL, tool,
                           sizeof(tool), resolved, sizeof(resolved));
    if (rc != 0) {
        rust_crate_free(&crate);
        return rc;
    }
    plan_probe_version(resolved);

    char cmd[4096];
    if (plan_command(&crate, tool, cmd, sizeof(cmd)) != 0) {
        diag("cannot plan the rustc command for crate '%s'", crate.name);
        rust_crate_free(&crate);
        return THORN_RS_EXIT_SCAN;
    }

    if (!emit_mode) {
        printf("%s\n", cmd);
        rust_crate_free(&crate);
        return THORN_RS_EXIT_OK;
    }

    if (ensure_parent_dir(crate.frag) != 0) {
        rust_crate_free(&crate);
        return THORN_RS_EXIT_IO;
    }
    FILE *fp = fopen(crate.frag, "w");
    if (fp == NULL) {
        diag("cannot write fragment '%s'", crate.frag);
        rust_crate_free(&crate);
        return THORN_RS_EXIT_IO;
    }
    int erc;
    if (format == FMT_NINJA) {
        const RustCrate *set[1] = { &crate };
        erc = emit_ninja_set(set, 1, tool, fp);
    } else {
        char fdir[1024];
        char ups[1024];
        dir_name(crate.frag, fdir, sizeof(fdir));
        compute_ups(fdir, ups, sizeof(ups));
        const RustCrate *set[1] = { &crate };
        erc = emit_make_set(set, 1, tool, ups, fp);
    }
    if (fclose(fp) != 0) {
        diag("write error on fragment '%s'", crate.frag);
        erc = THORN_RS_EXIT_IO;
    }
    if (erc != 0) {
        rust_crate_free(&crate);
        return erc;
    }
    diag("wrote %s (%s) for crate '%s' -> %s", crate.frag,
         format == FMT_NINJA ? "ninja" : "make", crate.name, crate.out);
    rust_crate_free(&crate);
    return THORN_RS_EXIT_OK;
}

int
main(int argc, char **argv)
{
    plan_scrub_env();

    if (argc < 2)
        return cmd_build(argc - 1, argv + 1);
    const char *cmd = argv[1];
    if (strcmp(cmd, "version") == 0) {
        printf("thorn-rs %s\n", THORN_RS_VERSION);
        return THORN_RS_EXIT_OK;
    }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "-h") == 0) {
        usage(stdout);
        return THORN_RS_EXIT_OK;
    }
    if (strcmp(cmd, "build") == 0)
        return cmd_build(argc - 2, argv + 2);
    if (strcmp(cmd, "emit") == 0)
        return cmd_run(1, argc - 2, argv + 2);
    if (strcmp(cmd, "plan") == 0)
        return cmd_run(0, argc - 2, argv + 2);
    if (strcmp(cmd, "check") == 0)
        return cmd_check(argc - 2, argv + 2);
    if (cmd[0] != '-')
        return cmd_build(argc - 1, argv + 1);

    diag("unknown command '%s'", cmd);
    usage(stderr);
    return THORN_RS_EXIT_USAGE;
}
