# Storage Dataflow Stack

Applying **Whack-a-Mole**, **FLUID**, **ARC**, and **Nanorq** to large-scale
distributed storage and caching.

This repository contains the design corpus and the W0-W6 implementation:
the C data plane (Whack-a-Mole dispatch, FLUID repair, ARC pacing), the
Python control plane, the live SHM chain, golden-vector parity, the fuzz
and packaging legs, and an opt-in Rust LT fountain arm behind the unchanged C
ABI. The C headers under `include/sdf/` are the ABI contract.

The implementation lives in `storage-dataflow/`; run the commands below from
that directory.

See [`docs/design/00-overview.md`](/storage-dataflow/docs/design/00-overview.md) for the full
design and plan.

## Quick start

```bash
./scripts/build.sh        # C libraries + optional CFFI bindings
./scripts/run_tests.sh    # C smoke tests + Python unit tests
./scripts/run_bench.sh    # benchmark harness entry point
./scripts/lint.sh         # clang-format, clang-tidy, ruff, mypy (if installed)
./scripts/build_doc.sh    # render docs/design/*.md to out/*.pdf (optional)
./scripts/build_wheel.sh  # installable Python/CFFI wheel (see packaging/wheel)
make ci                   # lint, C tests/sanitizers, install/deb/wheel, e2e, Rust gates
make e2e                  # cache-fill, repair, apply, and live SHM e2e harnesses
```

Requirements: CMake >= 3.24, a C17 compiler, and Python >= 3.10. The FFI,
lint, documentation, and benchmark steps are optional and skip cleanly when
their tooling is not installed. The Rust gates need cargo; the hosted matrix
pins Rust 1.100.0 with rustfmt/clippy, while local Rust legs report clean skips
when cargo/components are absent. Configure one functional Rust build with
`cmake -S . -B build-rust -DSDF_USE_RUST_FOUNTAIN=ON`.

## Layout

| Path | Purpose |
|---|---|
| `include/sdf/` | Public C ABI (the migration contract) |
| `src/c/` | Data-plane hot path (C) |
| `python/sdf/` | Control plane (Python) |
| `src/rust/` | Rust LT fountain arm + full declaration/cbindgen seam (C arm retained for A/B) |
| `tests/` | Unit, integration, fuzz, and system tests |
| `golden_vectors/` | Cross-language ABI conformance vectors |
| `docs/design/` | Design and planning documents |
| `docs/adr/` | Architecture decision records |
| `docs/ops/` | Runbooks and dashboards |
| `configs/` | Per-environment configuration |

## Status

