"""Benchmark plotting helpers (design 02, plan W3.6).

The C coordinators record one ``REPAIR arm=... overhead=... wall_ms=...``
line per run. This module parses those records and renders a dependency-free
SVG scatter plus CSV so the Pareto experiment is reproducible without
matplotlib. It plots recorded wall time, not a synthesized p99.
"""

from __future__ import annotations

import argparse
import csv
import html
import io
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from pathlib import Path

_PALETTE = ("#1f77b4", "#d62728", "#2ca02c", "#9467bd", "#ff7f0e", "#17becf")


@dataclass(frozen=True)
class ResultPoint:
    """One recorded repair result."""

    series: str
    x: float
    y: float
    label: str


def _fields(line: str, lineno: int) -> dict[str, str]:
    values: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" not in token:
            raise ValueError(f"line {lineno}: token {token!r} is not key=value")
        key, value = token.split("=", 1)
        if key in values:
            raise ValueError(f"line {lineno}: duplicate field {key!r}")
        values[key] = value
    return values


def parse_repair_lines(text: str) -> list[ResultPoint]:
    """Parse ``REPAIR`` records; non-REPAIR lines are ignored."""
    points: list[ResultPoint] = []
    for lineno, line in enumerate(text.splitlines(), 1):
        if not line.startswith("REPAIR "):
            continue
        fields = _fields(line, lineno)
        missing = [key for key in ("arm", "overhead", "wall_ms", "loss") if key not in fields]
        if missing:
            raise ValueError(f"line {lineno}: missing {', '.join(missing)}")
        try:
            overhead = float(fields["overhead"])
            wall_ms = float(fields["wall_ms"])
            loss = float(fields["loss"])
        except ValueError as exc:
            raise ValueError(f"line {lineno}: non-numeric metric") from exc
        if overhead < 0.0 or wall_ms < 0.0 or loss < 0.0:
            raise ValueError(f"line {lineno}: metrics must be non-negative")
        points.append(
            ResultPoint(
                series=fields["arm"],
                x=overhead,
                y=wall_ms,
                label=f"loss={loss:.0f}%",
            )
        )
    return points


def _dominates(left: ResultPoint, right: ResultPoint) -> bool:
    no_worse = left.x <= right.x and left.y <= right.y
    strictly_better = left.x < right.x or left.y < right.y
    return no_worse and strictly_better


def pareto_frontier(points: Iterable[ResultPoint]) -> tuple[ResultPoint, ...]:
    """Return the non-dominated points, sorted for deterministic output."""
    items = list(points)
    kept = [
        point
        for point in items
        if not any(other is not point and _dominates(other, point) for other in items)
    ]
    return tuple(sorted(kept, key=lambda point: (point.x, point.y, point.series, point.label)))


def to_csv(points: Sequence[ResultPoint]) -> str:
    """Render points as ``series,x,y,label`` CSV text."""
    out = io.StringIO()
    writer = csv.writer(out, lineterminator="\n")
    writer.writerow(("series", "x", "y", "label"))
    for point in points:
        writer.writerow((point.series, f"{point.x:.6g}", f"{point.y:.6g}", point.label))
    return out.getvalue()


def _number(value: float) -> str:
    return f"{value:.3g}"


