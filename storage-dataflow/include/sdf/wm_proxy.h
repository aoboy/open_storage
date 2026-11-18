#ifndef SDF_WM_PROXY_H
#define SDF_WM_PROXY_H

/*
 * Cache-fill proxy (plan prompt W2.5, design 01).
 *
 * App-level contract: the proxy core runs a fill workload over the
 * origin seam with seeded WM dispatch, the C severity controller in
 * the dispatch loop, and A/B bucketing per design 01 (treatment =
 * seeded spray, control = round-robin, kill_switch forces control).
 * The CLI shell and the integration test both drive sdf_wm_proxy_run.
 */

#include "sdf/common/metrics.h"
#include "sdf/origin.h"
#include "sdf/wm.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Loopback data-file size the proxy assumes (callers create files at
 * least this large; offsets wrap inside it). */
#define SDF_WM_PROXY_OBJ_BYTES (1u << 20)
#define SDF_WM_PROXY_MAX_STEPS 8

typedef struct {
    uint64_t at_unit;    /* apply when dispatched crosses this count */
    uint32_t origin;
    int enable;          /* set_enable(up/down) */
    uint64_t throttle_ns; /* optional throttle knob before enable */
} sdf_wm_proxy_step_t;

typedef struct {
    /* workload */
    uint64_t units;
    uint32_t unit_bytes;
    uint32_t jobs; /* fill jobs; each gets its own spray seed pair */

    /* dispatch / control (from the TOML [wm] block) */
    wm_profile_t profile;
    wm_compile_cfg_t spray;
    uint32_t control_hz;
    double alpha;
    double beta;
    uint32_t rollout_pct;
    int kill_switch;

    /* origins ([origin] blocks; cls must be loopback for the harness) */
    sdf_origin_cfg_t origins[SDF_ORIGIN_MAX];
    uint32_t n_origins;

    /* optional degradation timeline (the e2e profiles) */
    sdf_wm_proxy_step_t steps[SDF_WM_PROXY_MAX_STEPS];
    uint32_t n_steps;
} sdf_wm_proxy_cfg_t;

typedef struct {
    uint64_t dispatched[WM_MAX_PATHS];
    uint64_t ok[WM_MAX_PATHS];
    uint64_t failed[WM_MAX_PATHS];
    uint64_t bytes_rx;
    uint64_t wall_ns;
    uint32_t updates_applied; /* controller whacks that landed */
    uint64_t dispatch_treatment;
    uint64_t dispatch_control;
    int end_profile_b[WM_MAX_PATHS];
    /* series per design 01 §Metrics (W0.5 primitives) */
    sdf_counter_t shed_events[WM_MAX_PATHS];
    sdf_counter_t enolink[WM_MAX_PATHS];
} sdf_wm_proxy_stats_t;

/*
 * Run one fill workload synchronously. Returns 0 on success (out
 * filled), -EINVAL on bad configuration, -errno from the origin
 * driver on open failure.
 */
int sdf_wm_proxy_run(const sdf_wm_proxy_cfg_t *cfg, sdf_wm_proxy_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SDF_WM_PROXY_H */
