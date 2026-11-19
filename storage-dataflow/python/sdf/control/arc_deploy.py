"""ARC deployment controller (plan prompt W4.4, design 03).

The operator surface over the sidecar: mode flips land within one poll
interval (design 03's "mode flips land within one poll interval" — the
sidecar pump picks the new mode on its next pass), the shadow promotion
gate consumes the recorded evidence (24 h of shadow with added-delay
SLO clean, per design 03's rollout table), and the kill switch is always
armed: any SLO breach recommends (and can apply) the flip to killed.

The sidecar itself is C (W4.3); this controller drives it through the
CFFI bindings or a stubbed transport in tests.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Protocol


class ArcMode(str, Enum):
    SHADOW = "shadow"
    LIVE = "live"
    KILLED = "killed"


# The C sidecar's ABI mode codes (arc.h / design 03). CFFI users pass
# these integers, not the Python enum.
_MODE_CODE: dict[ArcMode, int] = {
    ArcMode.SHADOW: 0,
    ArcMode.LIVE: 1,
    ArcMode.KILLED: 2,
}


class SidecarTransport(Protocol):
    """What the controller needs from the sidecar."""

    def stats(self) -> dict[str, int]:
        ...

    def set_mode(self, mode: ArcMode) -> None:
        ...


class CffiSidecarTransport:
    """The real transport: the C ARC sidecar driven through CFFI (W6.4).

    One sidecar per process (the C side is a singleton). ``pump()`` is
    the sidecar's poll interval: requested mode flips land there, which
    is design 03's "within one poll interval" contract, and entering
    killed drains the pacer backlog in that same pump.

    ``stats()`` maps ``arc_stats_t`` onto the controller's vocabulary;
    ``budget_alerts`` is the pacer's cumulative alert count.
    Use as a context manager or call ``close()``.
    """

    def __init__(
        self,
        *,
        capacity: int = 256,
        entry_max: int = 248,
        batch_max: int = 64,
        batch_deadline_ns: int = 2_000_000,
        budget_ns: int = 0,
        mode: ArcMode = ArcMode.SHADOW,
    ) -> None:
        try:
            from sdf_cffi import ffi, lib
        except ImportError as exc:  # pragma: no cover - build-state dependent
            raise RuntimeError(
                "CFFI extension unavailable; run ./scripts/build.sh first"
            ) from exc
        self.ffi: Any = ffi
        self.lib: Any = lib
        cfg = ffi.new("arc_cfg_t *")
        cfg.capacity = capacity
        cfg.entry_max = entry_max
        cfg.batch_max = batch_max
        cfg.batch_deadline_ns = batch_deadline_ns
        cfg.budget_ns = budget_ns
        cfg.mode = lib.ARC_MODE_QG
        rc = lib.arc_sidecar_start(cfg, _MODE_CODE[mode])
        if rc != 0:
            raise RuntimeError(f"arc_sidecar_start failed: {rc}")
        self._closed = False

    def stats(self) -> dict[str, int]:
        st = self.lib.arc_sidecar_stats()
        return {
            "mode": int(self.lib.arc_sidecar_mode()),
            "releases": int(st.releases),
            "in_flight": int(st.in_flight),
            "heap_depth": int(st.heap_depth),
            "budget_alerts": int(st.budget_alerts),
            "guard_waits": int(st.guard_waits),
            "guard_timeouts": int(st.guard_timeouts),
            "added_p50_ns": int(st.added_p50_ns),
            "added_p95_ns": int(st.added_p95_ns),
            "added_p99_ns": int(st.added_p99_ns),
            "applied": int(self.lib.arc_sidecar_applied()),
        }

    def set_mode(self, mode: ArcMode) -> None:
        """Record the desired mode; the next pump applies it."""
        rc = self.lib.arc_sidecar_set_mode(_MODE_CODE[ArcMode(mode)])
        if rc != 0:
            raise RuntimeError(f"arc_sidecar_set_mode({mode.value}) failed: {rc}")

    def pump(self) -> int:
        """One sidecar poll interval (submissions + releases)."""
        return int(self.lib.arc_sidecar_pump_once())

    def submit(self, entry: bytes, producer_ts: int | None = None) -> int:
        """Push one recovered entry; returns 0, or -EAGAIN when full."""
        if producer_ts is None:
            producer_ts = time.monotonic_ns()
        buf = self.ffi.new("char[]", entry)
        return int(self.lib.arc_sidecar_submit(buf, len(entry), producer_ts))

    def close(self) -> None:
        if not self._closed:
            self.lib.arc_sidecar_stop()
            self._closed = True

    def __enter__(self) -> CffiSidecarTransport:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()


@dataclass
class ArcDeployConfig:
    """Design 03's rollout gates as controller knobs."""

    shadow_min_seconds: int = 24 * 3600
    added_latency_budget_ns: int = 10_000_000  # 10 ms p99 (design 03)
    added_latency_budget_max_alerts: int = 3
    poll_interval_s: float = 1.0
    auto_kill: bool = False