def render_svg(
    points: Sequence[ResultPoint],
    *,
    title: str = "FLUID repair: recorded wall time vs overhead",
    x_label: str = "bandwidth overhead (bytes_tx / object)",
    y_label: str = "recorded wall time (ms)",
    width: int = 760,
    height: int = 480,
) -> str:
    """Render a self-contained SVG scatter plot."""
    if not points:
        raise ValueError("points must not be empty")
    if width < 320 or height < 240:
        raise ValueError("width and height are too small")

    left, right, top, bottom = 78.0, 24.0, 30.0, 62.0
    plot_w = width - left - right
    plot_h = height - top - bottom
    x_max = max(point.x for point in points)
    y_max = max(point.y for point in points)
    x_hi = x_max * 1.08 if x_max > 0.0 else 1.0
    y_hi = y_max * 1.10 if y_max > 0.0 else 1.0

    def sx(value: float) -> float:
        return left + value / x_hi * plot_w

    def sy(value: float) -> float:
        return top + plot_h - value / y_hi * plot_h

    series = sorted({point.series for point in points})
    colors = {name: _PALETTE[index % len(_PALETTE)] for index, name in enumerate(series)}
    title_id = "sdf-bench-plot-title"

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" role="img" aria-labelledby="{title_id}">',
        f'<title id="{title_id}">{html.escape(title)}</title>',
        f'<rect x="0" y="0" width="{width}" height="{height}" fill="#ffffff"/>',
        f'<text x="{left}" y="20" font-family="sans-serif" font-size="15" '
        f'fill="#222">{html.escape(title)}</text>',
    ]

    for step in range(6):
        fraction = step / 5.0
        y = top + plot_h - fraction * plot_h
        value = fraction * y_hi
        parts.append(
            f'<line x1="{left:.2f}" y1="{y:.2f}" x2="{left + plot_w:.2f}" y2="{y:.2f}" '
            'stroke="#e2e2e2" stroke-width="1"/>'
        )
        parts.append(
            f'<text x="{left - 8:.2f}" y="{y + 4:.2f}" text-anchor="end" '
            f'font-family="sans-serif" font-size="11" fill="#555">{_number(value)}</text>'
        )
        x = left + fraction * plot_w
        x_value = fraction * x_hi
        parts.append(
            f'<line x1="{x:.2f}" y1="{top:.2f}" x2="{x:.2f}" y2="{top + plot_h:.2f}" '
            'stroke="#f0f0f0" stroke-width="1"/>'
        )
        parts.append(
            f'<text x="{x:.2f}" y="{top + plot_h + 18:.2f}" text-anchor="middle" '
            f'font-family="sans-serif" font-size="11" fill="#555">{_number(x_value)}</text>'
        )

    parts.extend(
        (
            f'<line x1="{left:.2f}" y1="{top + plot_h:.2f}" x2="{left + plot_w:.2f}" '
            f'y2="{top + plot_h:.2f}" stroke="#333" stroke-width="1.5"/>',
            f'<line x1="{left:.2f}" y1="{top:.2f}" x2="{left:.2f}" '
            f'y2="{top + plot_h:.2f}" stroke="#333" stroke-width="1.5"/>',
            f'<text x="{left + plot_w / 2:.2f}" y="{height - 16:.2f}" text-anchor="middle" '
            f'font-family="sans-serif" font-size="12" fill="#333">{html.escape(x_label)}</text>',
            f'<text x="16" y="{top + plot_h / 2:.2f}" text-anchor="middle" '
            f'transform="rotate(-90 16 {top + plot_h / 2:.2f})" '
            f'font-family="sans-serif" font-size="12" fill="#333">{html.escape(y_label)}</text>',
        )
    )

    for point in points:
        x = sx(point.x)
        y = sy(point.y)
        color = colors[point.series]
        parts.append(
            f'<circle cx="{x:.2f}" cy="{y:.2f}" r="4.5" fill="{color}" '
            f'stroke="#ffffff" stroke-width="1"><title>{html.escape(point.series)} '
            f"{html.escape(point.label)}: overhead={point.x:.3g}, "
            f"wall_ms={point.y:.3g}</title></circle>"
        )

    legend_x = left + plot_w - 150.0
    legend_y = top + 8.0
    for index, name in enumerate(series):
        y = legend_y + index * 18.0
        parts.append(
            f'<rect x="{legend_x:.2f}" y="{y - 8:.2f}" width="10" height="10" '
            f'fill="{colors[name]}"/>'
        )
        parts.append(
            f'<text x="{legend_x + 16:.2f}" y="{y:.2f}" font-family="sans-serif" '
            f'font-size="12" fill="#333">{html.escape(name)}</text>'
        )

    parts.append("</svg>")
    return "\n".join(parts) + "\n"


def write_report(
    points: Sequence[ResultPoint],
    out_dir: Path,
    *,
    stem: str = "repair_pareto",
) -> tuple[Path, Path]:
    """Write ``<stem>.csv`` and ``<stem>.svg``; return both paths."""
    if not points:
        raise ValueError("points must not be empty")
    out_dir.mkdir(parents=True, exist_ok=True)
    csv_path = out_dir / f"{stem}.csv"
    svg_path = out_dir / f"{stem}.svg"
    csv_path.write_text(to_csv(points), encoding="utf-8")
    svg_path.write_text(render_svg(points), encoding="utf-8")
    return csv_path, svg_path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path, help="file containing REPAIR lines")
    parser.add_argument("--out", type=Path, default=Path("out/bench"))
    parser.add_argument("--stem", default="repair_pareto")
    args = parser.parse_args(argv)

    points = parse_repair_lines(args.results.read_text(encoding="utf-8"))
    if not points:
        parser.error(f"no REPAIR records in {args.results}")
    csv_path, svg_path = write_report(points, args.out, stem=args.stem)
    print(f"wrote {csv_path} and {svg_path} ({len(points)} points)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
