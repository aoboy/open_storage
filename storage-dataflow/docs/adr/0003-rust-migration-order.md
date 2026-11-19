# ADR-0003: Rust Migration Order

**Status:** Accepted — fountain comparison window open (W6.8)
**Date:** 2026-11-16
**Related:** [Rust migration plan](../design/04-rust-migration.md),
[ADR-0001](0001-c-abi-as-contract.md)

## Context

Rust offers memory safety and ownership benefits, but the C data plane is
already designed around fixed layouts, preallocated pools, and a stable ABI.
A wholesale rewrite would delay the prototypes and obscure A/B results.

## Decision

Migrate module by module, in this order:

1. **Fountain codec** — pure compute, no I/O, no shared state.
2. **ARC pacer** — heap, timing, and callback; ownership removes a bug class.
3. **Whack-a-Mole dispatch** — table lookup; leave in C the longest.

Each step:

- Exports the same `extern "C"` symbols from a `staticlib` or `cdylib`.
- Passes the same golden vectors and tests as the C implementation.
- Runs behind a CMake option for one release.
- Regenerates the header with cbindgen; the diff must be empty except for
  intentional changes.

## Consequences

Positive:

- Low-risk, reversible migrations.
- A/B comparison between implementations is possible at each step.
- The C ABI remains the single contract for callers.

Negative:

- Two implementations must be maintained during the comparison window.
- Rust toolchain, vendoring, and CI cost arrive before the first migration
  pays off.
- Panic/FFI discipline is mandatory.

## Alternatives considered

| Alternative | Why rejected |
|---|---|
| Full rewrite in Rust | Delays prototypes; no incremental rollback |
| Keep everything in C | Leaves memory-safety benefits on the table for compute-heavy modules |
| Start with Whack-a-Mole | Lowest benefit; table lookup is already trivial and safe |

## Compliance

- W6.8 started the fountain step: `src/rust/sdf-fountain` now implements LT,
  while `src/rust/sdf-arc` remains a placeholder. Only the internal LT arm is
  replaced; C public dispatch and nanorq stay in the same archive.
- `scripts/test_rust_fountain.sh --require` builds both variants, runs the full
  C suites and golden vectors, diffs the cbindgen surface, checks exact archive
  symbols, and exercises sanitizer/install/package consumers.
- The C LT implementation remains available and default throughout the
  comparison window. It can be removed only after the required benchmark and
  hosted-run evidence; no removal is part of W6.8.
