/*
 * scan.h - crate root inspection.
 *
 * Part of thorn-rs, the zero-cargo Rust staticlib companion for thorn.
 * Copyright (c) 2026 abit-foggy
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef THORN_RS_SCAN_H
#define THORN_RS_SCAN_H

#include "rust.h"

enum {
    SCAN_OK      = 0,
    SCAN_INVALID = 1,  /* rejected content */
    SCAN_IO      = 2   /* cannot read      */
};

#define SCAN_MAX_BYTES (1u << 20)   /* 1 MiB cap on crate roots        */
#define SCAN_MAX_MODULES 64         /* max sibling modules to scan     */
#define SCAN_MAX_MODULE_BYTES (4u << 20) /* 4 MiB cap on module bytes   */

/*
 * Inspect a crate root and fill `out`.  Fills the gaps the CLI left
 * (crate name, no_std defaults: panic=abort, opt-level=z) and fails
 * fast when #![no_std] has no in-tree #[panic_handler].  Explicit CLI
 * flags always win over scanner defaults.
 */
int scan_crate(const char *path, RustCrate *out);

#endif /* THORN_RS_SCAN_H */
