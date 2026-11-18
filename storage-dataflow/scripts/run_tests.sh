#!/usr/bin/env bash
# Build and run the C tests, then the Python unit tests when pytest exists.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Always build (incremental no-op when fresh) so the gate can never run
# stale binaries against edited sources.
./scripts/build.sh

ctest --test-dir build --output-on-failure

if python3 -c 'import pytest' >/dev/null 2>&1; then
    PYTHONPATH="$ROOT/python${PYTHONPATH:+:$PYTHONPATH}" \
        python3 -m pytest tests/python -q
else
    echo "pytest not installed; skipping Python tests" >&2
fi

if [[ -x tests/system/e2e_cache_fill.sh ]] && command -v docker >/dev/null 2>&1; then
    echo "system tests are placeholders; skipping" >&2
else
    echo "system tests not configured; skipping" >&2
fi
