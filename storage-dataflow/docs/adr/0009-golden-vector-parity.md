# ADR-0009: Golden-Vector Parity as the Import Acceptance Gate

**Status:** Accepted
**Date:** 2026-11-18
**Related:** [Import map](../design/07-wam-import-map.md),
[ADR-0001](0001-c-abi-as-contract.md),
[Implementation plan W0.4](../design/08-implementation-plan.md)

## Context

The data-plane cores (spray/profile, FLUID budget, ARC schedule, RaptorQ
codec) are being imported from the wack_a_mole repository and adapted to
the `include/sdf/*.h` ABI (import map). Adaptation is where correctness
is lost: renamed fields, widened types, changed call orders. Both
repositories' ADR-0001 names `golden_vectors/` as the behavior contract —
but the sdf vector files are currently **empty placeholders** (`[]`), so
today they gate nothing.

wack_a_mole carries validated golden material: the spray golden
θ(249,10) = 636; ARC paper §12.3 release-schedule vectors; FLUID
budget/efficiency/cost bounds (O.1) and loss-ladder properties (O.5); and
property harnesses (discrepancy bounds, embodiment residual fairness).

## Decision

1. **Parity is the acceptance gate for every imported module.** An import
   prompt (per the implementation plan) is not done until the adapted code
   passes the corresponding golden vectors **and** the wack_a_mole-sourced
   property bounds, both wired into CTest.
2. Vectors are filled from the source test suite, not re-derived: θ and
   seed-sequence vectors into `golden_vectors/wm_select.json`; ARC §12.3
   schedules into `golden_vectors/arc_schedule.json`; fountain
   round-trip/loss pairs into `golden_vectors/fountain_esi.json`; FLUID
   budget/efficiency/cost identities live as property tests (they are
   inequalities over ranges, better expressed as sweeps than JSON).
3. **Provenance header rule:** every imported/adapted C file carries a
   first-comment block: source repo, source path, source commit, and the
   sdf ADR/prompt that authorized the adaptation. Doc-level provenance
   lives in the import map §3.
4. Where the sdf ABI legitimately diverges from the wack_a_mole API (e.g.
   `wm_select` vs `wam_spray_next` argument shapes), a **bridge test**
   asserts behavioral identity on the same inputs (same profile, same
   seeds ⇒ same unit→path sequence), so the divergence is mechanical, not
   semantic.
5. `tests/c/test_parity.c` (prompt W0.4) is the single harness that loads
   the JSON vectors and runs every parity check; CI (the `ci` gate) runs it
   on every landing, so golden drift fails the build, per ADR-0001.

## Consequences

Positive:

- Import risk becomes a binary, test-enforced property instead of a
  review claim.
- The empty-vector placeholder problem closes before the first import
  lands, in Wave 0.
- Future Rust migrations (ADR-0003) inherit the same gate unchanged.

Negative:

- Golden files grow with each import; they are append-only per
  ADR-0001 — behavior changes require an ADR and a vector bump.
- Bridge tests couple the two repositories' test expectations; the
  coupling is deliberate and time-boxed to the import waves (after Wave 5
  the sdf suite is self-standing).

## Alternatives considered

| Alternative | Why rejected |
|---|---|
| Diff the imported source against WAM per commit | Textual identity is neither necessary (ABI differs) nor sufficient (semantics can shift in adaptation) |
| Trust unit tests written fresh against the sdf ABI | Reproduces the validation burden instead of inheriting it; the source goldens are the evidence |
| Defer vectors until each prototype's e2e lands | The gate would be absent exactly during the highest-risk adaptation window |

## Compliance

- `tests/c/test_parity.c` in the `ci` gate (prompt W0.4); every import
  prompt's Acceptance section names its vectors and property sweeps.
- The import map §3's parity column is the authoritative prompt↔vector
  mapping; this ADR defines the rule it refers to.
