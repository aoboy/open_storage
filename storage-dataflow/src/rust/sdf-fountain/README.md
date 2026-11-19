# Rust module: fountain (W6.8)

The crate implements the **systematic LT arm** behind the existing C ABI.
`fe_enc.c`/`fe_dec.c` still own the public `fe_*`/`fd_*` dispatch and the
vendored nanorq adapter remains C. `src/lt.rs` replaces only
`fe_codec_lt.c + fe_prng.c` when CMake is configured with
`SDF_USE_RUST_FOUNTAIN=ON`; the C files remain the default during the
comparison window.

The internal `lt_*` functions preserve the C caller-owned layouts and return
codes. Encoding/decoding allocates no heap memory; every panic-capable export
catches unwinding and aborts rather than crossing the C boundary. cbindgen
excludes these internal symbols from the public generated header, whose
15-function / 5-typedef / 2-struct / 1-enum declaration diff stays unchanged.

```sh
# Declaration gate + Rust unit tests
./scripts/test_rust_seam.sh --require

# Both full C suites, byte-identical 556-golden reports, exact archive symbols,
# Rust-backed ASan+UBSan, installed CFFI/pkg-config/CMake and Debian consumers
./scripts/test_rust_fountain.sh --require

# One opt-in build
cmake -S . -B build-rust -DSDF_USE_RUST_FOUNTAIN=ON
cmake --build build-rust --parallel
ctest --test-dir build-rust --output-on-failure
```

Cargo builds both an `rlib` (unit tests) and a `staticlib` (CMake). The build
folds the staticlib into `libsdf_fountain.a`; consumers do not link a separate
Rust archive. The variant-specific native support libraries are configured
into pkg-config and `find_package(sdf)`. See
`docs/design/04-rust-migration.md` and ADR-0003.
