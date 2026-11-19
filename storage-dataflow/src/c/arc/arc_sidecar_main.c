
/*
 * ARC sidecar main (plan prompt W4.3/W6.9/W6.10, design 03).
 *
 * Sits between a recovery buffer and the engine apply callback
 * (design 03's objective): entries arrive over an SPSC ring (the
 * recovery side never blocks), the sidecar's poll loop drives
 * arc_poll on the receiver clock, and released entries leave through
 * the registered apply callback. Modes per design 03:
 *   shadow — the pacer runs ARC-QG and records its schedule, while the
 *            sidecar applies each accepted entry immediately and
 *            suppresses the later schedule apply (W6.10);
 *   live   — the pacer owns the apply timing (ARC_MODE_QG);
 *   killed — the IMMEDIATE oracle (design 03's kill switch: bypass
 *            the pacer entirely, data flows at arrival).
 * The W0.2 SPSC ring carries the entries; the batch drain bounds are
 * the sidecar loop's, not the schedule's. Entries accepted by the ring
 * but refused by a full pacer are retained in the pump-owned pending
 * slot and retried before another ring pop (W6.9).
 */

#include "arc_internal.h"

#include "sdf/common/metrics.h"
#include "sdf/common/metrics_endpoint.h"
#include "sdf/common/ring.h"
#include "sdf/common/timer.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SIDECAR_SLOT_BYTES 248u
#define SIDECAR_SHADOW_RES 256u

typedef struct {
    uint64_t producer_ts;
    uint32_t len;
    uint32_t _pad;
    uint8_t bytes[SIDECAR_SLOT_BYTES]; /* entry_max upper bound for the ring slot */
} sidecar_slot_t;

typedef struct {
    spsc_ring_t *in;
    uint8_t *in_mem;
    _Atomic int running;
    arc_pacer_t *pacer;
    _Atomic int mode; /* desired: 0 shadow, 1 live, 2 killed */
    int applied_mode; /* last mode pushed into the pacer (pump-owned) */
    uint32_t entry_max;
    uint64_t applied;

    /* W6.9: an entry the ring accepted but the pacer has not yet
     * accepted. Retried before any further ring pop. */
    sidecar_slot_t pending;
    int pending_valid;

    /* W6.10: shadow epoch evidence. */
    arc_shadow_stats_t shadow;
    arc_shadow_record_t shadow_records[ARC_SHADOW_RECORD_CAP];
    uint32_t shadow_record_start;
    uint32_t shadow_record_count;
    uint64_t shadow_added[SIDECAR_SHADOW_RES];
    size_t shadow_added_pos;
    size_t shadow_added_n;
    uint64_t shadow_budget_ns;

    /* W6.14: accepted-entry accounting. Caller-refused entries stay
     * caller-owned and are never counted as drops; invalid offers and
     * engine rejections are distinct drop reasons. */
    sdf_counter_t submit_accepted;
    sdf_counter_t submit_caller_refused;
    sdf_counter_t submit_invalid;
    sdf_counter_t submit_engine;
    sdf_histogram_t release_batch;
    sdf_histogram_t live_added_latency;
    sdf_histogram_t shadow_added_latency;
} sidecar_t;

static sidecar_t g_sidecar;

/* Shadow and live both run the exact QG schedule; killed is the
 * IMMEDIATE oracle. */
static arc_mode_t pacer_mode_for(int mode) {
    return mode == 2 ? ARC_MODE_IMMEDIATE : ARC_MODE_QG;
}

static void shadow_enter_epoch(void) {
    uint64_t epoch = g_sidecar.shadow.epoch + 1;
    memset(&g_sidecar.shadow, 0, sizeof(g_sidecar.shadow));
    g_sidecar.shadow.epoch = epoch;
    g_sidecar.shadow.active = 1;
    g_sidecar.shadow.schedule_mode = (uint32_t)ARC_MODE_QG;
    memset(g_sidecar.shadow_records, 0, sizeof(g_sidecar.shadow_records));
    g_sidecar.shadow_record_start = 0;
    g_sidecar.shadow_record_count = 0;
    g_sidecar.shadow_added_pos = 0;
    g_sidecar.shadow_added_n = 0;
    sdf_histogram_reset(&g_sidecar.shadow_added_latency);
}

