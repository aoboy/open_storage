"""Deterministic A/B bucketing tests (design 00 §7.4)."""

from __future__ import annotations

import pytest

from sdf.common.ab import _xxh64, bucket


def test_xxh64_known_vectors() -> None:
    assert _xxh64(b"") == 0xEF46DB3751D8E999
    assert _xxh64(b"a") == 0xD24EC4F1A98C6E5B
    assert _xxh64(b"abc") == 0x44BC2CF5AD770999


def test_bucket_is_xxhash64_modulo() -> None:
    assert bucket("abc", 100) == (0x44BC2CF5AD770999 % 100)
    assert bucket(b"abc", 100) == bucket("abc", 100)
    assert 0 <= bucket("job-17") < 100


def test_bucket_validation() -> None:
    with pytest.raises(ValueError):
        bucket("x", 0)
    with pytest.raises(ValueError):
        bucket("x", -1)
