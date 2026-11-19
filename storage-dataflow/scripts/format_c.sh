#!/usr/bin/env bash
# Format or check every first-party C source/header with the project style.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

mode="${1:---check}"
target=""
if [[ "$mode" == "--write-file" ]]; then
    if [[ $# -ne 2 ]]; then
        echo "usage: $0 --write-file PATH" >&2
        exit 2
    fi
    target="$2"
elif [[ $# -gt 1 ]] || [[ ! "$mode" =~ ^(--check|--write|--list)$ ]]; then
    echo "usage: $0 [--check|--write|--list|--write-file PATH]" >&2
    exit 2
fi

# Keep this scope shared by local lint and hosted CI. In particular, do not
# restyle vendored nanorq or generated CFFI/binding artifacts.
mapfile -d '' -t sources < <(
    find src/c include tests/c tools bench cmake -type f \
        \( -name '*.c' -o -name '*.h' \) \
        ! -path 'tests/c/golden_json.h' -print0 | sort -z
)

if ((${#sources[@]} == 0)); then
    echo "no first-party C sources found" >&2
    exit 1
fi

if [[ "$mode" == "--list" ]]; then
    printf '%s\n' "${sources[@]}"
    exit 0
fi

if ! command -v clang-format >/dev/null 2>&1; then
    echo "clang-format not installed (CI pins 23.1.5)" >&2
    exit 1
fi

if [[ "$mode" == "--write-file" ]]; then
    # Resolve symlinks and reject paths outside the repository. Membership in
    # the canonical list, rather than a second path policy, controls formatting.
    if ! relative="$(python3 - "$ROOT" "$target" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])
target = Path(sys.argv[2]).resolve()
try:
    relative = target.relative_to(root)
except ValueError:
    raise SystemExit(1)
if not target.is_file():
    raise SystemExit(1)
print(relative)
PY
)"; then
        exit 0
    fi
    for source in "${sources[@]}"; do
        if [[ "$source" == "$relative" ]]; then
            clang-format -i "$ROOT/$source"
            break
        fi
    done
    exit 0
fi

if [[ "$mode" == "--write" ]]; then
    clang-format -i "${sources[@]}"
else
    clang-format --dry-run --Werror "${sources[@]}"
fi
