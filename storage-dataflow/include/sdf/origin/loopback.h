#ifndef SDF_ORIGIN_LOOPBACK_H
#define SDF_ORIGIN_LOOPBACK_H

/*
 * Loopback origin driver — harness control surface (plan prompt W1.2,
 * ADR-0008 / design 09 §4).
 *
 * This header is for the TEST HARNESS and loadgen only: data-plane
 * consumers (proxy, repair coordinator) talk to origins exclusively
 * through sdf/origin.h. The knobs below are what turns a plain file
 * into a degraded origin — the storage-domain analogue of tc netem
 * (import map: the netns harness pattern, recast for origins).
 *
 * Loss/err values are percent times 100 (x100): 5% = 500, resolution
 * 0.01%. Knobs take effect on units scheduled after the store;
 * in-flight units complete under their scheduled behavior.
 */

#include "sdf/origin.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SDF_LB_THROTTLE_NS = 0,   /* per-request artificial delay */
    SDF_LB_JITTER_NS = 1,     /* uniform [0, value) added to throttle */
    SDF_LB_LOSS_PCT_X100 = 2, /* completed with -EHOSTUNREACH */
    SDF_LB_ERR_PCT_X100 = 3   /* completed with -EIO */
} sdf_loopback_knob_t;

/*
 * Live-reload a degradation knob. o must be a loopback driver.
 * Returns 0, -EINVAL on bad arguments, -ERANGE on out-of-band values.
 */
int sdf_loopback_set_knob(sdf_origin_t *o, uint32_t origin_id, sdf_loopback_knob_t knob,
                          uint64_t value);

/** The loopback driver's ops table (factories/tests install it). */
const sdf_origin_ops_t *sdf_loopback_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* SDF_ORIGIN_LOOPBACK_H */
