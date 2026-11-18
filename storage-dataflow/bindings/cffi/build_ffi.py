"""Compile the C sources into a CFFI extension for the control plane.

The generated extension is written to build/cffi as ``_sdf_cffi``. The
hand-written wrapper in this directory, ``sdf_cffi.py``, adds that path and
re-exports ``ffi`` and ``lib``.
"""

from __future__ import annotations

import re
from pathlib import Path

from cffi import FFI

ROOT = Path(__file__).resolve().parents[2]
INCLUDE_DIR = ROOT / "include"
HEADER = INCLUDE_DIR / "sdf" / "wm.h"
OUT_DIR = ROOT / "build" / "cffi"


def cdef_from_header(path: Path) -> str:
    """Return a CFFI-compatible declaration block from a C header."""
    lines: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith("#"):
            # CFFI understands numeric object-like defines, not guards/includes.
            if re.fullmatch(r"#define\s+\w+\s+\d+", stripped):
                lines.append(line)
            continue
        if stripped == 'extern "C" {':
            continue
        if stripped == "}":
            continue
        lines.append(line)
    return "\n".join(lines)


def main() -> int:
    ffi = FFI()
    ffi.cdef(cdef_from_header(HEADER))
    ffi.set_source(
        "_sdf_cffi",
        '#include "sdf/wm.h"',
        sources=[
            str(ROOT / "src/c/common/ring.c"),
            str(ROOT / "src/c/common/pool.c"),
            str(ROOT / "src/c/common/seqlock.c"),
            str(ROOT / "src/c/common/timer.c"),
            str(ROOT / "src/c/common/shm.c"),
            str(ROOT / "src/c/common/metrics.c"),
            str(ROOT / "src/c/common/log.c"),
            str(ROOT / "src/c/common/config.c"),
            str(ROOT / "src/c/whack_a_mole/wm_profile.c"),
            str(ROOT / "src/c/whack_a_mole/wm_select.c"),
            str(ROOT / "src/c/whack_a_mole/wm_shed.c"),
            str(ROOT / "src/c/whack_a_mole/wm_shm.c"),
        ],
        include_dirs=[str(INCLUDE_DIR)],
    )
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    ffi.compile(tmpdir=str(OUT_DIR), verbose=False)
    print(f"built _sdf_cffi in {OUT_DIR}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
