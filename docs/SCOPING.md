# thorn-rs — Technical Scoping & Architecture Specification

**Status:** scoping draft (no implementation)
**Date:** 2026-10-10
**Model:** companion tool in the `thornk` pattern — native C99, Pith-hosted, deterministic, zero-cargo
**Target:** enable Thorn to compile standalone `#![no_std]` Rust static libraries via `rustc` and link them into C99 targets without Cargo or external build wrappers

> **Phase 0 implementation note.** Phase 0 ships the CLI as a standalone
> C99 entry point (`src/main.c`), one of the two driver options the Phase 0
> task permits. The Pith driver (`src/main.pi`) remains the Phase 1 shape
> for the moment `rust.c` is absorbed into thorn's host API
> (`engine.rust_crate`, §4 Option B); the `vendor/` submodule pins are kept
> from day one as Phase 1 prerequisites. Everything else in this document
> — the crate model, the rustc contract, the lowering, the risks — is
> normative for Phase 0 as written.

All statements in this document were verified against `thorn` @ `v1.0`
(`a303258`), `thornk` @ `main` (`31155cb`), `pith` @ `v0.2.0` and
`rustc 1.98.1 (48a229cea 2026-09-01)`.

---

## 1. Current State & ABI Touchpoints

### 1.1 Thorn's in-memory model (`src/include/engine.h`)

The entire companion-tool ABI lives in one header. These are the exact
structures a tool like `thorn-rs` must populate or lower to:

```c
/* Target types */
enum { THORN_EXE = 1, THORN_STATIC_LIB = 2, THORN_SHARED_LIB = 3 };

typedef enum SourceKind { SRC_C = 0, SRC_ASM_RAW, SRC_ASM_CPP } SourceKind;

typedef struct StrList { char **items; size_t count; size_t cap; } StrList;

typedef struct Target {
    char name[256];
    int type;
    StrList sources;        /* file paths */
    SourceKind *source_kinds;
    StrList cflags, asflags, ldflags, includes, order_deps;
} Target;

typedef struct Command {          /* one custom build edge */
    char output[256];
    char command[1024];           /* raw shell command line */
    char input[256];
} Command;

typedef struct Graph {
    char proj[256];
    char backend[32];             /* "ninja" | "make" */
    Target *targets;  size_t count, cap;
    Command *commands; size_t cmd_count, cmd_cap;
} Graph;
```

Key helpers (`src/include/graph.h`, implemented in `src/graph.c`):

| Function | Role for thorn-rs |
|---|---|
| `classify_source` / `is_buildable_source` | Only `.c` and `.s`/`.S` are "buildable". **`.a`, `.o`, `.so` are passed through verbatim** — this is the load-bearing behavior that lets a Rust archive flow into a C link line untouched. |
| `obj_name` / `artifact_name` / `out_name` | Deterministic path derivation; `out_name` applies the backend directory prefix. |
| `graph_add_command(g, out, cmd, in)` | Registers a custom edge — the exact hook a `rustc` invocation lowers to. |
| `has_whitespace` | Thorn already rejects whitespace paths; Rust crate roots must obey the same rule (depfile compatibility, §5.4). |

### 1.2 The two Thorn API surfaces

Thorn exposes **two distinct integration surfaces**; `thornk` uses both.

**A. `engine.*` host namespace** — registered by `src/main.c::host_register()`
via `pith_register_ns_fn(ctx, "engine", name, fn, 'w', params)`. This is the
surface a `build.thorn` spec evaluates against, in-process, inside the `thorn`
binary:

```
project(p)  backend(p)  exe()  static_lib()  shared_lib()
add_target(name, type)         # "pw"
add_source / add_cflag / add_asflag / add_ldflag / add_include /
add_order_dep                  # "pp"
add_command(out, cmd, in)      # "ppp"
pkg_config(target, pkg)        # "pp"
emit()                         # "" -> writes build.ninja / Makefile
```

Implementation: `src/engine.c` keeps a `static Graph g_graph` behind
`ensure_graph()`; `want_target()` validates names with
`unknown target ... (declare it with add_target first)` diagnostics. The
epilogue every spec ends with (`EMISSION_EPILOGUE`, `main.c`):

```
if engine.emit() == 0
    proc.exit(1)
end
```

