#!/usr/bin/env python3
"""Consume shipped golden vectors through CFFI (plan W6.11).

This is the installed-artifact counterpart to ``tests/c/test_parity``:
it loads the same checked-in JSON through the CFFI extension built
against an installed prefix, and drives the public ABI for WM, ARC, and
both real fountain codecs. The toy codec lives inside the C parity test
itself, so its comparisons are counted and reported separately rather
than silently omitted.

Run through ``scripts/test_install.sh`` (which sets ``SDF_CFFI_DIR``),
or directly against the in-tree extension:

    PYTHONPATH=python:bindings/cffi python3 bindings/cffi/check_goldens.py
"""

from __future__ import annotations

import argparse
import json
import math
import time
from pathlib import Path
from typing import Any

from sdf_cffi import ffi, lib

ROOT = Path(__file__).resolve().parents[2]
MAX_ARC_POLL_NS = (1 << 62) - 1


def fail(message: str) -> None:
    raise SystemExit(f"installed CFFI goldens: FAIL: {message}")


def load(path: Path) -> dict[str, Any]:
    try:
        with path.open(encoding="utf-8") as handle:
            data = json.load(handle)
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"cannot read {path}: {exc}")
    if not isinstance(data, dict):
        fail(f"{path} is not a JSON object")
    return data


def compile_wm(root: dict[str, Any], method: int = 0, sa: int = 0, sb: int = 1) -> Any:
    profile = root["provenance"]["profile"]
    p = ffi.new("wm_profile_t *")
    p.version = 1
    p.m = profile["m"]
    p.n = profile["n"]
    for i, value in enumerate(profile["b"]):
        p.b[i] = value
    compiled = ffi.new("wm_compiled_t *")
    if method == 0:
        rc = lib.wm_profile_compile(p, compiled)
    else:
        cfg = ffi.new("wm_compile_cfg_t *")
        cfg.method = method
        cfg.sa = sa
        cfg.sb = sb
        rc = lib.wm_profile_compile_cfg(p, compiled, cfg)
    if rc != 0:
        fail(f"wm_profile_compile returned {rc}")
    return compiled


def check_wm(path: Path) -> int:
    root = load(path)
    comparisons = 0
    compiled = compile_wm(root)

    for pair in root["theta_ell10"]:
        index, expected = pair
        got = compiled.perm[index]
        if got != expected:
            fail(f"theta[{index}]: got {got}, expected {expected}")
        comparisons += 1

    sequences = [("plain_sequence", 0), ("seed_shuffle1", 1), ("seed_shuffle2", 2)]
    for name, _method in sequences:
        seq = root[name]
        method = 0
        if "requires" in seq:
            suffix = seq["requires"].rsplit(".", 1)[-1]
            method = 1 if suffix == "shuffle1" else 2
        compiled = compile_wm(root, method, seq.get("sa", 0), seq.get("sb", 1))
        start = seq["from"]
        if len(seq["paths"]) != seq["count"]:
            fail(f"{name}: count does not match paths")
        for offset, expected in enumerate(seq["paths"]):
            counter = start + offset
            got = lib.wm_select(compiled, counter)
            if got != expected:
                fail(f"{name} counter {counter}: got path {got}, expected {expected}")
            comparisons += 1

    compiled = compile_wm(root)
    for window in root["plain_windows"]:
        start = window["from"]
        length = window["len"]
        counts = [0] * compiled.n
        for offset in range(length):
            path = lib.wm_select(compiled, start + offset)
            if path >= compiled.n:
                fail(f"window from={start} len={length}: path {path} out of range")
            counts[path] += 1
        if counts != window["counts"]:
            fail(
                f"window from={start} len={length}: got {counts}, expected {window['counts']}"
            )
        comparisons += compiled.n

    return comparisons


