# Prototype 3 — ARC Pacing Sidecar

**Status:** **As-built (Wave 4, W6.4, W6.9, W6.10)** — ARC-QG core behind
ABI v2 with the paper's section 12.3 goldens landing on the analytic fixed
point (D alternates {94.7934, 93.8016} ms on the real clock); the sidecar
with shadow/live/killed, including true non-applying shadow scoring and
accepted-entry retention under pacer backpressure; the deploy controller
with the non-vacuous promotion gate and the always-armed kill switch; the
runbook executed as a test ([ADR-0006](../adr/0006-arc-abi-v2.md);
provenance [07 §3.3](07-wam-import-map.md#33-arc-prototype-3)).
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
| `shadow` | Compute the real QG release schedule without applying it; apply each accepted entry immediately and exactly once |
| `live` | Pacer owns the apply timing |
| `killed` | Bypass the pacer entirely; apply as data arrives |

Shadow mode must run for 24 h before live traffic on a shard, and the
promotion gate must see a live, non-vacuous shadow epoch (see
[true shadow scoring](#implementation-note--true-shadow-scoring-w69w610)).

## Rollout gates

| Stage | Entry criteria |
|---|---|
| Shadow | 24 h shadow on one pipeline; added latency measured from the non-applying QG schedule, not enforced |
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

True shadow (W6.10) adds separate schedule and immediate-apply series, so
the two paths are never summed as one:

- `sdf_arc_shadow_active`, `sdf_arc_shadow_epoch`,
  `sdf_arc_shadow_schedule_mode`
- `sdf_arc_shadow_schedule_releases_total` and
  `sdf_arc_shadow_schedule_samples_total` (the non-applying QG schedule)
- `sdf_arc_shadow_immediate_applies_total` (the applied sidecar path)
- `sdf_arc_shadow_scheduled_backlog`, `sdf_arc_shadow_record_count`,
  `sdf_arc_shadow_budget_alerts_total`
- `sdf_arc_shadow_added_latency_p50_ns`, `..._p95_ns`, `..._p99_ns`

The counters are epoch-scoped: each entry into shadow resets them, so
evidence from an earlier rollout cannot satisfy a later promotion.

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

Mode mapping to this document's table: `shadow` = ARC runs `ARC_MODE_QG`
for its schedule while the sidecar applies immediately and suppresses the
later QG apply (W6.10); `live` = `ARC_MODE_QG` with the pacer owning the
apply; `killed` = `ARC_MODE_IMMEDIATE`. The open questions close as
follows: `arc_sizeof` lands in W4.1; the EWMA/`safety` knobs are superseded
by the paper parameter set (auto-params from RTT and Δ_n quantiles, W4.2);
ordering violations are a correctness bug by construction under QG — any
observed violation rolls back to shadow (W4.5 kill-switch leg proves the
rollback).

### Implementation note — mode switching (W6.4)

`arc_set_mode()` swaps the pacer mode in place; `arc_sidecar_set_mode()`
stores an atomic *desired* mode that `arc_sidecar_pump_once()` applies,
so a controller flip lands within one poll interval. Entering `killed`
drains the whole QG backlog on that pump: under `ARC_MODE_IMMEDIATE`
`arc_drain` releases queued entries regardless of their `T_n` (the hit
is real elapsed time, so the added-latency sample stays honest).

W6.4 also baselines the controller's cumulative `budget_alerts_seen` at
promotion so historical shadow counters are never re-counted as fresh
live breaches.

### Implementation note — true shadow scoring (W6.9/W6.10)

Shadow is no longer mapped onto `ARC_MODE_IMMEDIATE`. The sidecar keeps
the pacer in `ARC_MODE_QG` for the entire shadow epoch, so `arc_submit()`
computes exactly the `T_n`/guard schedule that live mode would execute.
Each accepted entry is applied immediately by the sidecar itself; when the
pacer later releases that entry, the callback records the schedule release
and suppresses the apply. The result is one apply per accepted entry, a
separately measured QG schedule, and no late shadow callback.

**Evidence surface.** The additive public API in `include/sdf/arc.h` is:

- `arc_sidecar_shadow_stats(arc_shadow_stats_t *out)` — epoch-scoped
  schedule releases/samples, immediate applies, scheduled backlog,
  shadow budget alerts, last release timestamps, added-latency
  percentiles, record counts, and the schedule mode.
- `arc_sidecar_shadow_records(arc_shadow_record_t *out, uint32_t max)` —
  the newest up-to-`max` of the last `ARC_SHADOW_RECORD_CAP` (256)
  accepted entries in submit order, including `block_n`, `seq`, `S_n`,
  `A_n`, `T_n`, schedule `release_ns`, length, and the released/
  immediate-applied flags.

The structs are additive: existing `arc_cfg_t`/`arc_stats_t` layouts and
symbols are unchanged, and `test_abi` pins the new layouts. The scrape
endpoint exposes the `sdf_arc_shadow_*` series listed under
[Metrics](#metrics).

**Exactness and transitions.** `scheduled_backlog` counts entries accepted
but not yet released on the shadow schedule; it increments on immediate
apply and decrements on schedule release, so
`immediate_applies == schedule_releases + scheduled_backlog` holds for a
quiet epoch. Leaving shadow discards the already-applied schedule without
invoking apply, and advances QG order bookkeeping so the first post-flip
live entry is not parked as a missing predecessor. Entering shadow first
flushes accepted live/killed backlog in submit order; a live-to-killed flip
re-keys the backlog to release in submit order rather than heap `T_n`
order. Stop drains the ring, the pending slot, and the pacer; shadow stop
discards the schedule after the immediate path has applied every accepted
entry.

**W6.9 retention.** An entry accepted by the public SPSC ring but refused
by a full pacer (`-EAGAIN`) is retained in a pump-owned pending slot and
retried before another ring pop. `arc_submit()` reserves the slab slot
before mutating estimator, sequence, or auto-window state, so a retry is
not counted twice. A full *public* ring still returns `-EAGAIN` to the
caller, which keeps ownership of that unaccepted entry; that case is
distinct from already-accepted internal backpressure.

**Promotion gate.** The deploy controller promotes only when the existing
24 h window and alert budget pass *and* the shadow evidence is non-vacuous:
the current epoch is active, its schedule mode is QG, schedule samples
equal schedule releases and are non-zero, immediate applies equal
releases plus backlog and are non-zero, retained record totals equal the
immediate applies and are non-zero, and the last schedule release is
non-zero and no older than `shadow_evidence_max_age_s` (default 300 s). Absent, empty, stale,
inactive, inconsistent, or IMMEDIATE-only evidence is rejected, and the
decision record carries `shadow_evidence` with the rejection reason. A
fixture-shortened test run is not a production 24-hour soak.
