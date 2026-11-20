/*
 * bench_origin — HTTP origin transport measurement (plan W6.15).
 *
 * Records issue-to-completion latency percentiles, goodput, connection
 * reuse, post-warmup body-buffer allocation, and saturation/backpressure
 * behavior for the POSIX HTTP driver against a deterministic in-process
 * HTTP/1.1 Range server. The artifact line names the driver, server,
 * runner, build mode, workload flags, and sample counts so an A/B run
 * cannot silently compare different configurations.
 *
 * Recorded only (scripts/run_bench.sh), never ctest-gated: no regression
 * threshold is claimed until repeated named-runner baselines exist.
 * External servers and the optional curl/TLS arms are out of scope here;
 * no TLS or hosted evidence is produced by this tool.
 */

#define _POSIX_C_SOURCE 200809L

#include "bench_common.h"
#include "sdf/origin.h"
#include "sdf/origin/http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#ifndef SDF_BENCH_BUILD_TYPE
#define SDF_BENCH_BUILD_TYPE "unknown"
#endif

#if defined(__clang__)
#define BENCH_CC "clang"
#elif defined(__GNUC__)
#define BENCH_CC "gcc"
#else
#define BENCH_CC "cc"
#endif

#define BENCH_OBJ_BYTES (4u * 1024u * 1024u)
#define BENCH_DEFAULT_UNIT (64u * 1024u)
#define BENCH_DEFAULT_SAMPLES 512u
#define BENCH_DEFAULT_WARMUP 32u
#define BENCH_DEFAULT_WINDOW 1u
#define BENCH_MAX_WINDOW 1024u
#define BENCH_SRV_POLL_MS 100

/* ---- deterministic in-process HTTP/1.1 Range server -------------------- */

typedef struct {
    int listen_fd;
    int port;
    _Atomic int stop;
    _Atomic uint64_t conns;
    pthread_t tid;
    int started;
    uint8_t *obj;
    uint32_t obj_bytes;
} bench_srv_t;

static int send_all(int fd, const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n <= 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static void srv_serve_conn(bench_srv_t *s, int c) {
    char req[2048];
    for (;;) {
        size_t got = 0;
        int have_hdr = 0;
        while (!have_hdr) {
            if (got >= sizeof(req) - 1) {
                return;
            }
            ssize_t n = recv(c, req + got, sizeof(req) - 1 - got, 0);
            if (n <= 0) {
                return;
            }
            got += (size_t)n;
            req[got] = '\0';
            have_hdr = strstr(req, "\r\n\r\n") != NULL;
        }

        char path[32] = {0};
        if (sscanf(req, "GET %31s HTTP/1.", path) != 1 || strcmp(path, "/obj") != 0) {
            return;
        }
        const char *rng = strstr(req, "Range: bytes=");
        unsigned long long a = 0;
        unsigned long long b = 0;
        if (rng == NULL || sscanf(rng + 13, "%llu-%llu", &a, &b) != 2 || a > b ||
            b >= s->obj_bytes) {
            return;
        }
        unsigned long long len = b - a + 1;
        char head[256];
        int hlen = snprintf(head, sizeof(head),
                            "HTTP/1.1 206 Partial Content\r\n"
                            "Content-Length: %llu\r\n"
                            "Content-Range: bytes %llu-%llu/%u\r\n"
                            "Connection: keep-alive\r\n\r\n",
                            len, a, b, s->obj_bytes);
        if (hlen <= 0 || (size_t)hlen >= sizeof(head)) {
            return;
        }
        if (send_all(c, (const uint8_t *)head, (size_t)hlen) != 0) {
            return;
        }
        if (send_all(c, s->obj + (size_t)a, (size_t)len) != 0) {
            return;
        }
    }
}

static void *srv_main(void *arg) {
    bench_srv_t *s = arg;
    for (;;) {
        if (atomic_load_explicit(&s->stop, memory_order_relaxed)) {
            return NULL;
        }
        struct pollfd pfd = {.fd = s->listen_fd, .events = POLLIN, 0};
        if (poll(&pfd, 1, BENCH_SRV_POLL_MS) <= 0) {
            continue;
        }
        int c = accept(s->listen_fd, NULL, NULL);
        if (c < 0) {
            continue;
        }
        atomic_fetch_add_explicit(&s->conns, 1, memory_order_relaxed);
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        srv_serve_conn(s, c);
        close(c);
    }
}

static int srv_start(bench_srv_t *s, uint32_t obj_bytes) {
    memset(s, 0, sizeof(*s));
    s->listen_fd = -1;
    s->obj_bytes = obj_bytes;
    s->obj = malloc(obj_bytes);
    if (s->obj == NULL) {
        return -1;
    }
    for (uint32_t i = 0; i < obj_bytes; i++) {
        s->obj[i] = (uint8_t)(i * 13u + 5u);
    }
    atomic_init(&s->stop, 0);
    atomic_init(&s->conns, 0);

    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0) {
        free(s->obj);
        s->obj = NULL;
        return -1;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(s->listen_fd, 8) != 0) {
        close(s->listen_fd);
        free(s->obj);
        s->obj = NULL;
        return -1;
    }
    struct sockaddr_in got = {0};
    socklen_t glen = sizeof(got);
    if (getsockname(s->listen_fd, (struct sockaddr *)&got, &glen) != 0) {
        close(s->listen_fd);
        free(s->obj);
        s->obj = NULL;
        return -1;
    }
    s->port = ntohs(got.sin_port);
    if (pthread_create(&s->tid, NULL, srv_main, s) != 0) {
        close(s->listen_fd);
        free(s->obj);
        s->obj = NULL;
        return -1;
    }
    s->started = 1;
    return 0;
}

