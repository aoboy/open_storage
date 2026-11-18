/*
 * Monotonic clock helpers.
 *
 * Pattern imported from wack_a_mole include/wam/util.h @ a0f0422
 * (monotonic-ns clock used by spray/ARC pacing and the controller
 * timers): one CLOCK_MONOTONIC source for data-plane timestamps,
 * nanosleep for control-loop cadence. sdf_now_ns and sdf_mono_ns are
 * deliberately the same clock: the two names exist so scheduling code
 * can state its intent (per include/sdf/common/timer.h).
 *
 * Authorized by plan prompt W0.2 and ADR-0009 (provenance + parity).
 */

#define _POSIX_C_SOURCE 200809L

#include "sdf/common/timer.h"

#include <errno.h>
#include <time.h>

uint64_t sdf_now_ns(void) {
    return sdf_mono_ns();
}

uint64_t sdf_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void sdf_sleep_ns(uint64_t ns) {
    struct timespec ts = {
        .tv_sec = (time_t)(ns / 1000000000ull),
        .tv_nsec = (long)(ns % 1000000000ull),
    };
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts) == EINTR) {
        /* resume the remaining time; EINTR rewinds ts */
    }
}
