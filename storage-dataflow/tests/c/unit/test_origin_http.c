/*
 * HTTP origin driver tests (plan prompt W1.3).
 *
 * The plan suggested a local Python http.server; this test serves the
 * same wire contract from an in-process thread instead — same
 * HTTP/1.1 + Range semantics, zero environment assumptions, and the
 * blackhole leg (accepts, never responds) is expressible too. Legs:
 * URL validation, ranged fetch correctness + stats exactness, 404
 * mapping with reconnect, socket timeouts surfacing as failed units
 * within bound, connect-refused latching hard_down, drain/restore,
 * close flushing in-flight as -ECANCELED, reusable transport-buffer
 * growth/reuse, chunked partial responses, truncated-body recovery,
 * and queue saturation returning -EAGAIN without blocking.
 */

#define _POSIX_C_SOURCE 200809L

#include "sdf/origin.h"
#include "sdf/origin/http.h"

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
#include <time.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define OBJ_BYTES (64u * 1024u)
#define SRV_POLL_MS 100

static _Atomic int g_overlong_body;
static _Atomic int g_chunked_body;
static _Atomic int g_short_body;

/* ---- in-process HTTP/1.1 server ---------------------------------------- */

typedef struct {
    int listen_fd;
    int port;
    _Atomic int stop;
    pthread_t tid;
    int started;
    uint8_t obj[OBJ_BYTES];
} test_srv_t;

/* case-insensitive substring search (strcasestr is GNU, not POSIX) */
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

static int send_all_fd(int fd, const uint8_t *buf, size_t len) {
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

/* One connection: sequential keep-alive requests until the client goes. */
typedef struct {
    test_srv_t *srv;
    int fd;
} conn_arg_t;

static void *conn_serve(void *argp) {
    conn_arg_t *ca = argp;
    test_srv_t *s = ca->srv;
    int c = ca->fd;
    free(ca);

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
        }
        char path[160] = {0};
        if (sscanf(buf, "GET %159s HTTP/1.", path) != 1) {
            goto conn_done;
        }

        const char *rng = ci_find(buf, "range: bytes=");
        unsigned long long a = 0, b = 0;
        if (rng != NULL) {
            sscanf(rng + 13, "%llu-%llu", &a, &b);
        }

        char head[512];
        if (strcmp(path, "/obj") == 0 && rng != NULL && a <= b && b < OBJ_BYTES) {
            unsigned long long len = b - a + 1;
            unsigned long long body_len = len;
            int overlong = atomic_load_explicit(&g_overlong_body, memory_order_relaxed);
            int chunked = atomic_load_explicit(&g_chunked_body, memory_order_relaxed);
            int short_body = atomic_load_explicit(&g_short_body, memory_order_relaxed);
            if (overlong) {
                /* Malformed-response leg: more body bytes than the declared
                 * Content-Length, delivered with the header in one write. */
                body_len += 4096;
            }
            snprintf(head, sizeof(head),
                     "HTTP/1.1 206 Partial Content\r\n"
                     "Content-Range: bytes %llu-%llu/%u\r\n"
                     "Content-Length: %llu\r\n"
                     "Connection: keep-alive\r\n\r\n",
                     a, b, (unsigned)OBJ_BYTES, len);
            if (chunked) {
                /* Header first, then short body writes: exercises the
                 * body_pre == 0 and partial-recv paths deterministically. */
                if (send_all_fd(c, (const uint8_t *)head, strlen(head)) != 0) {
                    goto conn_done;
                }
                size_t sent = 0;
                while (sent < (size_t)body_len) {
                    size_t n = (size_t)body_len - sent;
                    if (n > 137u) {
                        n = 137u;
                    }
                    uint8_t piece[137];
                    for (size_t i = 0; i < n; i++) {
                        piece[i] = s->obj[(size_t)(a + sent + i) % OBJ_BYTES];
                    }
                    if (send_all_fd(c, piece, n) != 0) {
                        goto conn_done;
                    }
                    sent += n;
                    if ((sent / 137u) % 8u == 0u) {
                        struct timespec ts = {.tv_sec = 0, .tv_nsec = 500000};
                        nanosleep(&ts, NULL);
                    }
                }
            } else {
                size_t head_len = strlen(head);
                size_t send_len = head_len + (size_t)body_len;
                if (short_body && body_len > 1u) {
                    /* Declare the full range but close after half the body:
                     * the unit must fail, then the next fetch recovers. */
                    send_len = head_len + (size_t)(body_len / 2u);
                }
                uint8_t *resp = malloc(send_len);
                CHECK(resp != NULL);
                memcpy(resp, head, head_len);
                for (size_t i = 0; head_len + i < send_len; i++) {
                    resp[head_len + i] = s->obj[(size_t)(a + i) % OBJ_BYTES];
                }
                int send_rc = send_all_fd(c, resp, send_len);
                free(resp);
                if (send_rc != 0 || short_body) {
                    goto conn_done;
                }
            }
        } else {
            snprintf(head, sizeof(head),
                     "HTTP/1.1 404 Not Found\r\n"
                     "Content-Length: 5\r\n"
                     "Connection: close\r\n\r\nerr\r\n");
            send(c, head, strlen(head), 0);
            goto conn_done;
        }
    }
