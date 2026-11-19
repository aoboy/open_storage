#ifndef SDF_ARC_INTERNAL_H
#define SDF_ARC_INTERNAL_H

/*
 * Internal ARC contract (plan prompt W4.2): arc_pacer.c is the only
 * definition of the public arc_* symbols; the arrival path (arc_offset)
 * and the release queue (arc_heap) are internal halves of the same
 * object. Layout: the public opaque struct is the state + heap array
 * + slab tail, sized by arc_sizeof.
 */

#include "sdf/arc.h"

#include <stdint.h>

typedef struct {
    uint64_t T_n;
    uint64_t T_deadline;
    uint64_t block_n; /* submit order: the QG ordering key */
    uint64_t S_n;
    uint64_t A_n;
    uint64_t seq;
    uint32_t slot;
    uint32_t len;
    uint8_t parked;
} arc_rel_t;

typedef struct {
    /* Algorithm state (the paper's variables) */
    double D;
    double E;
    int initialized;
    uint64_t tau_last_ns;

    double rho_u, rho_l;
    double lambda_u, lambda_l;
    double U;
    double delta;
    double gamma;
    double J;
    double G;
    double T_idle_ns;
    int auto_params;
    double c_U, c_delta;
    uint64_t rtt_sm_ns;
    uint64_t min_smooth_bytes;
    uint64_t budget_ns;
    arc_mode_t mode;

    /* windows + bookkeeping */
    uint64_t delta_win[64];
    uint64_t reorder_win[64];
    size_t delta_cap;   /* == 64 (the window size; kept for parity with
                         * the wam state shape) */
    size_t reorder_cap;
    size_t delta_pos, delta_n;
    size_t reorder_pos, reorder_n;
    uint64_t prev_A;
    int have_prev_A;
    uint64_t blocks_since_param;
    uint64_t submit_seq;
    uint64_t next_block;

    /* release bookkeeping (populated by arc_heap) */
    uint64_t last_released_n;
    int have_last_release;
    uint64_t last_release_ts;
    uint64_t last_release_S_ns;
    uint64_t added_samples[256];
    size_t added_pos, added_n;
    uint64_t interval_error_max;
    struct {
        uint64_t releases;
        uint64_t idle_reanchors;
        uint64_t guard_waits;
        uint64_t guard_timeouts;
        uint64_t s_n_rejects;
        uint64_t delta_clamps;
        uint64_t budget_alerts;
    } stats;

    /* container */
    arc_apply_fn apply;
    void *ctx;
    size_t entry_max;
    uint32_t capacity;
    uint32_t batch_max;
    uint64_t batch_deadline_ns;
    arc_rel_t *heap;   /* capacity entries */
    size_t heap_n;
    uint8_t *slab;      /* capacity * entry_max */
    uint8_t *slot_used; /* capacity flags */
} arc_state_t;

struct arc_pacer {
    arc_state_t st;
};

/* arc_offset.c: the arrival path — returns the schedule for the entry
 * (T_n/T_deadline + identity); *scheduled_now: 0 = queue it, 1 = oracle
 * release, 2 = small-entry bypass (queued at T_n == A_n). */
typedef struct {
    uint64_t T_n;
    uint64_t T_deadline;
    uint64_t block_n;
    uint64_t S_n;
    uint64_t A_n;
    uint64_t seq;
    uint32_t len;
} arc_sched_t;

arc_sched_t arc_arrival(arc_state_t *a, uint64_t S_n, uint64_t A_n,
                        size_t len, int *scheduled_now);

/* arc_heap.c: the release queue. */
int arc_heap_push(arc_state_t *a, const arc_rel_t *rel);
int arc_heap_pop(arc_state_t *a, arc_rel_t *out);
void arc_note_release(arc_state_t *a, const arc_rel_t *rel,
                      uint64_t now_ns);
int arc_drain(arc_state_t *s, uint64_t now_ns);

/* percentile helper shared with tests */
double arc_res_pctl(const uint64_t *samples, size_t n, size_t cap, double p);

#endif /* SDF_ARC_INTERNAL_H */
