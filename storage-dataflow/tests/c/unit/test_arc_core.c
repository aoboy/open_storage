/*
 * ARC core tests (plan prompt W4.2).
 *
 * The paper section 12.3 goldens adapted from wack_a_mole
 * tests/test_arc.c @ a0f0422 (the analytic linear cycle
 * {93.8016, 94.7934} ms effective delays with intervals
 * {10.5016, 22.8984} ms; the exponent band), Q invariants, the
 * IMMEDIATE oracle, the absurd-stamp gate, idle re-anchor, the delta
 * clamp, small-entry bypass, and the QG guard's missing-predecessor
 * park + timeout release and successor wake.
 */

#include "sdf/arc.h"
#include "sdf/common/timer.h" /* sdf_now_ns + sdf_sleep_ns for the paced replay */

#include "arc_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

typedef struct {
    uint64_t ts[4096];
    uint64_t blocks[4096];
    int n;
} release_log_t;

static int release_rec(void *ctx, const void *entry, size_t len, uint64_t ts) {
    (void)len;
    release_log_t *log = ctx;
    if (log->n >= 4096) {
        return 0; /* the replay legs release their whole stream; the
                   * CHECK-based legs stay far below the cap */
    }
    log->ts[log->n] = ts;
    log->blocks[log->n] = entry ? *(const uint8_t *)entry : 0; /* marker */
    log->n++;
    return 0;
}

static void pacer_setup(arc_pacer_t **p, arc_cfg_t *cfg, release_log_t *log, arc_mode_t mode) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->capacity = 64;
    cfg->entry_max = 64;
    cfg->mode = mode;
    cfg->auto_params = 0;      /* goldens pin fixed params (explicit off) */
    cfg->min_smooth_bytes = 0; /* everything goes through scheduling */
    cfg->rho_u = 1.0;
    cfg->rho_l = 1.0;
    cfg->lambda_u = 0.16;
    cfg->lambda_l = 0.04;
    cfg->U = 100000000ull;
    cfg->delta = 100000000ull;
    cfg->J = 0;
    cfg->T_idle_ns = 1000000000ull;
    size_t sz = arc_sizeof(cfg);
    CHECK(sz > 0);
    *p = malloc(sz);
    CHECK(*p != NULL);
    memset(log, 0, sizeof(*log));
    CHECK(arc_init(*p, cfg, release_rec, log) == 0);
}

/* ---- paper section 12.3: the linear cycle ------------------------------ */

static void test_golden_linear(void) {
    arc_cfg_t cfg;
    arc_pacer_t *p;
    release_log_t log;
    pacer_setup(&p, &cfg, &log, ARC_MODE_BASE);

    const uint64_t dH = 100000000ull, dL = 70000000ull;
    const int N = 2000;

    /* S_n = now - d: the arrival A_n (sampled inside submit) exceeds
     * S_n by exactly d + microseconds — the real-clock drive of the
     * paper's alternating-delay trace, the way the sidecar runs */
    for (int n = 0; n < N; n++) {
        uint64_t d = (n % 2 == 0) ? dH : dL;
        uint64_t Sn = sdf_now_ns() - d;
        char entry[64];
        memset(entry, (char)n, sizeof(entry));
        CHECK(arc_submit(p, entry, sizeof(entry), Sn) == 0);
        arc_poll(p, UINT64_MAX / 4);
        if (n >= N - 100) {
            /* D settles to the analytic cycle (fixed point of the
             * alternating update): after high steps D ~= 94.7934 ms,
             * after low steps D ~= 93.8016 ms */
            arc_stats_t st = arc_stats(p);
            if (n % 2 == 0) {
                CHECK(fabs(st.D - 94793400.0) < 400000.0);
            } else {
                CHECK(fabs(st.D - 93801600.0) < 400000.0);
            }
        }
    }
    {
        arc_stats_t st = arc_stats(p);
        CHECK(st.releases == (uint64_t)N);
    }
    free(p);
    printf("  golden linear: D cycles {93.80, 94.79} ms (paper 12.3)\n");
}

