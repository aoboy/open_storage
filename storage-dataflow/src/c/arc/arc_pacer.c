/*
 * ARC pacer: the public ABI glue (plan prompt W4.2, ADR-0006).
 *
 * arc_sizeof lays out state + heap array + slab in one caller-provided
 * block; arc_submit runs the arrival path (arc_offset) and queues the
 * schedule (arc_heap); arc_poll drains due entries under the QG guard
 * with the batch bounds from cfg. The reservoir, auto windows and
 * defaults follow the wam core conventions; the sdf entry model
 * (slab copy, submit-order block numbers) replaces wam's block
 * payloads.
 */

#include "arc_internal.h"

#include "sdf/common/timer.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * Paper defaults ([ARC] section 9; wam wam_arc_init) — presence-style:
 * only fields the caller left at zero take defaults (the golden test
 * caught the first draft overwriting caller-set paper parameters,
 * which silently switched rho/lambda to 0.5/0.05 mid-test).
 */
static void cfg_defaults(arc_cfg_t *cfg) {
    if (cfg->rho_u == 0.0) {
        cfg->rho_u = 0.5;
    }
    if (cfg->rho_l == 0.0) {
        cfg->rho_l = 1.5;
    }
    if (cfg->lambda_u == 0.0) {
        cfg->lambda_u = 0.05;
    }
    if (cfg->lambda_l == 0.0) {
        cfg->lambda_l = 0.03;
    }
    if (cfg->U == 0) {
        cfg->U = 100000000ull; /* 100 ms */
    }
    if (cfg->delta == 0) {
        cfg->delta = 50000000ull; /* 50 ms */
    }
    if (cfg->gamma == 0) {
        cfg->gamma = 16666667ull; /* ~60 fps step */
    }
    /* J == 0 is a VALID paper setting (neutral band off): not defaulted */
    if (cfg->G == 0) {
        cfg->G = 50000000ull; /* 50 ms guard */
    }
    if (cfg->T_idle_ns == 0) {
        cfg->T_idle_ns = 1000000000ull;
    }
    if (cfg->c_U == 0.0) {
        cfg->c_U = 1.0;
    }
    if (cfg->c_delta == 0.0) {
        cfg->c_delta = 1.0;
    }
    if (cfg->min_smooth_bytes == 0) {
        cfg->min_smooth_bytes = 64;
    }
    /* budget_ns == 0 = off (valid); rtt_sm_ns == 0 = auto-U off */
    if (cfg->mode != ARC_MODE_BASE && cfg->mode != ARC_MODE_Q && cfg->mode != ARC_MODE_QG &&
        cfg->mode != ARC_MODE_IMMEDIATE) {
        cfg->mode = ARC_MODE_QG;
    }
    if (cfg->capacity == 0) {
        cfg->capacity = 256;
    }
    if (cfg->entry_max == 0) {
        cfg->entry_max = 256;
    }
    if (cfg->batch_max == 0) {
        cfg->batch_max = 64;
    }
    if (cfg->batch_deadline_ns == 0) {
        cfg->batch_deadline_ns = 2000000ull; /* 2 ms */
    }
    /* auto_params: the caller sets it explicitly (0/1); -1 => default on */
    if (cfg->auto_params < 0) {
        cfg->auto_params = 1;
    }
}

size_t arc_sizeof(const arc_cfg_t *cfg) {
    if (cfg == NULL) {
        return 0;
    }
    arc_cfg_t c = *cfg;
    cfg_defaults(&c);
    if (c.capacity < 2 || (c.capacity & (c.capacity - 1)) != 0 || c.entry_max == 0 ||
        c.entry_max > (1u << 20)) {
        return 0;
    }
    size_t need = sizeof(arc_state_t) + (size_t)c.capacity * (sizeof(arc_rel_t) + c.entry_max + 1);
    return (need + 63u) & ~(size_t)63;
}

int arc_init(arc_pacer_t *p, const arc_cfg_t *cfg, arc_apply_fn fn, void *ctx) {
    if (p == NULL || cfg == NULL) {
        return -EINVAL;
    }
    size_t need = arc_sizeof(cfg);
    if (need == 0) {
        return -EINVAL;
    }

    arc_cfg_t c = *cfg;
    cfg_defaults(&c);

    memset(p, 0, need);
    arc_state_t *s = &p->st;
    s->D = 0;
    s->E = 0;
    s->initialized = 0;
    s->rho_u = c.rho_u;
    s->rho_l = c.rho_l;
    s->lambda_u = c.lambda_u;
    s->lambda_l = c.lambda_l;
    s->U = (double)c.U;
    s->delta = (double)c.delta;
    s->gamma = (double)c.gamma;
    s->J = (double)c.J;
    s->G = (double)c.G;
    s->T_idle_ns = (double)c.T_idle_ns;
    s->auto_params = c.auto_params;
    s->c_U = c.c_U;
    s->c_delta = c.c_delta;
    s->rtt_sm_ns = c.rtt_sm_ns;
    s->min_smooth_bytes = c.min_smooth_bytes;
    s->budget_ns = c.budget_ns;
    s->mode = c.mode;
    s->apply = fn;
    s->ctx = ctx;
    s->entry_max = c.entry_max;
    s->capacity = c.capacity;
    s->batch_max = c.batch_max;
    s->batch_deadline_ns = c.batch_deadline_ns;

    uint8_t *tail = (uint8_t *)(p + 1);
    s->heap = (arc_rel_t *)tail;
    tail += (size_t)c.capacity * sizeof(arc_rel_t);
    s->slab = tail;
    tail += (size_t)c.capacity * c.entry_max;
    s->slot_used = tail;
    s->delta_cap = 64;
    s->reorder_cap = 64;
    return 0;
}