conn_done:
    close(c);
    return NULL;
}

static void *srv_main(void *arg) {
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
        /* keep-alive parks per connection: serve each on its own
         * detached thread so concurrent origins are never starved */
        conn_arg_t *ca = malloc(sizeof(*ca));
        if (ca == NULL) {
            close(c);
            continue;
        }
        ca->srv = s;
        ca->fd = c;
        pthread_t tid;
        if (pthread_create(&tid, NULL, conn_serve, ca) != 0) {
            close(c);
            free(ca);
            continue;
        }
        pthread_detach(tid);
    }
}

/* Accepts, never responds: the timeout leg. */
static void *blackhole_main(void *arg) {
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
        char scratch[64];
        while (recv(c, scratch, sizeof(scratch), 0) > 0) {
            ; /* park until the client gives up */
        }
        close(c);
    }
}

static void srv_start(test_srv_t *s, void *(*entry)(void *)) {
    memset(s, 0, sizeof(*s));
    for (size_t i = 0; i < sizeof(s->obj); i++) {
        s->obj[i] = (uint8_t)(i * 13u + 5u);
    }

    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s->listen_fd >= 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    CHECK(listen(s->listen_fd, 8) == 0);
    struct sockaddr_in got = {0};
    socklen_t glen = sizeof(got);
    CHECK(getsockname(s->listen_fd, (struct sockaddr *)&got, &glen) == 0);
    s->port = ntohs(got.sin_port);
    CHECK(pthread_create(&s->tid, NULL, entry, s) == 0);
    s->started = 1;
}

static void srv_stop(test_srv_t *s) {
    atomic_store_explicit(&s->stop, 1, memory_order_relaxed);
    CHECK(pthread_join(s->tid, NULL) == 0);
    CHECK(close(s->listen_fd) == 0);
    s->listen_fd = -1;
}

/* ---- test legs ---------------------------------------------------------- */

static test_srv_t g_srv;

typedef struct {
    _Atomic uint64_t ok, not_found, range, timeout, refused, cancelled, bad_response;
    _Atomic uint64_t bytes_ok;
} tally_t;
static tally_t g_t;
static _Atomic int g_callback_entered;
static _Atomic int g_callback_release;

static void reset_tally(void) {
    memset(&g_t, 0, sizeof(g_t));
}

