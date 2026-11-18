"""Whack-a-Mole soft-shedding controller.

See `docs/design/01-whack-a-mole.md` and `docs/design/00-overview.md` section 7.3.
"""

from __future__ import annotations

import statistics
from dataclasses import dataclass


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