def check_arc(path: Path) -> int:
    root = load(path)
    comparisons = 0
    for entry in root["paper_12_3"]["entries"]:
        drive_blocks = entry["drive_blocks"]
        sample_blocks = entry["sample_blocks"]
        samples = entry["samples"]
        if len(samples) != sample_blocks:
            fail(f"{entry['name']}: sample count does not match sample_blocks")

        cfg = ffi.new("arc_cfg_t *")
        cfg.capacity = 64
        cfg.entry_max = 64
        cfg.mode = lib.ARC_MODE_BASE
        cfg.auto_params = 0
        cfg.min_smooth_bytes = 0
        cfg.rho_u = entry["rho_u"]
        cfg.rho_l = entry["rho_l"]
        cfg.lambda_u = entry["lambda_u"]
        cfg.lambda_l = entry["lambda_l"]
        cfg.U = entry["U_ns"]
        cfg.delta = entry["delta_ns"]
        cfg.J = entry["J_ns"]
        cfg.G = 50_000_000
        cfg.T_idle_ns = 1_000_000_000_000

        size = lib.arc_sizeof(cfg)
        if size == 0:
            fail(f"{entry['name']}: arc_sizeof rejected the configuration")
        storage = ffi.new("char[]", size)
        pacer = ffi.cast("arc_pacer_t *", storage)
        if lib.arc_init(pacer, cfg, ffi.NULL, ffi.NULL) != 0:
            fail(f"{entry['name']}: arc_init rejected the configuration")

        payload = ffi.new("uint8_t[64]")
        sample_at = drive_blocks - sample_blocks
        sample_index = 0
        for n in range(drive_blocks):
            delay = entry["d_high_ns"] if n % 2 == 0 else entry["d_low_ns"]
            producer_ts = time.monotonic_ns() - delay
            rc = lib.arc_submit(pacer, payload, 64, producer_ts)
            if rc != 0:
                fail(f"{entry['name']} block {n}: arc_submit returned {rc}")
            lib.arc_poll(pacer, MAX_ARC_POLL_NS)
            if n < sample_at:
                continue
            sample = samples[sample_index]
            sample_index += 1
            if sample["n"] != n:
                fail(f"{entry['name']}: sample order differs at block {n}")
            stats = lib.arc_stats(pacer)
            if not math.isfinite(stats.D):
                fail(f"{entry['name']} block {n}: D is not finite")
            if abs(stats.D - sample["D_ns"]) > entry["tolerance_ns"]:
                fail(
                    f"{entry['name']} block {n}: D={stats.D:.1f} "
                    f"expected {sample['D_ns']} (tol {entry['tolerance_ns']})"
                )
            comparisons += 1
        if sample_index != len(samples):
            fail(f"{entry['name']}: not every recorded sample was checked")

    return comparisons


