#ifndef SDF_COMMON_LOG_H
#define SDF_COMMON_LOG_H

#include "sdf/common/compat.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SDF_LOG_DEBUG = 0,
    SDF_LOG_INFO = 1,
    SDF_LOG_WARN = 2,
    SDF_LOG_ERROR = 3,
} sdf_log_level_t;

void sdf_log_set_level(sdf_log_level_t level);
sdf_log_level_t sdf_log_get_level(void);

void sdf_log_write(sdf_log_level_t level, const char *file, int line, const char *fmt, ...)
    SDF_PRINTF_FORMAT(4, 5);

#define SDF_LOG(level, ...) sdf_log_write((level), __FILE__, __LINE__, __VA_ARGS__)
#define SDF_LOGD(...) SDF_LOG(SDF_LOG_DEBUG, __VA_ARGS__)
#define SDF_LOGI(...) SDF_LOG(SDF_LOG_INFO, __VA_ARGS__)
#define SDF_LOGW(...) SDF_LOG(SDF_LOG_WARN, __VA_ARGS__)
#define SDF_LOGE(...) SDF_LOG(SDF_LOG_ERROR, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* SDF_COMMON_LOG_H */
