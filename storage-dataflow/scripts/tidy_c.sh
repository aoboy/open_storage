#!/usr/bin/env bash
# Run clang-tidy on compiled first-party C sources, including tests/benchmarks.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if ! command -v clang-tidy >/dev/null 2>&1; then
    echo "clang-tidy not installed (CI pins 23.1.4)" >&2
    exit 1
fi
if [[ ! -f build/compile_commands.json ]]; then
    echo "build/compile_commands.json missing; run make build first" >&2
    exit 1
fi

mapfile -t sources < <(
    python3 - "$ROOT" <<'PY'
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
entries = json.loads((root / "build/compile_commands.json").read_text())
roots = ("src/c", "include", "tests/c", "tools", "bench", "cmake")
sources = set()
for entry in entries:
    source = Path(entry["file"])
    if not source.is_absolute():
        source = Path(entry["directory"]) / source
    source = source.resolve()
    try:
        relative = source.relative_to(root)
    except ValueError:
        continue
    if source.suffix == ".c" and str(relative).startswith(roots):
        sources.add(str(relative))
print("\n".join(sorted(sources)))
PY
)

if ((${#sources[@]} == 0)); then
    echo "no compiled first-party C sources found" >&2
    exit 1
fi

full_sources=()
style_sources=()
for source in "${sources[@]}"; do
    if [[ "$source" == src/c/* ]]; then
        full_sources+=("$source")
    else
        style_sources+=("$source")
    fi
done

# Preserve the established full production-source gate. The style rollout adds
# brace enforcement to other compiled C without silently expanding unrelated
# analyzer policy for tests and harnesses.
if ((${#full_sources[@]})); then
    clang-tidy -p build --quiet "${full_sources[@]}"
fi
if ((${#style_sources[@]})); then
    clang-tidy -p build --quiet \
        --checks='-*,readability-braces-around-statements' \
        --warnings-as-errors='readability-braces-around-statements' \
        --extra-arg=-Wno-error \
        "${style_sources[@]}"
fi
