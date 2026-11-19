#ifndef SDF_COMMON_METRICS_ENDPOINT_H
#define SDF_COMMON_METRICS_ENDPOINT_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Live scrape hook (plan prompt W6.1; the K.1b pattern imported from
 * wack_a_mole include/wam/metrics_endpoint.h + src/runtime/
 * metrics_endpoint.c @ a0f0422): serve the Prometheus text set over a
 * minimal HTTP endpoint while the process runs. The exit-time dump
 * stays as it is — this is the same series, live.
 *
 * One accept thread, one request at a time: GET answers with a
 * Content-Length-framed 200 body rendered by a caller callback; other
 * methods answer 405; a failing render answers 500. Unauthenticated
 * ops surface — bind loopback unless you mean otherwise.
 */

/* opaque handle: bound socket + accept thread + stop flag */
typedef struct sdf_metrics_endpoint sdf_metrics_endpoint_t;

/*
 * Render one scrape body into out (Prometheus text). Runs on the
 * endpoint thread for every request; out is a private memory stream
 * whose bytes become the response body.
 *
 * Returns 0 to answer 200 with the body, nonzero to answer 500.
 */
typedef int (*sdf_metrics_render_fn)(void *ud, FILE *out);

/*
 * Bind a TCP socket and start serving: addr is "host:port" with an
 * empty host meaning 127.0.0.1 ("0.0.0.0" opts into any-interface).
 * Clients are served sequentially and closed after each response;
 * stop() joins the thread (poll ticks at 100 ms, per-request read
 * timeout 2 s). Returns the open endpoint, or NULL (diagnostic on
 * stderr).
 */
sdf_metrics_endpoint_t *sdf_metrics_endpoint_start(const char *addr,
                                                    sdf_metrics_render_fn render,
                                                    void *ud);

/*
 * Stop serving and free the handle: signal the thread, join it, close
 * the socket. render/ud must still be valid when called; NULL is a
 * no-op.
 */
void sdf_metrics_endpoint_stop(sdf_metrics_endpoint_t *ep);

#ifdef __cplusplus
}
#endif

#endif /* SDF_COMMON_METRICS_ENDPOINT_H */
