/*
 * Controlled HTTP coordinator tests (plan prompt W6.15, first slice).
 *
 * Both production coordinators run over the real HTTP origin driver against
 * an in-process HTTP/1.1 server: WM proves exact normalized fill statistics
 * plus drain/restore, and FLUID proves a multi-block byte-exact repair with
 * two concurrent origin workers (the MPSC completion case). No hosted,
 * TLS, performance, or soak evidence is claimed by this local test.
 */

#define _POSIX_C_SOURCE 200809L

#include "sdf/fluid_repair.h"
#include "sdf/origin.h"
#include "sdf/wm_proxy.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define SRV_POLL_MS 100
#define SRV_MAX_CONNS 8

/* ---- in-process keep-alive HTTP/1.1 server ------------------------------- */

typedef struct test_srv_s test_srv_t;

typedef struct {
    test_srv_t *srv;
    int fd;
    pthread_t tid;
    int started;
} srv_conn_t;

struct test_srv_s {
    int listen_fd;
    int port;
    _Atomic int stop;
    pthread_t accept_tid;
    int accept_started;
    srv_conn_t conns[SRV_MAX_CONNS];
    const uint8_t *obj;
    uint64_t obj_len;
    _Atomic uint32_t accepted;
    _Atomic uint32_t requests;
};

static const char *ci_find(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == n) {
            return p;
        }
    }
    return NULL;
}

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

static void *conn_serve(void *argp) {
    srv_conn_t *self = argp;
    test_srv_t *owner = self->srv;
    int c = self->fd;
    char buf[2048];

    for (;;) {
        size_t got = 0;
        int have_hdr = 0;
        while (!have_hdr) {
            ssize_t n = recv(c, buf + got, sizeof(buf) - 1 - got, 0);
            if (n <= 0) {
                goto conn_done;
            }
            got += (size_t)n;
            buf[got] = '\0';
            have_hdr = strstr(buf, "\r\n\r\n") != NULL;
            if (got >= sizeof(buf) - 1 && !have_hdr) {
                goto conn_done;
            }
        }

        char path[256] = {0};
        if (sscanf(buf, "GET %255s HTTP/1.", path) != 1) {
            goto conn_done;
        }
        const char *rng = ci_find(buf, "range: bytes=");
        unsigned long long a = 0, b = 0;
        if (rng != NULL) {
            sscanf(rng + 13, "%llu-%llu", &a, &b);
        }

        /* obj is immutable while the server runs; capture once per request. */
        uint64_t obj_len = owner->obj_len;
        const uint8_t *object = owner->obj;
        if (strcmp(path, "/obj") != 0 || rng == NULL || a > b || b >= obj_len) {
            static const char not_found[] =
                "HTTP/1.1 404 Not Found\r\nContent-Length: 5\r\nConnection: close\r\n\r\nerr\r\n";
            (void)send_all(c, (const uint8_t *)not_found, sizeof(not_found) - 1);
            goto conn_done;
        }

        unsigned long long len = b - a + 1;
        char head[512];
        int hn = snprintf(head, sizeof(head),
                          "HTTP/1.1 206 Partial Content\r\n"
                          "Content-Range: bytes %llu-%llu/%llu\r\n"
                          "Content-Length: %llu\r\n"
                          "Connection: keep-alive\r\n\r\n",
                          a, b, (unsigned long long)obj_len, len);
        CHECK(hn > 0 && (size_t)hn < sizeof(head));
        atomic_fetch_add_explicit(&owner->requests, 1, memory_order_relaxed);
        if (send_all(c, (const uint8_t *)head, (size_t)hn) != 0 ||
            send_all(c, object + a, (size_t)len) != 0) {
            goto conn_done;
        }
    }

conn_done:
    close(c);
    return NULL;
}

