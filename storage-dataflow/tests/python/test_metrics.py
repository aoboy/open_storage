"""W6.14 — bounded Python export and control-loop liveness."""

from __future__ import annotations

import pytest
from prometheus_client import generate_latest

from sdf.common.metrics import LivenessMonitor, SdfMetrics


def scrape(metrics: SdfMetrics) -> str:
    return generate_latest(metrics.registry).decode()


def test_registration_export_and_bounded_labels():
    metrics = SdfMetrics()
    metrics.record_wm_step(
        version=7, units=[256, 256, 256, 256], down_mask=2, fresh_paths=3
    )
    metrics.record_arc_step(mode="live", shadow_elapsed_s=12.5, interval_s=0.5)

    body = scrape(metrics)
    assert metrics.export() == body.encode()
    assert "sdf_wm_control_steps_total 1.0" in body
    assert "sdf_wm_control_version 7.0" in body
    assert 'sdf_wm_control_units{path="3"} 256.0' in body
    assert "sdf_wm_control_fresh_paths 3.0" in body
    assert 'sdf_arc_deploy_mode{mode="live"} 1.0' in body
    assert 'sdf_arc_deploy_mode{mode="shadow"} 0.0' in body
    assert "sdf_arc_shadow_elapsed_seconds 12.5" in body
    assert 'sdf_control_loop_missed_intervals{loop="wm"} 0.0' in body
    assert 'sdf_control_loop_missed_intervals{loop="arc"} 0.0' in body

    # Private registries make repeated controller/test construction safe.
    other = SdfMetrics()
    other.record_arc_mode("killed")
    assert 'sdf_arc_deploy_mode{mode="killed"} 1.0' in scrape(other)
    assert "sdf_arc_deploy_steps_total 0.0" in scrape(other)

    with pytest.raises(ValueError):
        metrics.record_wm_step(version=1, units=[], down_mask=0, fresh_paths=0)
    with pytest.raises(ValueError):
        metrics.record_wm_step(version=1, units=[1] * 65, down_mask=0, fresh_paths=0)
    with pytest.raises(ValueError):
        metrics.record_wm_step(version=1, units=[-1], down_mask=0, fresh_paths=0)
    with pytest.raises(ValueError):
        metrics.record_arc_mode("bogus")
    with pytest.raises(ValueError):
        metrics.record_arc_step(mode="live", shadow_elapsed_s=float("inf"))


def test_liveness_warning_and_page_boundaries():
    monitor = LivenessMonitor()
    start = 1_000_000_000
    interval = 0.25

    assert monitor.sample("wm", interval_s=interval, now_ns=start) == 0
    monitor.beat("wm", interval_s=interval, now_ns=start)
    assert monitor.sample("wm", now_ns=start + 249_999_999) == 0
    assert monitor.is_healthy("wm", now_ns=start + 250_000_000)
    assert monitor.sample("wm", now_ns=start + 250_000_000) == 1

    # Two missed intervals is the warning boundary, not still healthy.
    assert monitor.sample("wm", now_ns=start + 500_000_000) == 2
    assert not monitor.is_healthy("wm", now_ns=start + 500_000_000)
    assert monitor.is_live("wm", now_ns=start + 500_000_000)

    assert monitor.sample("wm", now_ns=start + 2_499_999_999) == 9
    assert monitor.is_live("wm", now_ns=start + 2_499_999_999)
    assert monitor.sample("wm", now_ns=start + 2_500_000_000) == 10
    assert not monitor.is_live("wm", now_ns=start + 2_500_000_000)

    monitor.beat("wm", now_ns=start + 3_000_000_000)
    assert monitor.sample("wm", now_ns=start + 3_000_000_000) == 0
    with pytest.raises(ValueError):
        monitor.sample("bogus", now_ns=start)
    with pytest.raises(ValueError):
        monitor.beat("wm", interval_s=0)


def test_liveness_is_exported_at_current_sample():
    metrics = SdfMetrics()
    start = 5_000_000_000
    metrics.liveness.beat("fluid", interval_s=1.0, now_ns=start)
    assert metrics.sample_liveness("fluid", now_ns=start + 2_000_000_000) == 2
    assert 'sdf_control_loop_missed_intervals{loop="fluid"} 2.0' in scrape(metrics)
