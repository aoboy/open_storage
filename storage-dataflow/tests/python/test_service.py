"""Control-plane service entry-point tests (W6.17)."""

from __future__ import annotations

import json
import os
import threading
import uuid
from pathlib import Path

import pytest

from sdf.cli.main import main
from sdf.common.shm import WmShm
from sdf.control.service import (
    ControlPlaneService,
    build_tickables,
    load_service_config,
)

CONFIG_DIR = Path(__file__).resolve().parents[2] / "configs" / "dev"


def test_load_real_config() -> None:
    config = load_service_config(CONFIG_DIR)
    assert config.control_hz == 4.0
    assert config.interval_s == 0.25
    assert [item["name"] for item in config.fluid_classes] == [
        "hot-metadata",
        "cold-checkpoint",
    ]
    assert config.arc["mode"] == "arc_qg"


def test_invalid_config_is_rejected(tmp_path: Path) -> None:
    (tmp_path / "wm.toml").write_text(
        "[wm]\nm = 3\ninit_fracs = [1.0]\n", encoding="utf-8"
    )
    (tmp_path / "fluid.toml").write_text(
        "[fluid]\narq_fallback = true\n", encoding="utf-8"
    )
    (tmp_path / "arc.toml").write_text('[arc]\nmode = "arc_qg"\n', encoding="utf-8")
    with pytest.raises(ValueError, match="power of two"):
        load_service_config(tmp_path)

    (tmp_path / "wm.toml").write_text(
        "[wm]\nm = 1024\ninit_fracs = [0.5, 0.6]\ncontrol_hz = 4\n",
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="sum to 1.0"):
        load_service_config(tmp_path)


class FakeTick:
    name = "fake"

    def __init__(self) -> None:
        self.steps = 0

    def step(self) -> dict[str, int]:
        self.steps += 1
        return {"value": self.steps}


def test_service_step_and_health(tmp_path: Path) -> None:
    config = load_service_config(CONFIG_DIR)
    tick = FakeTick()
    sleeps: list[float] = []
    service = ControlPlaneService(
        config,
        [tick],
        clock=lambda: 10.0,
        sleeper=sleeps.append,
    )
    health = tmp_path / "health.json"
    completed = service.run(iterations=2, health_path=health)
    assert completed == 2
    assert tick.steps == 2
    assert len(sleeps) == 1
    assert sleeps[0] == pytest.approx(config.interval_s)
    payload = json.loads(health.read_text(encoding="utf-8"))
    assert payload["status"] == "ok"
    assert payload["heartbeat"] == 2
    assert payload["controller_names"] == ["fake"]
    assert payload["controllers"] == {"fake": {"value": 2}}
    assert payload["arc_mode"] == "arc_qg"
    assert [item["name"] for item in payload["fluid_classes"]] == [
        "hot-metadata",
        "cold-checkpoint",
    ]


def test_stop_event_prevents_ticks() -> None:
    config = load_service_config(CONFIG_DIR)
    stop = threading.Event()
    stop.set()
    tick = FakeTick()
    service = ControlPlaneService(
        config, [tick], clock=lambda: 0.0, sleeper=lambda _: None
    )
    assert service.run(iterations=0, stop_event=stop) == 0
    assert tick.steps == 0


def test_build_tickables_without_runtime_env(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("SDF_WM_SHM", raising=False)
    monkeypatch.delenv("SDF_ARC_SIDECAR", raising=False)
    assert build_tickables(load_service_config(CONFIG_DIR)) == []


def test_arc_tick_attaches(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("SDF_WM_SHM", raising=False)
    monkeypatch.setenv("SDF_ARC_SIDECAR", "1")
    tickables = build_tickables(load_service_config(CONFIG_DIR))
    assert [tick.name for tick in tickables] == ["arc"]
    try:
        record = tickables[0].step()
        assert record["mode"] == "shadow"
    finally:
        tickables[0].close()


def test_wm_tick_attaches_and_publishes(monkeypatch: pytest.MonkeyPatch) -> None:
    name = f"/sdf-service-{uuid.uuid4().hex}"
    created = WmShm(name, 2, create=True)
    created.close()
    try:
        monkeypatch.setenv("SDF_WM_SHM", name)
        tickables = build_tickables(load_service_config(CONFIG_DIR))
        assert [tick.name for tick in tickables] == ["wm"]
        record = tickables[0].step()
        assert record["units"] == [512, 512]
        tickables[0].close()
    finally:
        try:
            os.unlink("/dev/shm" + name)
        except FileNotFoundError:
            pass


def test_cli_status_reads_health(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    health = tmp_path / "health.json"
    health.write_text(
        json.dumps(
            {
                "status": "ok",
                "heartbeat": 7,
                "config_dir": "/etc/sdf",
                "controller_names": ["wm"],
                "arc_mode": "arc_qg",
                "fluid_classes": [{"name": "hot-metadata"}],
            }
        ),
        encoding="utf-8",
    )
    assert main(["status", "--health-file", str(health)]) == 0
    out = capsys.readouterr().out
    assert "status=ok" in out and "heartbeat=7" in out and "controllers=wm" in out
    assert "arc=arc_qg" in out and "classes=hot-metadata" in out

    assert main(["status", "--health-file", str(tmp_path / "missing.json")]) == 0
    assert "no control-plane health file" in capsys.readouterr().out

    health.write_text("{not json", encoding="utf-8")
    assert main(["status", "--health-file", str(health)]) == 1


def test_cli_serve_smoke(tmp_path: Path) -> None:
    health = tmp_path / "health.json"
    rc = main(
        [
            "serve",
            "--config",
            str(CONFIG_DIR),
            "--iterations",
            "2",
            "--health-file",
            str(health),
        ]
    )
    assert rc == 0
    assert json.loads(health.read_text(encoding="utf-8"))["heartbeat"] == 2
