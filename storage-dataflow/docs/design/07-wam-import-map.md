# 07 — Import Map: wack_a_mole → storage-dataflow

**Status:** Active — governs every data-plane landing until Wave 5 closes.
**Source project:** `wack_a_mole` (sibling repository), baseline `a0f0422`
("j3: UDP ≥10 attribution + codec/batch memset wins"), 48/48 ctest, local
`ci` + `bench-gate` green.
**Related:** [ADR-0009](../adr/0009-golden-vector-parity.md) (acceptance
rule), [08-implementation-plan](08-implementation-plan.md) (work packages),
[09-origin-driver-seam](09-origin-driver-seam.md) (the new seam),
[00-overview](00-overview.md) (design baseline).

---

## 1. Why an import map

The wack_a_mole repository implements the same paper stack — [WAM]
deterministic spraying, [FLUID] slack-based delivery, [ARC] adaptive
release control, and the nanorq RaptorQ codec — as a hardened multipath
*transport* library. storage-dataflow needs the same cores under a
storage-shaped ABI (`include/sdf/*.h`) where the "paths" are origins and
the "units" are byte-range requests. The execution principle is therefore
**import-first**:

> No data-plane module is written from scratch while a proven
> wack_a_mole implementation of the same math exists. An exception must
> cite this document in its commit message.

Imported code is *adapted*, not linked: the sdf ABI (ADR-0001) stays the
contract, and behavioral identity is proven by golden-vector parity
(ADR-0009), not by textual diff.

## 2. Source state (as of `a0f0422`)

| Evidence | Value |
|---|---|
| Test suite | 48/48 ctest (netns legs exit-77 without `CAP_NET_ADMIN`) |
| Local gates | `make ci` (format/tidy/gcc+clang/ASan/nanorq), `bench-gate` |
| Bench gates | spray ≤ 15 cyc/decision, ARC ≤ 200 ns/block, UDP ≥ 2.3 Gbps |
| ARC validation | paper §12.3 goldens; L.6 60 fps replay — ARC-QG 0 order violations, p99 added delay 38 ms |
| FLUID validation | O.1 budget/efficiency/cost bounds; O.5 loss ladder {0..50%}; soak with dropped feedback |
| Threading | SPSC rings + seqlock publish TSan-clean; RX/DEC/ARC split soak |

Key source files (LOC): `src/core/profile.c` 302, `src/core/spray.c` 121,
`src/core/schedule.c` 41, `src/arc/arc.c` 861, `src/codec/codec.c` 396,
`src/ctrl/controller.c` 164, `src/config/config.c` 1429, plus
`include/wam/{profile,spray,arc,codec,fluid,config,spsc,profpub,metrics,metrics_endpoint}.h`.

## 3. Asset map

Mode legend: **adopt** = copy with renaming only; **adapt** = re-shape
around the sdf ABI; **upgrade** = extend existing sdf code with the WAM
mechanism; **pattern** = import the design, not the code.

### 3.1 Dispatch core (Whack-a-Mole)

| sdf target | wam source (symbols) | Mode | ABI delta | Parity gate |
|---|---|---|---|---|
| `wm_select.c` — method + seeds | `src/core/spray.c` (`wam_theta`, `wam_spray_key`, `wam_spray_next`, wrap rotation) | upgrade (ADR-0007) | adds `wm_compile_cfg_t {method, sa, sb}` | θ(249,10)=636 golden; seed-pair sequences; discrepancy ≤ ℓ / ≤ 2ℓ property sweeps (adapt `wam_measure_discrepancy`) |
| `wm_profile.c` — embodiments 1–4 | `src/core/profile.c` (`wam_profile_update`, residual r, `wam_lookup_path`) | upgrade (ADR-0007) | adds `wm_profile_update(p, e, mode)` | embodiment goldens; Σb=m; residual fairness; last-healthy-path invariant |
| `wm_shed.c` — severity controller | `src/ctrl/controller.c` (`wam_controller_on_rx/on_feedback/tick`, severity windows, θ_hi/θ_lo, rate limit) | adapt | C fast-path controller; Python loop stays the slow authority | whack→update unit tests (adapt `test_feedback` cases) |
| `wm_shm.c` — profile publication | `include/wam/profpub.h` (seqlock publish) + `include/wam/spsc.h` | adapt (ADR-0002 layout) | none — implements the ADR-0002 segment as specified | no-torn-read contention test (adapt `test_profpub`) |
| schedules (time-varying profiles) | `src/core/schedule.c`, `include/wam/schedule.h` | defer | — | optional; WAM marks it wired but storage has no first user yet — revisit after P1 |

