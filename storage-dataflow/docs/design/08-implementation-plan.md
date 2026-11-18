# 08 — Implementation Plan (import-led, waves W0–W5)

**Document type:** executable backlog — self-contained work packages
("prompts") suitable for assignment to an engineer or an autonomous coding
agent, in the wack_a_mole plan style.
**Sources:** [WAM] `research/Whack-a-Mole.pdf`, [FLUID]
`research/FLUID_Slack-based-Low-latency-Delivery.pdf`, [ARC]
`research/arc_adaptive_release_control.pdf`; implementation source of
record: the wack_a_mole repository (see
[07-wam-import-map](07-wam-import-map.md)).
**Related:** [00-overview](00-overview.md) (design baseline),
[09-origin-driver-seam](09-origin-driver-seam.md),
ADRs [0001–0009](../adr/).

---

## IMPLEMENTATION STATUS (informative — judgement; refreshed per wave)

  W0.1 repo + discipline     : done — delivered with this plan (git init,
                                ignore rules, CONTRIBUTING.md, this backlog)
  W0.2–W0.5 foundations      : 0% — prompts below
  W1 origin driver seam      : 0%
  W2 WM dispatch (P1)        : ~20% — dispatch core v1 exists and is tested
                                (wm_profile.c/wm_select.c/wm_shed.c + unit
                                tests); W2.1–W2.6 upgrade it per ADR-0007
                                and complete the proxy around it
  W3 FLUID repair (P2)       : 0% — all C sources are stubs
  W4 ARC sidecar (P3)        : 0% — all C sources are stubs; arc.h v1 is
                                superseded by ADR-0006 (v2 lands in W4.1)
  W5 hardening               : 0%

Percentages are engineering judgement weighted by scope and test
evidence, not a contract; each entry names what is missing. This ledger
is the single source of completion truth — a landed import without its
golden-parity gate (ADR-0009) counts as 0%.

---

## Table of contents

