#ifndef THORN_RS_EMIT_H
#define THORN_RS_EMIT_H

#include <stdio.h>
#include "rust.h"

/* Fragment emitters (docs/SCOPING.md section 3.4). Both write
 * deterministic bytes: identical crate fields and tool reference yield
 * identical fragments. Paths are validated against the fragment-safe
 * character set before anything is written.
 *
 * Returns 0 on success, THORN_RS_EXIT_SCAN on path validation or
 * planning failure (the caller maps FILE* failures to EXIT_IO). */
int emit_ninja(const RustCrate *c, const char *tool, FILE *out);
int emit_make(const RustCrate *c, const char *tool, FILE *out);

#endif /* THORN_RS_EMIT_H */
