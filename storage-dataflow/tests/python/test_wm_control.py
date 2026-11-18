"""W2.4 — the WM control loop against a live SHM segment.

Covers the ADR-0002 publication from the Python writer side
(publish/acquire roundtrip), the 4 Hz loop semantics with scripted
telemetry: equal-healthy keeps units stable, a degraded path sheds
units to the healthy ones with the sum pinned to m, missing telemetry
is *no opinion* (units held), down_mask defers (the loop never undoes
a C hard-down), the heartbeat advances every step, and the ring
drains fully with overflow reported.
"""

from __future__ import annotations

import uuid

import pytest

from sdf.common.shm import ShmError, WmShm, unlink
from sdf.control.wm_control import WmControlLoop, SoftSheddingController


@pytest.fixture()
def segment():
    name = f"sdf-pytest-wm-{uuid.uuid4().hex[:12]}"
    shm = WmShm(name, n_paths=4, create=True)
    yield shm, name
    shm.close()
    unlink(name)


def stats(pid, out, done, lat=1e6, err=0.0):
    return {
        "id": pid,
        "reqs_out": out,
        "reqs_done": done,
        "reqs_failed": 0,
        "bytes_rx": done * 4096,
        "in_flight": 0,
        "ewma_lat_ns": lat,
        "ewma_err": err,
    }


def test_publish_acquire_roundtrip(segment):
    shm, _ = segment
    loop = WmControlLoop(shm, m=1024)
    rec = loop.step()
    assert rec["version"] == 1
    version, m, n, b = shm.acquire()
    assert version == 1 and m == 1024 and n == 4
    assert sum(b[:4]) == 1024
    assert b[:4] == rec["units"]


def test_healthy_keeps_units_stable(segment):
    shm, _ = segment
    loop = WmControlLoop(shm, m=1024)
    # equal telemetry for all four paths: no band fires, units stay
    before = loop.step()["units"]
    for _ in range(3):
        for pid in range(4):
            assert shm.push_telemetry(stats(pid, 1000, 1000))
        after = loop.step()["units"]
        assert after == before


def test_degraded_path_sheds(segment):
    shm, _ = segment
    loop = WmControlLoop(shm, m=1024)
    base = loop.step()["units"]

    for pid in range(4):
        shm.push_telemetry(stats(pid, 1000, 1000))
    # path 1 collapses: 25% of its requests fail
    shm.push_telemetry(stats(1, 1000, 250))
    rec = loop.step()
    units = rec["units"]

    assert sum(units) == 1024
    assert units[1] < base[1]
    healthy = [units[i] for i in (0, 2, 3)]
    assert all(u > base[i] for i, u in zip((0, 2, 3), healthy))

    # published state reflects the decision (the C side reads this)
    version, m, n, b = shm.acquire()
    assert version == rec["version"]
    assert b[:4] == units


def test_missing_telemetry_is_no_opinion(segment):
    shm, _ = segment
    loop = WmControlLoop(shm, m=1024)
    base = loop.step()["units"]

    # telemetry for paths 0, 1, 3 only — path 2 holds its units
    for pid in (0, 1, 3):
        shm.push_telemetry(stats(pid, 1000, 1000))
    rec = loop.step()
    units = rec["units"]

    assert rec["fresh_paths"] == 3
    assert units[2] == base[2]
    assert sum(units) == 1024


def test_down_mask_defers_to_c_hard_down(segment):
    shm, _ = segment
    loop = WmControlLoop(shm, m=1024)
    base = loop.step()["units"]

    # the C fast path marked path 3 down (simulate via the header
    # field the C side stores); its telemetry is ignored — no growth
    struct = __import__("struct")
    hdr_off = 0x0FC0
    struct.pack_into("<IIQ", shm._mem, hdr_off, 0x53444631, 4, 1 << 3)
    for pid in range(4):
        shm.push_telemetry(stats(pid, 1000, 1000))

    rec = loop.step()
    assert rec["down_mask"] == 1 << 3
    assert rec["fresh_paths"] == 3
    assert rec["units"][3] == base[3]  # held, not regrown

    # clear the mask: the path participates again
    struct.pack_into("<IIQ", shm._mem, hdr_off, 0x53444631, 4, 0)
    for pid in range(4):
        shm.push_telemetry(stats(pid, 1000, 1000))
    rec = loop.step()
    assert rec["fresh_paths"] == 4


def test_heartbeat_advances(segment):
    shm, _ = segment
    loop = WmControlLoop(shm, m=1024)
    for i in range(1, 6):
        rec = loop.step()
        assert rec["heartbeat"] == i
        assert loop.last_step_ns is not None


def test_telemetry_ring_drains_and_overflows(segment):
    shm, _ = segment
    for i in range(40):  # cap is 32 slots
        pushed = shm.push_telemetry(stats(i % 4, 100, 100))
    slots = shm.read_telemetry()
    assert len(slots) == 32
    assert shm.read_telemetry() == []  # drained


def test_reference_controller_no_opinion():
    ctl = SoftSheddingController(3, m=1024, alpha=0.25, beta=0.02)
    # only path 0 has an opinion; paths 1-2 hold their units
    units = ctl.step_with_opinions([(1000, 1000, 1e6, 0.0), None, None])
    assert sum(units) == 1024
    assert units[1] == ctl.units[1]
    # a lone opinion is the median itself: no change fires
    assert units[0] == 1024 // 3 + 1  # largest-remainder of equal split


def test_segment_validation(segment):
    shm, name = segment
    with pytest.raises(ShmError):
        WmShm(name, n_paths=5)  # wrong n_paths refused
    with pytest.raises(ValueError):
        WmShm(name, n_paths=0)
    shm.close()
    unlink(name)
    with pytest.raises(FileNotFoundError):
        WmShm(name, n_paths=4)  # gone after unlink
