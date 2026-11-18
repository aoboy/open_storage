#ifndef SDF_FLUID_H
#define SDF_FLUID_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * FLUID termination gate. Delivery completes when the product of per-round
 * loss fractions falls below eps (the Loss-Product Rule).
 */
typedef struct {
    double eps;
    uint32_t round;
    uint32_t sent_round;
    uint32_t recv_round;
    double loss_prod;
} fluid_gate_t;

void fg_init(fluid_gate_t *g, double eps);
void fg_round_start(fluid_gate_t *g);
void fg_observe(fluid_gate_t *g, uint32_t sent, uint32_t recv);
bool fg_done(const fluid_gate_t *g);

#ifdef __cplusplus
}
#endif

#endif /* SDF_FLUID_H */