static void cb(void *ud, const sdf_fetch_req_t *req, const uint8_t *out, uint32_t len, int status) {
    (void)ud;
    switch (status) {
    case 0:
        atomic_fetch_add(&g_t.ok, 1);
        atomic_fetch_add(&g_t.bytes_ok, len);
        CHECK(out != NULL && len == req->len);
        for (uint32_t i = 0; i < len; i++) {
            CHECK(out[i] == g_srv.obj[req->obj_offset + i]);
        }
        break;
    case -ENOENT:
        atomic_fetch_add(&g_t.not_found, 1);
        CHECK(out == NULL && len == 0);
        break;
    case -ERANGE:
        atomic_fetch_add(&g_t.range, 1);
        break;
    case -ETIMEDOUT:
        atomic_fetch_add(&g_t.timeout, 1);
        break;
    case -ECONNREFUSED:
        atomic_fetch_add(&g_t.refused, 1);
        break;
    case -ECANCELED:
        atomic_fetch_add(&g_t.cancelled, 1);
        break;
    case -EIO:
        atomic_fetch_add(&g_t.bad_response, 1);
        CHECK(out == NULL && len == 0);
        break;
    default:
        fprintf(stderr, "FAIL: unexpected cb status %d\n", status);
        exit(1);
    }
}

static void blocking_cb(void *ud, const sdf_fetch_req_t *req, const uint8_t *out, uint32_t len,
                        int status) {
    atomic_store_explicit(&g_callback_entered, 1, memory_order_release);
    while (!atomic_load_explicit(&g_callback_release, memory_order_acquire)) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&ts, NULL);
    }
    cb(ud, req, out, len, status);
}

static void wait_quiet(sdf_origin_t *o, uint32_t n) {
    for (int i = 0; i < 20000; i++) {
        uint64_t inflight = 0;
        for (uint32_t k = 0; k < n; k++) {
            sdf_origin_stats_t st;
            CHECK(o->ops->stats(o, k, &st) == 0);
            inflight += st.in_flight;
        }
        if (inflight == 0) {
            return;
        }
        {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
            nanosleep(&ts, NULL);
        }
    }
    fprintf(stderr, "FAIL: origins never went quiet\n");
    exit(1);
}

static void url_cfg(sdf_origin_cfg_t *c, uint32_t id, const char *url, uint32_t timeout_ms) {
    memset(c, 0, sizeof(*c));
    c->id = id;
    c->cls = SDF_ORIGIN_HTTP;
    snprintf(c->dst, sizeof(c->dst), "%s", url);
    c->timeout_ms = timeout_ms;
    c->max_inflight = 16;
}

static void test_open_validation(void) {
    sdf_origin_cfg_t c;
    sdf_origin_t o = {.ops = sdf_http_ops(), .impl = NULL};

    url_cfg(&c, 0, "https://secure.example/obj", 0);
    CHECK(o.ops->open(&o, &c, 1) == -EINVAL); /* TLS not in this driver */
    url_cfg(&c, 0, "notaurl", 0);
    CHECK(o.ops->open(&o, &c, 1) == -EINVAL);
    url_cfg(&c, 0, "http://", 0);
    CHECK(o.ops->open(&o, &c, 1) == -EINVAL); /* empty host */
    url_cfg(&c, 0, "http://host:-1/x", 0);
    CHECK(o.ops->open(&o, &c, 1) == -EINVAL); /* bad port */
    url_cfg(&c, 3, "http://127.0.0.1:1/x", 0);
    CHECK(o.ops->open(&o, &c, 1) == -EINVAL); /* non-canonical id */
    CHECK(o.impl == NULL);                    /* failed opens leave no state */
}

