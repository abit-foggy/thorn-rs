#ifndef THORN_RS_SCAN_H
#define THORN_RS_SCAN_H

#include "rust.h"

/* Scan result codes (map 1:1 onto thorn-rs exit classes). */
enum {
    SCAN_OK       = 0,
    SCAN_INVALID  = 1,  /* rejected content: whitespace, missing handler, ... */
    SCAN_IO       = 2   /* cannot read the crate root                         */
};

#define SCAN_MAX_BYTES (1u << 20) /* 1 MiB cap on crate roots */

/* Inspect a crate root and fill `out`:
 *   - rejects whitespace paths (thorn's has_whitespace invariant)
 *   - detects #![no_std] and #[panic_handler] through a comment- and
 *     string-aware normalizer (contents blanked, structure kept)
 *   - extracts #![crate_name = "..."] when `out->name` is empty
 *   - applies no_std defaults: panic_abort = 1, opt_level = 'z'
 *   - fails fast when #![no_std] lacks an in-tree #[panic_handler]
 * Flags parsed by the CLI are applied BEFORE the scan, so the scanner
 * only fills gaps and never overrides explicit user intent. */
int scan_crate(const char *path, RustCrate *out);

#endif /* THORN_RS_SCAN_H */
