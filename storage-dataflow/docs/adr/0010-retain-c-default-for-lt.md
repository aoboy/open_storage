# ADR-0010: Retain C as the Default LT Implementation

**Status:** Accepted
**Date:** 2026-11-20
**Related:** [ADR-0003](0003-rust-migration-order.md),
[ADR-0004](0004-nanorq-behind-fountain-abi.md),
[ADR-0009](0009-golden-vector-parity.md),
[Implementation plan W6.8/W6.11/W6.12/W6.13](../design/08-implementation-plan.md),
[W6.13 comparison review](../design/w6.13-lt-comparison-review.md),
[FLUID design](../design/02-fluid-repair.md)

## Context

W6.8 landed a functional Rust LT arm behind the unchanged fountain ABI.
`SDF_USE_RUST_FOUNTAIN=OFF` remains the default; `ON` selects Rust for the LT
arm while nanorq and the public C callers stay unchanged. ADR-0003 requires a
comparison window before any default cutover or C removal.

W6.13 asks for a decision grounded in:

1. hosted correctness evidence (W6.11);
2. byte-identical deterministic parity and both-variant consumers;
3. paired same-runner C/Rust performance measurements (W6.12);
4. at least seven paired nightly samples, or a documented decision to retain
   the status quo while evidence remains insufficient.

The available evidence is incomplete:

- The W6.8 landing recorded C/Rust parity, identical public declarations,
  both-variant installed consumers, and matching test suites at that revision.
- The current tree has since grown to 28 C tests, 116 Python tests, and 1,410
  zero-pending golden comparisons. Hosted rust-seam run 44774290565 at
  `0d6e60a` executed both variants against that current tree successfully.
- The authoritative hosted matrix is green at that revision. Nightly history
  does not exist yet.
- W6.12 has local single-runner C measurements only. No paired same-runner
  C/Rust LT measurements exist, and no stable named reference runner has been
  activated.

The missing evidence is not a negative performance result. It is absence of
the evidence the migration gate requires.

## Decision

1. **C remains the default LT implementation.**
2. **The Rust LT arm remains opt-in** through
   `SDF_USE_RUST_FOUNTAIN=ON`; both arms remain compiled and available.
3. **No C LT source is removed.** Nanorq remains C and outside this decision.
4. **No cutover, performance, or Rust-superiority claim is made.**
5. W6.13 closes with the status-quo retention branch of its acceptance
   criteria, not with a recommendation to switch.

## Revisit criteria

A future decision to change the default must satisfy all of the following,
using evidence recorded at a named revision:

1. Both variants pass the current C, Python, sanitizer, and golden-parity
   suites; the current 1,410 comparisons must be byte-identical, not merely
   the W6.8-era 556.
2. The authoritative hosted full matrix is green at that revision, including
   the Rust leg.
3. At least seven paired same-runner nightly samples cover the supported LT K,
   symbol-size, and loss patterns. The documented noise rule applies, and no
   workload regresses by more than 5%.
4. Installed pkg-config, CMake `find_package`, CFFI, wheel, and Debian
   consumers execute both LT and nanorq in both variants.
5. The change lands separately with its own tests and an exercised rollback
   plan. A decision record alone cannot change the default.

## Rollback

No production switch occurs under this ADR, so there is no active rollback.
If a later ADR authorizes `ON` by default, rollback is a reviewed landing that
restores `SDF_USE_RUST_FOUNTAIN=OFF`, rebuilds and reinstalls the C archive,
and re-runs both-variant ABI, parity, package, and consumer checks. The C arm
must remain present until the comparison window closes.

## Alternatives considered

| Alternative | Why rejected |
|---|---|
| Switch the default to Rust now | Hosted and paired-performance evidence is absent; W6.13 acceptance would fail |
| Delete the C LT arm | Irreversible without evidence, violates ADR-0003's comparison-window order, and removes the rollback path |
| Treat local C-only measurements as paired evidence | They are single-runner and contain no Rust samples; presenting them as paired would fabricate coverage |
| Defer any decision until W6.11/W6.12 finish | The prompt explicitly permits retaining the status quo while evidence is insufficient; recording that outcome prevents an implicit default change |

## Consequences

Positive:

- The default remains the implementation with the broadest executed evidence
  and the existing rollback path.
- The opt-in Rust arm remains available for the eventual paired comparison.
- Missing hosted/cargo evidence is recorded as a skip, not converted into a
  performance or cutover claim.

Negative:

- The comparison window stays open and W6.13 must be revisited after W6.11
  and W6.12 produce their evidence.
- Maintaining both arms costs build and test time until the decision changes.

## Compliance

- `SDF_USE_RUST_FOUNTAIN` remains `OFF` by default in CMake.
- This ADR changes no public header, ABI, golden vector, or installed artifact.
- W6.13's status and the implementation ledger record the decision and its
  missing-evidence conditions.
