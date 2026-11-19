#!/usr/bin/env bash
# Plan prompt W6.1 — the process-pair chain harness: a LIVE wm-proxy
# (own process, --shm, --pace-ns so it stays up) driven by the Python
# control loop over the ADR-0002 segment, with the scrape checked live.
#
#   C proxy: pushes per-origin stats into the telemetry ring per tick
#   Python: WmControlLoop drains the ring, publishes (version advances)
#   C proxy: acquires the published profile at its next tick
#   scrape:  sdf_proxy_shm_profile_version >= 1, heartbeat > 0
#
# Exits nonzero if the chain never closes.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

./scripts/build.sh >/dev/null

PORT=$((22700 + RANDOM % 200))
NAME="sdf-e2e-chain-$$"
UNITS=60000
PACE_US=150          # ~9 s of wall time: enough for several control ticks
TIMEOUT_S=30

rm -f "/dev/shm/$NAME" 2>/dev/null || true

SDF_METRICS_ENDPOINT="127.0.0.1:$PORT" \
    build/wm-proxy --units "$UNITS" --pace-ns $((PACE_US * 1000)) \
    --shm "$NAME" --profile steady >"/tmp/sdf-chain-proxy.log" 2>&1 &
PROXY_PID=$!

if ! PYTHONPATH="$ROOT/python" python3 tests/system/shm_chain_driver.py \
    "$NAME" 4 "$PORT" "$TIMEOUT_S"; then
    echo "E2E FAIL: the chain driver" >&2
    kill "$PROXY_PID" 2>/dev/null || true
    wait "$PROXY_PID" 2>/dev/null || true
    cat /tmp/sdf-chain-proxy.log >&2 || true
    rm -f "/dev/shm/$NAME"
    exit 1
fi

wait "$PROXY_PID"

# the proxy's own exit line must confirm the acquisition end-to-end
if ! grep -qE 'shm_version=[1-9]' /tmp/sdf-chain-proxy.log; then
    echo "E2E FAIL: the proxy never reported the acquired version" >&2
    cat /tmp/sdf-chain-proxy.log >&2
    rm -f "/dev/shm/$NAME"
    exit 1
fi
grep -E '^PROXY ' /tmp/sdf-chain-proxy.log

rm -f "/dev/shm/$NAME"
echo "e2e_shm_chain: OK (C-proxy -> telemetry -> Python loop -> published
profile -> acquired -> scrape; process pair, live)"
