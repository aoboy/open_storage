# Prototype 1 — Whack-a-Mole Dispatch

**Status:** C dispatch core implemented (compile, select, mark down/up);
SHM publication and proxy integration are stubs. Python controller is
implemented.
**Related:** [Overview §6.1, §7.3](00-overview.md#6-prototype-sequence-roirisk-order),
[ADR-0001](../adr/0001-c-abi-as-contract.md).

## Objective

Replace static hashing/round-robin for cache-fill striping and repair
dispatch with a deterministic, weighted path profile that responds to
degradation without hard circuit-breaking.

Targets may be origins, CDN edges, peers, disks, or regions. The first
deployment is the cache-fill proxy; no storage-format or replication-protocol
change is required.

## Model

A profile is a discrete allocation of `m` selection units over `n` paths:

```text
b[i] / m = p[i]                  target fraction for path i
sum(b[i]) = m                    exact allocation
m is a power of two              selection is a mask, not a modulo
```

`m = 1024` supports fine-grained weights while keeping the table small. `n` is
capped at `WM_MAX_PATHS` (64).

### Compile

`wm_profile_compile()` validates the profile and builds the lookup table:

1. Validate `m` (power of two, `2 <= m <= WM_MAX_UNITS`), `n <= WM_MAX_PATHS`,
   and `sum(b) == m`.
2. Build the cumulative distribution `cdf[]`.
3. Build the bit-reversal permutation `perm[j] = bit_reverse(j, log2(m))`.
4. Map each permuted index into its path by CDF:
   `unit_to_path[j] = path_for(perm[j])`.

### Select

```c
uint32_t wm_select(const wm_compiled_t *c, uint64_t counter) {
    return c->unit_to_path[counter & (c->m - 1)];
}
```

One load and one AND. The bit-reversal ordering bounds per-path discrepancy
over contiguous counter ranges, so short transfers are balanced too.

### Degradation

`wm_mark_down()` and `wm_mark_up()` are the C fast path for hard failures.
They must not wait for the 4 Hz controller. The Python controller handles
gradual degradation:

- Score each path from throughput, latency, and error rate.
- Multiplicative decrease on paths below the reference score.
- Additive increase on healthy paths.
- Renormalize to exactly `m` units.
- Publish a new profile with an incremented `version`.

## Control-plane loop

Reference implementation:
[`python/sdf/control/wm_control.py`](../../python/sdf/control/wm_control.py).

At 4 Hz:

```text
read telemetry -> score paths -> update units -> renormalize
              -> write inactive SHM buffer -> flip active_buf
```

`PathTelemetry.scores()` uses:

```text
throughput = acked / sent
score      = throughput / (1 + latency_us + error_rate)
```

Open question: the reference controller is intentionally simple. The
production scorer should use the same EWMA windows as the proxy and should
treat missing telemetry as "no opinion," not as zero throughput.

## A/B assignment

Deterministic bucket over the request key:

```text
bucket = xxhash64(key) % 100
```

The same key must land in the same arm for the life of the experiment.
Assignment percentages come from `configs/<env>/wm.yaml`.

## Rollout gates

| Stage | Entry criteria |
|---|---|
| 5% | Build green, golden vectors pass, no error regression in lab |
| 25% | Cache-fill p99 ≥ 10% better, completion-time variance ≥ 20% better, bytes neutral, no error regression |
| 100% | 72 h stable at 25%, no pager-worthy events |

Any error-rate regression, byte increase, or p99 regression stops the rollout.

## Metrics

- `wm_dispatch_total{path,arm}`
- `wm_discrepancy{path}`
- `wm_fill_duration_seconds{arm}`
- `wm_fill_bytes_total`
- `wm_shed_events_total{path}`

## Configuration

See `configs/<env>/wm.yaml`: `m`, `control_hz`, `alpha`, `beta`,
`rollout_pct`, `kill_switch`.

## Open questions

- Persist `version` per in-flight fill so a profile flip cannot mix two
  allocations within one object.
- Define the fallback distribution when every path is marked down. The current
  C core refuses to mark the last healthy path down and falls back to the
  original profile if a mask somehow covers all paths.
- Decide how the Python controller observes `down_mask` so it does not
  immediately undo a C-side hard-failure decision.
