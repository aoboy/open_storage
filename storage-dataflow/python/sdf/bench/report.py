"""Aggregate ``BENCH``/``REPAIR`` records into one JSON report.

The C benches write line-oriented records to ``build/bench_results.txt``. This
module preserves the raw fields, adds a record kind, and writes a stable JSON
artifact for downstream tooling without changing any bench or gate.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any

PREFIXES = (
    ("BENCH wm_select ", "wm"),
    ("BENCH arc ", "arc"),
    ("BENCH origin ", "origin"),
    ("BENCH fountain ", "fountain"),
    ("REPAIR ", "repair"),
)
_INT = re.compile(r"^-?\d+$")
_FLOAT = re.compile(r"^-?(?:\d+\.\d*|\.\d+)(?:[eE][+-]?\d+)?$")


def _value(raw: str) -> int | float | str:
    if _INT.match(raw):
        return int(raw)
    if _FLOAT.match(raw):
        return float(raw)
    return raw


def parse_line(line: str, lineno: int = 0) -> dict[str, Any] | None:
    """Parse one recognized record; return ``None`` for non-data lines."""
    for prefix, kind in PREFIXES:
        if not line.startswith(prefix):
            continue
        fields: dict[str, Any] = {"kind": kind, "line": lineno}
        for token in line[len(prefix) :].split():
            if "=" not in token:
                raise ValueError(f"line {lineno}: token {token!r} is not key=value")
            key, raw = token.split("=", 1)
            if key in fields:
                raise ValueError(f"line {lineno}: duplicate field {key!r}")
            fields[key] = _value(raw)
        return fields
    return None


def build_report(text: str) -> dict[str, Any]:
    """Return ``{"records": [...], "counts": {...}, "skipped_lines": N}``."""
    records: list[dict[str, Any]] = []
    counts: dict[str, int] = {}
    skipped = 0
    for lineno, line in enumerate(text.splitlines(), 1):
        if not line or line.startswith("#") or line.startswith("=="):
            skipped += 1
            continue
        record = parse_line(line, lineno)
        if record is None:
            skipped += 1
            continue
        records.append(record)
        kind = str(record["kind"])
        counts[kind] = counts.get(kind, 0) + 1
    return {"records": records, "counts": counts, "skipped_lines": skipped}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path("build/bench_results.txt"))
    parser.add_argument("--out", type=Path, default=Path("out/bench/bench_report.json"))
    args = parser.parse_args(argv)

    report = build_report(args.input.read_text(encoding="utf-8"))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        f"wrote {args.out} ({len(report['records'])} records: "
        f"{', '.join(f'{k}={v}' for k, v in sorted(report['counts'].items())) or 'none'})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
