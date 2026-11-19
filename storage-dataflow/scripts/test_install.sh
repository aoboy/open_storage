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
cmake -S . -B "$DEST/build" -DCMAKE_BUILD_TYPE=Release     -DCMAKE_INSTALL_PREFIX="$DEST/usr" >/dev/null
cmake --build "$DEST/build" --parallel >/dev/null
cmake --install "$DEST/build" >/dev/null

# 1. pkg-config surface
export PKG_CONFIG_PATH="$DEST/usr/lib/pkgconfig"
FLAGS="$(pkg-config --cflags --libs sdf_wm sdf_arc)"
cc tools/install_hello.c $FLAGS -o "$DEST/install_hello"
"$DEST/install_hello"

# 2. find_package(sdf) consumer
CONS="$(mktemp -d /tmp/sdf-consumer-XXXX)"
trap 'rm -rf "$DEST" "$CONS"' EXIT
cp tools/install_hello.c "$CONS/main.c"
cat > "$CONS/CMakeLists.txt" <<'CM'
cmake_minimum_required(VERSION 3.24)
project(sdf_consumer LANGUAGES C)
find_package(sdf 0.2 REQUIRED CONFIG)
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE sdf::sdf_wm sdf::sdf_arc)
CM
cmake -S "$CONS" -B "$CONS/build" -DCMAKE_PREFIX_PATH="$DEST/usr/lib/cmake/sdf" >/dev/null
cmake --build "$CONS/build" >/dev/null
"$CONS/build/consumer"

echo "test_install: OK (pkg-config + find_package legs)"
