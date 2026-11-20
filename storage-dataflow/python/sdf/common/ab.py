"""Deterministic A/B assignment."""

from __future__ import annotations

import xxhash


def bucket(key: str | bytes, buckets: int = 100) -> int:
    """Map a key to ``0 <= bucket < buckets`` using xxhash64."""
    if buckets <= 0:
        raise ValueError("buckets must be positive")
    if isinstance(key, str):
        key = key.encode("utf-8")
    digest = int(xxhash.xxh64(key).intdigest())
    return digest % buckets
