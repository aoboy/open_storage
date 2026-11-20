#!/usr/bin/env bash
# Plan prompt W6.12 — paired same-runner C/Rust LT fountain benchmark.
#
# Builds the same bench_fountain binary with SDF_USE_RUST_FOUNTAIN=OFF and
# ON, runs identical K/symbol/loss configurations, records both variants to
# out/bench/fountain_paired.txt, and validates/comparses the pairs. This is
# evidence only: no performance threshold is enforced.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

REQUIRE=0
if [[ "${1:-}" == "--require" ]]; then
    REQUIRE=1
fi

if ! command -v cargo >/dev/null 2>&1; then
    if ((REQUIRE)); then
        echo "bench_fountain_paired: cargo is REQUIRED but absent" >&2
        exit 1
    fi
    echo "bench_fountain_paired: cargo absent; skipping (use --require in hosted CI)" >&2
    exit 0
fi

for tool in cmake python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "bench_fountain_paired: missing required tool: $tool" >&2
        exit 1
    fi
done

C_BUILD="$ROOT/build-bench-c"
RUST_BUILD="$ROOT/build-bench-rust"
build_variant() {
    local build="$1"
    local variant="$2"
    local bin="$3"
    cmake -S "$ROOT" -B "$build" -DCMAKE_BUILD_TYPE=Release \
        -DSDF_USE_RUST_FOUNTAIN="$variant" >/dev/null
    cmake --build "$build" --target bench_fountain --parallel >/dev/null
    if [[ ! -x "$bin" ]]; then
        echo "bench_fountain_paired: $bin was not built" >&2
        exit 1
    fi
}

build_variant "$C_BUILD" OFF "$C_BUILD/bench_fountain"
build_variant "$RUST_BUILD" ON "$RUST_BUILD/bench_fountain"

RUNNER="$(uname -srm | tr ' ' '-')"
REVISION="$(git rev-parse --short HEAD 2>/dev/null || printf 'unknown')"
OUT="$ROOT/out/bench/fountain_paired.txt"
mkdir -p "$ROOT/out/bench"
{
    printf '# paired fountain bench %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    printf '# runner=%s revision=%s\n' "$RUNNER" "$REVISION"
} >"$OUT"

run_pair() {
    local k="$1"
    local sym_len="$2"
    local loss="$3"
    local blocks="$4"
    "$C_BUILD/bench_fountain" --k "$k" --sym-len "$sym_len" --loss "$loss" \
        --blocks "$blocks" --runner "$RUNNER" | tee -a "$OUT"
    "$RUST_BUILD/bench_fountain" --k "$k" --sym-len "$sym_len" --loss "$loss" \
        --blocks "$blocks" --runner "$RUNNER" | tee -a "$OUT"
}

run_pair 16 64 0 512
run_pair 32 32 10 512
run_pair 64 16 20 512
run_pair 64 64 30 512

python3 "$ROOT/scripts/bench_fountain_compare.py" "$OUT"
echo "bench_fountain_paired: OK (raw records: $OUT)"
