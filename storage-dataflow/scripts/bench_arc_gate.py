#!/usr/bin/env python3
"""ARC bench gate (plan W6.12).

Reads the recorded ``BENCH arc ...`` lines from ``build/bench_results.txt``,
requires the canonical immediate and QG records, and, when a reference runner
is named, enforces the 400 ns/block limit. The 200 ns/block target is always
reported separately as met or missed; it is not silently promoted to a gate.

The gate is inactive for unnamed local runners: structural problems (missing,
malformed, or non-draining records) still fail, but a slow machine is recorded
rather than treated as a regression. ``scripts/run_bench.sh`` passes
``SDF_BENCH_REFERENCE_RUNNER`` when a reference runner is configured.

Usage:
    bench_arc_gate.py --results build/bench_results.txt \
        --reference-runner reference-linux-x86_64

    bench_arc_gate.py --selftest
"""

from __future__ import annotations

import argparse
import math
import sys
import time
from dataclasses import dataclass
from pathlib import Path

TARGET_NS = 200.0
LIMIT_NS = 400.0
CANONICAL_BATCH = 64
REQUIRED_WORKLOADS = ("immediate", "qg")
REQUIRED_FIELDS = (
    "runner",
    "revision",
    "workload",
    "mode",
    "batch",
    "capacity",
    "entry_bytes",
    "blocks",
    "submit_ns_per_block",
    "poll_ns_per_block",
    "ns_per_block",
    "releases",
    "recorded_s",
)
INT_FIELDS = ("batch", "capacity", "entry_bytes", "blocks", "releases", "recorded_s")
FLOAT_FIELDS = ("submit_ns_per_block", "poll_ns_per_block", "ns_per_block")


@dataclass(frozen=True)
class ArcSample:
    runner: str
    revision: str
    workload: str
    mode: str
    batch: int
    capacity: int
    entry_bytes: int
    blocks: int
    submit_ns_per_block: float
    poll_ns_per_block: float
    ns_per_block: float
    releases: int
    recorded_s: int
    line: int


def parse_line(line: str, lineno: int) -> ArcSample:
    fields: dict[str, str] = {}
    for token in line.split()[2:]:
        if "=" not in token:
            raise ValueError(f"line {lineno}: token {token!r} is not key=value")
        key, value = token.split("=", 1)
        if key in fields:
            raise ValueError(f"line {lineno}: duplicate field {key!r}")
        fields[key] = value

    missing = [key for key in REQUIRED_FIELDS if key not in fields]
    if missing:
        raise ValueError(f"line {lineno}: missing {', '.join(missing)}")

    values: dict[str, int | float | str] = {}
    for key in REQUIRED_FIELDS:
        raw = fields[key]
        try:
            if key in INT_FIELDS:
                values[key] = int(raw, 10)
            elif key in FLOAT_FIELDS:
                values[key] = float(raw)
            else:
                values[key] = raw
        except ValueError as exc:
            raise ValueError(f"line {lineno}: invalid {key}={raw!r}") from exc

    ns = float(values["ns_per_block"])
    submit = float(values["submit_ns_per_block"])
    poll = float(values["poll_ns_per_block"])
    if not all(math.isfinite(v) for v in (ns, submit, poll)) or ns <= 0.0:
        raise ValueError(f"line {lineno}: non-finite or non-positive ns_per_block")
    if int(values["blocks"]) <= 0 or int(values["batch"]) <= 0:
        raise ValueError(f"line {lineno}: blocks and batch must be positive")

    return ArcSample(
        runner=str(values["runner"]),
        revision=str(values["revision"]),
        workload=str(values["workload"]),
        mode=str(values["mode"]),
        batch=int(values["batch"]),
        capacity=int(values["capacity"]),
        entry_bytes=int(values["entry_bytes"]),
        blocks=int(values["blocks"]),
        submit_ns_per_block=submit,
        poll_ns_per_block=poll,
        ns_per_block=ns,
        releases=int(values["releases"]),
        recorded_s=int(values["recorded_s"]),
        line=lineno,
    )


def parse_results(text: str) -> tuple[list[ArcSample], list[str]]:
    samples: list[ArcSample] = []
    errors: list[str] = []
    for lineno, line in enumerate(text.splitlines(), 1):
        if not line.startswith("BENCH arc "):
            continue
        try:
            samples.append(parse_line(line, lineno))
        except ValueError as exc:
            errors.append(str(exc))
    return samples, errors


def latest_matching(
    samples: list[ArcSample], workload: str, batch: int
) -> ArcSample | None:
    matches = [s for s in samples if s.workload == workload and s.batch == batch]
    if not matches:
        return None
    return max(matches, key=lambda s: (s.recorded_s, s.line))


