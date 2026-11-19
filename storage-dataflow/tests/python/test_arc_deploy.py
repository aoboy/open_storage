"""W4.4 — the ARC deploy controller against a stubbed sidecar.

Arming, the shadow promotion gate (24 h + SLO clean), the live-mode SLO
watch with the kill recommendation and the auto-kill switch, the
operator kill switch, and the un-armed guard.
"""

from __future__ import annotations

import time

import pytest

from sdf.control.arc_deploy import (
    ArcDeployConfig,
    ArcDeployLoop,
    ArcMode,
)


class StubSidecar:
    """Records mode flips and serves scripted stats."""

    def __init__(self) -> None:
        self.mode: ArcMode | None = None
        self.flips: list[str] = []
        self._stats: dict = {}

    def set_mode(self, mode: ArcMode) -> None:
        self.mode = mode
        self.flips.append(mode.value)

    def stats(self) -> dict:
        return dict(self._stats)

    def serve(self, **kw) -> None:
        self._stats.update(kw)


def test_start_arms_in_shadow():
    sc = StubSidecar()
    loop = ArcDeployLoop(sc)
    loop.start()
    assert sc.mode == ArcMode.SHADOW
    rec = loop.step()
    assert rec["mode"] == "shadow"
    assert rec["action"] == "hold-shadow"


def test_shadow_promotion_gate():
    sc = StubSidecar()
    loop = ArcDeployLoop(sc, ArcDeployConfig(shadow_min_seconds=10))
    loop.start()
    sc.serve(budget_alerts=0)

    # under the 24 h (here 10 s) window: hold
    rec = loop.step()
    assert rec["action"] == "hold-shadow"

    # force the elapsed window (the state is the controller's own clock;
    # tests drive it directly for determinism)
    loop.state.shadow_started_ns -= 11 * 1_000_000_000
    rec = loop.step()
    assert rec["action"] == "promote"
    assert sc.mode == ArcMode.LIVE
    assert "promoted to live" in loop.state.recommendations[-1]


def test_shadow_slo_breach_blocks_promotion():
    sc = StubSidecar()
    loop = ArcDeployLoop(sc, ArcDeployConfig(shadow_min_seconds=10))
    loop.start()
    sc.serve(budget_alerts=99)
    loop.state.shadow_started_ns -= 11 * 1_000_000_000
    rec = loop.step()
    assert rec["action"] == "hold-shadow"  # SLO dirty: no promotion


def test_live_slo_watch_recommends_kill():
    sc = StubSidecar()
    loop = ArcDeployLoop(sc, ArcDeployConfig(auto_kill=False))
    loop.start()
    # forced promotion (the promotion gate's clock is covered by its own
    # test): both the controller state and the transport move to LIVE
    loop.state.mode = ArcMode.LIVE
    sc.set_mode(ArcMode.LIVE)
    sc.serve(budget_alerts=0)

    rec = loop.step()
    assert rec["action"] == "hold-live"

    sc.serve(budget_alerts=2)
    rec = loop.step()
    assert rec["action"] == "breach-recommend-kill"
    assert "budget breach" in loop.state.recommendations[-1]
    assert sc.mode == ArcMode.LIVE  # recommended, not applied


def test_live_auto_kill():
    sc = StubSidecar()
    loop = ArcDeployLoop(sc, ArcDeployConfig(auto_kill=True))
    loop.start()
    loop.state.mode = ArcMode.LIVE
    sc.set_mode(ArcMode.LIVE)
    sc.serve(budget_alerts=1)
    rec = loop.step()
    assert rec["action"] == "auto-killed"
    assert sc.mode == ArcMode.KILLED


def test_operator_kill_switch():
    sc = StubSidecar()
    loop = ArcDeployLoop(sc)
    loop.start()
    loop.state.mode = ArcMode.LIVE
    sc.set_mode(ArcMode.LIVE)
    assert loop.kill() == ArcMode.KILLED
    assert sc.flips[-1] == "killed"
    rec = loop.step()
    assert rec["action"] == "killed-idle"


def test_unarmed_step_refuses():
    sc = StubSidecar()
    loop = ArcDeployLoop(sc)
    with pytest.raises(RuntimeError):
        loop.step()


def test_promotion_baselines_budget_alerts():
    """Alerts observed during shadow are not "new" on the first live step."""
    sc = StubSidecar()
    loop = ArcDeployLoop(
        sc,
        ArcDeployConfig(
            shadow_min_seconds=10,
            added_latency_budget_max_alerts=5,
        ),
    )
    loop.start()
    sc.serve(budget_alerts=3)

    loop.state.shadow_started_ns -= 11 * 1_000_000_000
    rec = loop.step()
    assert rec["action"] == "promote"
    assert loop.state.budget_alerts_seen == 3

    # The same cumulative count in live is hold, not a 3-alert breach.
    rec = loop.step()
    assert rec["action"] == "hold-live"

    sc.serve(budget_alerts=4)
    rec = loop.step()
    assert rec["action"] == "breach-recommend-kill"


# ---- W6.4: the live C sidecar through CFFI --------------------------------


def _cffi_transport():
    """Return the real transport, or skip when the extension is absent."""
    pytest.importorskip("sdf_cffi", reason="CFFI extension not built")
    from sdf.control.arc_deploy import CffiSidecarTransport

    return CffiSidecarTransport


def test_cffi_sidecar_mode_flips_land_in_one_pump():
    transport_cls = _cffi_transport()
    with transport_cls(mode=ArcMode.LIVE) as tr:
        tr.set_mode(ArcMode.LIVE)
        tr.pump()

        # A future producer stamp parks the entry under ARC-QG.
        assert tr.submit(
            b"x" * 64, producer_ts=time.monotonic_ns() + 50_000_000
        ) == 0
        assert tr.pump() >= 1
        st = tr.stats()
        assert st["in_flight"] == 1
        assert st["applied"] == 0

        # The kill request is visible immediately and lands on the next
        # pump, which also delivers the held backlog.
        tr.set_mode(ArcMode.KILLED)
        assert tr.stats()["mode"] == 2
        assert tr.pump() >= 1
        st = tr.stats()
        assert st["releases"] == 1
        assert st["applied"] == 1
        assert st["in_flight"] == 0


def test_cffi_deploy_loop_promotes_then_kills_live_sidecar():
    transport_cls = _cffi_transport()
    with transport_cls(mode=ArcMode.SHADOW) as tr:
        loop = ArcDeployLoop(
            tr,
            ArcDeployConfig(shadow_min_seconds=0, auto_kill=True),
        )
        loop.start()
        tr.pump()  # apply shadow

        loop.state.shadow_started_ns -= 1
        rec = loop.step()
        assert rec["action"] == "promote"
        tr.pump()  # the live flip lands here
        assert tr.stats()["mode"] == 1

        # Live owns the timing: this entry stays queued.
        assert tr.submit(
            b"y" * 64, producer_ts=time.monotonic_ns() + 50_000_000
        ) == 0
        assert tr.pump() >= 1
        assert tr.stats()["in_flight"] == 1

        assert loop.kill() == ArcMode.KILLED
        assert tr.pump() >= 1
        st = tr.stats()
        assert st["mode"] == 2
        assert st["applied"] == 1
        assert st["in_flight"] == 0
        assert loop.step()["action"] == "killed-idle"
