# ADR-0005: Strict TOML as the Ops Config Format

**Status:** Accepted (supersedes the YAML references in `docs/design/01–03`)
**Date:** 2026-11-18
**Related:** [Import map](../design/07-wam-import-map.md),
[Implementation plan W0.3](../design/08-implementation-plan.md),
[wack_a_mole plan §K.2]

## Context

The design docs reference `configs/<env>/{wm,fluid,arc}.yaml`. Those files are
placeholders today, and no loader exists (`src/c/common/config.c` is a stub).
Meanwhile the wack_a_mole project built and hardened a strict TOML loader
(`src/config/config.c`, ~1.4 kloc, tested by `test_config`): a hand-rolled
subset (`[table]`, `[[array]]`, homogeneous numeric arrays, comments) that
rejects unknown tables/keys, type mismatches, and out-of-range values with
hard `path:line:` diagnostics, so a typo never passes silently. Its
precedence rule (CLI flag > config file > built-in default) is already the
rule `docs/design/00-overview.md` describes for the proxy.

YAML would require either a heavyweight dependency (libyaml) or a hand-rolled
parser of a spec notorious for ambiguity (the Norway problem, implicit
typing, anchor semantics) — the exact failure mode a strict-schema loader
exists to prevent.

## Decision

1. The ops config format is **TOML**, loaded by an adaptation of the
   wack_a_mole strict loader behind `include/sdf/common/config.h`.
2. Precedence: **CLI flag > config file > built-in default**, matching the
   loader's shipped behavior.
3. The loader is **strict**: unknown keys, type mismatches, and out-of-range
   values are hard errors with `path:line:` diagnostics.
4. Example files land as `configs/{dev,staging,prod}/{wm,fluid,arc}.toml`
   (prompt W0.3 replaces the `.yaml` placeholders).
5. Each app merges only the sections the file actually set, so a partial
   file cannot clobber built-in defaults (presence-bit merge, as in the
   wack_a_mole K.2 contract).

## Consequences

Positive:

- One proven, dependency-free loader instead of a new parser to write,
  fuzz, and maintain.
- Strict-schema errors surface operator typos at load time.
- Golden config examples double as loader conformance tests.

Negative:

- YAML references in existing docs are stale until W0.3 lands; this ADR is
  the supersession record.
- TOML's expressiveness (no anchors) constrains config reuse patterns —
  acceptable for per-environment ops knobs.

## Alternatives considered

| Alternative | Why rejected |
|---|---|
| Hand-rolled strict YAML subset | Ambiguous spec; higher parser risk for no benefit |
| libyaml + validation layer | New link-time dependency on the hot-path library; error diagnostics weaker than the proven loader |
| JSON | No comments; worse ergonomics for ops runbooks |

## Compliance

- `tests/c/unit/test_config.c` (prompt W0.3) loads every shipped example,
  checks defaults, precedence, and asserts strict-schema rejections.
- A config-format change requires a new ADR; the loader is part of the
  Phase 0 foundations and precedes every prototype's rollout gates.
