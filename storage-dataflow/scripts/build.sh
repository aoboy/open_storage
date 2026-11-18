#!/usr/bin/env bash
# Build the C libraries and, when available, the CFFI bindings.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel

if python3 -c 'import cffi' >/dev/null 2>&1; then
    python3 bindings/cffi/build_ffi.py
else
    echo "cffi not installed; skipping CFFI bindings" >&2
fi
