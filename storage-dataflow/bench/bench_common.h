#ifndef SDF_BENCH_COMMON_H
#define SDF_BENCH_COMMON_H

/*
 * Shared micro-bench harness (plan prompt W0.5), mirroring the
 * wack_a_mole bench conventions (bench/bench_common.h @ a0f0422):
 * a volatile sink the optimizer cannot elide, best-of-rounds timing
 * over ns and cycle counters, and one "BENCH <name> ..." line per
 * configuration so scripts/run_bench.sh can record verbatim.
 *
 * Benches are RECORDED, never ctest-gated (plan §10); thresholds
 * activate with their owning waves (wm select gate: W2.6; arc gate:
 * W4.2).
 */

#include "sdf/common/timer.h"

#include <stdint.h>
#include <stdio.h>

static volatile uint64_t sdf_bench_sink;

/* TSC read on x86; 0 elsewhere (the ns figure stays authoritative). */
static inline uint64_t sdf_bench_cycles(void) {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    return 0;
#endif
}

/* Monotonic ns: the sdf timer source, same as the data plane uses. */
#define sdf_bench_now_ns() sdf_now_ns()

#endif /* SDF_BENCH_COMMON_H */
