#!/usr/bin/env python3
"""Regenerate the persistent WM profile-update corpus (plan W6.11).

Format (little-endian): 8s magic "WMPROF1\\0", u32 n, u32 m, u32 b[n],
u32 e[n], i32 mode, i32 expected_rc, u32 expected_b[n].
"""

from pathlib import Path
import struct

EINVAL = -22


def case(
    name: str, b: list[int], e: list[int], mode: int, rc: int, expected: list[int]
) -> tuple[str, bytes]:
    assert len(b) == len(e) == len(expected)
    m = sum(b)
    out = b"WMPROF1\0" + struct.pack("<II", len(b), m)
    out += struct.pack(f"<{len(b)}I", *b)
    out += struct.pack(f"<{len(b)}I", *e)
    out += struct.pack("<ii", mode, rc)
    out += struct.pack(f"<{len(b)}I", *expected)
    return name, out


def main() -> None:
    out = Path(__file__).resolve().parent
    cases = [
        case("mode1-single-hot.bin", [22, 21, 21], [10, 0, 0], 1, 0, [16, 24, 24]),
        case("mode2-multi.bin", [22, 21, 21], [5, 4, 3], 2, 0, [21, 21, 22]),
        case("mode3-kbar.bin", [22, 21, 21], [5, 0, 0], 3, 0, [17, 24, 23]),
        case("mode4-proportional.bin", [22, 21, 21], [5, 4, 3], 4, 0, [21, 21, 22]),
        case("last-path-zeroed.bin", [63, 1], [0, 1], 3, 0, [64, 0]),
        case(
            "reject-oversized-e.bin", [22, 21, 21], [23, 0, 0], 1, EINVAL, [22, 21, 21]
        ),
        case(
            "reject-unknown-mode.bin", [22, 21, 21], [0, 0, 0], 5, EINVAL, [22, 21, 21]
        ),
    ]
    for name, data in cases:
        (out / name).write_bytes(data)


if __name__ == "__main__":
    main()