def evaluate(
    samples: list[ArcSample],
    errors: list[str],
    *,
    reference_runner: str | None,
    target_ns: float,
    limit_ns: float,
    max_age_s: int,
    now_s: int,
) -> tuple[bool, list[str]]:
    messages: list[str] = []
    ok = True

    if errors:
        ok = False
        messages.extend(f"FAIL malformed: {error}" for error in errors)

    if reference_runner is not None:
        candidates = [s for s in samples if s.runner == reference_runner]
        if not candidates:
            ok = False
            messages.append(
                f"FAIL missing: no ARC samples for runner={reference_runner}"
            )
    else:
        candidates = samples
        messages.append(
            "status=inactive: no reference runner; 400 ns/block is recorded, not enforced"
        )

    selected: list[ArcSample] = []
    for workload in REQUIRED_WORKLOADS:
        sample = latest_matching(candidates, workload, CANONICAL_BATCH)
        if sample is None:
            ok = False
            messages.append(
                f"FAIL missing: workload={workload} batch={CANONICAL_BATCH} "
                f"for runner={reference_runner or 'any'}"
            )
            continue
        selected.append(sample)

        age_s = now_s - sample.recorded_s
        stale = age_s > max_age_s or age_s < 0
        if stale and reference_runner is not None:
            ok = False
            messages.append(
                f"FAIL stale: workload={workload} age_s={age_s} max_age_s={max_age_s}"
            )
        if sample.releases != sample.blocks:
            ok = False
            messages.append(
                f"FAIL incomplete: workload={workload} releases={sample.releases} "
                f"blocks={sample.blocks}"
            )

        target = "met" if sample.ns_per_block <= target_ns else "missed"
        limit = "ok" if sample.ns_per_block <= limit_ns else "over"
        if reference_runner is not None and sample.ns_per_block > limit_ns:
            ok = False
            messages.append(
                f"FAIL limit: workload={workload} ns_per_block={sample.ns_per_block:.2f} "
                f"> {limit_ns:.2f}"
            )
        messages.append(
            f"workload={workload} batch={sample.batch} mode={sample.mode} "
            f"ns_per_block={sample.ns_per_block:.2f} target={target} "
            f"limit={limit} age_s={age_s} revision={sample.revision}"
        )

    if selected and reference_runner is not None:
        messages.append(
            f"status=enforced runner={reference_runner} target_ns={target_ns:.2f} "
            f"limit_ns={limit_ns:.2f}"
        )
    return ok, messages


def selftest() -> int:
    def sample(
        workload: str, ns: float, *, batch: int = CANONICAL_BATCH, recorded: int = 1000
    ) -> str:
        mode = "IMMEDIATE" if workload == "immediate" else "QG"
        return (
            f"BENCH arc driver=arc-pacer revision=test runner=ref build=Debug cc=gcc "
            f"workload={workload} mode={mode} batch={batch} capacity={batch} "
            f"entry_bytes=1024 blocks=1024 submit_ns_per_block={ns / 2:.2f} "
            f"poll_ns_per_block={ns / 2:.2f} ns_per_block={ns:.2f} releases=1024 "
            f"recorded_s={recorded} sink=1"
        )

    def check(
        label: str,
        text: str,
        expected: bool,
        reference_runner: str | None = "ref",
    ) -> bool:
        parsed, errors = parse_results(text)
        ok, messages = evaluate(
            parsed,
            errors,
            reference_runner=reference_runner,
            target_ns=TARGET_NS,
            limit_ns=LIMIT_NS,
            max_age_s=60,
            now_s=1000,
        )
        if ok != expected:
            print(
                f"selftest FAIL: {label}: expected={expected} messages={messages}",
                file=sys.stderr,
            )
            return False
        return True

    good = sample("immediate", 120.0) + "\n" + sample("qg", 130.0) + "\n"
    target_missed = sample("immediate", 120.0) + "\n" + sample("qg", 250.0) + "\n"
    regressed_text = sample("immediate", 120.0) + "\n" + sample("qg", 500.0) + "\n"
    stale_text = (
        sample("immediate", 120.0, recorded=1)
        + "\n"
        + sample("qg", 130.0, recorded=1)
        + "\n"
    )
    passed = check("pass", good, True)
    passed = check("target-missed", target_missed, True) and passed
    regressed = check("regressed", regressed_text, False)
    missing = check("missing", sample("immediate", 120.0) + "\n", False)
    stale = check("stale", stale_text, False)
    malformed = check("malformed", "BENCH arc workload=qg ns_per_block=nan\n", False)
    advisory = check("advisory", regressed_text, True, reference_runner=None)
    if not all((passed, regressed, missing, stale, malformed, advisory)):
        return 1
    print(
        "bench_arc_gate: selftest ok "
        "(pass/target-miss/regressed/missing/stale/malformed/advisory)"
    )
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results", type=Path, default=Path("build/bench_results.txt"))
    parser.add_argument("--reference-runner", default=None)
    parser.add_argument("--target-ns", type=float, default=TARGET_NS)
    parser.add_argument("--limit-ns", type=float, default=LIMIT_NS)
    parser.add_argument("--max-age-s", type=int, default=3600)
    parser.add_argument("--now-s", type=int, default=None)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv[1:])

    if args.selftest:
        return selftest()
    if args.limit_ns < args.target_ns:
        parser.error("--limit-ns must be at least --target-ns")

    try:
        text = args.results.read_text(encoding="utf-8")
    except OSError as exc:
        print(
            f"bench_arc_gate: FAIL cannot read {args.results}: {exc}", file=sys.stderr
        )
        return 1

    samples, errors = parse_results(text)
    now_s = args.now_s if args.now_s is not None else int(time.time())
    ok, messages = evaluate(
        samples,
        errors,
        reference_runner=args.reference_runner,
        target_ns=args.target_ns,
        limit_ns=args.limit_ns,
        max_age_s=args.max_age_s,
        now_s=now_s,
    )
    stream = sys.stdout if ok else sys.stderr
    for message in messages:
        print(f"bench_arc_gate: {message}", file=stream)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
