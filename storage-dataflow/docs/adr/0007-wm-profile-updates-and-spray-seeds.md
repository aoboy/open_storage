# ADR-0007: WM Profile Updates and Spray Seeds

**Status:** Accepted (additive change to `include/sdf/wm.h`; no break to v1 symbols)
**Date:** 2026-11-18
**Related:** [WM dispatch design](../design/01-whack-a-mole.md),
[Import map §5](../design/07-wam-import-map.md),
[Implementation plan W2.1–W2.2](../design/08-implementation-plan.md),
[wack_a_mole `include/wam/{profile,spray}.h`, `src/core/{profile,spray}.c`]

## Context

`include/sdf/wm.h` v1 compiles a profile into a `unit_to_path` table and
selects with `unit_to_path[counter & (m-1)]` — the bit-reversal ordering
that bounds discrepancy over contiguous counters. Two paper mechanisms are
missing:

1. **Whack updates (embodiments 1–4).** The paper's congestion response is
   a ball-removal `e(i)` from degraded paths with deterministic
   redistribution (global residual index r), not just the boolean
   `down_mask` renormalization v1 ships. The controller design
   (`wm_shed`, the Python loop) needs the graded response to implement
   soft shedding; `down_mask` alone is the hard-failure fast path.
2. **Shuffle methods and seeds.** The paper randomizes the walk start via
   `shuffle1`/`shuffle2` (key = θ((sa + j·sb) mod 2^ℓ) or
   (sa + sb·θ(j,ℓ)) mod 2^ℓ), with discrepancy bounds ≤ ℓ and ≤ 2ℓ
   respectively. Without seeds, two jobs with the same profile emit
   identical unit→path sequences — correlated skews across jobs, which is
   precisely the failure mode dispatch exists to fix.

The wack_a_mole core implements both, table-driven
(`wam_profile_update(p, e, mode)` with modes 1–4; `wam_spray_init(ell, sa,
sb)`; wrap rotation; a 256-byte bit-reversal table; and
`wam_measure_discrepancy` used by property tests).

## Decision

1. `wm.h` gains, adapted from the wack_a_mole core:
   - `wm_profile_update(wm_profile_t *p, const uint32_t *e, int mode)`
     with embodiments 1–4 and the global residual index; Σb = m and
     "the last healthy path is never removed" remain invariants.
   - Spray method and seed knobs on the compiled form:
     `wm_compile_cfg_t { method ∈ {PLAIN, SHUFFLE1, SHUFFLE2}, sa, sb }`,
     `sb` odd (coprime to m); `wm_select` applies the method's key
     transform before the table lookup; wrap rotation advances the seed.
   - `wm_select_batch` stays the batched form (base counter).
   - `wm_mark_down/up` keep their v1 semantics (cdf retains the original
     allocation for exact restore).
2. Discrepancy property bounds become the test contract: deviation
   ≤ ℓ (shuffle1), ≤ 2ℓ (shuffle2) over any contiguous window, with the
   wack_a_mole harness (`wam_measure_discrepancy` adaptation) in
   `test_wm_select`.
3. Golden vectors: `golden_vectors/wm_select.json` gains the θ golden
   (θ(249,10) = 636), a seed-pair sequence, and a discrepancy-bound
   vector set (ADR-0009 parity).

## Consequences

Positive:

- Soft shedding gets its graded mechanism; the Python controller writes
  `b[]` deltas instead of booleans, and the C fast path keeps hard-down.
- Per-job seed decorrelation; the paper's short-transfer guarantee
  (bounded discrepancy on any contiguous window) becomes a property test,
  not a claim.

Negative:

- `wm_compiled_t` layout grows (method/seed fields); v1 struct users must
  recompile — acceptable pre-release, covered by `test_abi.c` layout
  asserts and a `SDF_VERSION` minor bump.
- Two more knobs (method, seeds) in config; the strict loader (ADR-0005)
  validates ranges (sb odd, method enum).

## Alternatives considered

| Alternative | Why rejected |
|---|---|
| Keep v1 selection, implement updates only in Python | Graded response would wait on the 4 Hz loop; the paper requires the update at spray time, and the controller could not publish coherent profiles |
| Per-job random hashing (stochastic) | Unbounded discrepancy on short transfers — the property the paper stack is chosen for |

## Compliance

- `tests/c/unit/test_wm_select.c` (prompt W2.1): θ golden, seed-pair
  sequences, wrap rotation, discrepancy property sweep.
- `tests/c/unit/test_wm_profile.c` (prompt W2.2): embodiment goldens,
  Σb = m, residual fairness, last-healthy-path invariant.
- `tests/c/test_abi.c` re-pins struct offsets after the change.