def check_codec_group(root: dict[str, Any], group: str) -> int:
    codec_name = "nanorq" if group == "nanorq" else "lt"
    codec = lib.SDF_FOUNTAIN_NANORQ if group == "nanorq" else lib.SDF_FOUNTAIN_LT
    comparisons = 0

    for entry in root[group]["entries"]:
        if entry["codec"] != codec_name:
            fail(f"{group}: entry names codec {entry['codec']}")
        k = entry["k"]
        sym_len = entry["sym_len"]
        block_bytes = bytes(entry["block"])
        if len(block_bytes) != k * sym_len:
            fail(f"{group} K={k}/T={sym_len}: block length mismatch")
        symbols = entry["symbols"]

        block = ffi.new("uint8_t[]", block_bytes)
        enc_cfg = ffi.new("fe_cfg_t *")
        enc_cfg.block_len = len(block_bytes)
        enc_cfg.k = k
        enc_cfg.overhead_pct = entry["overhead_pct"]
        enc_cfg.codec = codec
        enc_size = lib.fe_sizeof(enc_cfg)
        if enc_size == 0:
            fail(f"{group} K={k}/T={sym_len}: fe_sizeof rejected the entry")
        enc_storage = ffi.new("char[]", enc_size)
        encoder = ffi.cast("fountain_enc_t *", enc_storage)
        if lib.fe_init_in(encoder, enc_cfg, block) != 0:
            fail(f"{group} K={k}/T={sym_len}: fe_init_in failed")

        for symbol in symbols:
            out = ffi.new("uint8_t[]", sym_len)
            rc = lib.fe_generate(encoder, symbol["esi"], out, sym_len)
            if rc != 0:
                fail(
                    f"{group} K={k}/T={sym_len}: fe_generate ESI {symbol['esi']} returned {rc}"
                )
            got = bytes(ffi.buffer(out, sym_len))
            expected = bytes(symbol["bytes"])
            if got != expected:
                fail(f"{group} K={k}/T={sym_len}: symbol ESI {symbol['esi']} differs")
            comparisons += 1

        dec_cfg = ffi.new("fd_cfg_t *")
        dec_cfg.k = k
        dec_cfg.sym_len = sym_len
        dec_size = lib.fd_sizeof(dec_cfg)
        if dec_size == 0:
            fail(f"{group} K={k}/T={sym_len}: fd_sizeof rejected the entry")
        dec_storage = ffi.new("char[]", dec_size)
        decoder = ffi.cast("fountain_dec_t *", dec_storage)
        if lib.fd_init_codec(decoder, dec_cfg, codec) != 0:
            fail(f"{group} K={k}/T={sym_len}: fd_init_codec failed")

        symbol_buf = ffi.new("uint8_t[]", sym_len)
        for symbol in symbols:
            ffi.memmove(symbol_buf, bytes(symbol["bytes"]), sym_len)
            rc = lib.fd_push(decoder, symbol["esi"], symbol_buf, sym_len)
            if rc not in (0, 1):
                fail(
                    f"{group} K={k}/T={sym_len}: fd_push ESI {symbol['esi']} returned {rc}"
                )
        if not lib.fd_ready(decoder):
            fail(f"{group} K={k}/T={sym_len}: decoder is not ready")
        if lib.fd_rank(decoder) < k:
            fail(f"{group} K={k}/T={sym_len}: rank below K")

        out = ffi.new("uint8_t[]", len(block_bytes))
        if lib.fd_decode(decoder, out, len(block_bytes)) != 0:
            fail(f"{group} K={k}/T={sym_len}: fd_decode failed")
        if bytes(ffi.buffer(out, len(block_bytes))) != block_bytes:
            fail(f"{group} K={k}/T={sym_len}: decoded block differs")
        comparisons += 1

        lib.fd_destroy(decoder)
        lib.fe_destroy(encoder)

    return comparisons


def toy_comparisons(root: dict[str, Any]) -> int:
    """Toy groups are C-parity-only: no shipped artifact implements them."""
    total = 0
    for group in ("roundtrip", "lossy_repair"):
        for entry in root[group]["entries"]:
            total += len(entry["symbols"]) + 1
    return total


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--golden-dir",
        type=Path,
        default=ROOT / "golden_vectors",
        help="directory holding wm_select.json, arc_schedule.json, fountain_esi.json",
    )
    args = parser.parse_args()

    wm = check_wm(args.golden_dir / "wm_select.json")
    arc = check_arc(args.golden_dir / "arc_schedule.json")
    fountain_root = load(args.golden_dir / "fountain_esi.json")
    nanorq = check_codec_group(fountain_root, "nanorq")
    lt = check_codec_group(fountain_root, "lt")
    toy = toy_comparisons(fountain_root)
    real_fountain = nanorq + lt
    applicable = wm + arc + real_fountain

    if applicable == 0:
        fail("no applicable comparisons ran")
    print(
        "installed CFFI goldens: ok "
        f"(WM={wm}, ARC={arc}, real-codec fountain={real_fountain}; "
        f"{applicable} applicable comparisons; "
        f"toy-only={toy} not applicable to shipped artifacts)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
