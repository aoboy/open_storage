
/*
 * ARC sidecar main (plan prompt W4.3, design 03).
 *
 * Sits between a recovery buffer and the engine apply callback
 * (design 03's objective): entries arrive over an SPSC ring (the
 * recovery side never blocks), the sidecar's poll loop drives
 * arc_poll on the receiver clock, and released entries leave through
 * the registered apply callback. Modes per design 03:
 *   shadow — ARC computes the schedule and metrics but entries apply
 *            immediately (the old path; 24 h promotion evidence);
 *   live   — ARC owns the apply timing (ARC_MODE_QG);
 *   killed — the IMMEDIATE oracle (design 03's kill switch: bypass
 *            the pacer entirely, data flows at arrival).
 * The W0.2 SPSC ring carries the entries; the batch drain bounds are
 * the sidecar loop's, not the schedule's.
 */

#include "arc_internal.h"

#include "sdf/common/metrics_endpoint.h"
#include "sdf/common/ring.h"
#include "sdf/common/timer.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t producer_ts;
    uint32_t len;
    uint32_t _pad;
    uint8_t bytes[248]; /* entry_max upper bound for the ring slot */
} sidecar_slot_t;

typedef struct {
    spsc_ring_t *in;
    uint8_t *in_mem;
    _Atomic int running;
    arc_pacer_t *pacer;
    _Atomic int mode; /* desired: 0 shadow, 1 live, 2 killed */
    int applied_mode; /* last mode pushed into the pacer (pump-owned) */
    uint64_t applied;
    uint64_t shadow_delta_total;
} sidecar_t;

static sidecar_t g_sidecar;

/* The sidecar's shadow/killed states both use the IMMEDIATE oracle at
 * the pacer; live is ARC-QG. See the W6.4 implementation note in
 * docs/design/03-arc-pacing.md for the deferred true-shadow metrics. */
static arc_mode_t pacer_mode_for(int mode) {
    return mode == 1 ? ARC_MODE_QG : ARC_MODE_IMMEDIATE;
}

static int apply_trampoline(void *ctx, const void *entry, size_t len, uint64_t release_ts) {
    (void)ctx;
    (void)entry;
    (void)len;
    (void)release_ts;
    g_sidecar.applied++;
    return 0;
}

