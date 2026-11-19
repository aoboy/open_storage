#!/usr/bin/env bash
# Plan prompt W5.3/W6.5 — the Rust seam leg: cbindgen regenerates the
# fountain header from the Rust skeleton, and a declaration-surface
# comparison against include/sdf/fountain.h must be empty. Empty diff =
# the seam holds. Rust tooling not installed: skip cleanly by default;
# the hosted CI matrix passes --require so the leg is AUTHORITATIVE
# there (W6.5). The wire format is compared by
# scripts/check_rust_seam.py (functions by type, typedefs by name,
# structs by field order, enums by value) — formatting is not the seam.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CRATE="$ROOT/src/rust/sdf-fountain"
HEADER="$ROOT/include/sdf/fountain.h"
REQUIRE_CARGO=0
if [[ "${1:-}" == "--require" ]]; then
    REQUIRE_CARGO=1
fi

if ! command -v cargo >/dev/null 2>&1; then
    if ((REQUIRE_CARGO)); then
        echo "test_rust_seam: cargo is REQUIRED here (--require) but absent" >&2
        exit 1
    fi
    echo "test_rust_seam: cargo not installed; skipping (advisory locally; CI uses --require)" >&2
    exit 0
fi

# build: build.rs runs cbindgen and writes the generated header
(cd "$CRATE" && cargo build --quiet)
(cd "$CRATE" && cargo test --quiet)

GEN="$CRATE/sdf_fountain_generated.h"
if [[ ! -f "$GEN" ]]; then
    echo "test_rust_seam: cbindgen produced no header" >&2
    exit 1
fi

python3 scripts/check_rust_seam.py "$HEADER" "$GEN"
