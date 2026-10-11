#!/bin/sh
# verify.sh - the thorn-rs acceptance suite (Phase 0).
#
# Proves the standalone contract:
#   build:    bin/thorn-rs builds from C99 sources with zero dependencies
#   scan:     crate-name extraction, no_std defaults, fail-fast on a
#             missing #[panic_handler] (before any rustc invocation)
#   plan:     the canonical rustc command is printed deterministically
#   emit:     ninja and make fragments; format inference from --out
#   build:    ninja and make both compile lib<name>.a from the fragments
#   abi:      nm confirms the expected exported C-ABI symbols
#   determin: identical inputs emit byte-identical fragments; poisoned
#             RUSTFLAGS/CARGO_*/RUSTC_WRAPPER environments change nothing
#   codes:    scanner failures (1), I/O failures (2), and usage errors
#             (64) are distinct POSIX exit classes

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
PATH="$ROOT/bin:$PATH"
export PATH
SCRATCH=$(mktemp -d /tmp/thorn-rs-verify.XXXXXX)
trap 'rm -rf "$SCRATCH"' EXIT

PASS=0
FAIL=0

ok()  { printf '  ok  %s\n' "$1"; PASS=$((PASS + 1)); }
bad() { printf '  FAIL %s\n' "$1"; FAIL=$((FAIL + 1)); }
step() { printf '\n== %s ==\n' "$1"; }

# ------------------------------------------------------------------
step "build: bin/thorn-rs from C99 sources"
make clean >/dev/null 2>&1 || true
if make >/dev/null 2>&1 && [ -x bin/thorn-rs ]; then
    ok "bin/thorn-rs built"
else
    bad "build failed"; exit 1
fi
bin/thorn-rs version | grep -q "thorn-rs 0.1.0" \
    && ok "version reports 0.1.0"

FIXTURE=tests/fixtures/probe/lib.rs

# ------------------------------------------------------------------
step "scan: crate inspector"
bin/thorn-rs plan "$FIXTURE" >/dev/null 2>&1 \
    && ok "probe fixture passes the inspector"
bin/thorn-rs emit tests/fixtures/missing-ph/lib.rs --format ninja \
    --out "$SCRATCH/never.ninja" >/dev/null 2>&1
RC=$?
[ "$RC" -eq 1 ] && ok "missing panic_handler fails with exit 1"
[ ! -f "$SCRATCH/never.ninja" ] \
    && ok "no fragment written on scanner failure"
bin/thorn-rs emit tests/fixtures/missing-ph/lib.rs --format ninja \
    --out "$SCRATCH/never.ninja" 2>&1 | grep -q "panic_handler" \
    && ok "diagnostic names #[panic_handler]"

# ------------------------------------------------------------------
step "plan: deterministic rustc command"
PLAN=$(bin/thorn-rs plan "$FIXTURE" 2>/dev/null)
case "$PLAN" in
    *"--crate-type staticlib"*"--emit=dep-info="*"lib.rs")
        ok "plan prints the canonical rustc invocation" ;;
    *) bad "plan output malformed" ;;
esac
bin/thorn-rs plan "$FIXTURE" 2>/dev/null > "$SCRATCH/p1.txt"
bin/thorn-rs plan "$FIXTURE" 2>/dev/null > "$SCRATCH/p2.txt"
cmp -s "$SCRATCH/p1.txt" "$SCRATCH/p2.txt" \
    && ok "plan is byte-deterministic across runs"

# ------------------------------------------------------------------
step "emit: ninja fragment"
bin/thorn-rs emit "$FIXTURE" --out out/build-rs.ninja >/dev/null 2>&1 \
    && ok "emit (format inferred from .ninja) succeeds"
grep -q "ninja_required_version = 1.3" out/build-rs.ninja \
    && grep -q "deps = gcc" out/build-rs.ninja \
    && grep -q "depfile = \$out.d" out/build-rs.ninja \
    && grep -q "crate_name = probe" out/build-rs.ninja \
    && grep -q "edition = 2021" out/build-rs.ninja \
    && grep -q "rustflags = -C panic=abort -C opt-level=z" \
        out/build-rs.ninja \
    && ok "fragment carries the section 3.4 contract"
bin/thorn-rs emit "$FIXTURE" --out out/build-rs.ninja >/dev/null 2>&1
cp out/build-rs.ninja "$SCRATCH/n1.ninja"
bin/thorn-rs emit "$FIXTURE" --out out/build-rs.ninja >/dev/null 2>&1
cmp -s out/build-rs.ninja "$SCRATCH/n1.ninja" \
    && ok "ninja fragment is byte-deterministic"

# ------------------------------------------------------------------
step "emit: make fragment"
bin/thorn-rs emit "$FIXTURE" --format make --out out-make/build-rs.mk \
    >/dev/null 2>&1 && ok "emit make succeeds"
grep -q "RUSTC ?= rustc" out-make/build-rs.mk \
    && grep -q -- "-include" out-make/build-rs.mk \
    && grep -q "libprobe\.a\.d" out-make/build-rs.mk \
    && ok "make fragment carries RUSTC ?= and depfile inclusion"
