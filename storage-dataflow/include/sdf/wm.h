#ifndef SDF_WM_H
#define SDF_WM_H

#include "sdf/origin.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WM_MAX_PATHS 64
#define WM_MAX_UNITS 1024
#define WM_PROFILE_VERSION 1

/*
 * A path profile is a discrete allocation of m selection units over n paths.
 * b[i] / m is the target fraction of traffic for path i. The sum of b[i]
 * must equal m.
 *
 * The version field lets readers detect a profile replacement while fills
 * are in flight. Bump it on every publish. residual_r is the update
 * embodiment's global redistribution cursor (ADR-0007): it carries across
 * wm_profile_update() calls so repeated whacks distribute remainders
 * fairly over the cycle.
 */
typedef struct {
    uint32_t version;
    uint32_t m;
    uint32_t n;
    uint32_t b[WM_MAX_PATHS];
    uint32_t residual_r;
    uint32_t _pad;
} wm_profile_t;

/*
 * Compiled, read-mostly form of a profile. Selection is a single table
 * lookup: unit_to_path[counter & (m - 1)].
 *
 * down_mask is maintained by wm_mark_down() / wm_mark_up() so the C fast
 * path can react to hard failures without waiting for the Python controller.
 * cdf always describes the original profile, not the temporarily renormalized
 * one, so marking a path back up restores the exact original allocation.
 *
 * ADR-0007 additions: method/sa/sb select the paper's spray key
 * transform — PLAIN keeps the one-load hot path (unit_to_path);
 * SHUFFLE1/SHUFFLE2 compute the seeded key and resolve the path through
 * the path_of_key LUT (rebuilt under down_mask renormalization like
 * unit_to_path, so mark_down/up behave identically across methods).
 */
typedef struct {
    uint32_t m;
    uint32_t n;
    uint32_t log2m;
    uint32_t reserved;
    uint64_t down_mask;
    uint32_t cdf[WM_MAX_PATHS];
    uint32_t perm[WM_MAX_UNITS];
    uint32_t unit_to_path[WM_MAX_UNITS];
    uint32_t method; /* wm_method_t */
    uint32_t sa;     /* shuffle seed a, in [0, m) */
    uint32_t sb;     /* shuffle seed b, odd, in [1, m) */
    uint32_t path_of_key[WM_MAX_UNITS]; /* key -> path LUT (shuffles) */
} wm_compiled_t;

/* Spray key methods ([WAM] section 4, ADR-0007). */
typedef enum {
    WM_METHOD_PLAIN = 0,     /* key = theta(counter) */
    WM_METHOD_SHUFFLE1 = 1, /* key = theta((sa + counter*sb) mod m) */
    WM_METHOD_SHUFFLE2 = 2, /* key = (sa + sb*theta(counter)) mod m */
} wm_method_t;

/* Per-job spray configuration; seeds decorrelate jobs with equal
 * profiles so two fills never emit identical unit->path sequences. */
typedef struct {
    uint32_t method; /* wm_method_t */
    uint32_t sa;
    uint32_t sb; /* must be odd (coprime to m) */
} wm_compile_cfg_t;

/* Compile a profile. Returns 0 on success, negative errno-style value on error. */
int wm_profile_compile(const wm_profile_t *p, wm_compiled_t *out);

/*
 * Compile with a spray configuration (ADR-0007): method and seeds are
 * validated (sb odd, both seeds < m) and carried in the compiled form.
 * wm_profile_compile() is the PLAIN default (sa=0, sb=1).
 */
int wm_profile_compile_cfg(const wm_profile_t *p, wm_compiled_t *out,
                           const wm_compile_cfg_t *cfg);

/*
 * Deterministic seed rotation for wrap (the wam-send rule): caller
 * applies on counter wrap, keeping sb odd and both seeds in [0, m).
 */
void wm_seed_rotate(wm_compile_cfg_t *cfg, uint32_t m);

