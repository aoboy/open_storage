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
_Static_assert(offsetof(wm_profile_t, residual_r) == 268,
               "wm_profile_t.residual_r offset (ADR-0007)");
_Static_assert(sizeof(wm_profile_t) == 276, "wm_profile_t size");
_Static_assert(offsetof(wm_compiled_t, down_mask) == 16, "wm_compiled_t.down_mask offset");
_Static_assert(offsetof(wm_compiled_t, cdf) == 24, "wm_compiled_t.cdf offset");
/* ADR-0007 layout: method/sa/sb + path_of_key appended after the
 * counter table; sizes computed, never guessed. */
_Static_assert(offsetof(wm_compiled_t, perm) == 280, "wm_compiled_t.perm offset");
_Static_assert(offsetof(wm_compiled_t, unit_to_path) == 4376, "unit_to_path offset");
_Static_assert(offsetof(wm_compiled_t, method) == 8472, "wm_compiled_t.method offset");
_Static_assert(offsetof(wm_compiled_t, sa) == 8476, "wm_compiled_t.sa offset");
_Static_assert(offsetof(wm_compiled_t, sb) == 8480, "wm_compiled_t.sb offset");
_Static_assert(offsetof(wm_compiled_t, path_of_key) == 8484, "path_of_key offset");
_Static_assert(sizeof(wm_compiled_t) == 12584, "wm_compiled_t size (8-aligned)");

_Static_assert(sizeof(sdf_seqlock_t) == 64, "seqlock occupies one cacheline");
_Static_assert(offsetof(spsc_ring_t, slots) == 192, "SPSC ring header layout");

_Static_assert(SDF_SHM_OFF_SEQLOCK == 0x0000u, "SHM seqlock offset");
_Static_assert(SDF_SHM_OFF_PROFILE_A == 0x0040u, "SHM profile A offset");
_Static_assert(SDF_SHM_OFF_PROFILE_B == 0x0800u, "SHM profile B offset");
_Static_assert(SDF_SHM_OFF_TELEMETRY_HDR == 0x0FC0u, "SHM telemetry header offset");
_Static_assert(SDF_SHM_OFF_TELEMETRY_RING == 0x1000u, "SHM telemetry ring offset");

/* Origin driver seam (ADR-0008, plan W1.1). */
_Static_assert(SDF_ORIGIN_MAX == WM_MAX_PATHS, "origin set matches WM paths");
_Static_assert(SDF_ORIGIN_LOOPBACK == 0 && SDF_ORIGIN_HTTP == 1 && SDF_ORIGIN_S3 == 2 &&
                   SDF_ORIGIN_PEER == 3,
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

/* Fountain ABI v2 (plan W3.1; ADR-0001's opaque-size item closes). */
_Static_assert(offsetof(fe_cfg_t, k) == 8, "fe_cfg_t.k offset");
_Static_assert(offsetof(fe_cfg_t, overhead_pct) == 12, "fe_cfg_t.overhead_pct offset");
_Static_assert(offsetof(fe_cfg_t, codec) == 16, "fe_cfg_t.codec offset (ADR-0004 arm)");
_Static_assert(sizeof(fe_cfg_t) == 24, "fe_cfg_t size");
_Static_assert(offsetof(fd_cfg_t, sym_len) == 8, "fd_cfg_t.sym_len offset");
_Static_assert(sizeof(fd_cfg_t) == 16, "fd_cfg_t size");

/* ARC shadow evidence (W6.10): additive public structs. Existing
 * arc_cfg_t/arc_stats_t layouts are intentionally unchanged. */
_Static_assert(ARC_SHADOW_RECORD_CAP == 256, "shadow record retention is ABI-visible");
_Static_assert(offsetof(arc_shadow_record_t, block_n) == 0, "shadow record.block_n offset");
_Static_assert(offsetof(arc_shadow_record_t, seq) == 8, "shadow record.seq offset");
_Static_assert(offsetof(arc_shadow_record_t, S_n) == 16, "shadow record.S_n offset");
_Static_assert(offsetof(arc_shadow_record_t, A_n) == 24, "shadow record.A_n offset");
_Static_assert(offsetof(arc_shadow_record_t, T_n) == 32, "shadow record.T_n offset");
_Static_assert(offsetof(arc_shadow_record_t, release_ns) == 40, "shadow record.release_ns offset");
_Static_assert(offsetof(arc_shadow_record_t, len) == 48, "shadow record.len offset");
_Static_assert(offsetof(arc_shadow_record_t, released) == 52, "shadow record.released offset");
_Static_assert(offsetof(arc_shadow_record_t, immediate_applied) == 56,
               "shadow record.immediate_applied offset");
_Static_assert(sizeof(arc_shadow_record_t) == 64, "shadow record size");
_Static_assert(offsetof(arc_shadow_stats_t, epoch) == 0, "shadow stats.epoch offset");
_Static_assert(offsetof(arc_shadow_stats_t, schedule_releases) == 8,
               "shadow stats.schedule_releases offset");
_Static_assert(offsetof(arc_shadow_stats_t, immediate_applies) == 16,
               "shadow stats.immediate_applies offset");
_Static_assert(offsetof(arc_shadow_stats_t, schedule_samples) == 24,
               "shadow stats.schedule_samples offset");
_Static_assert(offsetof(arc_shadow_stats_t, budget_alerts) == 32,
               "shadow stats.budget_alerts offset");
_Static_assert(offsetof(arc_shadow_stats_t, last_schedule_release_ns) == 40,
               "shadow stats.last_schedule_release_ns offset");
_Static_assert(offsetof(arc_shadow_stats_t, last_immediate_apply_ns) == 48,
               "shadow stats.last_immediate_apply_ns offset");
_Static_assert(offsetof(arc_shadow_stats_t, scheduled_backlog) == 56,
               "shadow stats.scheduled_backlog offset");
_Static_assert(offsetof(arc_shadow_stats_t, record_total) == 64,
               "shadow stats.record_total offset");
_Static_assert(offsetof(arc_shadow_stats_t, record_dropped) == 72,
               "shadow stats.record_dropped offset");
_Static_assert(offsetof(arc_shadow_stats_t, added_p50_ns) == 80,
               "shadow stats.added_p50_ns offset");
_Static_assert(offsetof(arc_shadow_stats_t, added_p95_ns) == 88,
               "shadow stats.added_p95_ns offset");
_Static_assert(offsetof(arc_shadow_stats_t, added_p99_ns) == 96,
               "shadow stats.added_p99_ns offset");
_Static_assert(offsetof(arc_shadow_stats_t, record_count) == 104,
               "shadow stats.record_count offset");
_Static_assert(offsetof(arc_shadow_stats_t, active) == 108, "shadow stats.active offset");
_Static_assert(offsetof(arc_shadow_stats_t, schedule_mode) == 112,
               "shadow stats.schedule_mode offset");
_Static_assert(sizeof(arc_shadow_stats_t) == 120, "shadow stats size");

int main(void) {
    return 0;
}
