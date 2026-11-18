# ADR-0004: Nanorq Behind the Fountain ABI

**Status:** Proposed
**Date:** 2026-11-16
**Related:** [Nanorq evaluation](../design/06-nanorq-evaluation.md),
[FLUID repair](../design/02-fluid-repair.md)

## Context

Prototype 2 needs a fountain codec for slack-based repair. The design offers
two candidates: a simple LT codec and Nanorq's RaptorQ implementation
(RFC 6330). RaptorQ has near-zero reception overhead but a more complex
decoder, stateful codec objects, and a smaller community.

## Decision

Keep one codec ABI, `include/sdf/fountain.h`, and implement it twice:

- `src/c/fountain/fe_codec_lt.c` — reference LT implementation.
- `src/c/fountain/fe_codec_nanorq.c` — adapter over Nanorq's low-level API.

Run three arms in Prototype 2:

| Arm | Codec | Termination |
|---|---|---|
| Control | ARQ | All fragments |
| A | LT | FLUID gate |
| B | Nanorq RaptorQ | FLUID gate |

Keep the ARQ fallback in all arms. Choose the production default per job class
from the p99-versus-overhead Pareto curve, not from a single benchmark.

Vendor a pinned Nanorq commit under `third_party/nanorq/`, record the hash, and
disable host-CPU autotuning in reproducible builds.

## Consequences

Positive:

- The A/B isolates the codec effect from the FLUID effect.
- LT remains a simple regression reference and an ARQ-adjacent fallback.
- RFC 6330 gives an interop path for future Rust or hardware implementations.

Negative:

- Two codecs to test and maintain.
- Nanorq's stateful objects require one instance per worker/job.
- TSNC helpers are experimental and must not block the prototype.

## Alternatives considered

| Alternative | Why rejected |
|---|---|
| Use only LT | Leaves overhead on the table and weakens the decode gate |
| Use only Nanorq | No reference arm; a library bug becomes a project bug |
| Treat Nanorq as a Whack-a-Mole replacement | Category error; different layer |

## Compliance

- Both codecs must pass `golden_vectors/fountain_esi.json`.
- The CMake target links only one codec per arm.
- The ADR is revisited after the Prototype 2 Pareto report.
