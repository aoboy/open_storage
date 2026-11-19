#!/usr/bin/env bash
# Optional lint/format pass. Each tool is skipped when it is not installed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if command -v clang-format >/dev/null 2>&1; then
    mapfile -t sources < <(find src/c include -type f \( -name '*.c' -o -name '*.h' \))
    if ((${#sources[@]})); then
        clang-format --dry-run --Werror "${sources[@]}"
    fi
else
    echo "clang-format not installed; skipping" >&2
fi

if command -v clang-tidy >/dev/null 2>&1 && [[ -f build/compile_commands.json ]]; then
    mapfile -t c_sources < <(find src -type f -name '*.c')
    if ((${#c_sources[@]})); then
        clang-tidy -p build "${c_sources[@]}"
    fi
else
    echo "clang-tidy or build/compile_commands.json missing; skipping" >&2
fi

if command -v ruff >/dev/null 2>&1; then
    (cd python && ruff check sdf)
else
    echo "ruff not installed; skipping" >&2
fi

if command -v mypy >/dev/null 2>&1; then
    (cd python && mypy --config-file mypy.ini sdf)
else
    echo "mypy not installed; skipping" >&2
fi
