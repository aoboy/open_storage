#!/usr/bin/env bash
# Plan prompt W2.6 — P1 end-to-end over the four degradation profiles.
#
# Runs the wm-proxy shell across steady / slow / dies / flapping with
# the built-in gates trusted via exit codes, then the bytes-neutral
# A/B leg (treatment vs control must move the same bytes with zero
# failures), recording every PROXY line to build/bench_results.txt
# (recorded, never ctest-gated). Exits nonzero on any gate failure.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

./scripts/build.sh >/dev/null

UNITS="${SDF_E2E_UNITS:-8192}"
PROXY="build/wm-proxy"
RESULTS="build/bench_results.txt"
mkdir -p build

gate=0

run_profile() {
    local name="$1"
    local out
    if ! out="$("$PROXY" --units "$UNITS" --profile "$name")"; then
        echo "E2E FAIL: profile $name exited nonzero" >&2
        gate=1
        return
    fi
    echo "$out" | tee -a "$RESULTS"
    if ! grep -q 'GATES OK' <<<"$out"; then
        echo "E2E FAIL: profile $name gates" >&2
        gate=1
    fi
}

for p in steady slow dies flapping; do
    run_profile "$p"
done

# Bytes-neutral leg (design 01: a dispatch-only change must move the
# same bytes): treatment (100%) vs control (0%) complete every unit.
for arm in 100 0; do
    out="$("$PROXY" --units "$UNITS" --rollout "$arm" --profile steady)"
    echo "$out" | tee -a "$RESULTS"
    if ! grep -q 'GATES OK' <<<"$out"; then
        echo "E2E FAIL: arm rollout=$arm" >&2
        gate=1
    fi
    # every unit completed in both arms
    total_ok=$(grep -oE 'ok=[0-9]+' <<<"$out" | cut -d= -f2 |
        awk '{s+=$1} END {print s}')
    if [[ "$total_ok" != "$UNITS" ]]; then
        echo "E2E FAIL: arm rollout=$arm ok=$total_ok != $UNITS" >&2
        gate=1
    fi
done

if [[ "$gate" -ne 0 ]]; then
    echo "e2e_cache_fill: FAILED" >&2
    exit 1
fi
echo "e2e_cache_fill: OK (steady/slow/dies/flapping + bytes-neutral arms)"
