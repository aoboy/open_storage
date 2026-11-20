"""ARC offset-estimator simulator tests."""

from __future__ import annotations

import pytest

from sdf.sim.arc_sim import ArcMode, ArcOffsetSimulator, ArcParams, simulate


def solid_params() -> ArcParams:
    return ArcParams(
        rho_u=1.0,
        rho_l=1.0,
        lambda_u=0.16,
        lambda_l=0.04,
        U=100_000_000,
        delta=100_000_000,
        J=0,
        gamma=16_666_667,
        G=50_000_000,
        T_idle=1_000_000_000_000,
        mode=ArcMode.QG,
    )


def test_linear_cycle_reaches_golden_fixed_point() -> None:
    delays = [100_000_000 if index % 2 == 0 else 70_000_000 for index in range(500)]
    schedules = simulate(delays, solid_params())
    assert schedules[-2].D == pytest.approx(94_793_388.43, abs=1.0)
    assert schedules[-1].D == pytest.approx(93_801_652.89, abs=1.0)
    assert schedules[-1].E == pytest.approx(108_333_333.5, abs=1.0)


def test_initialization_and_quantized_offset() -> None:
    sim = ArcOffsetSimulator(solid_params())
    schedule = sim.on_arrival(0, 50_000_000)
    assert schedule.D == 50_000_000
    assert schedule.E == 58_333_333.5
    assert schedule.t_n == 58_333_334


def test_idle_reanchor() -> None:
    sim = ArcOffsetSimulator(ArcParams(T_idle=100_000_000))
    first = sim.on_arrival(0, 50_000_000)
    assert first.D == 50_000_000
    sim.on_arrival(1_000_000_000, 1_020_000_000)
    assert sim.idle_reanchors == 1
    assert sim.D == 20_000_000


def test_absurd_stamp_does_not_learn() -> None:
    sim = ArcOffsetSimulator(solid_params())
    first = sim.on_arrival(0, 50_000_000)
    rejected = sim.on_arrival(20_000_000_000, 1_000_000_000)
    assert sim.s_n_rejects == 1
    assert rejected.D == first.D
    assert rejected.t_n == 1_000_000_000


def test_base_mode_uses_d_and_small_bypass() -> None:
    base = simulate([50_000_000], ArcParams(mode=ArcMode.BASE))
    assert base[0].t_n == 50_000_000
    bypass = simulate([50_000_000], ArcParams(min_smooth_bytes=1024), start_s_ns=0)
    assert bypass[0].t_n == 50_000_000


def test_validation() -> None:
    with pytest.raises(ValueError):
        ArcParams(U=0)
    with pytest.raises(ValueError):
        simulate([-1])
    with pytest.raises(ValueError):
        simulate([1], step_s_ns=-1)
