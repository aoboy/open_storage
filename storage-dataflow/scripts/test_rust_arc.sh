#!/usr/bin/env bash
# W6.18 first slice — Rust ARC offset-estimator quality gate.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CRATE="$ROOT/src/rust/sdf-arc"

if ! command -v cargo >/dev/null 2>&1; then
    echo "test_rust_arc: cargo absent; skipping offset-estimator checks" >&2
    exit 0
fi

(cd "$CRATE" && cargo fmt --check)
(cd "$CRATE" && cargo clippy --all-targets -- -D warnings)
(cd "$CRATE" && cargo test --locked)

# W6.18 public-ABI seam: cbindgen regenerates the Rust view of
# include/sdf/arc.h and the declaration surfaces must be identical.
(cd "$CRATE" && cargo build --quiet)
GEN="$CRATE/sdf_arc_generated.h"
if [[ ! -f "$GEN" ]]; then
    echo "test_rust_arc: cbindgen produced no ARC header" >&2
    exit 1
fi
python3 "$ROOT/scripts/check_rust_seam.py" "$ROOT/include/sdf/arc.h" "$GEN"

# The staticlib is the future opt-in CMake swap artifact; the C pacer's
# public symbols stay C-owned (the crate only declares them).
if [[ ! -f "$CRATE/target/debug/libsdf_arc.a" ]]; then
    echo "test_rust_arc: staticlib missing after cargo build" >&2
    exit 1
fi

echo "test_rust_arc: OK (estimator + queue + drain + stats + slab/callback + sidecar + 20-function ABI seam + staticlib)"
