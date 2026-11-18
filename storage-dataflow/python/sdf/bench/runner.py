"""Benchmark entry point for the control plane.

The full storage benchmark harness described in the design is not implemented
yet. This runner exercises the Whack-a-Mole soft-shedding controller so the
nightly job has a real, deterministic smoke test in the meantime.
"""

from __future__ import annotations

import argparse
import json
import platform
import time
from pathlib import Path
from typing import Any

from sdf.common.config import load_yaml
from sdf.control.wm_control import PathTelemetry, SoftSheddingController


def percentile(values: list[int], fraction: float) -> int:
    """Nearest-rank percentile for a non-empty list."""
    if not values:
        raise ValueError("values must not be empty")
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, round(fraction * (len(ordered) - 1))))
    return ordered[index]


def build_telemetry(paths: int) -> PathTelemetry:
    """Synthetic interval with one degraded path and the rest healthy."""
    sent = [1000] * paths
    acked = [990] * paths
    latency = [500_000.0] * paths
    error = [0.001] * paths
    if paths > 1:
        acked[1] = 100
        latency[1] = 5_000_000.0
        error[1] = 0.05
    return PathTelemetry(sent=sent, acked=acked, ewma_lat_ns=latency, ewma_err=error)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default="configs/dev", help="environment config directory")
    parser.add_argument("--out", default="out/bench", help="output directory")
    parser.add_argument("--iterations", type=int, default=10_000)
    parser.add_argument("--paths", type=int, default=8)
    args = parser.parse_args(argv)

    if args.iterations <= 0:
        parser.error("--iterations must be positive")
    if args.paths <= 0:
        parser.error("--paths must be positive")

    config_dir = Path(args.config)
    wm_config_path = config_dir / "wm.yaml"
    config: dict[str, Any] = {}
    if wm_config_path.is_file():
        try:
            config = load_yaml(wm_config_path)
        except ModuleNotFoundError:
            print("pyyaml not installed; recording benchmark without config")

    controller = SoftSheddingController(args.paths)
    telemetry = build_telemetry(args.paths)

    durations: list[int] = []
    for _ in range(args.iterations):
        started = time.perf_counter_ns()
        controller.step(telemetry)
        durations.append(time.perf_counter_ns() - started)

    report = {
        "kind": "control-plane-microbench",
        "config_dir": str(config_dir),
        "config_loaded": bool(config),
        "platform": platform.platform(),
        "paths": args.paths,
        "iterations": args.iterations,
        "step_ns": {
            "p50": percentile(durations, 0.50),
            "p99": percentile(durations, 0.99),
            "max": max(durations),
        },
        "final_units": controller.units,
    }

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / "wm_control_microbench.json"
    out_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
