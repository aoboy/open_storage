/*
 * HTTP origin driver (plan prompt W1.3, ADR-0008, design 09 §4).
 *
 * Blocking per-origin workers behind the same vtable: real network
 * I/O cannot share one thread across origins without serializing
 * them (the head-of-line rule of design 09 §2). Each origin owns an
 * SPSC queue (the W0.2 ring, single-producer contract) and a worker
 * doing connect / Range-GET / recv with socket timeouts from
 * cfg.timeout_ms. The connection persists across units and is
 * reconnected on any error; one attempt per unit — retry is
 * dispatcher policy, exactly like the loopback driver.
 *
 * Response mapping: 200/206 -> success (Content-Length body);
 * 404 -> -ENOENT; 416 -> -ERANGE; other non-2xx -> -EIO; socket
 * errors -> -ECONNREFUSED (hard_down latched until a connect
 * succeeds) / -ETIMEDOUT. Bodies are malloc'd per unit and freed
 * after the callback returns — documented: this is the reference
 * smoke leg, the hot-path gates run on loopback (risk S8).
 */

#define _POSIX_C_SOURCE 200809L

#include "sdf/common/ring.h"
#include "sdf/common/timer.h"
#include "sdf/origin.h"
#include "sdf/origin/http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define HTTP_EWMA_GAIN 0.125
#define HTTP_HDR_MAX 8192u
#define HTTP_DEFAULT_TIMEOUT_MS 2000u
#define HTTP_HOST_MAX 128u

typedef struct {
    sdf_fetch_req_t req;
    sdf_fetch_done_fn cb;
    void *ud;
    uint64_t submit_ns;
} http_job_t;

typedef struct http_impl http_impl_t;

typedef struct {
    /* immutable after open */
    sdf_origin_cfg_t cfg;
    char host[HTTP_HOST_MAX];
    char path[SDF_ORIGIN_DST_MAX];
    int port;

    /* per-origin queue + worker */
    spsc_ring_t *queue;
    uint8_t *qmem;
    uint32_t qcap;
    pthread_t worker;
    int worker_started;

    _Atomic int enabled;

    /* stats: single writer (this origin's worker) */
    _Atomic uint64_t reqs_out;
    _Atomic uint64_t reqs_done;
    _Atomic uint64_t reqs_failed;
    _Atomic uint64_t bytes_tx;
    _Atomic uint64_t bytes_rx;
    _Atomic uint64_t in_flight;
    _Atomic double ewma_lat_ns;
    _Atomic double ewma_err;
    _Atomic int hard_down;

    struct http_impl *owner; /* quit flag + open timestamp */

    /* worker-thread-only */
    int sock; /* -1 = not connected */
} http_origin_t;

struct http_impl {
    uint32_t n;
    http_origin_t *origins;
    _Atomic int quit;
    uint64_t open_t_ns;
};

/* ---- helpers ----------------------------------------------------------- */

/* Parse "http://host[:port]/path" into host/port/path. */
static int url_parse(const char *url, char *host, size_t host_sz, char *path, size_t path_sz,
                     int *port) {
    if (url == NULL || strncmp(url, "http://", 7) != 0) {
        return -EINVAL;
    }
    const char *p = url + 7;
    const char *slash = strchr(p, '/');
    size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
    if (hlen == 0 || hlen >= host_sz) {
        return -EINVAL;
    }
    memcpy(host, p, hlen);
    host[hlen] = '\0';
    *port = 80;
    char *colon = strchr(host, ':');
    if (colon != NULL) {
        *colon = '\0';
        long v = strtol(colon + 1, NULL, 10);
        if (v <= 0 || v > 65535) {
            return -EINVAL;
        }
        *port = (int)v;
        if (colon == host) {
            return -EINVAL;
        }
    }
    if (slash == NULL) {
        path[0] = '/';
        path[1] = '\0';
    } else {
        size_t plen = strlen(slash);
        if (plen == 0 || plen >= path_sz) {
            return -EINVAL;
        }
        memcpy(path, slash, plen + 1);
    }
    return 0;
}

