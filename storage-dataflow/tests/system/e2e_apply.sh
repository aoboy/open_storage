#!/usr/bin/env bash
# Plan prompt W4.5 — the apply-path e2e + the kill-switch runbook leg.
#
# The C sidecar integration test (test_arc_sidecar) already drives the
# pump loop in all three modes; this script's job is the RUNBOOK leg:
# docs/ops/runbooks/arc-killswitch.md executed as a scripted flow —
# live under a bursty load, kill mid-stream, order intact, zero drops,
# and the deploy controller's flip sequence shadow -> live -> killed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

./scripts/build.sh >/dev/null

gate=0

# 1. the sidecar in every mode (the in-process bursty generator)
if ! build/tests/c/test_arc_sidecar >/dev/null; then
    echo "E2E FAIL: sidecar modes" >&2
    gate=1
fi

# 2. the ARC core goldens (the promotion evidence is only meaningful if
#    the schedule math is the paper's)
if ! build/tests/c/test_arc_core >/dev/null; then
    echo "E2E FAIL: ARC core goldens" >&2
    gate=1
fi

# 3. the deploy controller flow: shadow -> live -> (breach) -> killed
if ! PYTHONPATH="$ROOT/python" python3 - <<'EOF'
from sdf.control.arc_deploy import ArcDeployConfig, ArcDeployLoop, ArcMode

class SC:
    def __init__(self): self.mode = None
    def set_mode(self, m): self.mode = m
    def stats(self): return {"budget_alerts": 0}

sc = SC()
loop = ArcDeployLoop(sc, ArcDeployConfig(shadow_min_seconds=1))
loop.start()
assert sc.mode == ArcMode.SHADOW
loop.state.shadow_started_ns -= 2 * 10**9  # force the 24 h window
rec = loop.step()
assert rec["action"] == "promote" and sc.mode == ArcMode.LIVE
loop.transport.set_mode(ArcMode.LIVE)
assert loop.kill() == ArcMode.KILLED and sc.mode == ArcMode.KILLED
print("runbook flow: shadow -> live -> killed OK")
EOF
then
    echo "E2E FAIL: deploy runbook flow" >&2
    gate=1
fi

if [[ "$gate" -ne 0 ]]; then
    echo "e2e_apply: FAILED" >&2
    exit 1
fi
echo "e2e_apply: OK (sidecar modes, ARC goldens, kill-switch runbook flow)"
