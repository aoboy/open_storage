"""Deterministic A/B assignment.

The bucket is ``xxhash64(key) % buckets`` (design 00 §7.4, design 01).
The optional ``xxhash`` extension is used when present; a small pure-Python
XXH64 implements the same function when it is not, so control-plane imports
never depend on an undeclared runtime wheel.
"""

from __future__ import annotations

import importlib
from contextlib import suppress
from typing import Any

_MASK = (1 << 64) - 1
_P1 = 11400714785074694791
_P2 = 14029467366897019727
_P3 = 1609587929392839161
_P4 = 9650029242287828579
_P5 = 2870177450012600261

_xxhash: Any = None
with suppress(ImportError):
    _xxhash = importlib.import_module("xxhash")


def _rotl(value: int, bits: int) -> int:
    return ((value << bits) | (value >> (64 - bits))) & _MASK


def _round(acc: int, value: int) -> int:
    acc = (acc + value * _P2) & _MASK
    acc = _rotl(acc, 31)
    return (acc * _P1) & _MASK


def _merge_round(acc: int, value: int) -> int:
    acc ^= _round(0, value)
    return (acc * _P1 + _P4) & _MASK


def _xxh64(data: bytes, seed: int = 0) -> int:
    """XXH64 with seed 0, matching the ``xxhash`` extension's intdigest()."""
    length = len(data)
    offset = 0
    if length >= 32:
        v1 = (seed + _P1 + _P2) & _MASK
        v2 = (seed + _P2) & _MASK
        v3 = seed & _MASK
        v4 = (seed - _P1) & _MASK
        while offset <= length - 32:
            v1 = _round(v1, int.from_bytes(data[offset : offset + 8], "little"))
            offset += 8
            v2 = _round(v2, int.from_bytes(data[offset : offset + 8], "little"))
            offset += 8
            v3 = _round(v3, int.from_bytes(data[offset : offset + 8], "little"))
            offset += 8
            v4 = _round(v4, int.from_bytes(data[offset : offset + 8], "little"))
            offset += 8
        digest = (_rotl(v1, 1) + _rotl(v2, 7) + _rotl(v3, 12) + _rotl(v4, 18)) & _MASK
        digest = _merge_round(digest, v1)
        digest = _merge_round(digest, v2)
        digest = _merge_round(digest, v3)
        digest = _merge_round(digest, v4)
    else:
        digest = (seed + _P5) & _MASK
    digest = (digest + length) & _MASK
    while offset <= length - 8:
        digest ^= _round(0, int.from_bytes(data[offset : offset + 8], "little"))
        digest = (_rotl(digest, 27) * _P1 + _P4) & _MASK
        offset += 8
    if offset <= length - 4:
        digest ^= (int.from_bytes(data[offset : offset + 4], "little") * _P1) & _MASK
        digest = (_rotl(digest, 23) * _P2 + _P3) & _MASK
        offset += 4
    while offset < length:
        digest ^= (data[offset] * _P5) & _MASK
        digest = (_rotl(digest, 11) * _P1) & _MASK
        offset += 1
    digest ^= digest >> 33
    digest = (digest * _P2) & _MASK
    digest ^= digest >> 29
    digest = (digest * _P3) & _MASK
    digest ^= digest >> 32
    return digest


def _digest(key: bytes) -> int:
    if _xxhash is not None:
        return int(_xxhash.xxh64(key).intdigest())
    return _xxh64(key)


def bucket(key: str | bytes, buckets: int = 100) -> int:
    """Map a key to ``0 <= bucket < buckets`` using xxhash64."""
    if buckets <= 0:
        raise ValueError("buckets must be positive")
    if isinstance(key, str):
        key = key.encode("utf-8")
    return _digest(key) % buckets
