"""W6.14 — every documented warning/page boundary and generated YAML."""

from __future__ import annotations

import math
from pathlib import Path

import pytest
import yaml

from sdf.common.alerts import ALERT_SPECS, prometheus_rules, render_rules


@pytest.mark.parametrize("spec", ALERT_SPECS, ids=lambda spec: spec.name)
@pytest.mark.parametrize("severity", ["warning", "page"])
def test_alert_threshold_boundaries(spec, severity):
    threshold = spec.threshold(severity)
    below = math.nextafter(threshold, -math.inf)
    above = math.nextafter(threshold, math.inf)

    assert not spec.fires(severity, below)
    if spec.operator == ">":
        assert not spec.fires(severity, threshold)
    else:
        assert spec.fires(severity, threshold)
    assert spec.fires(severity, above)


def test_arc_drop_warning_and_page_both_fire_on_any_increase():
    spec = next(spec for spec in ALERT_SPECS if spec.name == "ArcDroppedEntries")
    assert not spec.fires("warning", 0)
    assert not spec.fires("page", 0)
    assert spec.fires("warning", 1)
    assert spec.fires("page", 1)
    assert spec.warning_for == spec.page_for == "0m"


def test_checked_in_rules_match_executable_specs():
    path = Path(__file__).resolve().parents[2] / "docs/ops/alerts/sdf-alerts.yml"
    assert path.read_text(encoding="utf-8") == render_rules()

    document = yaml.safe_load(path.read_text(encoding="utf-8"))
    assert document == prometheus_rules()
    rules = document["groups"][0]["rules"]
    assert len(rules) == len(ALERT_SPECS) * 2
    assert {rule["labels"]["severity"] for rule in rules} == {"warning", "page"}
    assert all(rule["expr"] for rule in rules)


def test_documented_numeric_thresholds_are_unchanged():
    expected = {
        "WmFillP99Regression": (0.05, 0.10),
        "FluidRepairP99Regression": (0.0, 0.10),
        "FluidBandwidthOverhead": (1.20, 1.50),
        "FluidArqFallbackRate": (0.02, 0.05),
        "ArcAddedLatencyP99": (10_000_000, 25_000_000),
        "ArcDroppedEntries": (0, 0),
        "ControlLoopLiveness": (2, 10),
    }
    assert {spec.name: (spec.warning, spec.page) for spec in ALERT_SPECS} == expected
