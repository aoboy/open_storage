# ADR-0006: ARC ABI v2 — Full ARC / ARC-Q / ARC-QG Semantics

**Status:** Accepted (ABI-breaking change to `include/sdf/arc.h`; version bump per ADR-0001)
**Date:** 2026-11-18
**Related:** [ARC pacing design](../design/03-arc-pacing.md),
[Import map §5](../design/07-wam-import-map.md),
[Implementation plan W4](../design/08-implementation-plan.md),
[wack_a_mole `include/wam/arc.h`, `src/arc/arc.c`]

## Context

`include/sdf/arc.h` (v1) models a simplified offset estimator: an adaptive
`offset_ns`, a heap keyed `(release_time, producer_ts, seq)`, and batch
release by `batch_max` / `batch_deadline_ns`. The ARC paper's algorithm is
richer, and wack_a_mole implemented and validated it end-to-end
(`src/arc/arc.c`, 861 loc): asymmetric D update (ρ_u/ρ_l, λ_u/λ_l, U clip),
ARC-Q quantized offset E with midpoint hysteresis, ARC-QG in-order guard G,
idle re-anchor, δ-clamp and a p99 added-latency budget, auto-parameters from
RTT and Δ_n quantiles, and an IMMEDIATE oracle for A/B measurement. That
implementation passes the paper §12.3 golden vectors and the 60 fps
peak/valley trace replay (ARC-QG: 0 order violations, median interval
error 0).

The v1 header also leaves the pacer opaque without a size/allocation
helper — an open question flagged in both `03-arc-pacing.md` and
ADR-0001's compliance notes.

## Decision

1. `include/sdf/arc.h` moves to **v2** with the full release-schedule
   semantics, adapted from the wack_a_mole core behind the sdf naming:
   - Modes: `ARC_MODE_BASE` (Alg 1), `ARC_MODE_Q` (quantized E),
     `ARC_MODE_QG` (production default: ordered release with guard G), and
     `ARC_MODE_IMMEDIATE` (test oracle only — the "killed" baseline).
   - `arc_cfg_t` gains the paper parameter set: ρ_u, ρ_l, λ_u, λ_l, U, δ,
     γ, J, G, T_idle, auto-param gains, `min_smooth_bytes`, and
     `budget_ms` (0 = off). `batch_max` / `batch_deadline_ns` are retained
     as the apply-side drain knobs.
   - `arc_submit(p, entry, len, producer_ts)` keeps its shape:
     `producer_ts` **is** the sender timestamp S_n; A_n is sampled on the
     receiver clock inside submit. No clock synchronization is required
     (paper property, preserved).
   - `arc_poll(p, now_ns)` drains due entries to the apply callback;
     `arc_run` becomes a poll loop helper over `arc_poll`.
   - `arc_stats_t` gains: current D and E, added-latency histogram
     (count/min/max/mean + p50/p95/p99), release-interval/jitter series,
     guard fires, δ-clamps, budget alerts, `S_n` rejects.
   - A `arc_sizeof(cfg)` / `arc_init` pair replaces the opaque
     caller-guess allocation.
2. Ordering rule (paper): an entry is released only through ARC; completion
   means all entries **released**, not merely recovered — the sidecar owns
   this contract end to end.
3. Golden vectors: `golden_vectors/arc_schedule.json` (today an empty
   placeholder) is filled from the paper §12.3 vectors carried by the
   wack_a_mole test suite, plus an L.6-style trace replay; passing them is
   the acceptance gate for the imported core (ADR-0009).
4. `shadow` / `killed` / `live` deployment modes (per `03-arc-pacing.md`)
   map onto the ABI: shadow = ARC computes releases and metrics but the
   sidecar applies immediately; killed = `ARC_MODE_IMMEDIATE`; live =
   `ARC_MODE_QG`.

## Consequences

Positive:

- The pacer implements the actual published algorithm, with the strongest
  available test evidence, instead of an approximation that would have to
  be re-proven.
- The IMMEDIATE oracle gives the A/B baseline the rollout gates need
  (added-latency ≤ 10 ms p99 vs killed).
- The allocation open question closes before any consumer compiles
  against the header.

Negative:

- ABI break versus v1: permitted only with this ADR and a
  `SDF_VERSION` minor bump (ADR-0001 process); v1 has no external
  consumers yet, so the cost is contained.
- More knobs to misconfigure; mitigated by strict-schema config with
  ranges (ADR-0005) and documented defaults in the loader examples.

## Alternatives considered

| Alternative | Why rejected |
|---|---|
| Keep v1 estimator, adapt WAM core beneath it | Hides modes/oracle/stats the rollout gates depend on; would fork the header from the algorithm it runs |
| Reimplement ARC from the paper in fresh code | Duplicates a validated implementation; forfeits §12.3 goldens and trace-replay evidence |
| ARC feedback to the sender | Explicitly a non-goal of the paper and of `03-arc-pacing.md` |

## Compliance

- `tests/c/unit/test_arc_offset.c`, `test_arc_heap.c` (prompt W4.2) run the
  §12.3 goldens, quantization invariants, guard bound, idle re-anchor,
  δ-clamp and budget alerts.
- `tests/c/integration/test_arc_sidecar.c` (prompt W4.5) runs the trace
  replay in all four modes and pins `ARC_MODE_QG`: 0 order violations.
- `tests/c/test_abi.c` asserts the v2 struct layout and the version bump.
