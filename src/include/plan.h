#ifndef THORN_RS_PLAN_H
#define THORN_RS_PLAN_H

#include <stddef.h>
#include "rust.h"

/* Scrub nondeterminism sources from this process's environment:
   RUSTFLAGS, RUSTC_WRAPPER, and every CARGO_* variable are unset and
   never read (docs/SCOPING.md section 2.2). Call once from main. */
void plan_scrub_env(void);

/* Resolve the rustc tool: --rustc flag -> RUSTC env -> PATH lookup
   (thorn validate_executable semantics; no hardcoded absolute paths).
   On success fills `emit_name` with the portable tool reference to use
   in emitted fragments (flag/env value verbatim, else "rustc") and
   `resolved` (when non-NULL) with the absolute path found - the
   absolute path is diagnostics-only and never reaches emitted bytes.
   Returns 0 on success, THORN_RS_EXIT_SCAN on failure. */
int plan_locate_rustc(const char *flag_value, char *emit_name,
                      size_t esz, char *resolved, size_t rsz);

/* Best-effort `rustc --version` diagnostic probe; never fatal. */
void plan_probe_version(const char *resolved);

/* Build the $rustflags segment - everything the canonical invocation
   places between `--crate-type staticlib` and `--emit=` (docs/SCOPING.md
   section 3.2): panic strategy, opt level, debuginfo, target triple,
   --cfg, --extern, -L, and validated extra passes. Returns 0 on
   success, 1 on validation failure or truncation. */
int plan_rustflags(const RustCrate *c, char *buf, size_t n);

/* Build the complete deterministic rustc command line (the Make recipe
   body and the `plan` subcommand output). Returns 0 on success. */
int plan_command(const RustCrate *c, const char *tool, char *buf,
                 size_t n);

#endif /* THORN_RS_PLAN_H */
