#!/usr/bin/env bash
# Plan prompt W3.6 — the three-arm A/B + loss ladder (design 02).
#
# Arms: control (plain ARQ), lt (FLUID + LT), nanorq (FLUID + RaptorQ).
# Ladder: loss 0..50% at eps=0.10, plus the eps sweep {0.01..0.20} at
# 10% loss. Every block must complete byte-exact (exit-code gates);
# the Pareto lines record to build/bench_results.txt (overhead =
# bytes_tx / block bytes). ARQ fallback is EXPECTED past the slack
# (loss > eps): the never-ship-incomplete guarantee is the gate.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

./scripts/build.sh >/dev/null

RESULTS="build/bench_results.txt"
mkdir -p build
gate=0

run_one() {
    local out
    if ! out="$(build/fluid-repair "$@")"; then
        echo "E2E FAIL: $* (nonzero exit)" >&2
        gate=1
        return
    fi
    echo "$out" | tee -a "$RESULTS"
}

echo "== loss ladder (eps=0.10) ==" | tee -a "$RESULTS"
for loss in 0 5 10 15 20 30 40 50; do
    for arm in control lt nanorq; do
        run_one --arm "$arm" --eps 0.10 --loss "$loss"
    done
done

echo "== eps sweep (loss=10%) ==" | tee -a "$RESULTS"
for eps in 0.01 0.05 0.10 0.20; do
    for arm in control lt nanorq; do
        run_one --arm "$arm" --eps "$eps" --loss 10
    done
done

if [[ "$gate" -ne 0 ]]; then
    echo "e2e_repair: FAILED" >&2
    exit 1
fi
echo "e2e_repair: OK (3 arms x loss ladder + eps sweep, byte-exact, recorded)"