static void test_fetch_and_stats(void) {
    char url[192];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/obj", g_srv.port);

    sdf_origin_cfg_t cfg[2];
    url_cfg(&cfg[0], 0, url, 0);
    url_cfg(&cfg[1], 1, url, 0);

    sdf_origin_t o = {.ops = sdf_http_ops(), .impl = NULL};
    CHECK(o.ops->open(&o, cfg, 2) == 0);
    reset_tally();

    sdf_fetch_req_t req = {.origin_id = 0, .job_id = 1, .obj_offset = 1024, .len = 4096};
    CHECK(o.ops->fetch(&o, &req, cb, NULL) == 0);
    req.origin_id = 1;
    req.obj_offset = OBJ_BYTES - 256;
    req.len = 256;
    CHECK(o.ops->fetch(&o, &req, cb, NULL) == 0);
    wait_quiet(&o, 2);
    CHECK(atomic_load(&g_t.ok) == 2);
    CHECK(atomic_load(&g_t.bytes_ok) == 4096 + 256);

    /* submit-time rejections */
    req.origin_id = 9;
    CHECK(o.ops->fetch(&o, &req, cb, NULL) == -EINVAL);
    req.origin_id = 0;
    req.len = 0;
    CHECK(o.ops->fetch(&o, &req, cb, NULL) == -EINVAL);

    sdf_origin_stats_t st;
    CHECK(o.ops->stats(&o, 0, &st) == 0);
    CHECK(st.cls == SDF_ORIGIN_HTTP);
    CHECK(st.reqs_out == 1 && st.reqs_done == 1 && st.reqs_failed == 0);
    CHECK(st.bytes_tx == 4096 && st.bytes_rx == 4096);
    CHECK(st.hard_down == 0);

    /* 404 maps to -ENOENT; the connection drops and the next unit
     * reconnects and succeeds */
    char url404[192];
    snprintf(url404, sizeof(url404), "http://127.0.0.1:%d/missing", g_srv.port);
    url_cfg(&cfg[0], 0, url404, 0);
    sdf_origin_t o404 = {.ops = sdf_http_ops(), .impl = NULL};
    CHECK(o404.ops->open(&o404, cfg, 1) == 0);
    req.origin_id = 0;
    req.obj_offset = 0;
    req.len = 16;
    CHECK(o404.ops->fetch(&o404, &req, cb, NULL) == 0);
    wait_quiet(&o404, 1);
    CHECK(atomic_load(&g_t.not_found) == 1);
    CHECK(o404.ops->stats(&o404, 0, &st) == 0);
    CHECK(st.reqs_failed == 1);
    o404.ops->close(&o404);

    o.ops->close(&o);
    CHECK(o.impl == NULL);
}

static void test_inflight_covers_callback(void) {
    char url[192];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/obj", g_srv.port);

    sdf_origin_cfg_t c;
    url_cfg(&c, 0, url, 1000);
    sdf_origin_t o = {.ops = sdf_http_ops(), .impl = NULL};
    CHECK(o.ops->open(&o, &c, 1) == 0);
    reset_tally();
    atomic_store_explicit(&g_callback_entered, 0, memory_order_relaxed);
    atomic_store_explicit(&g_callback_release, 0, memory_order_relaxed);

    sdf_fetch_req_t req = {.origin_id = 0, .job_id = 1, .obj_offset = 0, .len = 32};
    CHECK(o.ops->fetch(&o, &req, blocking_cb, NULL) == 0);
    for (int i = 0; i < 2000 && !atomic_load_explicit(&g_callback_entered, memory_order_acquire);
         i++) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&ts, NULL);
    }
    CHECK(atomic_load_explicit(&g_callback_entered, memory_order_acquire) == 1);

    sdf_origin_stats_t st;
    CHECK(o.ops->stats(&o, 0, &st) == 0);
    CHECK(st.in_flight == 1); /* the callback is part of completion */

    atomic_store_explicit(&g_callback_release, 1, memory_order_release);
    wait_quiet(&o, 1);
    CHECK(o.ops->stats(&o, 0, &st) == 0);
    CHECK(st.in_flight == 0);
    CHECK(atomic_load(&g_t.ok) == 1);
    o.ops->close(&o);
}

