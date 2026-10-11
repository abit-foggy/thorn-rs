/* main.c - thorn-rs standalone CLI driver (Phase 0).
 *
 * Subcommands:
 *   thorn-rs emit  <crate-root> [flags]   write a ninja or make fragment
 *   thorn-rs plan  <crate-root> [flags]   print the planned rustc command
 *   thorn-rs version | help
 *
 * Ordering of the pipeline (docs/SCOPING.md section 2.2):
 *   scrub env -> parse flags -> scan crate -> resolve toolchain ->
 *   plan command -> emit fragment. Scanner failures exit before any
 *   fragment bytes are written; file I/O failures are a distinct class.
 *
 * Exit codes (README): 0 ok, 1 scan/plan, 2 I/O, 64 usage. */
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

enum { FMT_NONE = 0, FMT_NINJA = 1, FMT_MAKE = 2 };

static void usage(FILE *out)
{
    fprintf(out,
        "thorn-rs " THORN_RS_VERSION " - zero-cargo #![no_std] Rust "
        "static libraries for thorn\n\n"
        "USAGE:\n"
        "    thorn-rs emit <crate-root.rs> [flags]   write a build "
        "fragment\n"
        "    thorn-rs plan <crate-root.rs> [flags]   print the planned "
        "rustc command\n"
        "    thorn-rs version | help\n\n"
        "FLAGS:\n"
        "    --name NAME           crate name (default: "
        "#![crate_name] attribute)\n"
        "    --format ninja|make   fragment format (default: infer "
        "from --out)\n"
        "    --out FILE            fragment destination\n"
        "                          (default: build-rs.ninja / "
        "build-rs.mk)\n"
        "    --artifact PATH       archive path\n"
        "                          (default: <fragment dir>/lib<name>"
        ".a)\n"
        "    --edition EDITION     2015 | 2018 | 2021 | 2024 "
        "(default: 2021)\n"
        "    --target TRIPLE       rustc --target (default: host)\n"
        "    --opt-level LEVEL     0 | 1 | 2 | 3 | s | z\n"
        "    --panic STRATEGY      abort | unwind "
        "(default: abort for #![no_std])\n"
        "    --extern NAME=PATH    repeatable\n"
        "    -L DIR                repeatable dependency search path\n"
        "    --cfg SPEC            repeatable, e.g. 'feature=\"std\"'\n"
        "    --rustflag FLAG       repeatable extra rustc pass\n"
        "    --rustc BIN           rustc binary (default: RUSTC env, "
        "then PATH)\n\n"
        "EXIT CODES: 0 ok; 1 scan/plan failure; 2 file I/O; 64 usage\n");
}

static void bad_flag(const char *flag)
{
    diag("missing value for %s", flag);
}

static int bad_flag_ret(const char *flag)
{
    bad_flag(flag);
    return THORN_RS_EXIT_USAGE;
}

/* ------------------------------------------------------------------ */
/* Format helpers                                                      */
/* ------------------------------------------------------------------ */

static int ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

static int infer_format(const char *out)
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

