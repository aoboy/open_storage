/*
 * Fixed-size object pool over caller storage.
 *
 * Pattern imported from wack_a_mole plan J.2 (M.1 scratch pool, TX
 * symbuf, ARC slab @ a0f0422): the hot path never allocates — storage
 * is carved once at init and ownership moves through a free list. This
 * sdf realization (include/sdf/common/pool.h) keeps the free list as an
 * index stack in the pool header so slots stay pure caller data with no
 * minimum slot size. Slot stride is rounded up to 8 bytes and the slots
 * region starts cacheline-aligned, so every slot is at least 8-aligned.
 *
 * Authorized by plan prompt W0.2 and ADR-0009 (provenance + parity).
 */

#include "sdf/common/pool.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SDF_POOL_HDR_BYTES 64u /* header: one cacheline */

struct sdf_pool {
    uint32_t slot_size; /* as requested */
    uint32_t stride;    /* slot_size rounded up to 8 */
    uint32_t capacity;
    uint32_t free_top; /* entries used on the free stack */
    uint8_t _pad[SDF_POOL_HDR_BYTES - 4u * sizeof(uint32_t)];
    /* uint32_t free_stack[capacity] follows; slots follow that,
     * cacheline-aligned. */
};

static uint32_t pool_stride(uint32_t slot_size) {
    return (slot_size + 7u) & ~7u;
}

/* Offset of the free stack (right after the 64-byte header). */
static size_t pool_stack_off(void) {
    return SDF_POOL_HDR_BYTES;
}

/* Offset of the slots region: free stack rounded up to a cacheline so
 * every slot starts 64-aligned. */
static size_t pool_slots_off(uint32_t capacity) {
    return (SDF_POOL_HDR_BYTES + (size_t)capacity * sizeof(uint32_t) + 63u) & ~(size_t)63u;
}

static uint32_t *pool_stack(sdf_pool_t *p) {
    return (uint32_t *)((uint8_t *)p + pool_stack_off());
}

size_t sdf_pool_sizeof(uint32_t slot_size, uint32_t capacity) {
    if (slot_size == 0 || capacity == 0)
        return 0;
    uint64_t need = pool_slots_off(capacity) + (uint64_t)pool_stride(slot_size) * capacity;
    if (need > (uint64_t)SIZE_MAX)
        return 0;
    return (size_t)need;
}

int sdf_pool_init(sdf_pool_t *p, void *mem, size_t mem_size, uint32_t slot_size,
                  uint32_t capacity) {
    if (p == NULL || mem == NULL)
        return -EINVAL;
    if (((uintptr_t)mem & 63u) != 0)
        return -EINVAL; /* slots must land cacheline-aligned */
    size_t need = sdf_pool_sizeof(slot_size, capacity);
    if (need == 0 || mem_size < need)
        return -EINVAL;

    /* first-touch: deterministic slot contents, no lazy faults later */
    memset(p, 0, need);
    p->slot_size = slot_size;
    p->stride = pool_stride(slot_size);
    p->capacity = capacity;
    p->free_top = capacity;
    for (uint32_t i = 0; i < capacity; i++)
        pool_stack(p)[i] = i;
    return 0;
}

void *sdf_pool_alloc(sdf_pool_t *p) {
    if (p == NULL || p->free_top == 0)
        return NULL;
    uint32_t idx = pool_stack(p)[--p->free_top];
    uint8_t *slots = (uint8_t *)p + pool_slots_off(p->capacity);
    return slots + (size_t)idx * p->stride;
}

void sdf_pool_free(sdf_pool_t *p, void *slot) {
    if (p == NULL || slot == NULL)
        return;
    uint8_t *slots = (uint8_t *)p + pool_slots_off(p->capacity);
    uint8_t *end = slots + (size_t)p->capacity * p->stride;
    uint8_t *s = slot;
    if (s < slots || s >= end || p->free_top >= p->capacity)
        return; /* not a live slot of this pool */

    size_t off = (size_t)(s - slots);
    if (off % p->stride != 0)
        return;
    pool_stack(p)[p->free_top++] = (uint32_t)(off / p->stride);
    /* NOTE: double free is UB — it duplicates a stack entry and will
     * hand the same slot to two owners. */
}

uint32_t sdf_pool_capacity(const sdf_pool_t *p) {
    return p == NULL ? 0 : p->capacity;
}

uint32_t sdf_pool_in_use(const sdf_pool_t *p) {
    if (p == NULL)
        return 0;
    return p->capacity - p->free_top;
}
