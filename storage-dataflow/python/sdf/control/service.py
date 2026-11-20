"""Long-running control-plane service (W6.17).

The service owns the periodic control loop that deployment uses instead of
the CLI smoke command. It loads the strict TOML environment files, ticks the
available controllers at the configured WM cadence, writes an atomic health
record for readiness/liveness probes, and stops cleanly on a caller-provided
event.

WM attaches only when ``SDF_WM_SHM`` names an existing segment; the service
then publishes profiles through ``WmControlLoop``. Missing optional controllers
are reported as disabled rather than turning liveness into a failure.
"""

from __future__ import annotations

import json
import os
import time
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Protocol, cast

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover - Python 3.10 fallback
    import tomli as tomllib

from sdf.common.shm import WmShm
from sdf.control.wm_control import WmControlLoop


class Tickable(Protocol):
    """A controller the service can tick."""

    name: str

    def step(self) -> Mapping[str, object]: ...


@dataclass(frozen=True)
class ServiceConfig:
    """Validated environment configuration."""

    directory: Path
    wm: dict[str, Any]
    fluid: dict[str, Any]
    arc: dict[str, Any]
    fluid_classes: list[dict[str, Any]]

    @property
    def control_hz(self) -> float:
        return float(self.wm["control_hz"])

    @property
    def interval_s(self) -> float:
        return 1.0 / self.control_hz


def _load_toml(path: Path) -> dict[str, Any]:
    with path.open("rb") as handle:
        data = tomllib.load(handle)
    if not isinstance(data, dict):
        raise ValueError(f"{path}: expected a TOML table")
    return data


def _require_table(data: dict[str, Any], name: str, path: Path) -> dict[str, Any]:
    table = data.get(name)
    if not isinstance(table, dict):
        raise ValueError(f"{path}: missing [{name}] table")
    return table


def load_service_config(directory: str | Path) -> ServiceConfig:
    """Load and validate ``wm.toml``, ``fluid.toml`` and ``arc.toml``."""
    root = Path(directory)
    if not root.is_dir():
        raise ValueError(f"{root}: config directory not found")

    wm_path = root / "wm.toml"
    fluid_path = root / "fluid.toml"
    arc_path = root / "arc.toml"
    wm_doc = _load_toml(wm_path)
    fluid_doc = _load_toml(fluid_path)
    arc_doc = _load_toml(arc_path)
    wm = _require_table(wm_doc, "wm", wm_path)
    fluid = _require_table(fluid_doc, "fluid", fluid_path)
    arc = _require_table(arc_doc, "arc", arc_path)

    m = wm.get("m")
    if not isinstance(m, int) or m < 2 or (m & (m - 1)) != 0:
        raise ValueError(f"{wm_path}: wm.m must be a power of two >= 2")
    init_fracs = wm.get("init_fracs")
    if (
        not isinstance(init_fracs, list)
        or not init_fracs
        or any(not isinstance(value, (int, float)) for value in init_fracs)
    ):
        raise ValueError(f"{wm_path}: wm.init_fracs must be a non-empty numeric list")
    if abs(sum(float(value) for value in init_fracs) - 1.0) > 1e-6:
        raise ValueError(f"{wm_path}: wm.init_fracs must sum to 1.0")
    control_hz = wm.get("control_hz", 4)
    if not isinstance(control_hz, (int, float)) or not 0 < float(control_hz) <= 1000:
        raise ValueError(f"{wm_path}: wm.control_hz must be in (0, 1000]")

    mode = arc.get("mode")
    if mode not in ("base", "arc_q", "arc_qg"):
        raise ValueError(f"{arc_path}: arc.mode must be base, arc_q, or arc_qg")
    for key in ("shadow_mode", "kill_switch"):
        if not isinstance(arc.get(key, False), bool):
            raise ValueError(f"{arc_path}: arc.{key} must be boolean")

    if not isinstance(fluid.get("arq_fallback", True), bool):
        raise ValueError(f"{fluid_path}: fluid.arq_fallback must be boolean")
    classes = fluid_doc.get("fluid_class", [])
    if not isinstance(classes, list):
        raise ValueError(f"{fluid_path}: fluid_class must be an array of tables")
    names: set[str] = set()
    for index, item in enumerate(classes):
        if not isinstance(item, dict):
            raise ValueError(f"{fluid_path}: fluid_class[{index}] must be a table")
        name = item.get("name")
        if not isinstance(name, str) or not name or name in names:
            raise ValueError(f"{fluid_path}: fluid_class[{index}].name is missing or duplicate")
        names.add(name)
        epsilon = item.get("eps")
        if not isinstance(epsilon, (int, float)) or not 0.0 <= float(epsilon) < 1.0:
            raise ValueError(f"{fluid_path}: fluid_class[{index}].eps must be in [0, 1)")
        if item.get("codec") not in ("lt", "nanorq"):
            raise ValueError(f"{fluid_path}: fluid_class[{index}].codec must be lt or nanorq")

    return ServiceConfig(
        directory=root,
        wm=wm,
        fluid=fluid,
        arc=arc,
        fluid_classes=cast(list[dict[str, Any]], classes),
    )


