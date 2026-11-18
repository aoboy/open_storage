# Rust Migration Plan

**Status:** Deferred to phase 4. Directories `src/rust/sdf-fountain` and
`src/rust/sdf-arc` are placeholders.
**Related:** [Overview §9, §12](00-overview.md#9-language-strategy-c-python-rust),
[ADR-0003](../adr/0003-rust-migration-order.md).

## Contract

The migration contract is:

1. Public headers in `include/sdf/*.h`.
2. Golden vectors in `golden_vectors/`.
3. Fixed shared-memory layouts and `extern "C"` symbol names.

A Rust module replaces a C module only when it exports the same symbols from a
`staticlib` or `cdylib` and passes the same vectors. CMake links the Rust
library instead of the C object files. No caller changes, no Python changes,
no ABI version bump for a pure implementation swap.

## Rules

- Only POD types and function pointers cross the boundary. No C++ ABI, no
  exceptions, no Rust panics across `extern "C"`.
- Every exported function is documented as `unsafe` on the Rust side and
  catches/aborts on panic at the boundary.
- The Rust build must be reproducible: pinned toolchain, vendored crates, and
  a recorded `Cargo.lock`.
- Golden vectors run in CI for both implementations before the C one is
  removed.

## Order

### 1. Fountain codec (`src/rust/sdf-fountain`)

Why first: pure compute, no I/O, no shared state. Highest confidence, best
test coverage.

- Implement LT first as the reference arm; add RaptorQ only if the Nanorq A/B
  says it is worth owning.
- Validate against `golden_vectors/fountain_esi.json`.
- Run cbindgen, regenerate the header, and diff it against
  `include/sdf/fountain.h`. Any drift is an ABI change and needs an ADR.
- Link the staticlib into `sdf_fountain`; keep the C implementation behind a
  CMake option for one release.

### 2. ARC pacer (`src/rust/sdf-arc`)

Why second: heap + timing + callback. Ownership removes the heap/ordering bug
class, but the callback and deadline behavior need careful tests.

- Match the C pacer's ordering key and deadline semantics exactly.
- Replay golden schedules and compare release timestamps within tolerance.
- Run both pacers in shadow and compare per-entry release times before cutover.

### 3. Whack-a-Mole dispatch

Leave in C longest. Selection is a table lookup; the marginal safety benefit
does not justify an early migration. Revisit only if the control-plane logic
moves to Rust as a whole.

## Build integration

```text
CMake option: SDF_USE_RUST_FOUNTAIN=ON|OFF
CMake option: SDF_USE_RUST_ARC=ON|OFF
```

When enabled, the target links the Rust staticlib and skips the C sources for
that module. When disabled, the C sources are used. CI builds both variants
and runs the same tests.

## CI gates

- `cargo fmt --check`, `cargo clippy -- -D warnings`, `cargo test`.
- Golden-vector conformance for both implementations.
- cbindgen header diff: no unintended ABI changes.
- Sanitizer run (ASan/UBSan) on the Rust-backed variant.
- Benchmark parity: no p99 or throughput regression beyond the nightly noise
  threshold.

## Non-goals

- Rewriting the control plane in Rust.
- Replacing the C ABI with a Rust-native API.
- Introducing async runtimes on the hot path.
- Cross-language LTO in the first migration.

## Risks

| Risk | Mitigation |
|---|---|
| Silent ABI drift | cbindgen diff + golden vectors in CI |
| Panic crosses FFI | `catch_unwind` at every exported boundary; abort on failure |
| Rust allocator in hot path | Preallocated buffers; no allocation after init |
| Toolchain/vendor drift | Pinned toolchain and vendored crates |
| Performance regression | Nightly benchmark gate before cutover |
