"""Command-line entry point for the control plane."""

from __future__ import annotations

import argparse

from sdf import __version__


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="sdf", description="Storage dataflow control plane")
    parser.add_argument("--version", action="version", version=f"sdf {__version__}")
    parser.add_argument(
        "command",
        nargs="?",
        choices=["status"],
        default="status",
        help="command to run (default: status)",
    )
    parser.parse_args(argv)

    print(f"sdf {__version__}: control-plane scaffold; no services are running.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
