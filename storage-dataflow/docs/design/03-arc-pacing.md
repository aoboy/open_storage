# Prototype 3 — ARC Pacing Sidecar

**Status:** **As-built (Wave 4)** — ARC-QG core behind ABI v2 with the
paper's section 12.3 goldens landing on the analytic fixed point
(D alternates {94.7934, 93.8016} ms on the real clock); the sidecar with
shadow/live/killed; the deploy controller with the promotion gate and
the always-armed kill switch; the runbook executed as a test
([ADR-0006](../adr/0006-arc-abi-v2.md); provenance
[07 §3.3](07-wam-import-map.md#33-arc-prototype-3)).
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

See `configs/<env>/arc.toml` (strict TOML per
[ADR-0005](../adr/0005-config-format-toml.md)): `pipeline`, `batch_max`,
`batch_deadline_ns`, `offset_min_ns`, `offset_max_ns`, `increase_gain`,
`decrease_gain`, `shadow_mode`, `kill_switch`, plus the ADR-0006
parameter set (`mode`, ρ/λ/U/δ/γ/J/G, `T_idle_ns`, `auto_params`,
`min_smooth_bytes`, `budget_ms`).

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

## Import delta (wack_a_mole)

What W4 imports versus keeps, per
[07 §3.3](07-wam-import-map.md#33-arc-prototype-3):

| Mechanism | This design (v1) | Imported target (ADR-0006) | Source |
|---|---|---|---|
| Offset update | single EWMA estimator | asymmetric D update (ρ_u/ρ_l, λ_u/λ_l, U clip) + ARC-Q quantized E | WAM `src/arc/arc.c` |
| Release schedule | `release_time` from offset | T_n = max(A_n, S_n + E + J) with in-order guard G (ARC-QG) | same |
| Ordering | heap key `(release_time, producer_ts, seq)` | identical keying; QG guard makes violations structurally impossible | same |
| Measurement baseline | none | `ARC_MODE_IMMEDIATE` oracle (= the killed mode) for A/B added-latency | same |
| Validation | design-only | paper §12.3 goldens + 60 fps trace replay (QG: 0 violations) into `golden_vectors/arc_schedule.json` | WAM `test_arc` / `test_arc_experiments` |
| Bounds | `offset_max_ns`, `batch_deadline_ns` | retained, plus δ-clamp and `budget_ms` p99 alert | WAM J.5 |

Mode mapping to this document's table: `shadow` = ARC computes the
schedule and metrics, apply flows through the old path; `live` =
`ARC_MODE_QG`; `killed` = `ARC_MODE_IMMEDIATE`. The open questions close
as follows: `arc_sizeof` lands in W4.1; the EWMA/`safety` knobs are
superseded by the paper parameter set (auto-params from RTT and Δ_n
quantiles, W4.2); ordering violations are a correctness bug by
construction under QG — any observed violation rolls back to shadow
(W4.5 kill-switch leg proves the rollback).
