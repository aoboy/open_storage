---
name: C Coding Style
description: Apply, review, and verify the storage-dataflow C brace and formatting style whenever creating, editing, or reviewing C sources or headers.
---

# C Coding Style

Use the repository's canonical formatter instead of hand-maintaining isolated
brace examples. All paths below are repository-root-relative.

## Workflow

1. Read `storage-dataflow/docs/c-coding-style.md` and the existing nearby code.
2. Preserve behavior, public declarations, golden data, and generated/vendored
   files. A style-only change must not alter golden expectations or ABIs.
3. After editing C, run `storage-dataflow/scripts/format_c.sh --write` from the
   repository root. OpenCode's project formatter also runs clang-format after
   its own C-file edits.
4. Inspect the diff. Always brace `if`/`else`, `for`, `while`, and `do` bodies,
   even for one statement and nested loops. Do not remove braces manually.
5. Run `storage-dataflow/scripts/format_c.sh --check`, then run the second
   formatting pass and check again when changing the formatter or performing a
   tree-wide restyle. No further diff may appear.
6. From `storage-dataflow/`, run `make build test` for ordinary C changes and
   `make ci` before a full-gate claim. Run `make lint` after `make build` so
   clang-tidy has `build/compile_commands.json`.

## Scope and exclusions

The shared script covers `src/c`, `include`, `tests/c`, `tools`, `bench`, and
`cmake`. Never bulk-format `third_party/nanorq`, `tests/c/golden_json.h`, or
generated CFFI/binding artifacts. Do not hand-edit generated headers to satisfy
the brace rule; change their generator only when that is separately intended
and regenerate them with provenance intact.

## Enforcement

- `storage-dataflow/.clang-format`: canonical layout and `InsertBraces: true`.
- `storage-dataflow/.clang-tidy`: fatal
  `readability-braces-around-statements` diagnostics on compiled first-party C.
- `make format`: write the canonical style; `make lint`: check it.
- Hosted CI calls the same scripts with pinned clang-format 23.1.5 and
  clang-tidy 23.1.4. Do not claim a hosted run until it actually executes.
