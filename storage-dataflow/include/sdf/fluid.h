#ifndef SDF_FLUID_H
#define SDF_FLUID_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * FLUID slack-based delivery (design 02; [FLUID] Luby, BitRipple
 * BR-TR-2026-01). Two halves compose:
 *
 *   SENDER BUDGET (sdf_fluid_budget_t): the sender never transmits
 *   past a per-block budget N = ceil(K/(1-eps)); count feedback
 *   (received counts) raises the allowable through the losses it
 *   reports. epsilon = 0 disables the budget (plain ARQ behavior —
 *   the control arm of the three-arm A/B).
 *
 *   COORDINATOR GATE (fluid_gate_t): delivery is certified when the
 *   loss-product pi falls to eps. The product is the paper section
 *   4.3 cumulative-loss chain — f_1 = L_1 / N, f_r = L_r / L_(r-1)
 *   — which telescopes exactly to pi = L_l / N: the gate IS
 *   cumulative-lost over budget, evaluated per count report, with
 *   stale (reordered, lower-cumulative) reports rejected (the wam
 *   O.5 rule). Certification means the observed losses sit within
 *   the budget's slack S = N - K. The gate never replaces decode
 *   sufficiency: the coordinator checks fd_ready() first and falls
 *   back to ARQ when the gate fires early (design 02's safety).
 */

/* ---- sender budget (plan O.1 math, imported from wam fluid.h) --------- */

/*
 * Block budget N = ceil(K / (1 - eps)) with the near-integer snap: a
 * 1-ulp overshoot in K/(1-eps) must not inflate N by a whole symbol.
 * Returns K when eps is out of (0, 1) (budget off).
 */
uint32_t sdf_fluid_budget_n(uint32_t k, double eps);

/* Delivery-efficiency bound: K/N >= (1-eps) * K/(K+1). */
int sdf_fluid_efficiency_ok(uint32_t k, uint32_t n, double eps);

/* Cost-ratio bound: N/K <= 1/(1-eps) + 1/K. */
int sdf_fluid_cost_ok(uint32_t k, uint32_t n, double eps);

typedef struct {
    uint32_t k;      /* source symbols in the block */
    uint32_t n;      /* budget ceil(K/(1-eps)); == k when eps == 0 */
    _Atomic uint64_t sent;       /* frames admitted (single blaster) */
    _Atomic uint64_t reported_lost; /* cumulative reported losses
                                        (monotone max of seen) */
} sdf_fluid_budget_t;

int sdf_fluid_budget_init(sdf_fluid_budget_t *b, uint32_t k, double eps);

/*
 * Admit one frame. Returns 0 when the budget allows it (the caller
 * transmits), 1 when the allowable is exhausted (the caller waits
 * for the next count report — or a block timer).
 */
int sdf_fluid_budget_admit(sdf_fluid_budget_t *b);

/*
 * Count feedback (plan O.2): the receiver's received-count report.
 * lost = max(seen, sent - received) keeps the allowable monotone
 * under report reordering.
 */
void sdf_fluid_budget_report(sdf_fluid_budget_t *b, uint64_t received);

/* Current allowable: N + cumulative reported losses. */
uint64_t sdf_fluid_budget_allowable(const sdf_fluid_budget_t *b);

/* ---- coordinator gate (the cumulative-loss chain) ---------------------- */

typedef struct {
    double eps;
    uint64_t budget_n;  /* the chain's L_0 denominator */
    uint32_t round;     /* count reports folded */
    uint32_t sent_round; /* wave telemetry (metrics only) */
    uint32_t recv_round;
    double loss_prod;   /* == cumulative_lost / N exactly (section 4.3) */
    uint64_t lost_prev; /* L_(r-1): stale reports with less are ignored */
} fluid_gate_t;

void fg_init(fluid_gate_t *g, double eps, uint32_t budget_n);
void fg_round_start(fluid_gate_t *g);

/*
 * Fold one count report (plan O.2/O.5): sent/recv are CUMULATIVE
 * totals, L = sent - recv. Stale reports (L below the last folded
 * cumulative) are rejected; zero losses certify immediately.
 */
void fg_observe(fluid_gate_t *g, uint64_t sent_total, uint64_t recv_total);
bool fg_done(const fluid_gate_t *g);

#ifdef __cplusplus
}
#endif

#endif /* SDF_FLUID_H */
