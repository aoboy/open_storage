# ADR-0003: Rust Migration Order

**Status:** Proposed
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

- `src/rust/sdf-fountain` and `src/rust/sdf-arc` are placeholders until their
  phase starts.
- CI must build both variants and run golden vectors before remove of a C
  implementation.
