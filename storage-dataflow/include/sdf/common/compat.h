#ifndef SDF_COMMON_COMPAT_H
#define SDF_COMMON_COMPAT_H

#include <stddef.h>

#ifndef SDF_CACHELINE
#define SDF_CACHELINE 64u
#endif

#define SDF_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#if defined(__GNUC__) || defined(__clang__)
#define SDF_LIKELY(x) __builtin_expect(!!(x), 1)
#define SDF_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define SDF_PRINTF_FORMAT(fmt_index, arg_index)                                                    \
    __attribute__((format(printf, fmt_index, arg_index)))
#define SDF_UNUSED __attribute__((unused))
#else
#define SDF_LIKELY(x) (x)
#define SDF_UNLIKELY(x) (x)
#define SDF_PRINTF_FORMAT(fmt_index, arg_index)
#define SDF_UNUSED
#endif

#define SDF_MIN(a, b) ((a) < (b) ? (a) : (b))
#define SDF_MAX(a, b) ((a) > (b) ? (a) : (b))

#endif /* SDF_COMMON_COMPAT_H */
