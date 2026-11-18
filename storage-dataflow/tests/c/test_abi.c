#include <stddef.h>
#include <stdint.h>

#include "sdf/common/metrics.h"
#include "sdf/common/ring.h"
#include "sdf/common/seqlock.h"
#include "sdf/common/shm.h"
#include "sdf/origin.h"
#include "sdf/sdf.h"

/*
 * Compile-time ABI checks. These catch accidental layout changes in the
 * public structs that cross the C/Rust/Python boundary.
 */
_Static_assert(WM_MAX_PATHS == 64, "WM_MAX_PATHS is part of the ABI");
_Static_assert(WM_MAX_UNITS == 1024, "WM_MAX_UNITS is part of the ABI");
_Static_assert(offsetof(wm_profile_t, version) == 0, "wm_profile_t.version offset");
_Static_assert(offsetof(wm_profile_t, m) == 4, "wm_profile_t.m offset");
_Static_assert(offsetof(wm_profile_t, n) == 8, "wm_profile_t.n offset");
_Static_assert(offsetof(wm_profile_t, b) == 12, "wm_profile_t.b offset");
_Static_assert(offsetof(wm_compiled_t, down_mask) == 16, "wm_compiled_t.down_mask offset");
_Static_assert(offsetof(wm_compiled_t, cdf) == 24, "wm_compiled_t.cdf offset");

_Static_assert(sizeof(sdf_seqlock_t) == 64, "seqlock occupies one cacheline");
_Static_assert(offsetof(spsc_ring_t, slots) == 192, "SPSC ring header layout");

_Static_assert(SDF_SHM_OFF_SEQLOCK == 0x0000u, "SHM seqlock offset");
_Static_assert(SDF_SHM_OFF_PROFILE_A == 0x0040u, "SHM profile A offset");
_Static_assert(SDF_SHM_OFF_PROFILE_B == 0x0800u, "SHM profile B offset");
_Static_assert(SDF_SHM_OFF_TELEMETRY_HDR == 0x0FC0u, "SHM telemetry header offset");
_Static_assert(SDF_SHM_OFF_TELEMETRY_RING == 0x1000u, "SHM telemetry ring offset");

/* Origin driver seam (ADR-0008, plan W1.1). */
_Static_assert(SDF_ORIGIN_MAX == WM_MAX_PATHS, "origin set matches WM paths");
_Static_assert(SDF_ORIGIN_LOOPBACK == 0 && SDF_ORIGIN_HTTP == 1 &&
                   SDF_ORIGIN_S3 == 2 && SDF_ORIGIN_PEER == 3,
               "origin class enum is the TOML cls vocabulary");
_Static_assert(sizeof(sdf_fetch_req_t) == 32, "fetch request layout");
_Static_assert(offsetof(sdf_fetch_req_t, origin_id) == 0, "req.origin_id offset");
_Static_assert(offsetof(sdf_fetch_req_t, flags) == 4, "req.flags offset");
_Static_assert(offsetof(sdf_fetch_req_t, job_id) == 8, "req.job_id offset");
_Static_assert(offsetof(sdf_fetch_req_t, obj_offset) == 16, "req.obj_offset offset");
_Static_assert(offsetof(sdf_fetch_req_t, len) == 24, "req.len offset");
_Static_assert(sizeof(sdf_origin_cfg_t) == 304, "origin cfg layout");
_Static_assert(offsetof(sdf_origin_cfg_t, dst) == 8, "cfg.dst offset");
_Static_assert(offsetof(sdf_origin_cfg_t, max_inflight) == 296, "cfg.max_inflight offset");
_Static_assert(offsetof(sdf_origin_stats_t, in_flight) == 48, "stats.in_flight offset");
_Static_assert(offsetof(sdf_origin_stats_t, hard_down) == 80, "stats.hard_down offset");
_Static_assert(offsetof(sdf_origin_t, impl) == 8, "driver impl offset");
_Static_assert(sizeof(sdf_origin_ops_t) == 5 * sizeof(void *), "vtable is 5 ops");

int main(void) {
    return 0;
}
