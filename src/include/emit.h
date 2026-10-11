/*
 * emit.h - fragment emitters.
 *
 * Part of thorn-rs, the zero-cargo Rust staticlib companion for thorn.
 * Copyright (c) 2026 abit-foggy
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef THORN_RS_EMIT_H
#define THORN_RS_EMIT_H

#include <stdio.h>

#include "rust.h"

/*
 * Both emitters write one fragment covering a set of crates, with
 * namespaced rules (rustc_<name>) so multi-crate fragments never
 * collide.  Both write deterministic bytes: identical crate fields and
 * tool reference yield identical fragments.  Paths are validated
 * before anything is written.  Returns 0 or THORN_RS_EXIT_SCAN (FILE*
 * failures map to THORN_RS_EXIT_IO in the caller).
 *
 * `ups` is the relative path from the fragment's directory back to the
 * emission working directory ("" when the fragment sits in it).  The
 * ninja fragment stays working-directory-relative; the make fragment
 * anchors itself through RS_ROOT so `make -C <out-dir>` works.
 */
int emit_ninja_set(const RustCrate *const *crates, size_t count,
                   const char *tool, FILE *out);
int emit_make_set(const RustCrate *const *crates, size_t count,
                  const char *tool, const char *ups, FILE *out);

#endif /* THORN_RS_EMIT_H */
