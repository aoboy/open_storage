"""Benchmark report aggregator tests."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from sdf.bench.report import build_report, main, parse_line

TEXT = """\
# recorded
BENCH wm_select method=plain n=5 m=1024 ns_per_dec=2.50 cyc_per_dec=7.00
BENCH arc workload=qg batch=64 ns_per_block=150.25 releases=64
BENCH origin driver=http server=local p50_ns=1000 p99_ns=2000
BENCH fountain variant=c codec=lt k=64 blocks=512 p99_ns_per_block=1234.5
REPAIR arm=lt eps=0.10 loss=10 overhead=1.12 wall_ms=5
unrelated line
"""


def test_parse_records() -> None:
    wm = parse_line(TEXT.splitlines()[1], 2)
    assert wm is not None
    assert wm["kind"] == "wm"
    assert wm["method"] == "plain"
    assert wm["ns_per_dec"] == 2.5
    assert wm["m"] == 1024


def test_build_report_counts_and_skips() -> None:
    report = build_report(TEXT)
    assert report["counts"] == {
        "wm": 1,
        "arc": 1,
        "origin": 1,
        "fountain": 1,
        "repair": 1,
    }
    assert len(report["records"]) == 5
    assert report["skipped_lines"] == 2


def test_malformed_record_is_rejected() -> None:
    with pytest.raises(ValueError, match="not key=value"):
        parse_line("BENCH arc broken", 1)
    with pytest.raises(ValueError, match="duplicate field"):
        parse_line("BENCH arc workload=qg workload=q", 1)


def test_cli_writes_json(tmp_path: Path) -> None:
    source = tmp_path / "bench_results.txt"
    source.write_text(TEXT, encoding="utf-8")
    out = tmp_path / "report.json"
    assert main(["--input", str(source), "--out", str(out)]) == 0
    report = json.loads(out.read_text(encoding="utf-8"))
    assert report["counts"]["fountain"] == 1
