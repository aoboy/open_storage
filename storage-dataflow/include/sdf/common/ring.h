#ifndef SDF_COMMON_RING_H
#define SDF_COMMON_RING_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Single-producer / single-consumer ring.
 *
 * head and tail are cacheline isolated. capacity must be a power of two and
 * at least 2. Slots are copied by value; the ring never allocates.
 *
 * Header layout is fixed for shared-memory use. slots[] begins at
 * offset 192.
 */
typedef struct {
    _Atomic uint64_t head;
    uint64_t pad_head[7];

    _Atomic uint64_t tail;
    uint64_t pad_tail[7];

    uint32_t mask;
    uint32_t slot_size;
    uint32_t pad_tail_meta[14];

    uint8_t slots[];
} spsc_ring_t;

/* Bytes required to hold a ring with the given capacity and slot size. */
size_t spsc_sizeof(uint32_t capacity, uint32_t slot_size);

/*
 * Place a ring at mem, which must be at least spsc_sizeof(capacity,
 * slot_size) bytes and aligned to 64 bytes. Returns 0 on success.
 */
int spsc_init(spsc_ring_t *r, void *mem, uint32_t capacity, uint32_t slot_size);

bool spsc_try_push(spsc_ring_t *r, const void *slot);
bool spsc_try_pop(spsc_ring_t *r, void *slot);

/* Pop up to max slots. Returns the number popped. */
uint32_t spsc_batch_pop(spsc_ring_t *r, void *out, uint32_t max);

/* Approximate occupancy; safe to call from either side. */
uint64_t spsc_count(const spsc_ring_t *r);

#ifdef __cplusplus
}
#endif

#endif /* SDF_COMMON_RING_H */
