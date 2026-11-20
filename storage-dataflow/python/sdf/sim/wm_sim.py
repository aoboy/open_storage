"""Whack-a-Mole plain-spray simulator (design 01).

This is the Python reference model for the C plain path: bit-reversal theta,
the cumulative unit table, and counter-window selection. It is deliberately
plain-only; seeded shuffle methods and SHM publication live in the C/control
plane. The checked-in ``wm_select.json`` goldens are the parity oracle.
"""

from __future__ import annotations

import argparse
from collections.abc import Sequence
from dataclasses import dataclass


def bit_reverse(value: int, bits: int) -> int:
    """Reverse the low ``bits`` bits of ``value`` (the paper's theta)."""
    if bits < 0 or value < 0 or value >= (1 << bits):
        raise ValueError("value does not fit in the requested bit width")
    reversed_value = 0
    for _ in range(bits):
        reversed_value = (reversed_value << 1) | (value & 1)
        value >>= 1
    return reversed_value


@dataclass(frozen=True)
class CompiledPlain:
    """Plain profile tables: theta and the counter-to-path mapping."""

    m: int
    b: tuple[int, ...]
    perm: tuple[int, ...]
    unit_to_path: tuple[int, ...]

    @property
    def n(self) -> int:
        return len(self.b)


def compile_plain(m: int, b: Sequence[int]) -> CompiledPlain:
    """Compile ``b`` over ``m`` units, mirroring ``wm_profile_compile``."""
    if m < 2 or (m & (m - 1)) != 0:
        raise ValueError("m must be a power of two >= 2")
    if not b:
        raise ValueError("b must not be empty")
    if any(value < 0 for value in b):
        raise ValueError("b entries must be non-negative")
    if sum(b) != m:
        raise ValueError("sum(b) must equal m")

    bits = m.bit_length() - 1
    cdf: list[int] = []
    total = 0
    for value in b:
        total += value
        cdf.append(total)

    perm: list[int] = []
    unit_to_path: list[int] = []
    for counter in range(m):
        key = bit_reverse(counter, bits)
        path = 0
        while path + 1 < len(cdf) and key >= cdf[path]:
            path += 1
        perm.append(key)
        unit_to_path.append(path)
    return CompiledPlain(
        m=m,
        b=tuple(int(value) for value in b),
        perm=tuple(perm),
        unit_to_path=tuple(unit_to_path),
    )


def select(compiled: CompiledPlain, counter: int) -> int:
    """Select one path for a monotonic counter (plain method)."""
    if counter < 0:
        raise ValueError("counter must be non-negative")
    return compiled.unit_to_path[counter & (compiled.m - 1)]


def select_range(compiled: CompiledPlain, start: int, count: int) -> list[int]:
    """Return the plain path sequence for ``count`` consecutive counters."""
    if start < 0 or count < 0:
        raise ValueError("start and count must be non-negative")
    return [select(compiled, start + offset) for offset in range(count)]


def window_counts(compiled: CompiledPlain, start: int, length: int) -> list[int]:
    """Count each path over ``length`` counters starting at ``start``."""
    counts = [0] * compiled.n
    for path in select_range(compiled, start, length):
        counts[path] += 1
    return counts


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--m", type=int, default=1024)
    parser.add_argument("--b", default="127,400,200,173,124")
    parser.add_argument("--from", dest="start", type=int, default=0)
    parser.add_argument("--count", type=int, default=64)
    args = parser.parse_args(argv)

    b = [int(token) for token in args.b.split(",") if token.strip()]
    compiled = compile_plain(args.m, b)
    print(",".join(str(path) for path in select_range(compiled, args.start, args.count)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
