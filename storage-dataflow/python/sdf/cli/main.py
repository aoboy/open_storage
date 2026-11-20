"""Command-line entry point for the control plane."""

from __future__ import annotations

import argparse
import json
import signal
import sys
import threading
from pathlib import Path

from sdf import __version__
from sdf.control.service import (
    ControlPlaneService,
    build_tickables,
    load_service_config,
)


def _status(health_file: str) -> int:
    path = Path(health_file) if health_file else None
    if path is not None and path.is_file():
        try:
            payload = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            print(f"sdf {__version__}: cannot read {path}: {exc}", file=sys.stderr)
            return 1
        controllers = ",".join(str(name) for name in payload.get("controller_names", []))
        print(
            f"sdf {__version__}: status={payload.get('status', 'unknown')} "
            f"heartbeat={payload.get('heartbeat', '?')} "
            f"config={payload.get('config_dir', '?')} "
            f"controllers={controllers or 'none'}"
        )
        return 0
    print(f"sdf {__version__}: no control-plane health file at {health_file or '(disabled)'}.")
    return 0


def _serve(args: argparse.Namespace) -> int:
    config = load_service_config(args.config)
    tickables = build_tickables(config)
    service = ControlPlaneService(config, tickables)
    stop = threading.Event()

    def _handle(_signum: int, _frame: object) -> None:
        stop.set()

    previous_int = signal.signal(signal.SIGINT, _handle)
    previous_term = signal.signal(signal.SIGTERM, _handle)
    health_path = Path(args.health_file) if args.health_file else None
    try:
        print(
            f"sdf serve: config={config.directory} hz={config.control_hz:g} "
            f"controllers={','.join(t.name for t in tickables) or 'none'}",
            flush=True,
        )
        completed = service.run(
            iterations=args.iterations,
            stop_event=stop,
            health_path=health_path,
            on_record=(lambda record: print(json.dumps(record, sort_keys=True), flush=True))
            if args.verbose
            else None,
        )
        print(f"sdf serve: stopped after {completed} ticks", flush=True)
        return 0
    finally:
        signal.signal(signal.SIGINT, previous_int)
        signal.signal(signal.SIGTERM, previous_term)
        service.close()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="sdf", description="Storage dataflow control plane")
    parser.add_argument("--version", action="version", version=f"sdf {__version__}")
    commands = parser.add_subparsers(dest="command")
    status = commands.add_parser("status", help="show control-plane status")
    status.add_argument(
        "--health-file",
        default="out/control-plane-health.json",
        help="health JSON written by sdf serve (empty disables)",
    )
    serve = commands.add_parser("serve", help="run the control-plane service")
    serve.add_argument("--config", default="configs/dev", help="environment config directory")
    serve.add_argument(
        "--iterations",
        type=int,
        default=0,
        help="stop after N ticks (0 = run until SIGINT/SIGTERM)",
    )
    serve.add_argument(
        "--health-file",
        default="out/control-plane-health.json",
        help="atomic health/readiness JSON path (empty disables)",
    )
    serve.add_argument("--verbose", action="store_true", help="print every tick record")
    args = parser.parse_args(argv)

    if args.command == "status":
        return _status(args.health_file)
    if args.command is None:
        return _status("out/control-plane-health.json")
    if args.command == "serve":
        return _serve(args)
    parser.error(f"unknown command: {args.command}")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
