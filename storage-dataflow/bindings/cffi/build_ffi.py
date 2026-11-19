"""Compile the C sources into a CFFI extension for the control plane.

The generated extension is written to build/cffi as ``_sdf_cffi`` (or to
``--out``). The hand-written wrapper in this directory, ``sdf_cffi.py``,
adds that path and re-exports ``ffi`` and ``lib``.

W6.4 extends the cdef chain from Origin + WM to the full control
surface: ARC (pacer + sidecar), FLUID (budget/gate), and the fountain
codec ABI. The order follows the include graph — WM's types reference
``sdf_origin_stats_t``, and the FLUID repair surface references WM and
fountain types — so the cdefs must be fed in that order.

Two link surfaces are supported:

* default — the in-tree build (``build/lib*.a`` plus the vendored
  ``third_party/nanorq/libnanorq.a``);
* ``--prefix DIR`` — an installed prefix (the W5.2 install leg's
  surface: ``DIR/include`` and ``DIR/lib*``), which is the acceptance
  check that the bindings work against shipped artifacts, not the build
  tree.

All archives are static and PIC (CMakeLists sets
CMAKE_POSITION_INDEPENDENT_CODE; the vendored nanorq Makefile builds
with -fPIC) because the extension itself is a shared object.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

from cffi import FFI

ROOT = Path(__file__).resolve().parents[2]

# The cdef chain, in include-graph order (W2.4's lesson, extended in
# W6.4). Each header is self-contained after its deps are declared.
HEADERS = [
    "sdf/origin.h",
    "sdf/wm.h",
    "sdf/arc.h",
    "sdf/fluid.h",
    "sdf/fountain.h",
]

# Static link order: consumers before providers. sdf_fluid uses WM,
# fountain and the loopback origin; fountain uses nanorq; everything
# uses sdf_common.
LIBRARIES = [
    "sdf_arc",
    "sdf_fluid",
    "sdf_fountain",
    "sdf_wm",
    "sdf_origin_loopback",
    "sdf_origin",
    "sdf_common",
    "nanorq",
]


def cdef_from_header(path: Path) -> str:
    """Return a CFFI-compatible declaration block from a C header.

    CFFI's cdef understands plain-numeric object-like defines but not
    guards, includes or expression macros; anything else in a
    preprocessor line is dropped. Declaration comments are kept
    (pycparser handles them).
    """
    lines: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith("#"):
            no_comment = re.sub(r"/\*.*?\*/", "", stripped).strip()
            m = re.fullmatch(r"#define\s+(\w+)\s+(\d+)", no_comment)
            if m:
                lines.append(f"#define {m.group(1)} {m.group(2)}")
            continue
        if stripped == 'extern "C" {':
            continue
        if stripped == "}":
            continue
        lines.append(line)
    return "\n".join(lines)


def _surface(prefix: Path | None) -> tuple[Path, list[Path], Path]:
    """Return (include_dir, library_dirs, output_dir) for a surface."""
    if prefix is None:
        lib_dirs = [ROOT / "build", ROOT / "third_party" / "nanorq"]
        return ROOT / "include", lib_dirs, ROOT / "build" / "cffi"
    lib_dirs = [d for d in (prefix / "lib", prefix / "lib64") if d.is_dir()]
    if not lib_dirs:
        lib_dirs = [prefix / "lib"]
    return prefix / "include", lib_dirs, ROOT / "build" / "cffi-installed"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--prefix",
        type=Path,
        default=None,
        help="build against an installed prefix (DIR/include, DIR/lib*)",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="output directory (default: build/cffi or build/cffi-installed)",
    )
    args = parser.parse_args(argv)

    include_dir, library_dirs, out_dir = _surface(args.prefix)
    if args.out is not None:
        out_dir = args.out
    if not include_dir.is_dir():
        parser.error(f"include directory not found: {include_dir}")

    ffi = FFI()
    for header in HEADERS:
        ffi.cdef(cdef_from_header(include_dir / header))

    includes = "\n".join(f'#include "{h}"' for h in HEADERS)
    ffi.set_source(
        "_sdf_cffi",
        includes,
        include_dirs=[str(include_dir)],
        library_dirs=[str(d) for d in library_dirs],
        libraries=LIBRARIES,
        extra_compile_args=["-std=c11"],
    )
    out_dir.mkdir(parents=True, exist_ok=True)
    ffi.compile(tmpdir=str(out_dir), verbose=False)
    surface = "installed prefix" if args.prefix is not None else "build tree"
    print(f"built _sdf_cffi in {out_dir} ({surface})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
