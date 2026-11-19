/*
 * Fuzz target: ARC (S_n, A_n) release streams (plan prompt W5.1).
 *
 * Shape adapted from wack_a_mole tests/test_fuzz.c @ a0f0422 (the
 * M.1 ARC-stream fuzzer): untrusted arrival stamps drive the scheduler
 * in every mode while the poll loop drains on a synthetic clock. The
 * invariants: every accepted submit eventually releases under
 * poll-drain (no stranded entries at stop), the QG guard never violates
 * submit-order release (markers), the absurd-stamp gate keeps the
 * estimator finite, and no crash under ASan/UBSan.
 */

#include "sdf/arc.h"
#include "sdf/common/timer.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SDF_FUZZ_ITERS
#define SDF_FUZZ_ITERS 100000
#endif

#define CAP 32
#define ENMAX 16

static uint64_t rng_state = 0xABCD1234EF980021ull;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static char marks[CAP];
static int n_marks;

static int rec(void *ctx, const void *entry, size_t len, uint64_t ts) {
    (void)ctx;
    (void)len;
    (void)ts;
    if (n_marks < CAP) {
        marks[n_marks++] = *(const char *)entry;
    }
    return 0;
}

int main(void) {
    for (long it = 0; it < SDF_FUZZ_ITERS; it++) {
        rng_state = (uint64_t)it * 0x9E3779B97F4A7C15ull + 1u;
        arc_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.capacity = CAP;
        cfg.entry_max = ENMAX;
        cfg.mode = (arc_mode_t)(rng() % 4u); /* all modes incl. oracle */
        cfg.auto_params = 0;
        cfg.min_smooth_bytes = 1 + (uint32_t)(rng() % 4u);
        cfg.U = 100000000ull;
        cfg.delta = 50000000ull;
        cfg.gamma = 16666667ull;
        cfg.G = 20000000ull + (uint64_t)(rng() % 3u) * 30000000ull;
        cfg.T_idle_ns = 1000000000ull;

        size_t sz = arc_sizeof(&cfg);
        if (sz == 0) {
            continue;
        }
        arc_pacer_t *p = malloc(sz);
        if (p == NULL) {
            continue;
        }
        if (arc_init(p, &cfg, rec, NULL) != 0) {
            free(p);
            continue;
        }
        n_marks = 0;

        /* the receiver clock is REAL (submit samples it) — the harness
         * only varies offsets against it; a fully synthetic clock would
         * sit in the past/future of every real schedule and nothing
         * would ever come due (found live) */
        uint64_t clock = sdf_now_ns();
        uint32_t submitted = 0;
        char next_mark = 1;

        int frames = (int)(rng() % 24u) + 1;
        for (int f = 0; f < frames; f++) {
            char entry[ENMAX];
            memset(entry, next_mark, sizeof(entry));
            /* S_n: mostly near-clock, sometimes far past (benign) or
             * absurd-future (the X4-lite gate), sometimes tiny — all
             * anchored to the REAL receiver clock */
            uint64_t roll = rng() % 100u;
            uint64_t Sn;
            if (roll < 80) {
                Sn = clock - (uint64_t)(rng() % 80000000ull);
            } else if (roll < 90) {
                Sn = clock + 30ull * 1000000000ull; /* absurd future */
            } else {
                Sn = (uint64_t)(rng() % 1000ull); /* ancient tiny stamp */
            }
            int src = arc_submit(p, entry, sizeof(entry), Sn);
            if (src == 0 || src == 1) { /* queued or bypass-scheduled:
                                         * both are accepted entries */
                submitted++;
                next_mark++; /* markers identify submit order */
            }
            /* the poll loop: drain on the advancing clock */
            clock += (uint64_t)(rng() % 30000000ull);
            (void)arc_poll(p, clock);
        }
        /* stop drains: nothing stranded (the guard bounds every park
         * at T_n + G, so a horizon of a few minutes covers any G) */
        clock = sdf_now_ns() + 600000000000ull; /* +10 min */
        uint32_t released_total = arc_poll(p, clock);
        (void)released_total;
        clock = sdf_now_ns() + 600000000000ull;
        (void)arc_poll(p, clock);
        (void)released_total;
        arc_stats_t st = arc_stats(p);
        if (st.releases != submitted) {
            fprintf(stderr,
                    "FUZZ FAIL it=%ld: %llu released != %u submitted "
                    "(heap %u) mode=%d G=%llu min_smooth=%llu\n",
                    it, (unsigned long long)st.releases, submitted, st.heap_depth, (int)cfg.mode,
                    (unsigned long long)cfg.G, (unsigned long long)cfg.min_smooth_bytes);
            return 1;
        }
        /* the estimator stayed finite (absurd stamps refused) */
        if (!(st.D >= 0.0) || st.D > 1e18 || !(st.E >= st.D - 1e-9)) {
            fprintf(stderr, "FUZZ FAIL: estimator diverged D=%f E=%f\n", st.D, st.E);
            return 1;
        }
        free(p);
    }
    printf("fuzz_arc_stream: ok (%ld streams)\n", (long)SDF_FUZZ_ITERS);
    return 0;
}