static void srv_stop(bench_srv_t *s) {
    if (!s->started) {
        return;
    }
    atomic_store_explicit(&s->stop, 1, memory_order_relaxed);
    pthread_join(s->tid, NULL);
    close(s->listen_fd);
    free(s->obj);
    s->obj = NULL;
    s->started = 0;
}

/* ---- measurement -------------------------------------------------------- */

typedef struct {
    uint64_t submit_ns;
    uint64_t lat_ns;
    int status;
} bench_sample_t;

typedef struct {
    bench_sample_t *samples;
    _Atomic uint64_t done;
    _Atomic uint64_t bad;
    const uint8_t *obj;
    uint32_t obj_bytes;
} bench_ctx_t;

static void bench_cb(void *ud, const sdf_fetch_req_t *req, const uint8_t *out, uint32_t len,
                     int status) {
    bench_ctx_t *ctx = ud;
    bench_sample_t *s = &ctx->samples[req->job_id];
    s->lat_ns = sdf_now_ns() - s->submit_ns;
    s->status = status;
    if (status == 0 && (out == NULL || len != req->len ||
                        memcmp(out, ctx->obj + (size_t)req->obj_offset, len) != 0)) {
        atomic_fetch_add_explicit(&ctx->bad, 1, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&ctx->done, 1, memory_order_release);
}

typedef struct {
    _Atomic uint64_t done;
    _Atomic uint64_t bad;
    const uint8_t *obj;
    uint32_t obj_bytes;
} sat_ctx_t;

static void sat_cb(void *ud, const sdf_fetch_req_t *req, const uint8_t *out, uint32_t len,
                   int status) {
    sat_ctx_t *ctx = ud;
    if (status == 0 && (out == NULL || len != req->len ||
                        memcmp(out, ctx->obj + (size_t)req->obj_offset, len) != 0)) {
        atomic_fetch_add_explicit(&ctx->bad, 1, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&ctx->done, 1, memory_order_release);
}

static void make_req(sdf_fetch_req_t *req, uint64_t idx, uint32_t unit_bytes, uint32_t obj_bytes) {
    memset(req, 0, sizeof(*req));
    req->origin_id = 0;
    req->job_id = idx;
    req->obj_offset = (idx * unit_bytes) % (uint64_t)(obj_bytes - unit_bytes + 1u);
    req->len = unit_bytes;
}

/* Submit `count` units keeping at most `window` outstanding. Latency is
 * issue-to-completion including queue wait, which is the dispatcher's
 * observable metric. */
static int run_phase(sdf_origin_t *o, bench_ctx_t *ctx, uint64_t first, uint32_t count,
                     uint32_t window, uint32_t unit_bytes, uint32_t obj_bytes, uint64_t *t0_out,
                     uint64_t *t1_out) {
    uint64_t base_done = atomic_load_explicit(&ctx->done, memory_order_acquire);
    uint64_t submitted = 0;
    uint64_t completed = 0;
    *t0_out = 0;
    *t1_out = 0;
    if (count == 0) {
        *t0_out = sdf_now_ns();
        *t1_out = *t0_out;
        return 0;
    }

    while (completed < count) {
        while (submitted < count && submitted - completed < window) {
            uint64_t idx = first + submitted;
            sdf_fetch_req_t req;
            make_req(&req, idx, unit_bytes, obj_bytes);
            if (submitted == 0) {
                *t0_out = sdf_now_ns();
            }
            ctx->samples[idx].submit_ns = sdf_now_ns();
            int rc = o->ops->fetch(o, &req, bench_cb, ctx);
            if (rc != 0) {
                fprintf(stderr, "bench_origin: fetch failed at index %" PRIu64 ": %d\n", idx, rc);
                return rc;
            }
            submitted++;
        }
        if (completed >= count) {
            break;
        }
        uint64_t d;
        do {
            d = atomic_load_explicit(&ctx->done, memory_order_acquire) - base_done;
        } while (d == completed);
        completed = d;
    }
    *t1_out = sdf_now_ns();
    return 0;
}

static uint64_t percentile(const uint64_t *v, uint32_t n, uint32_t pct) {
    uint64_t rank = ((uint64_t)pct * (uint64_t)n + 99u) / 100u;
    if (rank == 0) {
        rank = 1;
    }
    if (rank > n) {
        rank = n;
    }
    return v[rank - 1];
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int run_saturation(sdf_origin_t *o, uint32_t unit_bytes, uint32_t obj_bytes, uint32_t window,
                          const uint8_t *obj, uint64_t *accepted_out, uint64_t *eagain_out,
                          uint64_t *submit_max_out, uint64_t *drain_ns_out, uint64_t *bad_out) {
    sat_ctx_t ctx = {.obj = obj, .obj_bytes = obj_bytes};
    atomic_init(&ctx.done, 0);
    atomic_init(&ctx.bad, 0);

    uint64_t accepted = 0;
    uint64_t eagain = 0;
    uint64_t submit_max = 0;
    uint32_t attempts = window + 4u;
    for (uint32_t i = 0; i < attempts; i++) {
        sdf_fetch_req_t req;
        make_req(&req, i, unit_bytes, obj_bytes);
        uint64_t t0 = sdf_now_ns();
        int rc = o->ops->fetch(o, &req, sat_cb, &ctx);
        uint64_t dt = sdf_now_ns() - t0;
        if (dt > submit_max) {
            submit_max = dt;
        }
        if (rc == -EAGAIN) {
            eagain++;
        } else if (rc == 0) {
            accepted++;
        } else {
            fprintf(stderr, "bench_origin: saturation fetch returned %d\n", rc);
            return rc;
        }
    }
    if (eagain == 0) {
        fprintf(stderr, "bench_origin: saturation never hit -EAGAIN\n");
        return -EIO;
    }
    uint64_t t0 = sdf_now_ns();
    while (atomic_load_explicit(&ctx.done, memory_order_acquire) < accepted) {
        ; /* drain the accepted burst */
    }
    *accepted_out = accepted;
    *eagain_out = eagain;
    *submit_max_out = submit_max;
    *drain_ns_out = sdf_now_ns() - t0;
    *bad_out = atomic_load_explicit(&ctx.bad, memory_order_relaxed);
    return 0;
}

/* ---- main --------------------------------------------------------------- */

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--samples N] [--warmup N] [--unit-bytes N] [--window N]\n"
            "          [--server NAME] [--runner NAME]\n"
            "  --samples   measured units (default %u)\n"
            "  --warmup    warmup units before measurement (default %u)\n"
            "  --unit-bytes range bytes per unit (default %u, max %u)\n"
            "  --window    max outstanding units (default %u, max %u)\n",
            argv0, BENCH_DEFAULT_SAMPLES, BENCH_DEFAULT_WARMUP, BENCH_DEFAULT_UNIT,
            SDF_ORIGIN_UNIT_MAX, BENCH_DEFAULT_WINDOW, BENCH_MAX_WINDOW);
}

