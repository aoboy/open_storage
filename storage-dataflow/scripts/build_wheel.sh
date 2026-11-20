#!/usr/bin/env bash
# Plan prompt W6.16 — build the installable Python/CFFI wheel.
#
# The wheel bundles the Python control plane, the sdf_cffi loader and
# one _sdf_cffi extension linked against a named installed archive
# prefix. The prefix is staged from this checkout unless --prefix is
# supplied; no build-tree shared library is required at runtime.
#
# Usage:
#   ./scripts/build_wheel.sh [--out DIR] [--variant OFF|ON] [--prefix DIR]
#
# The last stdout line is the wheel path; progress goes to stderr.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

OUT="$ROOT/build/wheel"
VARIANT="${SDF_USE_RUST_FOUNTAIN:-OFF}"
PREFIX=""

while (($# > 0)); do
    case "$1" in
        --out)
            OUT="${2:?--out needs a directory}"
            shift 2
            ;;
        --variant)
            VARIANT="${2:?--variant needs OFF or ON}"
            shift 2
            ;;
        --prefix)
            PREFIX="${2:?--prefix needs an installed prefix}"
            shift 2
            ;;
        --help|-h)
            sed -n '2,14p' "$0"
            exit 0
            ;;
        *)
            echo "build_wheel: unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

case "$VARIANT" in
    OFF|ON) ;;
    *)
        echo "build_wheel: --variant must be OFF or ON (got $VARIANT)" >&2
        exit 1
        ;;
esac
if [[ "$VARIANT" == "ON" ]] && ! command -v cargo >/dev/null 2>&1; then
    echo "build_wheel: variant ON needs cargo, which is not installed" >&2
    exit 1
fi

for tool in python3 cmake; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "build_wheel: missing required tool: $tool" >&2
        exit 1
    fi
done
if ! python3 -m pip --version >/dev/null 2>&1; then
    echo "build_wheel: python3 pip is required" >&2
    exit 1
fi

WORK="$(mktemp -d /tmp/sdf-wheel-XXXX)"
trap 'rm -rf "$WORK"' EXIT

if [[ -z "$PREFIX" ]]; then
    PREFIX="$WORK/usr"
    echo "build_wheel: staging variant=$VARIANT into a temporary prefix" >&2
    cmake -S "$ROOT" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DSDF_USE_RUST_FOUNTAIN="$VARIANT" >&2
    cmake --build "$WORK/build" --parallel >&2
    cmake --install "$WORK/build" >&2
fi
if [[ ! -d "$PREFIX/include/sdf" ]]; then
    echo "build_wheel: $PREFIX is not an installed sdf prefix" >&2
    exit 1
fi

echo "build_wheel: building CFFI extension against $PREFIX" >&2
python3 "$ROOT/bindings/cffi/build_ffi.py" --prefix "$PREFIX" --out "$WORK/cffi" >&2
SO="$(find "$WORK/cffi" -maxdepth 1 -name '_sdf_cffi*.so' -print -quit)"
if [[ -z "$SO" ]]; then
    echo "build_wheel: CFFI build produced no extension" >&2
    exit 1
fi

# The wheel links the SDF archives statically, so a runtime search path is
# never needed. Some setuptools/pip combinations add one for library_dirs;
# strip it before packaging or the installed wheel would point outside itself.
if command -v readelf >/dev/null 2>&1 &&
    readelf -d "$SO" | grep -E 'RPATH|RUNPATH' >/dev/null; then
    if command -v patchelf >/dev/null 2>&1; then
        patchelf --remove-rpath "$SO"
    elif command -v chrpath >/dev/null 2>&1; then
        chrpath -d "$SO"
    else
        echo "build_wheel: bundled extension has RPATH/RUNPATH and neither patchelf nor chrpath is installed" >&2
        exit 1
    fi
    if readelf -d "$SO" | grep -E 'RPATH|RUNPATH' >/dev/null; then
        echo "build_wheel: failed to strip RPATH/RUNPATH from $SO" >&2
        exit 1
    fi
fi

VERSION="$(sed -n 's/#define SDF_VERSION_STRING "\(.*\)"/\1/p' include/sdf/version.h)"
if [[ -z "$VERSION" ]]; then
    echo "build_wheel: cannot read SDF_VERSION_STRING" >&2
    exit 1
fi

PROJ="$WORK/proj"
mkdir -p "$PROJ"
cp -R "$ROOT/python/sdf" "$PROJ/sdf"
find "$PROJ/sdf" -type d -name __pycache__ -prune -exec rm -rf {} +
cp "$ROOT/bindings/cffi/sdf_cffi.py" "$PROJ/sdf_cffi.py"
cp "$SO" "$PROJ/sdf/"
SO_NAME="$(basename "$SO")"

cat > "$PROJ/pyproject.toml" <<EOF
[build-system]
requires = ["setuptools>=68", "wheel"]
build-backend = "setuptools.build_meta"

[project]
name = "sdf"
version = "$VERSION"
description = "Storage dataflow stack control plane"
requires-python = ">=3.10"
dependencies = [
    "cffi",
    "prometheus-client",
    "pyyaml",
    "xxhash",
    "tomli; python_version < '3.11'",
]

[project.optional-dependencies]
dev = [
    "mypy",
    "pytest",
    "ruff",
]

[project.scripts]
sdf = "sdf.cli.main:main"

[tool.setuptools]
py-modules = ["sdf_cffi"]

[tool.setuptools.packages.find]
where = ["."]
include = ["sdf*"]

[tool.setuptools.package-data]
sdf = ["py.typed", "$SO_NAME"]
EOF

# A platform wheel, not py3-none-any: the bundled extension is built for
# this interpreter's ABI and architecture.
cat > "$PROJ/setup.py" <<'PY'
from setuptools import Distribution, setup


class BinaryDistribution(Distribution):
    def has_ext_modules(self) -> bool:
        return True


setup(distclass=BinaryDistribution)
PY

DIST="$WORK/dist"
mkdir -p "$DIST" "$OUT"
echo "build_wheel: building wheel (variant=$VARIANT)" >&2
python3 -m pip wheel --disable-pip-version-check --no-deps --no-build-isolation \
    --wheel-dir "$DIST" "$PROJ" >&2
WHEEL="$(find "$DIST" -maxdepth 1 -name 'sdf-*.whl' -print -quit)"
if [[ -z "$WHEEL" ]]; then
    echo "build_wheel: wheel build produced no artifact" >&2
    exit 1
fi
cp "$WHEEL" "$OUT/"
echo "$OUT/$(basename "$WHEEL")"