static void *accept_main(void *arg) {
    test_srv_t *s = arg;
    for (;;) {
        if (atomic_load_explicit(&s->stop, memory_order_relaxed)) {
            return NULL;
        }
        struct pollfd pfd = {.fd = s->listen_fd, .events = POLLIN, 0};
        if (poll(&pfd, 1, SRV_POLL_MS) <= 0) {
            continue;
        }
        int c = accept(s->listen_fd, NULL, NULL);
        if (c < 0) {
            continue;
        }
        atomic_fetch_add_explicit(&s->accepted, 1, memory_order_relaxed);

        srv_conn_t *slot = NULL;
        for (uint32_t i = 0; i < SRV_MAX_CONNS; i++) {
            if (!s->conns[i].started) {
                slot = &s->conns[i];
                break;
            }
        }
        if (slot == NULL) {
            close(c);
            continue;
        }
        slot->srv = s;
        slot->fd = c;
        if (pthread_create(&slot->tid, NULL, conn_serve, slot) != 0) {
            close(c);
            continue;
        }
        slot->started = 1;
    }
}

static void srv_start(test_srv_t *s, const uint8_t *obj, uint64_t obj_len) {
    memset(s, 0, sizeof(*s));
    s->obj = obj;
    s->obj_len = obj_len;
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s->listen_fd >= 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    CHECK(listen(s->listen_fd, 16) == 0);
    struct sockaddr_in got = {0};
    socklen_t glen = sizeof(got);
    CHECK(getsockname(s->listen_fd, (struct sockaddr *)&got, &glen) == 0);
    s->port = ntohs(got.sin_port);
    CHECK(pthread_create(&s->accept_tid, NULL, accept_main, s) == 0);
    s->accept_started = 1;
}

static void srv_stop(test_srv_t *s) {
    atomic_store_explicit(&s->stop, 1, memory_order_relaxed);
    CHECK(shutdown(s->listen_fd, SHUT_RDWR) == 0);
    if (s->accept_started) {
        CHECK(pthread_join(s->accept_tid, NULL) == 0);
    }
    CHECK(close(s->listen_fd) == 0);
    for (uint32_t i = 0; i < SRV_MAX_CONNS; i++) {
        if (s->conns[i].started) {
            CHECK(pthread_join(s->conns[i].tid, NULL) == 0);
            s->conns[i].started = 0;
        }
    }
}

/* ---- shared configuration helpers ---------------------------------------- */

static void http_cfg(sdf_origin_cfg_t *c, uint32_t id, const test_srv_t *srv, uint32_t max_inflight,
                     uint32_t timeout_ms) {
    memset(c, 0, sizeof(*c));
    c->id = id;
    c->cls = SDF_ORIGIN_HTTP;
    snprintf(c->dst, sizeof(c->dst), "http://127.0.0.1:%d/obj", srv->port);
    c->max_inflight = max_inflight;
    c->timeout_ms = timeout_ms;
}

/* ---- WM fill over HTTP ---------------------------------------------------- */

