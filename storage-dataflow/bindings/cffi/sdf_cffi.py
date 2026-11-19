"""Loader for the generated CFFI extension.

Run ``scripts/build.sh`` to generate ``build/cffi/_sdf_cffi.*``. This
wrapper adds that directory to ``sys.path`` and re-exports ``ffi`` and
``lib`` so the control plane can simply ``import sdf_cffi``.

W6.4: ``SDF_CFFI_DIR`` overrides the search path. The install leg and
CI use it to load an extension built against an installed prefix
(``build_ffi.py --prefix``) instead of the in-tree build.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

_OVERRIDE = os.environ.get("SDF_CFFI_DIR")
_build_dir = Path(_OVERRIDE) if _OVERRIDE else (
    Path(__file__).resolve().parents[2] / "build" / "cffi"
)
if _build_dir.is_dir():
    _path = str(_build_dir)
    if _path in sys.path:
        sys.path.remove(_path)
    sys.path.insert(0, _path)

try:
    from _sdf_cffi import ffi, lib  # type: ignore[import-not-found]
except ImportError as exc:  # pragma: no cover - depends on build state
    where = _OVERRIDE or "build/cffi"
    raise ImportError(
        f"CFFI extension _sdf_cffi could not be loaded from {where} ({exc}); "
        "run ./scripts/build.sh first"
    ) from exc

__all__ = ["ffi", "lib"]
