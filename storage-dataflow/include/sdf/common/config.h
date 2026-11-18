#ifndef SDF_COMMON_CONFIG_H
#define SDF_COMMON_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Strict TOML configuration loader (ADR-0005).
 *
 * The control plane and the C data plane share one ops format: the
 * strict TOML subset imported from wack_a_mole's plan-K.2 loader —
 * [table], [[array of tables]], bare keys, basic strings, integers,
 * floats, booleans, homogeneous numeric arrays, '#' comments and an
 * optional ';' key separator. The schema ([wm], [fluid], [[fluid_class]],
 * [arc], [[origin]], [ab], [metrics]) is enforced at load time:
 * unknown tables/keys, type mismatches, range/enum violations and
 * missing required keys are hard "path:line:" errors reported through
 * sdf/common/log.h — a typo never passes silently.
 *
 * Entries are stored flat: single tables become "table.key"; array-of-
 * table entries become "table[i].key". An absent key is simply absent,
 * so callers implement precedence CLI > file > built-in default by
 * reading through the getters with their own defaults as fallback
 * (the ADR-0005 presence-merge rule).
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