- [0. Scope, goals, non-goals](#0-scope-goals-non-goals)
- [1. Execution model](#1-execution-model)
- [2. Prototype ↔ wave ↔ design-doc map](#2-prototype--wave--design-doc-map)
- [3. Phase W0 — Foundations](#3-phase-w0--foundations)
- [4. Phase W1 — Origin driver seam](#4-phase-w1--origin-driver-seam)
- [5. Phase W2 — Whack-a-Mole dispatch (Prototype 1)](#5-phase-w2--whack-a-mole-dispatch-prototype-1)
- [6. Phase W3 — FLUID repair (Prototype 2)](#6-phase-w3--fluid-repair-prototype-2)
- [7. Phase W4 — ARC pacing sidecar (Prototype 3)](#7-phase-w4--arc-pacing-sidecar-prototype-3)
- [8. Phase W5 — Hardening, packaging, Rust seams](#8-phase-w5--hardening-packaging-rust-seams)
- [9. Testing map](#9-testing-map)
- [10. Bench gates](#10-bench-gates)
- [11. Risk register](#11-risk-register)
- [12. Sizes & milestones](#12-sizes--milestones)
- [13. Work-log protocol](#13-work-log-protocol)

---

## 0. Scope, goals, non-goals

### Goals

- Deliver the three prototypes of [00-overview §6](00-overview.md#6-prototype-sequence-roirisk-order)
  as independently deployable runtimes over one shared C core
  (`libsdf_{common,wm,fountain,fluid,arc}`):
  1. **P1 cache-fill proxy** — deterministic, seeded origin dispatch with
     graded shedding;
  2. **P2 repair coordinator** — fountain-coded, slack-gated WAN repair
     with ARQ fallback;
  3. **P3 apply sidecar** — ARC-QG receiver pacing with shadow/live/kill
     modes.
- **Import-first execution** (07 §1): every data-plane module starts from
  the proven wack_a_mole implementation, adapted behind `include/sdf`,
  and is accepted by golden-vector parity (ADR-0009).
- ABI discipline: `include/sdf/*.h` + `golden_vectors/` are the contract
  (ADR-0001); every delta is an ADR (0005–0009 register in 07 §5).
- Every prototype is independently A/B-able with the rollout gates
  already written in design docs 01–03.

### Non-goals (v1)

- No new storage engine, no replica protocol changes, no on-wire format of
  our own (repair feedback is count-based at the coordinator).
- No S3/peer drivers beyond the seam — `[[origin]]` classes loopback and
  HTTP ship; s3/peer are deferred design notes (09 §9).
- No Rust functionality — W5.3 validates the seam only (ADR-0003 order).
- No packet transports imported (07 §4).

### Key artifacts to implement verbatim (from the papers, via the import)

- [WAM]: bit-reversal spray θ, shuffle1/2 with seed pairs, discrepancy
  bounds ≤ ℓ / ≤ 2ℓ over any contiguous window, embodiments 1–4 with
  global residual, Σb = m invariant.
- [FLUID]: block budget N = ceil(K/(1−ε)); loss-product π = L_ℓ/N
  identity; efficiency K/N ≥ (1−ε)·K/(K+1); ARQ fallback safety.
- [ARC]: (S_n, A_n) → D/E → T_n schedule; ARC-QG ordered release;
  receiver clock only; IMMEDIATE as test oracle, never production.

---

## 1. Execution model

Work lands in **waves**; each prompt is a self-contained package with an
objective, the wack_a_mole source it imports (if any), deliverables,
contract, and an **acceptance** block that a reviewer or agent can verify
mechanically. The protocol (expanded in §13 and `CONTRIBUTING.md`):

1. `plan:` commit lands or amends the prompt before implementation.
2. `src:` commit references the prompt ID (e.g. `W2.3`) in its message.
3. Acceptance must be demonstrable from the repo: tests, goldens, gates.
4. `docs: <ID> outcome recorded, next = <ID>` sync commit follows each
   landing; the ledger above is refreshed per wave and pinned to a hash.

Velocity assumption (inherited from the wack_a_mole sizing document):
agent-assisted execution on well-specified prompts runs at 0.35–0.55×
solo-human time; design-heavy items 0.6–0.8×. Halve confidence if a
prompt goes stale mid-flight.

---

## 2. Prototype ↔ wave ↔ design-doc map

| Prototype | Wave | Design doc | Success metrics |
|---|---|---|---|
| 1 — profile dispatch + soft shedding | W1 + W2 | [01](01-whack-a-mole.md) | fill p99/variance down, bytes neutral |
| 2 — slack fountain repair | W3 | [02](02-fluid-repair.md) | repair p99 vs overhead Pareto; ARQ fallback < 2% |
| 3 — ARC pacing sidecar | W4 | [03](03-arc-pacing.md) | engine apply p99, backpressure events, added ≤ 10 ms p99 |
| cross-cutting | W0, W5 | [05](05-observability.md), [06](06-nanorq-evaluation.md) | CI gates, goldens, packaging |

Dependency spine: W0.2 → W2.3/W2.4 (SHM/seqlock) · W1 → W2.5/W3.5
(origin I/O) · W3.1–W3.4 → W3.5 · W4.1–W4.2 → W4.3. Waves W2, W3, W4
share W0 and W1 but are otherwise parallelizable across engineers.

---

## 3. Phase W0 — Foundations

> shared primitives, config, parity harness, gates. Everything below
> depends on this wave. Dependency: none.

### PROMPT W0.1 — Repo, gates, and work-log discipline ✅

**Objective:** version control and the execution protocol exist before
any code lands.
**Deliverables:** git repository at the `open_storage` root (papers +
code together, wack_a_mole layout); `.gitignore`; `CONTRIBUTING.md`
(commit prefixes, doc-sync protocol, evidence rules); this plan.
**Acceptance:** `git log` shows the baseline import and the doc series;
every subsequent landing follows §13.
**Size:** 0.5–1 day. **Status: done** — delivered with this plan's series.

### PROMPT W0.2 — Common primitives import

**Objective:** `src/c/common/*` implement the `include/sdf/common/*.h`
APIs with the proven wack_a_mole algorithms.
**Import:** `include/wam/spsc.h` (SPSC ring, TSan-clean) → `ring.c`;
`include/wam/profpub.h` (seqlock publish) → `seqlock.c`; pool/arena
patterns from WAM J.2 → `pool.c`; clock helpers → `timer.c`; ADR-0002
segment map → `shm.c`; log → `log.c`. Provenance blocks per ADR-0009.
**Contract:** exactly the existing `include/sdf/common/*.h` signatures —
no header changes in this prompt.
**Acceptance:** unit tests: ring wrap order + 2²² two-thread soak
(adapt `test_spsc`); seqlock no-torn-read under contention (adapt
`test_profpub`); pool no-malloc-after-warmup assert; SHM layout offset
asserts per ADR-0002. `scripts/run_tests.sh` green from clean build.
**Size:** 2–3 days.

### PROMPT W0.3 — Strict TOML loader + example configs

**Objective:** ops config loads with hard schema errors (ADR-0005).
**Import:** `src/config/config.c` (wack_a_mole K.2 loader, ~1.4 kloc) →
`src/c/common/config.c`, adapted to the sdf schema:
`[wm]`, `[fluid]`, `[arc]`, `[[origin]]`, `[ab]`, `[metrics]`.
**Deliverables:** loader behind `include/sdf/common/config.h`;
`configs/{dev,staging,prod}/{wm,fluid,arc}.toml` replacing the `.yaml`
placeholders; `tests/c/unit/test_config.c`.
**Contract:** precedence CLI > file > defaults; presence-bit merge (a
partial file cannot clobber built-in defaults); unknown key/type/range =
hard `path:line:` error.
**Acceptance:** `test_config` loads every shipped example, pins
defaults, and asserts rejection cases (adapt WAM `test_config`).
**Size:** 2–3 days.

### PROMPT W0.4 — Golden-parity harness

**Objective:** the ADR-0009 gate exists before the first import.
**Import:** golden material from the wack_a_mole test suite: θ(249,10)=636
and seed-pair sequences (`test_spray`), embodiment vectors
(`test_profile`), ARC §12.3 schedules (`test_arc`), FLUID O.1 identities
(`test_fluid`) — as cited in 07 §3's parity column.
**Deliverables:** `tests/c/test_parity.c` + a minimal JSON-subset reader
(the `golden_vectors/*.json` format stays); `golden_vectors/wm_select.json`
filled (θ golden, seed sequences, discrepancy vectors);
`arc_schedule.json`/`fountain_esi.json` gain their vector schemas now
(values land with W4.2/W3.1).
**Acceptance:** `test_parity` runs in ctest and validates every present
vector; CI (W0.5) includes it.
**Size:** 1–2 days.

### PROMPT W0.5 — Metrics conventions + Makefile gates

**Objective:** observability conventions and local gates mirror the
wack_a_mole `makefile.wam` set.
**Import:** counter/reservoir-histogram conventions from
`include/wam/metrics.h` → `src/c/common/metrics.c` (sdf series names per
00 §11.1 stay authoritative); gate structure from `makefile.wam`.
**Deliverables:** `metrics.c` behind the existing header; Makefile
targets `ci` (format+tidy+gcc+clang where available), `asan`, `tsan`,
`bench` (recorded), `docs`; `scripts/run_bench.sh` wired to record into
`build/bench_results.txt` (never ctest-gated).
**Acceptance:** `make ci` green from clean checkout; `make asan`/`tsan`
build and run the suite; bench target records spray/arc micro-benches
(thresholds activate in W2/W4).
**Size:** 1–2 days.

**W0 subtotal: 6–10 engineer-days.**

---

## 4. Phase W1 — Origin driver seam

> origins are the paths (ADR-0008, design 09). Depends on W0 only for
> gates; W1.1 has no code dependencies.

### PROMPT W1.1 — `include/sdf/origin.h` ABI

**Objective:** the vtable contract of design 09 §2–3 becomes a header.
**Deliverables:** `include/sdf/origin.h` (`sdf_origin_ops_t`,
`sdf_fetch_req_t`, `sdf_origin_stats_t`, `sdf_fetch_done_fn`,
`sdf_origin_cfg_t` with class enum); `tests/c/test_abi.c` extended with
layout asserts; ADR-0008 referenced from the header comment.
**Acceptance:** ABI test green; review confirms the link rule (no driver
types above the seam) is expressible.
**Size:** 1–2 days.

### PROMPT W1.2 — Loopback origin driver (the harness)

**Objective:** byte-range serving from local files with live degradation
knobs — the netem analogue.
**Deliverables:** `src/c/origin/loopback/loopback_origin.c` implementing
`sdf_origin_ops_t`; per-origin `throttle_ns`, `loss_pct`, `err_pct`,
`lat_jitter_ns` knobs, live-reloadable mid-run; completion callback on an
internal driver thread; `tests/c/unit/test_origin_loopback.c`.
**Acceptance:** stats exactness under each knob; knob reload mid-stream
takes effect without restart; drop/fail units produce correct
`reqs_failed`/`ewma_err`; no allocation per fetch after warmup.
**Size:** 2–3 days.

### PROMPT W1.3 — HTTP origin driver

**Objective:** a real-network reference driver behind the same ops.
**Deliverables:** `src/c/origin/http/http_origin.c` — POSIX-socket
HTTP/1.1 `Range` GET, connection reuse, `SDF_HAVE_CURL` optional path
(default OFF); origin config `url_template`, `timeout_ms`, `max_inflight`.
**Acceptance:** integration test against a local Python `http.server`
serving ranged files (loopback sockets, no external dependency);
timeouts surface as failed units, not hangs.
**Size:** 3–4 days.

### PROMPT W1.4 — Origin bench + degradation harness

**Objective:** repeatable multi-origin runs for every later e2e.
**Deliverables:** `tools/loadgen/origin_loadgen.c` (drives N origins
through a compiled profile, prints per-origin tallies + timing);
degradation profile files for the e2e scripts (steady, one-origin-slow,
one-origin-dies-mid-run, flapping).
**Acceptance:** loadgen runs each profile; tallies match
`wm_select_batch` counts exactly for a static profile.
**Size:** 1–2 days.

**W1 subtotal: 7–11 engineer-days.**

---

## 5. Phase W2 — Whack-a-Mole dispatch (Prototype 1)

> design [01](01-whack-a-mole.md); upgrades the implemented v1 core per
> ADR-0007 and completes the proxy around it.
> Dependencies: W0.2 (seqlock/SHM), W0.3 (config), W1 (origins).

### PROMPT W2.1 — Spray upgrade: methods, seeds, discrepancy

**Objective:** selection carries the paper's shuffle methods and seed
pairs; the bounded-discrepancy property becomes a test.
**Import:** `src/core/spray.c` — `wam_theta` (256-byte bit-reversal
table), `wam_spray_key` (plain/shuffle1/shuffle2), wrap rotation,
`wam_measure_discrepancy` harness.
**Deliverables:** `wm_select.c` upgraded: `wm_compile_cfg_t {method, sa,
sb}` (sb validated odd), seed rotation on wrap, batch form kept;
`tests/c/unit/test_wm_select.c` extended; `golden_vectors/wm_select.json`
seed-sequence + discrepancy vectors consumed by `test_parity`.
**Contract:** ADR-0007 exactly; `wm_compiled_t` layout change pinned in
`test_abi.c`; `SDF_VERSION` minor bump.
**Acceptance:** θ golden; deviation ≤ ℓ (shuffle1) and ≤ 2ℓ (shuffle2)
over contiguous-window property sweeps; two jobs with equal profiles and
different seeds never emit identical sequences over the first m units.
**Size:** 2–3 days.

### PROMPT W2.2 — Profile updates + severity controller

**Objective:** graded shedding: the controller removes `e(i)` balls with
deterministic redistribution; the C fast path whacks on hard signals
without Python.
**Import:** `src/core/profile.c` — `wam_profile_update` (embodiments
1–4, global residual r, renormalize with uint64 intermediates);
`src/ctrl/controller.c` — severity windows, θ_hi/θ_lo pulls, t_min rate
limit.
**Deliverables:** `wm_profile_update()` in `wm_profile.c`; severity
controller in `wm_shed.c` consuming `sdf_origin_stats_t` deltas
(ADR-0008 link rule: no driver headers); `tests/c/unit/test_wm_profile.c`
extended.
**Acceptance:** embodiment goldens (adapt `test_profile`); Σb = m and
residual fairness invariants; last-healthy-path never removed; C-side
whack latency bounded by t_min (unit test with synthetic stats).
**Size:** 3–4 days.

### PROMPT W2.3 — SHM profile publication

**Objective:** the Python loop publishes profiles; C readers never see a
torn profile (ADR-0002).
**Import:** `include/wam/profpub.h` publication protocol → the ADR-0002
segment map (double buffer + seqlock + `active_buf` flip + version).
**Deliverables:** `wm_shm.c` implemented: create/attach, publish
(inactive buffer → flip), acquire snapshot (reader), teardown;
`tests/python/test_shm.py` extended against a live segment.
**Acceptance:** no-torn-read contention test (reader loop vs writer
hammer, adapted `test_profpub`); version increments monotonically;
teardown leaves no segment (loop-restart test).
**Size:** 2–3 days.

### PROMPT W2.4 — Python control loop over live SHM

**Objective:** the 4 Hz loop closes against real telemetry.
**Import:** design 01 §Control-plane loop (reference `wm_control.py`
stays the scoring authority); presence-bit merge pattern from WAM K.2.
**Deliverables:** `python/sdf/control/wm_control.py` + `sdf/common/shm.py`
wired: read telemetry ring → score (missing telemetry = "no opinion") →
multiplicative decrease / additive increase → renormalize (largest
remainder stays) → write inactive buffer → flip; liveness heartbeat
metric; resolve 01 §Open-questions #3 (controller observes `down_mask`
and does not undo a C hard-down for one interval).
**Acceptance:** `tests/python/test_wm_control.py` end-to-end against a
stubbed SHM segment with scripted telemetry; unit tests for the
no-opinion scoring edge case; heartbeat visible to the metrics endpoint.
**Size:** 2–3 days.

### PROMPT W2.5 — Cache-fill proxy main

**Objective:** P1 exists as a runnable proxy over the origin seam.
**Deliverables:** `wm_proxy_main.c`: job admission (object + byte ranges
→ units), `wm_select` assignment with per-job counters, fetch issue +
completion ring accounting, A/B bucketing `bucket = xxhash64(key) % 100`
with arm assignment from config, metrics per 01 §Metrics, TOML config
per ADR-0005, live scrape endpoint (adapt WAM `metrics_endpoint.c`,
`SDF_METRICS_ENDPOINT`); `tests/c/integration/test_wm_proxy.c`.
**Acceptance:** integration test over loopback origins: per-origin
dispatch counts within the discrepancy bound of the compiled profile;
mid-run degradation redistributes within one control interval; kill
switch (config `wm.rollout_pct = 0`) falls back to round-robin arm.
**Size:** 3–5 days.

### PROMPT W2.6 — P1 e2e + rollout gates

**Objective:** the design-01 rollout table is measurable.
**Deliverables:** `tests/system/e2e_cache_fill.sh` over W1.4 degradation
profiles (steady / slow-origin / dies-mid-run / flapping); bench target
`bench_wm` records select cost (cycles/decision) and fill completion
p99/variance per arm; gates wired into `scripts/run_bench.sh`.
**Acceptance:** e2e green under every degradation profile with zero
failed fills (retries are controller-visible); dispatch-counts assertion
inside discrepancy bound; bytes-neutral check (treatment vs control);
spray decision cost ≤ 15 cyc recorded (gate activates: fail CI if
exceeded by 2× on the reference runner).
**Size:** 3–4 days.

**W2 subtotal: 15–22 engineer-days. Milestone M2 = P1 at the 5% gate of
design 01's rollout table.**

---

## 6. Phase W3 — FLUID repair (Prototype 2)

> design [02](02-fluid-repair.md); evaluation framing
> [06](06-nanorq-evaluation.md) (three arms, codec A/B).
> Dependencies: W0.4 (parity harness), W1 (origins), W2.1 (select for
> repair traffic can share live health).

### PROMPT W3.1 — Fountain ABI v2

**Objective:** close the ADR-0001 open item before any codec ships.
**Deliverables:** `include/sdf/fountain.h` gains `fe_sizeof(cfg)` /
`fd_sizeof(cfg)` + init-in-place forms; ESI/tag fields made explicit in
`fe_generate`/`fd_push`; `SDF_VERSION` minor bump;
`golden_vectors/fountain_esi.json` schema + first roundtrip vectors.
**Acceptance:** ABI test green; a toy codec implementing the ABI passes
`test_parity` on the schema vectors.
**Size:** 1–2 days.

### PROMPT W3.2 — nanorq vendoring + RaptorQ arm

**Objective:** the RaptorQ arm runs pinned, vendored, behind the ABI.
**Import:** nanorq pinned to the commit family wack_a_mole validated
(import map §7 — correct the `ursoid-tech` references in
`third_party/README.md` and 00 §2.4 in the same commit); encode/decode
block handling adapted from WAM `src/codec/codec.c` (block-level only —
no session layer, no S_n at this ABI).
**Deliverables:** `third_party/nanorq` vendored + `CMakeLists` builds
`libnanorq.a`; `fe_codec_nanorq.c`; fuzz budget hook in
`tests/c/fuzz/fuzz_fountain.c`.
**Acceptance:** roundtrip golden vectors; 20%-loss repair completes
(adapt `test_codec`); decode-abort budget respected (bounded work per
symbol push).
**Size:** 2–3 days.

### PROMPT W3.3 — LT reference arm (fresh code — no WAM source)

**Objective:** the A/B codec baseline (exception to import-first per
07 §1; cite the import map in the commit).
**Deliverables:** `fe_prng.c` (xorshift64*), Robust Soliton degree
distribution, `fe_codec_lt.c` (systematic LT: K source + on-demand
repair symbols), dense-GF(2)-free XOR decode.
**Acceptance:** roundtrip + loss-repair tests; overhead per block
recorded for the Pareto harness; `fuzz_fountain` budget runs 60 s clean.
**Size:** 3–5 days.

### PROMPT W3.4 — FLUID both halves

**Objective:** sender budget and coordinator gate implement the paper
together, with the WAM O.1/O.5 evidence attached.
**Import:** `include/wam/fluid.h` — `wam_fluid_budget_N`
(near-integer snap), `_efficiency_ok`, `_cost_ok`, `_first_round`,
admit/report state machine → `fluid_round.c` (sender budget per block)
and `fluid_gate.c` (coordinator π = L_ℓ/N gate, existing `fg_*` API).
**Deliverables:** `include/sdf/fluid.h` v2 (budget half added per 07
§5); `tests/c/unit/test_fluid_gate.c` with the O.1 sweep
(K = 1..64Ki × ε ∈ {0.01..0.2}) and randomized never-past-allowed
property; loss-product identity property.
**Acceptance:** all O.1 bounds hold verbatim; efficiency and cost
identities pass; ε = 0 disables cleanly (zero overhead path, WAM
pattern: NULL-check only).
**Size:** 2–3 days.

### PROMPT W3.5 — Repair coordinator loop

**Objective:** the design-02 repair loop runs over origins with ARQ
fallback.
**Deliverables:** `fluid_repair_main.c`: per-class job admission
(`hot-metadata` ε = 0.05, `cold-checkpoint` ε = 0.15 per 02 §Classes),
wave send via `wm_select` over origin stats, `fg_observe` on
`stats()` deltas, decode-sufficiency exit, **ARQ fallback when the gate
fires before `fd_ready`** (never ship an incomplete block), decode-abort
budget, metrics per 02 §Metrics.
**Acceptance:** integration test: completes under 20% injected loss with
ARQ fallback < 2% at ε = 0.10; gate-fires-early path returns a complete
block via fallback; class routing from TOML config.
**Size:** 3–5 days.

### PROMPT W3.6 — Three-arm A/B + loss ladder e2e

**Objective:** the design-02 Pareto experiment is reproducible.
**Deliverables:** `tests/system/e2e_repair.sh`: arms Control (ARQ) /
A (FLUID+LT) / B (FLUID+Nanorq), ε ∈ {0.01, 0.05, 0.10, 0.20}, loss
ladder {0..50%} via loopback knobs (adapt WAM `test_fluid_ladder_e2e`
shape); results to `build/bench_results.txt` + `python/sdf/bench`
plots (p99 vs overhead per arm).
**Acceptance:** ladder completes byte-exact at every step with fallback
bounded; the harness prints the Pareto table; soak leg with dropped
feedback (adapt `test_fluid_soak_e2e`: every 3rd report lost — budget
lags, never stalls).
**Size:** 4–6 days.

**W3 subtotal: 15–24 engineer-days. Milestone M3 = P2 at the 10% gate of
design 02's rollout table.**

---

## 7. Phase W4 — ARC pacing sidecar (Prototype 3)

> design [03](03-arc-pacing.md); ABI per ADR-0006.
> Dependencies: W0.2 (ring for handoff), W0.4 (parity harness).
> Highest interaction risk — shadow-first, kill switch always armed.

### PROMPT W4.1 — `arc.h` v2 (ADR-0006)

**Objective:** the ABI carries the real algorithm before the core lands.
**Deliverables:** `include/sdf/arc.h` v2: modes (`ARC_MODE_BASE/Q/QG`,
`ARC_MODE_IMMEDIATE` oracle), full parameter set (ρ_u/ρ_l, λ_u/λ_l, U, δ,
γ, J, G, T_idle, auto-param gains, `min_smooth_bytes`, `budget_ms`),
`arc_sizeof`, `arc_poll(p, now)` release loop, extended stats (D, E,
added-latency histogram, interval/jitter, guards, δ-clamps, budget
alerts, S_n rejects); `tests/c/test_abi.c` layout + version bump;
config schema ranges in the strict loader (ADR-0005).
**Acceptance:** ABI test green; loader rejects out-of-range params
(γ ∈ [8,20] ms etc. per the paper's static ranges).
**Size:** 1–2 days.

### PROMPT W4.2 — ARC core import

**Objective:** the validated wack_a_mole scheduler runs behind v2.
**Import:** `src/arc/arc.c` — asymmetric D update, ARC-Q E hysteresis,
ARC-QG guard ordering, idle re-anchor, δ-clamp, auto-params from RTT and
Δ_n quantiles, IMMEDIATE oracle, heap slab (adapt `test_arc` goldens).
**Deliverables:** `arc_offset.c` + `arc_heap.c` + `arc_pacer.c`;
`golden_vectors/arc_schedule.json` filled with paper §12.3 vectors;
`tests/c/unit/test_arc_offset.c` + `test_arc_heap.c`.
**Acceptance:** §12.3 goldens via `test_parity`; quantization invariants
(E ≥ D, 0 ≤ E−D < γ); guard bound; idle re-anchor; ARC per-block cost
≤ 200 ns recorded (`bench_arc`, gate activates at 2× on the reference
runner).
**Size:** 3–5 days.

### PROMPT W4.3 — Sidecar main + SPSC handoff

**Objective:** the sidecar sits between a recovery buffer and an engine
apply callback (design 03 §Objective).
**Deliverables:** `arc_sidecar_main.c`: entries in over an SPSC ring,
`arc_poll` release to the apply callback, small-entry bypass
(`min_smooth_bytes`), batch drain by `batch_max`/`batch_deadline_ns`,
modes wired: **shadow** (compute + metrics, apply immediately via the
old path), **live** (`ARC_MODE_QG`), **killed** (`ARC_MODE_IMMEDIATE`);
metrics per 03 §Metrics.
**Acceptance:** integration test: entry ordering preserved under
bursty injection (heap key `(release_time, producer_ts, seq)`, ties by
seq); killed-mode added latency = 0 by construction; shadow metrics
match live metrics' schedule exactly (same inputs, different apply path).
**Size:** 2–4 days.

### PROMPT W4.4 — Python deploy orchestration

**Objective:** shadow/live/killed is an operator action, not a rebuild.
**Import:** WAM K.3 live-tune pattern (control-plane updates live
sessions).
**Deliverables:** `python/sdf/control/arc_deploy.py` (stub today):
pipeline selection, mode commands over the control channel, shadow
24 h clock, added-latency SLO watch with auto-kill recommendation;
`tests/python/test_arc_deploy.py`.
**Acceptance:** deploy tests drive a running sidecar (mode flips land
within one poll interval); SLO breach recommends kill and the runbook
step is scriptable.
**Size:** 1–2 days.

### PROMPT W4.5 — Trace replay + kill-switch verification

**Objective:** the strongest ARC evidence transfers to this repo, and the
runbook is a test.
**Import:** WAM L.6 harness — 60 fps peak/valley trace, N = 600, through
IMMEDIATE/BASE/Q/QG (adapt `test_arc_experiments`).
**Deliverables:** `tests/c/integration/test_arc_sidecar.c` replay leg +
`tests/system/e2e_apply.sh` with a bursty recovery generator; the
`docs/ops/runbooks/arc-killswitch.md` steps executed as a scripted leg.
**Acceptance:** replay leg pins: QG 0 order violations, median interval
error 0, p99 added delay ≤ the 38 ms-class result under the same trace;
kill-switch leg: flip to killed under load, zero drops, ordering intact;
shadow→live promotion asserts the 03 rollout table's measurements are
produced (added latency ≤ 10 ms p99 gate on the harness generator).
**Size:** 2–3 days.

**W4 subtotal: 9–16 engineer-days. Milestone M4 = P3 at the shadow gate
of design 03's rollout table.**

---

## 8. Phase W5 — Hardening, packaging, Rust seams

> cross-cutting; can start opportunistically once W2 lands.

### PROMPT W5.1 — Fuzzers

**Import:** WAM M.1/X4 fuzzer shapes: ESI/tag streams over
`fd_push`; feedback + embodiment vectors over `wm_profile_update`;
`(S_n, A_n)` release streams over the ARC heap.
**Deliverables:** `tests/c/fuzz/{fuzz_fountain,fuzz_wm_profile}` filled +
`fuzz_arc_stream.c`; 60 s smoke in the `ci` gate; invariants: decoder
budget bounded, Σb = m, ordering never violated by guard logic.
**Acceptance:** clean 60 s runs at the CI seed budget; any invariant
breach fails the gate.
**Size:** 2–4 days.

### PROMPT W5.2 — Packaging + embedding

**Import:** WAM R2/M.2 pattern — install rules, pkg-config, version
macro as single source of truth, core/engine split.
**Deliverables:** CMake install + `sdf.pc` + `find_package(sdf)`
config; version macro `include/sdf/version.h`; cffi bindings verified
against **installed** artifacts (not the build tree); embedding notes
(thread contract, no-malloc-after-warmup) in `docs/design/05`.
**Acceptance:** external hello-world links via pkg-config and
`find_package`; `test_install` leg in CI.
**Size:** 3–4 days.

### PROMPT W5.3 — Rust seam validation

**Import:** none (seam only, per ADR-0003 order: fountain first).
**Deliverables:** `bindings/cbindgen.toml` exercised: a Rust
`sdf-fountain` skeleton crate regenerates a header diffed against
`include/sdf/fountain.h` in CI; golden vectors pass through the Rust
fountain stub where implemented; no functional Rust modules yet.
**Acceptance:** header diff empty for unchanged ABIs; any intentional
delta fails CI until an ADR bumps the vectors (ADR-0001 process).
**Size:** 2–3 days.

**W5 subtotal: 7–11 engineer-days.**

---

## 9. Testing map

| Test | Wave | Covers |
|---|---|---|
| `test_abi` | W0/W1/W2/W3/W4 | struct offsets, version bumps (extended per prompt) |
| `test_ring` | W0.2 | wrap order, 2²² two-thread soak |
| `test_seqlock` | W0.2 | no torn read under contention |
| `test_pool` | W0.2 | no-malloc-after-warmup |
| `test_shm` | W0.2/W2.3 | ADR-0002 offsets, publish/flip, teardown |
| `test_config` | W0.3 | every example TOML, defaults, strict rejections |
| `test_parity` | W0.4+ | every golden vector; the ADR-0009 gate |
| `test_metrics` | W0.5 | series names pinned |
| `test_origin_loopback` | W1.2 | stats exactness, knob live-reload |
| `test_origin_http` | W1.3 | ranged GET vs local http.server, timeout = failed unit |
| `test_wm_select` | W2.1 | θ golden, seeds, wrap, discrepancy properties |
| `test_wm_profile` | W2.2 | embodiments 1–4, Σb=m, residual fairness, last-path |
| `test_wm_proxy` | W2.5 | dispatch counts vs profile, degradation redistribution |
| `test_fluid_gate` | W3.4 | O.1 budget/efficiency/cost sweeps, π identity |
| `test_fe_codec` | W3.2/W3.3 | both arms: roundtrip, loss repair, abort budget |
| `test_fluid_repair` | W3.5 | coordinator loop, ARQ fallback, class routing |
| `test_arc_offset` | W4.2 | §12.3 goldens, quantization, re-anchor, clamps |
| `test_arc_heap` | W4.2 | guard bound, release order, slab |
| `test_arc_sidecar` | W4.3/W4.5 | ordering under bursts, modes, trace replay, kill switch |
| `test_{wm,fluid}_control`, `test_arc_deploy` | W2.4/W4.4 | Python loops against live segments/sidecars |
| `e2e_{cache_fill,repair,apply}.sh` | W2.6/W3.6/W4.5 | degradation profiles, loss ladder, bursty apply |
| `fuzz_*` | W5.1 | codec streams, embodiment vectors, ARC release streams |

## 10. Bench gates

Recorded by `make bench` into `build/bench_results.txt` (never
ctest-gated), thresholds enforced by `make ci` on the reference runner
at 2× headroom:

| Bench | Threshold (activate) | Source gate |
|---|---|---|
| `bench_wm` select | ≤ 15 cyc/decision (W2.6) | WAM J.3 spray gate |
| `bench_arc` | ≤ 200 ns/block (W4.2) | WAM J.3 ARC gate |
| `bench_origin` | fetch issue+completion p99 (W1.4, recorded only) | new |
| `bench_e2e_fill` | fill p99/variance per arm (W2.6, recorded) | design 01 gates |
| `bench_e2e_repair` | p99 vs overhead per arm (W3.6, recorded) | design 02 Pareto |

Nightly `bench.yml` fails on > 5% regression vs the 7-day rolling median
(00 §11.3 stays authoritative).

## 11. Risk register

| ID | Risk | Mitigation (design-in) |
|---|---|---|
| S1 | Adaptation breaks imported semantics silently | ADR-0009 golden parity + bridge tests per 07 §3; parity gate in `ci` |
| S2 | ARC ABI v2 diverges from paper under refactors | §12.3 goldens + L.6-style replay pinned in `test_parity` |
| S3 | LT arm (fresh code) has a decode bug | roundtrip + loss tests + fuzz budget; LT is the A/B reference, never the default |
| S4 | nanorq pin mismatch vs validated upstream | import map §7: pin to the wack_a_mole-validated commit family; correct URLs in the same commit |
| S5 | SHM seqlock misuse (torn profile) | ADR-0002 single-writer/single-reader; contention tests; version field per in-flight fill (design 01 open question #1) |
| S6 | Python loop stalls → stale profile | C `hard_down` fast path (W2.2); liveness metric with pager (W2.4); proxy keeps last-good profile |
| S7 | Controller fights C-side mark-downs | W2.4 resolves design 01 open question #3 (one-interval deference) |
| S8 | Origin HTTP driver variance masks e2e signal | loopback knobs are the primary gates (W2.6/W3.6); HTTP leg is a smoke, not a gate |
| S9 | ARC added latency on sparse pipelines | `budget_ms` clamp + shadow-first + auto-kill (W4.3/W4.4) |
| S10 | Repair gate fires before decode sufficiency | ARQ fallback mandatory (W3.5); fallback rate is a rollout gate (< 2%) |
| S11 | Config schema sprawl | strict loader rejects unknown keys (ADR-0005); schema changes ride their ADR |
| S12 | Rust seam drift (W5.3+) | cbindgen header diff in CI; golden vectors shared with C |

## 12. Sizes & milestones

Sizes are engineer-days, **P50–P90** (±30–40%), agent-assisted calendar
at 0.35–0.55× solo time (§1 velocity model).

| Wave | Scope | P50–P90 | Subtotal |
|---|---|---|---|
| W0 | foundations (W0.2–W0.5) | 6–8 | 6–10 incl. W0.1 |
| W1 | origin seam | 7–11 | 7–11 |
| W2 | P1 dispatch + proxy | 15–22 | 15–22 |
| W3 | P2 FLUID repair | 15–24 | 15–24 |
| W4 | P3 ARC sidecar | 9–16 | 9–16 |
| W5 | hardening/packaging | 7–11 | 7–11 |
| **Total** | | **59–92** | **~60–95 engineer-days** |

| Milestone | Waves | Gate (done when) |
|---|---|---|
| M0 | W0 | `make ci` green from clean clone; parity harness runs goldens; all example TOMLs load strict |
| M1 | W1 | loopback + HTTP drivers serve ranged fetches; stats exact under knobs; loadgen reproduces profiles |
| M2 — P1 5% | W2 | e2e_cache_fill green under all degradation profiles; discrepancy bounds hold; bytes neutral; spray cost ≤ 15 cyc |
| M3 — P2 10% | W3 | loss ladder byte-exact with ARQ fallback < 2% at ε = 0.10; Pareto table printed; O.1 bounds pass |
| M4 — P3 shadow | W4 | replay: QG 0 violations; kill-switch leg green; deploy loop drives a live sidecar |
| M5 | W5 | fuzz smoke in `ci`; install + cffi against installed artifacts; cbindgen diff clean |

Calendar (solo + agent, this repo's mode): **M0–M2 ≈ 3 weeks; M0–M5 ≈
5–7 weeks.** Two-engineer split: W3 ∥ W4 after W2, ≈ 4–5 weeks.
Contingency +20–25% if a prompt goes stale mid-flight (§1).

## 13. Work-log protocol

Full rules in `CONTRIBUTING.md` (repo root). Summary:

1. `plan: <prompt ID>` before implementation; `src:`/`tests:`/`include:`
   commits cite the ID (e.g. `src: W3.4 — FLUID sender budget`).
2. After each landing: `docs: <ID> outcome recorded, next = <ID>` —
   update the ledger above (per-wave %, what is missing) and the owning
   design doc's status header.
3. Per milestone: `docs: truth-pass` syncing every status claim against
   the tree, and a ledger refresh pinned to the head hash.
4. Evidence rule: completion claims cite test names, gate numbers, or
   commit hashes — never adjectives.
5. Exceptions to import-first (07 §1) must cite the import map in the
   commit message (see W3.3 for the precedent).

*End of document.*
