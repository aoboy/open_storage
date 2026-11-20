#!/usr/bin/env bash
# Plan prompt W6.16 — isolated wheel installation test.
#
# Builds the wheel against a staged installed prefix, inspects its
# contents and dynamic linking, then installs it into a fresh venv
# outside the checkout and exercises ARC plus both fountain codecs and
# the golden-vector consumer with PYTHONPATH and SDF_CFFI_DIR unset.
#
# The runner may be offline, so the venv inherits the base interpreter's
# dependency set (--system-site-packages) and the wheel is installed
# with --no-deps. This is recorded as an offline-runner limitation, not
# as a hosted dependency-resolution claim.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

VARIANT="${SDF_USE_RUST_FOUNTAIN:-OFF}"
while (($# > 0)); do
    case "$1" in
        --variant)
            VARIANT="${2:?--variant needs OFF or ON}"
            shift 2
            ;;
        --help|-h)
            sed -n '2,12p' "$0"
            exit 0
            ;;
        *)
            echo "test_wheel: unknown argument: $1" >&2
            exit 1
            ;;
    esac
done
case "$VARIANT" in
    OFF|ON) ;;
    *)
        echo "test_wheel: --variant must be OFF or ON (got $VARIANT)" >&2
        exit 1
        ;;
esac
if [[ "$VARIANT" == "ON" ]] && ! command -v cargo >/dev/null 2>&1; then
    echo "test_wheel: variant ON skipped (cargo unavailable)" >&2
    exit 0
fi

WORK="$(mktemp -d /tmp/sdf-wheel-test-XXXX)"
trap 'rm -rf "$WORK"' EXIT

WHEEL="$("$ROOT/scripts/build_wheel.sh" --out "$WORK/wheels" --variant "$VARIANT" | tail -n1)"
if [[ ! -f "$WHEEL" ]]; then
    echo "test_wheel: build produced no wheel" >&2
    exit 1
fi
BASENAME="$(basename "$WHEEL")"

# 1. Content: loader, package data, entry point, bundled extension.
python3 - "$WHEEL" <<'PY'
import sys
import zipfile

wheel = sys.argv[1]
with zipfile.ZipFile(wheel) as z:
    names = z.namelist()
    required = {
        "sdf_cffi.py",
        "sdf/__init__.py",
        "sdf/_cffi.py",
        "sdf/py.typed",
        "sdf/cli/main.py",
    }
    missing = required.difference(names)
    assert not missing, f"wheel missing {sorted(missing)}"
    assert any(n.startswith("sdf/_sdf_cffi") and n.endswith(".so") for n in names), (
        f"wheel has no bundled extension: {names}"
    )
    assert not any(n.startswith(("build/", "third_party/")) for n in names)
    metadata = next(n for n in names if n.endswith(".dist-info/METADATA"))
    meta = z.read(metadata).decode()
    assert "Requires-Dist: cffi" in meta
    assert "Name: sdf" in meta
    entry = next(n for n in names if n.endswith(".dist-info/entry_points.txt"))
    assert "sdf = sdf.cli.main:main" in z.read(entry).decode()
print(f"wheel content: OK ({len(names)} entries)")
PY

# Platform-specific tag, not a pure-Python wheel.
if [[ "$BASENAME" != *cp3*-*linux*.whl ]]; then
    echo "test_wheel: unexpected wheel tag: $BASENAME" >&2
    exit 1
fi

# 2. Dynamic dependency inspection: no RPATH/RUNPATH and no path-like
# NEEDED entries pointing back at the checkout or staging prefix.
python3 -m zipfile -e "$WHEEL" "$WORK/extract"
SO="$(find "$WORK/extract/sdf" -maxdepth 1 -name '_sdf_cffi*.so' -print -quit)"
if [[ -z "$SO" ]]; then
    echo "test_wheel: extracted wheel has no extension" >&2
    exit 1
fi
if ! command -v readelf >/dev/null 2>&1; then
    echo "test_wheel: readelf is required for the linkage inspection" >&2
    exit 1
fi
readelf -d "$SO" > "$WORK/readelf.txt"
if grep -Eq 'RPATH|RUNPATH' "$WORK/readelf.txt"; then
    echo "test_wheel: bundled extension carries an RPATH/RUNPATH" >&2
    exit 1
fi
if grep -E '\(NEEDED\)' "$WORK/readelf.txt" | grep -Eq '/home/|/tmp/|/build|third_party'; then
    echo "test_wheel: bundled extension has a path-like NEEDED entry" >&2
    exit 1
fi

# 3. Fresh venv outside the checkout. Dependencies come from the base
# interpreter on this offline runner; the wheel itself is installed
# without resolving them.
python3 -m venv --system-site-packages "$WORK/venv"
env -u PYTHONPATH -u SDF_CFFI_DIR "$WORK/venv/bin/python" -m pip install \
    --disable-pip-version-check --no-deps "$WHEEL" >/dev/null