bin/thorn-rs emit "$FIXTURE" --format make --out out-make/build-rs.mk \
    >/dev/null 2>&1
cp out-make/build-rs.mk "$SCRATCH/m1.mk"
bin/thorn-rs emit "$FIXTURE" --format make --out out-make/build-rs.mk \
    >/dev/null 2>&1
cmp -s out-make/build-rs.mk "$SCRATCH/m1.mk" \
    && ok "make fragment is byte-deterministic"

# ------------------------------------------------------------------
step "determinism: environment poisoning changes nothing"
RUSTFLAGS="-C panic=unwind -C opt-level=0" \
RUSTC_WRAPPER=/nonexistent/wrapper \
CARGO_HOME=/nonexistent/cargo \
CARGO_PKG_NAME=poison \
    bin/thorn-rs emit "$FIXTURE" --out out/build-rs.ninja \
    >/dev/null 2>&1
cmp -s out/build-rs.ninja "$SCRATCH/n1.ninja" \
    && ok "RUSTFLAGS/CARGO_*/RUSTC_WRAPPER are scrubbed"

# ------------------------------------------------------------------
step "build: ninja backend compiles the archive"
NINJA_BIN=""
command -v ninja >/dev/null 2>&1 && NINJA_BIN=ninja
[ -z "$NINJA_BIN" ] && command -v samu >/dev/null 2>&1 && NINJA_BIN=samu
if [ -n "$NINJA_BIN" ]; then
    "$NINJA_BIN" -f out/build-rs.ninja >/dev/null 2>&1 \
        && [ -f out/libprobe.a ] \
        && ok "$NINJA_BIN built out/libprobe.a"
    nm -g --defined-only out/libprobe.a 2>/dev/null \
        | grep -q " T thorn_rs_probe$" \
        && nm -g --defined-only out/libprobe.a 2>/dev/null \
            | grep -q " T thorn_rs_add$" \
        && ok "nm confirms the C-ABI exports"
else
    bad "no ninja or samu on PATH; skipping ninja backend build"
fi

# ------------------------------------------------------------------
step "build: make backend compiles the archive"
if make -f out-make/build-rs.mk >/dev/null 2>&1 \
    && [ -f out-make/libprobe.a ]; then
    ok "make built out-make/libprobe.a"
else
    bad "make backend failed"
fi
nm -g --defined-only out-make/libprobe.a 2>/dev/null \
    | grep -q " T thorn_rs_probe$" && ok "nm confirms the C-ABI export"
BEFORE=$(cat out-make/libprobe.a | sha256sum | cut -d' ' -f1)
sleep 1
touch "$FIXTURE"
make -f out-make/build-rs.mk >/dev/null 2>&1
AFTER=$(cat out-make/libprobe.a | sha256sum | cut -d' ' -f1)
[ "$BEFORE" != "$AFTER" ] \
    && ok "depfile tracking rebuilds after the crate root changes"

# ------------------------------------------------------------------
step "cli: naming, inference, and exit code classes"
bin/thorn-rs emit "$FIXTURE" --name renamed --out "$SCRATCH/r.ninja" \
    >/dev/null 2>&1
grep -q "crate_name = renamed" "$SCRATCH/r.ninja" \
    && grep -q "librenamed.a" "$SCRATCH/r.ninja" \
    && ok "--name overrides the #![crate_name] attribute"
bin/thorn-rs emit "$FIXTURE" --out "$SCRATCH/x.mk" >/dev/null 2>&1 \
    && grep -q "RUSTC ?= rustc" "$SCRATCH/x.mk" \
    && ok ".mk extension infers the make format"
bin/thorn-rs emit tests/fixtures/missing-ph/lib.rs 2>/dev/null
[ $? -eq 1 ] && ok "scanner failure exits 1"
bin/thorn-rs emit /nonexistent/lib.rs --format ninja \
    --out "$SCRATCH/y.ninja" 2>/dev/null
[ $? -eq 2 ] && ok "unreadable crate root exits 2 (I/O class)"
bin/thorn-rs emit "$FIXTURE" --edition 1999 --out "$SCRATCH/z.ninja" \
    2>/dev/null
[ $? -eq 64 ] && ok "invalid --edition exits 64 (usage class)"
bin/thorn-rs emit "$FIXTURE" --no-such-flag 2>/dev/null
[ $? -eq 64 ] && ok "unknown flag exits 64"
bin/thorn-rs emit --format ninja 2>/dev/null
[ $? -eq 64 ] && ok "missing crate root exits 64"

# ------------------------------------------------------------------
step "paths: whitespace rejection"
mkdir -p tests/out
cp "$FIXTURE" "tests/out/bad path.rs"
bin/thorn-rs emit "tests/out/bad path.rs" --format ninja \
    --out "$SCRATCH/w.ninja" 2>/dev/null
