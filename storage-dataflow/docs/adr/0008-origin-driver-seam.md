# ADR-0008: Origin Driver Seam (`sdf_origin_ops_t`)

**Status:** Accepted
**Date:** 2026-11-18
**Related:** [Origin driver seam design](../design/09-origin-driver-seam.md),
[Overview §5.3](../design/00-overview.md#53-origin-driver-seam-new),
[Implementation plan W1](../design/08-implementation-plan.md),
[wack_a_mole `include/wam/transport.h` (vtable pattern)]

## Context

In this system the "paths" of Whack-a-Mole dispatch are **origins** — S3
blobs, CDN edges, P2P peers, colocated clusters — not network interfaces.
`docs/design/00-overview.md` describes dispatch "over origins weighted by
observed throughput, error rate, and queue depth", but no seam exists: the
proxy cannot talk to origins, and the controller has no normalized signal
source to score. The wack_a_mole project solved the analogous problem for
packet transports with one vtable (`wam_transport_ops_t`: open / send /
poll_rx / path_stats / close) and normalized per-path stats
(`wam_path_stats_t`) that the severity controller consumes
transport-invariantly.

## Decision

1. A single **origin vtable** is the only origin I/O seam in the data plane:

   ```c
   typedef struct sdf_origin sdf_origin_t;

   typedef struct {
       int  (*open)(sdf_origin_t *o, const sdf_origin_cfg_t *cfgs, uint32_t n);
       int  (*fetch)(sdf_origin_t *o, const sdf_fetch_req_t *req,
                     sdf_fetch_done_fn cb, void *ud);   /* one unit */
       int  (*stats)(sdf_origin_t *o, uint32_t origin_id, sdf_origin_stats_t *st);
       int  (*set_enable)(sdf_origin_t *o, uint32_t origin_id, int up);
       void (*close)(sdf_origin_t *o);
   } sdf_origin_ops_t;
   ```

   One unit = one byte-range request of a fill/repair job; the dispatcher
   (`wm_select`) assigns units to origins exactly as WAM assigns packets to
   network paths.

2. **Normalized stats** (`sdf_origin_stats_t`) are the controller's only
   input: requests in/out, bytes, EWMA latency, error rate, in-flight,
   `hard_down` — the storage analogue of `wam_path_stats_t`. No driver
   leaks its own types upward; the severity controller and the Python
   loop stay driver-invariant.
3. **Driver set:** loopback-file (tests, with throttle/loss/error
   injection knobs — the netem analogue), HTTP range GET (POSIX sockets,
   `SDF_HAVE_CURL` optional), S3/blob (later, behind the same ops),
   NVMe-peer (later). The seam is the contract; drivers are plugins.
4. **Link rule** (mirroring the WAM dual-transport rule): wm dispatch,
   controller, FLUID, and ARC never include a driver header — only
   `include/sdf/origin.h`. `wm_mark_down` remains the C fast path for a
   hard-failing origin; `set_enable` is the controlled drain/restore.
5. New header `include/sdf/origin.h`; drivers live in
   `src/c/origin/{loopback,http,s3,peer}/`.

## Consequences

Positive:

- Prototype 1 (cache-fill proxy) and Prototype 2 (repair coordinator) are
  built once over one seam; the degraded-origin e2e harness is a driver
  knob, not a test-network setup.
- The Python controller scores the same stats shape regardless of origin
  class; A/B and rollout gates are uniform.
- Future origins (S3 multipart, P2P) are additive, ADR-free at the seam
  level (each still needs its own design note).

Negative:

- One more public header in the ABI surface to keep stable (ADR-0001
  discipline applies).
- Async fetch completion (callback) forces the proxy to own a completion
  model; the loopback driver keeps this honest in tests from day one.

## Alternatives considered

| Alternative | Why rejected |
|---|---|
| Import the UDP/RoCE transport vtable wholesale | Storage units are range requests with completion semantics, not datagrams; reusing the wire header would mismodel the domain |
| Blocking per-origin fetch in the proxy main loop | Serialized origins reintroduce the head-of-line blocking the dispatcher exists to remove |
| Python performs origin I/O | Data plane in Python violates the layer rule (00-overview §7.1); SHM is for control, not bytes |

## Compliance

- `tests/c/unit/test_origin_loopback.c` (prompt W1.2) pins per-origin
  stats exactness and injection knobs.
- `tests/c/integration/test_wm_proxy.c` (prompt W2.5) drives the proxy
  across mixed drivers and asserts per-origin dispatch counts against the
  compiled profile.
- The link rule is enforced by review + include-path hygiene in CI lint.
