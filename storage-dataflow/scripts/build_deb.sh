#!/usr/bin/env bash
# Plan prompt W6.6 — build a real .deb from the W5.2 install surface.
#
# No debhelper: the payload is headers + static archives + pkg-config +
# the CMake package config, so a staged DESTDIR install plus dpkg-deb is
# the honest, dependency-free path. Version comes from
# include/sdf/version.h (the single source of truth); the architecture
# from dpkg. Usage: scripts/build_deb.sh [OUT_DIR] (default build/deb).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

OUT="${1:-$ROOT/build/deb}"
BUILD="$OUT/build"
STAGE="$OUT/stage"
rm -rf "$BUILD" "$STAGE"
mkdir -p "$OUT"

VERSION="$(sed -n 's/#define SDF_VERSION_STRING "\(.*\)"/\1/p' include/sdf/version.h)"
ARCH="$(dpkg --print-architecture)"
if [[ -z "$VERSION" || -z "$ARCH" ]]; then
    echo "build_deb: could not read version/architecture" >&2
    exit 1
fi

# /usr (not /usr/local): this is a system package, and the .pc files
# bake the configure-time prefix.
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DSDF_USE_RUST_FOUNTAIN="${SDF_USE_RUST_FOUNTAIN:-OFF}" >/dev/null
cmake --build "$BUILD" --parallel >/dev/null
DESTDIR="$STAGE" cmake --install "$BUILD" >/dev/null

mkdir -p "$STAGE/DEBIAN"
sed -e "s/@SDF_VERSION@/$VERSION/g" -e "s/@DEB_ARCH@/$ARCH/g" \
    packaging/debian/control >"$STAGE/DEBIAN/control"

DEB="$OUT/libsdf-dev_${VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group --build "$STAGE" "$DEB"
echo "$DEB"