| Component | State |
|---|---|
| Latest release | **v1.0.0 (W6.6)** — [release notes](/storage-dataflow/docs/releases/v1.0.0.md), ledger-derived evidence, source tag and Debian development package |
| Public C ABI headers | Defined |
| CMake build | Working (libraries, 28-test suite, benches, fuzzers) |
| Whack-a-Mole dispatch (P1) | **As-built (Wave 2)**: seeded spray (280/280 goldens), embodiments + controller, SHM publication + Python loop, proxy + e2e gates |
| Whack-a-Mole control plane | Python loop over the live segment (no-opinion + down_mask deference) |
| Control-plane service | **As-built (W6.17 partial)**: `sdf serve` loads TOML, ticks at `control_hz`, attaches WM via `SDF_WM_SHM` and ARC via `SDF_ARC_SIDECAR`, writes atomic health JSON with ARC/FLUID policy, handles SIGINT/SIGTERM, exposes health through `sdf status`, and runs from the installed wheel; RPM/Helm remain pending |
| FLUID control plane | **As-built**: per-class epsilon policies, sender budget and cumulative-loss gate matching the C helpers, deterministic xxhash64 arm assignment, and Pareto tracking; pure-Python XXH64 fallback removed the undeclared `xxhash` import dependency (116 Python tests) |
| Common primitives, strict TOML config, metric primitives | Implemented — plan Wave 0 (W0.2–W0.5) |
| Python simulators | **As-built**: `wm_sim` (plain spray, golden-validated), `fluid_sim` (budget/gate loss ladder), and `arc_sim` (ARC offset/E hysteresis) |
| Fuzzers | **As-built (Wave 5)**: fountain streams, embodiment vectors, ARC release streams — live bug find on first run (the QG park stranding) |
| Packaging | **As-built (Wave 5/W6.4/W6.6/W6.8/W6.16)**: install + pkg-config + find_package + installed-CFFI legs in ci; version 1.0.0 from version.h; the installed surface includes PIC `libnanorq.a`; `libsdf-dev_1.0.0_amd64.deb` built, dpkg-installed and consumer-tested with both C and Rust LT; the isolated Python/CFFI wheel is built and exercised in a fresh venv locally for both OFF and Rust-ON variants, and the OFF wheel also passed hosted run 44774290565; the hosted wheel job now has an OFF/ON matrix, with the ON leg not yet executed (rpm/Helm remain placeholders) |
| Rust seam + LT | **As-built (W6.5/W6.8)**: full v2 ABI declared (15 functions / 5 typedefs / 2 structs / 1 enum), cbindgen 0.27 + parsed declaration diff; functional opt-in Rust LT with C dispatch/nanorq retained and exact archive symbols. Hosted rust-seam run 44774290565 at `0d6e60a` ran both variants against the current 28-test/1,410-golden tree |
| Live metrics scrape | **As-built (W6.1)**: SDF_METRICS_ENDPOINT serves the proxy + ARC sidecar live (the wam K.1b pattern) |
| Live control chain | **As-built (W6.1)**: C-proxy → SHM telemetry → Python loop → published profile → acquired — proven as a live process pair |
| CI | **W6.5/W6.8/W6.16**: 9-job matrix (style/tidy, gcc+clang, sanitizers, e2e, bench gate, install, isolated wheel, authoritative Rust quality + C/Rust functional A/B, full fuzz) + nightly p99 gate; hosted run 44774290565 at `0d6e60a` is green (10/10 matrix jobs); nightly history still pending |
| Known gaps (owned) | Retained nightly CI history is still pending (the hosted matrix is green at `0d6e60a`, W6.5); Rust LT remains opt-in with C as the comparison-window default because paired performance and seven nightly samples are still missing (W6.8/W6.13) — [plan §14](/storage-dataflow/docs/design/08-implementation-plan.md#14-phase-w6--production-hardening--the-migration-begins). True ARC shadow scoring is no longer a gap: W6.10 runs the non-applying QG schedule with separate evidence and a non-vacuity promotion gate ([design 03](/storage-dataflow/docs/design/03-arc-pacing.md#implementation-note--true-shadow-scoring-w69w610)); W6.9 closes accepted-entry retention under pacer backpressure |
| Golden-vector parity gate | Active: **1,410 comparisons, zero pending** — WM, both fountain arms, ARC §12.3 (W6.2/W6.11; ADR-0009) |
| Origin driver seam | **As-built (Wave 1)**: loopback harness driver + HTTP reference driver + loadgen with 4 degradation profiles (ADR-0008) |
| SHM / CFFI integration | **As-built (W6.4)**: CFFI covers origin → wm → arc → fluid → fountain, links the PIC archives, and builds against an installed prefix (`SDF_CFFI_DIR` override); `CffiSidecarTransport` drives the live C ARC sidecar (mode flips in one pump); SHM bridge live (W6.1 process-pair chain) |
| Fountain codecs (P2) | **As-built (Wave 3/W6.8/W6.11)**: nanorq (RaptorQ) + LT arms behind one ABI; LT is selectable as C (default) or Rust with no caller change; the C parity gate covers 1,410 comparisons; hosted rust-seam ran both variants against that set at `0d6e60a` |
| FLUID repair (P2) | **As-built (Wave 3/W6.3)**: budget + gate on the section-4.3 identity, coordinator with mandatory per-block ARQ fallback, three-arm loss ladder green; N-block objects (≤64) with per-block codec/budget/gate, deficit-ranked admission, per-block round accounting, and a **byte-exact** decode gate |
| ARC pacing sidecar (P3) | **As-built (Wave 4/W6.4/W6.9/W6.10)**: ARC-QG core on the paper's analytic fixed point, shadow/live/killed sidecar with ABI mode setters (one-pump flips, killed drains the backlog), accepted-entry retention under pacer backpressure, true non-applying shadow scoring with epoch-scoped schedule/immediate evidence and per-entry records, non-vacuity promotion gate, armed kill switch, runbook executed as a test |
| Rust modules | Fountain LT functional and opt-in (W6.8 comparison window); ARC migration remains deferred |

## Documentation

- [`docs/design/00-overview.md`](/storage-dataflow/docs/design/00-overview.md) — design baseline (three planes, prototypes, language strategy)
- [`docs/design/01-whack-a-mole.md`](/storage-dataflow/docs/design/01-whack-a-mole.md) — prototype 1
- [`docs/design/02-fluid-repair.md`](/storage-dataflow/docs/design/02-fluid-repair.md) — prototype 2
- [`docs/design/03-arc-pacing.md`](/storage-dataflow/docs/design/03-arc-pacing.md) — prototype 3
- [`docs/design/04-rust-migration.md`](/storage-dataflow/docs/design/04-rust-migration.md) — migration plan
- [`docs/design/05-observability.md`](/storage-dataflow/docs/design/05-observability.md) — metrics, config, CI
- [`docs/design/06-nanorq-evaluation.md`](/storage-dataflow/docs/design/06-nanorq-evaluation.md) — codec evaluation
- [`docs/design/07-wam-import-map.md`](/storage-dataflow/docs/design/07-wam-import-map.md) — provenance: what is imported from wack_a_mole, and the acceptance rules
- [`docs/design/08-implementation-plan.md`](/storage-dataflow/docs/design/08-implementation-plan.md) — **the executable backlog** (waves W0–W5, prompts, sizes, status ledger)
- [`docs/design/09-origin-driver-seam.md`](/storage-dataflow/docs/design/09-origin-driver-seam.md) — the `sdf_origin_ops_t` contract (origins are the paths)
- [`docs/releases/v1.0.0.md`](/storage-dataflow/docs/releases/v1.0.0.md) — v1.0.0 release notes, verification evidence and known limitations
- [`docs/adr/`](/storage-dataflow/docs/adr/) — decision records 0001–0009

History and landing discipline (commit prefixes, doc-sync protocol,
evidence rules): [`CONTRIBUTING.md`](/CONTRIBUTING.md). Ops config
format is strict TOML per
[ADR-0005](/storage-dataflow/docs/adr/0005-config-format-toml.md) — the `.yaml`
placeholders under `configs/` are superseded and land as `.toml` with
plan prompt W0.3.