@dataclass
class ArcDeployState:
    mode: ArcMode = ArcMode.SHADOW
    shadow_started_ns: int | None = None
    shadow_elapsed_s: float = 0.0
    budget_alerts_seen: int = 0
    recommendations: list[str] = field(default_factory=list)


class ArcDeployLoop:
    """One control interval per step(); the operator surface is the
    decision record, the transport applies the flips."""

    def __init__(self, transport: SidecarTransport,
                 config: ArcDeployConfig | None = None) -> None:
        self.transport = transport
        self.config = config or ArcDeployConfig()
        self.state = ArcDeployState()
        self._started = False

    def start(self) -> ArcDeployState:
        """Arm the loop in shadow mode (the design-03 starting state)."""
        self.state = ArcDeployState(
            mode=ArcMode.SHADOW,
            shadow_started_ns=time.monotonic_ns(),
        )
        self.transport.set_mode(ArcMode.SHADOW)
        self._started = True
        return self.state

    def step(self) -> dict[str, object]:
        """One interval: read the sidecar, advance the rollout state.

        Returns the decision record. Raises RuntimeError when the loop
        was never started (the sidecar must be armed before the
        controller runs — design 03's "kill switch always available"
        means the controller owns the mode from the start).
        """
        if not self._started:
            raise RuntimeError("start() must arm the loop before step()")

        stats = self.transport.stats()
        now = time.monotonic_ns()
        assert self.state.shadow_started_ns is not None
        self.state.shadow_elapsed_s = (
            (now - self.state.shadow_started_ns) / 1e9
        )

        record: dict[str, object] = {
            "mode": self.state.mode.value,
            "shadow_elapsed_s": self.state.shadow_elapsed_s,
            "budget_alerts": stats.get("budget_alerts", 0),
        }

        if self.state.mode == ArcMode.SHADOW:
            # promotion gate (design 03's rollout table): 24 h of shadow,
            # added-latency SLO clean
            slo_ok = (
                stats.get("budget_alerts", 0)
                <= self.config.added_latency_budget_max_alerts
            )
            if (
                self.state.shadow_elapsed_s >= self.config.shadow_min_seconds
                and slo_ok
            ):
                self.state.mode = ArcMode.LIVE
                self.transport.set_mode(ArcMode.LIVE)
                # Baseline the cumulative counter at promotion: alerts
                # observed during shadow are evidence, not fresh breaches
                # for the live watch (W6.4 fix — without this the first
                # live step called the whole shadow history "new").
                self.state.budget_alerts_seen = int(
                    stats.get("budget_alerts", 0)
                )
                self.state.recommendations.append(
                    f"promoted to live after {self.state.shadow_elapsed_s:.0f}s"
                )
                record["action"] = "promote"
            else:
                record["action"] = "hold-shadow"
            return record

        if self.state.mode == ArcMode.LIVE:
            alerts = stats.get("budget_alerts", 0)
            new_alerts = alerts - self.state.budget_alerts_seen
            self.state.budget_alerts_seen = alerts
            if new_alerts > 0:
                self.state.recommendations.append(
                    f"added-latency budget breach x{new_alerts}"
                )
                if self.config.auto_kill:
                    self.state.mode = ArcMode.KILLED
                    self.transport.set_mode(ArcMode.KILLED)
                    record["action"] = "auto-killed"
                else:
                    record["action"] = "breach-recommend-kill"
            else:
                record["action"] = "hold-live"
            return record

        record["action"] = "killed-idle"
        return record

    def kill(self) -> ArcMode:
        """The operator kill switch: flip to killed immediately."""
        self.state.mode = ArcMode.KILLED
        self.transport.set_mode(ArcMode.KILLED)
        self.state.recommendations.append("operator kill switch")
        return self.state.mode
