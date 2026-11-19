# Project Agent Instructions

## C coding style

- For any C/C-header edit or review under `storage-dataflow/`, load the
  `c-coding-style` skill and follow `storage-dataflow/docs/c-coding-style.md`.
- Always brace C control-statement bodies, including single-statement nested
  loops and conditionals. `.clang-format` and `.clang-tidy` enforce this rule.
- After C edits, run `storage-dataflow/scripts/format_c.sh --write` from the
  repository root, then inspect the diff for unintended changes.
- Do not restyle vendored `third_party/` code or generated files, especially
  `tests/c/golden_json.h` and generated CFFI artifacts.

## Validation

- Before completing C work, run `storage-dataflow/scripts/format_c.sh --check`.
  Formatting twice followed by `--check` must produce no further changes.
- From `storage-dataflow/`, run `make build test` for C changes and `make ci`
  for the complete local gate. Report skipped or unavailable hosted legs
  honestly; never claim hosted CI, sanitizer, or performance evidence that was
  not executed.
