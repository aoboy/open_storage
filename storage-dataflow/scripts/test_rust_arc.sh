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
echo "test_rust_arc: OK (offset estimator + queue + drain + stats + slab/callback + sidecar parity slices)"
