#!/usr/bin/env bash
# Plan prompt W3.6 — the three-arm A/B + loss ladder (design 02).
#
# Arms: control (plain ARQ), lt (FLUID + LT), nanorq (FLUID + RaptorQ).
# Ladder: loss 0..50% at eps=0.10, plus the eps sweep {0.01..0.20} at
# 10% loss. W6.3 adds the 4-block cold-checkpoint object (nanorq,
# eps=0.15) across the ladder with per-block assertions: blocks=,
# block_rounds= (one value per block), block_fallback=, block_ok=.
# Every block must complete byte-exact (exit-code gates); the Pareto
# lines record to build/bench_results.txt (overhead = bytes_tx /
# object bytes). ARQ fallback is EXPECTED past the slack (loss > eps):
# the never-ship-incomplete guarantee is the gate.
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

# W6.3: a multi-block run must report exactly `want` per-block values
# and every block_ok entry must be 1 (the coordinator's byte-exact
# gate). Missing/malformed fields fail the leg.
run_blocks() {
    local want="$1"
    shift
    local out
    if ! out="$(build/fluid-repair "$@")"; then
        echo "E2E FAIL: $* (nonzero exit)" >&2
        gate=1
        return
    fi
    echo "$out" | tee -a "$RESULTS"

    local blocks rounds ok
    blocks=$(sed -n 's/.* blocks=\([0-9][0-9]*\) .*/\1/p' <<<"$out")
    rounds=$(sed -n 's/.* block_rounds=\([^ ]*\) .*/\1/p' <<<"$out")
    ok=$(sed -n 's/.* block_ok=\([^ ]*\).*/\1/p' <<<"$out")
    if [[ "$blocks" != "$want" ]]; then
        echo "E2E FAIL: blocks=$blocks, expected $want ($*)" >&2
        gate=1
        return
    fi
    local IFS=','
    local -a rvals=($rounds) ovals=($ok)
    if [[ ${#rvals[@]} -ne $want || ${#ovals[@]} -ne $want ]]; then
        echo "E2E FAIL: per-block field count ($*)" >&2
        gate=1
        return
    fi
    local v
    for v in "${rvals[@]}"; do
        if ! [[ "$v" =~ ^[0-9]+$ ]]; then
            echo "E2E FAIL: block_rounds not numeric ($*)" >&2
            gate=1
            return
        fi
    done
    for v in "${ovals[@]}"; do
        if [[ "$v" != "1" ]]; then
            echo "E2E FAIL: block_ok=$ok ($*)" >&2
            gate=1
            return
        fi
    done
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

echo "== multi-block checkpoint (4 blocks, nanorq, eps=0.15, wave=64) ==" | tee -a "$RESULTS"
for loss in 0 10 20 30 40 50; do
    run_blocks 4 --arm nanorq --eps 0.15 --loss "$loss" --blocks 4 --wave 64
done

# Tight slack keeps the gate open for several rounds: proves the
# ranked scheduler and per-block round accounting (not just one
# certifying wave followed by fallback).
echo "== multi-block scheduler stress (eps=0.02, loss=30%) ==" | tee -a "$RESULTS"
run_blocks 4 --arm nanorq --eps 0.02 --loss 30 --blocks 4 --wave 64

if [[ "$gate" -ne 0 ]]; then
    echo "e2e_repair: FAILED" >&2
    exit 1
fi
echo "e2e_repair: OK (3 arms x loss ladder + eps sweep + 4-block checkpoint, byte-exact, recorded)"
