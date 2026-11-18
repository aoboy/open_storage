/*
 * wm_select micro-bench (plan prompt W0.5).
 *
 * Measures the per-unit dispatch cost of the v1 core (single table
 * load + AND) over the canonical parity profile, in the wack_a_mole
 * bench_spray shape: warmup, best of ROUNDS, sink-guarded loop,
 * BENCH line with ns/cycles per decision and decisions per second.
 * RECORDED ONLY — the <= 15 cyc/decision gate activates with W2.6
 * once seeded methods land (plan §10); bench_arc lands with W4.2.
 */

#include "bench_common.h"
#include "sdf/wm.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define WARMUP 200000u
#define ITERS 500000u
#define ROUNDS 3

int main(void) {
    wm_profile_t p;
    memset(&p, 0, sizeof(p));
    p.version = 1;
    p.m = 1024;
    p.n = 5;
    /* the golden canonical profile (golden_vectors/wm_select.json) */
    p.b[0] = 127;
    p.b[1] = 400;
    p.b[2] = 200;
    p.b[3] = 173;
    p.b[4] = 124;

    wm_compiled_t c;
    if (wm_profile_compile(&p, &c) != 0) {
        fprintf(stderr, "bench_wm: profile compile failed\n");
        return 1;
    }

    uint64_t counter = 0;
    for (uint32_t i = 0; i < WARMUP; i++)
        sdf_bench_sink += wm_select(&c, counter++);

    double best_ns = -1.0, best_cyc = -1.0;
    for (int r = 0; r < ROUNDS; r++) {
        uint64_t t0 = sdf_bench_now_ns();
        uint64_t c0 = sdf_bench_cycles();
        for (uint32_t i = 0; i < ITERS; i++)
            sdf_bench_sink += wm_select(&c, counter++);
        uint64_t c1 = sdf_bench_cycles();
        uint64_t t1 = sdf_bench_now_ns();
        double ns = (double)(t1 - t0) / (double)ITERS;
        double cyc = c1 > c0 ? (double)(c1 - c0) / (double)ITERS : 0.0;
        if (best_ns < 0.0 || ns < best_ns)
            best_ns = ns;
        if (best_cyc < 0.0 || cyc < best_cyc)
            best_cyc = cyc;
    }

    printf("BENCH wm_select n=5 m=1024 ns_per_dec=%.2f cyc_per_dec=%.1f "
           "Mdec_s=%.1f sink=%" PRIu64 "\n",
           best_ns, best_cyc, 1000.0 / best_ns,
           (uint64_t)sdf_bench_sink);
    printf("# gate activates with W2.6 (<= 15 cyc/decision); recorded only\n");
    return 0;
}
