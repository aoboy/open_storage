# Prototype 2 — FLUID Repair

**Status:** Design ready; C implementation is a stub. Lands in plan wave
[W3](08-implementation-plan.md#6-phase-w3--fluid-repair-prototype-2) with
the wack_a_mole FLUID phase-O core imported for the sender budget
([07 §3.2](07-wam-import-map.md#32-fluid-prototype-2)); the LT reference
arm is the one fresh-code exception (no WAM LT exists).
**Related:** [Overview §6.2, §7.4](00-overview.md#6-prototype-sequence-roirisk-order),
[ADR-0004](../adr/0004-nanorq-behind-fountain-abi.md),
[Nanorq evaluation](06-nanorq-evaluation.md).

## Objective

Cut cross-region repair p99 by finishing as soon as a block is decodable,
instead of waiting for every fragment. The repair path is isolated, so it can
be A/B tested without touching the write path.

## Mechanism

FLUID replaces all-or-nothing ARQ with a slack parameter `ε`. For each repair
round `r`, record `sent_r` and `recv_r` and update:

```text
loss_r    = max(0, (sent_r - recv_r) / sent_r)
loss_prod = product(loss_r) over rounds

finish when loss_prod < ε
```

The repair loop still stops when the decoder reports readiness:

```c
while (!fd_ready(dec) && !fg_done(gate)) {
    fg_round_start(gate);
    /* send a wave of coded symbols */
    /* wait for wave ack or timeout */
    fg_observe(gate, sent, recv);
}
if (fd_ready(dec)) {
    fd_decode(dec, out, len);
} else {
    /* ARQ fallback: never ship an incomplete block */
}
```

The ARQ fallback is the safety guarantee. If the gate fires before decode
sufficiency, the coordinator falls back rather than returning an incomplete
block.

## Codec ABI

`include/sdf/fountain.h` hides both codecs behind one encoder/decoder ABI:

- `fe_codec_lt.c` — reference LT implementation, simpler and higher overhead.
- `fe_codec_nanorq.c` — adapter over the Nanorq low-level API.

Both arms must pass `golden_vectors/fountain_esi.json` before any traffic is
enrolled.

## Three-arm experiment

| Arm | Codec | Termination | Purpose |
|---|---|---|---|
| Control | ARQ | All fragments | Baseline |
| A | LT | FLUID gate | Isolate FLUID |
| B | Nanorq (RaptorQ) | FLUID gate | Isolate codec effect vs. A |

`ε ∈ {0.01, 0.05, 0.10, 0.20}`. Plot repair p99 against bandwidth overhead
for each arm and each class.

## Classes

Configuration is per class, because metadata repair and cold checkpoint
repair have different latency and cost targets:

- `hot-metadata`: `ε = 0.05`, LT arm first.
- `cold-checkpoint`: `ε = 0.15`, Nanorq arm first.

## Rollout gates

| Stage | Entry criteria |
|---|---|
| 10% of one class | ARQ fallback path tested; decoder abort budget bounded |
| 50% | Repair p99 ≥ 25% better, overhead ≤ 1.20, ARQ fallback < 2% |
| 100% | 7 days stable |

## Metrics

- `fluid_repair_duration_seconds{arm,class}`
- `fluid_repair_bytes{sent,useful}`
- `fluid_rounds{arm}`
- `fluid_arq_fallback_total`
- `fluid_eps{class}`
- `fluid_decode_rank`
- `fluid_codec{lt,nanorq}`

## Configuration

See `configs/<env>/fluid.toml` (strict TOML per
[ADR-0005](../adr/0005-config-format-toml.md)): `eps_by_class`,
`rollout_pct_by_class`, `codec_by_class`, `arq_fallback`,
`overhead_abort_ratio`, `arms`.

## Failure modes

| Failure | Response |
|---|---|
| Gate fires before decode | ARQ fallback; increment `fluid_arq_fallback_total` |
| Decoder CPU blow-up on pathological ESI stream | Per-symbol work budget; abort to ARQ |
| Overhead ratio exceeds limit | Abort FLUID for the job class; alert |
| Nanorq object used concurrently | One codec object per worker/job; detects misuse in debug builds |

## Open questions

- The fountain encoder/decoder are opaque types with caller-managed storage;
  a size/allocation function must be added before the ABI is frozen.
- Define whether `overhead_pct` is a target or a hard cap.
- Specify how missing round telemetry affects `loss_prod`.

## Import delta (wack_a_mole)

What W3 imports versus implements, per
[07 §3.2](07-wam-import-map.md#32-fluid-prototype-2):

| Mechanism | Status | Source / note |
|---|---|---|
| Sender budget N = ceil(K/(1−ε)) | **import** | WAM phase O (`include/wam/fluid.h`): budget with near-integer snap, admit/report state, count feedback; O.1 bound sweep comes with it |
| Coordinator loss-product gate | **import math, keep `fg_*` ABI** | same paper identity π = L_ℓ/N (§4.3); the sdf gate stays the design above |
| RaptorQ arm (`fe_codec_nanorq`) | **import** | WAM `src/codec/codec.c` block handling over pinned nanorq (07 §7 pinning note) |
| LT reference arm (`fe_codec_lt`) | **fresh code** | no WAM LT exists — the documented import-first exception (plan W3.3) |
| Loss-ladder + soak e2e | **pattern** | WAM `test_fluid_ladder_e2e` / `test_fluid_soak_e2e` shapes over loopback origin knobs |

The open questions above close as follows: `fe_sizeof`/`fd_sizeof` land
in W3.1 (before any codec arm ships); `overhead_pct` is a **cap** feeding
`overhead_abort_ratio` (decision in W3.5); missing round telemetry
leaves `loss_prod` unchanged for that round (conservative, decided with
the O.2 count-feedback import in W3.4).