static void shadow_add_record(const arc_sched_t *sched) {
    arc_shadow_stats_t *ss = &g_sidecar.shadow;
    arc_shadow_record_t *rec;
    if (g_sidecar.shadow_record_count < ARC_SHADOW_RECORD_CAP) {
        uint32_t idx =
            (g_sidecar.shadow_record_start + g_sidecar.shadow_record_count) % ARC_SHADOW_RECORD_CAP;
        rec = &g_sidecar.shadow_records[idx];
        g_sidecar.shadow_record_count++;
    } else {
        rec = &g_sidecar.shadow_records[g_sidecar.shadow_record_start];
        g_sidecar.shadow_record_start = (g_sidecar.shadow_record_start + 1) % ARC_SHADOW_RECORD_CAP;
        ss->record_dropped++;
    }
    memset(rec, 0, sizeof(*rec));
    rec->block_n = sched->block_n;
    rec->seq = sched->seq;
    rec->S_n = sched->S_n;
    rec->A_n = sched->A_n;
    rec->T_n = sched->T_n;
    rec->len = sched->len;
    rec->immediate_applied = 1;
    ss->record_total++;
    ss->record_count = g_sidecar.shadow_record_count;
}

static void shadow_note_immediate(const arc_sched_t *sched, uint64_t now_ns) {
    arc_shadow_stats_t *ss = &g_sidecar.shadow;
    ss->immediate_applies++;
    ss->last_immediate_apply_ns = now_ns;
    ss->scheduled_backlog++;
    shadow_add_record(sched);
}

static void shadow_note_schedule_release(const arc_rel_t *rel, uint64_t now_ns) {
    arc_shadow_stats_t *ss = &g_sidecar.shadow;
    ss->schedule_releases++;
    ss->schedule_samples++;
    ss->last_schedule_release_ns = now_ns;
    if (ss->scheduled_backlog > 0) {
        ss->scheduled_backlog--;
    }

    uint64_t added = now_ns > rel->A_n ? now_ns - rel->A_n : 0;
    sdf_histogram_observe(&g_sidecar.shadow_added_latency, added);
    if (g_sidecar.shadow_added_n < SIDECAR_SHADOW_RES) {
        g_sidecar.shadow_added[g_sidecar.shadow_added_n++] = added;
    } else {
        g_sidecar.shadow_added[g_sidecar.shadow_added_pos] = added;
        g_sidecar.shadow_added_pos = (g_sidecar.shadow_added_pos + 1) % SIDECAR_SHADOW_RES;
    }

    for (uint32_t i = 0; i < g_sidecar.shadow_record_count; i++) {
        uint32_t idx = (g_sidecar.shadow_record_start + i) % ARC_SHADOW_RECORD_CAP;
        if (g_sidecar.shadow_records[idx].block_n == rel->block_n) {
            g_sidecar.shadow_records[idx].release_ns = now_ns;
            g_sidecar.shadow_records[idx].released = 1;
            break;
        }
    }

    if (g_sidecar.shadow_budget_ns > 0) {
        double p99 = arc_res_pctl(g_sidecar.shadow_added, g_sidecar.shadow_added_n,
                                  SIDECAR_SHADOW_RES, 0.99);
        if (p99 > (double)g_sidecar.shadow_budget_ns) {
            ss->budget_alerts++;
        }
    }
}

static int apply_trampoline(void *ctx, const void *entry, size_t len, uint64_t release_ts) {
    (void)ctx;
    (void)entry;
    (void)len;
    if (g_sidecar.applied_mode == 0) {
        /* The shadow path already applied this entry immediately. The
         * QG callback only records the schedule release; a second
         * apply here would double-count and violate exactly-once. */
        if (g_sidecar.pacer != NULL && g_sidecar.pacer->st.last_release_valid) {
            shadow_note_schedule_release(&g_sidecar.pacer->st.last_release, release_ts);
        }
        return 0;
    }
    if (g_sidecar.applied_mode == 1 && g_sidecar.pacer != NULL &&
        g_sidecar.pacer->st.last_release_valid) {
        uint64_t a_n = g_sidecar.pacer->st.last_release.A_n;
        uint64_t added = release_ts > a_n ? release_ts - a_n : 0;
        sdf_histogram_observe(&g_sidecar.live_added_latency, added);
    }
    g_sidecar.applied++;
    return 0;
}

