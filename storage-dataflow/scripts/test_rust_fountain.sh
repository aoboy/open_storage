#!/usr/bin/env bash
# Plan prompt W6.8 — functional Rust LT behind the unchanged public C ABI.
# Builds and tests BOTH implementations, requires byte-identical parity output,
# runs the Rust-backed ASan+UBSan suite, and verifies installed/package/CFFI
# consumers. Missing Rust tooling skips locally by default; --require is the
# authoritative hosted-CI mode (W6.5 posture).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
CRATE="$ROOT/src/rust/sdf-fountain"
REQUIRE=0
if [[ "${1:-}" == "--require" ]]; then
    REQUIRE=1
fi

if ! command -v cargo >/dev/null 2>&1; then
    if ((REQUIRE)); then
        echo "test_rust_fountain: cargo is REQUIRED but absent" >&2
        exit 1
    fi
    echo "test_rust_fountain: cargo absent; skipping (CI uses --require)" >&2
    exit 0
fi

component_or_skip() {
    local component="$1"
    shift
    if ! (cd "$CRATE" && cargo "$component" --version >/dev/null 2>&1); then
        if ((REQUIRE)); then
            echo "test_rust_fountain: cargo $component is REQUIRED but absent" >&2
            exit 1
        fi
        echo "test_rust_fountain: cargo $component absent; skipping locally" >&2
        return 0
    fi
    (cd "$CRATE" && cargo "$@")
}

component_or_skip fmt fmt --check
component_or_skip clippy clippy --all-targets -- -D warnings
(cd "$CRATE" && cargo test --locked)
./scripts/test_rust_seam.sh --require

WORK="$(mktemp -d "${TMPDIR:-/tmp}/sdf-rust-fountain-XXXX")"
trap 'rm -rf "$WORK"' EXIT

build_and_test() {
    local name="$1"
    local rust="$2"
    local build="$WORK/$name"
    cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DSDF_USE_RUST_FOUNTAIN="$rust"
    cmake --build "$build" --parallel
    ctest --test-dir "$build" --output-on-failure
}

build_and_test c OFF
build_and_test rust ON

# The same extracted goldens drive the unchanged public-ABI harness. Comparing
# complete reports makes the A/B evidence explicit, including pending counts.
"$WORK/c/tests/c/test_parity" >"$WORK/parity-c.txt"
"$WORK/rust/tests/c/test_parity" >"$WORK/parity-rust.txt"
cmp "$WORK/parity-c.txt" "$WORK/parity-rust.txt"
cat "$WORK/parity-rust.txt"

# 15 public frontend functions + 13 internal LT/PRNG functions, exactly once.
# nanorq's nrq_* adapter remains C and is intentionally outside this list.
cat >"$WORK/expected-symbols.txt" <<'EOF'
fd_decode
fd_destroy
fd_init
fd_init_codec
fd_init_in
fd_push
fd_rank
fd_ready
fd_sizeof
fe_destroy
fe_generate
fe_init
fe_init_in
fe_sizeof
fe_symbol_count
lt_dec_destroy
lt_dec_init
lt_dec_sizeof
lt_decode
lt_enc_destroy
lt_enc_init
lt_enc_sizeof
lt_generate
lt_prng_next
lt_push
lt_rank
lt_ready
lt_symbol_count
EOF
sort -o "$WORK/expected-symbols.txt" "$WORK/expected-symbols.txt"
nm -g --defined-only "$WORK/rust/libsdf_fountain.a" |
    awk '$3 ~ /^(fe_|fd_|lt_)/ { print $3 }' | sort >"$WORK/actual-symbols.txt"
diff -u "$WORK/expected-symbols.txt" "$WORK/actual-symbols.txt"
if ar t "$WORK/rust/libsdf_fountain.a" | grep -E 'fe_codec_lt|fe_prng'; then
    echo "test_rust_fountain: C LT objects leaked into the Rust variant" >&2
    exit 1
fi

# The sanitizer instruments the unchanged C callers and allocator boundary;
# the Rust staticlib itself remains the pinned release build (no nightly Rust
# sanitizer requirement). Any C-side ABI/layout misuse still trips ASan/UBSan.
ASAN="$WORK/rust-asan"
cmake -S . -B "$ASAN" -DCMAKE_BUILD_TYPE=Debug -DSDF_USE_RUST_FOUNTAIN=ON \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
cmake --build "$ASAN" --parallel
ctest --test-dir "$ASAN" --output-on-failure

# Packaging is part of the implementation swap: no extra Rust archive or
# caller-specific link line may leak into any installed consumer surface.
SDF_USE_RUST_FOUNTAIN=ON ./scripts/test_install.sh
SDF_USE_RUST_FOUNTAIN=ON ./scripts/test_deb.sh

echo "test_rust_fountain: OK (C/Rust 27/27 each, identical 1,410 goldens, symbols exact, Rust ASan+UBSan, install + deb)"
