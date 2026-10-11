# thorn-rs

A native C99 companion tool for [Thorn](https://github.com/abit-foggy/thorn) that compiles standalone `#![no_std]` Rust static libraries directly via `rustc` and links them into C99 targets — without Cargo, wrappers, or external build systems. Powered by deterministic, byte-identical emission. Minimal, zero-bloat, in the Unix spirit.

[![CI](https://github.com/abit-foggy/thorn-rs/actions/workflows/ci.yml/badge.svg)](https://github.com/abit-foggy/thorn-rs/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-BSD--2--Clause-blue.svg)](LICENSE)

## What it does

`thorn-rs` scans a crate root (`lib.rs`), validates the `#![no_std]` contract
(`#[panic_handler]` must be in-tree), plans one deterministic `rustc`
invocation, and emits a standalone build fragment for either backend:

```ninja
# Ninja:  ninja -f build-rs.ninja
rule rustc
  command = $rustc --edition $edition --crate-name $crate_name --crate-type staticlib $rustflags --emit=dep-info=$out.d,link=$out $in
  depfile = $out.d
  deps = gcc
```

```make
# Make:   make -f build-rs.mk
RUSTC ?= rustc

out/libprobe.a: src/lib.rs
	$(RUSTC) --edition 2021 --crate-name probe --crate-type staticlib -C panic=abort -C opt-level=z --emit=dep-info=$@.d,link=$@ $<

-include out/libprobe.a.d
```

The resulting `lib<name>.a` is a plain C-ABI static archive — add it to any
C link line, including thorn-generated `build.ninja` / `Makefile` backends.

## As a thorn plugin

`make dist/thorn-rs.tpkg` packages the brain as a thorn plugin (a tar of
`plugin.o` + manifest). A project declares it and calls it from the
unified spec — one graph, one command:

```toml
# pith.toml
[deps]
thorn-rs = "../thorn-rs/dist/thorn-rs.tpkg"
```

```pith
# build.thorn
rust.crate("safe", "rust/safe/src/lib.rs", "include/safe.h")
engine.add_target("app", engine.exe)
engine.add_source("app", "src/main.c")
engine.add_source("app", "out/libsafe.a")
engine.add_order_dep("app", "out/.thorn-rs/safe.verified")
```

```sh
thorn pkg sync && thorn --out-dir out && ninja -f out/build.ninja
```

## C-ABI verification

The classic no_std failure is a silent ABI mismatch: a forgotten
`#[no_mangle]`, a typo'd export, a header function nobody implemented.
`thorn-rs check` audits the archive with subset semantics — every
expected symbol must exist, extra exports are informational:

```sh
thorn-rs check out/libprobe.a --header include/probe.h
```

With `--header`, `emit` also stamps a verification edge into the
fragment, so the audit runs as part of every backend build:

```ninja
rule rust_verify
  command = mkdir -p .thorn-rs && $thorn_rs check $in && touch $out
  description = VERIFY C-ABI probe

build .thorn-rs/probe.verified: rust_verify out/libprobe.a include/probe.h
```

The expectations come from two nets: the functions the crate declares
(`#[no_mangle] pub extern "C" fn`, `#[export_name]`) become `--expect`
flags, and every declaration in the contract header must be exported
by the built archive.

## Build

```sh
make            # cc -std=c99 -O2, zero dependencies beyond libc
make test       # runs tests/verify.sh
```

## Usage

```sh
thorn-rs [crate-root ...] [flags]     build (default root: src/lib.rs)
thorn-rs build [-f root ...] [-o dir] [-e ninja|make|both]
              [--rustc bin] [--header FILE] [crate flags]
thorn-rs plan  <crate-root.rs>        # print the planned rustc command
thorn-rs check <archive.a> [--expect SYM]... [--header FILE]
thorn-rs version | help
```

`build` mirrors thorn's CLI conventions: `-o/--out-dir` is the
destination directory (created if missing, default `.`), `-e/--engine`
selects `ninja`, `make`, or `both` (default `ninja`), and `--rustc`
overrides the compiler with thorn's precedence (flag > `RUSTC` env >
`PATH`). Multiple positional crate roots (or repeated `-f`) produce one
fragment with namespaced rules per crate and an aggregated `all`
target; duplicate crate names are refused. Subdirectory fragments
anchor their paths through `RS_ROOT`, so `make -C out/rs` and
`ninja -f out/rs/build-rs.ninja` produce bit-for-bit identical
archives. Like thorn, `build` is strictly a meta-build generator — it
never invokes ninja or make itself.

- `--out` is the fragment destination; when `--format` is omitted it is
  inferred from the extension (`.ninja` vs `.mk` / `Makefile`).
- The archive path defaults to `<fragment dir>/lib<name>.a`; override with
  `--artifact`.
- Toolchain resolution: `--rustc` flag → `RUSTC` env → `PATH`. Resolved
  absolute paths appear in stderr diagnostics only — never in emitted bytes.

### Exit codes

| Code | Meaning |
| ---- | ------- |
| 0    | success |
| 1    | scanner / validation / planning failure (fail fast, before `rustc`) |
| 2    | file I/O error |
| 64   | command-line misuse |

## Non-goals

- No Cargo execution, no `Cargo.toml`, no crates.io, no dependency resolution.
- Pure local source trees targeting `#![no_std]` / `core` / `alloc`.
- Static libraries (`.a`) with standard C-ABI linkage only.
- One Rust crate per C link (each staticlib bundles `core` +
  `compiler_builtins`).

## Links

- **Thorn (build system)**: [https://github.com/abit-foggy/thorn](https://github.com/abit-foggy/thorn)
- **Issue Tracker**: [https://github.com/abit-foggy/thorn-rs/issues](https://github.com/abit-foggy/thorn-rs/issues)

## License

BSD 2-Clause Simplified License. See [LICENSE](LICENSE) for details.

## Authorship

The majority of the code in this repository was written by an AI. All
architectural design was made by a human, and every change was
reviewed by both a human and an AI for flaws before it landed.