### 3.2 FLUID (Prototype 2)

| sdf target | wam source (symbols) | Mode | ABI delta | Parity gate |
|---|---|---|---|---|
| `fluid_round.c` — sender budget | `include/wam/fluid.h` (`wam_fluid_budget_N`, `_efficiency_ok`, `_cost_ok`, `_first_round`, admit/report state) | adapt | `fluid.h` v2 gains the sender-side budget (N = ceil(K/(1−ε))); `fg_*` coordinator gate stays | O.1 budget sweep K=1..64Ki × ε∈{0.01..0.2}; efficiency + cost identities; never-past-allowed randomized property |
| `fluid_gate.c` — coordinator gate | same math (π = L_ℓ/N identity, §4.3) | implement | keeps `fg_init/round_start/observe/done` shape | loss-product identity property; O.5-style ladder {0..50%} e2e |
| `fe_codec_nanorq.c` — RaptorQ arm | `src/codec/codec.c` (session encode/decode, block repair, ESI/tag handling) | adapt | sdf `fountain.h` is block-level (`fe_*`/`fd_*`); wrap nanorq directly, not the WAM session layer; S_n stamping not needed at this layer | roundtrip + 20% loss repair; `fountain_esi.json` vectors |
| `fe_codec_lt.c` — LT reference arm | none in WAM | **write fresh** (exception: no WAM LT exists) | none | roundtrip + loss repair + overhead recording for the A/B |

### 3.3 ARC (Prototype 3)

| sdf target | wam source (symbols) | Mode | ABI delta | Parity gate |
|---|---|---|---|---|
| `arc_offset.c` — D/E algorithms | `src/arc/arc.c` (asymmetric D update, ARC-Q E hysteresis, idle re-anchor, δ-clamp, auto-params from RTT/Δ quantiles) | adapt (ADR-0006) | `arc.h` v2: modes, param set, `arc_sizeof`, extended stats | ARC paper §12.3 goldens into `arc_schedule.json`; quantization invariants; clamp + budget alerts |
| `arc_heap.c` — release queue | `src/arc/arc.c` (min-heap keyed T_n, slab, guard G ordering, IMMEDIATE oracle) | adapt (ADR-0006) | same | guard bound; release order; L.6-style trace replay (QG: 0 violations) |
| `arc_pacer.c` / sidecar | WAM app integration pattern (poll loop, small-block bypass) | adapt | `batch_max`/`batch_deadline_ns` retained as drain knobs | small-block bypass; batch deadline bound |

### 3.4 Foundations & cross-cutting

| sdf target | wam source | Mode | ABI delta | Parity gate |
|---|---|---|---|---|
| `common/ring.c` (SPSC) | `include/wam/spsc.h` (header-only, TSan-clean) | adapt to existing `ring.h` API | none | wrap order + 2^22 two-thread soak (adapt `test_spsc`) |
| `common/seqlock.c`, `shm.c` | `profpub.h` + ADR-0002 layout | adapt | none | ADR-0002 offset map; torn-read contention |
| `common/pool.c` | WAM pool/arena patterns (`src/codec`, J.2) | pattern | none | no-malloc-after-warmup assert in benches |
| `common/config.c` (TOML) | `src/config/config.c` (K.2 strict loader) | adapt (ADR-0005) | sdf table schema `[wm]/[fluid]/[arc]/[[origin]]` | strict-schema rejection tests (adapt `test_config`) |
| `common/metrics.c` | `include/wam/metrics.h` conventions (counters, reservoir histogram) | pattern | keep sdf metric names (§11.1 overview) | series pinned by unit test |
| live scrape endpoint | `src/runtime/metrics_endpoint.c` + `metrics_endpoint.h` | adapt | `SDF_METRICS_ENDPOINT` env; HTTP GET text dump | bad-addr refusal; 200 + exact Content-Length (adapt `test_metrics_endpoint`) |
| `common/log.c`, `timer.c` | WAM util/clock helpers | pattern | none | — |
| e2e degradation harness | `tools/netns_topology.sh` (netem) | pattern → **loopback origin knobs** | origins, not veth | W1.4 harness gates |
| Makefile gates | `makefile.wam` (`ci`, `asan`, `tsan`, `bench-gate`, `stress`, `docs`) | pattern | sdf equivalents | local `ci` green before any landing |

