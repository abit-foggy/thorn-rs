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
 * Both write deterministic bytes: identical crate fields and tool
 * reference yield identical fragments.  Paths are validated before
 * anything is written.  Returns 0 or THORN_RS_EXIT_SCAN (FILE*
 * failures map to THORN_RS_EXIT_IO in the caller).
 */
int emit_ninja(const RustCrate *c, const char *tool, FILE *out);
int emit_make(const RustCrate *c, const char *tool, FILE *out);

#endif /* THORN_RS_EMIT_H */
