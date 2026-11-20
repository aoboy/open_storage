#!/usr/bin/env python3
"""Validate and summarize paired C/Rust LT fountain benchmark lines (W6.12).

The paired runner writes one ``BENCH fountain`` line per variant and
configuration. This tool rejects missing pairs, duplicate variants, or records
whose runner/revision/build differ, then prints the paired p99 and throughput
ratios. It does not enforce a performance threshold; the output is evidence.
"""

from __future__ import annotations

import argparse
import math
import sys
from dataclasses import dataclass
from pathlib import Path

REQUIRED = (
    "variant",
    "codec",
    "k",
    "sym_len",
    "overhead_pct",
    "loss_pct",
    "blocks",
    "symbols",
    "ns_per_block",
    "p99_ns_per_block",
    "blocks_per_s",
    "runner",
    "revision",
    "build",
)
INT_FIELDS = ("k", "sym_len", "overhead_pct", "loss_pct", "blocks", "symbols")
FLOAT_FIELDS = ("ns_per_block", "p99_ns_per_block", "blocks_per_s")


@dataclass(frozen=True)
class FountainSample:
    variant: str
    codec: str
    k: int
    sym_len: int
    overhead_pct: int
    loss_pct: int
    blocks: int
    symbols: int
    ns_per_block: float
    p99_ns_per_block: float
    blocks_per_s: float
    runner: str
    revision: str
    build: str
    line: int

    @property
    def key(self) -> tuple[str, int, int, int, int, int]:
        return (
            self.codec,
            self.k,
            self.sym_len,
            self.overhead_pct,
            self.loss_pct,
            self.blocks,
        )


def parse_line(line: str, lineno: int) -> FountainSample:
    fields: dict[str, str] = {}
    for token in line.split()[2:]:
        if "=" not in token:
            raise ValueError(f"line {lineno}: token {token!r} is not key=value")
        key, value = token.split("=", 1)
        if key in fields:
            raise ValueError(f"line {lineno}: duplicate field {key!r}")
        fields[key] = value

    missing = [key for key in REQUIRED if key not in fields]
    if missing:
        raise ValueError(f"line {lineno}: missing {', '.join(missing)}")

    values: dict[str, str | int | float] = {}
    for key in REQUIRED:
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

    if values["variant"] not in ("c", "rust"):
        raise ValueError(f"line {lineno}: variant must be c or rust")
    if (
        int(values["k"]) <= 0
        or int(values["sym_len"]) <= 0
        or int(values["blocks"]) <= 0
    ):
        raise ValueError(f"line {lineno}: k, sym_len, and blocks must be positive")
    for key in FLOAT_FIELDS:
        value = float(values[key])
        if not math.isfinite(value) or value < 0.0:
            raise ValueError(f"line {lineno}: {key} must be finite and non-negative")

    return FountainSample(
        variant=str(values["variant"]),
        codec=str(values["codec"]),
        k=int(values["k"]),
        sym_len=int(values["sym_len"]),
        overhead_pct=int(values["overhead_pct"]),
        loss_pct=int(values["loss_pct"]),
        blocks=int(values["blocks"]),
        symbols=int(values["symbols"]),
        ns_per_block=float(values["ns_per_block"]),
        p99_ns_per_block=float(values["p99_ns_per_block"]),
        blocks_per_s=float(values["blocks_per_s"]),
        runner=str(values["runner"]),
        revision=str(values["revision"]),
        build=str(values["build"]),
        line=lineno,
    )


def parse_text(text: str) -> tuple[list[FountainSample], list[str]]:
    samples: list[FountainSample] = []
    errors: list[str] = []
    for lineno, line in enumerate(text.splitlines(), 1):
        if not line.startswith("BENCH fountain "):
            continue
        try:
            samples.append(parse_line(line, lineno))
        except ValueError as exc:
            errors.append(str(exc))
    return samples, errors


