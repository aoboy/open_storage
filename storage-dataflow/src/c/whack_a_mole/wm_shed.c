#include "wm_internal.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

static uint64_t active_path_mask(uint32_t n) {
    return n >= 64 ? UINT64_MAX : ((UINT64_C(1) << n) - 1u);
}

void wm_mark_down(wm_compiled_t *c, uint32_t path_id) {
    if (c == NULL || path_id >= c->n) {
        return;
    }

    uint64_t bit = UINT64_C(1) << path_id;
    uint64_t old_mask = c->down_mask;
    if ((old_mask & bit) != 0) {
        return;
    }

    /* Never take every path down: with no target, marking has no useful
     * meaning. The last healthy path stays available. */
    uint64_t all = active_path_mask(c->n);
    if (((old_mask | bit) & all) == all) {
        return;
    }

    c->down_mask = old_mask | bit;
    if (sdf_wm_rebuild(c) != 0) {
        c->down_mask = old_mask;
        (void)sdf_wm_rebuild(c);
    }
}

void wm_mark_up(wm_compiled_t *c, uint32_t path_id) {
    if (c == NULL || path_id >= c->n) {
        return;
    }

    uint64_t bit = UINT64_C(1) << path_id;
    uint64_t old_mask = c->down_mask;
    if ((old_mask & bit) == 0) {
        return;
    }

    c->down_mask = old_mask & ~bit;
    if (sdf_wm_rebuild(c) != 0) {
        c->down_mask = old_mask;
        (void)sdf_wm_rebuild(c);
    }
}

/* ---- severity controller (plan prompt W2.2) ---------------------------
 *
 * Adapted from wack_a_mole src/ctrl/controller.c @ a0f0422 (severity
 * bands with theta_hi/theta_lo pulls, e[i] = floor(alpha * b[i]), the
 * plan G.2 auto-rule: mode 1 when exactly one path whacked, else mode 2,
 * t_min rate limiting) — reshaped to consume sdf_origin_stats_t
 * snapshots per ADR-0008 instead of transport feedback.
 *
 * Severity model (documented contract, pinned by test_wm_profile):
 *   hard_down                                -> 100
 *   else: 100*min(ewma_err, 1) + latency inflation over the healthy
 *   fleet median, linear to +100 at 4x median, clamped to 100.
 * This side only whacks; recovery (additive increase) is the Python
 * control loop's job (design 01). */

void wm_ctrl_cfg_default(wm_ctrl_cfg_t *cfg) {
    if (cfg == NULL) {
        return;
    }
    cfg->alpha_lo = 0.05;
    cfg->alpha_hi = 0.25;
    cfg->theta_hi = 50;
    cfg->theta_lo = 20;
    cfg->t_min_ns = 2000000ull; /* 2 ms, plan K.2 */
    cfg->embodiment = 0;        /* auto: 1 when single hot path, else 2 */
}

int wm_ctrl_init(wm_ctrl_t *ctl, const wm_profile_t *base, const wm_ctrl_cfg_t *cfg,
                 uint32_t n_paths) {
    if (ctl == NULL || base == NULL || cfg == NULL) {
        return -EINVAL;
    }
    if (n_paths == 0 || n_paths > WM_MAX_PATHS || base->n != n_paths) {
        return -EINVAL;
    }
    ctl->work = *base;
    ctl->cfg = *cfg;
    ctl->last_tick_ns = 0;
    ctl->n_paths = n_paths;
    for (uint32_t i = 0; i < n_paths; i++) {
        memset(&ctl->last[i], 0, sizeof(ctl->last[i]));
        ctl->have[i] = 0;
    }
    return 0;
}

void wm_ctrl_on_stats(wm_ctrl_t *ctl, uint32_t path, const sdf_origin_stats_t *st) {
    if (ctl == NULL || st == NULL || path >= ctl->n_paths) {
        return;
    }
    ctl->last[path] = *st;
    ctl->have[path] = 1;
}