static void sidecar_apply_immediate(void) {
    uint64_t now = sdf_now_ns();
    g_sidecar.applied++;
    if (g_sidecar.pacer != NULL && g_sidecar.pacer->st.last_sched_valid) {
        shadow_note_immediate(&g_sidecar.pacer->st.last_sched, now);
    } else {
        /* A pacer-accepted entry without a schedule is an internal
         * invariant violation; count the apply but do not fabricate a
         * record. */
        g_sidecar.shadow.immediate_applies++;
        g_sidecar.shadow.last_immediate_apply_ns = now;
    }
}

static int sidecar_submit_one(const sidecar_slot_t *slot) {
    int rc = arc_submit(g_sidecar.pacer, slot->bytes, slot->len, slot->producer_ts);
    if (rc == -EAGAIN) {
        return -EAGAIN;
    }
    if (rc != 0 && rc != 1) {
        /* The ring already accepted this entry, so an engine rejection
         * is a real sidecar drop rather than caller backpressure. */
        sdf_counter_inc(&g_sidecar.submit_engine);
        return rc;
    }
    if (g_sidecar.applied_mode == 0) {
        sidecar_apply_immediate();
    }
    return 0;
}

/* Drain as much of the ring as the pacer will accept. A refused entry
 * is retained in the pending slot, not discarded (W6.9). */
static int sidecar_ingest(void) {
    int accepted = 0;
    if (g_sidecar.pending_valid) {
        int rc = sidecar_submit_one(&g_sidecar.pending);
        if (rc == -EAGAIN) {
            return 0;
        }
        g_sidecar.pending_valid = 0;
        accepted++;
    }

    sidecar_slot_t slot;
    while (spsc_try_pop(g_sidecar.in, &slot)) {
        int rc = sidecar_submit_one(&slot);
        if (rc == -EAGAIN) {
            g_sidecar.pending = slot;
            g_sidecar.pending_valid = 1;
            break;
        }
        if (rc != 0) {
            /* Engine-input violation: the ring/sidecar length checks
             * should have refused this entry before acceptance. */
            continue;
        }
        accepted++;
    }
    return accepted;
}

static void note_release_batch(int released) {
    if (released > 0) {
        sdf_histogram_observe(&g_sidecar.release_batch, (uint64_t)released);
    }
}

static void sidecar_poll_all(void) {
    uint64_t now = sdf_now_ns();
    arc_flush_ready(&g_sidecar.pacer->st, now);
    for (int i = 0; i < 1000000; i++) {
        if (g_sidecar.pacer == NULL || g_sidecar.pacer->st.heap_n == 0) {
            break;
        }
        int released = arc_poll(g_sidecar.pacer, now);
        note_release_batch(released);
        if (released <= 0) {
            break;
        }
    }
}

static void sidecar_apply_transition(int want) {
    int have = g_sidecar.applied_mode;
    if (want == have) {
        return;
    }

    if (have == 0) {
        /* Leaving shadow: every queued schedule entry was already
         * applied immediately. Drop the schedule, not the applies. */
        arc_discard_all(&g_sidecar.pacer->st);
        g_sidecar.shadow.scheduled_backlog = 0;
        g_sidecar.shadow.active = 0;
    } else if (have == 1 && want == 2) {
        /* The kill path drains the backlog itself, but re-key it first
         * so the kill preserves submit order instead of heap T_n order. */
        arc_flush_ready(&g_sidecar.pacer->st, sdf_now_ns());
    } else {
        /* Leaving live or killed: deliver the accepted backlog before
         * changing semantics. */
        sidecar_poll_all();
    }

    if (want == 0) {
        (void)arc_set_mode(g_sidecar.pacer, ARC_MODE_QG);
        shadow_enter_epoch();
    } else if (want == 1) {
        (void)arc_set_mode(g_sidecar.pacer, ARC_MODE_QG);
    } else {
        (void)arc_set_mode(g_sidecar.pacer, ARC_MODE_IMMEDIATE);
    }
    g_sidecar.applied_mode = want;
}

