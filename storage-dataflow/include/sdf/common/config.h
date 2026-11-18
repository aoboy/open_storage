#ifndef SDF_COMMON_CONFIG_H
#define SDF_COMMON_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimal C-side configuration.
 *
 * The control plane's canonical configuration is YAML
 * (configs/<env>/wm.yaml and siblings), read by Python. The C data plane only
 * needs a small, allocation-free view of resolved knobs, so this API reads a
 * flat "key = value" file with '#' comments and [section] prefixes. Section
 * keys become "section.key".
 */
#define SDF_CONFIG_KEY_MAX 64
#define SDF_CONFIG_VALUE_MAX 192

typedef struct {
    char key[SDF_CONFIG_KEY_MAX];
    char value[SDF_CONFIG_VALUE_MAX];
} sdf_config_entry_t;

typedef struct {
    sdf_config_entry_t *entries;
    size_t count;
    size_t capacity;
} sdf_config_t;

/* Load-time allocation is permitted; the hot path only calls getters. */
int sdf_config_load(sdf_config_t *cfg, const char *path);
const char *sdf_config_get(const sdf_config_t *cfg, const char *key, const char *fallback);
int sdf_config_get_u64(const sdf_config_t *cfg, const char *key, uint64_t *out);
void sdf_config_free(sdf_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* SDF_COMMON_CONFIG_H */