static int parse_u32(const char *s, uint32_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

int main(int argc, char **argv) {
    uint32_t samples = BENCH_DEFAULT_SAMPLES;
    uint32_t warmup = BENCH_DEFAULT_WARMUP;
    uint32_t unit_bytes = BENCH_DEFAULT_UNIT;
    uint32_t window = BENCH_DEFAULT_WINDOW;
    const char *server = "in-process-range-v1";
    char runner[256];

    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(runner, sizeof(runner), "%s-%s-%s", u.sysname, u.release, u.machine);
    } else {
        snprintf(runner, sizeof(runner), "unknown");
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (i + 1 >= argc) {
            usage(argv[0]);
            return 1;
        }
        const char *val = argv[++i];
        if (strcmp(argv[i - 1], "--samples") == 0) {
            if (parse_u32(val, &samples) != 0) {
                usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i - 1], "--warmup") == 0) {
            if (parse_u32(val, &warmup) != 0) {
                usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i - 1], "--unit-bytes") == 0) {
            if (parse_u32(val, &unit_bytes) != 0) {
                usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i - 1], "--window") == 0) {
            if (parse_u32(val, &window) != 0) {
                usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i - 1], "--server") == 0) {
            server = val;
        } else if (strcmp(argv[i - 1], "--runner") == 0) {
            if (strlen(val) >= sizeof(runner)) {
                fprintf(stderr, "bench_origin: --runner too long\n");
                return 1;
            }
            memcpy(runner, val, strlen(val) + 1);
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (samples == 0 || unit_bytes == 0 || unit_bytes > SDF_ORIGIN_UNIT_MAX ||
        unit_bytes > BENCH_OBJ_BYTES || window == 0 || window > BENCH_MAX_WINDOW ||
        warmup > UINT32_MAX - samples) {
        usage(argv[0]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    bench_srv_t srv;
    if (srv_start(&srv, BENCH_OBJ_BYTES) != 0) {
        fprintf(stderr, "bench_origin: cannot start in-process server\n");
        return 1;
    }

    char url[192];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/obj", srv.port);
    sdf_origin_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.id = 0;
    cfg.cls = SDF_ORIGIN_HTTP;
    snprintf(cfg.dst, sizeof(cfg.dst), "%s", url);
    cfg.timeout_ms = 2000;
    cfg.max_inflight = window;

    sdf_origin_t origin = {.ops = sdf_http_ops(), .impl = NULL};
    int rc = origin.ops->open(&origin, &cfg, 1);
    if (rc != 0) {
        fprintf(stderr, "bench_origin: open failed: %d\n", rc);
        srv_stop(&srv);
        return 1;
    }

    uint32_t total = warmup + samples;
    bench_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.obj = srv.obj;
    ctx.obj_bytes = srv.obj_bytes;
    ctx.samples = calloc(total, sizeof(*ctx.samples));
    if (ctx.samples == NULL) {
        fprintf(stderr, "bench_origin: out of memory\n");
        origin.ops->close(&origin);
        srv_stop(&srv);
        return 1;
    }
    atomic_init(&ctx.done, 0);
    atomic_init(&ctx.bad, 0);

    sdf_http_transport_stats_t t_open;
    sdf_http_transport_stats_t t_warm;
    sdf_http_transport_stats_t t_meas;
    if (sdf_http_transport_stats(&origin, 0, &t_open) != 0) {
        fprintf(stderr, "bench_origin: transport stats unavailable\n");
        free(ctx.samples);
        origin.ops->close(&origin);
        srv_stop(&srv);
        return 1;
    }

    uint64_t t0 = 0;
    uint64_t t1 = 0;
    rc = run_phase(&origin, &ctx, 0, warmup, window, unit_bytes, srv.obj_bytes, &t0, &t1);
    if (rc != 0) {
        goto fail;
    }
    if (sdf_http_transport_stats(&origin, 0, &t_warm) != 0) {
        rc = -EIO;
        goto fail;
    }

    rc = run_phase(&origin, &ctx, warmup, samples, window, unit_bytes, srv.obj_bytes, &t0, &t1);
    if (rc != 0) {
        goto fail;
    }
    if (sdf_http_transport_stats(&origin, 0, &t_meas) != 0) {
        rc = -EIO;
        goto fail;
    }

    uint64_t sat_accepted = 0;
    uint64_t sat_eagain = 0;
    uint64_t sat_submit_max = 0;
    uint64_t sat_drain_ns = 0;
    uint64_t sat_bad = 0;
    rc = run_saturation(&origin, unit_bytes, srv.obj_bytes, window, srv.obj, &sat_accepted,
                        &sat_eagain, &sat_submit_max, &sat_drain_ns, &sat_bad);
    if (rc != 0) {
        goto fail;
    }

    uint64_t bad = atomic_load_explicit(&ctx.bad, memory_order_relaxed) + sat_bad;
    uint64_t failures = 0;
    for (uint32_t i = 0; i < total; i++) {
        if (ctx.samples[i].status != 0) {
            failures++;
        }
    }

    sdf_origin_stats_t st;
    if (origin.ops->stats(&origin, 0, &st) != 0 || bad != 0 || failures != 0) {
        fprintf(stderr,
                "bench_origin: verification failed (bad=%" PRIu64 " failures=%" PRIu64 ")\n", bad,
                failures);
        rc = -EIO;
        goto fail;
    }

    uint64_t *lats = malloc((size_t)samples * sizeof(*lats));
    if (lats == NULL) {
        rc = -ENOMEM;
        goto fail;
    }
    for (uint32_t i = 0; i < samples; i++) {
        lats[i] = ctx.samples[warmup + i].lat_ns;
    }
    qsort(lats, samples, sizeof(*lats), cmp_u64);
    uint64_t p50 = percentile(lats, samples, 50);
    uint64_t p99 = percentile(lats, samples, 99);
    uint64_t max_lat = lats[samples - 1];
    free(lats);

    double seconds = (double)(t1 - t0) / 1000000000.0;
    uint64_t measured_bytes = (uint64_t)samples * unit_bytes;
    double units_s = seconds > 0.0 ? (double)samples / seconds : 0.0;
    double mib_s = seconds > 0.0 ? ((double)measured_bytes / (1024.0 * 1024.0)) / seconds : 0.0;

    printf("BENCH origin driver=sdf-http-posix server=%s runner=%s build=%s cc=%s "
           "flags=samples=%u,warmup=%u,unit_bytes=%u,window=%u "
           "connections=%" PRIu64 " reqs_out=%" PRIu64 " reqs_done=%" PRIu64 " reqs_failed=%" PRIu64
           " bytes=%" PRIu64 " seconds=%.6f units_s=%.1f "
           "MiB_s=%.2f p50_ns=%" PRIu64 " p99_ns=%" PRIu64 " max_ns=%" PRIu64
           " body_cap=%u body_allocs_open=%u body_allocs_warm=%u body_allocs_measured=%u "
           "sat_accepted=%" PRIu64 " sat_eagain=%" PRIu64 " sat_submit_max_ns=%" PRIu64
           " sat_drain_ns=%" PRIu64 "\n",
           server, runner, SDF_BENCH_BUILD_TYPE, BENCH_CC, samples, warmup, unit_bytes, window,
           atomic_load_explicit(&srv.conns, memory_order_relaxed), st.reqs_out, st.reqs_done,
           st.reqs_failed, measured_bytes, seconds, units_s, mib_s, p50, p99, max_lat,
           t_meas.body_cap, t_open.body_allocs, t_warm.body_allocs, t_meas.body_allocs,
           sat_accepted, sat_eagain, sat_submit_max, sat_drain_ns);

    free(ctx.samples);
    origin.ops->close(&origin);
    srv_stop(&srv);
    return 0;

fail:
    free(ctx.samples);
    origin.ops->close(&origin);
    srv_stop(&srv);
    return 1;
}
