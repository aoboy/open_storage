#!/usr/bin/env bash
# Plan prompt W6.6 — the package leg: build the .deb, dpkg-install it
# into an isolated rootfs, then compile the W5.2 hello-world against the
# installed payload through pkg-config's sysroot support. Exit nonzero
# on any failure; wired into `ci` and the hosted install job.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

WORK="$(mktemp -d /tmp/sdf-deb-XXXX)"
trap 'rm -rf "$WORK"' EXIT

DEB="$("$ROOT/scripts/build_deb.sh" "$WORK/out" | tail -1)"
if [[ ! -f "$DEB" ]]; then
    echo "test_deb: build produced no .deb" >&2
    exit 1
fi

VERSION="$(sed -n 's/#define SDF_VERSION_STRING "\(.*\)"/\1/p' include/sdf/version.h)"
dpkg-deb --info "$DEB" | grep -F "Version: $VERSION" >/dev/null

ROOTFS="$WORK/rootfs"
mkdir -p "$ROOTFS/var/lib/dpkg"
dpkg --root="$ROOTFS" --force-not-root --log="$WORK/dpkg.log" \
    --install "$DEB" >/dev/null

# Payload checks (headers, static archives incl. vendored nanorq,
# pkg-config, the CMake package config).
test -f "$ROOTFS/usr/include/sdf/arc.h"
LIBDIR="$(dirname "$(find "$ROOTFS/usr/lib" -name 'libsdf_arc.a' -print -quit)")"
test -f "$LIBDIR/libnanorq.a"
test -f "$LIBDIR/pkgconfig/sdf_arc.pc"
test -f "$LIBDIR/cmake/sdf/sdf-config.cmake"

# Compile an external consumer against the extracted payload: sysroot
# prefixes the .pc's /usr paths onto the isolated rootfs.
export PKG_CONFIG_LIBDIR="$LIBDIR/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$ROOTFS"
FLAGS="$(pkg-config --cflags --libs sdf_wm sdf_arc)"
cc tools/install_hello.c $FLAGS -o "$WORK/deb_hello"
"$WORK/deb_hello"

# W6.8: the packaged fountain archive is self-contained for both codecs,
# including the Rust staticlib's native support libraries when selected.
FOUNTAIN_FLAGS="$(pkg-config --cflags --libs sdf_fountain)"
cc tests/c/unit/test_fe_codec.c $FOUNTAIN_FLAGS -o "$WORK/deb_fountain"
"$WORK/deb_fountain"

echo "test_deb: OK (rust-fountain=${SDF_USE_RUST_FOUNTAIN:-OFF}; $(basename "$DEB") built, dpkg-installed, consumers compiled)"
