# ADR-0001: C ABI as the Migration Contract

**Status:** Proposed
**Date:** 2026-11-16
**Related:** [Overview §7.1, §12](../design/00-overview.md#7-implementation-break-down)

## Context

The data plane is written in C for predictable latency. The control plane is
Python. A future phase may move selected modules to Rust. Without a stable
boundary, each language change becomes a coordinated rewrite of every caller.

## Decision

The public C headers under `include/sdf/` are the migration contract:

- Every public symbol is `extern "C"` and versioned by `SDF_VERSION_*`.
- Public structs use fixed-width types, fixed layout, and no language-specific
  constructs beyond C11 atomics where documented.
- Golden vectors under `golden_vectors/` define expected input/output
  behaviour.
- An ABI-breaking change requires an ADR and a version bump.
- Rust modules replace C modules by exporting the same symbols from a
  `staticlib`/`cdylib`; callers do not change.

## Consequences

Positive:

- C, Python (via CFFI), and future Rust implementations share one contract.
- Golden vectors catch accidental layout and behaviour drift.
- Module-by-module migration is possible without a flag day.

Negative:

- The ABI is intentionally conservative; some convenient language features
  cannot cross it.
- Header changes have a review and documentation cost.
- Opaque types need explicit size/allocation helpers before freeze (open item
  for `fountain.h` and `arc.h`).

## Alternatives considered

| Alternative | Why rejected |
|---|---|
| Rewrite everything in Rust now | Too much scope and risk before the prototypes prove value |
| Use a C++ ABI | Exceptions, name mangling, and runtime differences complicate the boundary |
| RPC between C and Python | Adds latency and failure modes to the hot path |

## Compliance

- `tests/c/test_abi.c` checks struct offsets and constants at compile time.
- `bindings/cffi/build_ffi.py` parses the public header; any unsupported
  construct breaks the build.
- `golden_vectors/` must be updated with an ADR when behaviour changes.
