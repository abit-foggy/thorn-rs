/*
 * verify.h - C-ABI archive verification.
 *
 * Part of thorn-rs, the zero-cargo Rust staticlib companion for thorn.
 * Copyright (c) 2026 abit-foggy
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef THORN_RS_VERIFY_H
#define THORN_RS_VERIFY_H

#include "rust.h"

/*
 * Audit `archive` against the expected C-ABI surface: `expected`
 * symbols plus every function declaration found in `header` (when
 * non-NULL).  Subset semantics - expected symbols must exist, extra
 * exports are informational only.  Returns 0 verified,
 * THORN_RS_EXIT_SCAN on violations, THORN_RS_EXIT_IO on file errors.
 */
int verify_archive(const char *archive, const StrList *expected,
                   const char *header);

/* The check command for `archive` without the binary name (the
   emitters prepend their own tool reference).  0 or 1 on truncation. */
int verify_command(const char *archive, const char *header,
                   const StrList *expected, char *buf, size_t n);

/* The expected C-ABI symbol set: `expected` plus every declaration in
   `header`.  0 or THORN_RS_EXIT_IO. */
int verify_expected(const char *header, const StrList *extra,
                    StrList *out);

/* A binary-free audit edge for build graphs: a POSIX sh loop that
   fails unless every expected symbol is defined in `archive`.  0 or 1
   on truncation. */
int verify_edge_command(const char *archive, const char *header,
                        const StrList *expected, char *buf, size_t n);

#endif /* THORN_RS_VERIFY_H */