static const char *dir_name(const char *p, char *buf, size_t n)
{
    const char *slash = strrchr(p, '/');
    if (!slash) {
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

/* mkdir -p semantics over the fragment's parent directory. */
static int ensure_parent_dir(const char *path)
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

/* ------------------------------------------------------------------ */
/* Flag parsing                                                        */
/* ------------------------------------------------------------------ */

static int set_opt_level(RustCrate *c, const char *v)
{
    if (strlen(v) != 1)
        return -1;
    switch (v[0]) {
    case '0': case '1': case '2': case '3': case 's': case 'z':
        c->opt_level = (int)v[0];
        return 0;
    default:
        return -1;
    }
}

static int set_edition(RustCrate *c, const char *v)
{
    if (strcmp(v, "2015") && strcmp(v, "2018") &&
        strcmp(v, "2021") && strcmp(v, "2024"))
        return -1;
    snprintf(c->edition, sizeof(c->edition), "%s", v);
    return 0;
}

/* Parses argv into the crate model and CLI-only fields.
 * Returns 0, THORN_RS_EXIT_SCAN (value validation), or
 * THORN_RS_EXIT_USAGE (malformed command line). */
static int parse_args(int argc, char **argv, RustCrate *c, int *format,
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
            if (!v)
                return bad_flag_ret(a);
            snprintf(c->name, sizeof(c->name), "%s", v);
            consumes = 1;
        } else if (strcmp(a, "--format") == 0) {
            if (!v)
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
            if (!v)
                return bad_flag_ret(a);
            snprintf(c->frag, sizeof(c->frag), "%s", v);
            consumes = 1;
        } else if (strcmp(a, "--artifact") == 0) {
            if (!v)
                return bad_flag_ret(a);
            snprintf(c->out, sizeof(c->out), "%s", v);
            consumes = 1;
        } else if (strcmp(a, "--edition") == 0) {
            if (!v)
                return bad_flag_ret(a);
            if (set_edition(c, v) != 0) {
                diag("unknown --edition '%s' (expected 2015, 2018, "
                     "2021, or 2024)", v);
                return THORN_RS_EXIT_USAGE;
            }
            consumes = 1;
        } else if (strcmp(a, "--target") == 0) {
            if (!v)
                return bad_flag_ret(a);
            snprintf(c->target, sizeof(c->target), "%s", v);
            consumes = 1;
        } else if (strcmp(a, "--opt-level") == 0) {
            if (!v)
                return bad_flag_ret(a);
            if (set_opt_level(c, v) != 0) {
                diag("unknown --opt-level '%s' (expected 0-3, s, or z)",
                     v);
                return THORN_RS_EXIT_USAGE;
            }
            consumes = 1;
        } else if (strcmp(a, "--panic") == 0) {
            if (!v)
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
        } else if (strcmp(a, "--extern") == 0) {
            if (!v)
                return bad_flag_ret(a);
            strlist_push(&c->externs, v);
            consumes = 1;
        } else if (strcmp(a, "-L") == 0 ||
                   strcmp(a, "--search-path") == 0) {
            if (!v)
                return bad_flag_ret(a);
            strlist_push(&c->search_paths, v);
            consumes = 1;
        } else if (strcmp(a, "--cfg") == 0) {
            if (!v)
                return bad_flag_ret(a);
            strlist_push(&c->cfgs, v);
            consumes = 1;
        } else if (strcmp(a, "--rustflag") == 0) {
            if (!v)
                return bad_flag_ret(a);
            strlist_push(&c->rustflags, v);
            consumes = 1;
        } else if (strcmp(a, "--rustc") == 0) {
            if (!v)
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
            /* stashed below via c->root scratch; scan re-copies it */
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

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static int cmd_run(int emit_mode, int argc, char **argv)
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

    /* Fragment destination: explicit, else per-format default. */
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
            /* plan never writes a fragment; the default derivation
               above only fixes the artifact-path base. */
            format = FMT_NINJA;
        }
    }

    /* 1. scan (fails fast before any toolchain work) */
    rc = scan_crate(crate.root, &crate);
    if (rc != SCAN_OK) {
        rust_crate_free(&crate);
        return rc == SCAN_IO ? THORN_RS_EXIT_IO : THORN_RS_EXIT_SCAN;
    }

    /* 2. artifact path: explicit, else <fragment dir>/lib<name>.a */
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

    /* 3. resolve the toolchain (diagnostics only; nothing emitted yet) */
    char tool[1024], resolved[4096];
    rc = plan_locate_rustc(rustc_given ? rustc_flag : NULL, tool,
                           sizeof(tool), resolved, sizeof(resolved));
    if (rc != 0) {
        rust_crate_free(&crate);
        return rc;
    }
    plan_probe_version(resolved);

    /* 4. plan */
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

    /* 5. emit */
    if (ensure_parent_dir(crate.frag) != 0) {
        rust_crate_free(&crate);
        return THORN_RS_EXIT_IO;
    }
    FILE *fp = fopen(crate.frag, "w");
    if (!fp) {
        diag("cannot write fragment '%s'", crate.frag);
        rust_crate_free(&crate);
        return THORN_RS_EXIT_IO;
    }
    int erc = (format == FMT_NINJA)
                  ? emit_ninja(&crate, tool, fp)
                  : emit_make(&crate, tool, fp);
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

int main(int argc, char **argv)
{
    plan_scrub_env();

    if (argc < 2) {
        usage(stderr);
        return THORN_RS_EXIT_USAGE;
    }
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
    if (strcmp(cmd, "emit") == 0)
        return cmd_run(1, argc - 2, argv + 2);
    if (strcmp(cmd, "plan") == 0)
        return cmd_run(0, argc - 2, argv + 2);

    diag("unknown command '%s'", cmd);
    usage(stderr);
    return THORN_RS_EXIT_USAGE;
}
