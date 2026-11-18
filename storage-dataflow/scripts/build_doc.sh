#!/usr/bin/env bash
# Render the design documents to PDF when pandoc and xelatex are available.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

mkdir -p out

if command -v pandoc >/dev/null 2>&1 && command -v xelatex >/dev/null 2>&1; then
    pandoc docs/design/*.md \
        --from=gfm \
        --to=pdf \
        --pdf-engine=xelatex \
        --toc \
        --number-sections \
        --resource-path=docs/figures \
        --metadata title="Storage Dataflow Stack" \
        -o out/storage-dataflow-design.pdf
    echo "wrote out/storage-dataflow-design.pdf"
else
    echo "pandoc and/or xelatex not installed; skipping PDF render" >&2
fi