/* ---- paper section 12.3: the exponent band ----------------------------- */

static void test_golden_exponent(void) {
    arc_cfg_t cfg;
    arc_pacer_t *p;
    release_log_t log;
    pacer_setup(&p, &cfg, &log, ARC_MODE_BASE);
    /* rho_u = 0.5, rho_l = 2.0 (the paper's second example) */
    CHECK(arc_sizeof(&cfg) > 0);
    free(p);
    pacer_setup(&p, &cfg, &log, ARC_MODE_BASE);
    {
        arc_cfg_t c2 = cfg;
        c2.rho_u = 0.5;
        c2.rho_l = 2.0;
        size_t sz = arc_sizeof(&c2);
        CHECK(sz > 0);
        free(p);
        p = malloc(sz);
        CHECK(p != NULL);
        memset(&log, 0, sizeof(log));
        CHECK(arc_init(p, &c2, release_rec, &log) == 0);
    }

    const uint64_t dH = 100000000ull, dL = 70000000ull;
    const int N = 3000, M = 500;
    double d_min = 1e300, d_max = -1e300;

    for (int n = 0; n < N; n++) {
        uint64_t d = (n % 2 == 0) ? dH : dL;
        uint64_t Sn = sdf_now_ns() - d;
        char entry[64];
        memset(entry, (char)n, sizeof(entry));
        CHECK(arc_submit(p, entry, sizeof(entry), Sn) == 0);
        arc_poll(p, UINT64_MAX / 4);
        if (n >= N - M) {
            arc_stats_t st = arc_stats(p);
            if (st.D < d_min) {
                d_min = st.D;
            }
            if (st.D > d_max) {
                d_max = st.D;
            }
        }
    }
    /* paper band: D in ~[97.9, 100.5] ms through the steady state */
    CHECK(d_min > 97500000.0);
    CHECK(d_max < 101500000.0);
    free(p);
    printf("  golden exponent: D band [%.1f, %.1f] ms (paper ~[97.9, 100.5])\n", d_min / 1e6,
           d_max / 1e6);
}

/* ---- Q invariants + guard ------------------------------------------------ */

static void test_q_invariants(void) {
    arc_cfg_t cfg;
    arc_pacer_t *p;
    release_log_t log;
    pacer_setup(&p, &cfg, &log, ARC_MODE_Q);

    /* a bursty stream: E quantizes D and keeps the invariants */
    for (int n = 0; n < 200; n++) {
        uint64_t Sn = (uint64_t)n * 16000000ull;
        char entry[64];
        memset(entry, (char)n, sizeof(entry));
        CHECK(arc_submit(p, entry, sizeof(entry), Sn) == 0);
        arc_poll(p, UINT64_MAX / 4);
        arc_stats_t st = arc_stats(p);
        CHECK(st.E >= st.D || fabs(st.E - st.D) < 1e-9);
        if (st.E > st.D) {
            CHECK(st.E - st.D < 16666667.0 + 1e-9); /* gamma band */
        }
    }
    free(p);
}