## 4. Deliberately not imported

| WAM asset | Why not |
|---|---|
| UDP / RoCEv2 transport drivers (`src/net/*`) | Storage paths are origin endpoints with request/ completion semantics, not datagram sockets; the **vtable seam pattern** is imported instead (ADR-0008) |
| On-wire header `wam_hdr_t`, feedback PDUs | No sdf wire format yet; repair feedback is count-based at the coordinator, not per-packet |
| Session window, `wam_tx` API, pacing token bucket, playout deadlines | Transport-streaming concerns; revisit if/when P2 repair becomes a streaming pipeline (WAM's own N-phase analogue) |
| NCCL net plugin, media demo/GUI | Ecosystem-specific; no storage consumer |
| TSNC recoding (`WAM_TSNC`) | Experimental in WAM; the repair A/B does not need recoding relays — defer with WAM's own "M6 wire format" caveat |
| CRC32C trailer | Integrity belongs to the storage engine's apply path; revisit with P3 if end-to-end corruption detection is wanted |
| `wam_schedule` (time-varying profiles) | No first storage user; keep the seam (profiles are versioned) and defer |

## 5. ABI delta register

| # | Header | Delta | ADR |
|---|---|---|---|
| 1 | `arc.h` | v2: modes, full param set, `arc_sizeof`, extended stats, poll-based release | [0006](../adr/0006-arc-abi-v2.md) |
| 2 | `wm.h` | adds `wm_profile_update` + `wm_compile_cfg_t` (method/seeds); minor version bump | [0007](../adr/0007-wm-profile-updates-and-spray-seeds.md) |
| 3 | `fluid.h` | v2: sender budget (`fluid_budget_*`) alongside the coordinator gate | (design note in 02; lands with W3.4) |
| 4 | `fountain.h` | size/allocation helpers (`fe_sizeof`/`fd_sizeof`) before first codec arm ships | (closes the ADR-0001 open item; lands with W3.1) |
| 5 | `origin.h` | new public header: `sdf_origin_ops_t`, `sdf_origin_stats_t`, `sdf_fetch_req_t` | [0008](../adr/0008-origin-driver-seam.md) |
| 6 | `common/config.h` | strict TOML loader surface | [0005](../adr/0005-config-format-toml.md) |

## 6. Import acceptance rules (summary)

1. **Provenance block** in every adapted file: source repo, path, commit
   (`a0f0422` or the per-import refresh), authorizing prompt + ADR.
2. **Golden parity** per §3's parity column, wired into
   `tests/c/test_parity.c` and the `ci` gate (ADR-0009).
3. **Bridge tests** where the sdf ABI diverges mechanically from the WAM
   API (e.g. `wm_select(c, counter)` vs `wam_spray_next(s, p)`): same
   profile + same seeds ⇒ same sequence.
4. **Status honesty**: the implementation-plan ledger (08 §status) is the
   single source of completion truth; a landed import without its parity
   gate counts as 0%.

## 7. nanorq pinning note

`third_party/README.md` and `00-overview.md` §2.4 currently cite
`https://github.com/ursoid-tech/nanorq`; wack_a_mole validated against
`https://github.com/sleepybishop/nanorq` @ `stable` (submodule-pinned,
`libnanorq.a` built in-tree). Prompt W3.2 vendors nanorq **pinned to the
same commit family wack_a_mole validated** and corrects the README/URL
references in the same commit — the codec A/B must not introduce a
second, untested upstream.

## 8. Sizing

Import-led sizing (P50–P90 engineer-days per prompt) lives in
[08-implementation-plan §12](08-implementation-plan.md#12-sizes--milestones);
the import itself is what keeps the total near **60–96 engineer-days**
versus a from-scratch estimate roughly twice that.