VERSION_OUT="$(env -u PYTHONPATH -u SDF_CFFI_DIR "$WORK/venv/bin/sdf" --version)"
if [[ "$VERSION_OUT" != "sdf 1.0.0" ]]; then
    echo "test_wheel: console entry point returned: $VERSION_OUT" >&2
    exit 1
fi

cd "$WORK"
env -u PYTHONPATH -u SDF_CFFI_DIR "$WORK/venv/bin/python" - <<'PY'
import os
import sys
from pathlib import Path

assert "PYTHONPATH" not in os.environ
assert "SDF_CFFI_DIR" not in os.environ

import sdf
import sdf_cffi

# The bundled extension is registered under its top-level module name by
# the loader; import it only after ``sdf_cffi`` resolved it.
import _sdf_cffi

prefix = Path(sys.prefix).resolve()
for module in (sdf, sdf_cffi, _sdf_cffi):
    path = Path(module.__file__).resolve()
    assert path.is_relative_to(prefix), f"{path} is outside the venv {prefix}"

from sdf_cffi import ffi, lib

arc = ffi.new("arc_cfg_t *")
arc.capacity = 64
arc.entry_max = 248
assert lib.arc_sidecar_start(arc, 1) == 0
try:
    assert lib.arc_sidecar_set_mode(2) == 0
    assert lib.arc_sidecar_pump_once() >= 0
    assert lib.arc_sidecar_mode() == 2
    shadow = ffi.new("arc_shadow_stats_t *")
    assert lib.arc_sidecar_shadow_stats(shadow) == 0
finally:
    lib.arc_sidecar_stop()


def roundtrip(codec: int) -> None:
    k, sym_len = 8, 64
    block_len = k * sym_len
    block = bytes(((i * 7 + 3) & 0xFF) for i in range(block_len))
    block_buf = ffi.new("uint8_t[]", block)

    enc_cfg = ffi.new("fe_cfg_t *")
    enc_cfg.block_len = block_len
    enc_cfg.k = k
    enc_cfg.overhead_pct = 20
    enc_cfg.codec = codec
    enc_size = lib.fe_sizeof(enc_cfg)
    assert enc_size > 0
    # Keep the storage cdata alive: a cast pointer does not own it.
    enc_buf = ffi.new(f"uint8_t[{enc_size}]")
    enc = ffi.cast("fountain_enc_t *", enc_buf)
    assert lib.fe_init_in(enc, enc_cfg, block_buf) == 0
    try:
        dec_cfg = ffi.new("fd_cfg_t *")
        dec_cfg.k = k
        dec_cfg.sym_len = sym_len
        dec_size = lib.fd_sizeof(dec_cfg)
        assert dec_size > 0
        dec_buf = ffi.new(f"uint8_t[{dec_size}]")
        dec = ffi.cast("fountain_dec_t *", dec_buf)
        assert lib.fd_init_codec(dec, dec_cfg, codec) == 0
        try:
            count = lib.fe_symbol_count(enc)
            assert count >= k
            for esi in range(count):
                sym = ffi.new(f"uint8_t[{sym_len}]")
                assert lib.fe_generate(enc, esi, sym, sym_len) == 0
                assert lib.fd_push(dec, esi, sym, sym_len) in (0, 1)
            assert lib.fd_ready(dec)
            out = ffi.new(f"uint8_t[{block_len}]")
            assert lib.fd_decode(dec, out, block_len) == 0
            assert bytes(ffi.buffer(out, block_len)) == block
        finally:
            lib.fd_destroy(dec)
    finally:
        lib.fe_destroy(enc)


roundtrip(lib.SDF_FOUNTAIN_LT)
roundtrip(lib.SDF_FOUNTAIN_NANORQ)
print("isolated wheel: OK (ARC sidecar + LT/nanorq fountain roundtrips)")
PY

# 3b. The same golden consumer used by the installed-prefix leg, run from
# outside the checkout so only the wheel's bundled extension can satisfy
# the import. Copy the script first: executing it from bindings/cffi would
# put that source directory on sys.path.
cp "$ROOT/bindings/cffi/check_goldens.py" "$WORK/check_goldens.py"
env -u PYTHONPATH -u SDF_CFFI_DIR "$WORK/venv/bin/python" \
    "$WORK/check_goldens.py" --golden-dir "$ROOT/golden_vectors"

# 4. Development overrides remain explicit: with the source tree and
# build/cffi supplied, the same interpreter binds the build extension.
OVERRIDE_SO="$(
    cd "$WORK"
    env -u SDF_CFFI_DIR PYTHONPATH="$ROOT/python:$ROOT/bindings/cffi" \
        "$WORK/venv/bin/python" -c 'import sdf_cffi, _sdf_cffi; print(_sdf_cffi.__file__)'
)"
if [[ "$OVERRIDE_SO" != "$ROOT/build/cffi/"* ]]; then
    echo "test_wheel: explicit development override bound: $OVERRIDE_SO" >&2
    exit 1
fi

echo "test_wheel: OK (variant=$VARIANT; $BASENAME; isolated venv ARC + LT/nanorq + goldens)"
