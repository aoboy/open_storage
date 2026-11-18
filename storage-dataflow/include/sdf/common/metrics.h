#ifndef SDF_COMMON_METRICS_H
#define SDF_COMMON_METRICS_H

#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDF_HISTOGRAM_BUCKETS 32

typedef struct {
    _Atomic uint64_t value;
} sdf_counter_t;

typedef struct {
    _Atomic uint64_t count;
    _Atomic uint64_t sum;
    _Atomic uint64_t buckets[SDF_HISTOGRAM_BUCKETS];
} sdf_histogram_t;

void sdf_counter_init(sdf_counter_t *c);
void sdf_counter_add(sdf_counter_t *c, uint64_t delta);
void sdf_counter_inc(sdf_counter_t *c);
uint64_t sdf_counter_read(const sdf_counter_t *c);

void sdf_histogram_init(sdf_histogram_t *h);
/* Records value in a log-linear bucket. Safe for concurrent observers. */
void sdf_histogram_observe(sdf_histogram_t *h, uint64_t value);
void sdf_histogram_reset(sdf_histogram_t *h);

#ifdef __cplusplus
}
#endif

#endif /* SDF_COMMON_METRICS_H */