/* severity for one path given the fleet's healthy median latency */
static uint32_t path_severity(const sdf_origin_stats_t *st, double ref_lat) {
    if (st->hard_down) {
        return 100;
    }
    double sev = 100.0 * (st->ewma_err > 1.0 ? 1.0 : st->ewma_err);
    if (ref_lat > 0.0 && st->ewma_lat_ns > ref_lat) {
        double ratio = (st->ewma_lat_ns / ref_lat) - 1.0; /* 0 .. 3+ */
        if (ratio > 3.0) {
            ratio = 3.0;
        }
        sev += 100.0 * (ratio / 3.0); /* +100 at 4x median */
    }
    if (sev > 100.0) {
        sev = 100.0;
    }
    return (uint32_t)(sev + 0.5);
}

/* median latency over paths that are neither down nor error-heavy */
static double healthy_median_lat(const wm_ctrl_t *ctl) {
    double lats[WM_MAX_PATHS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < ctl->n_paths; i++) {
        if (!ctl->have[i] || ctl->last[i].hard_down || ctl->last[i].ewma_err >= 0.5) {
            continue;
        }
        lats[n++] = ctl->last[i].ewma_lat_ns;
    }
    if (n == 0) {
        return 0.0;
    }
    /* insertion sort (n <= 64) */
    for (uint32_t i = 1; i < n; i++) {
        double v = lats[i];
        int64_t j = (int64_t)i - 1;
        while (j >= 0 && lats[j] > v) {
            lats[j + 1] = lats[j];
            j--;
        }
        lats[j + 1] = v;
    }
    return n % 2 ? lats[n / 2] : (lats[n / 2 - 1] + lats[n / 2]) / 2.0;
}

int wm_ctrl_tick(wm_ctrl_t *ctl, uint64_t now_ns, wm_profile_t *out) {
    if (ctl == NULL || out == NULL) {
        return -EINVAL;
    }
    *out = ctl->work;

    /* rate limit: a control interval must have elapsed since the last
     * APPLIED update (the fast path never storms the profile) */
    if (ctl->last_tick_ns != 0 && now_ns - ctl->last_tick_ns < ctl->cfg.t_min_ns) {
        return 0;
    }

    double ref_lat = healthy_median_lat(ctl);
    uint32_t severity[WM_MAX_PATHS];
    uint32_t n_whacked = 0;
    uint32_t e[WM_MAX_PATHS];

    for (uint32_t i = 0; i < ctl->n_paths; i++) {
        severity[i] = ctl->have[i] ? path_severity(&ctl->last[i], ref_lat) : 0;
        double alpha = 0.0;
        if (severity[i] >= ctl->cfg.theta_hi) {
            alpha = ctl->cfg.alpha_hi;
        } else if (severity[i] >= ctl->cfg.theta_lo) {
            alpha = ctl->cfg.alpha_lo;
        }
        if (alpha > 0.0) {
            double pull = alpha * (double)ctl->work.b[i];
            e[i] = (uint32_t)pull; /* floor */
            if (e[i] > 0) {
                n_whacked++;
            }
        } else {
            e[i] = 0;
        }
    }

    if (n_whacked == 0) {
        return 0; /* nothing to whack: profile unchanged this tick */
    }

    int mode = ctl->cfg.embodiment;
    if (mode == 0) {
        mode = n_whacked == 1 ? 1 : 2; /* plan G.2 auto rule */
    }
    if (mode == 1 && n_whacked > 1) {
        return 0; /* mode 1 contract violated: refuse rather than clamp */
    }

    if (wm_profile_update(&ctl->work, e, mode) != 0) {
        /* invariant violation (e.g. every path whacked at once): keep
         * the previous profile — the last healthy path survives */
        *out = ctl->work;
        return 0;
    }
    ctl->work.version++;
    ctl->last_tick_ns = now_ns;
    *out = ctl->work;
    return 1;
}
