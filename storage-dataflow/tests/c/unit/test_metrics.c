/*
 * Metric primitive tests (plan prompt W0.5).
 *
 * Pins the documented bucket map of the log-linear histogram, counter
 * semantics, reset behavior, and concurrency safety: 4 threads
 * hammering observe() must yield exact count/sum/bucket totals —
 * the "safe for concurrent observers" contract of
 * include/sdf/common/metrics.h. Conventions imported per WAM K.1:
 * bounded memory, exact count/sum.
 */

#include "sdf/common/metrics.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void test_counter(void) {
    sdf_counter_t c;
    sdf_counter_init(&c);
    CHECK(sdf_counter_read(&c) == 0);
    sdf_counter_inc(&c);
    sdf_counter_inc(&c);
    sdf_counter_inc(&c);
    sdf_counter_add(&c, 5);
    CHECK(sdf_counter_read(&c) == 8);
    sdf_counter_init(&c);
    CHECK(sdf_counter_read(&c) == 0);

    /* NULL-safety: reads 0, writes no-op */
    CHECK(sdf_counter_read(NULL) == 0);
    sdf_counter_inc(NULL);
    sdf_counter_add(NULL, 5);
}

static uint64_t rendered_value(const char *body, const char *line_prefix) {
    const char *p = strstr(body, line_prefix);
    CHECK(p != NULL);
    p += strlen(line_prefix);
    return strtoull(p, NULL, 10);
}

static void test_histogram_render(void) {
    sdf_histogram_t h;
    sdf_histogram_init(&h);
    sdf_histogram_observe(&h, 0);
    sdf_histogram_observe(&h, 1);
    sdf_histogram_observe(&h, 2);
    sdf_histogram_observe(&h, 3);
    sdf_histogram_observe(&h, 4);
    sdf_histogram_observe(&h, 1024);
    sdf_histogram_observe(&h, (1ull << 31) + 5);
    sdf_histogram_observe(&h, 1ull << 40);

    char *body = NULL;
    size_t len = 0;
    FILE *out = open_memstream(&body, &len);
    CHECK(out != NULL);
    CHECK(sdf_histogram_render(out, "sdf_test_latency_ns", "arm=\"treatment\"", &h, 1.0) == 0);
    CHECK(fclose(out) == 0);
    CHECK(body != NULL);

    CHECK(rendered_value(body, "sdf_test_latency_ns_bucket{arm=\"treatment\",le=\"1\"} ") == 2);
    CHECK(rendered_value(body, "sdf_test_latency_ns_bucket{arm=\"treatment\",le=\"3\"} ") == 4);
    CHECK(rendered_value(body, "sdf_test_latency_ns_bucket{arm=\"treatment\",le=\"7\"} ") == 5);
    CHECK(rendered_value(body, "sdf_test_latency_ns_bucket{arm=\"treatment\",le=\"+Inf\"} ") == 8);
    CHECK(rendered_value(body, "sdf_test_latency_ns_count{arm=\"treatment\"} ") == 8);
    CHECK(strstr(body, "sdf_test_latency_ns_sum{arm=\"treatment\"} ") != NULL);

    /* cumulative finite buckets are monotone; +Inf is the exact count */
    uint64_t prev = 0;
    const char *line = body;
    while ((line = strstr(line, "sdf_test_latency_ns_bucket{")) != NULL) {
        const char *v = strchr(line, '}');
        CHECK(v != NULL);
        uint64_t cur = strtoull(v + 2, NULL, 10);
        CHECK(cur >= prev);
        prev = cur;
        line = v;
    }
    CHECK(prev == 8);

    /* scale converts the stored integer sum (seconds export) */
    char *scaled = NULL;
    size_t scaled_len = 0;
    FILE *sout = open_memstream(&scaled, &scaled_len);
    CHECK(sout != NULL);
    CHECK(sdf_histogram_render(sout, "sdf_test_seconds", NULL, &h, 1e-9) == 0);
    CHECK(fclose(sout) == 0);
    CHECK(scaled != NULL);
    CHECK(strstr(scaled, "sdf_test_seconds_bucket{le=\"+Inf\"} 8\n") != NULL);
    CHECK(strstr(scaled, "sdf_test_seconds_count 8\n") != NULL);

    free(body);
    free(scaled);
    CHECK(sdf_histogram_bucket_bound(0) == 1);
    CHECK(sdf_histogram_bucket_bound(1) == 3);
    CHECK(sdf_histogram_bucket_bound(30) == ((1ull << 31) - 1));
    CHECK(sdf_histogram_bucket_bound(31) == UINT64_MAX);
    CHECK(sdf_histogram_bucket_bound(99) == UINT64_MAX);

    CHECK(sdf_histogram_render(NULL, "x", NULL, &h, 1.0) == -EINVAL);
    CHECK(sdf_histogram_render(stdout, NULL, NULL, &h, 1.0) == -EINVAL);
    CHECK(sdf_histogram_render(stdout, "x", NULL, NULL, 1.0) == -EINVAL);
}

