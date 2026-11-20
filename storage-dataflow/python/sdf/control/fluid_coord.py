"""FLUID repair coordinator (design 02, overview §7.4).

The C data plane owns block repair and the never-ship-incomplete guarantee.
This control-plane module owns the decisions around it:

* per-class epsilon policy (``hot-metadata``, ``cold-checkpoint``);
* the sender budget and cumulative-loss gate, matching ``fluid.h``;
* deterministic control/experimental-arm assignment; and
* Pareto tracking of repair p99 against bandwidth overhead.

The arm assignment uses ``sdf.common.ab.bucket`` (xxhash64 modulo 100), so the
same job id maps to the same arm across processes and machines.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from enum import Enum

from sdf.common.ab import bucket


class Arm(str, Enum):
    """The three-arm experiment from design 02."""

    CONTROL = "control"
    LT = "lt"
    NANORQ = "nanorq"


@dataclass(frozen=True)
class FluidClass:
    """Per-class policy from design 02's class table."""

    name: str
    epsilon: float
    primary_arm: Arm

    def __post_init__(self) -> None:
        if not self.name:
            raise ValueError("class name must not be empty")
        if not 0.0 < self.epsilon < 1.0:
            raise ValueError("epsilon must be in (0, 1)")
        if self.primary_arm is Arm.CONTROL:
            raise ValueError("the experimental arm must be LT or NANORQ")


HOT_METADATA = FluidClass("hot-metadata", 0.05, Arm.LT)
COLD_CHECKPOINT = FluidClass("cold-checkpoint", 0.15, Arm.NANORQ)
CLASSES: dict[str, FluidClass] = {policy.name: policy for policy in (HOT_METADATA, COLD_CHECKPOINT)}


def fluid_class(name: str) -> FluidClass:
    """Return a named class policy or raise ``KeyError``."""
    return CLASSES[name]


def budget_n(k: int, epsilon: float) -> int:
    """Sender budget ``ceil(K / (1 - epsilon))`` with the near-integer snap."""
    if k <= 0:
        raise ValueError("k must be positive")
    if not 0.0 <= epsilon < 1.0:
        raise ValueError("epsilon must be in [0, 1)")
    if epsilon == 0.0:
        return k
    exact = k / (1.0 - epsilon)
    rounded = math.floor(exact)
    if exact - rounded < 1e-9:
        return int(rounded)
    return math.ceil(exact)


class SenderBudget:
    """Count-feedback budget matching ``sdf_fluid_budget_*`` in C."""

    def __init__(self, k: int, epsilon: float) -> None:
        self.k = k
        self.epsilon = epsilon
        self.n = budget_n(k, epsilon)
        self.sent = 0
        self.reported_lost = 0

    @property
    def allowable(self) -> int:
        return self.n + self.reported_lost

    def admit(self) -> bool:
        """Admit one frame, or return ``False`` when the budget is exhausted."""
        if self.sent >= self.allowable:
            return False
        self.sent += 1
        return True

    def report(self, received: int) -> None:
        """Fold a cumulative received count; losses never decrease."""
        if received < 0:
            raise ValueError("received must be non-negative")
        lost = max(0, self.sent - received)
        self.reported_lost = max(self.reported_lost, lost)


class EpsilonGate:
    """Cumulative-loss gate matching ``fluid_gate_t`` in ``fluid.h``.

    ``loss_prod`` is the telescoped section 4.3 quotient ``L_l / N``, not a
    multiplicative fold. A reordered report with a lower cumulative loss is
    ignored; zero losses certify immediately.
    """

    def __init__(self, epsilon: float, budget: int) -> None:
        if not 0.0 <= epsilon < 1.0:
            raise ValueError("epsilon must be in [0, 1)")
        if budget <= 0:
            raise ValueError("budget must be positive")
        self.epsilon = epsilon
        self.budget = budget
        self.rounds = 0
        self.loss_prod = 1.0
        self.lost_prev = 0

    def observe(self, sent_total: int, recv_total: int) -> float:
        """Fold one cumulative count report and return ``loss_prod``."""
        if sent_total < 0 or recv_total < 0:
            raise ValueError("cumulative counts must be non-negative")
        lost = max(0, sent_total - recv_total)
        if self.rounds > 0 and lost < self.lost_prev:
            return self.loss_prod
        self.loss_prod = 0.0 if lost == 0 else lost / self.budget
        self.lost_prev = lost
        self.rounds += 1
        return self.loss_prod

    def done(self) -> bool:
        return self.loss_prod <= self.epsilon


def assign_arm(policy: FluidClass, job_id: int, rollout_pct: float) -> Arm:
    """Assign a job to the class's experimental arm or the control arm.

    ``rollout_pct`` is 0..100. Zero selects control, 100 selects the class's
    primary arm, and intermediate values use the deterministic xxhash64
    bucket. The class name is part of the key so two classes with the same job
    id do not share a bucket.
    """
    if job_id < 0:
        raise ValueError("job_id must be non-negative")
    if not math.isfinite(rollout_pct) or not 0.0 <= rollout_pct <= 100.0:
        raise ValueError("rollout_pct must be in [0, 100]")
    if rollout_pct == 0.0:
        return Arm.CONTROL
    if rollout_pct == 100.0:
        return policy.primary_arm
    key = f"{policy.name}:{job_id}"
    return policy.primary_arm if bucket(key, 100) < rollout_pct else Arm.CONTROL


@dataclass(frozen=True)
class ParetoSample:
    """One arm's repair observation: lower p99 and lower overhead are better."""

    arm: Arm
    overhead_ratio: float
    p99_ns: float

    def __post_init__(self) -> None:
        if not math.isfinite(self.overhead_ratio) or self.overhead_ratio <= 0.0:
            raise ValueError("overhead_ratio must be finite and positive")
        if not math.isfinite(self.p99_ns) or self.p99_ns <= 0.0:
            raise ValueError("p99_ns must be finite and positive")

    def dominates(self, other: ParetoSample) -> bool:
        no_worse = self.overhead_ratio <= other.overhead_ratio and self.p99_ns <= other.p99_ns
        strictly_better = self.overhead_ratio < other.overhead_ratio or self.p99_ns < other.p99_ns
        return no_worse and strictly_better


@dataclass
class ParetoTracker:
    """Collects arm samples and exposes the non-dominated frontier."""

    samples: list[ParetoSample] = field(default_factory=list)

    def record(self, sample: ParetoSample) -> None:
        self.samples.append(sample)

    def frontier(self) -> tuple[ParetoSample, ...]:
        kept = [
            sample
            for sample in self.samples
            if not any(other is not sample and other.dominates(sample) for other in self.samples)
        ]
        return tuple(
            sorted(
                kept,
                key=lambda sample: (
                    sample.overhead_ratio,
                    sample.p99_ns,
                    sample.arm.value,
                ),
            )
        )

    def best(self, max_overhead: float | None = None) -> ParetoSample | None:
        """Return the lowest-p99 sample within an optional overhead ceiling."""
        candidates = [
            sample
            for sample in self.frontier()
            if max_overhead is None or sample.overhead_ratio <= max_overhead
        ]
        if not candidates:
            return None
        return min(
            candidates,
            key=lambda sample: (sample.p99_ns, sample.overhead_ratio, sample.arm.value),
        )

    def clear(self) -> None:
        self.samples.clear()
