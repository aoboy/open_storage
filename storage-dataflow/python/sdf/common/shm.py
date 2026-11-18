"""C<->Python shared-memory handoff for the WM profile (plan prompt
W2.4, ADR-0002).

Pure ``mmap`` + ``struct`` mirror of the C segment contract in
``include/sdf/wm_shm.h`` — the Python side is the profile WRITER and
the telemetry READER (the production direction), with a telemetry
push helper for tests and loopback use.

Layout (offsets are ABI, shared with the C side):

    0x0000  seqlock (uint32 seq; byte at +4 = active_buf)
    0x0040  profile buffer A   (version, m, n, pad, down_mask, b[64])
    0x0800  profile buffer B
    0x0FC0  telemetry header   (magic, n_paths, down_mask)
    0x1000  telemetry ring     (SPSC: head u64, tail u64, mask u32,
                                slot_size u32, slots at +192)
"""

from __future__ import annotations

import mmap
import os
import struct
import time

SEQLOCK_OFF = 0x0000
ACTIVE_BYTE_OFF = 0x0004
PROFILE_A_OFF = 0x0040
PROFILE_B_OFF = 0x0800
TELEM_HDR_OFF = 0x0FC0
TELEM_RING_OFF = 0x1000
MIN_SIZE = 0x2000
WM_SHM_MAGIC = 0x53444631
MAX_PATHS = 64

# Telemetry ring constants (mirror of src/c/whack_a_mole/wm_shm.c).
_TELEM_CAP = 32
_TELEM_SLOT_FMT = "<IIQQQQQdd"  # id, pad, reqs_out/done/failed, bytes_rx, in_flight, lats
_TELEM_SLOT_SIZE = struct.calcsize(_TELEM_SLOT_FMT)

_PROFILE_FMT = "<IIIIQ"  # version, m, n, pad, down_mask (b[] follows)
_TELEM_HDR_FMT = "<IIQ"  # magic, n_paths, down_mask


class ShmError(Exception):
    """Raised on protocol violations (bad magic, wrong n_paths)."""


