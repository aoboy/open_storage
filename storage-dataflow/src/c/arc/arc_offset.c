/*
 * ARC core: the (S_n, A_n) -> D/E -> T_n machinery (plan prompt W4.2,
 * ADR-0006).
 *
 * Algorithms imported from wack_a_mole src/arc/arc.c @ a0f0422 (861
 * lines, paper-golden validated there): the Algorithm-1 asymmetric
 * offset update with the envelope clamp, the Algorithm-2 quantized-E
 * midpoint hysteresis, idle re-anchoring, the X4-lite absurd-stamp
 * gate (S_n far ahead of the receiver clock is not a sender timestamp
 * — the block still releases, scheduled from A_n, but the estimators
 * refuse to learn from it), the delta-window/reorder-window auto
 * params, and the J.5 delta clamp with the p99 budget alert.
 * Reshaped to the sdf v2 ABI: submit copies caller bytes into the
 * slab; the heap and release bookkeeping live in arc_heap.c.
 */

#include "arc_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ARC_SANE_NS (10ull * 1000000000ull) /* 10 s absurd-stamp window */

/* reservoir window for added-delay percentiles (exact while count <= n) */
#define ARC_RES 256

static double clampd(double v, double lo, double hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static double pow_rho(double x, double rho) {
    return pow(fabs(x), rho); /* rho kept in the paper's ranges */
}

/* ---- percentile over the reservoir (qsort like the wam hist) ---------- */

static int u64_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static double res_pctl(const uint64_t *samples, size_t n, size_t cap, double p) {
    if (n == 0) {
        return 0.0;
    }
    uint64_t tmp[ARC_RES];
    size_t take = n < cap ? n : cap;
    memcpy(tmp, samples, take * sizeof(uint64_t));
    qsort(tmp, take, sizeof(uint64_t), u64_cmp);
    if (p < 0.0) {
        p = 0.0;
    }
    if (p > 1.0) {
        p = 1.0;
    }
    size_t idx = (size_t)((double)(take - 1) * p + 0.5);
    if (idx >= take) {
        idx = take - 1;
    }
    return (double)tmp[idx];
}

/* ---- auto params (paper 9.2 / wam update_auto_params) ------------------ */

static void auto_params(arc_state_t *a) {
    if (!a->auto_params) {
        return;
    }
    /* U ~ c_U * RTT (clamped into the paper band) */
    if (a->rtt_sm_ns > 0) {
        double u = a->c_U * (double)a->rtt_sm_ns;
        a->U = clampd(u, 50e6, 200e6);
    }
    /* delta ~ c_delta * (q95 - q50) of the inter-arrival window */
    if (a->delta_n >= 32) {
        double q95 = res_pctl(a->delta_win, a->delta_n, a->delta_cap, 0.95);
        double q50 = res_pctl(a->delta_win, a->delta_n, a->delta_cap, 0.50);
        double d = a->c_delta * (q95 - q50);
        if (d > 1e3) {
            a->delta = clampd(d, 30e6, 100e6);
        }
    }
    /* G ~ q95 of observed reorder gaps */
    if (a->reorder_n >= 16) {
        double g = res_pctl(a->reorder_win, a->reorder_n, a->reorder_cap, 0.95);
        if (g > 1e6) {
            a->G = clampd(g, 20e6, 200e6);
        }
    }
}

/* ---- the arrival path (Algorithm 1 + 2, from wam wam_arc_on_recovered) - */

arc_sched_t arc_arrival(arc_state_t *a, uint64_t S_n, uint64_t A_n, size_t len,
                        int *scheduled_now) {
    arc_sched_t sched;
    memset(&sched, 0, sizeof(sched));
    sched.block_n = a->next_block++;
    sched.S_n = S_n;
    sched.A_n = A_n;
    sched.len = (uint32_t)len;
    sched.seq = a->submit_seq++;
    *scheduled_now = 0;

    /* IMMEDIATE oracle: release at arrival, no scheduling, no ordering */
    if (a->mode == ARC_MODE_IMMEDIATE) {
        sched.T_n = A_n;
        sched.T_deadline = A_n;
        *scheduled_now = 1;
        return sched;
    }

    /* small entries bypass smoothing (paper: tiny control blocks) */
    if (len < a->min_smooth_bytes) {
        sched.T_n = A_n;
        sched.T_deadline = A_n + a->G;
        *scheduled_now = 2;
        return sched;
    }

    /* X4-lite absurd-stamp gate: S_n far ahead of the receiver clock is
     * not a sender timestamp — schedule from A_n, refuse to learn */
    if (A_n < S_n && S_n - A_n > ARC_SANE_NS) {
        a->stats.s_n_rejects++;
        sched.T_n = A_n;
        sched.T_deadline = A_n + a->G;
        return sched;
    }

    if (!a->initialized) {
        a->D = (double)A_n - (double)S_n;
        if (a->D < 0.0) {
            a->D = 0.0;
        }
        a->E = a->D + a->gamma / 2.0;
        a->tau_last_ns = A_n;
        a->initialized = 1;
    } else {
        /* signed gap: out-of-order arrivals must not wrap the idle test */
        int64_t gap = (int64_t)(A_n - a->tau_last_ns);
        if ((double)gap >= (double)a->T_idle_ns) {
            a->D = (double)A_n - (double)S_n;
            if (a->D < 0.0) {
                a->D = 0.0;
            }
            a->stats.idle_reanchors++;
        }
        a->tau_last_ns = A_n;
    }

    /* inter-arrival windows feed the auto params */
    if (a->have_prev_A) {
        int64_t dA = (int64_t)(A_n - a->prev_A);
        if (dA >= 0) {
            a->delta_win[a->delta_pos] = (uint64_t)dA;
            a->delta_pos = (a->delta_pos + 1) % a->delta_cap;
            if (a->delta_n < a->delta_cap) {
                a->delta_n++;
            }
        } else {
            a->reorder_win[a->reorder_pos] = (uint64_t)(-dA);
            a->reorder_pos = (a->reorder_pos + 1) % a->reorder_cap;
            if (a->reorder_n < a->reorder_cap) {
                a->reorder_n++;
            }
        }
    }
    a->prev_A = A_n;
    a->have_prev_A = 1;

    if (a->blocks_since_param++ >= 128) {
        a->blocks_since_param = 0;
        auto_params(a);
    }

    /* Algorithm 1: asymmetric update of D (envelope-clamped) */
    if (a->U <= 0) {
        a->U = 100e6;
    }
    double X = (double)A_n - ((double)S_n + a->D);
    if (X > 0) {
        double Y = pow_rho(clampd(X, 0, a->U) / a->U, a->rho_u) * a->U;
        a->D += a->lambda_u * Y;
    } else if (X <= -(double)a->J) {
        double Y = -pow_rho(clampd(fabs(X + (double)a->J), 0, a->U) / a->U, a->rho_l) * a->U;
        a->D += a->lambda_l * Y;
    }
    double dmax = (double)A_n - (double)S_n + (double)a->delta;
    if (a->D > dmax) {
        a->D = dmax;
    }
    if (a->D < 0.0) {
        a->D = 0.0;
    }

    /* Algorithm 2: quantized E with midpoint hysteresis */
    if (a->mode >= ARC_MODE_Q) {
        if (a->D > a->E || a->D < a->E - a->gamma) {
            a->E = a->D + a->gamma / 2.0;
        }
    }

    double offset = a->mode >= ARC_MODE_Q ? a->E : a->D;
    double Tn = (double)A_n;
    double proj = (double)S_n + offset + (double)a->J;
    if (proj > Tn) {
        Tn = proj;
    }

    sched.T_n = (uint64_t)Tn;
    sched.T_deadline = sched.T_n + a->G;
    return sched;
}
