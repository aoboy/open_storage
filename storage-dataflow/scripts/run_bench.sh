#!/usr/bin/env bash
# Run the control-plane microbenchmark. The full data-plane harness is TODO.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PYTHONPATH="$ROOT/python${PYTHONPATH:+:$PYTHONPATH}" \
    python3 -m sdf.bench.runner --config configs/dev --out out/bench "$@"