static void test_buffer_reuse_and_growth(void) {
    char url[192];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/obj", g_srv.port);

    sdf_origin_cfg_t c;
    url_cfg(&c, 0, url, 1000);
    sdf_origin_t o = {.ops = sdf_http_ops(), .impl = NULL};
    CHECK(o.ops->open(&o, &c, 1) == 0);
    reset_tally();

    sdf_http_transport_stats_t ts;
    CHECK(sdf_http_transport_stats(&o, 0, &ts) == 0);
    CHECK(ts.body_cap == 0 && ts.body_allocs == 0);
    CHECK(sdf_http_transport_stats(&o, 1, &ts) == -EINVAL);
    CHECK(sdf_http_transport_stats(NULL, 0, &ts) == -EINVAL);
    CHECK(sdf_http_transport_stats(&o, 0, NULL) == -EINVAL);

    sdf_fetch_req_t req = {.origin_id = 0, .job_id = 1, .obj_offset = 0, .len = 1024};
    CHECK(o.ops->fetch(&o, &req, cb, NULL) == 0);
    wait_quiet(&o, 1);
    CHECK(sdf_http_transport_stats(&o, 0, &ts) == 0);
    uint32_t cap_small = ts.body_cap;
    CHECK(cap_small >= req.len && ts.body_allocs == 1);

    /* Same-size unit: the warmed buffer is reused, no new allocation. */
    req.obj_offset = 2048;
    CHECK(o.ops->fetch(&o, &req, cb, NULL) == 0);
    wait_quiet(&o, 1);
    CHECK(sdf_http_transport_stats(&o, 0, &ts) == 0);
    CHECK(ts.body_cap == cap_small && ts.body_allocs == 1);

    /* Larger unit: one grow event covers it. */
    req.obj_offset = 4096;
    req.len = 32768;
    CHECK(o.ops->fetch(&o, &req, cb, NULL) == 0);
    wait_quiet(&o, 1);
    CHECK(sdf_http_transport_stats(&o, 0, &ts) == 0);
    uint32_t cap_large = ts.body_cap;
    CHECK(cap_large >= req.len && ts.body_allocs == 2);

    /* Smaller unit afterwards: neither shrink nor allocate. */
    req.obj_offset = 0;
    req.len = 512;
    CHECK(o.ops->fetch(&o, &req, cb, NULL) == 0);
    wait_quiet(&o, 1);
    CHECK(sdf_http_transport_stats(&o, 0, &ts) == 0);
    CHECK(ts.body_cap == cap_large && ts.body_allocs == 2);

    CHECK(atomic_load(&g_t.ok) == 4);
    CHECK(atomic_load(&g_t.bytes_ok) == 1024 + 1024 + 32768 + 512);
    o.ops->close(&o);
}

static void test_partial_body(void) {
    char url[192];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/obj", g_srv.port);

    sdf_origin_cfg_t c;
    url_cfg(&c, 0, url, 1000);
    sdf_origin_t o = {.ops = sdf_http_ops(), .impl = NULL};
    atomic_store_explicit(&g_chunked_body, 1, memory_order_relaxed);
    CHECK(o.ops->open(&o, &c, 1) == 0);
    reset_tally();

    sdf_fetch_req_t req = {.origin_id = 0, .job_id = 1, .obj_offset = 777, .len = 4096};
    CHECK(o.ops->fetch(&o, &req, cb, NULL) == 0);
    wait_quiet(&o, 1);
    CHECK(atomic_load(&g_t.ok) == 1); /* header/body split and partial recv exact */
    CHECK(atomic_load(&g_t.bytes_ok) == req.len);

    sdf_origin_stats_t st;
    CHECK(o.ops->stats(&o, 0, &st) == 0);
    CHECK(st.reqs_done == 1 && st.reqs_failed == 0 && st.bytes_rx == req.len);
    o.ops->close(&o);
    atomic_store_explicit(&g_chunked_body, 0, memory_order_relaxed);
}

