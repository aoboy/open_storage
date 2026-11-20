#!/usr/bin/env bash
# Build and run the C tests, then the Python unit tests when pytest exists.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Always build (incremental no-op when fresh) so the gate can never run
# stale binaries against edited sources.
./scripts/build.sh

# C integration tests render their real scrape bodies for the Python
# dashboard/query validator. Keep this inside the ignored build tree.
export SDF_METRICS_FIXTURE_DIR="$ROOT/build/metrics-fixtures"
mkdir -p "$SDF_METRICS_FIXTURE_DIR"
rm -f "$SDF_METRICS_FIXTURE_DIR"/{arc,wm,fluid}.prom
ctest --test-dir build --output-on-failure

if python3 -c 'import pytest' >/dev/null 2>&1; then
    PYTHONPATH="$ROOT/python${PYTHONPATH:+:$PYTHONPATH}" \
        python3 -m pytest tests/python -q
else
    echo "pytest not installed; skipping Python tests" >&2
fi

echo "system/e2e harnesses are separate from ctest; run 'make e2e' to execute them" >&2