static void test_wm_http_fill_exact(test_srv_t *srv) {
    enum { UNITS = 128, UNIT_BYTES = 256, N_ORIGINS = 2 };
    static uint8_t obj[SDF_WM_PROXY_OBJ_BYTES];
    for (size_t i = 0; i < sizeof(obj); i++) {
        obj[i] = (uint8_t)(i * 17u + 3u);
    }
    srv_start(srv, obj, sizeof(obj));

    sdf_wm_proxy_cfg_t pc;
    memset(&pc, 0, sizeof(pc));
    pc.units = UNITS;
    pc.unit_bytes = UNIT_BYTES;
    pc.jobs = 4;
    pc.control_hz = 1;
    pc.alpha = 0.25;
    pc.beta = 0.02;
    pc.rollout_pct = 0; /* pure round-robin: exact per-origin expectations */
    pc.profile.version = 1;
    pc.profile.m = 1024;
    pc.profile.n = N_ORIGINS;
    pc.profile.b[0] = 512;
    pc.profile.b[1] = 512;
    pc.spray.method = WM_METHOD_SHUFFLE1;
    pc.spray.sa = 333;
    pc.spray.sb = 735;
    pc.n_origins = N_ORIGINS;
    http_cfg(&pc.origins[0], 0, srv, 16, 1000);
    http_cfg(&pc.origins[1], 1, srv, 16, 1000);

    sdf_wm_proxy_stats_t st;
    int run_rc = sdf_wm_proxy_run(&pc, &st);
    if (run_rc != 0) {
        fprintf(stderr, "WM HTTP steady run failed: rc=%d errno=%d\n", run_rc, errno);
    }
    CHECK(run_rc == 0);
    CHECK(st.n_origins == N_ORIGINS);
    for (uint32_t i = 0; i < N_ORIGINS; i++) {
        CHECK(st.dispatched[i] == UNITS / N_ORIGINS);
        CHECK(st.ok[i] == UNITS / N_ORIGINS);
        CHECK(st.failed[i] == 0);
        CHECK(sdf_counter_read(&st.enolink[i]) == 0);
        CHECK(st.end_profile_b[i] == 512);
    }
    CHECK(st.bytes_rx == (uint64_t)UNITS * UNIT_BYTES);
    CHECK(st.dispatch_control == UNITS && st.dispatch_treatment == 0);
    CHECK(atomic_load(&st.fill_bytes_by_arm[0]) == st.bytes_rx);
    CHECK(atomic_load(&st.fill_bytes_by_arm[1]) == 0);
    CHECK(atomic_load(&st.fill_latency_by_arm[0].count) == UNITS);
    CHECK(atomic_load(&st.fill_latency_by_arm[1].count) == 0);
    CHECK(atomic_load(&srv->requests) == UNITS);

    /* Drain origin 0 for one quarter, then restore it. The control arm's
     * round-robin makes the refusal accounting exact. */
    memset(&st, 0, sizeof(st));
    pc.steps[0].at_unit = UNITS / 2;
    pc.steps[0].origin = 0;
    pc.steps[0].enable = 0;
    pc.steps[1].at_unit = (UNITS * 3) / 4;
    pc.steps[1].origin = 0;
    pc.steps[1].enable = 1;
    pc.n_steps = 2;
    CHECK(sdf_wm_proxy_run(&pc, &st) == 0);
    uint64_t total_ok = 0;
    uint64_t total_enolink = 0;
    for (uint32_t i = 0; i < N_ORIGINS; i++) {
        total_ok += st.ok[i];
        total_enolink += sdf_counter_read(&st.enolink[i]);
        CHECK(st.failed[i] == 0);
    }
    CHECK(st.ok[0] == UNITS / 2 - 16);
    CHECK(sdf_counter_read(&st.enolink[0]) == 16);
    CHECK(sdf_counter_read(&st.enolink[1]) == 0);
    CHECK(total_ok + total_enolink == UNITS);
    srv_stop(srv);
}

static void test_wm_selection_failures(void) {
    sdf_origin_cfg_t origins[2];
    memset(origins, 0, sizeof(origins));
    origins[0].id = 0;
    origins[1].id = 1;
    snprintf(origins[0].dst, sizeof(origins[0].dst), "s3://bucket/object");
    snprintf(origins[1].dst, sizeof(origins[1].dst), "s3://bucket/object");
    origins[0].cls = SDF_ORIGIN_S3;
    origins[1].cls = SDF_ORIGIN_S3;

    sdf_wm_proxy_cfg_t pc;
    memset(&pc, 0, sizeof(pc));
    pc.units = 1;
    pc.unit_bytes = 16;
    pc.jobs = 1;
    pc.profile.version = 1;
    pc.profile.m = 1024;
    pc.profile.n = 2;
    pc.profile.b[0] = 512;
    pc.profile.b[1] = 512;
    pc.n_origins = 2;
    pc.origins[0] = origins[0];
    pc.origins[1] = origins[1];

    sdf_wm_proxy_stats_t st;
    CHECK(sdf_wm_proxy_run(&pc, &st) == -ENOTSUP);
    origins[1].cls = SDF_ORIGIN_LOOPBACK;
    snprintf(origins[1].dst, sizeof(origins[1].dst), "/tmp/sdf-missing-origin.bin");
    pc.origins[1] = origins[1];
    CHECK(sdf_wm_proxy_run(&pc, &st) == -EINVAL);
    origins[0].cls = 99;
    pc.origins[0] = origins[0];
    CHECK(sdf_wm_proxy_run(&pc, &st) == -EINVAL);
}

