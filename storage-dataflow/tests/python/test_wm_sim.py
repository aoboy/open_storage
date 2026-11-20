"""Plain WM spray simulator parity tests."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from sdf.sim.wm_sim import bit_reverse, compile_plain, select_range, window_counts

GOLDENS = Path(__file__).resolve().parents[2] / "golden_vectors" / "wm_select.json"


def load_golden() -> dict[str, object]:
    with GOLDENS.open(encoding="utf-8") as handle:
        return json.load(handle)


def canonical() -> tuple[int, list[int]]:
    root = load_golden()
    profile = root["provenance"]["profile"]  # type: ignore[index]
    assert isinstance(profile, dict)
    return int(profile["m"]), [int(value) for value in profile["b"]]


def test_theta_and_uniform_window() -> None:
    root = load_golden()
    m, b = canonical()
    compiled = compile_plain(m, b)
    for pair in root["theta_ell10"]:  # type: ignore[index]
        index, expected = pair
        assert compiled.perm[index] == expected
        assert bit_reverse(index, 10) == expected

    assert window_counts(compiled, 0, m) == b
    assert window_counts(compiled, 1, m) == b


def test_plain_sequence_and_windows_match_goldens() -> None:
    root = load_golden()
    m, b = canonical()
    compiled = compile_plain(m, b)

    sequence = root["plain_sequence"]  # type: ignore[index]
    assert isinstance(sequence, dict)
    paths = select_range(compiled, int(sequence["from"]), int(sequence["count"]))
    assert paths == [int(value) for value in sequence["paths"]]

    for window in root["plain_windows"]:  # type: ignore[index]
        assert isinstance(window, dict)
        counts = window_counts(compiled, int(window["from"]), int(window["len"]))
        assert counts == [int(value) for value in window["counts"]]


def test_compile_validation() -> None:
    with pytest.raises(ValueError, match="power of two"):
        compile_plain(3, [1, 2])
    with pytest.raises(ValueError, match="sum"):
        compile_plain(8, [1, 2])
    with pytest.raises(ValueError, match="non-negative"):
        compile_plain(8, [9, -1])
    with pytest.raises(ValueError, match="non-negative"):
        select_range(compile_plain(8, [4, 4]), -1, 1)
