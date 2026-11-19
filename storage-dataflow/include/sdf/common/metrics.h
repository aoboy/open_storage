#ifndef SDF_COMMON_METRICS_H
#define SDF_COMMON_METRICS_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

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

/*
 * W6.14 histogram text contract. Bucket i covers the inclusive integer
 * range documented in src/c/common/metrics.c; the finite Prometheus
 * buckets are 0..SDF_HISTOGRAM_BUCKETS-2 (le="1" then le="2^(i+1)-1"),
 * and bucket 31 is the saturation bucket carried by le="+Inf".
 * sdf_histogram_bucket_bound(31) returns UINT64_MAX (the +Inf marker).
 */
uint64_t sdf_histogram_bucket_bound(uint32_t index);

/*
 * Render one Prometheus cumulative histogram family instance:
 *   name_bucket{labels,le="..."} cumulative
 *   name_bucket{labels,le="+Inf"} count
 *   name_sum{labels} sum * scale
 *   name_count{labels} count
 *
 * labels is an optional Prometheus label list without braces (for
 * example "arm=\"treatment\""). scale converts the stored integer sum
 * to the exported unit (1.0 keeps native units, 1e-9 exports seconds).
 * Returns 0, or -EINVAL on bad arguments. The caller owns the # TYPE
 * line so multi-label families can emit it exactly once.
 */
int sdf_histogram_render(FILE *out, const char *name, const char *labels, const sdf_histogram_t *h,
                         double scale);

#ifdef __cplusplus
}
#endif

#endif /* SDF_COMMON_METRICS_H */
