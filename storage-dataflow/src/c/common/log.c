/*
 * Minimal stderr logger.
 *
 * Pattern imported from wack_a_mole logging conventions @ a0f0422:
 * level-gated, one line per event, basename:line provenance, no
 * allocation and no locking on the hot path (fprintf serializes
 * internally; a data-plane component logs rate-limited events only).
 *
 * Authorized by plan prompt W0.2 and ADR-0009 (provenance + parity).
 */

#include "sdf/common/log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static _Atomic int g_level = SDF_LOG_INFO;

void sdf_log_set_level(sdf_log_level_t level) {
    atomic_store_explicit(&g_level, (int)level, memory_order_relaxed);
}

sdf_log_level_t sdf_log_get_level(void) {
    return (sdf_log_level_t)atomic_load_explicit(&g_level, memory_order_relaxed);
}

static const char *level_name(sdf_log_level_t level) {
    switch (level) {
    case SDF_LOG_DEBUG:
        return "DEBUG";
    case SDF_LOG_INFO:
        return "INFO";
    case SDF_LOG_WARN:
        return "WARN";
    case SDF_LOG_ERROR:
        return "ERROR";
    default:
        return "?";
    }
}

void sdf_log_write(sdf_log_level_t level, const char *file, int line, const char *fmt, ...) {
    if (level < sdf_log_get_level())
        return;

    const char *base = file;
    if (file != NULL) {
        const char *slash = strrchr(file, '/');
        if (slash != NULL)
            base = slash + 1;
    }

    fprintf(stderr, "[sdf %s] %s:%d: ", level_name(level), base != NULL ? base : "?", line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
