#ifndef SDF_COMMON_SEQLOCK_H
#define SDF_COMMON_SEQLOCK_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Single-writer / multi-reader seqlock.
 *
 * Occupies the first 64 bytes of the C<->Python shared-memory segment.
 * The writer increments seq to odd before writing and to even after. A
 * reader retries when the sequence changed or is odd.
 */
typedef struct {
    _Atomic uint32_t seq;
    uint32_t pad[15];
} sdf_seqlock_t;

void sdf_seqlock_init(sdf_seqlock_t *l);

uint32_t sdf_seqlock_read_begin(const sdf_seqlock_t *l);
bool sdf_seqlock_read_retry(const sdf_seqlock_t *l, uint32_t start);

void sdf_seqlock_write_begin(sdf_seqlock_t *l);
void sdf_seqlock_write_end(sdf_seqlock_t *l);

#ifdef __cplusplus
}
#endif

#endif /* SDF_COMMON_SEQLOCK_H */
