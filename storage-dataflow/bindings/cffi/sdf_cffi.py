"""Loader for the generated CFFI extension.

Run ``scripts/build.sh`` to generate ``build/cffi/_sdf_cffi.*``. This wrapper
adds that directory to ``sys.path`` and re-exports ``ffi`` and ``lib`` so the
control plane can simply ``import sdf_cffi``.
"""

from __future__ import annotations

import sys
from pathlib import Path

_BUILD_DIR = Path(__file__).resolve().parents[2] / "build" / "cffi"
if _BUILD_DIR.is_dir() and str(_BUILD_DIR) not in sys.path:
    sys.path.insert(0, str(_BUILD_DIR))

try:
    from _sdf_cffi import ffi, lib  # type: ignore[import-not-found]
except ImportError as exc:  # pragma: no cover - depends on build state
    raise ImportError(
        f"CFFI extension _sdf_cffi could not be loaded ({exc}); run ./scripts/build.sh first"
    ) from exc

__all__ = ["ffi", "lib"]
