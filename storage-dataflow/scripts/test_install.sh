#!/usr/bin/env bash
# Plan prompt W5.2 — the install leg: build, install to DESTDIR, compile
# the external hello-world against the installed pkg-config surface, and
# a find_package consumer via a fresh CMake project. Exit nonzero on any
# failure. Not in ctest (it mutates an install tree); wired into `ci`.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DEST="$(mktemp -d /tmp/sdf-install-XXXX)"
trap 'rm -rf "$DEST"' EXIT

# The .pc files bake the configure-time prefix: stage a dedicated
# install build so the recorded paths point INTO the staging tree
# (the pkg-config-correct way to test an install surface).
cmake -S . -B "$DEST/build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$DEST/usr" \
    -DSDF_USE_RUST_FOUNTAIN="${SDF_USE_RUST_FOUNTAIN:-OFF}" >/dev/null
cmake --build "$DEST/build" --parallel >/dev/null
cmake --install "$DEST/build" >/dev/null

# 1. pkg-config surface
export PKG_CONFIG_PATH="$DEST/usr/lib/pkgconfig"
FLAGS="$(pkg-config --cflags --libs sdf_wm sdf_arc)"
cc tools/install_hello.c $FLAGS -o "$DEST/install_hello"
"$DEST/install_hello"

# W6.8: exercise BOTH fountain arms through the unchanged public ABI. This
# catches a missing Rust native support library in the installed .pc surface.
FOUNTAIN_FLAGS="$(pkg-config --cflags --libs sdf_fountain)"
cc tests/c/unit/test_fe_codec.c $FOUNTAIN_FLAGS -o "$DEST/fountain_hello"
"$DEST/fountain_hello"

# 2. find_package(sdf) consumer
CONS="$(mktemp -d /tmp/sdf-consumer-XXXX)"
trap 'rm -rf "$DEST" "$CONS"' EXIT
cp tools/install_hello.c "$CONS/main.c"
cp tests/c/unit/test_fe_codec.c "$CONS/fountain.c"
cat > "$CONS/CMakeLists.txt" <<'CM'
cmake_minimum_required(VERSION 3.24)
project(sdf_consumer LANGUAGES C)
find_package(sdf 1.0 REQUIRED CONFIG)
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE sdf::sdf_wm sdf::sdf_arc)
add_executable(fountain_consumer fountain.c)
target_link_libraries(fountain_consumer PRIVATE sdf::sdf_fountain)
CM
cmake -S "$CONS" -B "$CONS/build" -DCMAKE_PREFIX_PATH="$DEST/usr/lib/cmake/sdf" >/dev/null
cmake --build "$CONS/build" >/dev/null
"$CONS/build/consumer"
"$CONS/build/fountain_consumer"

# 3. CFFI against the installed prefix (W6.4): the extension must link
# from $DEST/usr, not the in-tree build, and drive the real ARC sidecar.
if python3 -c 'import cffi' >/dev/null 2>&1; then
    CFFI_OUT="$DEST/cffi"
    python3 bindings/cffi/build_ffi.py --prefix "$DEST/usr" --out "$CFFI_OUT"
    SDF_CFFI_DIR="$CFFI_OUT" PYTHONPATH="$ROOT/python:$ROOT/bindings/cffi" \
        python3 - <<'PY'
from sdf_cffi import ffi, lib

cfg = ffi.new("arc_cfg_t *")
cfg.capacity = 64
cfg.entry_max = 248
assert lib.arc_sidecar_start(cfg, 1) == 0
assert lib.arc_sidecar_set_mode(2) == 0
assert lib.arc_sidecar_pump_once() >= 0
assert lib.arc_sidecar_mode() == 2
lib.arc_sidecar_stop()
assert lib.sdf_fluid_budget_n(64, 0.1) == 72
assert ffi.sizeof("fe_cfg_t") > 0
print("installed CFFI: OK (arc sidecar + fluid budget + fountain ABI)")
PY
else
    echo "test_install: cffi unavailable; installed CFFI leg skipped" >&2
fi

echo "test_install: OK (rust-fountain=${SDF_USE_RUST_FOUNTAIN:-OFF}; pkg-config + find_package + installed-CFFI, both codecs)"
