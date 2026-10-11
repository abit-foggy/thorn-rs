/*
 * plan.c - deterministic rustc command planner.
 *
 * The environment is scrubbed before it is read, and fragments
 * reference the tool by name only, so identical inputs yield
 * identical command bytes.
 *
 * Part of thorn-rs, the zero-cargo Rust staticlib companion for thorn.
 * Copyright (c) 2026 abit-foggy
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "include/plan.h"

extern char **environ;

void
plan_scrub_env(void)
{
    static const char *const exact[] = {
        "RUSTFLAGS", "RUSTC_WRAPPER", NULL
    };
    for (size_t i = 0; exact[i]; i++)
        unsetenv(exact[i]);

    /* Snapshot index: unsetenv shifts environ. */
    for (size_t i = 0; environ && environ[i]; i++) {
        char *eq = strchr(environ[i], '=');
        if (eq == NULL)
            continue;
        size_t nlen = (size_t)(eq - environ[i]);
        if (nlen > 6 && strncmp(environ[i], "CARGO_", 6) == 0) {
            char name[128];
            if (nlen < sizeof(name)) {
                memcpy(name, environ[i], nlen);
                name[nlen] = '\0';
                unsetenv(name);
                i = (size_t)-1;
            }
        }
    }
}

/* thorn validate_executable semantics: paths with '/' are checked
   directly; bare names are searched over PATH. */
static int
executable_ok(const char *cand)
{
    if (cand == NULL || *cand == '\0')
        return 0;
    if (strchr(cand, '/'))
        return access(cand, X_OK) == 0;
    const char *path_env = getenv("PATH");
    if (path_env == NULL)
        path_env = "/usr/bin:/bin";
    char *copy = strdup(path_env);
    if (copy == NULL)
        return 0;
    const char *saveptr = NULL;
    int found = 0;
    for (char *dir = strtok_r(copy, ":", (char **)&saveptr); dir;
         dir = strtok_r(NULL, ":", (char **)&saveptr)) {
        char cand_path[4096];
        snprintf(cand_path, sizeof(cand_path), "%s/%s", dir, cand);
        if (access(cand_path, X_OK) == 0) {
            found = 1;
            break;
        }
    }
    free(copy);
    return found;
}

static int
find_in_path(const char *name, char *out, size_t n)
{
    const char *path_env = getenv("PATH");
    if (path_env == NULL)
        path_env = "/usr/bin:/bin";
    char *copy = strdup(path_env);
    if (copy == NULL)
        return 0;
    const char *saveptr = NULL;
    int found = 0;
    for (char *dir = strtok_r(copy, ":", (char **)&saveptr); dir;
         dir = strtok_r(NULL, ":", (char **)&saveptr)) {
        char cand[4096];
        snprintf(cand, sizeof(cand), "%s/%s", dir, name);
        if (access(cand, X_OK) == 0) {
            snprintf(out, n, "%s", cand);
            found = 1;
            break;
        }
    }
    free(copy);
    return found;
}

int
plan_locate_rustc(const char *flag_value, char *emit_name, size_t esz,
                  char *resolved, size_t rsz)
{
    const char *cand = (flag_value && *flag_value) ? flag_value
                                                   : getenv("RUSTC");
    if (cand && *cand) {
        if (!executable_ok(cand)) {
            diag("rustc '%s' is not executable (from %s)", cand,
                 (flag_value && *flag_value) ? "--rustc" : "RUSTC env");
            return THORN_RS_EXIT_SCAN;
        }
        snprintf(emit_name, esz, "%s", cand);
        if (resolved && rsz)
            snprintf(resolved, rsz, "%s", cand);
        return 0;
    }

    char found[4096];
    if (!find_in_path("rustc", found, sizeof(found))) {
        diag("rustc not found in PATH; install Rust or set "
             "RUSTC/--rustc");
        return THORN_RS_EXIT_SCAN;
    }
    snprintf(emit_name, esz, "rustc");
    if (resolved && rsz)
        snprintf(resolved, rsz, "%s", found);
    return 0;
}

void
plan_probe_version(const char *resolved)
{
    if (resolved == NULL || *resolved == '\0')
        return;
    char cmd[4200];
    snprintf(cmd, sizeof(cmd), "%s --version 2>/dev/null", resolved);
    FILE *pp = popen(cmd, "r");
    if (pp == NULL)
        return;
    char line[256];
    if (fgets(line, sizeof(line), pp)) {
        line[strcspn(line, "\r\n")] = '\0';
        diag("using %s (%s)", resolved, line);
    }
    pclose(pp);
}

/* Bounded append; 0 on success, 1 on truncation. */
static int
seg(char *buf, size_t n, const char *fmt, ...)
{
    size_t len = strlen(buf);
    va_list ap;

    va_start(ap, fmt);
    int written = vsnprintf(buf + len, n - len, fmt, ap);
    va_end(ap);
    return (written < 0 || (size_t)written >= n - len) ? 1 : 0;
}

int
plan_rustflags(const RustCrate *c, char *buf, size_t n)
{
    buf[0] = '\0';
    if (c->panic_abort == 1 && seg(buf, n, " -C panic=abort"))
        return 1;
    if (c->opt_level != RUST_OPT_UNSET &&
        seg(buf, n, " -C opt-level=%c", (char)c->opt_level))
        return 1;
    if (c->debuginfo >= 0 && seg(buf, n, " -C debuginfo=%d", c->debuginfo))
        return 1;
    if (c->target[0] && seg(buf, n, " --target %s", c->target))
        return 1;
    for (size_t i = 0; i < c->cfgs.count; i++) {
        if (strchr(c->cfgs.items[i], '\'')) {
            diag("cfg spec contains a single quote: '%s'",
                 c->cfgs.items[i]);
            return 1;
        }
        if (seg(buf, n, " --cfg '%s'", c->cfgs.items[i]))
            return 1;
    }
    for (size_t i = 0; i < c->externs.count; i++) {
        const char *e = c->externs.items[i];
        const char *eq = strchr(e, '=');
        if (eq == NULL || eq == e || eq[1] == '\0' || has_whitespace(e)) {
            diag("--extern expects NAME=PATH without whitespace: '%s'",
                 e);
            return 1;
        }
        if (seg(buf, n, " --extern %s", e))
            return 1;
    }
    for (size_t i = 0; i < c->search_paths.count; i++) {
        if (has_whitespace(c->search_paths.items[i])) {
            diag("-L path contains whitespace: '%s'",
                 c->search_paths.items[i]);
            return 1;
        }
        if (seg(buf, n, " -L %s", c->search_paths.items[i]))
            return 1;
    }
    for (size_t i = 0; i < c->rustflags.count; i++) {
        const char *f = c->rustflags.items[i];
        if (f[0] != '-' || has_whitespace(f)) {
            diag("--rustflag must start with '-' and contain no "
                 "whitespace: '%s'", f);
            return 1;
        }
        if (seg(buf, n, " %s", f))
            return 1;
    }
    return 0;
}

int
plan_command(const RustCrate *c, const char *tool, char *buf, size_t n)
{
    char flags[2048];

    if (plan_rustflags(c, flags, sizeof(flags)))
        return 1;
    buf[0] = '\0';
    if (seg(buf, n,
            "%s --edition %s --crate-name %s --crate-type staticlib%s "
            "--emit=dep-info=%s.d,link=%s %s",
            tool, c->edition, c->name, flags,
            c->out, c->out, c->root))
        return 1;
    return 0;
}
