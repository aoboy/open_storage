from __future__ import annotations

import pytest

from sdf.control.wm_control import PathTelemetry, SoftSheddingController, renormalize


def test_renormalize_preserves_total() -> None:
    units = renormalize([1, 1, 1], 10)
    assert units == [4, 3, 3]
    assert sum(units) == 10


def test_renormalize_is_identity_when_total_matches() -> None:
    units = [10, 20, 30]
    result = renormalize(units, 60)
    assert result == units
    assert result is not units


def test_renormalize_rejects_invalid_input() -> None:
    with pytest.raises(ValueError):
        renormalize([1, 2], 0)
    with pytest.raises(ValueError):
        renormalize([0, 0], 10)


def test_controller_starts_balanced() -> None:
    controller = SoftSheddingController(7, m=1024)
    assert sum(controller.units) == 1024
    assert len(controller.units) == 7
    assert min(controller.units) >= 1


def test_controller_sheds_degraded_path() -> None:
    controller = SoftSheddingController(3, m=1024, alpha=0.25, beta=0.02)
    before = list(controller.units)
    telemetry = PathTelemetry(
        sent=[100, 100, 100],
        acked=[100, 10, 100],
        ewma_lat_ns=[0.0, 0.0, 0.0],
        ewma_err=[0.0, 0.0, 0.0],
    )

    after = controller.step(telemetry)

    assert sum(after) == 1024
    assert after[1] < before[1]
    assert after[0] >= before[0]


def test_controller_rejects_wrong_telemetry_width() -> None:
    controller = SoftSheddingController(2)
    telemetry = PathTelemetry(sent=[1], acked=[1], ewma_lat_ns=[0.0], ewma_err=[0.0])
    with pytest.raises(ValueError):
        controller.step(telemetry)


def test_controller_rejects_bad_configuration() -> None:
    with pytest.raises(ValueError):
        SoftSheddingController(0)
    with pytest.raises(ValueError):
        SoftSheddingController(4, m=2)
    with pytest.raises(ValueError):
        SoftSheddingController(2, alpha=0.0)
    with pytest.raises(ValueError):
        SoftSheddingController(2, beta=1.0)
