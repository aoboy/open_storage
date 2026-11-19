"""W6.14 — local Grafana-query and Prometheus scrape validation.

These tests validate JSON structure and metric references against C-generated
scrapes plus the declared external engine fixture. They do not import into a
hosted Grafana instance and make no hosted-evidence claim.
"""

from __future__ import annotations

import json
import math
import os
import re
from pathlib import Path

import pytest
from prometheus_client.parser import text_string_to_metric_families

from sdf.common.alerts import ALERT_SPECS
from sdf.common.metrics import SdfMetrics

ROOT = Path(__file__).resolve().parents[2]
DASHBOARDS = ROOT / "docs/ops/dashboards"
EXTERNAL_FIXTURE = ROOT / "docs/ops/fixtures/engine.prom"
LOCAL_FIXTURES = Path(
    os.environ.get("SDF_METRICS_FIXTURE_DIR", ROOT / "build/metrics-fixtures")
)

# PromQL functions/keywords containing underscores. Any other non-SDF token
# containing an underscore is treated as a metric reference and must resolve.
_PROMQL_WORDS = {
    "histogram_quantile",
    "median_over_time",
    "max_over_time",
    "min_over_time",
    "avg_over_time",
    "sum_over_time",
    "group_left",
    "group_right",
}
_IDENTIFIER = re.compile(r"(?<![a-zA-Z0-9_:])[a-zA-Z_:][a-zA-Z0-9_:]*")
_STRING = re.compile(r'"(?:\\.|[^"\\])*"')


def metric_references(expr: str) -> set[str]:
    references: set[str] = set()
    for match in _IDENTIFIER.finditer(_STRING.sub('""', expr)):
        token = match.group(0)
        if token in _PROMQL_WORDS:
            continue
        if token.startswith(("sdf_", "engine_")) or "_" in token:
            references.add(token)
    assert references, f"no metric references found in {expr}"
    return references


def scrape_samples(paths: list[Path]):
    samples = {}
    for path in paths:
        text = path.read_text(encoding="utf-8")
        families = list(text_string_to_metric_families(text))
        assert families, f"no Prometheus families in {path}"
        for family in families:
            for sample in family.samples:
                samples.setdefault(sample.name, []).append(sample)
    return samples


def local_scrape(component: str):
    path = LOCAL_FIXTURES / f"{component}.prom"
    if not path.exists():
        pytest.skip(
            f"{path} absent; run scripts/run_tests.sh to generate live C scrapes"
        )
    return scrape_samples([path])


@pytest.mark.parametrize("component", ["wm", "fluid", "arc"])
def test_dashboard_json_and_queries_resolve(component):
    document = json.loads(
        (DASHBOARDS / f"{component}.json").read_text(encoding="utf-8")
    )
    assert document["uid"] == f"sdf-{component}"
    assert document["schemaVersion"] >= 39
    assert document["panels"]
    panel_ids = [panel["id"] for panel in document["panels"]]
    assert len(panel_ids) == len(set(panel_ids))

    paths = [LOCAL_FIXTURES / f"{component}.prom"]
    if not paths[0].exists():
        pytest.skip(
            f"{paths[0]} absent; run scripts/run_tests.sh to generate live C scrapes"
        )
    if component == "arc":
        paths.append(EXTERNAL_FIXTURE)
    samples = scrape_samples(paths)

    for panel in document["panels"]:
        assert panel["title"]
        assert panel["targets"]
        for target in panel["targets"]:
            expr = target["expr"]
            assert expr.count("(") == expr.count(")"), expr
            missing = metric_references(expr) - samples.keys()
            assert not missing, (
                f"{component} panel {panel['id']}: unresolved {sorted(missing)}"
            )


def test_external_engine_fixture_is_explicit_and_bounded():
    samples = scrape_samples([EXTERNAL_FIXTURE])
    assert set(samples) == {
        "engine_apply_p99_seconds",
        "engine_backpressure_events_total",
    }
    for name, family_samples in samples.items():
        assert {sample.labels["mode"] for sample in family_samples} == {"bypass", "arc"}
        assert all(sample.labels["pipeline"] == "default" for sample in family_samples)
        assert all(
            set(sample.labels) == {"pipeline", "mode"} for sample in family_samples
        )


@pytest.mark.parametrize("component", ["wm", "fluid", "arc"])
def test_live_scrape_histograms_are_cumulative_and_exact(component):
    path = LOCAL_FIXTURES / f"{component}.prom"
    if not path.exists():
        pytest.skip(
            f"{path} absent; run scripts/run_tests.sh to generate live C scrapes"
        )
    families = list(text_string_to_metric_families(path.read_text(encoding="utf-8")))
    histograms = [family for family in families if family.type == "histogram"]
    assert histograms

    for family in histograms:
        grouped: dict[frozenset, list] = {}
        for sample in family.samples:
            key = frozenset((k, v) for k, v in sample.labels.items() if k != "le")
            grouped.setdefault(key, []).append(sample)
        for samples in grouped.values():
            buckets = [sample for sample in samples if sample.name.endswith("_bucket")]
            counts = [sample for sample in samples if sample.name.endswith("_count")]
            sums = [sample for sample in samples if sample.name.endswith("_sum")]
            assert len(counts) == len(sums) == 1
            assert len(buckets) == 32
            ordered = sorted(
                buckets,
                key=lambda sample: (
                    math.inf
                    if sample.labels["le"] == "+Inf"
                    else int(sample.labels["le"])
                ),
            )
            values = [sample.value for sample in ordered]
            assert values == sorted(values)
            assert ordered[-1].labels["le"] == "+Inf"
            assert ordered[-1].value == counts[0].value
            assert sums[0].value >= 0


@pytest.mark.parametrize("component", ["wm", "fluid", "arc"])
def test_live_scrape_labels_remain_bounded(component):
    samples = local_scrape(component)
    allowed = {
        "arm": {"control", "treatment", "lt", "nanorq"},
        "class": {"default"},
        "reason": {"ring_full", "invalid_input", "engine"},
        "kind": {"sent", "useful"},
        "codec": {"arq", "lt", "nanorq"},
    }
    for family_samples in samples.values():
        for sample in family_samples:
            for label, value in sample.labels.items():
                if label in {"path", "origin"}:
                    assert value.isdigit() and int(value) < 64
                elif label in allowed:
                    assert value in allowed[label]
                else:
                    assert label == "le"


def test_alert_queries_resolve_against_local_and_external_contracts():
    paths = [
        LOCAL_FIXTURES / f"{component}.prom" for component in ("wm", "fluid", "arc")
    ]
    missing_fixtures = [str(path) for path in paths if not path.exists()]
    if missing_fixtures:
        pytest.skip(
            f"live C scrapes absent: {missing_fixtures}; run scripts/run_tests.sh"
        )
    samples = scrape_samples(paths)
    python_metrics = SdfMetrics()
    python_metrics.record_wm_step(version=1, units=[1024], down_mask=0, fresh_paths=1)
    python_metrics.record_arc_step(mode="shadow", shadow_elapsed_s=0)
    for family in text_string_to_metric_families(python_metrics.export().decode()):
        for sample in family.samples:
            samples.setdefault(sample.name, []).append(sample)
    for spec in ALERT_SPECS:
        assert not (metric_references(spec.expr) - samples.keys()), spec.name
