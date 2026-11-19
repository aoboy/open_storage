# Storage Dataflow Stack

Applying **Whack-a-Mole**, **FLUID**, **ARC**, and **Nanorq** to large-scale
distributed storage and caching.

This repository currently contains the planning documents and a buildable
skeleton. The C headers under `include/sdf/` are the ABI contract. The
Whack-a-Mole dispatch core is implemented; the remaining C sources are stubs
to be filled in per the design documents.

See [`docs/design/00-overview.md`](docs/design/00-overview.md) for the full
design and plan.

## Quick start

```bash
./scripts/build.sh        # C libraries + optional CFFI bindings
./scripts/run_tests.sh    # C smoke tests + Python unit tests
./scripts/run_bench.sh    # benchmark harness entry point
./scripts/lint.sh         # clang-format, clang-tidy, ruff, mypy (if installed)
./scripts/build_doc.sh    # render docs/design/*.md to out/*.pdf (optional)
make ci                   # local gate: lint (+clean skips), test, ASan+UBSan, TSan
```

Requirements: CMake >= 3.24, a C17 compiler, and Python >= 3.10. The FFI,
lint, documentation, and benchmark steps are optional and skip cleanly when
their tooling is not installed.

## Layout

| Path | Purpose |
|---|---|
| `include/sdf/` | Public C ABI (the migration contract) |
| `src/c/` | Data-plane hot path (C) |
| `python/sdf/` | Control plane (Python) |
| `src/rust/` | Phase 4 Rust modules (empty until then) |
| `tests/` | Unit, integration, fuzz, and system tests |
| `golden_vectors/` | Cross-language ABI conformance vectors |
| `docs/design/` | Design and planning documents |
| `docs/adr/` | Architecture decision records |
| `docs/ops/` | Runbooks and dashboards |
| `configs/` | Per-environment configuration |

## Status

| Component | State |
|---|---|
| Public C ABI headers | Defined |
| CMake build | Working (libraries, 10-test suite, benches) |
| Whack-a-Mole dispatch (P1) | **As-built (Wave 2)**: seeded spray (280/280 goldens), embodiments + controller, SHM publication + Python loop, proxy + e2e gates |
| Whack-a-Mole control plane | Python loop over the live segment (no-opinion + down_mask deference) |
| Common primitives, strict TOML config, metric primitives | Implemented — plan Wave 0 (W0.2–W0.5) |
| Fuzzers | **As-built (Wave 5)**: fountain streams, embodiment vectors, ARC release streams — live bug find on first run (the QG park stranding) |
| Packaging | **As-built (Wave 5)**: install + pkg-config + find_package legs in ci; version.h single source of truth |
| Rust seam | Skeleton crate + cbindgen declaration-diff gate (advisory without cargo; no functional Rust) |
| Known gaps (owned) | Live scrape endpoint not yet wired; arc_schedule.json schema-only (12.3 goldens pinned in C); no live C-proxy→SHM→Python chain yet — all three close in **Wave 6** ([plan §14](docs/design/08-implementation-plan.md#14-phase-w6--production-hardening--the-migration-begins)) |
| Golden-vector parity gate | Active: 152 comparisons green, seed groups pending W2.1 (ADR-0009) |
| Origin driver seam | **As-built (Wave 1)**: loopback harness driver + HTTP reference driver + loadgen with 4 degradation profiles (ADR-0008) |
| SHM / CFFI integration | CFFI build wired; SHM bridge is a stub |
| Fountain codecs (P2) | **As-built (Wave 3)**: nanorq (RaptorQ) + LT arms behind one ABI, 356 golden comparisons |
| FLUID repair (P2) | **As-built (Wave 3)**: budget + gate on the section-4.3 identity, coordinator with mandatory ARQ fallback, three-arm loss ladder green |
| ARC pacing sidecar (P3) | **As-built (Wave 4)**: ARC-QG core on the paper's analytic fixed point, shadow/live/killed sidecar, deploy gate + armed kill switch, runbook executed as a test |
| Rust modules | Deferred to phase 4 |

## Documentation

- [`docs/design/00-overview.md`](docs/design/00-overview.md) — design baseline (three planes, prototypes, language strategy)
- [`docs/design/01-whack-a-mole.md`](docs/design/01-whack-a-mole.md) — prototype 1
- [`docs/design/02-fluid-repair.md`](docs/design/02-fluid-repair.md) — prototype 2
- [`docs/design/03-arc-pacing.md`](docs/design/03-arc-pacing.md) — prototype 3
- [`docs/design/04-rust-migration.md`](docs/design/04-rust-migration.md) — migration plan
- [`docs/design/05-observability.md`](docs/design/05-observability.md) — metrics, config, CI
- [`docs/design/06-nanorq-evaluation.md`](docs/design/06-nanorq-evaluation.md) — codec evaluation
- [`docs/design/07-wam-import-map.md`](docs/design/07-wam-import-map.md) — provenance: what is imported from wack_a_mole, and the acceptance rules
- [`docs/design/08-implementation-plan.md`](docs/design/08-implementation-plan.md) — **the executable backlog** (waves W0–W5, prompts, sizes, status ledger)
- [`docs/design/09-origin-driver-seam.md`](docs/design/09-origin-driver-seam.md) — the `sdf_origin_ops_t` contract (origins are the paths)
- [`docs/adr/`](docs/adr/) — decision records 0001–0009

History and landing discipline (commit prefixes, doc-sync protocol,
evidence rules): [`../CONTRIBUTING.md`](../CONTRIBUTING.md). Ops config
format is strict TOML per
[ADR-0005](docs/adr/0005-config-format-toml.md) — the `.yaml`
placeholders under `configs/` are superseded and land as `.toml` with
plan prompt W0.3.
