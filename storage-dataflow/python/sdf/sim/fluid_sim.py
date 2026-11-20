"""FLUID budget/gate simulator (design 02).

This deterministic model covers the sender budget and cumulative-loss gate,
not codec internals: one wave at the budget N, a requested symbol-loss
percentage, and the resulting certification state. It is useful for exploring
slack and fallback behavior without fabricating a codec decode result.
"""

from __future__ import annotations

import argparse
from collections.abc import Sequence
from dataclasses import dataclass

from sdf.control.fluid_coord import EpsilonGate, budget_n


@dataclass(frozen=True)
class FluidRound:
    """One deterministic budget/gate observation."""

    k: int
    epsilon: float
    loss_pct: float
    budget: int
    sent: int
    received: int
    loss_product: float
    gate_done: bool
    arq_fallback: bool
    efficiency: float
    overhead_ratio: float


def simulate_round(k: int, epsilon: float, loss_pct: float) -> FluidRound:
    """Apply one deterministic loss observation to the FLUID gate."""
    if k <= 0:
        raise ValueError("k must be positive")
    if not 0.0 <= epsilon < 1.0:
        raise ValueError("epsilon must be in [0, 1)")
    if not 0.0 <= loss_pct <= 100.0:
        raise ValueError("loss_pct must be in [0, 100]")

    budget = budget_n(k, epsilon)
    received = round(budget * (100.0 - loss_pct) / 100.0)
    received = min(budget, max(0, received))
    gate = EpsilonGate(epsilon, budget)
    loss_product = gate.observe(budget, received)
    done = gate.done()
    return FluidRound(
        k=k,
        epsilon=epsilon,
        loss_pct=loss_pct,
        budget=budget,
        sent=budget,
        received=received,
        loss_product=loss_product,
        gate_done=done,
        arq_fallback=not done,
        efficiency=k / budget,
        overhead_ratio=budget / k,
    )


def sweep(k: int, epsilon: float, losses: Sequence[float]) -> list[FluidRound]:
    """Run a loss ladder and return one row per loss percentage."""
    return [simulate_round(k, epsilon, loss) for loss in losses]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--k", type=int, default=64)
    parser.add_argument("--eps", type=float, default=0.10)
    parser.add_argument("--losses", default="0,5,10,15,20,30,40,50")
    args = parser.parse_args(argv)

    losses = [float(token) for token in args.losses.split(",") if token.strip()]
    print("loss_pct,budget,sent,received,loss_product,gate_done,arq_fallback,overhead_ratio")
    for row in sweep(args.k, args.eps, losses):
        print(
            f"{row.loss_pct:.0f},{row.budget},{row.sent},{row.received},"
            f"{row.loss_product:.6f},{int(row.gate_done)},{int(row.arq_fallback)},"
            f"{row.overhead_ratio:.4f}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
