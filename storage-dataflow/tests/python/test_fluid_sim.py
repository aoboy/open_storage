"""FLUID budget/gate simulator tests."""

from __future__ import annotations

import pytest

from sdf.sim.fluid_sim import simulate_round, sweep


def test_zero_loss_certifies() -> None:
    row = simulate_round(64, 0.10, 0.0)
    assert row.budget == 72
    assert row.received == 72
    assert row.loss_product == 0.0
    assert row.gate_done
    assert not row.arq_fallback
    assert row.efficiency == pytest.approx(64 / 72)
    assert row.overhead_ratio == pytest.approx(72 / 64)


def test_slack_boundary_and_fallback() -> None:
    inside = simulate_round(64, 0.10, 10.0)
    assert inside.received == 65
    assert inside.loss_product == pytest.approx(7 / 72)
    assert inside.gate_done
    assert not inside.arq_fallback

    outside = simulate_round(64, 0.10, 20.0)
    assert outside.received == 58
    assert outside.loss_product == pytest.approx(14 / 72)
    assert not outside.gate_done
    assert outside.arq_fallback


def test_control_arm_has_no_slack() -> None:
    assert simulate_round(64, 0.0, 0.0).gate_done
    row = simulate_round(64, 0.0, 1.0)
    assert row.budget == 64
    assert not row.gate_done
    assert row.arq_fallback


def test_sweep_and_validation() -> None:
    rows = sweep(64, 0.10, [0, 10, 20])
    assert [row.loss_pct for row in rows] == [0.0, 10.0, 20.0]
    assert [row.gate_done for row in rows] == [True, True, False]

    with pytest.raises(ValueError):
        simulate_round(0, 0.1, 0.0)
    with pytest.raises(ValueError):
        simulate_round(64, 1.0, 0.0)
    with pytest.raises(ValueError):
        simulate_round(64, 0.1, 101.0)
