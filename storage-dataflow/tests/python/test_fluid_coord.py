"""FLUID control-plane policy tests (design 02, prompt W3.4/W3.6)."""

from __future__ import annotations

import math

import pytest

from sdf.control.fluid_coord import (
    CLASSES,
    COLD_CHECKPOINT,
    HOT_METADATA,
    Arm,
    EpsilonGate,
    FluidClass,
    ParetoSample,
    ParetoTracker,
    SenderBudget,
    assign_arm,
    budget_n,
    fluid_class,
)


def test_class_policies() -> None:
    assert HOT_METADATA.epsilon == 0.05
    assert HOT_METADATA.primary_arm is Arm.LT
    assert COLD_CHECKPOINT.epsilon == 0.15
    assert COLD_CHECKPOINT.primary_arm is Arm.NANORQ
    assert fluid_class("hot-metadata") is HOT_METADATA
    assert set(CLASSES) == {"hot-metadata", "cold-checkpoint"}
    with pytest.raises(KeyError):
        fluid_class("missing")
    with pytest.raises(ValueError):
        FluidClass("bad", 0.0, Arm.LT)
    with pytest.raises(ValueError):
        FluidClass("bad", 0.5, Arm.CONTROL)


def test_budget_n() -> None:
    assert budget_n(64, 0.0) == 64
    assert budget_n(64, 0.1) == 72  # ceil(64 / 0.9)
    assert budget_n(100, 0.2) == 125  # exact integer snaps down
    assert budget_n(1, 0.5) == 2
    with pytest.raises(ValueError):
        budget_n(0, 0.1)
    with pytest.raises(ValueError):
        budget_n(10, 1.0)


def test_sender_budget_count_feedback() -> None:
    budget = SenderBudget(100, 0.1)
    assert budget.n == 112
    assert budget.allowable == 112
    for _ in range(112):
        assert budget.admit()
    assert not budget.admit()

    budget.report(100)  # 12 lost -> allowable 124
    assert budget.allowable == 124
    for _ in range(12):
        assert budget.admit()
    assert not budget.admit()

    budget.report(90)  # cumulative loss is now 124 - 90
    assert budget.allowable == 146
    budget.report(120)  # lower loss cannot shrink it
    assert budget.allowable == 146
    with pytest.raises(ValueError):
        budget.report(-1)


def test_epsilon_gate_zero_loss_certifies() -> None:
    gate = EpsilonGate(0.05, 100)
    assert not gate.done()
    assert gate.observe(100, 100) == 0.0
    assert gate.done()


def test_epsilon_gate_tracks_and_rejects_stale_reports() -> None:
    gate = EpsilonGate(0.05, 100)
    assert gate.observe(100, 90) == pytest.approx(0.10)
    assert not gate.done()

    assert gate.observe(90, 80) == pytest.approx(0.10)  # same cumulative loss
    assert gate.observe(80, 78) == pytest.approx(0.10)  # lower: stale, ignored
    assert gate.observe(120, 118) == pytest.approx(0.10)  # still below lost_prev
    assert gate.rounds == 2
    assert not gate.done()

    fresh = EpsilonGate(0.05, 100)
    assert fresh.observe(120, 118) == pytest.approx(0.02)
    assert fresh.done()


def test_epsilon_gate_validation() -> None:
    with pytest.raises(ValueError):
        EpsilonGate(1.0, 100)
    with pytest.raises(ValueError):
        EpsilonGate(0.1, 0)
    gate = EpsilonGate(0.1, 10)
    with pytest.raises(ValueError):
        gate.observe(-1, 0)


def test_python_policy_matches_c_helpers() -> None:
    pytest.importorskip("sdf_cffi")
    from sdf_cffi import ffi, lib

    for k, epsilon in ((1, 0.5), (64, 0.0), (64, 0.1), (100, 0.2), (257, 0.15)):
        assert budget_n(k, epsilon) == int(lib.sdf_fluid_budget_n(k, epsilon))

    reports = ((100, 90), (90, 80), (80, 78), (120, 118))
    py_gate = EpsilonGate(0.05, 100)
    c_gate = ffi.new("fluid_gate_t *")
    lib.fg_init(c_gate, 0.05, 100)
    for sent, recv in reports:
        py_value = py_gate.observe(sent, recv)
        lib.fg_observe(c_gate, sent, recv)
        assert py_value == pytest.approx(c_gate.loss_prod)
        assert py_gate.done() == bool(lib.fg_done(c_gate))


def test_arm_assignment_is_deterministic_and_bounded() -> None:
    assert assign_arm(HOT_METADATA, 17, 0.0) is Arm.CONTROL
    assert assign_arm(HOT_METADATA, 17, 100.0) is Arm.LT
    assert assign_arm(COLD_CHECKPOINT, 17, 100.0) is Arm.NANORQ

    first = assign_arm(HOT_METADATA, 42, 50.0)
    assert assign_arm(HOT_METADATA, 42, 50.0) is first

    arms = [assign_arm(HOT_METADATA, job, 50.0) for job in range(1000)]
    primary = arms.count(Arm.LT)
    control = arms.count(Arm.CONTROL)
    assert primary + control == 1000
    assert 350 <= primary <= 650

    with pytest.raises(ValueError):
        assign_arm(HOT_METADATA, -1, 50.0)
    with pytest.raises(ValueError):
        assign_arm(HOT_METADATA, 1, 101.0)
    with pytest.raises(ValueError):
        assign_arm(HOT_METADATA, 1, math.nan)


def test_pareto_frontier_and_best() -> None:
    tracker = ParetoTracker()
    control = ParetoSample(Arm.CONTROL, 1.0, 100.0)
    lt = ParetoSample(Arm.LT, 1.05, 80.0)
    dominated = ParetoSample(Arm.LT, 1.10, 95.0)
    nanorq = ParetoSample(Arm.NANORQ, 1.20, 90.0)
    for sample in (control, lt, dominated, nanorq):
        tracker.record(sample)

    assert tracker.frontier() == (control, lt)
    assert tracker.best() == lt
    assert tracker.best(max_overhead=1.02) == control
    assert tracker.best(max_overhead=0.99) is None

    tracker.clear()
    assert tracker.frontier() == ()


def test_pareto_validation() -> None:
    with pytest.raises(ValueError):
        ParetoSample(Arm.LT, 0.0, 10.0)
    with pytest.raises(ValueError):
        ParetoSample(Arm.LT, 1.0, math.inf)