int arc_sidecar_start(const arc_cfg_t *cfg, int mode) {
    if (cfg == NULL) {
        return -EINVAL;
    }
    if (mode < 0 || mode > 2) {
        return -EINVAL;
    }

    memset(&g_sidecar, 0, sizeof(g_sidecar));
    atomic_store(&g_sidecar.mode, mode);
    g_sidecar.applied_mode = mode;
    g_sidecar.entry_max = cfg->entry_max != 0 ? cfg->entry_max : 256u;
    if (g_sidecar.entry_max > SIDECAR_SLOT_BYTES) {
        g_sidecar.entry_max = SIDECAR_SLOT_BYTES;
    }
    g_sidecar.shadow_budget_ns = cfg->budget_ns;

    size_t need = arc_sizeof(cfg);
    if (need == 0) {
        return -EINVAL;
    }
    g_sidecar.pacer = malloc(need);
    if (g_sidecar.pacer == NULL) {
        return -ENOMEM;
    }

    arc_cfg_t eff = *cfg;
    eff.mode = pacer_mode_for(mode);
    if (arc_init(g_sidecar.pacer, &eff, apply_trampoline, NULL) != 0) {
        free(g_sidecar.pacer);
        g_sidecar.pacer = NULL;
        return -EINVAL;
    }

    size_t qsz = spsc_sizeof(cfg->capacity ? cfg->capacity : 64, (uint32_t)sizeof(sidecar_slot_t));
    if (qsz == 0) {
        free(g_sidecar.pacer);
        g_sidecar.pacer = NULL;
        return -EINVAL;
    }
    g_sidecar.in_mem = aligned_alloc(64, (qsz + 63u) & ~63u);
    if (g_sidecar.in_mem == NULL) {
        free(g_sidecar.pacer);
        g_sidecar.pacer = NULL;
        return -ENOMEM;
    }
    g_sidecar.in = (spsc_ring_t *)g_sidecar.in_mem;
    if (spsc_init(g_sidecar.in, g_sidecar.in_mem, cfg->capacity ? cfg->capacity : 64,
                  (uint32_t)sizeof(sidecar_slot_t)) != 0) {
        free(g_sidecar.in_mem);
        free(g_sidecar.pacer);
        g_sidecar.in_mem = NULL;
        g_sidecar.pacer = NULL;
        return -EINVAL;
    }
    if (mode == 0) {
        shadow_enter_epoch();
    }
    atomic_store(&g_sidecar.running, 1);
    return 0;
}

/*
 * The recovery side pushes one entry. Returns 0, -EAGAIN when the
 * ring is full (the caller drops or slows — telemetry is not worth
 * stalling recovery).
 */
int arc_sidecar_submit(const void *entry, size_t len, uint64_t producer_ts) {
    if (!atomic_load(&g_sidecar.running)) {
        return -EINVAL;
    }
    if ((entry == NULL && len > 0) || len > g_sidecar.entry_max || len > SIDECAR_SLOT_BYTES) {
        sdf_counter_inc(&g_sidecar.submit_invalid);
        return -EINVAL;
    }
    sidecar_slot_t slot;
    memset(&slot, 0, sizeof(slot));
    slot.producer_ts = producer_ts;
    slot.len = (uint32_t)len;
    if (len > 0) {
        memcpy(slot.bytes, entry, len);
    }
    if (!spsc_try_push(g_sidecar.in, &slot)) {
        sdf_counter_inc(&g_sidecar.submit_caller_refused);
        return -EAGAIN;
    }
    sdf_counter_inc(&g_sidecar.submit_accepted);
    return 0;
}

/* One poll iteration: apply a requested mode flip, drain the ring into
 * ARC (retaining a refused entry), then ARC into apply. */
int arc_sidecar_pump_once(void) {
    if (!atomic_load(&g_sidecar.running)) {
        return -EINVAL;
    }
    int want = atomic_load(&g_sidecar.mode);
    sidecar_apply_transition(want);

    int submitted = sidecar_ingest();
    int released = arc_poll(g_sidecar.pacer, sdf_now_ns());
    note_release_batch(released);

    /* Killed is the oracle: deliver the whole backlog now, not one
     * batch, and not only the entries whose QG T_n has passed. */
    if (g_sidecar.applied_mode == 2) {
        for (int i = 0; i < 1024 && g_sidecar.pacer->st.heap_n > 0; i++) {
            int n = arc_poll(g_sidecar.pacer, sdf_now_ns());
            note_release_batch(n);
            released += n;
            if (n == 0) {
                break;
            }
        }
    }
    return submitted + released;
}

