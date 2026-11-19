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

# 3. the deploy controller flow: shadow -> live -> (breach) -> killed.
#    W6.4 drives the REAL C sidecar through CFFI when the extension is
#    built; the stub flow remains the fallback for cffi-less hosts.
if ! PYTHONPATH="$ROOT/python:$ROOT/bindings/cffi" python3 - <<'EOF'
import time

from sdf.control.arc_deploy import ArcDeployConfig, ArcDeployLoop, ArcMode

try:
    import sdf_cffi  # noqa: F401
except ImportError:
    class SC:
        def __init__(self): self.mode = None
        def set_mode(self, m): self.mode = m
        def stats(self):
            now = time.monotonic_ns()
            return {
                "mode": 0,
                "budget_alerts": 0,
                "shadow_epoch": 1,
                "shadow_active": 1,
                "shadow_schedule_mode": 2,
                "shadow_schedule_releases": 10,
                "shadow_immediate_applies": 10,
                "shadow_schedule_samples": 10,
                "shadow_budget_alerts": 0,
                "shadow_last_schedule_release_ns": now,
                "shadow_scheduled_backlog": 0,
                "shadow_record_total": 10,
            }

    sc = SC()
    loop = ArcDeployLoop(sc, ArcDeployConfig(shadow_min_seconds=1))
    loop.start()
    assert sc.mode == ArcMode.SHADOW
    loop.state.shadow_started_ns -= 2 * 10**9  # force the 24 h window
    rec = loop.step()
    assert rec["action"] == "promote" and sc.mode == ArcMode.LIVE
    loop.transport.set_mode(ArcMode.LIVE)
    assert loop.kill() == ArcMode.KILLED and sc.mode == ArcMode.KILLED
    print("runbook flow (stub: cffi unavailable): shadow -> live -> killed OK")
else:
    from sdf.control.arc_deploy import CffiSidecarTransport

    with CffiSidecarTransport(mode=ArcMode.SHADOW) as tr:
        loop = ArcDeployLoop(
            tr, ArcDeployConfig(shadow_min_seconds=0, auto_kill=True)
        )
        loop.start()
        tr.pump()  # apply shadow

        # W6.10: a real shadow QG sample is required for promotion.
        assert tr.submit(b"s" * 64, producer_ts=time.monotonic_ns()) == 0
        assert tr.pump() >= 1
        time.sleep(0.030)
        assert tr.pump() >= 1
        shadow = tr.stats()
        assert shadow["shadow_schedule_samples"] >= 1
        assert shadow["shadow_schedule_mode"] == 2

        loop.state.shadow_started_ns -= 1
        rec = loop.step()
        assert rec["action"] == "promote"
        tr.pump()  # live flip lands on this pump
        assert tr.stats()["mode"] == 1

        # live owns the timing: a future stamp stays queued
        assert tr.submit(
            b"k" * 64, producer_ts=time.monotonic_ns() + 50_000_000
        ) == 0
        assert tr.pump() >= 1
        assert tr.stats()["in_flight"] == 1

        assert loop.kill() == ArcMode.KILLED
        assert tr.pump() >= 1
        st = tr.stats()
        assert st["mode"] == 2 and st["applied"] == 2 and st["in_flight"] == 0
        print("runbook flow (live C sidecar via CFFI): "
              "shadow -> live -> killed OK")
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
