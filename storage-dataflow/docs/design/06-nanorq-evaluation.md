# Nanorq Evaluation (RaptorQ vs. LT Codec)

**Status:** Recommendation accepted for Prototype 2; library not vendored yet.
**Related:** [Overview §10](00-overview.md#10-nanorq-evaluation-raptorq-vs-lt-codec),
[ADR-0004](../adr/0004-nanorq-behind-fountain-abi.md).

## 1. Clarification: Not a Whack-a-Mole Substitute

A direct "Nanorq vs. Whack-a-Mole" comparison is a category error. Nanorq is
an erasure codec; Whack-a-Mole is a path dispatcher. They operate at different
layers and compose. The correct comparison is **Nanorq vs. LT inside
Prototype 2**.

## 2. What Nanorq Is

Nanorq is a C implementation of RaptorQ (RFC 6330), described as a fast,
compact rateless codec capable of multi-gigabit speeds on a single core. It
provides:

- A high-level API (`nanorq.h`) with allocation, alignment, and I/O handling.
- A low-level API (`nanorq_core.h`) usable without libc under `NDEBUG`.
- Experimental TSNC helpers for multipath recoding and streaming.
- Host-CPU autotuning.

RaptorQ targets near-zero reception overhead: roughly `K` symbols plus a tiny
fraction, rather than 1.5× or 2× the data. It uses GF(256) for improved rank
properties.

## 3. Why It Matters for FLUID

FLUID finishes when the decoder is ready. If the codec needs 1.5× the symbols,
the gate is reachable only after substantial extra traffic. RaptorQ makes the
decode-sufficiency gate cleaner and lowers the overhead axis of the
p99-versus-overhead Pareto curve.

## 4. Pros

| ID | Pro | Relevance |
|---|---|---|
| P1 | RFC-compliant near-zero overhead | Lowers the overhead axis of the Pareto curve |
| P2 | C implementation, no-libc low-level API | Fits the hot-path language decision |
| P3 | Maximally fungible symbols | Cleaner decode-sufficiency gate than LT |
| P4 | TSNC multipath recoding | Intermediate peers can recode without a full decode |
| P5 | Standardized interop (RFC 6330) | Future Rust/HW/third-party interop |

## 5. Cons

| ID | Con | Mitigation |
|---|---|---|
| C1 | RaptorQ decode is more complex than LT | Consume the library; LT remains the reference arm |
| C2 | Codec objects are stateful and not thread-safe per object | One codec object per worker/job |
| C3 | Block-size and symbol-size constraints | Account for them in Pareto measurement |
| C4 | TSNC helpers are experimental | Defer multipath recoding to phase 2 |
| C5 | Modest community footprint | Vendor and pin; fork-ready |

## 6. Recommendation

- **Prototype 1:** Nanorq is irrelevant.
- **Prototype 2:** Swap LT for a Nanorq adapter behind the same
  `include/sdf/fountain.h` ABI. Run three arms: Control (ARQ), A (FLUID + LT),
  B (FLUID + Nanorq). Keep the ARQ fallback.
- **Prototype 3:** Nanorq is orthogonal.

Expected outcome: Treatment B has lower overhead at the same p99 than A. If
the Pareto curves are close, the LT arm remains viable and simpler; if B
dominates, Nanorq becomes the default and LT becomes the regression reference.

## 7. Integration plan

1. Vendor a pinned commit under `third_party/nanorq/` and record the hash.
2. Implement `src/c/fountain/fe_codec_nanorq.c` as an adapter over the
   low-level API; keep the LT implementation as the reference.
3. Add `golden_vectors/fountain_esi.json` cases that both codecs must pass.
4. Measure both arms with `ε ∈ {0.01, 0.05, 0.10, 0.20}`.
5. Decide the default per class from the Pareto result, not from the library's
   marketing numbers.

## 8. Risks and open questions

| Risk / question | Owner action |
|---|---|
| License compatibility | Review before vendoring; record in the ADR |
| Build flags (`-march=native`) break reproducibility | Disable autotuning in CI; allow it only in benchmark builds |
| Low-level API without libc under `NDEBUG` | Verify the exact build and symbol configuration |
| Codec object lifetime | Add a size/create/destroy API before freezing `fountain.h` |
| TSNC maturity | Defer to phase-2 experiment; do not block Prototype 2 |
| Symbol-size constraints | Document valid block/symbol ranges in the header |

## 9. Net assessment

Nanorq is a better codec choice than a hand-rolled LT implementation for
FLUID repair, given the complexity tax and thread-safety discipline. It is not
a Whack-a-Mole replacement. The cleanest path is to keep the `fountain.h` ABI,
implement twice, and let the A/B decide.
