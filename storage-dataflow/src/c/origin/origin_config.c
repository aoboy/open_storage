/*
 * Strict-TOML origin configuration loader (plan prompt W6.15).
 *
 * The common config loader stores array-of-table entries under flat keys such
 * as origin[0].cls. This adapter materializes the dense origin array without
 * changing the config schema or silently replacing a configured class.
 */

#include "sdf/origin/config.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int origin_key_index(const char *key, uint32_t *out_index) {
    if (key == NULL || strncmp(key, "origin[", 7) != 0) {
        return 0;
    }
    const char *p = key + 7;
    if (*p < '0' || *p > '9') {
        return -EINVAL;
    }
    uint32_t index = 0;
    do {
        uint32_t digit = (uint32_t)(*p - '0');
        if (index > (UINT32_MAX - digit) / 10u) {
            return -EINVAL;
        }
        index = index * 10u + digit;
        p++;
    } while (*p >= '0' && *p <= '9');
    if (p[0] != ']' || p[1] != '.' || p[2] == '\0' || index >= SDF_ORIGIN_MAX) {
        return -EINVAL;
    }
    *out_index = index;
    return 1;
}

/* 1 = present, 0 = absent, -EINVAL = malformed. */
static int optional_u64(const sdf_config_t *cfg, const char *key, uint64_t *out) {
    if (sdf_config_get(cfg, key, NULL) == NULL) {
        *out = 0;
        return 0;
    }
    if (sdf_config_get_u64(cfg, key, out) != 0) {
        return -EINVAL;
    }
    return 1;
}

static int optional_u32(const sdf_config_t *cfg, const char *key, uint32_t *out) {
    uint64_t value = 0;
    int present = optional_u64(cfg, key, &value);
    if (present < 0 || value > UINT32_MAX) {
        return -EINVAL;
    }
    if (present > 0) {
        *out = (uint32_t)value;
    }
    return present;
}

static int optional_pct(const sdf_config_t *cfg, const char *key, double *out) {
    const char *value = sdf_config_get(cfg, key, NULL);
    if (value == NULL) {
        *out = 0.0;
        return 0;
    }
    char *end = NULL;
    errno = 0;
    double parsed = strtod(value, &end);
    if (errno != 0 || end == value || *end != '\0' || !isfinite(parsed) || parsed < 0.0 ||
        parsed > 100.0) {
        return -EINVAL;
    }
    *out = parsed;
    return 1;
}

int sdf_origin_config_load(const sdf_config_t *cfg, sdf_origin_cfg_t *origins, uint32_t capacity,
                           uint32_t *n_origins) {
    if (cfg == NULL || origins == NULL || n_origins == NULL || capacity == 0) {
        return -EINVAL;
    }
    *n_origins = 0;

    uint32_t max_index = 0;
    int found = 0;
    for (size_t i = 0; i < cfg->count; i++) {
        uint32_t index = 0;
        int key_rc = origin_key_index(cfg->entries[i].key, &index);
        if (key_rc < 0) {
            return -EINVAL;
        }
        if (key_rc == 0) {
            continue;
        }
        found = 1;
        if (index > max_index) {
            max_index = index;
        }
    }
    if (!found || max_index >= capacity) {
        return -EINVAL;
    }

    uint32_t count = max_index + 1;
    for (uint32_t i = 0; i < count; i++) {
        char key[SDF_CONFIG_KEY_MAX];
        sdf_origin_cfg_t *origin = &origins[i];
        memset(origin, 0, sizeof(*origin));

        snprintf(key, sizeof(key), "origin[%u].cls", i);
        const char *class_name = sdf_config_get(cfg, key, NULL);
        if (class_name == NULL || sdf_origin_cls_parse(class_name, &origin->cls) != 0) {
            return -EINVAL;
        }

        snprintf(key, sizeof(key), "origin[%u].dst", i);
        const char *dst = sdf_config_get(cfg, key, NULL);
        if (dst == NULL || dst[0] == '\0' || strlen(dst) >= sizeof(origin->dst)) {
            return -EINVAL;
        }
        memcpy(origin->dst, dst, strlen(dst) + 1);

        snprintf(key, sizeof(key), "origin[%u].id", i);
        uint64_t id = 0;
        int has_id = optional_u64(cfg, key, &id);
        if (has_id < 0 || (has_id > 0 && id != i)) {
            return -EINVAL;
        }
        origin->id = i;

        snprintf(key, sizeof(key), "origin[%u].throttle_ns", i);
        if (optional_u64(cfg, key, &origin->throttle_ns) < 0) {
            return -EINVAL;
        }
        snprintf(key, sizeof(key), "origin[%u].loss_pct", i);
        if (optional_pct(cfg, key, &origin->loss_pct) < 0) {
            return -EINVAL;
        }
        snprintf(key, sizeof(key), "origin[%u].err_pct", i);
        if (optional_pct(cfg, key, &origin->err_pct) < 0) {
            return -EINVAL;
        }
        snprintf(key, sizeof(key), "origin[%u].lat_jitter_ns", i);
        if (optional_u64(cfg, key, &origin->lat_jitter_ns) < 0) {
            return -EINVAL;
        }
        origin->max_inflight = 64;
        snprintf(key, sizeof(key), "origin[%u].max_inflight", i);
        if (optional_u32(cfg, key, &origin->max_inflight) < 0) {
            return -EINVAL;
        }
        if (origin->max_inflight == 0) {
            origin->max_inflight = 64;
        }
        snprintf(key, sizeof(key), "origin[%u].timeout_ms", i);
        if (optional_u32(cfg, key, &origin->timeout_ms) < 0) {
            return -EINVAL;
        }
    }

    *n_origins = count;
    return 0;
}
