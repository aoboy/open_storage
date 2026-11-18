#!/usr/bin/env bash
# Bench harness (plan W0.5): C data-plane micro-benches recorded to
# build/bench_results.txt (never ctest-gated), then the Python
# control-plane bench as leg 2. Thresholds activate with their waves
# (wm gate: W2.6; arc gate: W4.2).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

./scripts/build.sh >/dev/null

RESULTS="build/bench_results.txt"
mkdir -p build
{
    echo "# recorded $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    for b in bench_wm; do
        if [[ -x "build/$b" ]]; then
            "build/$b"
        else
            echo "# $b not built; skipping" >&2
        fi
    done
} >> "$RESULTS"
echo "recorded to $RESULTS:" >&2
tail -n 8 "$RESULTS" >&2

# W2.6 bench gate (plan 10): the seeded spray decision cost must stay
# under 2x the 15-cyc target on this runner. The gate parses the freshest
# recorded seeded-method lines and fails loudly past the bound.
for cyc in $(grep -E 'BENCH wm_select method=shuffle[12]' "$RESULTS" |
             tail -n 2 | grep -oE 'cyc_per_dec=[0-9.]+' | cut -d= -f2); do
    if awk -v c="$cyc" 'BEGIN { exit !(c > 30.0) }'; then
        echo "BENCH GATE FAIL: seeded spray ${cyc} cyc/dec > 30 (2x of 15)" >&2
        exit 1
    fi
done

# Leg 2 — control-plane bench (unchanged behavior; args pass through).
PYTHONPATH="$ROOT/python${PYTHONPATH:+:$PYTHONPATH}" \
    python3 -m sdf.bench.runner --config configs/dev --out out/bench "$@"
