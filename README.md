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

## Build

```sh
make            # cc -std=c99 -O2, zero dependencies beyond libc
make test       # runs tests/verify.sh
```

## Usage

```sh
thorn-rs emit <crate-root.rs> [--name NAME] [--format ninja|make]
              [--out FILE] [--artifact PATH] [--edition 2015|2018|2021|2024]
              [--target TRIPLE] [--opt-level 0|1|2|3|s|z]
              [--panic abort|unwind] [--extern NAME=PATH] [-L DIR] [--cfg SPEC]
              [--rustflag FLAG] [--rustc BIN]
thorn-rs plan  <same flags>          # print the planned rustc command
thorn-rs version | help
```

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
