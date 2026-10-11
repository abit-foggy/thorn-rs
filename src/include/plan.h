/*
 * plan.h - deterministic rustc command planning.
 *
 * Part of thorn-rs, the zero-cargo Rust staticlib companion for thorn.
 * Copyright (c) 2026 abit-foggy
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef THORN_RS_PLAN_H
#define THORN_RS_PLAN_H

#include <stddef.h>

#include "rust.h"

/* Unset RUSTFLAGS, RUSTC_WRAPPER, and every CARGO_* variable. */
void plan_scrub_env(void);

/*
 * Resolve the tool: --rustc flag -> RUSTC env -> PATH (thorn's
 * validate_executable semantics).  `emit_name` gets the portable
 * reference for fragments (flag/env value verbatim, else "rustc");
 * `resolved` (optional) gets the absolute path, for diagnostics only.
 * Returns 0 or THORN_RS_EXIT_SCAN.
 */
int plan_locate_rustc(const char *flag_value, char *emit_name,
                      size_t esz, char *resolved, size_t rsz);

/* Best-effort `rustc --version` diagnostic; never fatal. */
void plan_probe_version(const char *resolved);

/* The segment between --crate-type staticlib and --emit=.  0 or 1. */
int plan_rustflags(const RustCrate *c, char *buf, size_t n);

/* The complete rustc command line.  0 or 1. */
int plan_command(const RustCrate *c, const char *tool, char *buf,
                 size_t n);

#endif /* THORN_RS_PLAN_H */