class WmTick:
    """Tick the WM controller against an existing shared-memory segment."""

    name = "wm"

    def __init__(self, name: str, config: ServiceConfig) -> None:
        self.shm = WmShm(name, len(config.wm["init_fracs"]))
        self.loop = WmControlLoop(
            self.shm,
            m=int(config.wm["m"]),
            alpha=float(config.wm.get("alpha", 0.25)),
            beta=float(config.wm.get("beta", 0.02)),
        )

    def step(self) -> Mapping[str, object]:
        return self.loop.step()

    def close(self) -> None:
        self.shm.close()


def build_tickables(config: ServiceConfig) -> list[Tickable]:
    """Attach controllers whose runtime surface is configured."""
    tickables: list[Tickable] = []
    shm_name = os.environ.get("SDF_WM_SHM")
    if shm_name:
        tickables.append(WmTick(shm_name, config))
    return tickables


class ControlPlaneService:
    """Periodic control loop with atomic health reporting."""

    def __init__(
        self,
        config: ServiceConfig,
        tickables: list[Tickable] | None = None,
        *,
        clock: Any = time.monotonic,
        sleeper: Any = time.sleep,
    ) -> None:
        self.config = config
        self.tickables = tickables or []
        self.clock = clock
        self.sleeper = sleeper
        self.heartbeat = 0
        self.last_tick_ns: int | None = None

    def step(self) -> dict[str, object]:
        tick_ns = time.time_ns()
        controllers: dict[str, object] = {}
        for tickable in self.tickables:
            controllers[tickable.name] = tickable.step()
        self.heartbeat += 1
        self.last_tick_ns = tick_ns
        return {
            "heartbeat": self.heartbeat,
            "tick_ns": tick_ns,
            "controllers": controllers,
        }

    def _write_health(self, path: Path, record: dict[str, object]) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "status": "ok",
            "config_dir": str(self.config.directory),
            "control_hz": self.config.control_hz,
            "controller_names": [tickable.name for tickable in self.tickables],
            **record,
        }
        temp = path.with_name(path.name + ".tmp")
        temp.write_text(json.dumps(payload, sort_keys=True) + "\n", encoding="utf-8")
        temp.replace(path)

    def run(
        self,
        *,
        iterations: int = 0,
        stop_event: Any = None,
        health_path: Path | None = None,
        on_record: Any = None,
    ) -> int:
        """Run forever (``iterations == 0``) or for a bounded smoke test."""
        if iterations < 0:
            raise ValueError("iterations must be non-negative")
        completed = 0
        next_tick = self.clock()
        while iterations == 0 or completed < iterations:
            if stop_event is not None and stop_event.is_set():
                break
            record = self.step()
            completed += 1
            if health_path is not None:
                self._write_health(health_path, record)
            if on_record is not None:
                on_record(record)
            if iterations != 0 and completed >= iterations:
                break
            next_tick += self.config.interval_s
            delay = next_tick - self.clock()
            if delay > 0:
                self.sleeper(delay)
            else:
                next_tick = self.clock()
        return completed

    def close(self) -> None:
        for tickable in self.tickables:
            close = getattr(tickable, "close", None)
            if callable(close):
                close()
