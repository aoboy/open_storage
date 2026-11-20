"""Benchmark plot helper tests (plan W3.6)."""

from __future__ import annotations

from pathlib import Path

import pytest

from sdf.bench.plots import (
    ResultPoint,
    main,
    pareto_frontier,
    parse_repair_lines,
    render_svg,
    to_csv,
    write_report,
)

REPAIRS = """\
# recorded
== loss ladder ==
REPAIR arm=control eps=0.10 loss=0 k=64 sent=64 received=64 bytes_tx=4096 rounds=1 fallback=0 rank=64 ok=1 overhead=1.00 wall_ms=2
REPAIR arm=lt eps=0.10 loss=0 k=64 sent=64 received=64 bytes_tx=4096 rounds=1 fallback=0 rank=64 ok=1 overhead=1.00 wall_ms=3
REPAIR arm=lt eps=0.10 loss=10 k=64 sent=72 received=65 bytes_tx=4608 rounds=2 fallback=0 rank=64 ok=1 overhead=1.12 wall_ms=5
REPAIR arm=nanorq eps=0.10 loss=10 k=64 sent=72 received=72 bytes_tx=4608 rounds=1 fallback=0 rank=64 ok=1 overhead=1.12 wall_ms=4
BENCH wm_select method=plain n=5 m=1024 ns_per_dec=2.0
"""


def test_parse_repair_lines() -> None:
    points = parse_repair_lines(REPAIRS)
    assert len(points) == 4
    assert points[0] == ResultPoint("control", 1.0, 2.0, "loss=0%")
    assert points[2].series == "lt"
    assert points[2].x == pytest.approx(1.12)
    assert points[2].y == pytest.approx(5.0)


def test_parse_rejects_malformed_repair_lines() -> None:
    with pytest.raises(ValueError, match="missing arm"):
        parse_repair_lines("REPAIR overhead=1 wall_ms=2 loss=0\n")
    with pytest.raises(ValueError, match="non-numeric"):
        parse_repair_lines("REPAIR arm=lt overhead=x wall_ms=2 loss=0\n")
    with pytest.raises(ValueError, match="not key=value"):
        parse_repair_lines("REPAIR arm=lt overhead=1 wall_ms=2 loss=0 broken\n")


def test_pareto_frontier() -> None:
    points = parse_repair_lines(REPAIRS)
    frontier = pareto_frontier(points)
    # control has both the lowest overhead and the lowest wall time here,
    # so every other recorded point is dominated.
    assert frontier == (ResultPoint("control", 1.0, 2.0, "loss=0%"),)


def test_csv_and_svg() -> None:
    points = parse_repair_lines(REPAIRS)
    text = to_csv(points)
    assert text.splitlines()[0] == "series,x,y,label"
    assert "lt,1.12,5,loss=10%" in text

    svg = render_svg(points)
    assert svg.startswith("<svg")
    assert "FLUID repair" in svg
    assert "bandwidth overhead" in svg
    assert "control" in svg and "lt" in svg and "nanorq" in svg
    assert "<circle" in svg


def test_write_report_and_cli(tmp_path: Path) -> None:
    results = tmp_path / "bench_results.txt"
    results.write_text(REPAIRS, encoding="utf-8")
    csv_path, svg_path = write_report(parse_repair_lines(REPAIRS), tmp_path / "out")
    assert csv_path.is_file() and svg_path.is_file()
    assert "series,x,y,label" in csv_path.read_text(encoding="utf-8")

    assert main([str(results), "--out", str(tmp_path / "cli")]) == 0
    assert (tmp_path / "cli" / "repair_pareto.csv").is_file()
    assert (tmp_path / "cli" / "repair_pareto.svg").is_file()


def test_empty_input_is_rejected(tmp_path: Path) -> None:
    with pytest.raises(ValueError):
        render_svg([])
    with pytest.raises(ValueError):
        write_report([], tmp_path / "empty")
    results = tmp_path / "empty.txt"
    results.write_text("no repair lines\n", encoding="utf-8")
    with pytest.raises(SystemExit):
        main([str(results), "--out", str(tmp_path / "empty")])
