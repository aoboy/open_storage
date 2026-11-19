#!/usr/bin/env python3
"""Nightly bench regression gate (plan W6.5, design 00 section 11.3).

Fails when the current control-plane p99 exceeds the median of the last
N days by more than --threshold (default 5%). The gate is deliberately
NOT in ctest: it needs history and a quiet runner.

Usage:
    bench_regression.py --history bench-history \
        --current out/bench/wm_control_microbench.json

    bench_regression.py --selftest
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from pathlib import Path


def metric(doc: dict, path: str) -> float:
    cur = doc
    for part in path.split("."):
        cur = cur[part]
    return float(cur)


def load_history(directory: Path, path: str) -> list[float]:
    values: list[float] = []
    for file in sorted(directory.glob("*.json")):
        try:
            doc = json.loads(file.read_text(encoding="utf-8"))
            values.append(metric(doc, path))
        except (OSError, ValueError, KeyError, TypeError):
            print(f"bench_regression: ignoring unreadable {file}", file=sys.stderr)
    return values


def gate(
    history: list[float],
    current: float,
    threshold: float,
    min_samples: int,
) -> tuple[bool, str]:
    if len(history) < min_samples:
        return (
            True,
            f"skip: {len(history)} history sample(s), need >= {min_samples}",
        )
    baseline = statistics.median(history)
    if baseline <= 0 or current <= 0:
        return False, f"invalid metric (current={current}, median={baseline})"
    delta = (current - baseline) / baseline
    verdict = "PASS" if delta <= threshold else "FAIL"
    message = (
        f"{verdict}: current={current:.0f} ns median={baseline:.0f} ns "
        f"delta={delta:+.2%} threshold=+{threshold:.1%} "
        f"(n={len(history)})"
    )
    return delta <= threshold, message


def selftest() -> int:
    history = [100.0, 102.0, 98.0, 101.0]
    ok, msg = gate(history, 104.0, 0.05, 3)
    if not ok:
        print(f"selftest FAIL: +3.5% should pass ({msg})", file=sys.stderr)
        return 1
    ok, msg = gate(history, 106.0, 0.05, 3)
    if ok:
        print(f"selftest FAIL: +5.5% should fail ({msg})", file=sys.stderr)
        return 1
    ok, msg = gate([100.0, 101.0], 200.0, 0.05, 3)
    if not ok or "skip" not in msg:
        print(f"selftest FAIL: short history should skip ({msg})", file=sys.stderr)
        return 1
    ok, msg = gate([0.0, 0.0, 0.0], 100.0, 0.05, 3)
    if ok:
        print(f"selftest FAIL: zero baseline should fail ({msg})", file=sys.stderr)
        return 1
    print("bench_regression: selftest ok (pass/fail/skip/zero-baseline)")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--history", type=Path, default=Path("bench-history"))
    ap.add_argument(
        "--current", type=Path, default=Path("out/bench/wm_control_microbench.json")
    )
    ap.add_argument("--metric", default="step_ns.p99")
    ap.add_argument("--threshold", type=float, default=0.05)
    ap.add_argument("--min-samples", type=int, default=3)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv[1:])

    if args.selftest:
        return selftest()

    try:
        current = metric(
            json.loads(args.current.read_text(encoding="utf-8")), args.metric
        )
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"bench_regression: cannot read current bench: {exc}", file=sys.stderr)
        return 1

    history = load_history(args.history, args.metric)
    ok, message = gate(history, current, args.threshold, args.min_samples)
    stream = sys.stdout if ok else sys.stderr
    print(f"bench_regression: {message}", file=stream)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