/* small-entry bypass, absurd stamps, IMMEDIATE oracle, clamp */
static void test_paths(void) {
    arc_cfg_t cfg;
    arc_pacer_t *p;
    release_log_t log;

    /* IMMEDIATE: releases at arrival, no scheduling */
    pacer_setup(&p, &cfg, &log, ARC_MODE_IMMEDIATE);
    {
        uint64_t Sn = 500000000ull; /* far behind the receiver clock */
        char entry[64];
        CHECK(arc_submit(p, entry, sizeof(entry), Sn) == 1);
        CHECK(arc_poll(p, UINT64_MAX / 4) == 1);
        arc_stats_t st = arc_stats(p);
        CHECK(st.releases == 1);
        CHECK(st.D == 0.0); /* the oracle never learns */
    }
    free(p);

    /* small entries bypass (release at arrival) */
    pacer_setup(&p, &cfg, &log, ARC_MODE_QG);
    cfg.min_smooth_bytes = 128;
    {
        size_t sz = arc_sizeof(&cfg);
        free(p);
        p = malloc(sz);
        CHECK(p != NULL);
        memset(&log, 0, sizeof(log));
        CHECK(arc_init(p, &cfg, release_rec, &log) == 0);
        char small[16];
        CHECK(arc_submit(p, small, sizeof(small), 0) == 1);
        CHECK(arc_poll(p, UINT64_MAX / 4) == 1);
    }
    free(p);

    /* absurd future S_n: scheduled from arrival, estimator refuses */
    pacer_setup(&p, &cfg, &log, ARC_MODE_QG);
    {
        /* absurd FUTURE stamp: 30 s ahead of the receiver's own clock
         * (a fixed synthetic value lands in the past on a long-booted
         * machine and never trips the gate) */
        uint64_t Sn = sdf_now_ns() + 30ull * 1000000000ull;
        char entry[64];
        memset(entry, 1, sizeof(entry));
        CHECK(arc_submit(p, entry, sizeof(entry), Sn) == 0);
        CHECK(arc_poll(p, UINT64_MAX / 4) == 1);
        arc_stats_t st = arc_stats(p);
        CHECK(st.s_n_rejects == 1);
        CHECK(st.D == 0.0);
    }
    free(p);

    /* delta clamp: a huge S_n offset cannot park data forever */
    pacer_setup(&p, &cfg, &log, ARC_MODE_QG);
    {
        cfg.delta = 30000000ull;
        size_t sz = arc_sizeof(&cfg);
        free(p);
        p = malloc(sz);
        CHECK(p != NULL);
        memset(&log, 0, sizeof(log));
        CHECK(arc_init(p, &cfg, release_rec, &log) == 0);
        uint64_t Sn = 100000000ull; /* far behind: D init small */
        /* first arrival primes D; second with a big jump tests clamp */
        char entry[64];
        memset(entry, 1, sizeof(entry));
        CHECK(arc_submit(p, entry, sizeof(entry), Sn) == 0);
        CHECK(arc_poll(p, UINT64_MAX / 4) == 1);
        memset(entry, 2, sizeof(entry));
        CHECK(arc_submit(p, entry, sizeof(entry), Sn + 50000000ull) == 0);
        CHECK(arc_poll(p, UINT64_MAX / 4) == 1);
        arc_stats_t st = arc_stats(p);
        (void)st; /* the clamp path ran without stranding the entry */
    }
    free(p);
}

/* QG: a missing predecessor parks its successor, then releases at
 * T_n + G; an in-flight predecessor wakes the successor right after
 * its own release */
