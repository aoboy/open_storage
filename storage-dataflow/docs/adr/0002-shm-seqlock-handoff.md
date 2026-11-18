# ADR-0002: SHM + Seqlock for the C↔Python Handoff

**Status:** Proposed
**Date:** 2026-11-16
**Related:** [Overview §7.2](../design/00-overview.md#72-phase-0--shared-foundations-weeks-12)

## Context

The Python control plane recomputes Whack-a-Mole profiles and reads telemetry
at 4 Hz. The C data plane must not take a lock or perform an RPC per data unit,
and it must never block on a stalled Python process.

## Decision

Use a single shared-memory segment with fixed offsets and a seqlock:

```text
offset 0x0000 : seqlock (64 bytes)
offset 0x0040 : profile/knob buffer A
offset 0x0800 : profile/knob buffer B
offset 0x0FC0 : telemetry header
offset 0x1000 : telemetry ring
```

Protocol:

1. Python writes a complete profile into the inactive buffer.
2. Python increments the profile version.
3. Python takes the write seqlock and flips `active_buf`.
4. C reads `active_buf`, then reads the active buffer under the read seqlock,
   retrying if the sequence changed or is odd.
5. Telemetry flows the other way through a lock-free SPSC ring.

There is exactly one writer and one reader per channel. No RPC, no `mmap`
remapping after init, no dynamic allocation on the hot path.

## Consequences

Positive:

- No syscalls or locks in the data path.
- A stalled control plane leaves the last complete profile in place.
- Layout is language-neutral and testable with golden vectors.

Negative:

- The layout is ABI: offsets cannot change without a version bump.
- Seqlock misuse (multiple writers, torn reads) is a correctness bug; it needs
  a single-writer discipline and unit tests.
- Fixed-size buffers cap the number of paths and knobs.

## Alternatives considered

| Alternative | Why rejected |
|---|---|
| Unix socket RPC per refresh | Adds latency and a failure mode for no benefit at 4 Hz |
| POSIX shared memory without seqlock | Reader can observe a torn profile during a flip |
| Double-buffer with atomics only | Still needs a consistent publication protocol for multi-field profiles |

## Compliance

- `include/sdf/common/seqlock.h` and `shm.h` define the layout.
- `tests/c/test_abi.c` asserts the offsets.
- The C-side reader must be exercised by a concurrent Python writer test
  before Prototype 1 goes live.
