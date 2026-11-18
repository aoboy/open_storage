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
| CMake build | Working (libraries compile) |
| Whack-a-Mole dispatch core | Implemented: profile compile, selection, mark down/up |
| Whack-a-Mole control plane | Reference implementation in Python |
| SHM / CFFI integration | CFFI build wired; SHM bridge is a stub |
| Fountain / FLUID / ARC runtime | Stubs — import-led per [`docs/design/07-wam-import-map.md`](docs/design/07-wam-import-map.md); execution backlog in [`08`](docs/design/08-implementation-plan.md) |
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