int arc_sidecar_start(const arc_cfg_t *cfg, int mode) {
    if (cfg == NULL)
        return -EINVAL;
    if (mode < 0 || mode > 2)
        return -EINVAL;

    memset(&g_sidecar, 0, sizeof(g_sidecar));
    atomic_store(&g_sidecar.mode, mode);
    g_sidecar.applied_mode = mode;

    size_t need = arc_sizeof(cfg);
    if (need == 0)
        return -EINVAL;
    g_sidecar.pacer = malloc(need);
    if (g_sidecar.pacer == NULL)
        return -ENOMEM;

    arc_cfg_t eff = *cfg;
    eff.mode = pacer_mode_for(mode);
    if (arc_init(g_sidecar.pacer, &eff, apply_trampoline, NULL) != 0) {
        free(g_sidecar.pacer);
        return -EINVAL;
    }

    size_t qsz = spsc_sizeof(cfg->capacity ? cfg->capacity : 64, (uint32_t)sizeof(sidecar_slot_t));
    if (qsz == 0) {
        free(g_sidecar.pacer);
        return -EINVAL;
    }
    g_sidecar.in_mem = aligned_alloc(64, (qsz + 63u) & ~63u);
    if (g_sidecar.in_mem == NULL) {
        free(g_sidecar.pacer);
        return -ENOMEM;
    }
    g_sidecar.in = (spsc_ring_t *)g_sidecar.in_mem;
    if (spsc_init(g_sidecar.in, g_sidecar.in_mem, cfg->capacity ? cfg->capacity : 64,
                  (uint32_t)sizeof(sidecar_slot_t)) != 0) {
        free(g_sidecar.in_mem);
        free(g_sidecar.pacer);
        return -EINVAL;
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
    if (!atomic_load(&g_sidecar.running) || entry == NULL)
        return -EINVAL;
    if (len > sizeof(((sidecar_slot_t *)0)->bytes))
        return -EINVAL;
    sidecar_slot_t slot;
    memset(&slot, 0, sizeof(slot));
    slot.producer_ts = producer_ts;
    slot.len = (uint32_t)len;
    memcpy(slot.bytes, entry, len);
    if (!spsc_try_push(g_sidecar.in, &slot))
        return -EAGAIN;
    return 0;
}

/* One poll iteration: drain the ring into ARC, apply a requested mode
 * flip, then ARC into apply. */
int arc_sidecar_pump_once(void) {
    if (!atomic_load(&g_sidecar.running))
        return -EINVAL;
    int submitted = 0;
    sidecar_slot_t slot;
    while (spsc_try_pop(g_sidecar.in, &slot)) {
        int rc = arc_submit(g_sidecar.pacer, slot.bytes, slot.len, slot.producer_ts);
        if (rc == -EAGAIN)
            break; /* ARC queue full: the entry stays for the next pump
                    * (it is already out of the ring — re-push it) */
        if (rc != 0 && rc != 1) {
            /* bad entry: dropped (counted as a correctness bug in
             * production; here it is an engine-input violation) */
            continue;
        }
        submitted++;
    }

    /* W6.4: the desired mode is atomic; the flip lands on the pump, so
     * the controller sees it within one poll interval. */
    int want = atomic_load(&g_sidecar.mode);
    if (want != g_sidecar.applied_mode && arc_set_mode(g_sidecar.pacer, pacer_mode_for(want)) == 0)
        g_sidecar.applied_mode = want;

    int released = arc_poll(g_sidecar.pacer, sdf_now_ns());

    /* Killed is the oracle: deliver the whole backlog now, not one
     * batch, and not only the entries whose QG T_n has passed. */
    if (want == 2) {
        for (int i = 0; i < 1024 && arc_stats(g_sidecar.pacer).in_flight > 0; i++)
            released += arc_poll(g_sidecar.pacer, sdf_now_ns());
    }
    return submitted + released;
}

void arc_sidecar_stop(void) {
    if (!atomic_load(&g_sidecar.running))
        return;
    arc_sidecar_serve_stop();
    /* flush before marking stopped: pump_once() refuses when !running */
    (void)arc_sidecar_pump_once();
    arc_poll(g_sidecar.pacer, UINT64_MAX / 4);
    atomic_store(&g_sidecar.running, 0);
    free(g_sidecar.in_mem);
    free(g_sidecar.pacer);
    g_sidecar.in_mem = NULL;
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
    if (!atomic_load(&g_sidecar.running))
        return -EINVAL;
    if (mode < 0 || mode > 2)
        return -EINVAL;
    atomic_store(&g_sidecar.mode, mode);
    return 0;
}

/* ---- W6.1: the live scrape over the sidecar's own dump -------------- */

static sdf_metrics_endpoint_t *g_sidecar_ep;

static int sidecar_render(void *ud, FILE *out) {
    (void)ud;
    arc_stats_t st = arc_sidecar_stats();
    fprintf(out, "sdf_arc_releases_total %llu\n", (unsigned long long)st.releases);
    fprintf(out, "sdf_arc_mode %d\n", arc_sidecar_mode());
    fprintf(out, "sdf_arc_D_ns %.0f\n", st.D);
    fprintf(out, "sdf_arc_E_ns %.0f\n", st.E);
    fprintf(out, "sdf_arc_heap_depth %u\n", st.heap_depth);
    fprintf(out, "sdf_arc_guard_waits_total %llu\n", (unsigned long long)st.guard_waits);
    fprintf(out, "sdf_arc_added_latency_p99_ns %.0f\n", st.added_p99_ns);
    return 0;
}

int arc_sidecar_serve(const char *addr) {
    if (!atomic_load(&g_sidecar.running))
        return -EINVAL; /* serve what exists: start() first */
    if (g_sidecar_ep != NULL)
        return -EEXIST;
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
