# Contributing — Commit & Documentation Discipline

This repository runs on the workflow proven in the sibling
`wack_a_mole` repository (223 commits, docs-first): small, tagged,
traceable landings where **documentation upkeep is a first-class
activity** (that repo's history has more `docs:` commits than `src:`).
The executable backlog is
[`storage-dataflow/docs/design/08-implementation-plan.md`](storage-dataflow/docs/design/08-implementation-plan.md)
("the plan"); its status ledger is the single source of completion
truth.

## 1. Commit message prefixes

Every commit starts with a tag, then a lowercase imperative summary
(`tag: area — what landed`):

| Prefix | Use for |
|---|---|
| `plan:` | adding/amending a prompt in the plan (before implementation) |
| `src:` | C/Python implementation landings |
| `include:` | public ABI header changes (must cite the ADR) |
| `tests:` | test-only landings |
| `configs:` | example/env config files |
| `tools:`, `scripts:`, `bench:` | harness, build, benchmark landings |
| `ci:` | CI/gate wiring |
| `docs:` | design docs, ADRs, status refreshes, truth-passes |
| `research:` | papers and research notes |
| `meta:` | repo scaffolding, ignore rules, non-code plumbing |

Rules:

- Reference the plan prompt ID in every code commit, e.g.
  `src: W3.4 — FLUID sender budget (O.1 bounds adapted)`.
- Cite the authorizing ADR for ABI changes, e.g.
  `include: arc.h v2 per ADR-0006`.
- Exceptions to import-first (import map §1) must cite the import map,
  e.g. `src: W3.3 — LT reference arm (import map §3.2 exception: no WAM LT)`.

## 2. Plan-first protocol

1. A work package is `plan:`-committed (or amended) **before** the
   implementation lands — the prompt is the contract.
2. Implementation commits cite the prompt ID.
3. Acceptance must be mechanically checkable from the repo: tests,
   golden vectors, gate output. No "done" without evidence.

## 3. Doc-sync protocol

- After each landing: a `docs: <ID> outcome recorded, next = <ID>`
  commit updates the plan's ledger (per-wave %) and the owning design
  doc's status header.
- Per milestone: a `docs: truth-pass` commit syncs **every** status
  claim against the tree — stale claims are bugs.
- The plan's ledger header is pinned to a commit hash
  (`as of <hash>`), refreshed in its own `plan: ledger header refresh`
  commit.

## 4. Evidence rules

Completion claims cite test names, suite counts, bench numbers, or
commit hashes — never adjectives. Examples of the expected form
(from wack_a_mole): "48/48 ctest", "spray 7.4/15 cyc", "ARC-QG 0 order
violations, p95 12.4 ms (L.6 replay)", "done (`533c426`)".

## 5. Branching

- `main` is always green (`make ci` from a clean clone).
- Wave branches (`wack/w2-dispatch` style) merge into `main` with their
  final doc-sync commit; merges that exist to reconcile a doc sync are
  legitimate (that is the wack_a_mole pattern) but squash-free and
  traceable.

## 6. C coding style

Follow [`storage-dataflow/docs/c-coding-style.md`](storage-dataflow/docs/c-coding-style.md).
Always brace C control-statement bodies, including single-statement nested
loops and conditionals. From `storage-dataflow/`, apply the canonical style
with `make format` and verify it with `./scripts/format_c.sh --check`. The same
script drives local lint and CI; do not bulk-format vendored or generated files.
OpenCode contributors also have the project `c-coding-style` skill and automatic
C formatter configuration.

## 7. Gates before every landing

From `storage-dataflow/`: `make build test`; `make lint` when touched
paths are linted; `make asan`/`tsan` for concurrency-touching prompts
(W0.2, W2.3, W4.3+); `make bench` recorded (never gated) when a prompt's
threshold activates. CI mirrors these.

## 8. Import-specific rules (from ADR-0009 and the import map)

- Every imported/adapted file carries a provenance block: source repo,
  path, commit, authorizing prompt + ADR.
- Every import prompt is accepted only when its golden-vector parity
  gate runs in `tests/c/test_parity.c` (CI).
- Where the sdf ABI diverges mechanically from the source API, a bridge
  test asserts behavioral identity (same inputs ⇒ same sequences).