[ $? -eq 1 ] && ok "whitespace paths rejected before planning"
rm -f "tests/out/bad path.rs"
bin/thorn-rs emit "$FIXTURE" --rustc /nonexistent/rustc \
    --out "$SCRATCH/t.ninja" 2>/dev/null
[ $? -eq 1 ] && ok "non-executable --rustc fails planning"

# ------------------------------------------------------------------
step "verify: C-ABI symbol audit"
bin/thorn-rs check out/libprobe.a --expect thorn_rs_probe >/dev/null 2>&1 \
    && ok "check passes on the verified archive"
bin/thorn-rs check out/libprobe.a --expect thorn_rs_missing 2>/dev/null
[ $? -eq 1 ] && ok "a missing export fails with exit 1"
bin/thorn-rs check out/libprobe.a --header tests/fixtures/probe/probe.h \
    >/dev/null 2>&1 && ok "the header contract verifies"
printf 'int thorn_rs_typo(void);\n' > tests/out/bad.h
bin/thorn-rs check out/libprobe.a --header tests/out/bad.h 2>&1 \
    | grep -q "thorn_rs_typo" \
    && ok "a header mismatch names the missing symbol"

# ------------------------------------------------------------------
step "verify: the emitted verification edge"
bin/thorn-rs emit "$FIXTURE" --header tests/fixtures/probe/probe.h \
    --out out/build-verify.ninja >/dev/null 2>&1 \
    && ok "emit --header succeeds"
grep -q "rust_verify" out/build-verify.ninja \
    && grep -q -- "--expect thorn_rs_probe" out/build-verify.ninja \
    && grep -q -- "--header tests/fixtures/probe/probe.h" \
        out/build-verify.ninja \
    && ok "the fragment carries the audit command"
if [ -n "$NINJA_BIN" ]; then
    rm -rf .thorn-rs
    "$NINJA_BIN" -f out/build-verify.ninja >/dev/null 2>&1 \
        && [ -f .thorn-rs/probe.verified ] \
        && ok "ninja runs the audit and stamps the result"
fi

# ------------------------------------------------------------------
step "build: thorn-parity front-end"
DEFDIR="$SCRATCH/defbuild"
mkdir -p "$DEFDIR/src"
cp tests/fixtures/probe/lib.rs "$DEFDIR/src/lib.rs"
(cd "$DEFDIR" && thorn-rs build >/dev/null 2>&1) \
    && [ -f "$DEFDIR/build-rs.ninja" ] \
    && ok "bare build defaults to src/lib.rs in the CWD"
(cd "$DEFDIR" && ninja -f build-rs.ninja >/dev/null 2>&1) \
    && [ -f "$DEFDIR/libprobe.a" ] \
    && ok "the default fragment builds in place"

rm -rf out/rs
bin/thorn-rs build -f tests/fixtures/probe/lib.rs --out-dir out/rs \
    --engine both >/dev/null 2>&1
[ -f out/rs/build-rs.ninja ] && [ -f out/rs/Makefile ] \
    && ok "--out-dir --engine both writes both backends"
ninja -f out/rs/build-rs.ninja >/dev/null 2>&1 \
    && [ -f out/rs/libprobe.a ] \
    && ok "ninja -f out/rs/build-rs.ninja compiles the archive"
NINJA_SHA=$(sha256sum out/rs/libprobe.a | cut -d' ' -f1)
rm -f out/rs/libprobe.a out/rs/libprobe.a.d
make -C out/rs >/dev/null 2>&1 && [ -f out/rs/libprobe.a ] \
    && ok "make -C out/rs compiles the archive independently"
MAKE_SHA=$(sha256sum out/rs/libprobe.a | cut -d' ' -f1)
if [ -n "$NINJA_SHA" ] && [ "$NINJA_SHA" = "$MAKE_SHA" ]; then
    ok "ninja and make archives are bit-for-bit identical"
else
    bad "backend archives diverge"
fi

bin/thorn-rs build tests/fixtures/probe/lib.rs \
    tests/fixtures/probe2/lib.rs --out-dir out/multi >/dev/null 2>&1 \
    && ok "multi-crate build emits one fragment"
grep -q "rule rustc_probe" out/multi/build-rs.ninja \
    && grep -q "rule rustc_probe2" out/multi/build-rs.ninja \
    && grep -q "build all: phony" out/multi/build-rs.ninja \
    && ok "multi-crate fragments namespace rules and aggregate all"
ninja -f out/multi/build-rs.ninja >/dev/null 2>&1 \
    && [ -f out/multi/libprobe.a ] && [ -f out/multi/libprobe2.a ] \
    && ok "multi-crate build compiles both archives"
bin/thorn-rs build tests/fixtures/probe/lib.rs \
    tests/fixtures/probe/lib.rs --out-dir "$SCRATCH/dup" 2>/dev/null
[ $? -eq 1 ] && ok "duplicate crate names are refused"

(cd "$SCRATCH" && thorn-rs build >/dev/null 2>&1)
[ $? -eq 2 ] && ok "missing src/lib.rs fails with the I/O class"

# ------------------------------------------------------------------
printf '\nthorn-rs acceptance: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
