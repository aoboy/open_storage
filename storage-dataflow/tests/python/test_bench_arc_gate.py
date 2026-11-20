"""W6.12 — ARC bench gate and nightly-history regression checks.

These tests use synthetic records only. They prove that a required gate fails
on missing, malformed, stale, and regressed ARC samples instead of silently
skipping. No hosted runner or measured baseline is claimed.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / "scripts/bench_arc_gate.py"
REGRESSION = ROOT / "scripts/bench_regression.py"


def arc_line(
    workload: str,
    ns: float,
    *,
    runner: str = "ref",
    batch: int = 64,
    recorded_s: int = 1000,
    releases: int = 1024,
) -> str:
    mode = "IMMEDIATE" if workload == "immediate" else "QG"
    return (
        f"BENCH arc driver=arc-pacer revision=test runner={runner} build=Debug cc=gcc "
        f"workload={workload} mode={mode} batch={batch} capacity={batch} "
        f"entry_bytes=1024 blocks=1024 submit_ns_per_block={ns / 2:.2f} "
        f"poll_ns_per_block={ns / 2:.2f} ns_per_block={ns:.2f} "
        f"releases={releases} recorded_s={recorded_s} sink=1"
    )


def run_gate(
    tmp_path: Path, *lines: str, args: tuple[str, ...] = ()
) -> subprocess.CompletedProcess[str]:
    results = tmp_path / "bench_results.txt"
    results.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return subprocess.run(
        [sys.executable, str(GATE), "--results", str(results), *args],
        capture_output=True,
        text=True,
        check=False,
    )


def reference(*args: str) -> tuple[str, ...]:
    return ("--reference-runner", "ref", "--now-s", "1000", "--max-age-s", "60", *args)


def test_gate_passes_and_reports_target_separately(tmp_path):
    result = run_gate(
        tmp_path,
        arc_line("immediate", 150.0),
        arc_line("qg", 180.0),
        args=reference(),
    )
    assert result.returncode == 0, result.stderr
    assert "target=met" in result.stdout
    assert "status=enforced" in result.stdout


def test_gate_reports_target_missed_but_below_limit(tmp_path):
    result = run_gate(
        tmp_path,
        arc_line("immediate", 150.0),
        arc_line("qg", 250.0),
        args=reference(),
    )
    assert result.returncode == 0, result.stderr
    assert "target=missed" in result.stdout
    assert "limit=ok" in result.stdout


def test_gate_fails_above_enforced_limit(tmp_path):
    result = run_gate(
        tmp_path,
        arc_line("immediate", 150.0),
        arc_line("qg", 500.0),
        args=reference(),
    )
    assert result.returncode != 0
    assert "FAIL limit" in result.stderr


def test_gate_fails_on_missing_required_record(tmp_path):
    result = run_gate(tmp_path, arc_line("immediate", 150.0), args=reference())
    assert result.returncode != 0
    assert "FAIL missing" in result.stderr


def test_gate_fails_on_malformed_sample(tmp_path):
    result = run_gate(
        tmp_path,
        "BENCH arc workload=qg ns_per_block=nan",
        args=reference(),
    )
    assert result.returncode != 0
    assert "malformed" in result.stderr


def test_gate_fails_on_stale_sample(tmp_path):
    result = run_gate(
        tmp_path,
        arc_line("immediate", 150.0, recorded_s=1),
        arc_line("qg", 180.0, recorded_s=1),
        args=reference(),
    )
    assert result.returncode != 0
    assert "FAIL stale" in result.stderr


def test_gate_fails_for_wrong_runner(tmp_path):
    result = run_gate(
        tmp_path,
        arc_line("immediate", 150.0, runner="other"),
        arc_line("qg", 180.0, runner="other"),
        args=reference(),
    )
    assert result.returncode != 0
    assert "no ARC samples" in result.stderr


def test_gate_fails_on_incomplete_drain(tmp_path):
    result = run_gate(
        tmp_path,
        arc_line("immediate", 150.0),
        arc_line("qg", 180.0, releases=512),
        args=reference(),
    )
    assert result.returncode != 0
    assert "FAIL incomplete" in result.stderr


def test_gate_is_inactive_without_reference_runner(tmp_path):
    result = run_gate(
        tmp_path,
        arc_line("immediate", 150.0),
        arc_line("qg", 500.0),
    )
    assert result.returncode == 0, result.stderr
    assert "status=inactive" in result.stdout


def test_gate_selftest_and_nightly_history_selftest():
    gate = subprocess.run(
        [sys.executable, str(GATE), "--selftest"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert gate.returncode == 0, gate.stderr
    regression = subprocess.run(
        [sys.executable, str(REGRESSION), "--selftest"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert regression.returncode == 0, regression.stderr
