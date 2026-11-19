# Runbook: ARC Kill Switch

**Component:** Prototype 3 — receiver-side pacing sidecar
**Owner:** Receiver/apply plane
**Last reviewed:** 2026-11-19

## Purpose

Bypass the ARC pacer within one control interval when it adds latency, drops
entries, reorders applies, or destabilises the storage engine.

## Triggers

- Added latency p99 above 25 ms.
- Engine apply p99 regression greater than 15%.
- Any increase in `arc_dropped_total`.
- Out-of-order apply or a correctness mismatch.
- Heap depth growing without bound or sustained starvation.
- Leadership change / engine backpressure storm correlated with ARC.

## Immediate kill switch

1. Set `arc.kill_switch: true` in the environment config (the sidecar reads it
   every control interval).
2. Confirm the sidecar bypasses to the direct apply path:
   - `arc_added_latency_ns` returns to zero or baseline.
   - `arc_heap_depth` drains.
   - `engine_apply_p99_seconds` returns to baseline.
3. Leave `shadow_mode: true` if the pipeline should keep collecting
   comparison metrics; set it to false only when fully disabling the sidecar.
4. Verify `arc_dropped_total` stops increasing and no entries remain queued
   past `batch_deadline_ns`.

## If the kill switch does not take effect

1. Remove the pipeline from `arc.pipeline` (empty string disables the sidecar).
2. Restart the sidecar process with the bypass flag.
3. If the pipeline is still unhealthy, disable the sidecar at the process
   supervisor and alert the platform owner.

## Recovery

- Do not re-enable live mode until the root cause is fixed.
- Re-run in shadow for 24 h.
- Confirm the shadow epoch is non-vacuous before trusting the comparison:
  - `shadow_active == 1` and `shadow_schedule_mode == QG`.
  - `shadow_schedule_samples == shadow_schedule_releases > 0`.
  - `shadow_immediate_applies == shadow_schedule_releases +
    shadow_scheduled_backlog`.
  - `shadow_record_total == shadow_immediate_applies > 0` (every applied
    entry has a replayable record).
  - The last shadow schedule release is fresh (promotion default: within
    300 s).
- Re-enter live on one shard only after:
  - Engine p99 at least 15% better than the bypass baseline.
  - Backpressure events at least 30% fewer.
  - Added latency at most 10 ms p99.
  - `arc_dropped_total` remains zero.

## Escalation

| Situation | Escalate to |
|---|---|
| Dropped or reordered apply | Storage engine owner + on-call |
| Latency regression | Receiver/apply plane owner |
| Sidecar crash | Platform / supervisor owner |
| Suspected data corruption | Durability owner immediately |

## Post-incident

- Capture the offset trajectory, heap depth, batch sizes, and engine p99.
- Identify whether the offset estimator, deadline, or ordering logic was at
  fault.
- Add a regression test to `tests/c/unit/test_arc_offset.c` or
  `tests/c/unit/test_arc_heap.c` before re-enabling.
