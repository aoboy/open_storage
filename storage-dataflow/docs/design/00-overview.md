# Storage Dataflow Stack

## Applying Whack-a-Mole, FLUID, and ARC to Large-Scale Distributed Storage & Caching

**Document type:** Internal planning & design document
**Author:** Senior R&D Technical Staff
**Status:** Design baseline — import-led execution phase. The executable
backlog is [`08-implementation-plan.md`](08-implementation-plan.md);
provenance from the wack_a_mole implementation lives in
[`07-wam-import-map.md`](07-wam-import-map.md).
**Version:** 1.1

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Source Papers](#2-source-papers)
3. [Layer Separation & Interaction Model](#3-layer-separation--interaction-model)
4. [Application to Distributed Storage / Caching](#4-application-to-distributed-storage--caching)
5. [System Architecture](#5-system-architecture)
6. [Prototype Sequence (ROI/Risk Order)](#6-prototype-sequence-roirisk-order)
7. [Implementation Break-Down](#7-implementation-break-down)
8. [Project Folder Structure](#8-project-folder-structure)
9. [Language Strategy: C, Python, Rust](#9-language-strategy-c-python-rust)
10. [Nanorq Evaluation (RaptorQ vs. LT Codec)](#10-nanorq-evaluation-raptorq-vs-lt-codec)
11. [Observability, Config, CI](#11-observability-config-ci)
12. [Rust Migration Plan](#12-rust-migration-plan)
13. [Milestones & Staffing](#13-milestones--staffing)
14. [Risks & Mitigations](#14-risks--mitigations)
15. [Fit to Modal Storage MTS Role](#15-fit-to-modal-storage-mts-role)
16. [Appendices](#16-appendices)

---

## 1. Executive Summary

The three source papers — **Whack-a-Mole** (arXiv:2509.18519), **FLUID**
(arXiv:2605.23947), and **ARC** (arXiv:2511.16902) — form a coherent stack for
**deterministic, coded, receiver-paced data movement**:

- **Whack-a-Mole** determines *where* each unit goes: a discrete path profile
  with provably bounded discrepancy and graceful congestion response.
- **FLUID** determines *when to stop* sending or recovering: slack-based
  fountain decoding instead of all-or-nothing ARQ.
- **ARC** determines *when to release* at the receiver: timing-preserving
  pacing without feedback.

Applied to a large-scale distributed storage or caching system, this stack
attacks three chronic problems:

1. **Skewed cache-fill and repair traffic** across heterogeneous origins,
   peers, and WAN paths.
2. **Tail latency in cross-region replication and EC repair**, where the
   slowest fragment dominates p99.
3. **Bursty write/apply phases** that spike engine p99 write latency and
   trigger leader backpressure.

The prototype sequence is ordered by ROI/risk:

| Order | Prototype | Scope | Risk |
|---|---|---|---|
| 1 | Profile-based dispatch + soft shedding for cache-fill striping | Proxy data plane | Low |
| 2 | Slack-based fountain-coded EC repair over WAN | Repair pipeline (isolated) | Medium |
| 3 | ARC-style pacing sidecar in one ingest/replication pipeline | Receiver/apply plane | Higher |

The implementation uses **C** for the data-plane hot path, **Python** for the
control plane, orchestration, A/B, and metrics, with **Rust migration seams**
designed in from day one. **Nanorq** (RaptorQ, RFC 6330) is evaluated as a
replacement for a hand-rolled LT codec inside Prototype 2 and is recommended
as one of three A/B arms rather than a wholesale replacement.

---

## 2. Source Papers

### 2.1 Whack-a-Mole (arXiv:2509.18519)

**Mechanism.** Represents a *path profile* as a discrete allocation of `m`
selection units across `n` paths, where `b(i)/m = p(i)` is the target fraction
of packets for path `i`. A **bit-reversal counter** selects a path per unit;
the discrepancy between expected and actual packet counts per path is
**provably bounded** — at most `log2 m` (or `2 log2 m` depending on shuffle
method) over any contiguous packet sequence. The algorithm responds to
congestion feedback by reducing allocation to degraded paths and
redistributing load.

**Key properties for storage:**

- Deterministic, low-overhead, sub-microsecond path decisions (table lookup
  after compile).
- Bounded per-path discrepancy on short transfers — unlike stochastic
  spraying.
- Graceful adaptation when paths degrade or recover.

### 2.2 FLUID (arXiv:2605.23947)

**Mechanism.** Replaces all-or-nothing ARQ with a **controlled slack `ε`**.
Under the **Loss-Product Rule**, delivery finishes once the product of loss
fractions across rounds falls below `ε`, rather than requiring every packet to
arrive. Yields **substantially tighter tail latency** while remaining
**deterministically close to the bandwidth-optimal ARQ bound**.

**Key properties for storage:**

- Converts repair p99 from "max over all source paths" to "quantile over
  decode sufficiency."
- Tunable bandwidth premium via `ε`.
- Can never be worse than ARQ if a fallback is retained.

### 2.3 ARC (arXiv:2511.16902)

**Mechanism.** A **receiver-clock-only** release scheduler that restores the
sender's timing at the receiver, maintaining ordering and limiting reordering
with minimal added latency. **No feedback, no synchronization, no transport
changes.** Sits as an independent module between recovery and application
delivery.

**Key properties for storage:**

- Decouples recovery from apply.
- Prevents bursty apply phases that spike engine p99 and propagate leader
  backpressure.
- Ordering preserved by keying on `(release_time, producer_ts, seq)`.

### 2.4 Nanorq (external library, <https://github.com/ursoid-tech/nanorq>)

**Not a paper**, but a relevant library evaluated for the FLUID repair codec.
Nanorq is a **C implementation of RaptorQ** (RFC 6330), described as a "fast
compact rateless codec" capable of **multi-gigabit speeds on a single core**.
It provides:

- A high-level API (`nanorq.h`) with allocation, alignment, and I/O handling.
- A low-level API (`nanorq_core.h`) that can run **without libc** under
  `NDEBUG`.
- Experimental **Tunable Sparse Network Coding (TSNC)** helpers for multipath
  recoding and streaming.
- Host-CPU autotuning (`-march=native`).

Detailed evaluation in [section 10](#10-nanorq-evaluation-raptorq-vs-lt-codec).

---

## 3. Layer Separation & Interaction Model

Whack-a-Mole, FLUID, ARC, and Nanorq operate at **different layers** and are
**orthogonal**, not substitutes.

| Concern | Whack-a-Mole | FLUID | ARC | Nanorq |
|---|---|---|---|---|
| **Layer** | Path selection / dispatch | Repair termination policy | Receiver release scheduling | Erasure coding |
| **Question answered** | *Which path carries this unit?* | *When can I stop repairing?* | *When should I apply?* | *What symbols should I send?* |
| **Output** | Deterministic unit→path assignment | Finish signal when Loss-Product < ε | Smoothed release stream | Fungible repair symbols |
| **Failure model** | Path degradation, congestion, skew | Partial arrival, packet loss | Bursty arrival, jitter | Erasures, symbol loss |
| **Side** | Sender | Sender + receiver (rounds) | Receiver | Sender encode + receiver decode |
| **Pipeline position** | Before the wire | Repair loop | After recovery, before apply | Inside repair loop |

**The strongest architecture uses all four together:**

```text
[Origin/Peer] --(symbols)--> [Whack-a-Mole dispatch] --(paths)-->
[FLUID slack gate + Nanorq codec] --(recovered entries)-->
[ARC pacer] --(smoothed stream)--> [Storage engine apply]
```

- **Whack-a-Mole** keeps the symbol stream flowing across healthy paths.
- **FLUID + Nanorq** makes the decode gate reachable as soon as the rank
  condition is met.
- **ARC** smooths the release into the engine, preventing apply bursts.

A direct "Nanorq vs. Whack-a-Mole" comparison is a **category error**. They
solve different failure modes and compose cleanly.

---

## 4. Application to Distributed Storage / Caching

### 4.1 Symptom → Mechanism Mapping

| Paper / Library | Storage symptom | Root cause |
|---|---|---|
| **Whack-a-Mole** | Cache-fill striping across nodes/disks/regions is skewed; some targets saturate while others idle; repair traffic repeatedly hammers degraded paths | Static ECMP/round-robin over heterogeneous targets ignores capacity, latency, transient degradation |
| **FLUID** | Cross-region EC repair or replica catch-up has a long tail: p99 dominated by the slowest chunk even when the block is decodable | All-or-nothing ARQ semantics; strict "wait for every fragment" |
| **ARC** | Replication apply or edge-ingest write path is bursty; follower engine p99 spikes and leader backpressure events increase under jitter | Recovery dynamics distort arrival timing; apply fires as soon as data is available, not when engine can absorb it |
| **Nanorq** | LT codec at practical block sizes has non-trivial reception overhead and a rank-deficiency tail | LT codes are not near-zero-overhead; RaptorQ is designed for it |

### 4.2 Concrete Application Patterns

**Whack-a-Mole for cache-fill striping and repair dispatch.** In a Modal-like
storage layer — hundreds of petabytes replicated across multiple cloud object
stores, cached on local NVMe, shared P2P within each datacenter — a cache-fill
operation fetches a large object (container image layer, model weight shard,
checkpoint) across several origins: S3, a CDN edge, a peer worker, a colocated
storage cluster. Today, hashing or round-robin may send a disproportionate
share to a slow or saturated origin. A Whack-a-Mole-style path profile over
these origins (weighted by observed throughput, ECN-like signals, and loss)
with bit-reversal deterministic selection gives tight per-object balance and
immediate redistribution when an origin degrades. Because the discrepancy is
bounded over any contiguous packet sequence, even small objects get close to
the target profile — unlike stochastic spraying, which can be badly skewed on
short transfers.

**FLUID for slack-based EC repair over WAN.** Cross-region erasure-coded
repair is a natural FLUID fit. Today, a repair job may wait for every fragment
from every source region, so a single slow WAN path sets p99. With
fountain-coded repair fragments (generated by Nanorq or an LT codec) and a
slack parameter `ε`, the repair coordinator finishes as soon as enough
distinct fragments are available to decode, rather than waiting for all. The
overhead is bounded: FLUID remains deterministically close to ARQ bandwidth
optimality. For a storage system, this converts repair p99 from "max over all
source paths" to "quantile over decode sufficiency," at a tunable bandwidth
premium.

**ARC for replication/apply pacing.** In follower apply or edge-cache ingest,
the engine sees bursty writes because replication fragments or ingest batches
arrive irregularly. An ARC-style receiver-side pacer sits between the
recovered data queue and the engine's apply path. It maintains an adaptive
offset tracking the upper envelope of observed recovery times and releases
data in a smoothed stream that follows the sender's original timing. No
feedback to the leader is required; no transport changes. The measurable
outcome is reduced engine p99 write latency and fewer leader backpressure
events, because the apply phase no longer oscillates between starvation and
burst.

**Nanorq for fungible repair symbols.** Inside the FLUID repair loop, Nanorq
generates RaptorQ symbols that are maximally fungible: any `K + small
overhead` distinct symbols decode with high probability. This makes the
decode-sufficiency gate cleaner than with LT codes and lowers the overhead
axis of the Pareto curve. The RFC 6330 format also provides an interop surface
for future Rust repair agents, hardware accelerators, or third-party object
stores.

---

## 5. System Architecture

### 5.1 Three-Plane Architecture

```text
SENDER / SOURCE PLANE
  Origin A (S3/blob) ---+
  Origin B (CDN edge) --+--> Whack-a-Mole dispatch (profile + bit-reversal
  P2P peer/colocated ---+    counter + soft shedding)
                              |
                              | packets / coded fragments
                              v
WAN / INTERCONNECT PLANE
  Path 1 (healthy) ---+
  Path 2 (degraded) --+--> FLUID slack repair
  Path k (healthy) ---+    decode gate: finish when Loss-Product < epsilon
                           codec: Nanorq RaptorQ | LT
                              |
                              | recovered / decoded entries
                              v
RECEIVER / APPLY PLANE
  ARC receiver-side pacing (adaptive release offset; receiver clock only;
  no feedback)
                              |
                              | smoothed stream
                              v
  Storage engine / apply path (local NVMe cache, replication apply, WAL replay)
```

### 5.2 Three-Plane Architecture Block Diagram

The following block diagram expands §5.1 into the payload, control, and
telemetry paths. The Python control plane configures and observes the C data
plane but never sits inline with payload movement. Codec and future language
implementations remain behind the stable `include/sdf/` C ABI.

```text
                       CACHE FILL / EC REPAIR / REPLICATION INGEST
                                           |
                                           v
+================================================================================+
| SENDER / SOURCE PLANE                                                          |
|                                                                                |
|  [Job + byte-range planner]                                                    |
|               |                                                                |
|               v                                                                |
|  [Repair-only fountain encoder: Nanorq RaptorQ | C LT | opt-in Rust LT]        |
|               | source units / repair symbols                                  |
|               v                                                                |
|  [Whack-a-Mole dispatch: compiled profile + bit-reversal + soft shedding]      |
|               | unit -> origin assignment                                      |
|               v                                                                |
|  [Origin I/O seam: sdf_origin_ops_t; nonblocking fetch + completion callback]  |
|               |                                                                |
|       +-------+----------+-------------+---------------+                       |
|       | Loopback harness | HTTP / CDN  | S3 / blob (gated)| NVMe peer (gated)  |
|       +-------+----------+-------------+---------------+                       |
+===============+================================================================+
                | byte-range responses / coded symbols
                v
+================================================================================+
| WAN / INTERCONNECT PLANE                                                       |
|                                                                                |
|  [Path 1] [Path 2] ... [Path k]: independent latency, loss, and degradation    |
|               |                                                                |
|               +--> [Completion tally + normalized sdf_origin_stats_t]          |
+===============+================================================================+
                | received source units / repair symbols
                v
+================================================================================+
| RECEIVER / APPLY PLANE                                                         |
|                                                                                |
|  [FLUID repair coordinator]                                                    |
|    [Nanorq / LT decoder] --> [Decode-sufficiency + Loss-Product < epsilon gate]|
|               |                         |                                      |
|               |                         +--> [Exact-range ARQ fallback]        |
|               v recovered entries                                              |
|  [ARC receiver pacer: adaptive offset + ordered release heap]                  |
|    shadow / live / killed modes; receiver clock only; no leader feedback       |
|               | smoothed, ordered apply stream                                 |
|               v                                                                |
|  [Storage engine: local NVMe cache | follower replication apply | WAL replay]  |
+================================================================================+

+================================================================================+
| CROSS-CUTTING CONTROL + OBSERVABILITY PLANE (Python; off the payload hot path) |
|                                                                                |
|  [Strict TOML configs] --> [wm_control | fluid_coord | arc_deploy]             |
|                                  | profiles / epsilon / A-B arms / modes       |
|                                  v                                             |
|                         [SHM seqlock publication] ==> acquire by C components  |
|                                                                                |
|  [C proxy + FLUID + ARC] ..> [SHM telemetry ring + lock-free metrics]          |
|                                  :                                             |
|                                  :                                             |
|                    [4 Hz scoring / A-B analysis / rollout gates]               |
|                                  |                                             |
|                    +-------------+--------------+                              |
|                    v                            v                              |
|             [/metrics endpoint]       [Deploy gate + kill switch]              |
|                    |                            |                              |
|                    v                            +==> ARC / profile control     |
|         [Dashboards + CI/SLO evidence]                                         |
+================================================================================+

  | / --> / v : payload or recovered-data flow
  ..> / :   : telemetry and observation
  ==>       : published configuration or deployment control
```

The control loop consumes normalized origin statistics and pipeline metrics,
then publishes new profiles, repair parameters, and ARC modes. FLUID's
exact-range ARQ fallback closes the decode-safety path before entries enter
ARC; it does not bypass receiver pacing. Hard origin failures still use the C
`wm_mark_down` fast path, without waiting for Python.
S3 and NVMe-peer blocks denote the common origin-driver seam; production
endpoint evidence remains separately gated and is not implied by this diagram.

### 5.3 Component Responsibilities

| Component | Side | Responsibility | Language |
|---|---|---|---|
| `wm_select` | Sender | Deterministic unit→path selection | C |
| `wm_control` | Control | 4 Hz soft-shedding controller writing profiles to SHM | Python |
| `fe_codec` | Sender/Receiver | Fountain encode/decode | C (Nanorq or LT) |
| `fluid_gate` | Repair loop | Loss-Product state machine | C |
| `fluid_coord` | Control | ε scheduling, A/B assignment, Pareto tracking | Python |
| `arc_pacer` | Receiver | Offset estimator + heap + release loop | C |
| `arc_deploy` | Control | Pipeline selection, kill switch, shadow mode | Python |

### 5.4 Origin driver seam (new)

The interconnect "paths" of §5.1 are **origins** — S3 blobs, CDN edges,
peers, colocated clusters. One vtable (`sdf_origin_ops_t`) is the only
origin I/O seam in the data plane: a unit is one byte-range fetch,
completion is a callback (a stalled origin never blocks dispatch), and
every driver exports the same normalized `sdf_origin_stats_t` that the
severity controller and the Python loop score. This is the storage
analogue of wack_a_mole's transport vtable, imported as a pattern — the
UDP/RoCE drivers themselves are deliberately not imported (import map
§4). Full contract: [`09-origin-driver-seam.md`](09-origin-driver-seam.md),
decision record: [ADR-0008](../adr/0008-origin-driver-seam.md).

### 5.5 Provenance and execution

The data-plane cores (spray, FLUID budget, ARC schedule, RaptorQ codec)
are **imported from the wack_a_mole repository** behind the
`include/sdf/*.h` ABI and accepted by golden-vector parity
([ADR-0009](../adr/0009-golden-vector-parity.md)). The asset map, ABI
delta register, and the not-imported list live in
[`07-wam-import-map.md`](07-wam-import-map.md); waves, prompts, sizes,
and milestone gates live in
[`08-implementation-plan.md`](08-implementation-plan.md).

---

## 6. Prototype Sequence (ROI/Risk Order)

### 6.1 Prototype 1 — Profile-based dispatch + soft shedding (weeks 3–6)

**Why first:** Pure proxy change; zero protocol risk; immediate skew fix. You
do not need to touch the storage format, the replication protocol, or the
engine.

**What to build:**

- A **path/origin profile** represented as integer selection units `b(i)/m`,
  where targets are origins, peers, disks, or regions.
- A **bit-reversal counter** per fill/repair job (or per origin-group), with
  path selection in O(1) after compile and bounded discrepancy.
- **Soft shedding**: when a target exhibits degradation signals (elevated
  latency, error rate, queue depth), reduce its allocation units and
  redistribute to healthy targets. "Soft" means gradual weight reduction
  rather than hard circuit-breaking, so recovery is also smooth.
- A **C-side hard-failure fast path** (`wm_mark_down`) so hard failures do not
  wait for the Python control loop.

**Success metrics:**

- Reduction in **cache-fill p99** and **variance of fill completion time
  across objects**.
- **No increase in total bytes fetched** (a dispatch-only change should be
  bandwidth-neutral).
- Secondary: reduction in per-origin queue-depth skew and incidence of "one
  origin saturated while others idle."

**Why low risk:** Toggleable in the proxy; no persistent state changes; no
protocol compatibility matrix.

### 6.2 Prototype 2 — Slack-based fountain-coded EC repair over WAN (weeks 7–11)

**Why second:** Isolated repair path; easy A/B on repair p99 vs. overhead.

**What to build:**

- **Fountain encoding** of repair fragments at the source(s), with receiver
  feedback carrying only *decode sufficiency* information.
- A **slack parameter `ε`** exposed as an operator knob. Delivery finishes
  when the product of observed loss fractions across rounds falls below `ε`.
- A **decode-sufficiency gate** at the repair coordinator: finish when enough
  distinct fragments have arrived, not when all fragments have arrived.
- **ARQ fallback** if the gate fires before decode sufficiency — the
  protocol-safety guarantee.
- **Three-arm A/B:** Control (ARQ), Treatment A (FLUID + LT), Treatment B
  (FLUID + Nanorq).

**Success metrics:**

- **Repair p99 latency** vs. **bandwidth overhead** relative to the ARQ
  baseline.
- Pareto curve: `ε ∈ {0.01, 0.05, 0.10, 0.20}` plotted against p99 repair
  time and overhead ratio.
- The delta between Treatment A and Treatment B isolates the codec effect.

### 6.3 Prototype 3 — ARC-style pacing sidecar (weeks 12–15)

**Why third:** Highest leverage but highest interaction risk because it
touches the write/apply critical path.

**What to build:**

- A **receiver-side sidecar** between the replication recovery buffer and the
  storage engine's apply API.
- An **adaptive release offset** that tracks the upper envelope of observed
  recovery times.
- **No feedback to the leader**, no transport changes.
- **Ordering preserved** via heap key `(release_time, producer_ts, seq)`.
- **Shadow mode first** (24 h) before live.

**Success metrics:**

- **Engine p99 write latency** and **leader backpressure event rate**.
- **Added latency** ≤ 10 ms p99.

---

## 7. Implementation Break-Down

### 7.1 Design Principles

| Principle | Consequence |
|---|---|
| **Hot path in C, no allocations, no locks, no syscalls per unit** | Preallocated pools, SPSC rings, per-core state, batched I/O. Target < 100 ns per unit on the data path. |
| **Control plane in Python** | Profile recompute, ε scheduling, A/B assignment, metrics aggregation, rollout gating. Python never sits inline with data. |
| **C ABI is the contract** | Everything in `include/sdf/*.h` is `extern "C"`, versioned, golden-vector tested. |
| **Shared memory + seqlock for C↔Python handoff** | No RPC on the hot path. |
| **Every prototype is independently A/B-able** | Toggle at proxy, per repair job class, or per pipeline. |
| **Observability before optimization** | Each component emits the metric its success criterion is defined on. |
| **Rust is a phase, not a rewrite** | Module-by-module, behind the stable C ABI. |

### 7.2 Phase 0 — Shared Foundations (weeks 1–2)

**SPSC ring** (`include/sdf/common/ring.h`): single-producer
single-consumer, no locks, cacheline-isolated head/tail.

```c
typedef struct {
    _Atomic uint64_t head;
    uint64_t pad_head[7];

    _Atomic uint64_t tail;
    uint64_t pad_tail[7];

    uint32_t mask;
    uint32_t slot_size;
    uint32_t pad_meta[14];

    uint8_t slots[];
} spsc_ring_t;

size_t   spsc_sizeof(uint32_t capacity, uint32_t slot_size);
int      spsc_init(spsc_ring_t *r, void *mem, uint32_t capacity, uint32_t slot_size);
bool     spsc_try_push(spsc_ring_t *r, const void *slot);
bool     spsc_try_pop(spsc_ring_t *r, void *slot);
uint32_t spsc_batch_pop(spsc_ring_t *r, void *out, uint32_t max);
```

**Object pool** (`include/sdf/common/pool.h`): fixed-size slots, per-core
free lists, no `malloc` in the hot path.

**Seqlock and SHM layout:** Python writes into the inactive buffer, then flips
an `active_buf` byte under the write lock. C reads `active_buf` (acquire) and
the buffer under the read seqlock.

```text
SHM segment:
  offset 0x0000 : seqlock
  offset 0x0040 : profile/knob buffer A
  offset 0x0800 : profile/knob buffer B
  offset 0x0FC0 : telemetry header
  offset 0x1000 : telemetry ring
```

**Metrics:** lock-free counters and log-linear histograms, exported via
Python `/metrics`.

**Build and CI:** CMake ≥ 3.24, C17, `-Wall -Wextra -Werror`. CTest for unit
tests; docker-compose for integration.

**C↔Python FFI:** `bindings/cffi/build_ffi.py` compiles `src/c` into a shared
library and generates a Python module via CFFI.

### 7.3 Prototype 1 — Whack-a-Mole Implementation

**Entry point:** `wm_proxy_main.c` links as a shared library (`libsdfwm.so`)
into the existing cache-fill proxy, or runs as a thin `LD_PRELOAD`/plugin shim.

**C data plane:**

```c
#define WM_MAX_PATHS 64
#define WM_MAX_UNITS 1024

typedef struct {
    uint32_t version;
    uint32_t m;
    uint32_t n;
    uint32_t b[WM_MAX_PATHS];
} wm_profile_t;

typedef struct {
    uint32_t m;
    uint32_t n;
    uint32_t log2m;
    uint32_t reserved;
    uint64_t down_mask;
    uint32_t cdf[WM_MAX_PATHS];
    uint32_t perm[WM_MAX_UNITS];
    uint32_t unit_to_path[WM_MAX_UNITS];
} wm_compiled_t;

int      wm_profile_compile(const wm_profile_t *p, wm_compiled_t *out);
uint32_t wm_select(const wm_compiled_t *c, uint64_t counter);
void     wm_select_batch(const wm_compiled_t *c, uint64_t base,
                         uint32_t *out, size_t n);
void     wm_mark_down(wm_compiled_t *c, uint32_t path_id);
void     wm_mark_up(wm_compiled_t *c, uint32_t path_id);
```

**Hot path:** `unit_to_path[ctr & (m - 1)]` — one load and one AND.

`wm_mark_down()` renormalizes the remaining active paths in place; `cdf`
retains the original profile so `wm_mark_up()` restores it exactly. The last
healthy path is never marked down.

**Python control plane at 4 Hz:** score paths, multiplicative decrease on
degraded paths, additive increase on healthy paths, renormalize units to `m`,
write the profile into the inactive SHM buffer, flip `active_buf`.

**A/B assignment:** deterministic `bucket = xxhash(key) % 100`.

**Rollout gates:**

- 5% → 25% if p99 is at least 10% better, variance at least 20% better, bytes
  are neutral, and there is no error regression.
- 25% → 100% after 72 h stable.

### 7.4 Prototype 2 — FLUID Repair Implementation

**Fountain codec behind one ABI:**

```c
typedef struct fountain_enc fountain_enc_t;

int      fe_init(fountain_enc_t *e, const uint8_t *block, size_t block_len,
                 uint32_t k, uint32_t overhead_pct);
uint32_t fe_symbol_count(const fountain_enc_t *e);
int      fe_generate(fountain_enc_t *e, uint32_t esi, uint8_t *out, size_t out_len);

typedef struct fountain_dec fountain_dec_t;

int  fd_init(fountain_dec_t *d, uint32_t k, size_t sym_len);
int  fd_push(fountain_dec_t *d, uint32_t esi, const uint8_t *sym, size_t len);
bool fd_ready(const fountain_dec_t *d);
int  fd_decode(fountain_dec_t *d, uint8_t *out, size_t block_len);
```

Two implementations: `fe_codec_lt.c` (reference) and `fe_codec_nanorq.c`
(adapter over the Nanorq low-level API).

**FLUID gate:**

```c
typedef struct {
    double   eps;
    uint32_t round;
    uint32_t sent_round;
    uint32_t recv_round;
    double   loss_prod;
} fluid_gate_t;

void fg_init(fluid_gate_t *g, double eps);
void fg_round_start(fluid_gate_t *g);
void fg_observe(fluid_gate_t *g, uint32_t sent, uint32_t recv);
bool fg_done(const fluid_gate_t *g);
```

**Repair loop:**

```c
while (!fd_ready(dec) && !fg_done(gate)) {
    fg_round_start(gate);
    /* send a wave of repair symbols (via wm_select if Prototype 1 is live) */
    /* wait for wave ack or timeout */
    fg_observe(gate, sent, recv);
}
if (fd_ready(dec)) {
    fd_decode(dec, out, len);
} else {
    /* fall back to ARQ */
}
```

**Rollout gates:**

- 10% of one class, `ε = 0.10`.
- 50% if repair p99 is at least 25% better, overhead ≤ 1.20, and ARQ fallback
  < 2%.
- 100% after 7 days stable.

### 7.5 Prototype 3 — ARC Pacing Sidecar Implementation

**Pacer core:**

```c
typedef int (*arc_apply_fn)(void *ctx, const void *entry, size_t len, uint64_t ts);

typedef struct {
    uint32_t capacity;
    uint32_t batch_max;
    uint64_t batch_deadline_ns;
    uint64_t offset_min_ns;
    uint64_t offset_max_ns;
    double   increase_gain;
    double   decrease_gain;
} arc_cfg_t;

typedef struct arc_pacer arc_pacer_t;

int   arc_init(arc_pacer_t *p, const arc_cfg_t *cfg, arc_apply_fn fn, void *ctx);
int   arc_submit(arc_pacer_t *p, const void *entry, size_t len, uint64_t producer_ts);
int   arc_run(arc_pacer_t *p);
void  arc_stop(arc_pacer_t *p);
arc_stats_t arc_stats(const arc_pacer_t *p);
```

**Offset estimator:**

```text
offset_ns = max(offset_ns, ewma_max_lateness * safety)
```

Then apply a slow decay, clamped to `[offset_min_ns, offset_max_ns]`.

**Ordering:** heap key `(release_time, producer_ts, seq)`; ties by `seq`.

**Rollout gates:**

- 24 h shadow → live on one shard.
- Fleet if engine p99 is at least 15% better, backpressure events are at least
  30% fewer, and added latency is ≤ 10 ms p99.

---

## 8. Project Folder Structure

```text
storage-dataflow/
├── README.md
├── LICENSE
├── Makefile
├── CMakeLists.txt
├── meson.build
├── .clang-format
├── .clang-tidy
├── .editorconfig
├── .gitignore
├── .github/workflows/{ci,bench,docs}.yml
├── docs/
│   ├── design/{00-overview,01-whack-a-mole,02-fluid-repair,03-arc-pacing,
│   │          04-rust-migration,05-observability,06-nanorq-evaluation}.md
│   ├── adr/{0001-c-abi-as-contract,0002-shm-seqlock-handoff,
│   │        0003-rust-migration-order,0004-nanorq-behind-fountain-abi}.md
│   ├── ops/runbooks/{wm-rollout,fluid-rollback,arc-killswitch}.md
│   ├── ops/dashboards/{wm,fluid,arc}.json
│   └── figures/architecture.svg
├── configs/{dev,staging,prod}/{wm,fluid,arc}.yaml
├── third_party/{unity,libfuzzer,nanorq}/
├── include/sdf/
│   ├── sdf.h
│   ├── wm.h
│   ├── fountain.h
│   ├── fluid.h
│   ├── arc.h
│   └── common/{ring,pool,seqlock,timer,shm,metrics,log,config,compat}.h
├── src/
│   ├── c/
│   │   ├── common/{ring,pool,seqlock,timer,shm,metrics,log,config}.c
│   │   ├── whack_a_mole/{wm_profile,wm_select,wm_shed,wm_shm,wm_proxy_main}.c
│   │   ├── fountain/{fe_prng,fe_codec_lt,fe_codec_nanorq,fe_enc,fe_dec}.c
│   │   ├── fluid/{fluid_gate,fluid_round,fluid_repair_main}.c
│   │   └── arc/{arc_heap,arc_offset,arc_pacer,arc_sidecar_main}.c
│   └── rust/{sdf-fountain,sdf-arc}/     # phase 4+
├── python/
│   ├── pyproject.toml, setup.cfg, ruff.toml, mypy.ini
│   └── sdf/
│       ├── __init__.py, py.typed
│       ├── common/{__init__,shm,config,metrics,logging,ab}.py
│       ├── control/{__init__,wm_control,fluid_coord,arc_deploy}.py
│       ├── sim/{__init__,wm_sim,fluid_sim,arc_sim}.py
│       ├── bench/{__init__,runner,plots}.py
│       └── cli/{__init__,main}.py
├── bindings/
│   ├── cffi/{build_ffi,sdf_cffi}.py
│   └── cbindgen.toml
├── tests/
│   ├── c/{test_abi.c,unit,integration,fuzz}
│   ├── python/{conftest,test_wm_control,test_fluid_coord,test_arc_deploy,test_shm}.py
│   └── system/{docker-compose.yml,e2e_cache_fill.sh,e2e_repair.sh,e2e_apply.sh}
├── tools/
│   ├── loadgen/{wm_loadgen,repair_loadgen,apply_loadgen}.c
│   └── {profile_gen,trace_analyze}.py
├── packaging/{debian,rpm,helm/sdf}/
├── scripts/{build,run_tests,run_bench,lint,build_doc}.sh
└── golden_vectors/{wm_select,fountain_esi,arc_schedule}.json
```

---

## 9. Language Strategy: C, Python, Rust

| Layer | Current | Phase 4 | Why |
|---|---|---|---|
| Common primitives | C | C | Fixed-layout structs and ABI stability are the point |
| Whack-a-Mole dispatch | C | C | Table lookup; marginal benefit from Rust |
| Fountain codec | C default; opt-in Rust LT; Nanorq stays C | Rust LT A/B open (W6.8) | Pure compute, no I/O, golden-vector tested |
| FLUID gate | C | C | Small state machine; low risk |
| ARC pacer | C | Rust second | Heap + timing + callback; ownership removes a bug class |
| Control plane | Python | Python | Orchestration, A/B, metrics, config |
| FFI bindings | CFFI | CFFI | ABI-stable; Python side sees no change |

---

## 10. Nanorq Evaluation (RaptorQ vs. LT Codec)

### 10.1 Clarification: Not a Whack-a-Mole Substitute

A direct "Nanorq vs. Whack-a-Mole" comparison is a category error. Nanorq is
an erasure codec; Whack-a-Mole is a path dispatcher. They compose. The correct
Nanorq comparison is Nanorq vs. LT inside Prototype 2.

### 10.2 What Nanorq Is

A C implementation of RaptorQ (RFC 6330), reaching multi-gigabit speeds on a
single core. It provides:

- A high-level API (`nanorq.h`).
- A low-level API (`nanorq_core.h`) usable without libc under `NDEBUG`.
- Experimental TSNC helpers for multipath recoding and streaming.
- Host-CPU autotuning.

RaptorQ is designed for near-zero reception overhead: roughly `K` symbols
plus a tiny fraction, rather than 1.5× or 2× the data. It uses GF(256) for
improved rank properties.

### 10.3 Pros

| ID | Pro | Relevance |
|---|---|---|
| P1 | RFC-compliant near-zero overhead | Lowers the overhead axis of the Pareto curve |
| P2 | C implementation, no-libc low-level API | Fits the hot-path language decision |
| P3 | Maximally fungible symbols | Cleaner decode-sufficiency gate than LT |
| P4 | TSNC multipath recoding | Intermediate peers can recode without a full decode |
| P5 | Standardized interop (RFC 6330) | Future Rust/HW/third-party interop |

### 10.4 Cons

| ID | Con | Mitigation |
|---|---|---|
| C1 | RaptorQ decode is more complex than LT | Consume the library; LT remains the reference arm |
| C2 | Codec objects are stateful and not thread-safe per object | One codec object per worker/job |
| C3 | Block-size and symbol-size constraints | Account for them in Pareto measurement |
| C4 | TSNC helpers are experimental | Defer multipath recoding to phase 2 |
| C5 | Modest community footprint | Vendor and pin; fork-ready |

### 10.5 Recommendation

- **Prototype 1:** Nanorq is irrelevant.
- **Prototype 2:** Swap LT for a Nanorq adapter behind the same `fountain.h`
  ABI. Run three arms: Control (ARQ), A (FLUID + LT), B (FLUID + Nanorq).
  Keep the ARQ fallback.
- **Prototype 3:** Nanorq is orthogonal.

**Expected outcome:** Treatment B has lower overhead at the same p99 than A.
If the Pareto curves are close, the LT arm remains viable and simpler; if B
dominates, Nanorq is the default and LT becomes the regression reference.

### 10.6 Net Assessment

Nanorq is a better codec choice than a hand-rolled LT implementation for
FLUID repair, given the complexity tax and thread-safety discipline. It is not
a Whack-a-Mole replacement. Cleanest path: keep the `fountain.h` ABI,
implement twice, let the A/B decide.

---

## 11. Observability, Config, CI

### 11.1 Metric Inventory

| Prototype | Metrics |
|---|---|
| WM | `wm_dispatch_total{path,arm}`, `wm_discrepancy{path}`, `wm_fill_duration_seconds{arm}`, `wm_fill_bytes_total`, `wm_shed_events_total{path}` |
| FLUID | `fluid_repair_duration_seconds{arm,class}`, `fluid_repair_bytes{sent,useful}`, `fluid_rounds{arm}`, `fluid_arq_fallback_total`, `fluid_eps{class}`, `fluid_decode_rank`, `fluid_codec{lt,nanorq}` |
| ARC | `arc_offset_ns`, `arc_heap_depth`, `arc_release_batch_size`, `arc_added_latency_ns`, `arc_dropped_total`, `engine_apply_p99_seconds`, `engine_backpressure_events_total` |

### 11.2 Config Examples

See `configs/prod/{wm,fluid,arc}.yaml` in the repository tree.

### 11.3 CI Gates

- `ci.yml`: build + C unit tests + fuzz smoke (60 s each) + Python unit tests
  + mypy + integration (docker-compose) + CFFI golden-vector check.
- `bench.yml` (nightly): fail if p99 regresses > 5% vs. the 7-day rolling
  median.
- `golden_vectors/` is the ABI contract; changes require an ADR and a version
  bump.

---

## 12. Rust Migration Plan

**Contract:** `include/sdf/*.h` headers and `golden_vectors/` are the
contract. Rust replaces a module by exporting the same `extern "C"` symbols
from a staticlib/cdylib. CMake links the Rust library instead of the C object
files. No caller changes.

**Order:**

1. **Fountain** — pure compute, no I/O, no shared state. Validate against
   `golden_vectors/fountain_esi.json`. Use cbindgen to regenerate
   `fountain.h` and diff it against the C header. **W6.8 as-built:** the Rust
   LT internal arm is available via `SDF_USE_RUST_FOUNTAIN=ON`; C dispatch and
   nanorq are unchanged, both variants pass the same 556-comparison parity
   report, and C remains the default during the comparison window.
2. **ARC** — heap + timing + callback. Ownership removes the heap/ordering bug
   class.
3. **Whack-a-Mole** — leave in C longest. Table lookup; marginal benefit.

**Seams already in place:** SPSC ring, pool, seqlock, and SHM layouts are
language-neutral C structs with fixed offsets. The C ABI uses only POD types
and function pointers. No C++ or exceptions cross the boundary.

---

## 13. Milestones & Staffing

| Weeks | Deliverable | Owner |
|---|---|---|
| 1–2 | Phase 0 foundations + CI green | 1 systems engineer |
| 3–6 | Prototype 1 at 25% in one DC | 1 systems engineer + 1 SRE |
| 7–11 | Prototype 2 Pareto report + 50% one class | 1 systems engineer + 0.5 data scientist |
| 12–15 | Prototype 3 shadow → live one shard | 1 systems engineer + 1 engine owner |
| 16 | Joint evaluation doc + Rust migration ADR | all |

> Scheduling note: this table is the original staffing view; the
> operative sequence, sizes (P50–P90), and milestone gates are
> maintained in [`08-implementation-plan.md`](08-implementation-plan.md)
> §12. Weeks map to waves: 1–2 → W0, 3–6 → W1+W2, 7–11 → W3,
> 12–15 → W4, 16 → W5.

---

## 14. Risks & Mitigations

| Risk | Mitigation |
|---|---|
| WM profile flip races with in-flight fills | Version field in `wm_profile_t`; per-job counters keyed to version |
| Fountain decoder CPU pathological ESI streams | Fuzz gate + bounded work per symbol; abort to ARQ on budget |
| RaptorQ decode complexity hides a correctness bug | Golden vectors + fuzz + LT reference arm + ARQ fallback |
| Nanorq codec thread-safety violation | One codec object per worker/job |
| ARC added latency on sparse pipelines | `batch_deadline_ns` hard bound; shadow-mode first |
| Python control loop stalls → stale profile | C-side `wm_mark_down` fast path; liveness metric with pager |
| Rust migration breaks ABI | Golden vectors in CI day one; cbindgen header diff |
| SHM seqlock misuse | Single writer, single reader per segment; ADR-0002; unit-tested |
| TSNC dependency drift | Deferred to phase-2 experiment; vendored + pinned if used |

---

## 15. Fit to Modal Storage MTS Role

Modal's storage team operates a distributed object storage system underpinning
container images, volumes, and checkpoints: hundreds of petabytes, replicated
across multiple cloud object stores and a CDN, cached on local NVMe across a
large worker fleet, and shared P2P within each datacenter. Stated goals:
cold starts feeling local, hiding object-store latency, keeping public ingress
off saturated uplinks, and owning durability and cost at petabyte scale.

| Modal goal | Paper-stack application |
|---|---|
| Cold starts feel local | WM dispatch across origins/edges/peers; ARC pacing for image/checkpoint ingest into NVMe |
| Keep ingress off saturated uplinks | Soft shedding preferentially drains via P2P/colocated clusters |
| Durability and cost at petabyte scale | FLUID slack EC repair tightens the repair tail at bounded overhead; Nanorq's near-zero overhead lowers the cost axis |
| P2P sharing within a datacenter | Deterministic profile-based piece selection across peers |
| Replication across blob providers | ARC receiver-side pacing reduces engine p99/backpressure without sender feedback |

The role explicitly asks for experience building high-performance distributed
storage or caching systems at large scale, caching/preloading/P2P layers,
durability and cost at petabyte scale, and working across the stack from page
cache to distributed blob storage. The prototype sequence is a direct
demonstration of exactly that profile.

---

## 16. Appendices

### 16.1 Build & Doc Commands

```bash
./scripts/build.sh        # C + CFFI + Python package
./scripts/run_tests.sh    # C unit + Python unit + fuzz smoke
./scripts/run_bench.sh    # nightly bench
./scripts/build_doc.sh    # render docs/design/*.md -> out/*.pdf
```

### 16.2 Glossary

| Term | Meaning |
|---|---|
| ESI | Encoding Symbol ID |
| Loss-Product Rule | FLUID termination: product of per-round loss fractions < ε |
| RaptorQ | RFC 6330 fountain code, near-zero overhead |
| LT code | Luby Transform; simpler, higher overhead |
| TSNC | Tunable Sparse Network Coding (Nanorq experimental) |
| Seqlock | Lock-free single-writer SHM primitive |
| SPSC | Single-producer single-consumer ring |
| Golden vectors | Fixed input/output pairs defining the ABI contract |

### 16.3 Decision Log (Proposed ADRs)

- **ADR-0001:** C ABI is the migration contract; public symbols in
  `include/sdf/*.h` are `extern "C"` and versioned.
- **ADR-0002:** SHM + seqlock is the only C↔Python handoff on the hot path; no
  RPC.
- **ADR-0003:** Rust migration order is fountain → arc → whack_a_mole; golden
  vectors gate every step.
- **ADR-0004:** Nanorq is integrated as one of two fountain codec arms behind
  `fountain.h`; LT remains the reference.
- **ADR-0005:** Ops config format is strict TOML (wack_a_mole K.2 loader
  adapted); supersedes the YAML references in docs 01–03.
- **ADR-0006:** `include/sdf/arc.h` v2 — full ARC/ARC-Q/ARC-QG semantics,
  IMMEDIATE test oracle, size helper, extended stats.
- **ADR-0007:** `wm.h` gains profile-update embodiments 1–4 and spray
  shuffle methods/seeds; discrepancy bounds become property tests.
- **ADR-0008:** Origin driver seam `sdf_origin_ops_t` — origins are the
  paths; normalized stats are the controller's only input.
- **ADR-0009:** Golden-vector parity is the acceptance gate for every
  imported module; fills the empty `golden_vectors/` placeholders.

*End of document.*