static void complete(http_origin_t *og, const http_job_t *j, const uint8_t *out, uint32_t len,
                     int status) {
    uint64_t now = sdf_now_ns();
    double lat = (double)(now - j->submit_ns);

    if (status == 0) {
        atomic_fetch_add_explicit(&og->reqs_done, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&og->bytes_rx, len, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&og->reqs_failed, 1, memory_order_relaxed);
    }
    double lat_e = atomic_load_explicit(&og->ewma_lat_ns, memory_order_relaxed);
    atomic_store_explicit(&og->ewma_lat_ns, lat_e + HTTP_EWMA_GAIN * (lat - lat_e),
                          memory_order_relaxed);
    double err_e = atomic_load_explicit(&og->ewma_err, memory_order_relaxed);
    atomic_store_explicit(&og->ewma_err,
                          err_e + HTTP_EWMA_GAIN * ((status == 0 ? 0.0 : 1.0) - err_e),
                          memory_order_relaxed);
    j->cb(j->ud, &j->req, out, len, status);

    /* in_flight covers the whole completion, including the callback.
     * A waiter that observes zero therefore knows the callback has
     * returned and its observable completion effects are visible. */
    atomic_fetch_sub_explicit(&og->in_flight, 1, memory_order_release);
}

/* Connect og->sock (or leave -1). Returns 0 / -errno. */
static int ensure_sock(http_origin_t *og) {
    if (og->sock >= 0) {
        return 0;
    }

    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", og->port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(og->host, portstr, &hints, &res) != 0 || res == NULL) {
        return -ECONNREFUSED;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -ECONNREFUSED;
    }
    uint32_t tmo = og->cfg.timeout_ms ? og->cfg.timeout_ms : HTTP_DEFAULT_TIMEOUT_MS;
    struct timeval tv = {.tv_sec = tmo / 1000u, .tv_usec = (long)(tmo % 1000u) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        close(fd);
        return -ECONNREFUSED;
    }
    freeaddrinfo(res);
    og->sock = fd;
    atomic_store_explicit(&og->hard_down, 0, memory_order_relaxed);
    return 0;
}

static void drop_sock(http_origin_t *og) {
    if (og->sock >= 0) {
        close(og->sock);
        og->sock = -1;
    }
}

/* Case-insensitive header lookup in [hdr_start, hdr_end). */
static int64_t header_value(const char *hdr, const char *name) {
    size_t nlen = strlen(name);
    const char *p = hdr;
    while ((p = strstr(p, name)) != NULL) {
        int at_line_start = (p == hdr || p[-1] == '\n');
        if (at_line_start && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (*v == ' ') {
                v++;
            }
            return (int64_t)strtoll(v, NULL, 10);
        }
        p += nlen;
    }
    return -1;
}

/* One attempt: returns 0 / negative errno. On success *out and *out_len
 * carry the malloc'd body (caller frees). On any socket error the
 * connection drops and the next unit reconnects. */
static int do_fetch(http_origin_t *og, const http_job_t *j, uint8_t **out, uint32_t *out_len) {
    *out = NULL;
    *out_len = 0;

    int rc = ensure_sock(og);
    if (rc != 0) {
        atomic_store_explicit(&og->hard_down, 1, memory_order_relaxed);
        return rc;
    }

    char req[HTTP_HDR_MAX];
    int rlen = snprintf(req, sizeof(req),
                        "GET %s HTTP/1.1\r\n"
                        "Host: %s\r\n"
                        "Range: bytes=%llu-%llu\r\n"
                        "Connection: keep-alive\r\n"
                        "\r\n",
                        og->path, og->host, (unsigned long long)j->req.obj_offset,
                        (unsigned long long)(j->req.obj_offset + j->req.len - 1));
    if (rlen <= 0 || (size_t)rlen >= sizeof(req)) {
        return -EINVAL;
    }

    if (send(og->sock, req, (size_t)rlen, 0) != (ssize_t)rlen) {
        drop_sock(og);
        rc = ensure_sock(og); /* one reconnect attempt per unit */
        if (rc != 0) {
            atomic_store_explicit(&og->hard_down, 1, memory_order_relaxed);
            return rc;
        }
        if (send(og->sock, req, (size_t)rlen, 0) != (ssize_t)rlen) {
            drop_sock(og);
            return -EIO;
        }
    }

    /* read the header block */
    char hdr[HTTP_HDR_MAX];
    size_t got = 0;
    for (;;) {
        if (got >= sizeof(hdr) - 1) {
            drop_sock(og);
            return -EIO;
        }
        ssize_t n = recv(og->sock, hdr + got, sizeof(hdr) - 1 - got, 0);
        if (n < 0) {
            drop_sock(og);
            return -ETIMEDOUT;
        }
        if (n == 0) {
            drop_sock(og);
            return -EIO;
        }
        got += (size_t)n;
        hdr[got] = '\0';
        char *sep = strstr(hdr, "\r\n\r\n");
        if (sep != NULL) {
            size_t hdr_len = (size_t)(sep - hdr) + 4;
            /* keep any body bytes that arrived with the header */
            size_t body_pre = got - hdr_len;
            int code = 0;
            if (sscanf(hdr, "HTTP/1.%*d %d", &code) != 1) {
                code = 0;
            }

            if (code != 200 && code != 206) {
                drop_sock(og); /* error pages may close; reconnect is cheap */
                if (code == 404) {
                    return -ENOENT;
                }
                if (code == 416) {
                    return -ERANGE;
                }
                return -EIO;
            }

            int64_t clen = header_value(hdr, "Content-Length");
            if (clen < 0 || (uint64_t)clen != j->req.len) {
                /* server disagrees about the body: unacceptable for a
                 * range unit (the loopback driver is byte-exact; the
                 * reference driver must be too) */
                drop_sock(og);
                return -EIO;
            }
            uint8_t *body = malloc(j->req.len);
            if (body == NULL) {
                return -ENOMEM;
            }
            memcpy(body, sep + 4, body_pre);
            size_t need = j->req.len - body_pre;
            size_t off = body_pre;
            while (need > 0) {
                ssize_t n2 = recv(og->sock, body + off, need, 0);
                if (n2 < 0) {
                    free(body);
                    drop_sock(og);
                    return -ETIMEDOUT;
                }
                if (n2 == 0) {
                    free(body);
                    drop_sock(og);
                    return -EIO;
                }
                off += (size_t)n2;
                need -= (size_t)n2;
            }
            *out = body;
            *out_len = j->req.len;
            return 0;
        }
    }
}

