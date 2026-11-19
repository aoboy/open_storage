/*
 * SPSC ring — single-producer/single-consumer queue over caller storage.
 *
 * Adapted from wack_a_mole: include/wam/spsc.h @ a0f0422 (plan J.1;
 * two-thread soak proven in wam tests/test_spsc.c, TSan-clean). Reshaped
 * to the fixed-layout contract of include/sdf/common/ring.h: the slots
 * are inline at offset 192 (shared-memory ready) instead of a malloc'd
 * buffer, and the API is the sdf shape (bool push/pop, batch pop).
 * Memory-order discipline is unchanged: producer owns head (release on
 * push), consumer owns tail (release on pop), each side acquires the
 * other's index. A successful push happens-before the matching pop.
 *
 * Authorized by plan prompt W0.2 and ADR-0009 (provenance + parity).
 */

#include "sdf/common/ring.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

_Static_assert(offsetof(spsc_ring_t, slots) == 192,
               "slots must begin at offset 192 (pinned by tests/c/test_abi.c)");

static bool spsc_args_ok(uint32_t capacity, uint32_t slot_size) {
    return capacity >= 2 && (capacity & (capacity - 1u)) == 0 && slot_size != 0;
}

size_t spsc_sizeof(uint32_t capacity, uint32_t slot_size) {
    if (!spsc_args_ok(capacity, slot_size)) {
        return 0;
    }
    uint64_t need = (uint64_t)capacity * (uint64_t)slot_size + 192u;
    if (need > (uint64_t)SIZE_MAX) {
        return 0;
    }
    return (size_t)need;
}

int spsc_init(spsc_ring_t *r, void *mem, uint32_t capacity, uint32_t slot_size) {
    if (r == NULL || mem == NULL) {
        return -EINVAL;
    }
    if ((void *)r != mem) {
        return -EINVAL; /* the ring header is the start of the block */
    }
    if (((uintptr_t)mem & 63u) != 0) {
        return -EINVAL; /* 64-byte alignment is part of the contract */
    }
    if (spsc_sizeof(capacity, slot_size) == 0) {
        return -EINVAL;
    }

    /* zero header + slots: no page-fault-on-first-touch in the hot loop
     * (wam_spsc_init first-touch discipline) */
    memset(r, 0, 192u + (size_t)capacity * (size_t)slot_size);
    r->mask = capacity - 1u;
    r->slot_size = slot_size;
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    return 0;
}

bool spsc_try_push(spsc_ring_t *r, const void *slot) {
    if (r == NULL || slot == NULL || r->slot_size == 0) {
        return false;
    }

    uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (head - tail >= (uint64_t)r->mask + 1u) { /* full */
        return false;
    }

    memcpy(r->slots + (size_t)(head & (uint64_t)r->mask) * r->slot_size, slot, r->slot_size);
    atomic_store_explicit(&r->head, head + 1, memory_order_release);
    return true;
}

bool spsc_try_pop(spsc_ring_t *r, void *slot) {
    if (r == NULL || slot == NULL || r->slot_size == 0) {
        return false;
    }

    uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    if (head == tail) { /* empty */
        return false;
    }

    memcpy(slot, r->slots + (size_t)(tail & (uint64_t)r->mask) * r->slot_size, r->slot_size);
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
    return true;
}

uint32_t spsc_batch_pop(spsc_ring_t *r, void *out, uint32_t max) {
    if (r == NULL || out == NULL || r->slot_size == 0 || max == 0) {
        return 0;
    }

    uint8_t *cur = out;
    uint32_t popped = 0;
    while (popped < max && spsc_try_pop(r, cur)) {
        cur += r->slot_size;
        popped++;
    }
    return popped;
}

uint64_t spsc_count(const spsc_ring_t *r) {
    if (r == NULL || r->slot_size == 0) {
        return 0;
    }
    uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    return head - tail;
}
