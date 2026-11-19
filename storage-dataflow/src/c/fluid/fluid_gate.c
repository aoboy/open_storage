/*
 * FLUID sender budget + coordinator gate (plan prompt W3.4, design 02).
 *
 * Budget math imported from wack_a_mole include/wam/fluid.h @ a0f0422
 * (plan O.1: budget N with the near-integer snap, efficiency and
 * cost-ratio bounds; O.2: count-based feedback raising the allowable
 * through reported losses, monotone under reordering). The gate is
 * the loss-product rule with the paper section 4.3 identity
 * pi = L_l / N — the rolling product and the cumulative quotient are
 * the same number, checked in tests.
 */

#include "sdf/fluid.h"

#include <math.h>
#include <stdatomic.h>
#include <stddef.h>

uint32_t sdf_fluid_budget_n(uint32_t k, double eps) {
    if (k == 0 || !(eps > 0.0) || eps >= 1.0)
        return k;
    double x = (double)k / (1.0 - eps);
    double r = floor(x);
    if (x - r < 1e-9) /* near-integer: snap down before ceil */
        return (uint32_t)r;
    return (uint32_t)ceil(x);
}

int sdf_fluid_efficiency_ok(uint32_t k, uint32_t n, double eps) {
    if (n == 0)
        return 0;
    double lhs = (double)k / (double)n;
    double rhs = (1.0 - eps) * (double)k / (double)(k + 1);
    return lhs + 1e-12 >= rhs;
}

int sdf_fluid_cost_ok(uint32_t k, uint32_t n, double eps) {
    if (k == 0)
        return 0;
    double lhs = (double)n / (double)k;
    double rhs = 1.0 / (1.0 - eps) + 1.0 / (double)k;
    return lhs <= rhs + 1e-12;
}

int sdf_fluid_budget_init(sdf_fluid_budget_t *b, uint32_t k, double eps) {
    if (b == NULL || k == 0)
        return -1;
    b->k = k;
    b->n = sdf_fluid_budget_n(k, eps);
    atomic_store_explicit(&b->sent, 0, memory_order_relaxed);
    atomic_store_explicit(&b->reported_lost, 0, memory_order_relaxed);
    return 0;
}

int sdf_fluid_budget_admit(sdf_fluid_budget_t *b) {
    if (b == NULL)
        return 1;
    uint64_t sent = atomic_load_explicit(&b->sent, memory_order_relaxed);
    if (sent >= sdf_fluid_budget_allowable(b))
        return 1; /* exhausted: wait for a count report or the timer */
    atomic_store_explicit(&b->sent, sent + 1, memory_order_relaxed);
    return 0;
}

void sdf_fluid_budget_report(sdf_fluid_budget_t *b, uint64_t received) {
    if (b == NULL)
        return;
    uint64_t sent = atomic_load_explicit(&b->sent, memory_order_relaxed);
    uint64_t lost = sent > received ? sent - received : 0;
    /* monotone max: reports may arrive out of order (wam O.2 rule) */
    uint64_t seen = atomic_load_explicit(&b->reported_lost, memory_order_relaxed);
    while (lost > seen &&
           !atomic_compare_exchange_weak_explicit(&b->reported_lost, &seen, lost,
                                                  memory_order_relaxed, memory_order_relaxed))
        ;
}

uint64_t sdf_fluid_budget_allowable(const sdf_fluid_budget_t *b) {
    if (b == NULL)
        return 0;
    return (uint64_t)b->n + atomic_load_explicit(&b->reported_lost, memory_order_relaxed);
}

void fg_init(fluid_gate_t *g, double eps, uint32_t budget_n) {
    if (g == NULL)
        return;
    g->eps = eps;
    g->budget_n = budget_n ? budget_n : 1;
    g->round = 0;
    g->sent_round = 0;
    g->recv_round = 0;
    g->loss_prod = 1.0;
    g->lost_prev = 0;
}

void fg_round_start(fluid_gate_t *g) {
    if (g == NULL)
        return;
    g->round++;
    g->sent_round = 0;
    g->recv_round = 0;
}

void fg_observe(fluid_gate_t *g, uint64_t sent_total, uint64_t recv_total) {
    if (g == NULL)
        return;
    uint64_t lost = sent_total > recv_total ? sent_total - recv_total : 0;

    /* stale-reject (wam O.5): a reordered report with a lower
     * cumulative than the last folded one cannot shrink losses */
    if (g->round > 0 && lost < g->lost_prev)
        return;

    /*
     * pi is the telescoped quotient the section 4.3 chain proves:
     * (L_1/N)(L_2/L_1)...(L_l/L_(l-1)) == L_l / N. Implement the
     * quotient directly — a multiplicative fold would wedge at zero
     * after a no-loss report (0 x anything), while the quotient
     * restarts naturally when losses resume.
     */
    g->loss_prod = lost == 0 ? 0.0 : (double)lost / (double)g->budget_n;
    g->lost_prev = lost;
    g->round++;
}

bool fg_done(const fluid_gate_t *g) {
    if (g == NULL)
        return false;
    return g->loss_prod <= g->eps;
}
