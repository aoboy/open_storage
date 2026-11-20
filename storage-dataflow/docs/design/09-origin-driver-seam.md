# 09 — Origin Driver Seam (`sdf_origin_ops_t`)

**Status:** **As-built (Wave 1)** — header + ABI pins (W1.1), loopback
harness driver with live degradation knobs (W1.2), HTTP reference driver
(W1.3), and the origin_loadgen harness with the four canonical
degradation profiles (W1.4); 12/12 ctest with ASan+UBSan and TSan green.
**Related:** [ADR-0008](../adr/0008-origin-driver-seam.md) (the decision),
[00-overview §5.4](00-overview.md#54-origin-driver-seam-new),
[01-whack-a-mole](01-whack-a-mole.md) (the consumer),
[07-import-map §4](07-wam-import-map.md) (why the UDP/RoCE drivers are not
imported).

---

## 1. Objective

In this system, Whack-a-Mole's "paths" are **origins**: S3/blob stores,
CDN edges, P2P peers, colocated clusters, local NVMe. The dispatcher needs
one uniform seam to (a) issue fetch units against any origin class,
(b) read normalized per-origin telemetry for the severity controller and
the Python loop, and (c) drain/restore origins without hard
circuit-breaking. This is the storage analogue of wack_a_mole's
`wam_transport_ops_t` vtable + `wam_path_stats_t` normalization, and it is
imported as a **pattern** (ADR-0008): the drivers themselves are
storage-specific and written fresh.

## 2. The vtable

`include/sdf/origin.h` (lands with W1.1):

```c
typedef struct sdf_origin sdf_origin_t;      /* driver instance */

/* One unit of work: a byte-range request of a fill/repair job.
 * The dispatcher assigns units to origins via wm_select; the origin_id
 * in the request must equal the dispatcher's pick. */
typedef struct {
    uint32_t origin_id;      /* 0..n-1, matches the profile path index */
    uint64_t job_id;        /* fill or repair job */
    uint64_t obj_offset;    /* byte range [obj_offset, obj_offset+len) */
    uint32_t len;
    uint32_t flags;         /* SDF_FETCH_* (repair symbols, priority) */
} sdf_fetch_req_t;

/* Completion. Called once per request, on the driver's completion
 * context, with the data in *out (driver-owned until the callback
 * returns). status < 0 => unit failed (feeds error-rate scoring). */
typedef void (*sdf_fetch_done_fn)(void *ud, const sdf_fetch_req_t *req,
                                 const uint8_t *out, uint32_t len,
                                 int status);

typedef struct {
    int  (*open)(sdf_origin_t *o, const sdf_origin_cfg_t *cfgs, uint32_t n);
    int  (*fetch)(sdf_origin_t *o, const sdf_fetch_req_t *req,
                  sdf_fetch_done_fn cb, void *ud);
    int  (*stats)(sdf_origin_t *o, uint32_t origin_id, sdf_origin_stats_t *st);
    int  (*set_enable)(sdf_origin_t *o, uint32_t origin_id, int up);
    void (*close)(sdf_origin_t *o);
} sdf_origin_ops_t;

struct sdf_origin { const sdf_origin_ops_t *ops; void *impl; };
```

Rules:

- **One unit = one fetch.** The proxy never multiplexes a unit across
  origins; re-assignment is a controller decision, visible as a new unit.
- **Completion is a callback**, never a blocking return: a stalled origin
  must not head-of-line-block the dispatch loop. The loopback driver
  keeps this honest from day one (W1.2).
- **Link rule** (ADR-0008): dispatch, controller, FLUID, ARC, and the
  Python plane include only `origin.h`; drivers never leak their types.
  `hard_down` in stats is the only boolean escape hatch, consumed by the
  C fast path (`wm_mark_down`).

## 3. Normalized stats

`sdf_origin_stats_t` — one struct shape for every driver, the storage
analogue of `wam_path_stats_t`:

```c
typedef struct {
    uint32_t id;
    uint32_t cls;             /* SDF_ORIGIN_{LOOPBACK,HTTP,S3,PEER} */
    uint64_t reqs_out, reqs_done, reqs_failed;
    uint64_t bytes_tx, bytes_rx;
    uint64_t in_flight;
    double   ewma_lat_ns;     /* per-request completion latency */
    double   ewma_err;        /* failed/done over the scoring window */
    double   goodput;         /* bytes_rx / window, controller-scaled */
    int      hard_down;       /* driver-detected (conn refused, DNS) */
} sdf_origin_stats_t;
```

The Python scorer (`wm_control.py::PathTelemetry`) consumes exactly
`reqs_out/done/failed`, `ewma_lat_ns`, `ewma_err` — the reference
implementation is already shape-compatible; missing telemetry reads as
"no opinion", not zero throughput (design note in 01 §Control-plane loop).

## 4. Drivers

| Driver | Class | Purpose | Knobs (test harness) |
|---|---|---|---|
| `src/c/origin/loopback/` | `LOOPBACK` | serves byte ranges from local files; the e2e workhorse | per-origin `throttle_ns` (bandwidth), `loss_pct` (drop units), `err_pct` (fail units), `lat_jitter_ns`; live-reloadable mid-run — the **netem analogue** |
| `src/c/origin/http/` | `HTTP` | POSIX-socket HTTP/1.1 `Range` GET; `SDF_HAVE_CURL` optional path | none (real network) |
| `src/c/origin/s3/` | `S3` | blob store driver (multipart-aware ranges) | defer — behind the same ops, needs its own design note |
| `src/c/origin/peer/` | `PEER` | NVMe peer / colocated cluster | defer |

The loopback driver is a first-class test asset, not a mock: its injection
knobs drive the discrepancy/skew e2e gates (W2.6) and the repair loss
ladder (W3.6), replacing wack_a_mole's `netns_topology.sh` + `tc netem`
stack, which has no storage-domain equivalent.

## 5. Dispatch integration (Prototype 1)

```text
fill job (object, byte ranges)
   │
   ▼
[proxy main: wm_compiled_t snapshot (SHM publish, ADR-0002)]
   │  unit k: origin = wm_select(c, counter++)        (ADR-0007 seeds)
   ▼
sdf_origin->ops->fetch(req, done_cb)  ──► loopback | http | s3 | peer
   │                                       │
   │◄──────── completion(status) ──────────┘
   ▼
per-origin tally → telemetry ring (SHM) → Python 4 Hz loop
                                          → new profile → flip active_buf
C fast path on hard_down: wm_mark_down(path) — no Python round trip
```

The proxy owns: unit counters (monotonic, per job), the completion ring,
A/B bucketing (`xxhash64(key) % 100`, per design 01), and the metric set
(`wm_dispatch_total{path,arm}`, `wm_discrepancy{path}`, … per 01
§Metrics).

## 6. Repair integration (Prototype 2)

The repair coordinator reuses the same seam: fountain symbols
(W3.2/W3.3) are units fetched across origins; the FLUID gate observes
per-round sent/recv on `stats()` deltas; ARQ fallback re-fetches exact
ranges when the decode-sufficiency gate fires early (design 02
§Mechanism). `wm_select` assignment is shared with P1 so repair traffic
inherits live origin health.

## 7. What this seam is not

- **Not a packet transport**: no ECN, no path MTU, no per-packet feedback;
  the unit granularity is a byte range and the signal granularity is a
  completed (or failed) request.
- **Not a cache**: the seam moves bytes; placement policy is the storage
  engine's job above the apply callback (P3's ARC sidecar is the last
  hop, not an origin).