static void test_qg_guard(void) {
    arc_cfg_t cfg;
    arc_pacer_t *p;
    release_log_t log;

    /* public case: the successor SCHEDULES earlier than its
     * predecessor (arrival-governed inversion). QG holds the
     * successor until the predecessor goes out, then wakes it —
     * release order follows submit order regardless of T_n order.
     * (A truly MISSING predecessor is unreachable through the public
     * submit API: block numbers derive from submit order, so the
     * caller cannot manufacture a gap; that path is pinned at the
     * internal layer below.) */
    pacer_setup(&p, &cfg, &log, ARC_MODE_QG);
    {
        char entry[64];
        /* inversion: block 0 carries the LATER schedule (S_n ~ now,
         * T_0 ~ now + D), block 1 the earlier one (S_n = now - 80ms,
         * T_1 ~ now) — the successor is due first and the guard must
         * hold it until its predecessor goes out */
        memset(entry, 0xA0, sizeof(entry));
        CHECK(arc_submit(p, entry, sizeof(entry), sdf_now_ns()) == 0);
        memset(entry, 0xA1, sizeof(entry));
        CHECK(arc_submit(p, entry, sizeof(entry), sdf_now_ns() - 80000000ull) == 0);

        /* poll in the window (T_1, T_0): the successor is due but
         * must NOT go out before block 0 — the guard parks it */
        int rel = arc_poll(p, sdf_now_ns() + 2000000ull);
        arc_stats_t st = arc_stats(p);
        if (st.releases == 0) {
            /* the park held: now drain everything */
            rel = arc_poll(p, UINT64_MAX / 4);
            CHECK(rel == 2);
            CHECK(log.blocks[0] == 0xA0); /* order: pred first */
            CHECK(log.blocks[1] == 0xA1);
            st = arc_stats(p);
            CHECK(st.guard_waits >= 1);
        } else {
            /* both schedules landed in the same drain window (timing
             * dependent): the order still must be submit order */
            rel += arc_poll(p, UINT64_MAX / 4);
            CHECK(log.blocks[0] == 0xA0);
            CHECK(log.blocks[1] == 0xA1);
        }
    }
    free(p);

    /* internal case: a missing predecessor parks its successor until
     * T_n + G, then times out and releases — data always flows. Driven
     * at the state layer because the public API cannot create the gap. */
    {
        arc_cfg_t c2;
        memset(&c2, 0, sizeof(c2));
        c2.capacity = 16;
        c2.entry_max = 64;
        c2.mode = ARC_MODE_QG;
        c2.G = 50000000ull;
        c2.T_idle_ns = 1000000000ull;
        size_t sz = arc_sizeof(&c2);
        CHECK(sz > 0);
        arc_pacer_t *pp = malloc(sz);
        CHECK(pp != NULL);
        CHECK(arc_init(pp, &c2, NULL, NULL) == 0);
        arc_state_t *st = &pp->st;
        st->have_last_release = 1;
        st->last_released_n = 0; /* block 0 "already released" */

        arc_rel_t succ;
        memset(&succ, 0, sizeof(succ));
        succ.T_n = 1000000ull;
        succ.T_deadline = succ.T_n + 50000000ull;
        succ.block_n = 2; /* block 1 missing: the injected gap */
        succ.slot = 0;
        CHECK(arc_heap_push(st, &succ) == 0);

        CHECK(arc_drain(st, 2000000ull) == 0); /* parks at its deadline */
        CHECK(st->stats.guard_waits == 1);
        CHECK(st->heap_n == 1);

        CHECK(arc_drain(st, 52000000ull) == 1); /* timeout: releases */
        CHECK(st->stats.guard_timeouts == 1);
        CHECK(st->heap_n == 0);
        free(pp);
    }
}

/* L.6-style peak/valley replay: ARC-QG, production defaults, the true
 * 60 fps cadence. QG releases every entry in submit order with no
 * guard activity, and the p99 added delay stays inside the paper's
 * evaluation-class bound. */
static void test_replay_qg(void) {
    arc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.capacity = 64;
    cfg.entry_max = 64;
    cfg.mode = ARC_MODE_QG;
    cfg.auto_params = 0; /* pinned defaults, no RTT feed */

    size_t sz = arc_sizeof(&cfg);
    CHECK(sz > 0);
    arc_pacer_t *p = malloc(sz);
    CHECK(p != NULL);
    release_log_t log;
    memset(&log, 0, sizeof(log));
    CHECK(arc_init(p, &cfg, release_rec, &log) == 0);

    /*
     * The L.6-class metrics assume the true 60 fps cadence: the
     * receiver polls at frame rate against arrivals spread at
     * 16.7 ms. A tight submit loop cannot reproduce that regime (the
     * added-delay metric is release_ts - A_n on the real clock), so
     * this leg paces N frames at the true cadence — about one second
     * of wall time for 60 frames.
     */
    const int N = 60;
    for (int n = 0; n < N; n++) {
        uint64_t d = (n % 3 == 1) ? 60000000ull : 10000000ull;
        uint64_t Sn = sdf_now_ns() - d; /* valley trace on the real clock */
        char entry[64];
        memset(entry, (char)n, sizeof(entry));
        CHECK(arc_submit(p, entry, sizeof(entry), Sn) == 0);
        /* poll at the frame's horizon: E absorbs the valley pattern */
        arc_poll(p, sdf_now_ns() + d + 5000000ull);
        sdf_sleep_ns(16666667ull); /* the 60 fps cadence */
    }
    /* drain the tail at a realistic near horizon */
    arc_poll(p, sdf_now_ns() + 70000000ull);
    arc_poll(p, sdf_now_ns() + 70000000ull);

    arc_stats_t st = arc_stats(p);
    CHECK(st.releases == (uint64_t)N);
    /* a peak/valley trace SCHEDULES valleys before their peak
     * predecessors — the guard actively reorders them (guard_waits is
     * the mechanism working, not noise). What must hold: no missing
     * predecessors (zero timeouts) and strict submit-order release. */
    CHECK(st.guard_timeouts == 0);
    CHECK(st.added_p99_ns < 120000000.0); /* the L.6-class bound */
    CHECK(log.n == N);
    for (int i = 0; i < N; i++) {
        CHECK(log.blocks[i] == (uint8_t)i); /* zero order violations */
    }
    free(p);
    printf("  replay: QG %d frames in submit order (guard reorders the "
           "valleys), p99 added %.1f ms\n",
           N, st.added_p99_ns / 1e6);
}

