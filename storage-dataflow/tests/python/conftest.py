"""Make the in-repo Python package and CFFI loader importable."""

from __future__ import annotations

import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_ROOT / "python"))
# ``sdf_cffi`` is the loader for build/cffi/_sdf_cffi.*; tests that need
# the real bindings import it (and skip when the extension was not
# built). SDF_CFFI_DIR still overrides the extension search path.
sys.path.insert(0, str(_ROOT / "bindings" / "cffi"))
