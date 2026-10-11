# thorn-rs

Compiles small `#![no_std]` Rust libraries into C linkable archives.
No Cargo. A [Thorn](https://github.com/abit-foggy/thorn) plugin.

[![CI](https://github.com/abit-foggy/thorn-rs/actions/workflows/ci.yml/badge.svg)](https://github.com/abit-foggy/thorn-rs/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-BSD--2--Clause-blue.svg)](LICENSE)

## Quick start (with thorn)

Package thorn-rs once:

```sh
make dist/thorn-rs.tpkg
```

Declare it and call it from your spec:

```toml
# pith.toml
[deps]
thorn-rs = "../thorn-rs/dist/thorn-rs.tpkg"
```

```pith
# build.thorn
engine.project("demo")
engine.backend("ninja")

rust.crate("safe", "rust/safe/src/lib.rs", "include/safe.h")

engine.add_target("app", engine.exe)
engine.add_source("app", "src/main.c")
engine.add_source("app", "out/libsafe.a")
engine.add_order_dep("app", "out/.thorn-rs/safe.verified")
```

Build:

```sh
thorn pkg sync
thorn --out-dir out
ninja -f out/build.ninja
./out/app
```

`rust.crate(name, root, header)` scans the crate, plans one `rustc`
invocation, and adds it to thorn's graph. The header is your C-ABI
contract: if the built archive is missing a symbol the header declares,
the build fails right there.

## Without thorn

thorn-rs also works alone — it writes a `build-rs.ninja` (or
`build-rs.mk`) and ninja or make does the rest:

```sh
thorn-rs build rust/safe/src/lib.rs --out-dir out
ninja -f out/build-rs.ninja
```

## Checking an archive

```sh
thorn-rs check out/libsafe.a --header include/safe.h
```

Passes when every symbol the crate declares and every function the
header declares is exported by the archive. Extra exports are fine.

## The crate

No Cargo means the crate owns a few things itself:

```rust
#![no_std]                        // required
#![crate_name = "safe"]           // or pass --name

use core::panic::PanicInfo;
#[panic_handler]
fn ph(_: &PanicInfo) -> ! { loop {} }   // required: no Cargo to supply one

#[no_mangle]
pub extern "C" fn rust_add(a: i32, b: i32) -> i32 { a.wrapping_add(b) }
```

## Building this repo

```sh
make          # cc -std=c99, nothing else needed
make test     # tests/verify.sh
```

Exit codes: 0 ok, 1 bad input, 2 file I/O, 64 wrong command line.

## Non-goals

No Cargo, no crates.io, no dependency resolution. Local `#![no_std]`
sources only, C-ABI static archives only, one Rust crate per C link.

## Links

- **Thorn**: [https://github.com/abit-foggy/thorn](https://github.com/abit-foggy/thorn)
- **Issues**: [https://github.com/abit-foggy/thorn-rs/issues](https://github.com/abit-foggy/thorn-rs/issues)

## License

BSD 2-Clause Simplified License. See [LICENSE](LICENSE) for details.

## Authorship

The majority of the code in this repository was written by an AI. All
architectural design was made by a human, and every change was
reviewed by both a human and an AI for flaws before it landed.
