# Observability, Config, and CI

**Status:** W6.14 schema, executable metrics, dashboards, and alert rules implemented; local gate green.
**Related:** [Overview §11](00-overview.md#11-observability-config-ci).

## Principles

- Every success criterion has a metric emitted by the component that owns it.
- No high-cardinality labels on the hot path. Path IDs, classes, and arms are
  bounded sets.
- Control-plane metrics are exported by Python; data-plane metrics are
  lock-free counters and log-linear histograms.
- Alerts page only on user-visible symptoms, not on internal churn.
- A dashboard never implies a signal that the exporter does not own. External
  engine inputs have a separate, explicit contract.

## Naming and schema contract

All locally emitted series use the `sdf_` prefix. The historical names in the
design tables below are reconciled to the actual scrape names in this section.
Existing `sdf_proxy_*`, `sdf_arc_D_ns`, `sdf_arc_E_ns`, and the existing
`sdf_arc_shadow_*` series remain compatibility aliases; they are not removed
by this schema change.

The Python control plane owns `sdf_wm_control_*`, `sdf_arc_deploy_*`, and
`sdf_control_loop_missed_intervals`. The C data plane owns the WM, FLUID, and
ARC families listed below.

### Prometheus histogram contract

Every owned latency or batch metric is one Prometheus cumulative histogram
family. Its exposition contains `_bucket`, `_sum`, and `_count` samples, with
`le="+Inf"` present exactly once per label set.

The C histogram has 32 storage buckets:

| Storage bucket | Inclusive values | Exported finite `le` |
|---|---|---|
| 0 | 0..1 | 1 |
| i, 1..30 | 2^i..2^(i+1)-1 | 2^(i+1)-1 |
| 31 | 2^31 and above | `+Inf` only |

The finite bucket samples are cumulative and monotonically non-decreasing.
`le="+Inf"` equals the exact observation count. `_count` is exact. `_sum` is
the exact integer sum scaled into the exported unit: nanoseconds for the ARC
histograms and seconds for WM/FLUID latency (`1e-9` scale). Tests compare
scaled sums within their numeric tolerance.

### Bounded labels

| Label | Domain | Owner |
|---|---|---|
| `path`, `origin` | Decimal path ID 0..63 | WM |
| `arm` | `control`, `treatment` (WM); `control`, `lt`, `nanorq` (FLUID) | WM/FLUID |
| `class` | `default` in this release | FLUID |
| `kind` | `sent`, `useful` | FLUID |
| `codec` | `arq`, `lt`, `nanorq` | FLUID |
| `reason` | `ring_full`, `invalid_input`, `engine` | ARC |
| `mode` on `sdf_arc_deploy_mode` | `shadow`, `live`, `killed` | Python |
| `mode` on engine series | `bypass`, `arc` | external engine exporter |
| `pipeline` | Deployment-configured bounded pipeline names | external engine exporter |
| `loop` | `wm`, `fluid`, `arc` | Python liveness |

Object IDs, byte offsets, URLs, file paths, and unbounded error strings are
never labels.

## Metric inventory

### Whack-a-Mole

| Actual series | Type | Meaning |
|---|---|---|
| `sdf_wm_dispatch_total{path,arm}` | counter | Units dispatched per path and arm |
| `sdf_wm_discrepancy{path}` | gauge | Actual minus target allocation, in units |
| `sdf_wm_fill_duration_seconds{arm}` | histogram | Dispatch-to-completion fill latency |
| `sdf_wm_fill_bytes_total{arm}` | counter | Bytes fetched; must stay neutral |
| `sdf_wm_shed_events_total{path}` | counter | Soft-shedding decisions |
| `sdf_wm_enolink_total{path}` | counter | Submissions refused by a drained origin |
| `sdf_wm_kill_switch` | gauge | 1 when the proxy forces the control arm |

### FLUID

| Actual series | Type | Meaning |
|---|---|---|
| `sdf_fluid_repair_duration_seconds{arm,class}` | histogram | Repair completion latency |
| `sdf_fluid_repair_round_size{arm,class}` | histogram | Symbols admitted per non-empty round |
| `sdf_fluid_repair_bytes_total{arm,class,kind}` | counter | Bytes sent and useful bytes received |
| `sdf_fluid_rounds_total{arm,class}` | counter | Coordinator admit rounds |
| `sdf_fluid_arq_fallback_total{arm,class}` | counter | Gate fired before decode sufficiency |
| `sdf_fluid_eps{arm,class}` | gauge | Active slack parameter |
| `sdf_fluid_decode_rank{arm,class}` | gauge | Rank at completion |
| `sdf_fluid_codec{codec}` | gauge | Active codec identity (1) |
| `sdf_fluid_repair_ok{arm,class}` | gauge | Byte-exact repair result (1/0) |

### ARC

| Actual series | Type | Meaning |
|---|---|---|
| `sdf_arc_D_ns`, `sdf_arc_E_ns` | gauge | Adaptive offset and quantized offset |
| `sdf_arc_heap_depth` | gauge | Queued entries awaiting release |
| `sdf_arc_release_batch_size` | histogram | Entries per apply batch |
| `sdf_arc_live_added_latency_ns` | histogram | Added latency on the live apply path |
| `sdf_arc_shadow_added_latency_ns` | histogram | Added latency of the non-applying shadow schedule |
| `sdf_arc_accepted_total` | counter | Entries accepted into the sidecar's input ring |
| `sdf_arc_caller_refused_total{reason="ring_full"}` | counter | Caller retains ownership after a full ring |
| `sdf_arc_dropped_total{reason}` | counter | Invalid input or engine rejection after acceptance |
| `sdf_arc_kill_switch` | gauge | 1 in the killed/oracle mode |
| `sdf_arc_releases_total` | counter | Pacer releases |
| `sdf_arc_guard_waits_total` | counter | Ordered-release guard waits |

True shadow (W6.10) publishes separate `sdf_arc_shadow_*` series for the
non-applying QG schedule releases/samples, immediate shadow applies,
scheduled backlog, epoch, record count, and schedule added-latency
percentiles. The shadow schedule and immediate paths are distinct evidence
and must not be summed as one apply count; promotion uses the shadow epoch
plus the non-vacuity checks in design 03.

The W6.9 accepted-entry contract is explicit: an entry accepted by the input
ring is retained if the pacer refuses it. Such internal backpressure is not
a drop and does not increment `sdf_arc_dropped_total`. Only an invalid input
rejected before acceptance (`reason="invalid_input"`) or an engine rejection
of an already accepted entry (`reason="engine"`) is a drop.

### Python control plane

| Actual series | Type | Meaning |
|---|---|---|
| `sdf_wm_control_steps_total` | counter | Completed WM control intervals |
| `sdf_wm_control_version` | gauge | Last published profile version |
| `sdf_wm_control_units{path}` | gauge | Published units per bounded path |
| `sdf_wm_control_fresh_paths` | gauge | Paths with fresh telemetry |
| `sdf_wm_control_down_mask` | gauge | C fast-path down mask |
| `sdf_arc_deploy_steps_total` | counter | Completed ARC deployment intervals |
| `sdf_arc_deploy_mode{mode}` | gauge | Exactly one active ARC deployment mode |
| `sdf_arc_shadow_elapsed_seconds` | gauge | Current shadow epoch elapsed time |
| `sdf_control_loop_missed_intervals{loop}` | gauge | Whole heartbeat intervals missed |

`SdfMetrics` owns a private Prometheus registry, so independent controllers
and tests cannot collide on metric registration. `LivenessMonitor` samples
whole missed intervals from monotonic time: warning at two, page at ten.

### External engine input contract

The ARC sidecar does not own storage-engine apply latency or leader
backpressure. The external exporter contract requires:

| Series | Type | Required labels | Unit/semantics |
|---|---|---|---|
| `engine_apply_p99_seconds` | gauge | `pipeline`, `mode` | Latest engine apply p99 in seconds |
| `engine_backpressure_events_total` | counter | `pipeline`, `mode` | Monotonic cumulative backpressure events |

`mode` is `bypass` or `arc`; `pipeline` comes from the deployment's bounded
configured set. The checked-in parser/query fixture is
`docs/ops/fixtures/engine.prom`. See
[`docs/ops/external-engine-metrics.md`](../ops/external-engine-metrics.md).
An absent external exporter leaves those panels empty; it is not a zero
measurement and never substitutes for locally owned evidence.

## SLOs and alert thresholds

| Signal | Warning | Page |
|---|---|---|
| Cache-fill p99 vs. 7-day median | +5% | +10% |
| Repair p99 vs. ARQ baseline | +0% | +10% |
| FLUID overhead ratio | > 1.20 | > 1.50 |
| FLUID ARQ fallback rate | > 2% | > 5% |
| ARC live added latency p99 | > 10 ms | > 25 ms |
| `sdf_arc_dropped_total` | any increase | any increase |
| Control-loop liveness | 2 missed intervals | 10 missed intervals |

`python/sdf/common/alerts.py` is the executable source for these thresholds.
Regenerate `docs/ops/alerts/sdf-alerts.yml` from the storage-dataflow root with
`PYTHONPATH=python python3 -m sdf.common.alerts`; threshold tests cover every
warning/page boundary, including an ARC drop. Regression and latency
boundaries use strict `>`; missed-interval boundaries use `>=`.

## Dashboards

`docs/ops/dashboards/{wm,fluid,arc}.json` are importable Grafana JSON with
stable UIDs `sdf-wm`, `sdf-fluid`, and `sdf-arc`. They show:

- The experiment arm split and rollout traffic.
- The primary success metric and its baseline.
- Guardrail metrics (bytes, errors, fallback rate, added latency).
- Kill-switch state where the component owns it.
- Separate ARC shadow-schedule and live/immediate apply behavior.

Local tests resolve every query's metric references against C-generated scrape
bodies and the checked-in external engine fixture. This is local query
validation, not a hosted Grafana import and not hosted evidence.

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
7. Observability validation: C integration tests write live scrape fixtures,
   then Python validates dashboard queries, alert inputs, histogram
   invariants, and bounded labels.

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