def evaluate(samples: list[FountainSample]) -> tuple[bool, list[str]]:
    messages: list[str] = []
    ok = True
    groups: dict[tuple[str, int, int, int, int, int], dict[str, FountainSample]] = {}
    for sample in samples:
        bucket = groups.setdefault(sample.key, {})
        if sample.variant in bucket:
            ok = False
            messages.append(f"FAIL duplicate variant={sample.variant} key={sample.key}")
            continue
        bucket[sample.variant] = sample

    if not groups:
        ok = False
        messages.append("FAIL no fountain records")
        return ok, messages

    for key in sorted(groups):
        pair = groups[key]
        if "c" not in pair or "rust" not in pair:
            ok = False
            missing = "rust" if "c" in pair else "c"
            messages.append(f"FAIL missing {missing} for key={key}")
            continue
        c = pair["c"]
        rust = pair["rust"]
        if c.codec != "lt" or rust.codec != "lt":
            ok = False
            messages.append(f"FAIL paired comparison requires lt, key={key}")
        if (
            c.runner != rust.runner
            or c.revision != rust.revision
            or c.build != rust.build
        ):
            ok = False
            messages.append(f"FAIL mixed runner/revision/build for key={key}")
        if c.symbols != rust.symbols:
            ok = False
            messages.append(f"FAIL symbol count differs for key={key}")
        ratio = (
            rust.p99_ns_per_block / c.p99_ns_per_block
            if c.p99_ns_per_block
            else math.inf
        )
        thr_ratio = rust.blocks_per_s / c.blocks_per_s if c.blocks_per_s else math.inf
        messages.append(
            f"key=codec:{key[0]},k:{key[1]},sym:{key[2]},overhead:{key[3]},"
            f"loss:{key[4]},blocks:{key[5]} "
            f"c_p99_ns={c.p99_ns_per_block:.1f} rust_p99_ns={rust.p99_ns_per_block:.1f} "
            f"p99_ratio={ratio:.3f} c_blocks_s={c.blocks_per_s:.1f} "
            f"rust_blocks_s={rust.blocks_per_s:.1f} throughput_ratio={thr_ratio:.3f}"
        )
    return ok, messages


def selftest() -> int:
    text = (
        "BENCH fountain variant=c revision=r1 runner=box build=Release cc=gcc "
        "codec=lt k=16 sym_len=64 overhead_pct=100 loss_pct=0 blocks=8 symbols=32 "
        "ns_per_block=10 p99_ns_per_block=12 blocks_per_s=1000\n"
        "BENCH fountain variant=rust revision=r1 runner=box build=Release cc=gcc "
        "codec=lt k=16 sym_len=64 overhead_pct=100 loss_pct=0 blocks=8 symbols=32 "
        "ns_per_block=8 p99_ns_per_block=9 blocks_per_s=1250\n"
    )
    cases = (
        (text, True),
        (text.replace("variant=rust ", ""), False),
        (text + text.splitlines()[0] + "\n", False),
        (
            text.replace(
                "runner=box build=Release cc=gcc",
                "runner=other build=Release cc=gcc",
                1,
            ),
            False,
        ),
    )
    for index, (case, expected) in enumerate(cases):
        samples, errors = parse_text(case)
        ok = not errors
        if ok:
            ok, _ = evaluate(samples)
        if ok != expected:
            print(f"bench_fountain_compare selftest FAIL case {index}", file=sys.stderr)
            return 1
    print("bench_fountain_compare selftest: OK")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", nargs="?", type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.results is None:
        parser.error("results file is required unless --selftest is used")

    samples, errors = parse_text(args.results.read_text(encoding="utf-8"))
    ok, messages = evaluate(samples)
    if errors:
        ok = False
        messages.extend(f"FAIL malformed: {error}" for error in errors)
    for message in messages:
        print(message)
    print(f"bench_fountain_compare: {'OK' if ok else 'FAIL'} ({len(samples)} records)")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