void arc_sidecar_stop(void) {
    if (!atomic_load(&g_sidecar.running)) {
        return;
    }
    arc_sidecar_serve_stop();

    /* Deliver every accepted entry before freeing. In shadow the
     * schedule is evidence only; discard it after the immediate path
     * has applied each entry. In live/killed, drain the pacer. */
    for (int guard = 0; guard < 1000000; guard++) {
        (void)sidecar_ingest();
        if (g_sidecar.applied_mode == 0) {
            arc_discard_all(&g_sidecar.pacer->st);
            g_sidecar.shadow.scheduled_backlog = 0;
        } else {
            sidecar_poll_all();
        }
        if (!g_sidecar.pending_valid && spsc_count(g_sidecar.in) == 0 &&
            g_sidecar.pacer->st.heap_n == 0) {
            break;
        }
    }

    g_sidecar.pending_valid = 0;
    g_sidecar.shadow.active = 0;
    g_sidecar.shadow.scheduled_backlog = 0;
    atomic_store(&g_sidecar.running, 0);
    free(g_sidecar.in_mem);
    free(g_sidecar.pacer);
    g_sidecar.in_mem = NULL;
    g_sidecar.in = NULL;
    g_sidecar.pacer = NULL;
}

arc_stats_t arc_sidecar_stats(void) {
    return arc_stats(g_sidecar.pacer);
}

uint64_t arc_sidecar_applied(void) {
    return g_sidecar.applied;
}

int arc_sidecar_mode(void) {
    return atomic_load(&g_sidecar.mode);
}

int arc_sidecar_set_mode(int mode) {
    if (!atomic_load(&g_sidecar.running)) {
        return -EINVAL;
    }
    if (mode < 0 || mode > 2) {
        return -EINVAL;
    }
    atomic_store(&g_sidecar.mode, mode);
    return 0;
}

int arc_sidecar_shadow_stats(arc_shadow_stats_t *out) {
    if (out == NULL || !atomic_load(&g_sidecar.running) || g_sidecar.pacer == NULL) {
        return -EINVAL;
    }
    arc_shadow_stats_t ss = g_sidecar.shadow;
    ss.active = g_sidecar.applied_mode == 0 ? 1u : 0u;
    ss.record_count = g_sidecar.shadow_record_count;
    ss.added_p50_ns =
        arc_res_pctl(g_sidecar.shadow_added, g_sidecar.shadow_added_n, SIDECAR_SHADOW_RES, 0.50);
    ss.added_p95_ns =
        arc_res_pctl(g_sidecar.shadow_added, g_sidecar.shadow_added_n, SIDECAR_SHADOW_RES, 0.95);
    ss.added_p99_ns =
        arc_res_pctl(g_sidecar.shadow_added, g_sidecar.shadow_added_n, SIDECAR_SHADOW_RES, 0.99);
    *out = ss;
    return 0;
}

uint32_t arc_sidecar_shadow_records(arc_shadow_record_t *out, uint32_t max) {
    if (out == NULL || max == 0 || !atomic_load(&g_sidecar.running)) {
        return 0;
    }
    uint32_t n = g_sidecar.shadow_record_count;
    if (n > max) {
        n = max;
    }
    uint32_t skip = g_sidecar.shadow_record_count - n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (g_sidecar.shadow_record_start + skip + i) % ARC_SHADOW_RECORD_CAP;
        out[i] = g_sidecar.shadow_records[idx];
    }
    return n;
}

/* ---- W6.1: the live scrape over the sidecar's own dump -------------- */

static sdf_metrics_endpoint_t *g_sidecar_ep;

