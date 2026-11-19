/*
 * Single-writer / multi-reader seqlock.
 *
 * Adapted from wack_a_mole: include/wam/profpub.h @ a0f0422 (plan R2 /
 * J.3 profile publication; contention-proven in wam
 * tests/test_profpub.c, TSan-clean). The publication protocol is
 * unchanged: writer marks seq odd (seq_cst) before touching payload,
 * writes payload with relaxed stores, then marks seq even (seq_cst);
 * readers re-check with a seq_cst fence between payload reads and the
 * recheck, so a successful read is exactly one complete generation.
 * Reshaped to the sdf API of include/sdf/common/seqlock.h, which splits
 * the snapshot loop into begin/retry around caller-owned payload.
 *
 * Authorized by plan prompt W0.2 and ADR-0009 (provenance + parity).
 */

#include "sdf/common/seqlock.h"

#include <stddef.h>

void sdf_seqlock_init(sdf_seqlock_t *l) {
    if (l == NULL) {
        return;
    }
    atomic_init(&l->seq, 0);
}

uint32_t sdf_seqlock_read_begin(const sdf_seqlock_t *l) {
    if (l == NULL) {
        return 0;
    }
    return atomic_load_explicit(&l->seq, memory_order_seq_cst);
}

bool sdf_seqlock_read_retry(const sdf_seqlock_t *l, uint32_t start) {
    if (l == NULL) {
        return true;
    }
    /* Payload reads happen between begin and this call: order them
     * before the recheck (wam_profpub_snapshot fence placement). */
    atomic_thread_fence(memory_order_seq_cst);
    uint32_t s = atomic_load_explicit(&l->seq, memory_order_seq_cst);
    return s != start || (s & 1u) != 0;
}

void sdf_seqlock_write_begin(sdf_seqlock_t *l) {
    if (l == NULL) {
        return;
    }
    uint32_t s = atomic_load_explicit(&l->seq, memory_order_relaxed);
    atomic_store_explicit(&l->seq, s + 1u, memory_order_seq_cst); /* odd */
    atomic_thread_fence(memory_order_seq_cst); /* payload stays behind the marker */
}

void sdf_seqlock_write_end(sdf_seqlock_t *l) {
    if (l == NULL) {
        return;
    }
    uint32_t s = atomic_load_explicit(&l->seq, memory_order_relaxed);
    /* seq_cst store also releases the relaxed payload stores. */
    atomic_store_explicit(&l->seq, s + 1u, memory_order_seq_cst); /* even */
}
