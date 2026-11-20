"""Paired fountain history summarizer tests."""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import bench_fountain_history as history  # noqa: E402


def line(variant: str, p99: float, blocks_s: float) -> str:
    return (
        f"BENCH fountain variant={variant} revision=r1 runner=box build=Release cc=gcc "
        "codec=lt k=16 sym_len=64 overhead_pct=100 loss_pct=0 blocks=8 symbols=32 "
        f"ns_per_block=10 p99_ns_per_block={p99} blocks_per_s={blocks_s}"
    )


def test_summarize_two_nights(tmp_path: Path) -> None:
    hist = tmp_path / "history"
    hist.mkdir()
    (hist / "run1.txt").write_text(
        line("c", 10, 100) + "\n" + line("rust", 5, 200) + "\n"
    )
    (hist / "run2.txt").write_text(
        line("c", 10, 100) + "\n" + line("rust", 15, 50) + "\n"
    )

    report = history.summarize(hist)
    assert report["errors"] == []
    assert report["files"] == ["run1.txt", "run2.txt"]
    assert report["runners"] == ["box"]
    item = next(iter(report["configs"].values()))
    assert item["nights"] == 2
    assert item["median_p99_ratio"] == 1.0
    assert item["median_throughput_ratio"] == 1.25


def test_incomplete_pair_is_an_error(tmp_path: Path) -> None:
    hist = tmp_path / "history"
    hist.mkdir()
    (hist / "run1.txt").write_text(line("c", 10, 100) + "\n")
    report = history.summarize(hist)
    assert report["errors"]
    out = tmp_path / "history.json"
    assert history.main(["--history", str(hist), "--out", str(out)]) == 1
    assert json.loads(out.read_text(encoding="utf-8"))["errors"]


def test_min_nights_gate(tmp_path: Path) -> None:
    hist = tmp_path / "history"
    hist.mkdir()
    (hist / "run1.txt").write_text(
        line("c", 10, 100) + "\n" + line("rust", 10, 100) + "\n"
    )
    out = tmp_path / "history.json"
    assert (
        history.main(["--history", str(hist), "--out", str(out), "--min-nights", "2"])
        == 1
    )
    assert (
        history.main(["--history", str(hist), "--out", str(out), "--min-nights", "1"])
        == 0
    )
