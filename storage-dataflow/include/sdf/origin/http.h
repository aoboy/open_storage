#ifndef SDF_ORIGIN_HTTP_H
#define SDF_ORIGIN_HTTP_H

/*
 * HTTP origin driver (plan prompt W1.3, ADR-0008 / design 09 §4).
 *
 * Reference real-network driver: POSIX-socket HTTP/1.1 Range GET with
 * a persistent connection per origin and reconnect-on-error. No
 * harness knobs — degradation comes from the real network or the
 * in-process test server. TLS and SDF_HAVE_CURL remain the documented
 * future options (design 09 §9); this driver is the loopback-socket
 * smoke leg, not the hot-path gate (risk S8).
 */

#include "sdf/origin.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The HTTP driver's ops table (tests/apps install it). */
const sdf_origin_ops_t *sdf_http_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* SDF_ORIGIN_HTTP_H */