- **Not a control-plane RPC**: origin enable/disable is a driver op
  (`set_enable`) mirroring `wm_mark_up/down`, distinct from the Python
  loop's profile authority.

## 8. Test strategy

| Level | What pins the seam |
|---|---|
| Unit (W1.2) | loopback stats exactness under throttle/loss/err knobs; knob live-reload mid-run |
| Unit (W2.x) | bridge parity: same profile+seeds ⇒ same unit→origin sequence as the golden vectors |
| Integration (W2.5/W2.6) | mixed-driver proxy: per-origin dispatch counts within discrepancy bound; degradation mid-run redistributes within one control interval |
| e2e (W3.6) | repair loss ladder over loopback origins; ARQ fallback < 2% at the 50% rollout point |

## 9. Open questions

- `s3` driver authorization model (instance roles vs signed URLs) — needs
  its own design note when scheduled; the ops surface (`[[origin]]`
  TOML blocks, ADR-0005 schema) is drafted in the W1.1 prompt.
- Whether `set_enable` needs a drain timeout knob (WAM's `drain_path`
  semantics) — decide at W2.2 when the controller import lands.
- **`SDF_HAVE_CURL` (W6.15 decision): not admitted for now.** The POSIX
  driver remains the only HTTP implementation, so libcurl stays out of the
  default and optional builds. `bench_origin` records the POSIX arm's p99,
  goodput, connection reuse, allocation, and saturation behavior; a curl A/B
  becomes worth the dependency only when a named-runner baseline exists and
  the TLS development surface is available. If admitted later, it must build
  OFF by default behind the same vtable and keep POSIX as the fallback.
- **HTTPS/TLS: deferred, fail-closed.** `https://` destinations are rejected
  at open rather than downgraded to HTTP. Certificate/hostname verification,
  explicit CA configuration, and certificate-failure tests need a TLS
  development dependency that was not present in the verification
  environment; no TLS evidence is claimed until that leg runs.
