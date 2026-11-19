# Observability, Config, and CI

**Status:** Metric inventory and gates defined; dashboards are placeholders.
**Related:** [Overview §11](00-overview.md#11-observability-config-ci).

## Principles

- Every success criterion has a metric emitted by the component that owns it.
- No high-cardinality labels on the hot path. Path IDs, classes, and arms are
  bounded sets.
- Control-plane metrics are exported by Python; data-plane metrics are
  lock-free counters and log-linear histograms read through shared memory.
- Alerts page only on user-visible symptoms, not on internal churn.

## Metric inventory

### Whack-a-Mole

| Metric | Type | Meaning |
|---|---|---|
| `wm_dispatch_total{path,arm}` | counter | Units dispatched per path and arm |
| `wm_discrepancy{path}` | gauge | Actual minus target allocation, in units |
| `wm_fill_duration_seconds{arm}` | histogram | End-to-end fill latency |
| `wm_fill_bytes_total` | counter | Bytes fetched; must stay neutral |
| `wm_shed_events_total{path}` | counter | Soft-shedding decisions |

### FLUID

| Metric | Type | Meaning |
|---|---|---|
| `fluid_repair_duration_seconds{arm,class}` | histogram | Repair completion latency |
| `fluid_repair_bytes{sent,useful}` | counter | Bandwidth overhead |
| `fluid_rounds{arm}` | histogram | Rounds per repair |
| `fluid_arq_fallback_total` | counter | Gate fired before decode sufficiency |
| `fluid_eps{class}` | gauge | Active slack parameter |
| `fluid_decode_rank` | histogram | Rank at completion |
| `fluid_codec{lt,nanorq}` | counter | Symbols generated/decoded per codec |

### ARC

| Metric | Type | Meaning |
|---|---|---|
| `arc_offset_ns` | gauge | Current release offset |
| `arc_heap_depth` | gauge | Queued entries awaiting release |
| `arc_release_batch_size` | histogram | Entries per apply batch |
| `arc_added_latency_ns` | histogram | Pacer-added delay |
| `arc_dropped_total` | counter | Entries dropped; must be zero |
| `engine_apply_p99_seconds` | gauge | Engine apply tail latency |
| `engine_backpressure_events_total` | counter | Leader backpressure events |

## SLOs and alert thresholds

| Signal | Warning | Page |
|---|---|---|
| Cache-fill p99 vs. 7-day median | +5% | +10% |
| Repair p99 vs. ARQ baseline | +0% | +10% |
| FLUID overhead ratio | > 1.20 | > 1.50 |
| FLUID ARQ fallback rate | > 2% | > 5% |
| ARC added latency p99 | > 10 ms | > 25 ms |
| `arc_dropped_total` | any increase | any increase |
| Control-loop liveness | 2 missed intervals | 10 missed intervals |

## Dashboards

`docs/ops/dashboards/{wm,fluid,arc}.json` contain placeholder Grafana
definitions. Each dashboard should show:

- The experiment arm split and rollout percentage.
- The primary success metric and its baseline.
- The guardrail metrics (bytes, errors, fallback rate, added latency).
- A kill-switch state panel.

## Configuration

Environment configs live in `configs/{dev,staging,prod}/{wm,fluid,arc}.yaml`.
The Python control plane reads YAML; the C data plane reads a flat resolved
view (see `include/sdf/common/config.h`). Production rollout percentages stay
at zero until the relevant gate is signed off.

## CI gates

`ci.yml`:

1. Build with `-Wall -Wextra -Werror`.
2. C unit tests and the ABI smoke test.
3. Fuzz smoke, 60 s per target (once fuzz targets are implemented).
4. Python unit tests, ruff, mypy.
5. Integration via docker-compose (once system tests are implemented).
6. CFFI build and golden-vector conformance.

`bench.yml` (nightly): fail if p99 regresses more than 5% against the 7-day
rolling median. `docs.yml`: build the PDF and fail on broken links.

`golden_vectors/` is an ABI contract. Changing a vector requires an ADR and a
version bump.

## Logging

- Data plane: rate-limited, structured key/value lines; no allocation on the
  hot path.
- Control plane: Python `logging` with a stable format, request/job IDs where
  available.
- Never log object contents, keys that may contain user data, or full paths in
  production.

## Open work

- Replace dashboard placeholders with exported Grafana JSON.
- Define the fuzz corpus and seed inputs.
- Wire the CFFI golden-vector check into `run_tests.sh`.


## Embedding notes (plan prompt W5.2)

The install surface (`pkg-config sdf_*` / `find_package(sdf)`) exposes
static libraries; the embedder's thread and allocation contract:

- **Thread contract.** `sdf_common` primitives are single-producer /
  single-consumer (`ring.h`) or single-writer (`seqlock.h`, the ADR-0002
  publication). The ARC pacer is single-threaded by design: submit and
  poll from one thread (the sidecar's pump). The origin drivers accept
  fetches from one dispatcher thread and complete on their own worker —
  callbacks run on the driver's completion context, never block the
  dispatcher (ADR-0008's head-of-line rule).
- **No allocation after warmup** on the data path: pool/ring/slab carry
  the hot path; the fountain codecs allocate at init and free in
  destroy (the v2 lifecycle pair exists exactly for that); the loopback
  driver is zero-copy via mmap. The HTTP driver is the reference smoke
  leg and does allocate per unit (risk S8, documented).
- **Version.** `include/sdf/version.h` is the single source of truth
  (CMake parses it for the pkg-config and find_package versions); the
  on-wire/ABI contracts are separate and move only with their ADRs.
- **Linking.** One `.pc` per link granularity; everything vendored
  builds from source in-tree, so static linking carries no hidden
  `--static` dependency surprises.
