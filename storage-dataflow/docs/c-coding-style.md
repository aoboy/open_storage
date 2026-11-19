# C Coding Style

The canonical machine-readable style is [`../.clang-format`](../.clang-format).
Use clang-format rather than reproducing layout by hand. CI pins clang-format
23.1.5 and clang-tidy 23.1.4 so formatting and diagnostics are repeatable.

## Required braces

Always brace C control-statement bodies, including one-statement and nested
bodies:

```c
for (uint32_t i = 0; i < degree; i++) {
    for (size_t b = 0; b < symbol_len; b++) {
        out[b] ^= source[neighbors[i]][b];
    }
}
```

This applies to `if`/`else`, `for`, `while`, and `do`. Do not submit unbraced
single-statement bodies or remove braces to shorten code. The layout otherwise
follows LLVM style with four-space indentation and a 100-column limit.

## Apply and verify

From `storage-dataflow/`:

```sh
make format                 # rewrite all selected first-party C files
./scripts/format_c.sh --check
make build                  # produces compile_commands.json for tidy
make lint                   # format, tidy, and available Python checks
make test
```

For formatter changes or tree-wide restyles, prove idempotence:

```sh
./scripts/format_c.sh --write
./scripts/format_c.sh --write
./scripts/format_c.sh --check
git diff --check
```

The second write must not add another diff and the check must pass. A successful
local check is not evidence of a hosted CI run.

## Owned scope

`scripts/format_c.sh` is the single source of the format file list. It covers
first-party `.c` and `.h` files under:

- `src/c` and `include`
- `tests/c`, `tools`, `bench`, and `cmake`

It deliberately excludes vendored `third_party/nanorq`, generated CFFI/binding
artifacts, and generated `tests/c/golden_json.h`. Do not hand-restyle generated
goldens. If a generator's own C output should change, handle that as a separate
change with golden provenance and parity review.

`scripts/tidy_c.sh` uses `build/compile_commands.json` for compiled first-party
C from the same roots. Production `src/c` retains the full curated tidy gate;
compiled tests, tools, and benchmarks receive the brace-specific style check
without silently expanding unrelated analyzer policy or inheriting compiler
`-Werror` for non-style diagnostics. Standalone generators
without compilation-database entries remain formatted but are not assigned
invented tidy compiler flags.

## Agent automation

The repository-root `opencode.jsonc` calls `format_c.sh --write-file` after
OpenCode edits to `.c` and `.h` files. This per-file mode uses the same file
list and silently leaves excluded or out-of-scope paths unchanged. `AGENTS.md`
directs agents to the reusable `.opencode/skills/c-coding-style/SKILL.md`
workflow. These conveniences do not replace the required check and test gates.
