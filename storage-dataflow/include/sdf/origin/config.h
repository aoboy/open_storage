#ifndef SDF_ORIGIN_CONFIG_H
#define SDF_ORIGIN_CONFIG_H

#include "sdf/common/config.h"
#include "sdf/origin.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Load the flat [[origin]] entries from a strict TOML configuration.
 *
 * The helper preserves each entry's class and destination. Optional knobs
 * use zero defaults, except max_inflight which defaults to 64. Entries must
 * have dense indexes and ids. Returns 0 on success or -EINVAL for an absent,
 * malformed, sparse or over-capacity origin set.
 */
int sdf_origin_config_load(const sdf_config_t *cfg, sdf_origin_cfg_t *origins, uint32_t capacity,
                           uint32_t *n_origins);

#ifdef __cplusplus
}
#endif

#endif /* SDF_ORIGIN_CONFIG_H */