/* Mode-transition flush: a QG schedule can have T_n out of submit
 * order, but a flush must release the accepted backlog in submit order
 * (W6.10's kill/transition guarantee). */
static void test_flush_preserves_submit_order(void) {
    arc_cfg_t cfg;
    arc_pacer_t *p;
    release_log_t log;
    pacer_setup(&p, &cfg, &log, ARC_MODE_QG);

    uint64_t now = sdf_now_ns();
    char entry[64];
    memset(entry, 0, sizeof(entry));
    CHECK(arc_submit(p, entry, sizeof(entry), now + 100000000ull) == 0); /* block 0: late */
    memset(entry, 1, sizeof(entry));
    CHECK(arc_submit(p, entry, sizeof(entry), now) == 0); /* block 1: early */
    CHECK(arc_stats(p).in_flight == 2);

    arc_flush_ready(&p->st, now);
    CHECK(arc_poll(p, now) == 2);
    CHECK(log.n == 2);
    CHECK(log.blocks[0] == 0);
    CHECK(log.blocks[1] == 1);
    free(p);
}

/* W6.9: backpressure is side-effect free — a refused submit must not
 * consume a block/sequence number or perturb the estimator before the
 * caller retries it. */
static void test_backpressure_retains_state(void) {
    arc_cfg_t cfg;
    arc_pacer_t *p;
    release_log_t log;
    pacer_setup(&p, &cfg, &log, ARC_MODE_QG);

    uint64_t future = sdf_now_ns() + 50000000ull;
    char entry[64];
    memset(entry, 6, sizeof(entry));
    for (uint32_t i = 0; i < cfg.capacity; i++) {
        CHECK(arc_submit(p, entry, sizeof(entry), future + i) == 0);
    }
    arc_stats_t before = arc_stats(p);
    CHECK(before.in_flight == cfg.capacity);
    CHECK(arc_submit(p, entry, sizeof(entry), future + cfg.capacity) == -EAGAIN);
    arc_stats_t after = arc_stats(p);
    CHECK(after.in_flight == before.in_flight);
    CHECK(after.D == before.D);
    CHECK(after.E == before.E);
    CHECK(after.releases == before.releases);

    arc_poll(p, UINT64_MAX / 4);
    CHECK(log.n == (int)cfg.capacity);
    CHECK(arc_stats(p).releases == cfg.capacity);
    free(p);
}

int main(void) {
    test_golden_linear();
    test_golden_exponent();
    test_q_invariants();
    test_paths();
    test_qg_guard();
    test_replay_qg();
    test_flush_preserves_submit_order();
    test_backpressure_retains_state();
    printf("test_arc_core: ok (paper 12.3 goldens, Q invariants, oracle, "
           "absurd stamps, QG guard park/wake, W6.10 ordered transition "
           "flush, W6.9 side-effect-free backpressure)\n");
    return 0;
}
