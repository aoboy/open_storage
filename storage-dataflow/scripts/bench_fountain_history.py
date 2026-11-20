#!/usr/bin/env python3
"""Summarize paired C/Rust fountain history across nightly artifacts (W6.12).

Reads every ``*.txt`` file in ``--history``, requires one C and one Rust record
per configuration, and writes per-configuration ratio samples plus medians.
The tool does not gate performance; it reports how many paired nights exist.
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench_fountain_compare import FountainSample, parse_text  # noqa: E402


def config_name(sample: FountainSample) -> str:
    return (
        f"{sample.codec}:k={sample.k}:sym={sample.sym_len}:"
        f"overhead={sample.overhead_pct}:loss={sample.loss_pct}:blocks={sample.blocks}"
    )


def summarize(history: Path) -> dict[str, Any]:
    files = sorted(history.glob("*.txt"))
    per_config: dict[str, list[dict[str, Any]]] = {}
    runners: set[str] = set()
    errors: list[str] = []

    for path in files:
        samples, parse_errors = parse_text(path.read_text(encoding="utf-8"))
        if parse_errors:
            errors.extend(f"{path.name}: {error}" for error in parse_errors)
            continue
        groups: dict[str, dict[str, FountainSample]] = {}
        for sample in samples:
            groups.setdefault(config_name(sample), {})[sample.variant] = sample
        for key, pair in groups.items():
            if "c" not in pair or "rust" not in pair:
                errors.append(f"{path.name}: incomplete pair for {key}")
                continue
            c = pair["c"]
            rust = pair["rust"]
            if c.runner != rust.runner or c.revision != rust.revision:
                errors.append(f"{path.name}: mixed runner/revision for {key}")
                continue
            runners.add(c.runner)
            p99_ratio = (
                rust.p99_ns_per_block / c.p99_ns_per_block
                if c.p99_ns_per_block
                else None
            )
            thr_ratio = rust.blocks_per_s / c.blocks_per_s if c.blocks_per_s else None
            per_config.setdefault(key, []).append(
                {
                    "file": path.name,
                    "runner": c.runner,
                    "revision": c.revision,
                    "c_p99_ns": c.p99_ns_per_block,
                    "rust_p99_ns": rust.p99_ns_per_block,
                    "p99_ratio": p99_ratio,
                    "throughput_ratio": thr_ratio,
                }
            )

    configs: dict[str, Any] = {}
    for key, samples in sorted(per_config.items()):
        p99 = [s["p99_ratio"] for s in samples if s["p99_ratio"] is not None]
        thr = [
            s["throughput_ratio"] for s in samples if s["throughput_ratio"] is not None
        ]
        configs[key] = {
            "nights": len(samples),
            "median_p99_ratio": statistics.median(p99) if p99 else None,
            "median_throughput_ratio": statistics.median(thr) if thr else None,
            "samples": samples,
        }

    return {
        "files": [path.name for path in files],
        "runners": sorted(runners),
        "configs": configs,
        "errors": errors,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--history", type=Path, required=True)
    parser.add_argument(
        "--out", type=Path, default=Path("out/bench/fountain_history.json")
    )
    parser.add_argument("--min-nights", type=int, default=0)
    args = parser.parse_args(argv)

    report = summarize(args.history)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    for key, item in report["configs"].items():
        print(
            f"{key} nights={item['nights']} "
            f"median_p99_ratio={item['median_p99_ratio']} "
            f"median_throughput_ratio={item['median_throughput_ratio']}"
        )
    for error in report["errors"]:
        print(f"FAIL {error}", file=sys.stderr)
    if report["errors"]:
        return 1
    for key, item in report["configs"].items():
        if item["nights"] < args.min_nights:
            print(
                f"bench_fountain_history: {key} has {item['nights']} night(s), "
                f"need {args.min_nights}",
                file=sys.stderr,
            )
            return 1
    print(f"bench_fountain_history: OK ({len(report['files'])} file(s) -> {args.out})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
