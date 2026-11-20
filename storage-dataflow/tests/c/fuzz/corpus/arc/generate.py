#!/usr/bin/env python3
"""Regenerate the deterministic ARC replay corpus (plan W6.11).

The replay format records wall-clock timestamps explicitly; the fixed base
below makes these files independent of when the regression runs.
"""

from pathlib import Path
import struct

MAGIC = b"ARCSTRM1"
SUBMIT = 1
POLL = 2
BASE = 10_000_000_000
FINAL = BASE + 600_000_000_000


def header(capacity, entry_max, batch_max, mode, min_smooth, g, ops, releases, seed):
    return struct.pack(
        "<8sIIIIQQQQQQQQII",
        MAGIC,
        capacity,
        entry_max,
        batch_max,
        mode,
        min_smooth,
        100_000_000,
        50_000_000,
        16_666_667,
        g,
        1_000_000_000,
        1 << 62,
        seed,
        ops,
        releases,
    )


def submit(identity, length, expected, producer_ts, arrival_ts, payload):
    assert len(payload) == length
    return (
        struct.pack(
            "<BIIiQQ", SUBMIT, identity, length, expected, producer_ts, arrival_ts
        )
        + payload
    )


def poll(now):
    return struct.pack("<BQ", POLL, now)


def park_632():
    seed = (632 * 0x9E3779B97F4A7C15 + 1) & ((1 << 64) - 1)
    shapes = [
        (34_702_554, 12_192_419),
        (6_967_016, 7_433_405),
        (-30_000_000_000, 4_153_203),
        (-30_000_000_000, 2_061_783),
        (25_115_264, 17_114_965),
        (72_216_352, 21_805_638),
    ]
    ops = [header(32, 16, 32, 2, 1, 50_000_000, 14, 6, seed)]
    clock = BASE
    for identity, (early, advance) in enumerate(shapes):
        payload = bytes([identity]) * 16
        ops.append(submit(identity, 16, 0, clock - early, clock, payload))
        clock += advance
        ops.append(poll(clock))
    ops.append(poll(FINAL))
    ops.append(poll(FINAL))
    return b"".join(ops)


def saturation():
    ops = [header(2, 16, 2, 3, 1, 50_000_000, 7, 3, 0)]
    for identity in range(3):
        payload = bytes([0x40 + identity]) * 16
        # IMMEDIATE accepts with rc=1 (bypass-scheduled); EAGAIN while both slots live.
        expected = -11 if identity == 2 else 1
        ops.append(submit(identity, 16, expected, BASE - 30_000_000, BASE, payload))
    ops.append(poll(FINAL))
    ops.append(submit(2, 16, 1, BASE - 30_000_000, BASE, b"\x42" * 16))
    ops.append(poll(FINAL))
    ops.append(poll(FINAL))
    return b"".join(ops)


def main():
    out = Path(__file__).resolve().parent
    (out / "qg-park-632.bin").write_bytes(park_632())
    (out / "saturation-retry.bin").write_bytes(saturation())


if __name__ == "__main__":
    main()
