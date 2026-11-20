# Rust module: ARC

First W6.18 slice: a pure offset-estimator reference mirroring the C
Algorithm-1 update, envelope clamp, idle re-anchor, absurd-stamp gate, and
Algorithm-2 E hysteresis. It is **not** wired into the public ABI; the C pacer
remains the default and owns heap ordering, deadlines, callbacks, statistics,
and sidecar behavior.

```sh
make rust-arc
```

The crate has no dependencies and no `unsafe` code yet. Its tests reproduce
the alternating d_high/d_low fixed point from the C golden trajectory
(~94.793 ms / ~93.802 ms). Remaining W6.18 slices:

- Q/QG heap ordering, guard/deadline/wake, drain and callbacks
- `SDF_USE_RUST_ARC=ON|OFF` staticlib integration and symbol parity
- sidecar parity and installed consumer matrix
- panic containment at the future FFI boundary
