#!/usr/bin/env bash
# Plan prompt W6.17 — control-plane Helm chart lint/template gate.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CHART="$ROOT/packaging/helm/sdf"
HELM="${HELM:-helm}"
KUBECONFORM="${KUBECONFORM:-kubeconform}"

if ! command -v "$HELM" >/dev/null 2>&1; then
    echo "test_helm: helm not installed; skipping chart lint/template" >&2
    exit 0
fi

OUT="$ROOT/out/helm"
mkdir -p "$OUT"

"$HELM" lint --strict "$CHART"
for env in dev staging prod; do
    values="$CHART/values-$env.yaml"
    "$HELM" lint --strict "$CHART" -f "$values"
    "$HELM" template sdf "$CHART" -f "$values" >"$OUT/$env.yaml"
    for expected in "kind: Deployment" "kind: Service" "name: control-plane" \
        "path: /healthz" "path: /readyz" "readOnlyRootFilesystem: true"; do
        if ! grep -q "$expected" "$OUT/$env.yaml"; then
            echo "test_helm: $env manifest missing: $expected" >&2
            exit 1
        fi
    done
done
"$HELM" template sdf "$CHART" >"$OUT/default.yaml"

if command -v "$KUBECONFORM" >/dev/null 2>&1; then
    "$KUBECONFORM" -strict -summary "$OUT"/*.yaml
else
    echo "test_helm: kubeconform not installed; skipping schema validation" >&2
fi

echo "test_helm: OK (lint + dev/staging/prod templates -> $OUT)"