static void *worker_main(void *arg) {
    http_origin_t *og = arg;
    for (;;) {
        http_job_t j;
        while (spsc_try_pop(og->queue, &j)) {
            if (atomic_load_explicit(&og->owner->quit, memory_order_relaxed)) {
                /* flush discipline: never leave callers hanging */
                complete(og, &j, NULL, 0, -ECANCELED);
                continue;
            }
            uint8_t *out = NULL;
            uint32_t len = 0;
            int status = do_fetch(og, &j, &out, &len);
            complete(og, &j, out, len, status);
            free(out);
        }
        if (atomic_load_explicit(&og->owner->quit, memory_order_relaxed) &&
            spsc_count(og->queue) == 0) {
            return NULL;
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 200000};
        nanosleep(&ts, NULL);
    }
}

/* ---- vtable ------------------------------------------------------------ */

static int http_open(sdf_origin_t *o, const sdf_origin_cfg_t *cfgs, uint32_t n) {
    if (o == NULL || cfgs == NULL || n == 0 || n > SDF_ORIGIN_MAX) {
        return -EINVAL;
    }

    http_impl_t *d = calloc(1, sizeof(*d));
    if (d == NULL) {
        return -ENOMEM;
    }
    d->n = n;
    atomic_store_explicit(&d->quit, 0, memory_order_relaxed);
    o->impl = d;

    d->origins = calloc(n, sizeof(*d->origins));
    if (d->origins == NULL) {
        goto fail;
    }

    uint64_t inflight = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (cfgs[i].id != i || cfgs[i].dst[0] == '\0') {
            goto fail;
        }
        http_origin_t *og = &d->origins[i];
        og->cfg = cfgs[i];
        og->owner = d;
        og->sock = -1;
        atomic_store(&og->enabled, 1);
        atomic_store(&og->hard_down, 0);

        if (url_parse(cfgs[i].dst, og->host, sizeof(og->host), og->path, sizeof(og->path),
                      &og->port) != 0) {
            goto fail;
        }

        uint32_t per = cfgs[i].max_inflight > 0 ? cfgs[i].max_inflight : 16;
        og->qcap = 2u;
        while (og->qcap < per && og->qcap < (1u << 16)) {
            og->qcap <<= 1;
        }
        size_t qmem = spsc_sizeof(og->qcap, (uint32_t)sizeof(http_job_t));
        if (qmem == 0) {
            goto fail;
        }
        og->qmem = aligned_alloc(64, (qmem + 63u) & ~63u);
        if (og->qmem == NULL) {
            goto fail;
        }
        og->queue = (spsc_ring_t *)og->qmem;
        if (spsc_init(og->queue, og->qmem, og->qcap, (uint32_t)sizeof(http_job_t)) != 0) {
            goto fail;
        }

        inflight += per;
    }
    (void)inflight;

    d->open_t_ns = sdf_now_ns();
    for (uint32_t i = 0; i < n; i++) {
        if (pthread_create(&d->origins[i].worker, NULL, worker_main, &d->origins[i]) != 0) {
            goto fail_threads;
        }
        d->origins[i].worker_started = 1;
    }
    return 0;

