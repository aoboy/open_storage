#ifndef SDF_COMMON_POOL_H
#define SDF_COMMON_POOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fixed-size object pool. All storage is caller-provided so the hot path
 * never calls malloc. Intended to be partitioned per core/worker.
 */
typedef struct sdf_pool sdf_pool_t;

size_t sdf_pool_sizeof(uint32_t slot_size, uint32_t capacity);

/* mem must be at least sdf_pool_sizeof() bytes. Returns 0 on success. */
int sdf_pool_init(sdf_pool_t *p, void *mem, size_t mem_size, uint32_t slot_size, uint32_t capacity);

void *sdf_pool_alloc(sdf_pool_t *p);
void sdf_pool_free(sdf_pool_t *p, void *slot);

uint32_t sdf_pool_capacity(const sdf_pool_t *p);
uint32_t sdf_pool_in_use(const sdf_pool_t *p);

#ifdef __cplusplus
}
#endif

#endif /* SDF_COMMON_POOL_H */
