# Rust module: ARC

First W6.18 slices: a pure offset-estimator reference mirroring the C
Algorithm-1 update, envelope clamp, idle re-anchor, absurd-stamp gate, and
Algorithm-2 E hysteresis, the stable `(T_n, seq)` release-queue ordering, and
the QG guard/deadline drain (predecessor re-queue, park timeout, successor
wake), and release statistics (heap depth, added-delay percentiles, interval
error). It is **not** wired into the public ABI; the C pacer remains the default
and owns callbacks, slab storage, and sidecar behavior.

```sh
make rust-arc
```

The crate has no dependencies and no `unsafe` code yet. Its tests reproduce
the alternating d_high/d_low fixed point from the C golden trajectory
(~94.793 ms / ~93.802 ms). Remaining W6.18 slices:

- callbacks and slab storage
- `SDF_USE_RUST_ARC=ON|OFF` staticlib integration and symbol parity
- sidecar parity and installed consumer matrix
- panic containment at the future FFI boundary
