/*
 * FLUID budget/gate tests (plan prompt W3.4; the O.1/O.5 identity
 * sweeps adapted from wack_a_mole tests/test_fluid.c @ a0f0422).
 *
 * Budget identities over K x eps sweeps with the near-integer snap;
 * efficiency and cost bounds hold everywhere; admit stops exactly at
 * the allowable and resumes only through reports (never-past-allowed,
 * randomized); the loss-product identity pi == L_l / N holds to
 * 1e-12; the gate fires exactly when pi <= eps.
 */

#include "sdf/fluid.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);       \
            exit(1);                                                          \
        }                                                                     \
    } while (0)

static uint64_t rng_state = 0x5EED1234ull;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void test_budget_identities(void) {
    static const uint32_t ks[] = {1, 2, 16, 100, 1024, 4096, 16384, 65536};
    static const double eps[] = {0.01, 0.02, 0.05, 0.10, 0.15, 0.20};

    for (size_t ki = 0; ki < sizeof(ks) / sizeof(ks[0]); ki++) {
        for (size_t ei = 0; ei < sizeof(eps) / sizeof(eps[0]); ei++) {
            uint32_t k = ks[ki];
            uint32_t n = sdf_fluid_budget_n(k, eps[ei]);
            CHECK(n >= k); /* slack only ever adds capacity */
            CHECK((double)n <= (double)k / (1.0 - eps[ei]) + 1.0);
            CHECK(sdf_fluid_efficiency_ok(k, n, eps[ei]));
            CHECK(sdf_fluid_cost_ok(k, n, eps[ei]));
        }
    }

    /* near-integer snap: 16 / (1 - 0.2) = 20 exactly — not 21 */
    CHECK(sdf_fluid_budget_n(16, 0.20) == 20);
    CHECK(sdf_fluid_budget_n(8, 0.5) == 16); /* 8 / 0.5 = 16 exact */
    /* epsilon out of range: budget off (the control arm) */
    CHECK(sdf_fluid_budget_n(64, 0.0) == 64);
    CHECK(sdf_fluid_budget_n(64, 1.5) == 64);
}

static void test_never_past_allowed(void) {
    for (int trial = 0; trial < 200; trial++) {
        uint32_t k = 1u + (uint32_t)(rng() % 512u);
        double e = 0.01 + (double)(rng() % 20u) / 100.0; /* 0.01..0.20 */
        sdf_fluid_budget_t b;
        CHECK(sdf_fluid_budget_init(&b, k, e) == 0);

        uint64_t admitted = 0;
        while (sdf_fluid_budget_admit(&b) == 0)
            admitted++;
        CHECK(admitted == b.n); /* stops exactly at the budget */
        CHECK(sdf_fluid_budget_admit(&b) == 1); /* and stays stopped */

        /* reports raise the allowable through losses only */
        uint64_t recv = b.n / 2; /* half of the wave lost */
        sdf_fluid_budget_report(&b, recv);
        CHECK(sdf_fluid_budget_allowable(&b) == b.n + (b.n - recv));
        uint64_t more = 0;
        while (sdf_fluid_budget_admit(&b) == 0)
            more++;
        CHECK(more == b.n - recv);

        /* reordered (stale) reports never LOWER the allowable */
        sdf_fluid_budget_report(&b, b.n); /* stale: fewer losses */
        CHECK(sdf_fluid_budget_allowable(&b) == b.n + (b.n - recv));
    }
}

static void test_loss_product_identity(void) {
    /* the chain telescopes: after each count report,
     * pi == cumulative_lost / N exactly (paper section 4.3) */
    for (int trial = 0; trial < 50; trial++) {
        uint32_t k = 16u + (uint32_t)(rng() % 256u);
        double e = 0.05 + (double)(rng() % 15u) / 100.0;
        uint32_t n = sdf_fluid_budget_n(k, e);
        CHECK(n > k);

        fluid_gate_t g;
        fg_init(&g, e, n);
        uint64_t sent = 0, recv = 0;
        for (int r = 0; r < 8; r++) {
            fg_round_start(&g);
            uint32_t wave = 1u + (uint32_t)(rng() % 64u);
            uint32_t lost = (uint32_t)(rng() % (wave + 1u));
            sent += wave;
            recv += wave - lost;
            fg_observe(&g, sent, recv);
            uint64_t cum = sent - recv;
            if (cum == 0) {
                CHECK(g.loss_prod == 0.0);
            } else {
                CHECK(fabs(g.loss_prod - (double)cum / (double)n) < 1e-12);
            }
            CHECK((g.loss_prod <= g.eps) == fg_done(&g));

            /* stale report (lower cumulative): rejected, pi unchanged */
            double before = g.loss_prod;
            fg_observe(&g, sent - 1, recv > 0 ? recv - 1 : 0);
            CHECK(g.loss_prod == before);
        }
    }
}

static void test_gate_fires_within_slack(void) {
    /* K = 100, eps = 0.10: N = 112, slack S = 12. Certification means
     * observed losses sit within the slack: pi = L/N <= eps */
    fluid_gate_t g;
    uint32_t n = sdf_fluid_budget_n(100, 0.10);
    CHECK(n == 112);
    fg_init(&g, 0.10, n);

    /* first report: 5 lost of 40 sent — within slack, certified */
    fg_observe(&g, 40, 35);
    CHECK(fabs(g.loss_prod - 5.0 / 112.0) < 1e-12);
    CHECK(fg_done(&g));

    /* heavier loss stream: 20 lost — beyond the slack, not certified */
    fg_init(&g, 0.10, n);
    fg_observe(&g, 100, 80);
    CHECK(fabs(g.loss_prod - 20.0 / 112.0) < 1e-12);
    CHECK(!fg_done(&g));

    /* losses grow past the slack: still not certified (the budget
     * is spent — the coordinator falls back to ARQ, never ships) */
    fg_observe(&g, 112, 80);
    CHECK(!fg_done(&g));

    /* zero losses: certified immediately */
    fg_init(&g, 0.01, n);
    fg_observe(&g, 112, 112);
    CHECK(g.loss_prod == 0.0);
    CHECK(fg_done(&g));
}

int main(void) {
    test_budget_identities();
    test_never_past_allowed();
    test_loss_product_identity();
    test_gate_fires_within_slack();
    printf("test_fluid_gate: ok (O.1 sweep, never-past-allowed x200, "
           "chain identity + stale-reject, slack certification)\n");
    return 0;
}
