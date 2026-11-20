# Rust module: ARC

First W6.18 slices: a pure offset-estimator reference mirroring the C
Algorithm-1 update, envelope clamp, idle re-anchor, absurd-stamp gate, and
Algorithm-2 E hysteresis, the stable `(T_n, seq)` release-queue ordering, and
the QG guard/deadline drain (predecessor re-queue, park timeout, successor
wake), and release statistics (heap depth, added-delay percentiles, interval
error), slab storage with an apply callback and slot reuse, and the
shadow/live/killed sidecar apply semantics. `ffi.rs` declares the complete
public ARC ABI v2 surface; cbindgen regenerates the Rust view and the gate
diffs it against `include/sdf/arc.h`. The crate builds a staticlib for a
future opt-in CMake swap, but the exported symbols are still C-owned: the C
pacer remains the default.

```sh
make rust-arc
```

The crate's only build dependency is pinned `cbindgen`; declarations are
`extern "C"` with no Rust-side `unsafe` implementation yet. Its tests
reproduce the alternating d_high/d_low fixed point from the C golden
trajectory (~94.793 ms / ~93.802 ms). The seam gate reports 20 functions,
7 typedefs, 4 structs, 1 enum, and the apply-callback signature identical; a
second gate compiles one probe against each header and compares 69
`sizeof`/`offsetof`/enum/define observations plus 6 type sizes.
Remaining W6.18 slices:

- `SDF_USE_RUST_ARC=ON|OFF` definition swap and symbol parity
- sidecar production parity and installed consumer matrix
- panic containment at the Rust implementation boundary
