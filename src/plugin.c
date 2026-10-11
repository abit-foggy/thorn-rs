/*
 * plugin.c - the thorn-rs plugin entry point.
 *
 * Compiled as one unity unit (rust.c, scan.c, plan.c and verify.c are
 * included below; the Makefile renames their shared helpers to rs_*)
 * so dist/plugin.o carries the whole brain.  The exports use pith's
 * plugin mangling c_<author>_<module>_<fn> and the bundled manifest
 * (`module = "rust"` + `fn` lines) is the registration: thorn builds
 * an import unit from it and the spec calls rust.crate / rust.check.
 * The engine symbols this unit calls are bound by the host (in-memory)
 * or thorn's own engine.o link object (temp-executable fallback).
 *
 * Part of thorn-rs, the zero-cargo Rust staticlib companion for thorn.
 * Copyright (c) 2026 abit-foggy
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pith.h>

#include "include/rust.h"
#include "include/scan.h"
#include "include/plan.h"
#include "include/verify.h"

/* Engine symbols bound by the thorn host (see thorn src/main.c). */
extern int c_engine_add_command(PithValue *output, PithValue *command,
                                PithValue *input);

/* rust.crate(name, root, header)
 *
 * Scans the crate root, plans the deterministic rustc invocation, and
 * lowers two graph edges: the build edge producing <out-dir>/lib<name>.a
 * and, with a header contract, the C-ABI audit edge stamping
 * .thorn-rs/<name>.verified.  Returns 1 on success; 0 fails the spec
 * (the engine epilogue then aborts the run).
 *
 * The export name is the pith plugin mangling c_<author>_<module>_<fn>
 * for author "abit-foggy", module "rust"; the manifest's fn table is
 * what makes the spec-facing name rust.crate. */
int
c_abit_foggy_rust_crate(PithValue *name, PithValue *root,
                        PithValue *header)
{
    if (name == NULL || root == NULL ||
        pithStringLength(name) == 0 || pithStringLength(root) == 0) {
        diag("rust.crate() expects a crate name and root");
        return 0;
    }

    RustCrate c;
    rust_crate_init(&c);
    snprintf(c.name, sizeof(c.name), "%s", pithStringData(name));
    if (header && pithStringLength(header) > 0 &&
        pithStringLength(header) < (uint32_t)sizeof(c.header))
        snprintf(c.header, sizeof(c.header), "%s", pithStringData(header));

    int rc = scan_crate(pithStringData(root), &c);
    if (rc != SCAN_OK)
        return 0;   /* diagnostics already emitted; fail the spec */

    /* The command writes the archive relative to the build CWD; the
       graph edge carries the bare name (thorn prefixes the out-dir).
       The audit edge consumes the out-dir-prefixed path - the same
       string the spec's add_source uses (SCOPING.md section 3.3). */
    const char *od = getenv("THORN_OUT_DIR");
    if (od == NULL || *od == '\0' || strcmp(od, ".") == 0)
        od = "";
    char ar_cwd[1200];
    int w = (od[0] == '\0')
                ? snprintf(ar_cwd, sizeof(ar_cwd), "lib%s.a", c.name)
                : snprintf(ar_cwd, sizeof(ar_cwd), "%s/lib%s.a", od,
                           c.name);
    if (w < 0 || (size_t)w >= sizeof(ar_cwd)) {
        diag("artifact path for crate '%s' exceeds the path limit",
             c.name);
        return 0;
    }
    w = snprintf(c.out, sizeof(c.out), "%s", ar_cwd);
    if (w < 0 || (size_t)w >= sizeof(c.out)) {
        diag("artifact path for crate '%s' exceeds the path limit",
             c.name);
        return 0;
    }

    char cmd[1024];
    if (plan_command(&c, "rustc", cmd, sizeof(cmd)) != 0) {
        diag("cannot plan the rustc command for crate '%s'", c.name);
        return 0;
    }

    char edge_out[256];
    snprintf(edge_out, sizeof(edge_out), "lib%s.a", c.name);
    PithValue *vo = pithNewString(edge_out);
    PithValue *vc = pithNewString(cmd);
    PithValue *vi = pithNewString(c.root);
    int ok = c_engine_add_command(vo, vc, vi);
    pithRelease(vo);
    pithRelease(vc);
    pithRelease(vi);
    if (!ok) {
        diag("the engine rejected the build edge for '%s'", c.name);
        return 0;
    }

    if (c.header[0]) {
        StrList want = {0};
        char vcmd[1024];
        if (verify_expected(c.header, &c.capi_syms, &want) != 0 ||
            verify_edge_command(ar_cwd, NULL, &want, vcmd,
                                sizeof(vcmd)) != 0) {
            strlist_free(&want);
            diag("cannot plan the audit edge for crate '%s'", c.name);
            return 0;
        }
        strlist_free(&want);

        char stamp[1024];
        int sw = (od[0] == '\0')
            ? snprintf(stamp, sizeof(stamp),
                       "mkdir -p .thorn-rs && %s && touch "
                       ".thorn-rs/%s.verified", vcmd, c.name)
            : snprintf(stamp, sizeof(stamp),
                       "mkdir -p %s/.thorn-rs && %s && touch "
                       "%s/.thorn-rs/%s.verified", od, vcmd, od, c.name);
        char stamp_edge[256];
        int ew = snprintf(stamp_edge, sizeof(stamp_edge),
                 ".thorn-rs/%s.verified", c.name);
        if (sw < 0 || (size_t)sw >= sizeof(stamp) || ew < 0 ||
            (size_t)ew >= sizeof(stamp_edge)) {
            diag("audit edge for crate '%s' exceeds the command limit",
                 c.name);
            return 0;
        }

        vo = pithNewString(stamp_edge);
        vc = pithNewString(stamp);
        vi = pithNewString(ar_cwd);
        ok = c_engine_add_command(vo, vc, vi);
        pithRelease(vo);
        pithRelease(vc);
        pithRelease(vi);
        if (!ok) {
            diag("the engine rejected the audit edge for '%s'", c.name);
            return 0;
        }
    }

    diag("rust: planned crate '%s' -> %s", c.name, ar_cwd);
    return 1;
}

/* rust.check(archive, header)
 *
 * Audits a built archive: every symbol the crate declares and every
 * declaration in the contract header must be exported. */
int
c_abit_foggy_rust_check(PithValue *archive, PithValue *header)
{
    if (archive == NULL || pithStringLength(archive) == 0) {
        diag("rust.check() expects an archive path");
        return 0;
    }
    const char *h = (header && pithStringLength(header) > 0)
                        ? pithStringData(header)
                        : NULL;
    return verify_archive(pithStringData(archive), NULL, h) == 0;
}

/* The brain, as one translation unit. */
#include "rust.c"
#include "scan.c"
#include "plan.c"
#include "verify.c"