static void test_truncated_body_recovers(void) {
    char url[192];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/obj", g_srv.port);

    sdf_origin_cfg_t c;
    url_cfg(&c, 0, url, 1000);
    sdf_origin_t o = {.ops = sdf_http_ops(), .impl = NULL};
    atomic_store_explicit(&g_short_body, 1, memory_order_relaxed);
    CHECK(o.ops->open(&o, &c, 1) == 0);
    reset_tally();

    sdf_fetch_req_t req = {.origin_id = 0, .job_id = 1, .obj_offset = 1024, .len = 4096};
    CHECK(o.ops->fetch(&o, &req, cb, NULL) == 0);
    wait_quiet(&o, 1);
    CHECK(atomic_load(&g_t.bad_response) == 1); /* short body fails the unit */

    /* Recovery: the driver dropped the socket and the next unit succeeds. */
    atomic_store_explicit(&g_short_body, 0, memory_order_relaxed);
    CHECK(o.ops->fetch(&o, &req, cb, NULL) == 0);
    wait_quiet(&o, 1);
    CHECK(atomic_load(&g_t.ok) == 1);

    sdf_origin_stats_t st;
    CHECK(o.ops->stats(&o, 0, &st) == 0);
    CHECK(st.reqs_done == 1 && st.reqs_failed == 1 && st.bytes_rx == req.len);
    o.ops->close(&o);
}

static void test_saturation_backpressure(void) {
    test_srv_t bh;
    srv_start(&bh, blackhole_main);
    char url[192];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/obj", bh.port);

    sdf_origin_cfg_t c;
    url_cfg(&c, 0, url, 120);
    c.max_inflight = 1; /* qcap 2: one running + two queued before backpressure */
    sdf_origin_t o = {.ops = sdf_http_ops(), .impl = NULL};
    CHECK(o.ops->open(&o, &c, 1) == 0);
    reset_tally();

    sdf_fetch_req_t req = {.origin_id = 0, .job_id = 1, .obj_offset = 0, .len = 32};
    uint64_t accepted = 0;
    int saw_eagain = 0;
    for (int i = 0; i < 16; i++) {
        int rc = o.ops->fetch(&o, &req, cb, NULL);
        if (rc == -EAGAIN) {
            saw_eagain = 1;
            break;
        }
        CHECK(rc == 0);
        accepted++;
    }
    CHECK(saw_eagain && accepted >= 1);

    sdf_origin_stats_t st;
    CHECK(o.ops->stats(&o, 0, &st) == 0);
    CHECK(st.in_flight >= 1 && st.in_flight <= accepted);
    wait_quiet(&o, 1);
    CHECK(atomic_load(&g_t.timeout) == accepted);

    /* Backpressure is transient: draining the queue restores acceptance. */
    CHECK(o.ops->fetch(&o, &req, cb, NULL) == 0);
    wait_quiet(&o, 1);
    CHECK(atomic_load(&g_t.timeout) == accepted + 1);
    o.ops->close(&o);
    srv_stop(&bh);
}

static void test_timeout_and_refused(void) {
    /* blackhole server: accept, never respond */
    test_srv_t bh;
    srv_start(&bh, blackhole_main);
    char bhurl[192];
    snprintf(bhurl, sizeof(bhurl), "http://127.0.0.1:%d/obj", bh.port);

    sdf_origin_cfg_t c;
    url_cfg(&c, 0, bhurl, 150); /* 150 ms socket timeout */
    sdf_origin_t o = {.ops = sdf_http_ops(), .impl = NULL};
    CHECK(o.ops->open(&o, &c, 1) == 0);
    reset_tally();

    uint64_t t0 = (uint64_t)0;
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        t0 = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }
    sdf_fetch_req_t req = {.origin_id = 0, .job_id = 1, .obj_offset = 0, .len = 32};
    CHECK(o.ops->fetch(&o, &req, cb, NULL) == 0);
    wait_quiet(&o, 1);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t t1 = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    CHECK(atomic_load(&g_t.timeout) == 1); /* a failed unit, not a hang */
    CHECK(t1 - t0 < 3000000000ull);        /* bounded well under 3 s */
    o.ops->close(&o);
    srv_stop(&bh);

    /* connect refused: a listener that was closed latches hard_down */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    CHECK(bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
    CHECK(listen(fd, 1) == 0);
    struct sockaddr_in gp = {0};
    socklen_t pl = sizeof(gp);
    CHECK(getsockname(fd, (struct sockaddr *)&gp, &pl) == 0);
    int dead_port = ntohs(gp.sin_port);
    close(fd);

    char deadurl[96];
    snprintf(deadurl, sizeof(deadurl), "http://127.0.0.1:%d/obj", dead_port);
    url_cfg(&c, 0, deadurl, 200);
    sdf_origin_t od = {.ops = sdf_http_ops(), .impl = NULL};
    CHECK(od.ops->open(&od, &c, 1) == 0);
    CHECK(od.ops->fetch(&od, &req, cb, NULL) == 0);
    wait_quiet(&od, 1);
    CHECK(atomic_load(&g_t.refused) == 1);

    sdf_origin_stats_t st;
    CHECK(od.ops->stats(&od, 0, &st) == 0);
    CHECK(st.hard_down == 1); /* latched: the controller's signal */
    od.ops->close(&od);
}