int arc_submit(arc_pacer_t *p, const void *entry, size_t len, uint64_t producer_ts) {
    if (p == NULL || (len > 0 && entry == NULL)) {
        return -EINVAL;
    }
    arc_state_t *s = &p->st;
    if (len > s->entry_max) {
        return -EINVAL;
    }
    s->last_sched_valid = 0;

    /* Reserve the slab slot before the arrival path mutates the
     * estimator, sequence, or auto windows. A full pacer is an
     * accepted-by-the-ring retry (W6.9): the same entry must be
     * schedulable later without being counted twice. */
    uint32_t slot = UINT32_MAX;
    for (uint32_t i = 0; i < s->capacity; i++) {
        if (!s->slot_used[i]) {
            slot = i;
            break;
        }
    }
    if (slot == UINT32_MAX) {
        return -EAGAIN;
    }

    int how = 0;
    arc_sched_t sched = arc_arrival(s, producer_ts, (uint64_t)sdf_now_ns(), len, &how);

    if (len > 0) {
        memcpy(s->slab + (size_t)slot * s->entry_max, entry, len);
    }
    s->slot_used[slot] = 1;

    arc_rel_t rel;
    memset(&rel, 0, sizeof(rel));
    rel.T_n = sched.T_n;
    rel.T_deadline = sched.T_deadline;
    rel.block_n = sched.block_n;
    rel.S_n = sched.S_n;
    rel.A_n = sched.A_n;
    rel.seq = sched.seq;
    rel.slot = slot;
    rel.len = (uint32_t)len;

    /* J.5 delta clamp: the schedule never exceeds A_n + delta (the
     * data-driven playout horizon; wam arc_delta_clamp) */
    if (how == 0 && rel.T_n > rel.A_n + (uint64_t)s->delta) {
        rel.T_n = rel.A_n + (uint64_t)s->delta;
        s->stats.delta_clamps++;
        if (rel.T_deadline < rel.T_n) {
            rel.T_deadline = rel.T_n;
        }
    }

    if (arc_heap_push(s, &rel) != 0) {
        s->slot_used[slot] = 0;
        return -EAGAIN;
    }

    /* Preserve the final, clamped schedule for the sidecar's W6.10
     * shadow record. This handoff is internal and does not add public
     * ABI surface beyond the dedicated shadow accessors. */
    sched.T_n = rel.T_n;
    sched.T_deadline = rel.T_deadline;
    s->last_sched = sched;
    s->last_sched_valid = 1;
    return how == 0 ? 0 : 1;
}

int arc_poll(arc_pacer_t *p, uint64_t now_ns) {
    if (p == NULL) {
        return -EINVAL;
    }
    arc_state_t *s = &p->st;
    uint64_t deadline = now_ns + s->batch_deadline_ns;
    uint32_t released = 0;
    while (released < s->batch_max) {
        int n = arc_drain(s, now_ns);
        released += (uint32_t)n;
        if (n == 0) {
            break;
        }
        if ((uint64_t)sdf_now_ns() > deadline) {
            break;
        }
    }
    return (int)released;
}

arc_stats_t arc_stats(const arc_pacer_t *p) {
    arc_stats_t out;
    memset(&out, 0, sizeof(out));
    if (p == NULL) {
        return out;
    }
    const arc_state_t *s = &p->st;
    out.D = s->D;
    out.E = s->mode >= ARC_MODE_Q ? s->E : s->D;
    out.releases = s->stats.releases;
    out.in_flight = s->heap_n;
    out.heap_depth = (uint32_t)s->heap_n;
    out.idle_reanchors = s->stats.idle_reanchors;
    out.guard_waits = s->stats.guard_waits;
    out.guard_timeouts = s->stats.guard_timeouts;
    out.s_n_rejects = s->stats.s_n_rejects;
    out.delta_clamps = s->stats.delta_clamps;
    out.budget_alerts = s->stats.budget_alerts;
    out.added_count = s->added_n;
    out.added_p50_ns = arc_res_pctl(s->added_samples, s->added_n, 256, 0.50);
    out.added_p95_ns = arc_res_pctl(s->added_samples, s->added_n, 256, 0.95);
    out.added_p99_ns = arc_res_pctl(s->added_samples, s->added_n, 256, 0.99);
    out.interval_error_max_ns = s->interval_error_max;
    return out;
}

void arc_set_rtt(arc_pacer_t *p, uint64_t rtt_sm_ns) {
    if (p == NULL) {
        return;
    }
    p->st.rtt_sm_ns = rtt_sm_ns;
}

int arc_set_mode(arc_pacer_t *p, arc_mode_t mode) {
    if (p == NULL) {
        return -EINVAL;
    }
    if (mode != ARC_MODE_BASE && mode != ARC_MODE_Q && mode != ARC_MODE_QG &&
        mode != ARC_MODE_IMMEDIATE) {
        return -EINVAL;
    }
    /* The mode is read by the next submit/poll on the pacer's thread;
     * callers that own the pacer (the sidecar pump) serialize the
     * flip, so no extra synchronization is needed here. */
    p->st.mode = mode;
    return 0;
}
