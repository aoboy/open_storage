#!/usr/bin/env python3
"""Regenerate the persistent fountain replay corpus (plan W6.11).

Each file is a raw ``fuzz_fountain`` stream: one codec byte, little-endian
``k-1`` and ``sym_len-1``, then records of ``u32 esi`` + ``sym_len`` bytes.
Replay keeps ESIs exactly as written (the random fuzzer remaps one quarter of
them into the valid window).

The nanorq K=512 case is the historical 512-entry ESI-bitmap regression: a
repair at ESI 512 used to alias the end of ``have[]``. The remaining files pin
the K=257 boundary, both codec extremes, duplicate accounting, and a truncated
record tail.
"""

from pathlib import Path
import struct

LT = 0
NANORQ = 1


def stream(codec: int, k: int, sym_len: int, records: list[tuple[int, bytes]]) -> bytes:
    assert 1 <= k <= (512 if codec == NANORQ else 64)
    assert 1 <= sym_len <= (8192 if codec == NANORQ else 256)
    out = bytes([codec]) + struct.pack("<HH", k - 1, sym_len - 1)
    for esi, payload in records:
        assert len(payload) == sym_len
        out += struct.pack("<I", esi) + payload
    return out


def source_records(k: int, sym_len: int) -> list[tuple[int, bytes]]:
    return [
        (esi, bytes([(esi + b) & 0xFF for b in range(sym_len)])) for esi in range(k)
    ]


def main() -> None:
    out = Path(__file__).resolve().parent
    files = {
        # Repair 512 arrives before the block is complete. With the old
        # 512-entry bitmap it aliased `repaired` and reported false
        # readiness at rank 1; the replay invariant catches that.
        "nanorq-k512-t1-repair-boundary.bin": stream(
            NANORQ, 512, 1, [(512, b"\xa5")] + source_records(512, 1)
        ),
        "nanorq-k257-t1-source-set.bin": stream(NANORQ, 257, 1, source_records(257, 1)),
        "nanorq-k1-t1024-header-only.bin": stream(NANORQ, 1, 1024, []),
        "lt-k64-t1-source-set.bin": stream(LT, 64, 1, source_records(64, 1)),
        "lt-k1-t256-source-one.bin": stream(
            LT, 1, 256, [(0, bytes((i * 7 + 3) & 0xFF for i in range(256)))]
        ),
        "lt-k8-t8-duplicates-and-repair.bin": stream(
            LT,
            8,
            8,
            [(0, b"\x00" * 8), (0, b"\x00" * 8)]
            + [(esi, bytes([esi]) * 8) for esi in range(1, 8)]
            + [(8, b"\x55" * 8)],
        ),
    }
    # A partial trailing record must be ignored, not read past the file.
    files["lt-k4-t4-truncated-record.bin"] = (
        stream(LT, 4, 4, source_records(4, 4)) + struct.pack("<I", 99) + b"\x00\x00"
    )
    for name, data in files.items():
        (out / name).write_bytes(data)


if __name__ == "__main__":
    main()
