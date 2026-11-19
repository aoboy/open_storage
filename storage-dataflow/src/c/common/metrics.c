/*
 * Lock-free metric primitives (plan prompt W0.5).
 *
 * Conventions imported from wack_a_mole K.1 (include/wam/metrics.h
 * @ a0f0422): fixed-capacity, bounded-memory series that never
 * allocate, with exact count/sum so a dump can always be rendered.
 * The sdf shape (include/sdf/common/metrics.h) is atomic lock-free
 * counters plus log-linear histograms that are "safe for concurrent
 * observers": every series is monotone per update and no single
 * store tears, so an observer reading mid-stream sees a coherent
 * per-series view (series-to-series skew is inherent and accepted
 * for 4 Hz scraping).
 *
 * Histogram bucket map (documented contract of this implementation,
 * pinned by tests/c/unit/test_metrics.c):
 *   bucket 0        : values 0 and 1
 *   bucket i (1..30): [2^i, 2^(i+1))
 *   bucket 31       : [2^31, inf) — saturation bucket
 */

#include "sdf/common/metrics.h"

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>

static void counter_init_impl(sdf_counter_t *c, uint64_t v) {
    atomic_store_explicit(&c->value, v, memory_order_relaxed);
}

void sdf_counter_init(sdf_counter_t *c) {
    if (c == NULL) {
        return;
    }
    counter_init_impl(c, 0);
}

void sdf_counter_add(sdf_counter_t *c, uint64_t delta) {
    if (c == NULL) {
        return;
    }
    atomic_fetch_add_explicit(&c->value, delta, memory_order_relaxed);
}

void sdf_counter_inc(sdf_counter_t *c) {
    sdf_counter_add(c, 1);
}

uint64_t sdf_counter_read(const sdf_counter_t *c) {
    if (c == NULL) {
        return 0;
    }
    return atomic_load_explicit(&c->value, memory_order_relaxed);
}

/* Bucket index for the log-linear map above. */
static uint32_t hist_bucket(uint64_t v) {
    if (v < 2u) {
        return 0;
    }
#if defined(__GNUC__) || defined(__clang__)
    uint32_t log2v = 63u - (uint32_t)__builtin_clzll(v);
#else
    uint32_t log2v = 0;
    uint64_t x = v;
    while (x > 1) {
        x >>= 1;
        log2v++;
    }
#endif
    return log2v > 31u ? 31u : log2v;
}

void sdf_histogram_init(sdf_histogram_t *h) {
    sdf_histogram_reset(h);
}

void sdf_histogram_observe(sdf_histogram_t *h, uint64_t value) {
    if (h == NULL) {
        return;
    }
    atomic_fetch_add_explicit(&h->count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&h->sum, value, memory_order_relaxed);
    atomic_fetch_add_explicit(&h->buckets[hist_bucket(value)], 1, memory_order_relaxed);
}

void sdf_histogram_reset(sdf_histogram_t *h) {
    if (h == NULL) {
        return;
    }
    atomic_store_explicit(&h->count, 0, memory_order_relaxed);
    atomic_store_explicit(&h->sum, 0, memory_order_relaxed);
    for (size_t i = 0; i < SDF_HISTOGRAM_BUCKETS; i++) {
        atomic_store_explicit(&h->buckets[i], 0, memory_order_relaxed);
    }
}

uint64_t sdf_histogram_bucket_bound(uint32_t index) {
    if (index >= SDF_HISTOGRAM_BUCKETS) {
        return UINT64_MAX;
    }
    if (index == 0) {
        return 1;
    }
    if (index == SDF_HISTOGRAM_BUCKETS - 1) {
        return UINT64_MAX; /* the saturation bucket is le="+Inf" */
    }
    return ((uint64_t)1 << (index + 1)) - 1;
}

static void render_labels(FILE *out, const char *labels, int with_le, uint64_t bound) {
    if (labels != NULL && labels[0] != '\0') {
        if (with_le) {
            fprintf(out, "{%s,le=\"%" PRIu64 "\"}", labels, bound);
        } else {
            fprintf(out, "{%s}", labels);
        }
        return;
    }
    if (with_le) {
        fprintf(out, "{le=\"%" PRIu64 "\"}", bound);
    }
}

int sdf_histogram_render(FILE *out, const char *name, const char *labels, const sdf_histogram_t *h,
                         double scale) {
    if (out == NULL || name == NULL || name[0] == '\0' || h == NULL) {
        return -EINVAL;
    }
    if (!(scale > 0.0)) {
        scale = 1.0;
    }

    uint64_t cumulative = 0;
    for (uint32_t i = 0; i + 1 < SDF_HISTOGRAM_BUCKETS; i++) {
        cumulative += atomic_load_explicit(&h->buckets[i], memory_order_relaxed);
        uint64_t bound = sdf_histogram_bucket_bound(i);
        fprintf(out, "%s_bucket", name);
        render_labels(out, labels, 1, bound);
        fprintf(out, " %" PRIu64 "\n", cumulative);
    }

    /* Read the exact count after the finite buckets: observers only
     * increment, so le="+Inf" is never below the cumulative tail. Read
     * the sum first so a concurrent new observation cannot make the
     * exported sum exceed the exported count. */
    uint64_t sum = atomic_load_explicit(&h->sum, memory_order_relaxed);
    uint64_t count = atomic_load_explicit(&h->count, memory_order_relaxed);
    fprintf(out, "%s_bucket", name);
    if (labels != NULL && labels[0] != '\0') {
        fprintf(out, "{%s,le=\"+Inf\"}", labels);
    } else {
        fprintf(out, "{le=\"+Inf\"}");
    }
    fprintf(out, " %" PRIu64 "\n", count);

    fprintf(out, "%s_sum", name);
    render_labels(out, labels, 0, 0);
    fprintf(out, " %.17g\n", (double)sum * scale);

    fprintf(out, "%s_count", name);
    render_labels(out, labels, 0, 0);
    fprintf(out, " %" PRIu64 "\n", count);
    return 0;
}
