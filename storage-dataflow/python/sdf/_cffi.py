"""CFFI loader with explicit development overrides and a bundled fallback.

Resolution order:

1. ``SDF_CFFI_DIR`` when set — the development and installed-prefix
   override used by ``scripts/test_install.sh``.
2. ``build/cffi`` when running from a source checkout.
3. the wheel-bundled ``sdf/_sdf_cffi*.so`` extension.
4. an importable top-level ``_sdf_cffi`` already on ``sys.path``.

The bundled arm comes before the generic import so a wheel can never
silently bind to a different CFFI extension installed elsewhere in the
same interpreter. ``bindings/cffi/sdf_cffi.py`` re-exports this module.
"""

from __future__ import annotations

import importlib.util
import os
import sys
from pathlib import Path
from types import ModuleType
from typing import Any


def _prepend(path: Path) -> None:
    entry = str(path)
    if entry in sys.path:
        sys.path.remove(entry)
    sys.path.insert(0, entry)


def _bundled() -> Path | None:
    here = Path(__file__).resolve().parent
    for pattern in ("_sdf_cffi*.so", "_sdf_cffi*.pyd", "_sdf_cffi*.dylib"):
        matches = sorted(here.glob(pattern))
        if matches:
            return matches[0]
    return None


def _load_bundled(path: Path) -> ModuleType:
    spec = importlib.util.spec_from_file_location("_sdf_cffi", path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load bundled CFFI extension {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    sys.modules["_sdf_cffi"] = module
    return module


def _load() -> tuple[Any, Any]:
    override = os.environ.get("SDF_CFFI_DIR")
    if override:
        _prepend(Path(override))
        from _sdf_cffi import ffi, lib

        return ffi, lib

    build_dir = Path(__file__).resolve().parents[2] / "build" / "cffi"
    if build_dir.is_dir():
        _prepend(build_dir)
        from _sdf_cffi import ffi, lib

        return ffi, lib

    bundled = _bundled()
    if bundled is not None:
        module = _load_bundled(bundled)
        return module.ffi, module.lib

    try:
        from _sdf_cffi import ffi, lib
    except ImportError as exc:  # pragma: no cover - depends on build state
        raise ImportError(
            "CFFI extension _sdf_cffi was not found; run ./scripts/build.sh, "
            "set SDF_CFFI_DIR, or install the sdf wheel"
        ) from exc
    return ffi, lib


ffi, lib = _load()

__all__ = ["ffi", "lib"]
