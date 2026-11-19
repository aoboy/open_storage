"""Whack-a-Mole soft-shedding controller.

See `docs/design/01-whack-a-mole.md` and `docs/design/00-overview.md` section 7.3.
"""

from __future__ import annotations

import statistics
import time
from dataclasses import dataclass
from typing import TYPE_CHECKING

from sdf.common.shm import TelemetrySlot, WmShm

if TYPE_CHECKING:
    from sdf.common.metrics import SdfMetrics


@dataclass
class PathTelemetry:
    """Per-path counters observed over one control interval."""

    sent: list[int]
    acked: list[int]
    ewma_lat_ns: list[float]
    ewma_err: list[float]

    def scores(self) -> list[float]:
        """Return a throughput score per path, penalised by latency and errors."""
        scores: list[float] = []
        for sent, acked, lat_ns, err in zip(
            self.sent, self.acked, self.ewma_lat_ns, self.ewma_err, strict=True
        ):
            throughput = (acked / sent) if sent else 0.0
            scores.append(throughput / (1.0 + lat_ns / 1e6 + err))
        return scores


def renormalize(units: list[int], m: int) -> list[int]:
    """Scale ``units`` so they sum to ``m`` using the largest-remainder method.

    The tie-break is by index, which keeps the result deterministic across
    runs and processes.
    """
    if m <= 0:
        raise ValueError("m must be positive")
    if not units:
        raise ValueError("units must not be empty")
    total = sum(units)
    if total <= 0:
        raise ValueError("units must sum to a positive value")
    if total == m:
        return list(units)

    scaled = [u * m // total for u in units]
    remainder = m - sum(scaled)
    order = sorted(
        range(len(units)),
        key=lambda i: (-(units[i] * m - scaled[i] * total), i),
    )
    for i in order[:remainder]:
        scaled[i] += 1
    return scaled


class SoftSheddingController:
    """Multiplicative decrease / additive increase over integer path units."""

    def __init__(
        self,
        n: int,
        m: int = 1024,
        alpha: float = 0.25,
        beta: float = 0.02,
    ) -> None:
        if n <= 0:
            raise ValueError("n must be positive")
        if m < n:
            raise ValueError("m must be at least n so every path has one unit")
        if not 0.0 < alpha < 1.0:
            raise ValueError("alpha must be in (0, 1)")
        if not 0.0 < beta < 1.0:
            raise ValueError("beta must be in (0, 1)")

        self.n = n
        self.m = m
        self.alpha = alpha
        self.beta = beta
        self.units = renormalize([m // n] * n, m)

    def step_with_opinions(self, opinions: list[tuple[int, int, float, float] | None]) -> list[int]:
        """Soft shedding over per-path opinions.

        ``opinions[i]`` is ``(sent, acked, ewma_lat_ns, ewma_err)`` or
        ``None`` for *no opinion*: a path with no fresh telemetry keeps
        its current units (neither decreased nor increased) but still
        participates in the renormalization so the sum stays m. The
        reference score (median) is computed over opinion paths only —
        design 01's missing-telemetry rule.
        """
        scores: list[float | None] = []
        for obs in opinions:
            if obs is None:
                scores.append(None)
                continue
            sent, acked, lat_ns, err = obs
            throughput = (acked / sent) if sent else 0.0
            scores.append(throughput / (1.0 + lat_ns / 1e6 + err))

        present = [s for s in scores if s is not None]
        reference = statistics.median(present) if present else 1.0

        for i, score in enumerate(scores):
            if score is None:
                continue
            if score < reference * (1.0 - self.alpha):
                self.units[i] = max(1, int(self.units[i] * (1.0 - self.alpha)))
            elif score > reference:
                grow = max(1, int(self.m * self.beta))
                self.units[i] = min(self.m, self.units[i] + grow)

        return renormalize(self.units, self.m)

    def step(self, telemetry: PathTelemetry) -> list[int]:
        """Apply one control interval and return the new unit allocation."""
        if len(telemetry.sent) != self.n:
            raise ValueError(f"expected telemetry for {self.n} paths")

        scores = telemetry.scores()
        reference = statistics.median(scores) if scores else 1.0

        for i, score in enumerate(scores):
            if score < reference * (1.0 - self.alpha):
                self.units[i] = max(1, int(self.units[i] * (1.0 - self.alpha)))
            elif score > reference:
                grow = max(1, int(self.m * self.beta))
                self.units[i] = min(self.m, self.units[i] + grow)

        self.units = renormalize(self.units, self.m)
        return self.units


# ---- W2.4: the live control loop (plan prompt, design 01) --------------


class WmControlLoop:
    """4 Hz loop closing against a live WM SHM segment.

    Per interval: drain the telemetry ring, score (a path with no fresh
    telemetry is *no opinion* — it keeps its units this interval), apply
    multiplicative decrease / additive increase, renormalize, publish
    into the inactive buffer and flip (ADR-0002). Paths in the C fast
    path's down_mask get the same deference for one interval — the
    control loop never undoes a C hard-down decision (design 01, open
    question 3). The heartbeat counter is the liveness metric: it must
    advance every step.
    """

    def __init__(
        self,
        shm: WmShm,
        m: int = 1024,
        alpha: float = 0.25,
        beta: float = 0.02,
        *,
        metrics: SdfMetrics | None = None,
    ) -> None:
        n = shm.n_paths
        self.shm = shm
        self.metrics = metrics
        self.n = n
        self.controller = SoftSheddingController(n, m=m, alpha=alpha, beta=beta)
        self.version = 0
        self.heartbeat = 0
        self.last_step_ns: int | None = None

    def step(self) -> dict[str, object]:
        """One control interval. Returns the decision record."""
        slots: list[TelemetrySlot] = self.shm.read_telemetry()
        mask = self.shm.down_mask()

        # latest snapshot per path; absent => no opinion
        latest: dict[int, tuple[int, int, float, float]] = {}
        for slot in slots:
            (
                pid,
                _pad,
                reqs_out,
                reqs_done,
                _reqs_failed,
                _bytes,
                _inflight,
                ewma_lat,
                ewma_err,
            ) = slot
            latest[pid] = (reqs_out, reqs_done, ewma_lat, ewma_err)

        # C hard-down deference: a masked path carries no opinion this
        # interval — its units were already shed on the C side and
        # growing them back here would fight the fast path
        for pid in list(latest):
            if mask & (1 << pid):
                del latest[pid]

        opinions = []
        for pid in range(self.n):
            obs = latest.get(pid)
            opinions.append(None if obs is None else obs)

        units = self.controller.step_with_opinions(opinions)
        self.version += 1
        self.heartbeat += 1
        self.last_step_ns = time.monotonic_ns()
        self.shm.publish(self.version, self.controller.m, self.n, units)
        if self.metrics is not None:
            self.metrics.record_wm_step(
                version=self.version,
                units=units,
                down_mask=mask,
                fresh_paths=len(latest),
            )
        return {
            "version": self.version,
            "units": units,
            "heartbeat": self.heartbeat,
            "down_mask": mask,
            "fresh_paths": len(latest),
        }
