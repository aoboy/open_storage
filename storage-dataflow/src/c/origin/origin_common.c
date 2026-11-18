/*
 * Origin seam utilities (plan W1.1 follow-up, ADR-0008).
 *
 * Declared in include/sdf/origin.h; implemented here so the seam's
 * diagnostics resolve for every consumer (found live by the CFFI
 * full-link build, which is exactly what the bindings leg is for).
 */

#include "sdf/origin.h"

const char *sdf_origin_cls_name(uint32_t cls) {
    switch (cls) {
    case SDF_ORIGIN_LOOPBACK:
        return "loopback";
    case SDF_ORIGIN_HTTP:
        return "http";
    case SDF_ORIGIN_S3:
        return "s3";
    case SDF_ORIGIN_PEER:
        return "peer";
    default:
        return "?";
    }
}
