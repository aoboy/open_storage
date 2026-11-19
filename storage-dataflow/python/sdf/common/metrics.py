"""Control-plane metric registration, export, and liveness (W6.14).

Every instance owns a private Prometheus registry: independent controllers and
tests never contend for global metric names. Label domains are deliberately
small and validated before they reach ``prometheus_client``.
"""

from __future__ import annotations

import math
import time
from collections.abc import Callable, Sequence
from typing import Final

from prometheus_client import CollectorRegistry, Counter, Gauge, generate_latest

MAX_PATHS: Final = 64
LOOPS: Final = frozenset({"wm", "fluid", "arc"})
ARC_MODES: Final = ("shadow", "live", "killed")
WARNING_MISSED_INTERVALS: Final = 2
PAGE_MISSED_INTERVALS: Final = 10


def _loop_name(loop: str) -> str:
    if loop not in LOOPS:
        raise ValueError(f"loop must be one of {sorted(LOOPS)}")
    return loop


class SdfMetrics:
    """Registered control-plane series with bounded labels.

    ``registry`` is public so callers can serve it with their existing HTTP
    stack or pass it directly to ``prometheus_client.generate_latest``.
    """

    def __init__(self, registry: CollectorRegistry | None = None) -> None:
        self.registry = registry or CollectorRegistry()
        self.wm_steps = Counter(
            "sdf_wm_control_steps_total", "WM control intervals completed", registry=self.registry
        )
        self.wm_version = Gauge(
            "sdf_wm_control_version", "Last published WM profile version", registry=self.registry
        )
        self.wm_units = Gauge(
            "sdf_wm_control_units",
            "Published units by bounded path id",
            ["path"],
            registry=self.registry,
        )
        self.wm_fresh_paths = Gauge(
            "sdf_wm_control_fresh_paths", "Paths with fresh telemetry", registry=self.registry
        )
        self.wm_down_mask = Gauge(
            "sdf_wm_control_down_mask", "C fast-path down mask", registry=self.registry
        )
        self.arc_steps = Counter(
            "sdf_arc_deploy_steps_total",
            "ARC deployment intervals completed",
            registry=self.registry,
        )
        self.arc_mode = Gauge(
            "sdf_arc_deploy_mode",
            "ARC deployment mode (one active sample)",
            ["mode"],
            registry=self.registry,
        )
        self.arc_shadow_elapsed = Gauge(
            "sdf_arc_shadow_elapsed_seconds",
            "Current shadow epoch elapsed time",
            registry=self.registry,
        )
        self.liveness_missed = Gauge(
            "sdf_control_loop_missed_intervals",
            "Whole control intervals missed since the last heartbeat",
            ["loop"],
            registry=self.registry,
        )
        self.liveness = LivenessMonitor(self)

    def export(self) -> bytes:
        """Return the Prometheus text exposition for this registry."""
        return generate_latest(self.registry)

    def record_wm_step(
        self, *, version: int, units: Sequence[int], down_mask: int, fresh_paths: int
    ) -> None:
        if not 0 < len(units) <= MAX_PATHS:
            raise ValueError(f"units must contain 1..{MAX_PATHS} paths")
        if version < 0 or down_mask < 0 or not 0 <= fresh_paths <= len(units):
            raise ValueError("invalid WM control sample")
        if any(value < 0 for value in units):
            raise ValueError("path units must be non-negative")
        self.wm_steps.inc()
        self.wm_version.set(version)
        self.wm_down_mask.set(down_mask)
        self.wm_fresh_paths.set(fresh_paths)
        for path, value in enumerate(units):
            self.wm_units.labels(path=str(path)).set(value)
        self.liveness.beat("wm", interval_s=0.25)

    def record_arc_step(
        self, *, mode: str, shadow_elapsed_s: float, interval_s: float = 1.0
    ) -> None:
        self.record_arc_mode(mode)
        if not math.isfinite(shadow_elapsed_s) or shadow_elapsed_s < 0:
            raise ValueError("shadow elapsed time must be finite and non-negative")
        self.arc_steps.inc()
        self.arc_shadow_elapsed.set(shadow_elapsed_s)
        self.liveness.beat("arc", interval_s=interval_s)

    def record_arc_mode(self, mode: str) -> None:
        if mode not in ARC_MODES:
            raise ValueError(f"mode must be one of {ARC_MODES}")
        for candidate in ARC_MODES:
            self.arc_mode.labels(mode=candidate).set(1 if candidate == mode else 0)

    def sample_liveness(
        self, loop: str, *, interval_s: float | None = None, now_ns: int | None = None
    ) -> int:
        return self.liveness.sample(loop, interval_s=interval_s, now_ns=now_ns)


class LivenessMonitor:
    """Heartbeat tracker using monotonic time and whole missed intervals.

    Before the first heartbeat a loop reports zero missed intervals. Warning
    begins at two missed intervals and page at ten, matching design 05.
    """

    def __init__(
        self,
        metrics: SdfMetrics | None = None,
        clock: Callable[[], int] = time.monotonic_ns,
    ) -> None:
        self.metrics = metrics
        self.clock = clock
        self.last_beat_ns: dict[str, int] = {}
        self.intervals_s: dict[str, float] = {}

    @staticmethod
    def _interval(loop: str, interval_s: float | None, current: dict[str, float]) -> float:
        value = interval_s if interval_s is not None else current.get(loop, 1.0)
        if not math.isfinite(value) or value <= 0:
            raise ValueError("interval_s must be finite and positive")
        current[loop] = value
        return value

    def _publish(self, loop: str, missed: int) -> None:
        if self.metrics is not None:
            self.metrics.liveness_missed.labels(loop=loop).set(missed)

    def beat(
        self, loop: str, *, interval_s: float | None = None, now_ns: int | None = None
    ) -> None:
        loop = _loop_name(loop)
        self._interval(loop, interval_s, self.intervals_s)
        self.last_beat_ns[loop] = self.clock() if now_ns is None else now_ns
        self._publish(loop, 0)

    def sample(
        self, loop: str, *, interval_s: float | None = None, now_ns: int | None = None
    ) -> int:
        loop = _loop_name(loop)
        interval = self._interval(loop, interval_s, self.intervals_s)
        if loop not in self.last_beat_ns:
            self._publish(loop, 0)
            return 0
        now = self.clock() if now_ns is None else now_ns
        elapsed_ns = max(0, now - self.last_beat_ns[loop])
        missed = int(elapsed_ns // int(interval * 1_000_000_000))
        self._publish(loop, missed)
        return missed

    def is_healthy(
        self, loop: str, *, interval_s: float | None = None, now_ns: int | None = None
    ) -> bool:
        """False at the warning boundary (two missed intervals)."""
        return self.sample(loop, interval_s=interval_s, now_ns=now_ns) < WARNING_MISSED_INTERVALS

    def is_live(
        self, loop: str, *, interval_s: float | None = None, now_ns: int | None = None
    ) -> bool:
        """False at the page boundary (ten missed intervals)."""
        return self.sample(loop, interval_s=interval_s, now_ns=now_ns) < PAGE_MISSED_INTERVALS
