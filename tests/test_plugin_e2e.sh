#!/bin/sh
# test_plugin_e2e.sh - thorn-rs.tpkg plugs into thorn.
#
# Proves the full plugin contract: thorn loads the thorn-rs plugin from
# the pith.toml dependency, the unified build.thorn drives BOTH
# languages through one graph, ninja compiles C + Rust + the C-ABI
# audit, the binary runs, and a header mismatch fails the build at the
# audit edge.
#
# Prerequisites: thorn built (THORN_BIN or ../thorn/out/thorn) and
# dist/thorn-rs.tpkg (make dist/thorn-rs.tpkg).

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
THORN="${THORN_BIN:-$ROOT/../thorn/out/thorn}"
if [ ! -x "$THORN" ]; then
    THORN=$(command -v thorn 2>/dev/null || true)
fi
if [ -z "${THORN:-}" ] || [ ! -x "$THORN" ]; then
    echo "thorn binary not found (set THORN_BIN or build ../thorn)"
    exit 1
fi

PASS=0
FAIL=0
ok()  { printf '  ok  %s\n' "$1"; PASS=$((PASS + 1)); }
bad() { printf '  FAIL %s\n' "$1"; FAIL=$((FAIL + 1)); }
step() { printf '\n== %s ==\n' "$1"; }

# ------------------------------------------------------------------
step "package: thorn-rs.tpkg"
make -C "$ROOT" dist/thorn-rs.tpkg >/dev/null 2>&1 \
    && [ -f "$ROOT/dist/thorn-rs.tpkg" ] \
    && ok "the .tpkg bundle builds"

FIX="$ROOT/tests/fixtures/hybrid_tpkg"
cd "$FIX" || exit 1
rm -rf out .thorn-rs .pith .thorn

# ------------------------------------------------------------------
step "host: plugin discovery and spec evaluation"
"$THORN" pkg sync >/dev/null 2>&1 \
    && ok "thorn pkg sync resolves the .tpkg dependency"
"$THORN" --out-dir out --engine ninja > host.log 2>&1 \
    && ok "thorn evaluates the unified spec with the plugin loaded"
grep -q "plugin loaded: thorn-rs" host.log \
    && ok "the host reports the plugin namespace"
rm -f host.log

# ------------------------------------------------------------------
step "graph: one backend carries both languages"
grep -q "rustc --edition" out/build.ninja \
    && grep -q "dep-info=out/libsafe.a.d" out/build.ninja \
    && ok "the rustc edge uses a gcc-style depfile"
grep -q "out/libsafe.a: cmd" out/build.ninja \
    && ok "the archive edge is a custom command"
grep -q "link out/app__src_main.o out/libsafe.a" out/build.ninja \
    && ok "the C link edge consumes the audited archive"
grep -q ".thorn-rs/safe.verified" out/build.ninja \
    && ok "the audit edge is in the graph"

# ------------------------------------------------------------------
step "build: ninja compiles C, Rust and the audit"
ninja -f out/build.ninja >/dev/null 2>&1 && ok "ninja build succeeded"
[ -f out/libsafe.a ] && ok "out/libsafe.a built"
[ -f out/.thorn-rs/safe.verified ] && ok "the audit stamp was written"
./out/app && ok "the hybrid binary runs (probe_answer == 42)"

# ------------------------------------------------------------------
step "verify: a header mismatch fails at the audit"
sed -i 's/probe_answer/probe_answer_typo/' include/safe.h
"$THORN" --out-dir out --engine ninja >/dev/null 2>&1 \
    && ok "re-planning after the header edit succeeds"
if ninja -f out/build.ninja >/dev/null 2>&1; then
    bad "the mismatched header passed the audit"
else
    ok "the mismatched header fails the build at the audit edge"
fi
sed -i 's/probe_answer_typo/probe_answer/' include/safe.h
"$THORN" --out-dir out --engine ninja >/dev/null 2>&1
ninja -f out/build.ninja >/dev/null 2>&1 \
    && ok "the restored header audits clean again"

# ------------------------------------------------------------------
printf '\nthorn-rs plugin e2e: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
