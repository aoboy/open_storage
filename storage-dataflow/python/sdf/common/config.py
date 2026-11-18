"""Configuration loading for the control plane."""

from __future__ import annotations

from pathlib import Path
from typing import Any

try:
    import yaml
except ModuleNotFoundError:  # pragma: no cover - exercised only without the extra
    yaml = None


def load_yaml(path: str | Path) -> dict[str, Any]:
    """Load a YAML document, returning an empty mapping for an empty file."""
    if yaml is None:
        raise ModuleNotFoundError("pyyaml is required to load control-plane configuration")

    with Path(path).open(encoding="utf-8") as handle:
        data = yaml.safe_load(handle)
    if data is None:
        return {}
    if not isinstance(data, dict):
        raise ValueError(f"{path}: expected a mapping at the top level")
    return data