int arc_sidecar_render_metrics(FILE *out) {
    if (out == NULL || !atomic_load(&g_sidecar.running) || g_sidecar.pacer == NULL) {
        return -EINVAL;
    }
    arc_stats_t st = arc_stats(g_sidecar.pacer);
    arc_shadow_stats_t ss;
    memset(&ss, 0, sizeof(ss));
    (void)arc_sidecar_shadow_stats(&ss);
    fprintf(out, "sdf_arc_releases_total %llu\n", (unsigned long long)st.releases);
    fprintf(out, "sdf_arc_mode %d\n", arc_sidecar_mode());
    fprintf(out, "sdf_arc_kill_switch %d\n", g_sidecar.applied_mode == 2 ? 1 : 0);
    fprintf(out, "sdf_arc_D_ns %.0f\n", st.D);
    fprintf(out, "sdf_arc_E_ns %.0f\n", st.E);
    fprintf(out, "sdf_arc_heap_depth %u\n", st.heap_depth);
    fprintf(out, "sdf_arc_guard_waits_total %llu\n", (unsigned long long)st.guard_waits);
    fprintf(out, "sdf_arc_added_latency_p99_ns %.0f\n", st.added_p99_ns);
    fprintf(out, "sdf_arc_accepted_total %llu\n",
            (unsigned long long)sdf_counter_read(&g_sidecar.submit_accepted));
    fprintf(out, "sdf_arc_caller_refused_total{reason=\"ring_full\"} %llu\n",
            (unsigned long long)sdf_counter_read(&g_sidecar.submit_caller_refused));
    fprintf(out, "sdf_arc_dropped_total{reason=\"invalid_input\"} %llu\n",
            (unsigned long long)sdf_counter_read(&g_sidecar.submit_invalid));
    fprintf(out, "sdf_arc_dropped_total{reason=\"engine\"} %llu\n",
            (unsigned long long)sdf_counter_read(&g_sidecar.submit_engine));
    fprintf(out, "sdf_arc_shadow_active %u\n", ss.active);
    fprintf(out, "sdf_arc_shadow_epoch %llu\n", (unsigned long long)ss.epoch);
    fprintf(out, "sdf_arc_shadow_schedule_mode %u\n", ss.schedule_mode);
    fprintf(out, "sdf_arc_shadow_schedule_releases_total %llu\n",
            (unsigned long long)ss.schedule_releases);
    fprintf(out, "sdf_arc_shadow_immediate_applies_total %llu\n",
            (unsigned long long)ss.immediate_applies);
    fprintf(out, "sdf_arc_shadow_schedule_samples_total %llu\n",
            (unsigned long long)ss.schedule_samples);
    fprintf(out, "sdf_arc_shadow_budget_alerts_total %llu\n", (unsigned long long)ss.budget_alerts);
    fprintf(out, "sdf_arc_shadow_scheduled_backlog %llu\n",
            (unsigned long long)ss.scheduled_backlog);
    fprintf(out, "sdf_arc_shadow_record_count %u\n", ss.record_count);
    fprintf(out, "sdf_arc_shadow_added_latency_p50_ns %.0f\n", ss.added_p50_ns);
    fprintf(out, "sdf_arc_shadow_added_latency_p95_ns %.0f\n", ss.added_p95_ns);
    fprintf(out, "sdf_arc_shadow_added_latency_p99_ns %.0f\n", ss.added_p99_ns);

    fprintf(out, "# TYPE sdf_arc_release_batch_size histogram\n");
    if (sdf_histogram_render(out, "sdf_arc_release_batch_size", NULL, &g_sidecar.release_batch,
                             1.0) != 0) {
        return -EINVAL;
    }
    fprintf(out, "# TYPE sdf_arc_live_added_latency_ns histogram\n");
    if (sdf_histogram_render(out, "sdf_arc_live_added_latency_ns", NULL,
                             &g_sidecar.live_added_latency, 1.0) != 0) {
        return -EINVAL;
    }
    fprintf(out, "# TYPE sdf_arc_shadow_added_latency_ns histogram\n");
    if (sdf_histogram_render(out, "sdf_arc_shadow_added_latency_ns", NULL,
                             &g_sidecar.shadow_added_latency, 1.0) != 0) {
        return -EINVAL;
    }
    return 0;
}

static int sidecar_render(void *ud, FILE *out) {
    (void)ud;
    return arc_sidecar_render_metrics(out);
}

int arc_sidecar_serve(const char *addr) {
    if (!atomic_load(&g_sidecar.running)) {
        return -EINVAL; /* serve what exists: start() first */
    }
    if (g_sidecar_ep != NULL) {
        return -EEXIST;
    }
    g_sidecar_ep = sdf_metrics_endpoint_start(addr, sidecar_render, NULL);
    return g_sidecar_ep != NULL ? 0 : -EINVAL; /* the endpoint prints its
                                                * own diagnostic; a stable
                                                * -EINVAL beats a stale
                                                * errno */
}

void arc_sidecar_serve_stop(void) {
    if (g_sidecar_ep != NULL) {
        sdf_metrics_endpoint_stop(g_sidecar_ep);
        g_sidecar_ep = NULL;
    }
}
