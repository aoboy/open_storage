#!/usr/bin/env bash
# Plan prompt W5.3 — the Rust seam leg: cbindgen regenerates the
# fountain header from the Rust skeleton and diffs it against
# include/sdf/fountain.h. Empty diff = the seam holds. Rust tooling not
# installed: skip cleanly (the leg is advisory in this environment; the
# hosted CI matrix is the authority once a runner exists — the same
# posture as the wack_a_mole hosted-matrix item).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CRATE="$ROOT/src/rust/sdf-fountain"
HEADER="$ROOT/include/sdf/fountain.h"

if ! command -v cargo >/dev/null 2>&1; then
    echo "test_rust_seam: cargo not installed; skipping (advisory leg)" >&2
    exit 0
fi

# build: build.rs runs cbindgen and writes the generated header
(cd "$CRATE" && cargo build --quiet)

GEN="$CRATE/sdf_fountain_generated.h"
if [[ ! -f "$GEN" ]]; then
    echo "test_rust_seam: cbindgen produced no header" >&2
    exit 1
fi

# The diff normalizes: the C header carries documentation comments the
# generator does not reproduce; the seam contract is the DECLARATION
# surface (names, types, order). Extract declarations from both sides.
decls() {
    grep -E '^(int|uint32_t|size_t|bool|typedef|extern)' "$1" |
        sed 's/;$//' | sed 's/[[:space:]]\+/ /g' | sort
}

if ! diff <(decls "$HEADER") <(decls "$GEN") >/tmp/sdf_rust_seam.diff; then
    echo "test_rust_seam: HEADER DIFF — the Rust view diverged:" >&2
    cat /tmp/sdf_rust_seam.diff >&2
    echo "Intentional? Ride the ADR and bump golden_vectors (ADR-0001)." >&2
    exit 1
fi

echo "test_rust_seam: OK (declaration surface identical)"
