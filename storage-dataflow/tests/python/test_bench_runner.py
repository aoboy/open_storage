"""Control-plane benchmark runner tests."""

from __future__ import annotations

import json
from pathlib import Path

from sdf.bench.runner import main, percentile

CONFIG_DIR = Path(__file__).resolve().parents[2] / "configs" / "dev"


def test_runner_loads_toml_config(tmp_path: Path) -> None:
    out = tmp_path / "out"
    assert (
        main(["--config", str(CONFIG_DIR), "--out", str(out), "--iterations", "50"])
        == 0
    )
    report = json.loads(
        (out / "wm_control_microbench.json").read_text(encoding="utf-8")
    )
    assert report["config_loaded"] is True
    assert report["control_hz"] == 4
    assert report["arc_mode"] == "arc_qg"
    assert report["fluid_classes"] == ["hot-metadata", "cold-checkpoint"]
    assert report["step_ns"]["p99"] >= report["step_ns"]["p50"]


def test_runner_tolerates_missing_config(tmp_path: Path) -> None:
    config = tmp_path / "empty-config"
    config.mkdir()
    out = tmp_path / "out"
    assert main(["--config", str(config), "--out", str(out), "--iterations", "10"]) == 0
    report = json.loads(
        (out / "wm_control_microbench.json").read_text(encoding="utf-8")
    )
    assert report["config_loaded"] is False
    assert report["arc_mode"] is None
    assert report["fluid_classes"] == []


def test_percentile() -> None:
    assert percentile([1, 2, 3, 4], 0.0) == 1
    assert percentile([1, 2, 3, 4], 0.5) == 3
    assert percentile([1, 2, 3, 4], 1.0) == 4
