#ifndef SDF_ORIGIN_HTTP_H
#define SDF_ORIGIN_HTTP_H

/*
 * HTTP origin driver (plan prompt W1.3, ADR-0008 / design 09 §4).
 *
 * Reference real-network driver: POSIX-socket HTTP/1.1 Range GET with
 * a persistent connection per origin and reconnect-on-error. Each
 * origin reuses one grow-on-demand response-body buffer, so the
 * steady-state fetch/completion path allocates nothing after warmup.
 * No harness knobs — degradation comes from the real network or the
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

/*
 * Reusable transport-buffer diagnostics (tests, bench_origin).
 *
 * The worker keeps one response-body buffer per origin and only grows
 * it. `body_allocs` counts allocation events since open; once it
 * stops increasing, fetches at or below `body_cap` perform no body
 * allocation. `connects` counts successful socket establishes, so a
 * steady run with one persistent connection reports one connect.
 * Snapshot semantics: safe to call at any time; values are per-origin
 * worker state, not dispatch telemetry.
 */
typedef struct {
    uint32_t body_cap;    /* bytes reserved for a response body */
    uint32_t body_allocs; /* buffer allocation events since open */
    uint64_t connects;    /* successful socket connects since open */
} sdf_http_transport_stats_t;

/** Snapshot one HTTP origin's transport-buffer state. Returns 0 or -EINVAL. */
int sdf_http_transport_stats(const sdf_origin_t *o, uint32_t origin_id,
                             sdf_http_transport_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SDF_ORIGIN_HTTP_H */