/* ---- multi-block FLUID repair over HTTP ---------------------------------- */

static void test_fluid_http_multi_block(void) {
    enum { K = 64, T = 32, BLOCKS = 3, N_ORIGINS = 2 };
    sdf_fluid_repair_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.arm = SDF_REPAIR_NANORQ;
    cfg.k = K;
    cfg.sym_len = T;
    cfg.eps = 0.10;
    cfg.loss_pct = 0.0; /* HTTP gets its loss from the real network */
    cfg.n_blocks = BLOCKS;
    cfg.wave = 32;
    cfg.n_origins = N_ORIGINS;

    uint64_t bytes = sdf_fluid_repair_stream_bytes(&cfg);
    CHECK(bytes > 0);
    uint8_t *stream = malloc(bytes);
    CHECK(stream != NULL);
    CHECK(sdf_fluid_repair_encode_stream(&cfg, stream, bytes) == 0);

    test_srv_t srv;
    srv_start(&srv, stream, bytes);
    for (uint32_t i = 0; i < N_ORIGINS; i++) {
        http_cfg(&cfg.origins[i], i, &srv, 8, 1000);
    }

    sdf_fluid_repair_stats_t st;
    CHECK(sdf_fluid_repair_run(&cfg, &st) == 0);
    CHECK(st.ok == 1);
    CHECK(st.n_blocks == BLOCKS);
    /* The no-loss gate certifies after the first wave, before decode
     * sufficiency; the mandatory per-block ARQ fallback then fetches the
     * remaining systematic ESIs. The result is still exactly one submit per
     * systematic symbol per block. */
    CHECK(st.arq_fallback == 1);
    CHECK(st.rounds == 1);
    CHECK(st.sent == (uint64_t)K * BLOCKS);
    CHECK(st.received == (uint64_t)K * BLOCKS);
    CHECK(st.bytes_tx == st.sent * T);
    CHECK(st.bytes_rx == st.received * T);
    CHECK(st.received >= (uint64_t)K * BLOCKS);
    CHECK(st.received <= bytes / T);
    for (uint32_t b = 0; b < BLOCKS; b++) {
        CHECK(st.blocks[b].ok == 1);
        CHECK(st.blocks[b].decode_rank >= K);
        CHECK(st.blocks[b].bytes_tx == st.blocks[b].sent * T);
        CHECK(st.blocks[b].bytes_rx == st.blocks[b].received * T);
    }
    /* Two origin workers were actually used: the MPSC delivery path ran. */
    CHECK(atomic_load(&srv.accepted) >= N_ORIGINS);

    srv_stop(&srv);
    free(stream);
}

static void test_fluid_selection_failures(void) {
    sdf_fluid_repair_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.arm = SDF_REPAIR_LT;
    cfg.k = 64;
    cfg.sym_len = 32;
    cfg.eps = 0.10;
    cfg.n_origins = 2;
    cfg.origins[0].id = 0;
    cfg.origins[1].id = 1;
    cfg.origins[0].cls = SDF_ORIGIN_PEER;
    cfg.origins[1].cls = SDF_ORIGIN_PEER;
    snprintf(cfg.origins[0].dst, sizeof(cfg.origins[0].dst), "peer-dc-a");
    snprintf(cfg.origins[1].dst, sizeof(cfg.origins[1].dst), "peer-dc-b");

    sdf_fluid_repair_stats_t st;
    CHECK(sdf_fluid_repair_run(&cfg, &st) == -ENOTSUP);
    cfg.origins[1].cls = SDF_ORIGIN_HTTP;
    snprintf(cfg.origins[1].dst, sizeof(cfg.origins[1].dst), "http://127.0.0.1:1/obj");
    CHECK(sdf_fluid_repair_run(&cfg, &st) == -EINVAL);
}

int main(void) {
    test_srv_t srv;
    test_wm_http_fill_exact(&srv);
    test_wm_selection_failures();
    test_fluid_http_multi_block();
    test_fluid_selection_failures();
    printf("test_origin_coordinators: ok (HTTP WM exact fill/drain + multi-block "
           "FLUID repair + clear selection failures)\n");
    return 0;
}