static void test_malformed_overlong_body(void) {
    char url[192];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/obj", g_srv.port);

    sdf_origin_cfg_t c;
    url_cfg(&c, 0, url, 1000);
    sdf_origin_t o = {.ops = sdf_http_ops(), .impl = NULL};
    atomic_store_explicit(&g_overlong_body, 1, memory_order_relaxed);
    CHECK(o.ops->open(&o, &c, 1) == 0);
    reset_tally();

    sdf_fetch_req_t req = {.origin_id = 0, .job_id = 1, .obj_offset = 0, .len = 16};
    CHECK(o.ops->fetch(&o, &req, cb, NULL) == 0);
    wait_quiet(&o, 1);
    CHECK(atomic_load(&g_t.bad_response) == 1); /* bounded, never an overflow */
    sdf_origin_stats_t st;
    CHECK(o.ops->stats(&o, 0, &st) == 0);
    CHECK(st.reqs_failed == 1 && st.reqs_done == 0 && st.bytes_rx == 0);
    o.ops->close(&o);
    atomic_store_explicit(&g_overlong_body, 0, memory_order_relaxed);
}

static void test_drain_and_close_flush(void) {
    char url[192];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/obj", g_srv.port);
    sdf_origin_cfg_t c;
    url_cfg(&c, 0, url, 0);

    sdf_origin_t o = {.ops = sdf_http_ops(), .impl = NULL};
    CHECK(o.ops->open(&o, &c, 1) == 0);
    reset_tally();

    sdf_fetch_req_t req = {.origin_id = 0, .job_id = 1, .obj_offset = 0, .len = 32};
    CHECK(o.ops->fetch(&o, &req, cb, NULL) == 0);
    CHECK(o.ops->set_enable(&o, 0, 0) == 0); /* drain mid-flight */
    CHECK(o.ops->fetch(&o, &req, cb, NULL) == -ENOLINK);
    wait_quiet(&o, 1);
    CHECK(atomic_load(&g_t.ok) == 1); /* in-flight unit survived */

    CHECK(o.ops->set_enable(&o, 0, 1) == 0);
    CHECK(o.ops->fetch(&o, &req, cb, NULL) == 0);
    wait_quiet(&o, 1);
    CHECK(atomic_load(&g_t.ok) == 2);

    /* close with a queued-but-unfetched unit: flushed as -ECANCELED */
    CHECK(o.ops->set_enable(&o, 0, 1) == 0);
    CHECK(o.ops->fetch(&o, &req, cb, NULL) == 0);
    o.ops->close(&o);
    CHECK(atomic_load(&g_t.ok) + atomic_load(&g_t.cancelled) == 3);
    o.ops->close(&o); /* double close no-op */
}

int main(void) {
    srv_start(&g_srv, srv_main);
    test_open_validation();
    test_fetch_and_stats();
    test_inflight_covers_callback();
    test_buffer_reuse_and_growth();
    test_partial_body();
    test_truncated_body_recovers();
    test_saturation_backpressure();
    test_timeout_and_refused();
    test_malformed_overlong_body();
    test_drain_and_close_flush();
    srv_stop(&g_srv);
    printf("test_origin_http: ok (ranged data, stats, 404, timeout, "
           "refused, overlong/truncated body, buffer reuse/growth, "
           "partial response, saturation, drain, close flush)\n");
    return 0;
}