static void test_histogram_buckets(void) {
    sdf_histogram_t h;
    sdf_histogram_init(&h);

    /* documented bucket map: 0={0,1}, i in 1..30 = [2^i, 2^(i+1)),
     * 31 = [2^31, inf) */
    sdf_histogram_observe(&h, 0);
    sdf_histogram_observe(&h, 1);
    CHECK(atomic_load(&h.buckets[0]) == 2);

    sdf_histogram_observe(&h, 2);
    sdf_histogram_observe(&h, 3);
    CHECK(atomic_load(&h.buckets[1]) == 2);

    sdf_histogram_observe(&h, 4);
    CHECK(atomic_load(&h.buckets[2]) == 1);

    sdf_histogram_observe(&h, 1024);
    CHECK(atomic_load(&h.buckets[10]) == 1);

    sdf_histogram_observe(&h, (1ull << 31) + 5);
    CHECK(atomic_load(&h.buckets[31]) == 1);

    sdf_histogram_observe(&h, 1ull << 40); /* saturation stays in 31 */
    CHECK(atomic_load(&h.buckets[31]) == 2);

    CHECK(atomic_load(&h.count) == 8);
    uint64_t expect_sum = 0 + 1 + 2 + 3 + 4 + 1024 + ((1ull << 31) + 5) + (1ull << 40);
    CHECK(atomic_load(&h.sum) == expect_sum);

    /* total across buckets equals count */
    uint64_t total = 0;
    for (int i = 0; i < SDF_HISTOGRAM_BUCKETS; i++) {
        total += atomic_load(&h.buckets[i]);
    }
    CHECK(total == 8);

    sdf_histogram_reset(&h);
    CHECK(atomic_load(&h.count) == 0);
    CHECK(atomic_load(&h.sum) == 0);
    for (int i = 0; i < SDF_HISTOGRAM_BUCKETS; i++) {
        CHECK(atomic_load(&h.buckets[i]) == 0);
    }

    sdf_histogram_observe(NULL, 5); /* no-op, no crash */
}

/* ---- concurrency legs ------------------------------------------------ */

enum { OBS_THREADS = 4, OBS_PER_THREAD = 100000, OBS_VALUE = 7 };

static sdf_histogram_t g_hist;
static sdf_counter_t g_ctr;

static void *obs_main(void *arg) {
    (void)arg;
    for (int i = 0; i < OBS_PER_THREAD; i++) {
        sdf_histogram_observe(&g_hist, OBS_VALUE);
        sdf_counter_add(&g_ctr, 1);
    }
    return NULL;
}

static void test_concurrent_observe(void) {
    sdf_histogram_init(&g_hist);
    sdf_counter_init(&g_ctr);

    pthread_t tids[OBS_THREADS];
    for (int i = 0; i < OBS_THREADS; i++) {
        CHECK(pthread_create(&tids[i], NULL, obs_main, NULL) == 0);
    }
    for (int i = 0; i < OBS_THREADS; i++) {
        CHECK(pthread_join(tids[i], NULL) == 0);
    }

    /* every update is counted exactly once, no lost adds */
    CHECK(atomic_load(&g_hist.count) == (uint64_t)OBS_THREADS * OBS_PER_THREAD);
    CHECK(atomic_load(&g_hist.sum) == (uint64_t)OBS_THREADS * OBS_PER_THREAD * OBS_VALUE);
    CHECK(atomic_load(&g_hist.buckets[2]) == /* 7 in [4,8) */
          (uint64_t)OBS_THREADS * OBS_PER_THREAD);
    CHECK(sdf_counter_read(&g_ctr) == (uint64_t)OBS_THREADS * OBS_PER_THREAD);
}

int main(void) {
    test_counter();
    test_histogram_render();
    test_histogram_buckets();
    test_concurrent_observe();
    printf("test_metrics: ok (bucket map pinned; cumulative render; "
           "4-thread exact totals)\n");
    return 0;
}
