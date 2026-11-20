"""Compatibility loader for the generated CFFI extension.

Development runs use ``SDF_CFFI_DIR`` or the in-tree ``build/cffi``
extension. An installed wheel uses the extension bundled inside the
``sdf`` package. Both paths are implemented by ``sdf._cffi``; this
module keeps the historical ``import sdf_cffi`` surface for the control
plane and the Python tests.
"""

from __future__ import annotations

from sdf._cffi import ffi, lib

__all__ = ["ffi", "lib"]