class WmShm:
    """The control-plane side of one WM segment."""

    def __init__(self, name: str, n_paths: int, *, create: bool = False) -> None:
        if not 0 < n_paths <= MAX_PATHS:
            raise ValueError("n_paths out of range")
        norm = name if name.startswith("/") else "/" + name
        path = "/dev/shm" + norm
        if create:
            fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
            os.ftruncate(fd, MIN_SIZE)
        else:
            fd = os.open(path, os.O_RDWR)
        self.n_paths = n_paths
        self._fd = fd
        self._mem = mmap.mmap(fd, MIN_SIZE)
        self._closed = False
        if create:
            self._mem[:] = b"\0" * MIN_SIZE
            struct.pack_into(_TELEM_HDR_FMT, self._mem, TELEM_HDR_OFF,
                             WM_SHM_MAGIC, n_paths, 0)
            # initialize the SPSC ring exactly as spsc_init does
            struct.pack_into("<Q", self._mem, TELEM_RING_OFF, 0)  # head
            struct.pack_into("<Q", self._mem, TELEM_RING_OFF + 64, 0)  # tail
            struct.pack_into("<I", self._mem, TELEM_RING_OFF + 128,
                             _TELEM_CAP - 1)  # mask
            struct.pack_into("<I", self._mem, TELEM_RING_OFF + 132,
                             _TELEM_SLOT_SIZE)  # slot_size
        else:
            magic, got_n, _ = struct.unpack_from(_TELEM_HDR_FMT, self._mem,
                                                  TELEM_HDR_OFF)
            if magic != WM_SHM_MAGIC or got_n != n_paths:
                self.close()
                raise ShmError("segment magic/n_paths mismatch")

    # ---- profile publication (writer side) --------------------------

    def publish(self, version: int, m: int, n: int, b: list[int]) -> None:
        """Fill the inactive buffer, then flip active_buf under the
        seqlock write section (the ADR-0002 protocol; single writer)."""
        if n != self.n_paths or len(b) < n:
            raise ValueError("profile shape mismatch")
        active = self._mem[ACTIVE_BYTE_OFF]
        dst = PROFILE_A_OFF if active == 1 else PROFILE_B_OFF
        struct.pack_into(_PROFILE_FMT, self._mem, dst, version, m, n, 0, 0)
        for i in range(MAX_PATHS):
            unit = b[i] if i < n else 0
            struct.pack_into("<I", self._mem, dst + _PROFILE_FMT_SIZE + 4 * i,
                             unit)
        seq = struct.unpack_from("<I", self._mem, SEQLOCK_OFF)[0]
        struct.pack_into("<I", self._mem, SEQLOCK_OFF, seq + 1)  # odd
        self._mem[ACTIVE_BYTE_OFF] = 1 - active
        struct.pack_into("<I", self._mem, SEQLOCK_OFF, seq + 2)  # even

    def acquire(self) -> tuple[int, int, int, list[int]]:
        """Snapshot the active profile under the read seqlock (single
        reader); retries on overlap. Returns (version, m, n, b)."""
        for _ in range(1000):
            s0 = struct.unpack_from("<I", self._mem, SEQLOCK_OFF)[0]
            if s0 & 1:
                continue
            active = self._mem[ACTIVE_BYTE_OFF]
            src = PROFILE_B_OFF if active == 1 else PROFILE_A_OFF
            version, m, n, _pad, _dm = struct.unpack_from(
                _PROFILE_FMT, self._mem, src)
            b = list(struct.unpack_from("<64I", self._mem,
                                        src + _PROFILE_FMT_SIZE))
            s1 = struct.unpack_from("<I", self._mem, SEQLOCK_OFF)[0]
            if s0 == s1 and not (s1 & 1):
                return version, m, n, b
        raise ShmError("seqlock never settled")

    # ---- telemetry (reader side; writer helper for tests) ------------

    def read_telemetry(self) -> list[tuple]:
        """Drain all available slots (consumer side of the SPSC ring).

        Ring layout mirrors spsc_ring_t exactly: head at +0, tail at
        +64 (its own cacheline), mask at +128, slots at +192.
        """
        out: list[tuple] = []
        head = struct.unpack_from("<Q", self._mem, TELEM_RING_OFF)[0]
        tail = struct.unpack_from("<Q", self._mem, TELEM_RING_OFF + 64)[0]
        mask = struct.unpack_from("<I", self._mem, TELEM_RING_OFF + 128)[0]
        while head != tail:
            slot_off = TELEM_RING_OFF + 192 + (tail & mask) * _TELEM_SLOT_SIZE
            out.append(struct.unpack_from(_TELEM_SLOT_FMT, self._mem, slot_off))
            tail += 1
        if out:
            struct.pack_into("<Q", self._mem, TELEM_RING_OFF + 64, tail)
        return out

    def push_telemetry(self, stats: dict) -> bool:
        """Producer side of the ring — test/loopback use (the production
        direction is C -> Python). Returns False when full (-EAGAIN)."""
        head = struct.unpack_from("<Q", self._mem, TELEM_RING_OFF)[0]
        tail = struct.unpack_from("<Q", self._mem, TELEM_RING_OFF + 64)[0]
        mask = struct.unpack_from("<I", self._mem, TELEM_RING_OFF + 128)[0]
        if head - tail >= mask + 1:
            return False
        slot_off = TELEM_RING_OFF + 192 + (head & mask) * _TELEM_SLOT_SIZE
        struct.pack_into(_TELEM_SLOT_FMT, self._mem, slot_off,
                          stats["id"], 0, stats["reqs_out"],
                          stats["reqs_done"], stats["reqs_failed"],
                          stats["bytes_rx"], stats["in_flight"],
                          stats["ewma_lat_ns"], stats["ewma_err"])
        struct.pack_into("<Q", self._mem, TELEM_RING_OFF, head + 1)
        return True

    # ---- C fast-path deference (design 01 open question 3) ----------

    def down_mask(self) -> int:
        return struct.unpack_from(_TELEM_HDR_FMT, self._mem,
                                  TELEM_HDR_OFF)[2]

    # ---- liveness ----------------------------------------------------

    def close(self) -> None:
        if not self._closed:
            self._mem.close()
            os.close(self._fd)
            self._closed = True

    def __enter__(self) -> "WmShm":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


_PROFILE_FMT_SIZE = struct.calcsize(_PROFILE_FMT)


def unlink(name: str) -> None:
    norm = name if name.startswith("/") else "/" + name
    try:
        os.unlink("/dev/shm" + norm)
    except FileNotFoundError:
        pass
