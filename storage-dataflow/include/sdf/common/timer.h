#ifndef SDF_COMMON_TIMER_H
#define SDF_COMMON_TIMER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic nanoseconds since an unspecified epoch. */
uint64_t sdf_now_ns(void);

/* Same clock, explicit name for scheduling code. */
uint64_t sdf_mono_ns(void);

/* Sleep for at least ns nanoseconds. */
void sdf_sleep_ns(uint64_t ns);

#ifdef __cplusplus
}
#endif

#endif /* SDF_COMMON_TIMER_H */
