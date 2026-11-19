/*
 * ARC release queue (plan prompt W4.2): binary min-heap over T_n with
 * the ARC-QG guard — imported from wack_a_mole src/arc/arc.c @ a0f0422
 * (heap_push/heap_pop + wam_arc_poll's Algorithm 3: ordered release,
 * predecessor-park with the T_n+G timeout, successor wake on the
 * predecessor's release) — reshaped onto the slab-backed sdf entries.
 */

#include "arc_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- min-heap over T_n --------------------------------------------------- */

static int less_than(const arc_rel_t *a, const arc_rel_t *b) {
    if (a->T_n != b->T_n)
        return a->T_n < b->T_n;
    return a->seq < b->seq; /* stable: submit order on ties */
}

static void heap_swap(arc_state_t *s, size_t i, size_t j) {
    arc_rel_t tmp = s->heap[i];
    s->heap[i] = s->heap[j];
    s->heap[j] = tmp;
}

static void heap_up(arc_state_t *s, size_t i) {
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (!less_than(&s->heap[i], &s->heap[parent]))
            break;
        heap_swap(s, i, parent);
        i = parent;
    }
}

static void heap_down(arc_state_t *s, size_t i) {
    for (;;) {
        size_t l = 2 * i + 1, r = 2 * i + 2, best = i;
        if (l < s->heap_n && less_than(&s->heap[l], &s->heap[best]))
            best = l;
        if (r < s->heap_n && less_than(&s->heap[r], &s->heap[best]))
            best = r;
        if (best == i)
            break;
        heap_swap(s, i, best);
        i = best;
    }
}

int arc_heap_push(arc_state_t *s, const arc_rel_t *rel) {
    if (s == NULL || rel == NULL || s->heap_n >= s->capacity)
        return -1;
    s->heap[s->heap_n] = *rel;
    heap_up(s, s->heap_n);
    s->heap_n++;
    return 0;
}

int arc_heap_pop(arc_state_t *s, arc_rel_t *out) {
    if (s == NULL || out == NULL || s->heap_n == 0)
        return -1;
    *out = s->heap[0];
    s->heap_n--;
    if (s->heap_n > 0) {
        s->heap[0] = s->heap[s->heap_n];
        heap_down(s, 0);
    }
    return 0;
}

/* ---- release bookkeeping ------------------------------------------------- */

double arc_res_pctl(const uint64_t *samples, size_t n, size_t cap, double p) {
    if (n == 0)
        return 0.0;
    uint64_t tmp[256];
    size_t take = n < cap ? n : cap;
    if (take > 256)
        take = 256;
    memcpy(tmp, samples, take * sizeof(uint64_t));
    /* insertion sort: take <= 256, called rarely (stats snapshots) */
    for (size_t i = 1; i < take; i++) {
        uint64_t v = tmp[i];
        int64_t j = (int64_t)i - 1;
        while (j >= 0 && tmp[j] > v) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = v;
    }
    if (p < 0.0)
        p = 0.0;
    if (p > 1.0)
        p = 1.0;
    size_t idx = (size_t)((double)(take - 1) * p + 0.5);
    if (idx >= take)
        idx = take - 1;
    return (double)tmp[idx];
}

void arc_note_release(arc_state_t *s, const arc_rel_t *rel, uint64_t now_ns) {
    s->stats.releases++;

    /* added delay: release time minus arrival (the J.5 budget metric) */
    uint64_t added = now_ns > rel->A_n ? now_ns - rel->A_n : 0;
    if (s->added_n < 256) {
        s->added_samples[s->added_n++] = added;
    } else {
        s->added_samples[s->added_pos] = added;
        s->added_pos = (s->added_pos + 1) % 256;
    }

    /* release-interval error: |gap between releases - producer gap| */
    if (s->have_last_release) {
        int64_t rel_gap = (int64_t)(now_ns - s->last_release_ts);
        int64_t prod_gap = (int64_t)(rel->S_n - s->last_release_S_ns);
        int64_t err = rel_gap - prod_gap;
        if (err < 0)
            err = -err;
        if ((uint64_t)err > s->interval_error_max)
            s->interval_error_max = (uint64_t)err;
    }
    s->last_release_ts = now_ns;
    s->last_release_S_ns = rel->S_n;
    s->have_last_release = 1;

    /* J.5 budget: session p99 of the added delay over the budget */
    if (s->budget_ns > 0) {
        double p99 = arc_res_pctl(s->added_samples, s->added_n, 256, 0.99);
        if (p99 > (double)s->budget_ns)
            s->stats.budget_alerts++;
    }
}

/* ---- the guard-aware drain (Algorithm 3) --------------------------------- */

static uint64_t min_pred_T(const arc_state_t *s, uint64_t block_n) {
    uint64_t best = UINT64_MAX;
    for (size_t i = 0; i < s->heap_n; i++) {
        if (s->heap[i].block_n + 1 == block_n && s->heap[i].T_n < best)
            best = s->heap[i].T_n;
    }
    return best;
}

/* release everything due at now_ns under QG ordering; returns count */
int arc_drain(arc_state_t *s, uint64_t now_ns) {
    int released = 0;
    arc_rel_t rel;
    while (s->heap_n > 0 && s->heap[0].T_n <= now_ns) {
        if (arc_heap_pop(s, &rel) != 0)
            break;

        if (s->mode >= ARC_MODE_QG) {
            int ordered =
                s->have_last_release
                    ? (rel.block_n <= s->last_released_n + 1)
                    : (rel.block_n == 0);
            if (!ordered) {
                uint64_t pred_T = min_pred_T(s, rel.block_n);
                if (pred_T != UINT64_MAX) {
                    /* predecessor in flight: order beats the guard —
                     * wake right after it */
                    rel.T_n = pred_T + 1;
                    s->stats.guard_waits++;
                    (void)arc_heap_push(s, &rel);
                    break;
                }
                /* predecessor missing: park until T_n + G, then release */
                if (now_ns < rel.T_deadline) {
                    rel.T_n = rel.T_deadline;
                    rel.parked = 1;
                    s->stats.guard_waits++;
                    (void)arc_heap_push(s, &rel);
                    break;
                }
                s->stats.guard_timeouts++;
            }
        }

        arc_note_release(s, &rel, now_ns);
        s->last_released_n = rel.block_n;
        if (s->apply != NULL) {
            uint8_t *entry = s->slab + (size_t)rel.slot * s->entry_max;
            (void)s->apply(s->ctx, entry, rel.len, now_ns);
        }
        s->slot_used[rel.slot] = 0;
        released++;

        /* wake a guard-held successor parked at its deadline so it
         * releases right after the predecessor (parked flag only —
         * a T_n == T_deadline from ordinary schedules is not a park) */
        if (s->stats.guard_waits > 0) {
            for (size_t i = 0; i < s->heap_n; i++) {
                if (s->heap[i].block_n == s->last_released_n + 1 &&
                    s->heap[i].parked && s->heap[i].T_n > now_ns) {
                    s->heap[i].T_n = now_ns;
                    s->heap[i].parked = 0;
                    heap_up(s, i);
                }
            }
        }
    }
    return released;
}
