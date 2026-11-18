# Prototype 3 — ARC Pacing Sidecar

**Status:** Design ready; C implementation is a stub. Highest interaction
risk.
**Related:** [Overview §6.3, §7.5](00-overview.md#6-prototype-sequence-roirisk-order).

## Objective

Smooth the receiver's apply stream so the storage engine sees the sender's
original timing instead of bursts caused by recovery. The sidecar sits between
the replication recovery buffer and the engine apply API. It uses only the
receiver clock: no sender feedback, no transport changes, no leader RPC.

## Offset estimator

Maintain an adaptive release offset that tracks the upper envelope of observed
lateness:

```text
offset_ns = max(offset_ns, ewma_max_lateness_ns * safety)
offset_ns = max(offset_ns - decay_ns, offset_min_ns)
offset_ns = min(offset_ns, offset_max_ns)
```

A sparse pipeline must not accumulate unbounded delay: `offset_max_ns` and
`batch_deadline_ns` are hard bounds.

## Release ordering

Entries are released in producer order. The heap key is:

```text
(release_time, producer_ts, seq)
```

Ties break on `seq`, so ordering is deterministic. A batch is released when
either `batch_max` entries are ready or `batch_deadline_ns` expires, whichever
comes first.

## Modes

| Mode | Behaviour |
|---|---|
| `shadow` | Compute releases and metrics, but apply immediately through the old path |
| `live` | Pacer owns the apply timing |
| `killed` | Bypass the pacer entirely; apply as data arrives |

Shadow mode must run for 24 h before live traffic on a shard.

## Rollout gates

| Stage | Entry criteria |
|---|---|
| Shadow | 24 h shadow on one pipeline; added latency measured but not enforced |
| One shard | No correctness errors; `arc_dropped_total == 0` |
| Fleet | Engine p99 ≥ 15% better, backpressure events ≥ 30% fewer, added latency ≤ 10 ms p99 |

The kill switch is always available and must be tested before live rollout.

## Metrics

- `arc_offset_ns`
- `arc_heap_depth`
- `arc_release_batch_size`
- `arc_added_latency_ns`
- `arc_dropped_total`
- `engine_apply_p99_seconds`
- `engine_backpressure_events_total`

## Configuration

See `configs/<env>/arc.yaml`: `pipeline`, `batch_max`,
`batch_deadline_ns`, `offset_min_ns`, `offset_max_ns`, `increase_gain`,
`decrease_gain`, `shadow_mode`, `kill_switch`.

## Failure modes

| Failure | Response |
|---|---|
| Added latency exceeds bound | Shrink offset; if still over, kill switch |
| Heap growth / starvation | Release on `batch_deadline_ns`; alert on sustained depth |
| Out-of-order release | Treat as correctness bug; roll back to shadow |
| Sparse pipeline accumulates offset | Clamp and decay; kill switch if p99 regresses |

## Open questions

- The pacer is opaque and caller-allocated; like the fountain objects it needs
  a size/allocation function before the ABI freezes.
- Define the exact EWMA half-life and `safety` multiplier per pipeline class.
- Decide whether ordering violations are dropped or force a bypass.