/*
 * Whack update (ADR-0007, [WAM] section 7, adapted from wack_a_mole
 * src/core/profile.c @ a0f0422): remove e[i] balls from degraded paths
 * and redistribute deterministically. sum(b) == m is preserved and the
 * global residual cursor p->residual_r carries across calls.
 *
 * Modes (update embodiments):
 *   1 - single hot path: e[] has one positive entry; e[j] is spread
 *       over all n (x per path plus the residual walk).
 *   2 - multi path: every e[i] removed, the total spread over all n.
 *   3 - K/K-bar: balls taken from the positive-e paths are given only
 *       to paths with e[i] == 0; fails unless both sides are non-empty.
 *   4 - proportional renormalize: remaining mass renormalized to m with
 *       uint64 intermediates (the paper's embodiment 4; the fuzz-found
 *       all-positive-e fallback from wack_a_mole's M.1 is included).
 *
 * Invariants: e[i] <= b[i]; the update never zeroes every path
 * (n > 1) — "the last healthy path is never removed".
 * Returns 0 on success, -EINVAL on violations.
 */
int wm_profile_update(wm_profile_t *p, const uint32_t *e, int mode);

/* ---- severity controller (plan prompt W2.2) --------------------------- */

/* Defaults follow wack_a_mole plan K.2: alpha_lo 0.05, alpha_hi 0.25,
 * theta_hi 50, theta_lo 20, t_min 2 ms. */
typedef struct {
    double alpha_lo;   /* pull fraction in the low band */
    double alpha_hi;   /* pull fraction in the high band */
    uint32_t theta_hi; /* severity threshold, high band */
    uint32_t theta_lo; /* severity threshold, low band */
    uint64_t t_min_ns; /* rate limit between applied updates */
    int embodiment;   /* 0 = auto: mode 1 when exactly one path whacked,
                         else mode 2 (the wam plan G.2 rule) */
} wm_ctrl_cfg_t;

/*
 * C fast-path severity controller: consumes normalized origin stats
 * (sdf_origin_stats_t per ADR-0008 — the only cross-header the WM
 * side is allowed, per the link rule) and applies graded whacks
 * without waiting for the Python loop. Severity per path:
 *   hard_down                     -> 100
 *   else 100*min(ewma_err, 1)  +  latency inflation over the healthy
 *   fleet median (linear to +100 at 4x median)     , clamped to 100.
 * Recovery (additive increase) is the Python loop's job; this side
 * only whacks. wm_ctrl_tick is rate-limited by t_min_ns.
 */
typedef struct {
    wm_profile_t work; /* carries residual_r across updates */
    wm_ctrl_cfg_t cfg;
    uint64_t last_tick_ns;
    sdf_origin_stats_t last[WM_MAX_PATHS]; /* freshest snapshots */
    int have[WM_MAX_PATHS];
    uint32_t n_paths;
} wm_ctrl_t;

void wm_ctrl_cfg_default(wm_ctrl_cfg_t *cfg);
int wm_ctrl_init(wm_ctrl_t *ctl, const wm_profile_t *base,
                 const wm_ctrl_cfg_t *cfg, uint32_t n_paths);
void wm_ctrl_on_stats(wm_ctrl_t *ctl, uint32_t path,
                      const sdf_origin_stats_t *st);
/* Returns 1 when an update was applied (rate limit + thresholds), 0
 * when not; *out always carries the current working profile. */
int wm_ctrl_tick(wm_ctrl_t *ctl, uint64_t now_ns, wm_profile_t *out);

/* Select one path for a monotonic unit counter. Lock-free and allocation-free. */
uint32_t wm_select(const wm_compiled_t *c, uint64_t counter);

/* Batched form of wm_select. Base is the counter for out[0]. */
void wm_select_batch(const wm_compiled_t *c, uint64_t base, uint32_t *out, size_t n);

/* Fast-path congestion response used before the Python controller reacts. */
void wm_mark_down(wm_compiled_t *c, uint32_t path_id);
void wm_mark_up(wm_compiled_t *c, uint32_t path_id);

#ifdef __cplusplus
}
#endif

#endif /* SDF_WM_H */
