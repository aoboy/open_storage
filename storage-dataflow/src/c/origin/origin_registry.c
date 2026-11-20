/*
 * Origin driver registry (plan prompt W6.15, ADR-0008).
 *
 * One configured origin set maps to one driver instance. The registry keeps
 * class selection explicit: loopback and HTTP are implemented, S3/peer stay
 * reserved for W6.7, and a mixed set is never silently rewritten.
 */

#include "sdf/origin.h"

#include "sdf/origin/http.h"
#include "sdf/origin/loopback.h"

#include <errno.h>

int sdf_origin_select(const sdf_origin_cfg_t *cfgs, uint32_t n, const sdf_origin_ops_t **out_ops,
                      sdf_origin_select_status_t *out_status) {
    if (out_status != NULL) {
        *out_status = SDF_ORIGIN_SELECT_INVALID;
    }
    if (cfgs == NULL || out_ops == NULL || n == 0 || n > SDF_ORIGIN_MAX) {
        return -EINVAL;
    }
    *out_ops = NULL;

    /* Validate the whole set before classifying it. This keeps diagnostics
     * deterministic even when more than one entry is bad. */
    for (uint32_t i = 0; i < n; i++) {
        if (cfgs[i].id != i || cfgs[i].dst[0] == '\0') {
            return -EINVAL;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        if (cfgs[i].cls != SDF_ORIGIN_LOOPBACK && cfgs[i].cls != SDF_ORIGIN_HTTP &&
            cfgs[i].cls != SDF_ORIGIN_S3 && cfgs[i].cls != SDF_ORIGIN_PEER) {
            if (out_status != NULL) {
                *out_status = SDF_ORIGIN_SELECT_UNKNOWN;
            }
            return -EINVAL;
        }
    }

    uint32_t cls = cfgs[0].cls;
    for (uint32_t i = 1; i < n; i++) {
        if (cfgs[i].cls != cls) {
            if (out_status != NULL) {
                *out_status = SDF_ORIGIN_SELECT_MIXED;
            }
            return -EINVAL;
        }
    }
    if (cls == SDF_ORIGIN_S3 || cls == SDF_ORIGIN_PEER) {
        if (out_status != NULL) {
            *out_status = SDF_ORIGIN_SELECT_UNSUPPORTED;
        }
        return -ENOTSUP;
    }

    *out_ops = cls == SDF_ORIGIN_LOOPBACK ? sdf_loopback_ops() : sdf_http_ops();
    if (out_status != NULL) {
        *out_status = SDF_ORIGIN_SELECT_OK;
    }
    return 0;
}

const char *sdf_origin_select_status_name(sdf_origin_select_status_t status) {
    switch (status) {
    case SDF_ORIGIN_SELECT_OK:
        return "ok";
    case SDF_ORIGIN_SELECT_INVALID:
        return "invalid origin configuration";
    case SDF_ORIGIN_SELECT_MIXED:
        return "mixed origin classes are not supported";
    case SDF_ORIGIN_SELECT_UNSUPPORTED:
        return "origin class is reserved but not implemented";
    case SDF_ORIGIN_SELECT_UNKNOWN:
        return "unknown origin class";
    default:
        return "invalid origin selection status";
    }
}

int sdf_origin_set_harness_throttle_ns(sdf_origin_t *o, uint32_t origin_id, uint64_t throttle_ns) {
    if (o == NULL || o->ops != sdf_loopback_ops()) {
        return -ENOTSUP;
    }
    return sdf_loopback_set_knob(o, origin_id, SDF_LB_THROTTLE_NS, throttle_ns);
}
