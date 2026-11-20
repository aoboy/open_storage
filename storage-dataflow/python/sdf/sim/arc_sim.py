"""ARC offset-estimator simulator (design 03).

This is a deterministic model of the Algorithm-1 offset update and Algorithm-2
quantized E hysteresis, not the C release heap. It is useful for exploring
convergence, idle re-anchoring, and absurd-stamp behavior without wall-clock
sampling.
"""

from __future__ import annotations

import argparse
import math
from collections.abc import Sequence
from dataclasses import dataclass
from enum import IntEnum

ARC_SANE_NS = 10_000_000_000


class ArcMode(IntEnum):
    BASE = 0
    Q = 1
    QG = 2
    IMMEDIATE = 3


@dataclass(frozen=True)
class ArcParams:
    rho_u: float = 0.5
    rho_l: float = 1.5
    lambda_u: float = 0.05
    lambda_l: float = 0.03
    U: int = 100_000_000
    delta: int = 50_000_000
    gamma: int = 16_666_667
    J: int = 0
    G: int = 50_000_000
    T_idle: int = 1_000_000_000
    mode: ArcMode = ArcMode.QG
    min_smooth_bytes: int = 0

    def __post_init__(self) -> None:
        if not 0.0 <= self.rho_u <= 1.0 or not 0.0 <= self.rho_l <= 2.0:
            raise ValueError("rho values out of range")
        if not 0.0 <= self.lambda_u <= 1.0 or not 0.0 <= self.lambda_l <= 0.1:
            raise ValueError("lambda values out of range")
        if self.U <= 0 or self.delta < 0 or self.gamma < 0 or self.J < 0:
            raise ValueError("U/delta/gamma/J invalid")
        if self.T_idle < 0 or self.G < 0 or self.min_smooth_bytes < 0:
            raise ValueError("T_idle/G/min_smooth_bytes invalid")


@dataclass(frozen=True)
class ArcSchedule:
    block_n: int
    s_n: int
    a_n: int
    D: float
    E: float
    t_n: int


class ArcOffsetSimulator:
    """Stateful Python mirror of the estimator portion of arc_arrival()."""

    def __init__(self, params: ArcParams | None = None) -> None:
        self.params = params or ArcParams()
        self.D = 0.0
        self.E = 0.0
        self.tau_last_ns = 0
        self.initialized = False
        self.block_n = 0
        self.idle_reanchors = 0
        self.s_n_rejects = 0

    def on_arrival(self, s_n: int, a_n: int, length: int = 64) -> ArcSchedule:
        p = self.params
        block_n = self.block_n
        self.block_n += 1

        if p.mode is ArcMode.IMMEDIATE:
            return ArcSchedule(block_n, s_n, a_n, self.D, self.E, a_n)
        if length < p.min_smooth_bytes:
            return ArcSchedule(block_n, s_n, a_n, self.D, self.E, a_n)
        if a_n < s_n and s_n - a_n > ARC_SANE_NS:
            self.s_n_rejects += 1
            return ArcSchedule(block_n, s_n, a_n, self.D, self.E, a_n)

        if not self.initialized:
            self.D = max(0.0, float(a_n - s_n))
            self.E = self.D + p.gamma / 2.0
            self.tau_last_ns = a_n
            self.initialized = True
        else:
            gap = a_n - self.tau_last_ns
            if gap >= p.T_idle:
                self.D = max(0.0, float(a_n - s_n))
                self.idle_reanchors += 1
            self.tau_last_ns = a_n

        x = float(a_n) - (float(s_n) + self.D)
        if x > 0.0:
            clipped = min(max(x, 0.0), float(p.U))
            y = math.pow(clipped / p.U, p.rho_u) * p.U
            self.D += p.lambda_u * y
        elif x + float(p.J) <= 0.0:
            clipped = min(max(abs(x + p.J), 0.0), float(p.U))
            y = -math.pow(clipped / p.U, p.rho_l) * p.U
            self.D += p.lambda_l * y

        dmax = float(a_n - s_n + p.delta)
        self.D = min(self.D, dmax)
        self.D = max(self.D, 0.0)

        if p.mode >= ArcMode.Q and (self.D > self.E or self.E - p.gamma > self.D):
            self.E = self.D + p.gamma / 2.0
        offset = self.E if p.mode >= ArcMode.Q else self.D
        t_n = max(a_n, round(float(s_n) + offset + p.J))
        return ArcSchedule(block_n, s_n, a_n, self.D, self.E, t_n)


def simulate(
    delays: Sequence[int],
    params: ArcParams | None = None,
    *,
    start_s_ns: int = 0,
    step_s_ns: int = 1_000_000,
) -> list[ArcSchedule]:
    """Run one arrival per delay, with s_n advancing by ``step_s_ns``."""
    if start_s_ns < 0 or step_s_ns < 0:
        raise ValueError("start_s_ns and step_s_ns must be non-negative")
    sim = ArcOffsetSimulator(params)
    out: list[ArcSchedule] = []
    for index, delay in enumerate(delays):
        if delay < 0:
            raise ValueError("delays must be non-negative")
        s_n = start_s_ns + index * step_s_ns
        out.append(sim.on_arrival(s_n, s_n + delay))
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--d-high-ns", type=int, default=100_000_000)
    parser.add_argument("--d-low-ns", type=int, default=70_000_000)
    parser.add_argument("--blocks", type=int, default=2000)
    parser.add_argument("--gamma-ns", type=int, default=16_666_667)
    args = parser.parse_args(argv)

    delays = [args.d_high_ns if index % 2 == 0 else args.d_low_ns for index in range(args.blocks)]
    schedules = simulate(delays, ArcParams(gamma=args.gamma_ns))
    last = schedules[-1]
    print(f"blocks={args.blocks} D_ns={last.D:.1f} E_ns={last.E:.1f} t_n={last.t_n}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