fail_threads:
    atomic_store_explicit(&d->quit, 1, memory_order_relaxed);
    for (uint32_t i = 0; i < n; i++) {
        if (d->origins[i].worker_started) {
            pthread_join(d->origins[i].worker, NULL);
        }
    }
fail:
    for (uint32_t i = 0; i < n; i++) {
        if (d->origins != NULL && d->origins[i].qmem != NULL) {
            free(d->origins[i].qmem);
        }
    }
    free(d->origins);
    free(d);
    o->impl = NULL;
    return -EINVAL;
}

static int http_fetch(sdf_origin_t *o, const sdf_fetch_req_t *req, sdf_fetch_done_fn cb, void *ud) {
    if (o == NULL || o->impl == NULL || req == NULL || cb == NULL) {
        return -EINVAL;
    }
    http_impl_t *d = o->impl;
    if (req->origin_id >= d->n) {
        return -EINVAL;
    }
    if (req->len == 0 || req->len > SDF_ORIGIN_UNIT_MAX) {
        return -EINVAL;
    }
    http_origin_t *og = &d->origins[req->origin_id];
    if (!atomic_load_explicit(&og->enabled, memory_order_relaxed)) {
        return -ENOLINK;
    }

    http_job_t j;
    memset(&j, 0, sizeof(j));
    j.req = *req;
    j.cb = cb;
    j.ud = ud;
    j.submit_ns = sdf_now_ns();
    if (!spsc_try_push(og->queue, &j)) {
        return -EAGAIN;
    }

    atomic_fetch_add_explicit(&og->reqs_out, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&og->bytes_tx, req->len, memory_order_relaxed);
    atomic_fetch_add_explicit(&og->in_flight, 1, memory_order_relaxed);
    return 0;
}

static int http_stats(sdf_origin_t *o, uint32_t origin_id, sdf_origin_stats_t *st) {
    if (o == NULL || o->impl == NULL || st == NULL) {
        return -EINVAL;
    }
    http_impl_t *d = o->impl;
    if (origin_id >= d->n) {
        return -EINVAL;
    }
    http_origin_t *og = &d->origins[origin_id];

    memset(st, 0, sizeof(*st));
    st->id = og->cfg.id;
    st->cls = og->cfg.cls;
    /* Acquire the completion boundary before reading its counters. */
    st->in_flight = atomic_load_explicit(&og->in_flight, memory_order_acquire);
    st->reqs_out = atomic_load(&og->reqs_out);
    st->reqs_done = atomic_load(&og->reqs_done);
    st->reqs_failed = atomic_load(&og->reqs_failed);
    st->bytes_tx = atomic_load(&og->bytes_tx);
    st->bytes_rx = atomic_load(&og->bytes_rx);
    st->ewma_lat_ns = atomic_load(&og->ewma_lat_ns);
    st->ewma_err = atomic_load(&og->ewma_err);
    st->hard_down = atomic_load(&og->hard_down);

    uint64_t elapsed = sdf_now_ns() - d->open_t_ns;
    if (elapsed > 0) {
        st->goodput = (double)st->bytes_rx / ((double)elapsed / 1000000000.0);
    }
    return 0;
}

static int http_set_enable(sdf_origin_t *o, uint32_t origin_id, int up) {
    if (o == NULL || o->impl == NULL) {
        return -EINVAL;
    }
    http_impl_t *d = o->impl;
    if (origin_id >= d->n) {
        return -EINVAL;
    }
    atomic_store_explicit(&d->origins[origin_id].enabled, up ? 1 : 0, memory_order_relaxed);
    /* drained == controller-visible (same semantics as loopback) */
    atomic_store_explicit(&d->origins[origin_id].hard_down, up ? 0 : 1, memory_order_relaxed);
    return 0;
}

static void http_close(sdf_origin_t *o) {
    if (o == NULL || o->impl == NULL) {
        return;
    }
    http_impl_t *d = o->impl;
    atomic_store_explicit(&d->quit, 1, memory_order_relaxed);
    for (uint32_t i = 0; i < d->n; i++) {
        if (d->origins[i].worker_started) {
            pthread_join(d->origins[i].worker, NULL);
        }
        drop_sock(&d->origins[i]);
        free(d->origins[i].qmem);
    }
    free(d->origins);
    free(d);
    o->impl = NULL;
}

static const sdf_origin_ops_t HTTP_OPS = {
    .open = http_open,
    .fetch = http_fetch,
    .stats = http_stats,
    .set_enable = http_set_enable,
    .close = http_close,
};

const sdf_origin_ops_t *sdf_http_ops(void) {
    return &HTTP_OPS;
}