**B. `decompile.*` namespace** — created by pith's C-FFI import when a Pith
script does `import "vendor/thorn/src/decompile.c"`. pith derives the
namespace from the **basename sans extension** (`src/cffi.c`, "namespace:
basename sans extension") and compiles the translation unit into the running
program (JIT) or the AOT artifact. Exposes the full decompiler/host API
(`reset, set_project, set_compiler, set_ar, ignore_target, keep_target,
strip_cflag, inject_cflag, inject_include, remap_target, add_command_edge,
parse_file, parse_string, target_count, get_target_*, set_target_type,
add_target_source, remove_target, emit_ninja_file, to_ninja, to_thorn,
convert, ...`).

> **Design consequence for thorn-rs:** a C file named `rust.c` imported by a
> spec automatically becomes the `rust.*` namespace. This is the zero-thorn-
> change integration path (§4, Phase 1 option A).

### 1.3 Serialization formats

**`build.thorn` spec language** (emitted by `src/spec.c::print_spec`, consumed
by the `engine.*` API):

```
engine.project("name")
engine.backend("ninja")
engine.add_target("foo", engine.exe)          # engine.exe|engine.static_lib|engine.shared_lib
engine.add_source("foo", "src/main.c")
engine.add_include("foo", "include")
engine.add_cflag("foo", "-O2")
engine.add_ldflag("foo", "-lm")
engine.add_order_dep("foo", "generated.h")
engine.add_command("out/gen.h", "tool -o out/gen.h", "schema")
```

**`build.ninja`** (emitted by `src/ninja.c::emit_ninja`):

```
ninja_required_version = 1.3
thorn_project = <proj>
cc = <cc>          # from --compiler / CC env / "cc", PATH-validated
as = <as|env AS>
ar = <ar>

rule cc     { command = $cc $cflags -MMD -MF $out.d -c $in -o $out
              depfile = $out.d   deps = gcc }
rule as     { command = $as $asflags -c $in -o $out }
rule as_cpp { command = $cc -MD -MF $out.d $asflags $cflags -c $in -o $out
              depfile = $out.d   deps = gcc }
rule link   { command = $cc $in $ldflags -o $out }
rule solink { command = $cc -shared $in $ldflags -o $out }
rule ar     { command = rm -f $out && $ar rcs $out $in }

# per graph_add_command edge:
rule cmd_<N> { command = <raw command> }
build <pfx><output>: cmd_<N> [<input>]

# per target: object edges (buildable sources only, order-deps as "||"),
# then the artifact edge:
build <pfx><artifact>: link <obj...> <verbatim-non-buildable...> | <order-deps...>
build <pfx>thorn_all: phony <all artifacts>     # + "default"
```

Critical ABI subtleties for a Rust lowering:

1. **Non-buildable sources are link inputs verbatim** (`ninja.c` writes the
   raw string, no object derivation, no prefix). A `lib<name>.a` added via
   `engine.add_source(c_target, "out/libfoo_rs.a")` lands directly on the
   `link`/`solink` command line.
2. **Custom command outputs are prefix-rewritten** through
   `out_name(cout, pfx, c->output)` where `pfx = dir_prefix(ninja_path)`, but
   non-buildable link inputs are **not** rewritten. The producer path and the
   consumer path must therefore be **byte-identical strings** chosen by the
   caller. thorn-rs must emit one canonical path for both (§3.3).
3. **Order deps** appear as `||` on object edges and `|` on the artifact edge
   — the right vehicle for "build the Rust archive before linking" if the
   archive is *not* a link input (e.g., header-only FFI bindings).
4. The **Makefile backend supports custom commands too** (`src/make.c` emits
   them after `subst_cmd_for_make()` escaping; `make_uses_pattern()` is only
   a pattern-rule optimization gate, not a constraint). Rust lowering works
   on both backends for free.

### 1.4 How `thornk` hooks in today (the model to mirror)

`thornk` is a **separate repository** that vendors `thorn` as a git submodule
pinned to the release tag (`vendor/thorn @ v1.0`) and builds a **standalone
AOT binary with pith**:

```make
# thornk/Makefile — the entire build
build:
	pith build src/kconfig.pi src/composite.pi src/walker.pi src/main.pi -o bin/thornk
```

Control flow (`thornk/src/main.pi`):

1. `import "vendor/thorn/src/decompile.c"` → in-process `decompile.*`
   namespace (pattern B above).
2. Pure-Pith scanner walks the kernel tree (`walker.pi`, `composite.pi`,
   `scan.c`, `resolve.c`), calling `decompile.reset/set_project/set_compiler/
   set_ar/ignore_target/strip_cflag/inject_cflag/inject_include/
   set_target_type/add_target_source`.
3. `decompile.to_thorn()` → writes `thorn.build` spec text.
4. **Subprocess hand-off** (`thornk.c` → `post.c::post_run_thorn`): locates
   the `thorn` binary (`THORN_BIN` env → hardcoded `/home/foggy/...`
   fallbacks → `PATH`), sets `PITH_TCCDIR` for the vendored tcc fallback, then
   `system("thorn -f <spec> --engine ninja --out-dir <dir>")`.
5. Post-processes the generated `build.ninja` in-place for vmlinux edges
   (`post_vmlinux`).

**Where thorn-rs mirrors the pattern:** separate repo, submodule pins,
`pith build … -o bin/thorn-rs`, Pith CLI driver, C scanner/planner exposed
through an imported namespace, deterministic text emission.

**Where thorn-rs must diverge (thornk warts):**

| thornk wart | thorn-rs policy |
|---|---|
| Hardcoded `/home/foggy/...` fallback paths for thorn discovery | `RUSTC` env → `--rustc` flag → `PATH` lookup via thorn's `validate_executable()` precedence; **no absolute paths, ever** |
| Post-hoc rewriting of generated `build.ninja` | Never rewrite backend output; the plan must be complete *before* emission |
| `system()` with ad-hoc quoting | One quoted-argv construction site in the planner; the emitted command is a single deterministic string |
| Spec text assembled in Pith strings | Spec/fragment emission in C (like `spec.c::print_spec`), unit-testable |

---

## 2. `thorn-rs` Component Architecture

### 2.1 Proposed file layout

```
thorn-rs/
├── README.md  LICENSE (BSD-2)  SECURITY.md   # family conventions
├── install.sh  run.sh                        # mirror thorn/thornk installers
├── Makefile                                  # pith build src/*.pi -o bin/thorn-rs
├── build.thorn                               # thorn-rs self-hosts its C harness
├── vendor/
│   ├── thorn/                                # submodule, pin v1.0
│   └── pith/                                 # submodule, pin v0.2.0+
├── src/
│   ├── main.pi                  # CLI driver (Pith): arg parsing, dispatch
│   ├── include/
│   │   ├── rust.h               # RustCrate model + planner API (§3.1)
│   │   ├── scan.h               # crate inspector API
│   │   ├── plan.h               # rustc argv planner API
│   │   └── verify.h             # Phase-2 symbol verification API
│   ├── rust.c                   # TOP-LEVEL import unit: rust.* namespace
│   ├── scan.c                   # crate inspector (lib.rs attribute scanner)
│   ├── plan.c                   # deterministic rustc argv planner + toolchain probe
│   └── verify.c                 # nm-based export verification (Phase 2)
└── tests/
    ├── verify.sh                # acceptance suite (thorn-style)
    └── fixtures/
        ├── probe/               # minimal no_std crate: panic_handler + extern "C"
        ├── alloc/               # alloc crate with #[global_allocator]
        └── missing-ph/          # negative: no_std without panic_handler
```

`rust.c` is the **only** file a spec or thorn ever needs to import; it
`#include`s the others. It must compile with `-std=c99 -Wall -Wextra`, no
thorn-internal includes beyond a relative `engine.h` include path (so both
standalone import and future in-thorn compilation work).

### 2.2 Component breakdown

**CLI driver (`main.pi`, Pith)** — mirrors `thornk/src/main.pi`:

```
thorn-rs plan    <crate-root.rs> [--name N] [--edition E] [--target T]
                 [--out FILE] [--opt-level L] [--panic abort|unwind]
                 [--extern name=path]... [-L dir]... [--rustc BIN]
thorn-rs emit    <same flags> --format ninja|thorn --out FILE
thorn-rs check   <archive.a> [--expect sym]... [--header decl.h]
thorn-rs version | help
```

All planning decisions live in C; Pith only parses args, reads/writes files,
and prints — the same division thornk uses (`main.pi` ↔ `thornk.c`).

**Crate inspector (`scan.c`)** — bounded, fixed-buffer scanner of the crate
root (`lib.rs`), no dependencies:

| Detection | Method | Effect |
|---|---|---|
| `#![no_std]` | inner-attribute scan | sets `-C panic=abort` default; enables no-std checks |
| `#[panic_handler]` present | attribute scan | else **hard error** (verified: rustc fails with `` `#[panic_handler]` function required, but not found ``) |
| `extern crate alloc;` | token scan | warns unless a `#[global_allocator]` is present in-tree |
| `#[no_mangle] pub extern "C" fn <name>` / `#[export_name = "..."]` | attribute+signature scan | seeds `capi_syms` for Phase 2 |
| crate name | `#![crate_name = "..."]` or `--name` flag | feeds `--crate-name`; unset → error (output name must be deterministic) |
| `mod` file references | naive token scan | optional: depfile covers real deps; scanner only warns on missing modules |

Trust model: the crate root is a **trusted project input** (identical to a
`Makefile`); the scanner is read-only, bounded, and never executes anything.

**Thorn ABI bridge (`rust.c`)** — two exposure modes from one implementation:

- *Standalone (Phase 0):* imported by `main.pi` as `rust.*`; writes fragment
  files itself.
- *In-spec (Phase 1 option A):* imported by a user's `build.thorn` as
  `rust.*`; functions call through to engine-graph population via emitted
  `engine.add_command(...)` lines written to a side file, or (option B)
  compiled **into** thorn where it can call `graph_add_command()` directly
  (§4).

**Toolchain probe (`plan.c`)** — locate and pin the toolchain, mirroring
thorn's precedence chain:

1. `--rustc` flag → 2. `RUSTC` env → 3. `PATH` search for `rustc`
   (`validate_executable()` semantics: absolute path → `access(X_OK)`;
   otherwise `strtok_r` over `PATH`, default `/usr/bin:/bin`).
2. Version probe: `rustc --version` → diagnostics only, never embedded in
   output bytes (determinism invariant).
3. Host triple: `rustc -vV` → `host:` line. Target triple resolution order:
   explicit `--target` → `RUST_TARGET` env → host triple. (Mapping from
   `cc -dumpmachine` is deliberately **not** automatic; see §5.6.)

Environment scrubbing for determinism: `RUSTFLAGS`, `CARGO_*`, and
`RUSTC_WRAPPER` are **ignored** (never read, never passed through). Only
explicit spec/flag input shapes the command line.

---

## 3. Intermediate Specification / Rule Format

### 3.1 The crate model (C struct — normative)

```c
/* src/include/rust.h */
enum { RUST_CRATE_STATICLIB = 1 };        /* Phase 0: the only kind */

typedef struct RustCrate {
    char name[128];        /* --crate-name; defines artifact lib<name>.a   */
    char root[1024];       /* crate root source file (lib.rs)               */
    char out[1024];        /* canonical archive path (see §3.3 invariant)   */
    int  kind;             /* RUST_CRATE_STATICLIB                          */
    char edition[16];      /* "2015"|"2018"|"2021"|"2024"; default "2021"   */
    char target[128];      /* --target triple; "" = host                    */
    int  opt_level;        /* 0..3,  's'|'z' encoded; -1 = omit (rustc dflt)*/
    int  panic_abort;      /* 1 => -C panic=abort (default for #![no_std])  */
    int  debuginfo;        /* 0..2                                          */
    StrList cfgs;          /* --cfg '<name>="<value>"' (declared, never resolved) */
    StrList rustflags;     /* extra -C/-Z( stable-only) passes, allowlisted */
    StrList externs;       /* --extern name=path (sibling in-tree crates)   */
    StrList search_paths;  /* -L <dir>                                      */
    StrList capi_syms;     /* expected extern "C" exports (Phase 2)         */
} RustCrate;
```

Invariants: all paths whitespace-free (thorn `has_whitespace` convention);
`name` matches `[A-Za-z_][A-Za-z0-9_-]*` (valid in `lib<name>.a` and as a
ninja-friendly identifier); `out` is derived once (`out/<...>lib<name>.a`)
and reused for every reference.

### 3.2 The `rustc` contract (verified against rustc 1.98.1)

The single canonical invocation thorn-rs plans:

```
rustc --edition <E> --crate-name <name> --crate-type staticlib
      [-C panic=abort] [-C opt-level=<L>] [-C debuginfo=<D>]
      [--target <triple>]
      [--cfg '<k>="<v>"']... [--extern <n>=<p>]... [-L <dir>]...
      --emit=dep-info=<out>.d,link=<out>
      <crate-root>
```

Verified properties (probe crate: `#![no_std]`, one `#[panic_handler]`, one
`#[no_mangle] pub extern "C" fn thorn_rs_probe() -> i32`):

- Emits exactly `libprobe.a` at the `link=` path; no `-o` needed, no stray
  side artifacts in cwd.
- `--emit=dep-info=<out>.d,link=<out>` composes with a single flag; the
  depfile appears even on compile failure (partial content) — harmless to
  ninja, which treats an unusable depfile as "rebuild".
- The depfile is gcc-style Make syntax with **multiple target lines**:

  ```
  probe.d: probe.rs

  libprobe.a: probe.rs

  probe.rs:
  ```

  ninja's `deps = gcc` parser accepts multiple targets and the trailing
  empty rule; thorn's existing `cc` rule already uses `depfile = $out.d` +
  `deps = gcc`, so the Rust depfile slots into the **same convention** with
  no backend changes.
- `#[no_mangle] pub extern "C" fn thorn_rs_probe` exports as `T thorn_rs_probe`
  (verified via `nm -g --defined-only`). The archive also exports
  `rust_begin_unwind` and `core::panicking` internals — Phase 2 must do
  **subset matching** (expected ⊆ defined), never equality (§5.5).
- Archive members carry **mtime epoch 0** — rustc writes deterministic
  archives; byte-determinism holds for identical inputs, flags, toolchain.
- The archive **bundles all of `core` and `compiler_builtins`** (13 MB for a
  3-line crate at `-Oz`; the `core` CGU alone is ~6 MB) — see §5.1.
- `#![no_std]` without `#[panic_handler]` is a hard rustc error — the
  inspector must catch this *before* spending a rustc invocation.

Flag mapping policy:

| User intent | rustc flag | Default |
|---|---|---|
| C-ABI static archive | `--crate-type=staticlib` | always |
| Edition | `--edition` | `2021`, pinned explicitly per crate (never implicit) |
| Panic strategy | `-C panic=abort` | `abort` when `#![no_std]`, else must be stated |
| Optimization | `-C opt-level=<L>` | `z` for no_std libs unless overridden |
| Cross compile | `--target <triple>` | host triple from `rustc -vV` |
| Debug info | `-C debuginfo=<D>` | `0` |

### 3.3 Lowering onto Thorn's graph (Phase 1 semantics)

One `RustCrate` lowers to three graph mutations — **no new target type, no
emitter changes**:

```
1. build edge   engine.add_command("<out>", "<planned rustc argv>", "<root>")
                # ninja: rule cmd_N + build <pfx><out>: cmd_N <root>
2. link input   engine.add_source("<c_target>", "<out>")
                # .a is non-buildable -> verbatim on the link/solink line
3. ordering     engine.add_order_dep("<c_target>", "<out>")
                # belt-and-braces for backend edges that lose implicit deps
```

**Path identity invariant (normative):** the string passed to
`add_command(output, …)` and to `add_source(c_target, …)` must be
byte-identical, because `ninja.c` rewrites the former through
`out_name(…, dir_prefix(ninja_path), …)` but passes the latter through raw.
thorn-rs therefore canonicalizes `out` at plan time relative to the spec's
out-dir and refuses to emit if the two derivations disagree.

### 3.4 Emitted formats

**Phase 0 standalone Ninja fragment** (dedicated rule + per-edge variables —
mirrors thorn's `rule cc` / per-edge `cflags` idiom):

```
# build-rs.ninja - generated by thorn-rs <version>, do not edit
# project: <proj> (deterministic: same inputs, same bytes)

ninja_required_version = 1.3
rustc = <resolved rustc path or "rustc">

rule rustc
  command = $rustc --edition $edition --crate-name $crate_name --crate-type staticlib $rustflags --emit=dep-info=$out.d,link=$out $in
  depfile = $out.d
  deps = gcc
  description = rustc $crate_name -> $out

build out/libprobe.a: rustc src/lib.rs
  crate_name = probe
  edition = 2021
  rustflags = -C panic=abort -C opt-level=z
```

**Phase 1 spec fragment** (what option-A import writes for a build.thorn to
inline, byte-identical to `spec.c` style):

```
engine.add_command("out/libprobe.a", "rustc --edition 2021 --crate-name probe --crate-type staticlib -C panic=abort -C opt-level=z --emit=dep-info=out/libprobe.a.d,link=out/libprobe.a src/lib.rs", "src/lib.rs")
```

**Phase 2 verification edge** (emitted as a phony check, POSIX tools only):

```
rule rust_verify
  command = sh tools/verify-syms.sh $in $expected   # nm -g --defined-only | subset match
  description = verify C-ABI exports of $in

build .thorn-rs/probe.verified: rust_verify out/libprobe.a
```

---

## 4. Integration Roadmap & Milestones

### Phase 0 — standalone CLI, deterministic rustc Ninja rules

**Scope.** Repo scaffold (family conventions: BSD-2, SECURITY.md, install.sh,
CI), `vendor/thorn`+`vendor/pith` submodules pinned to release tags, Pith CLI
driver, C planner + inspector, Ninja-fragment emission, depfile wiring.

**Deliverables / acceptance:**

- `thorn-rs emit --format ninja` produces the §3.4 fragment; `ninja -f
  build-rs.ninja` builds `libprobe.a`; `nm` shows the expected `T` symbols.
- Re-running the emitter on identical inputs is **byte-identical**
  (determinism test in `tests/verify.sh`).
- Negative tests: missing `#[panic_handler]` → clear diagnostic before any
  rustc run; whitespace path → refused; `extern crate alloc` without
  `#[global_allocator]` → warning.
- `make check` green; no absolute paths in any output.

**Explicitly out of scope:** thorn graph integration, make-fragment emission
(can be added trivially later; ninja first because depfile support is native).

### Phase 1 — full Thorn spec ABI hookup

Goal: a `build.thorn` declares a Rust staticlib and a C target that consumes
it; `thorn` emits a backend where the C link depends on the Rust archive.

**Option A — import-into-spec (zero thorn changes):**

```
# build.thorn (user project)
import "vendor/thorn-rs/src/rust.c"
rust.plan("src/lib.rs", "out/libprobe.a")     # -> engine.add_command via bridge
engine.add_target("app", engine.exe)
engine.add_source("app", "src/main.c")
engine.add_source("app", "out/libprobe.a")    # verbatim link input (§1.3.1)
```

The bridge writes the planned `engine.add_command(...)` through the same
in-process graph? No — an imported `rust.c` **cannot see thorn's `static
g_graph`** (it is private to `engine.c`). Option A therefore has the bridge
*emit the add_command spec line into the user's graph by writing a side
spec the project includes textually*, or thorn gains a tiny host hook.
Honest assessment: option A works only for standalone fragment workflows,
not for single-graph specs.

**Option B — thorn absorbs `rust.c` (recommended end-state):**
`rust.c` is vendored into thorn (`src/rust.c`), and `host_register()` gains:

```
{ "rust_crate", c_engine_rust_crate, "p..." },   # name, root, [edition], [target], ...
{ "rust_emit",  c_engine_rust_emit,  ""    },    # folds planned crates into graph edges
```

Then a spec reads naturally:

```
engine.project("mixed")
engine.rust_crate("probe", "rust/probe/src/lib.rs")
engine.add_target("app", engine.exe)
engine.add_source("app", "src/main.c")
engine.add_source("app", "out/libprobe.a")
engine.add_order_dep("app", "out/libprobe.a")
```

`rust_crate` runs the inspector + planner at spec-evaluation time (configure
phase, like `pkg_config`), validates the crate, and defers edge creation to
`rust_emit()` (or folds immediately — decision: fold immediately, matching
`pkg_config`'s eagerness). Backends need **zero changes**; §3.3's three
graph mutations are all existing API.

**Migration path:** `rust.c` is written from day one against only
`engine.h` + libc, so Phase 1B is a vendor-and-register step, not a rewrite.
thorn-rs remains a separate repo (thornk model) whose `rust.c` is consumed
by thorn via submodule pin, with the standalone CLI kept for cargo-less
projects that don't use thorn specs.

### Phase 2 — C-ABI symbol verification

- Scanner-derived `capi_syms` (from `#[no_mangle]`/`#[export_name]`) are
  checked against the built archive via `nm -g --defined-only`, **subset
  semantics** (every expected symbol must be defined; unexpected exports are
  reported as informational, never fatal — §5.5).
- Optional `--header decl.h`: a bounded scanner extracts `extern`-declared
  function identifiers and cross-checks the two sets (header ⊆ archive for
  the declared-`extern` names). No preprocessor, no full C parsing —
  declaration-shape scanning only, mirroring pith's prototype-scanner
  philosophy.
- Emitted as a ninja verification edge (§3.4) so verification is part of the
  normal build, plus a standalone `thorn-rs check` for CI use.
- Failure mode: hard error with `expected C-ABI symbol '<sym>' not exported
  by <archive>` — catches `#[no_mangle]` typos and edition-driven mangling
  regressions at build time instead of link time.

---

## 5. Identified Risks & Edge Cases

### 5.1 `core` + `compiler_builtins` duplication (the big one)

`--crate-type=staticlib` bakes **all** upstream rlib objects — including the
full `core` and `compiler_builtins` CGUs — into every archive (verified:
13 MB for a 3-line crate at `-Oz`). Consequences:

- Two Rust staticlibs in one C link → duplicate `T` symbols
  (`rust_begin_unwind`, `core::panicking::*`, builtins) → link errors or
  silent dead-weight.
- Policy for Phase 0/1: **one Rust crate per link** (documented hard rule,
  enforced by a Phase-2 nm cross-check across all linked archives when more
  than one is declared). Alternatives (`--emit=obj` per crate with a single
  merged archive, `-Z` flags) are unstable or cargo-territory — out of scope.

### 5.2 builtins / compiler-rt interaction on non-glibc targets

- Linux/glibc x86_64: self-contained; the C link needs nothing extra
  (verified probe links against plain `cc`).
- FreeBSD (`*-freebsd` triples): `rust-std` ships its own
  `compiler-builtins`; no action needed, but `--target` must be explicit —
  there is no host inference away from the build machine's triple.
- Bare-metal/arm/riscv: builtins may provide `memcpy`-class symbols that
  collide with libc/libgcc expectations of the C link. Out of thorn-rs's
  support perimeter for Phase 0 (hosted POSIX targets only); documented as a
  non-goal so nobody reaches for `-C link-arg` hacks through `rustflags`.

### 5.3 `#![no_std]` prerequisites without Cargo

- **`#[panic_handler]` is mandatory and nothing supplies it** (verified hard
  error). The inspector fails fast with a diagnostic pointing at the fixture
  pattern in `tests/fixtures/probe/`.
- **`alloc` requires an in-tree `#[global_allocator]`**; warning, not error
  (a crate may legitimately re-export alloc types only).
- Panic strategy: default `-C panic=abort` for `no_std`; `panic=unwind`
  would pull unwinding runtime expectations into the C link — allowed only
  with an explicit opt-in and a documented warning.

### 5.4 Depfile mechanics

- Format verified: gcc-style, multiple target lines, one empty trailing
  rule; compatible with ninja's `deps = gcc` parser and identical in
  convention to thorn's `cc` rule (`depfile = $out.d`).
- `--emit=dep-info` writes the depfile even when compilation fails; ninja
  treats a partial/odd depfile as "needs rebuild" — no special handling.
- **Whitespace paths are unrepresentable** in Make-style depfiles; thorn-rs
  adopts thorn's `has_whitespace` refusal at plan time (crate root, out
  path, `-L` dirs, `--extern` paths).
- Incremental-rustc (`-Zincremental`, `cargo check`-style caching) is a
  non-goal: every build edge is a clean `rustc` invocation; ninja's own
  incrementalism (depfile + mtime) is the only caching layer. This matches
  thorn's determinism philosophy and keeps `RUSTFLAGS`-style env leakage
  impossible.

### 5.5 Symbol-surface hazards

- Staticlib exports are **superset-noisy**: `rust_begin_unwind`,
  `core::panicking::assert_failed` (both verified in `nm` output) plus every
  instantiated generic. Phase 2 uses subset matching for correctness and
  treats collisions with C code as a documented risk (a C `assert_failed`
  would clash — unlikely, but the check cross-reports duplicates).
- `#[no_mangle]` name collisions across the C and Rust sides are caught at
  C link time; thorn-rs's verification edge surfaces the archive side early.

### 5.6 Toolchain and environment

- **Triple mapping is manual.** `cc -dumpmachine` yields
  `x86_64-redhat-linux`-style strings that do not match Rust's
  `x86_64-unknown-linux-gnu`. Automatic translation tables would be guesswork;
  thorn-rs takes `--target`/`RUST_TARGET` explicitly and validates against
  `rustc --print target-list` (bounded probe, cached per run).
- **Env hygiene:** `RUSTFLAGS`, `CARGO`, `RUSTC_WRAPPER` are scrubbed from
  consideration; the planned argv is the *entire* input. This is what makes
  "same inputs → same bytes" auditable.
- **rustc version drift** changes codegen bytes (not an error — artifacts are
  rebuild products, not distributed); thorn-rs records the resolved
  `rustc --version` in stderr diagnostics only.

### 5.7 Backend parity and known thorn constraints

- The Makefile backend supports custom commands (`subst_cmd_for_make`
  escaping; `make_uses_pattern()` is only a pattern-rule optimization) but
  has **no depfile support** — Make relies on explicit order deps for the
  Rust archive. Acceptable: the archive's inputs are the crate tree, tracked
  coarsely via `add_order_dep`; fine-grained dep tracking is a ninja-only
  capability, stated in docs (`--engine ninja` recommended for Rust-mixed
  projects).
- `Command.command[1024]` bounds the planned argv; the planner fails cleanly
  if the rustc line exceeds it (long `-L`/`--extern` lists) — the fix is
  thorn-side (widen or multi-edge), not silent truncation.

---

## Appendix A — Verified probe transcript (rustc 1.98.1, Fedora 44)

```
$ printf '#![no_std]\nuse core::panic::PanicInfo;\n#[panic_handler]\nfn ph(_: &PanicInfo) -> ! { loop {} }\n#[no_mangle]\npub extern "C" fn thorn_rs_probe() -> i32 { 42 }\n' > probe.rs
$ rustc --edition 2021 --crate-name probe --crate-type staticlib -C panic=abort -C opt-level=z --emit=dep-info=probe.d,link=libprobe.a probe.rs
$ cat probe.d
probe.d: probe.rs
<blank>
libprobe.a: probe.rs
<blank>
probe.rs:
$ nm -g --defined-only libprobe.a | grep -E "thorn_rs_probe|rust_begin"
0000000000000000 T _RNvCs…_7___rustc17rust_begin_unwind
0000000000000000 T thorn_rs_probe
$ ar tv libprobe.a        # determinism: all member mtimes are epoch 0
rw-r--r-- 0/0 1328  Dec 31 14:00 1969 probe.probe.3b97bd93ae41fc-cgu.0.rcgu.o
rw-r--r-- 0/0 6208960 Dec 31 14:00 1969 core-4bf756d1bac7a391.core.dc34d2c961210e2a-cgu.0.rcgu.o
rw-r--r-- 0/0 239152 Dec 31 14:00 1969 compiler_builtins-ed6bc9cdf3a73b3b…rcgu.o
```

Without `#[panic_handler]`, the same invocation fails hard:
``error: `#[panic_handler]` function required, but not found`` — confirming
the inspector's fail-fast requirement.

## Appendix B — Glossary of surfaces

| Surface | Defined in | Consumed by |
|---|---|---|
| `engine.*` host namespace | `thorn/src/main.c::host_register` | `build.thorn` specs (in-process) |
| `decompile.*` import namespace | `thorn/src/decompile.c` via pith C-FFI | thornk's `main.pi`, future thorn-rs `main.pi` |
| `rust.*` import namespace (proposed) | `thorn-rs/src/rust.c` | Phase 0 driver, Phase 1A specs |
| `engine.rust_crate` (proposed) | thorn `main.c` + vendored `rust.c` | Phase 1B specs |
| Graph/Target/Command structs | `thorn/src/include/engine.h` | both backends, unchanged |
