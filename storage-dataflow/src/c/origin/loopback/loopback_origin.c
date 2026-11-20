/*
 * Loopback origin driver (plan prompt W1.2, ADR-0008, design 09 §4).
 *
 * Serves byte ranges read-only-mapped from local files — the e2e
 * degradation workhorse. Design points:
 *
 *  - ONE worker thread with a deadline scheduler (not a sleeping
 *    worker): a throttled origin must not delay other origins'
 *    completions (the cross-origin head-of-line rule of design 09 §2),
 *    and throttle/jitter are per-origin scheduling, not sleeping.
 *  - Completions are zero-copy: the callback receives a pointer into
 *    the file mapping (driver-owned until the callback returns, held
 *    until close). No allocation per fetch — ever, not just after
 *    warmup.
 *  - Simulated loss/err COMPLETE the unit (-EHOSTUNREACH / -EIO) and
 *    count into reqs_failed: the dispatcher sees the failure and owns
 *    the retry policy (design 01 open questions resolve at W2.4).
 *  - Knobs are atomics: sdf_loopback_set_knob reloads them mid-run.
 *  - Queue is the W0.2 SPSC ring; fetch() is called from one
 *    dispatcher thread (the single-producer contract), the worker is
 *    the single consumer.
 *
 * Conventions imported from wack_a_mole (the netem-harness pattern,
 * import map §3.4): deterministic PRNG so loss/err/jitter profiles are
 * reproducible, and stats exact enough to gate e2e assertions on.
 */

#define _POSIX_C_SOURCE 200809L

#include "sdf/common/ring.h"
#include "sdf/common/timer.h"
#include "sdf/origin.h"
#include "sdf/origin/loopback.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Submit-to-completion EWMA gain (1/8, WAM controller convention). */
#define LB_EWMA_GAIN 0.125

typedef struct {
    sdf_fetch_req_t req;
    sdf_fetch_done_fn cb;
    void *ud;
    uint64_t submit_ns;
    uint64_t deadline_ns;
    int status;
    uint32_t _pad;
} lb_job_t;

typedef struct {
    /* immutable after open */
    sdf_origin_cfg_t cfg;
    uint8_t *map;
    uint64_t map_len;

    /* live knobs (harness control surface) */
    _Atomic uint64_t throttle_ns;
    _Atomic uint64_t jitter_ns;
    _Atomic uint32_t loss_x100;
    _Atomic uint32_t err_x100;
    _Atomic int enabled;

    /* stats: single writer (the worker), atomic for stats() readers */
    _Atomic uint64_t reqs_out;
    _Atomic uint64_t reqs_done;
    _Atomic uint64_t reqs_failed;
    _Atomic uint64_t bytes_tx;
    _Atomic uint64_t bytes_rx;
    _Atomic uint64_t in_flight;
    _Atomic double ewma_lat_ns;
    _Atomic double ewma_err;
    _Atomic int hard_down;
} lb_origin_t;

typedef struct {
    uint32_t n;
    lb_origin_t *origins;
    spsc_ring_t *queue;
    uint8_t *queue_mem;
    uint32_t qcap;
    lb_job_t *pending; /* scheduled, not yet fired */
    uint32_t npending;
    _Atomic int quit;
    uint64_t open_t_ns;
    uint64_t rng; /* worker-thread-only state */
    pthread_t worker;
    int worker_started;
} lb_impl_t;

/* ---- worker ------------------------------------------------------------ */

static uint64_t rng_next(lb_impl_t *d) {
    uint64_t x = d->rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    d->rng = x;
    return x;
}

static void lb_fire(lb_impl_t *d, lb_job_t *j) {
    lb_origin_t *og = &d->origins[j->req.origin_id];
    const uint8_t *out = NULL;
    uint32_t len = j->req.len;
    int status = j->status;

    if (status == 0) {
        if ((uint64_t)j->req.obj_offset + j->req.len > og->map_len) {
            status = -ERANGE; /* range beyond EOF */
            len = 0;
        } else {
            out = og->map + j->req.obj_offset;
        }
    } else {
        len = 0;
    }

    uint64_t now = sdf_now_ns();
    double lat = (double)(now - j->submit_ns);

    if (status == 0) {
        atomic_fetch_add_explicit(&og->reqs_done, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&og->bytes_rx, len, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&og->reqs_failed, 1, memory_order_relaxed);
    }

    double lat_e = atomic_load_explicit(&og->ewma_lat_ns, memory_order_relaxed);
    atomic_store_explicit(&og->ewma_lat_ns, lat_e + LB_EWMA_GAIN * (lat - lat_e),
                          memory_order_relaxed);
    double err_e = atomic_load_explicit(&og->ewma_err, memory_order_relaxed);
    double err_samp = status == 0 ? 0.0 : 1.0;
    atomic_store_explicit(&og->ewma_err, err_e + LB_EWMA_GAIN * (err_samp - err_e),
                          memory_order_relaxed);

    j->cb(j->ud, &j->req, out, len, status);

    /* in_flight covers the whole completion, including the callback.
     * A waiter that observes zero therefore knows the callback has
     * returned and its observable completion effects are visible. */
    atomic_fetch_sub_explicit(&og->in_flight, 1, memory_order_release);
}

/* Pop queued jobs into pending, bounded by the pending capacity;
 * leftovers stay in the ring and are scheduled on the next pass
 * (a saturated queue means -EAGAIN is already the caller's reality). */
static void lb_schedule(lb_impl_t *d) {
    lb_job_t j;
    while (d->npending < d->qcap && spsc_try_pop(d->queue, &j)) {
        lb_origin_t *og = &d->origins[j.req.origin_id];
        uint64_t now = sdf_now_ns();
        j.status = 0;
        j.deadline_ns = now;

        uint32_t loss = atomic_load_explicit(&og->loss_x100, memory_order_relaxed);
        uint32_t err = atomic_load_explicit(&og->err_x100, memory_order_relaxed);
        if (loss > 0 && (uint32_t)(rng_next(d) % 10000u) < loss) {
            j.status = -EHOSTUNREACH; /* simulated transport loss */
        } else if (err > 0 && (uint32_t)(rng_next(d) % 10000u) < err) {
            j.status = -EIO; /* simulated origin error */
        } else {
            uint64_t thr = atomic_load_explicit(&og->throttle_ns, memory_order_relaxed);
            uint64_t jit = atomic_load_explicit(&og->jitter_ns, memory_order_relaxed);
            j.deadline_ns = now + thr + (jit > 0 ? rng_next(d) % (jit + 1) : 0);
        }
        d->pending[d->npending++] = j;
    }
}

static void *lb_worker_main(void *arg) {
    lb_impl_t *d = arg;

    for (;;) {
        lb_schedule(d);

        int quit = atomic_load_explicit(&d->quit, memory_order_relaxed);
        if (quit) {
            /* flush: fire everything scheduled right now so callers
             * are never left hanging (close discipline) */
            for (uint32_t i = 0; i < d->npending; i++) {
                lb_fire(d, &d->pending[i]);
            }
            d->npending = 0;
            if (spsc_count(d->queue) == 0) {
                return NULL;
            }
            continue;
        }

        if (d->npending == 0) {
            sdf_sleep_ns(200000); /* idle poll bound */
            continue;
        }

        uint64_t now = sdf_now_ns();
        uint64_t next = UINT64_MAX;
        uint32_t remaining = 0;
        for (uint32_t i = 0; i < d->npending;) {
            if (d->pending[i].deadline_ns <= now) {
                lb_fire(d, &d->pending[i]);
                d->pending[i] = d->pending[d->npending - 1];
                d->npending--;
                continue; /* re-examine swapped-in slot */
            }
            if (d->pending[i].deadline_ns < next) {
                next = d->pending[i].deadline_ns;
            }
            i++;
            remaining++;
        }
        if (remaining > 0) {
            uint64_t wake = next > now ? next - now : 0;
            if (wake > 200000) {
                wake = 200000; /* bounded sleep: quit/knobs stay live */
            }
            sdf_sleep_ns(wake);
        }
    }
}

/* ---- vtable ------------------------------------------------------------ */

static uint32_t clamp_x100(double pct) {
    if (!(pct > 0.0)) {
        return 0;
    }
    if (pct > 100.0) {
        return 10000u;
    }
    return (uint32_t)(pct * 100.0);
}

static int lb_open(sdf_origin_t *o, const sdf_origin_cfg_t *cfgs, uint32_t n) {
    if (o == NULL || cfgs == NULL || n == 0 || n > SDF_ORIGIN_MAX) {
        return -EINVAL;
    }

    lb_impl_t *d = calloc(1, sizeof(*d));
    if (d == NULL) {
        return -ENOMEM;
    }
    d->n = n;
    d->rng = 0x5EEDBEEFCAFEull; /* fixed seed: reproducible profiles */
    o->impl = d;

    d->origins = calloc(n, sizeof(*d->origins));
    if (d->origins == NULL) {
        goto fail;
    }

    uint64_t inflight = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (cfgs[i].id != i || cfgs[i].dst[0] == '\0' || cfgs[i].cls != SDF_ORIGIN_LOOPBACK) {
            goto fail; /* canonical dense ids and one explicit driver class */
        }
        d->origins[i].cfg = cfgs[i];

        int fd = open(cfgs[i].dst, O_RDONLY);
        if (fd < 0) {
            goto fail; /* dst must exist: open() fails wholesale */
        }
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size <= 0) {
            close(fd);
            goto fail;
        }
        void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (map == MAP_FAILED) {
            goto fail;
        }
        d->origins[i].map = map;
        d->origins[i].map_len = (uint64_t)st.st_size;

        atomic_store(&d->origins[i].throttle_ns, cfgs[i].throttle_ns);
        atomic_store(&d->origins[i].jitter_ns, cfgs[i].lat_jitter_ns);
        atomic_store(&d->origins[i].loss_x100, clamp_x100(cfgs[i].loss_pct));
        atomic_store(&d->origins[i].err_x100, clamp_x100(cfgs[i].err_pct));
        atomic_store(&d->origins[i].enabled, 1);
        atomic_store(&d->origins[i].hard_down, 0);

        inflight += cfgs[i].max_inflight > 0 ? cfgs[i].max_inflight : 16;
    }

    d->qcap = 2u;
    while (d->qcap < inflight && d->qcap < (1u << 16)) {
        d->qcap <<= 1;
    }
    size_t qmem = spsc_sizeof(d->qcap, (uint32_t)sizeof(lb_job_t));
    if (qmem == 0) {
        goto fail;
    }
    d->queue_mem = aligned_alloc(64, (qmem + 63u) & ~63u);
    if (d->queue_mem == NULL) {
        goto fail;
    }
    d->queue = (spsc_ring_t *)d->queue_mem;
    if (spsc_init(d->queue, d->queue_mem, d->qcap, (uint32_t)sizeof(lb_job_t)) != 0) {
        goto fail;
    }

    d->pending = calloc(d->qcap, sizeof(*d->pending));
    if (d->pending == NULL) {
        goto fail;
    }

    d->open_t_ns = sdf_now_ns();
    atomic_store_explicit(&d->quit, 0, memory_order_relaxed);
    if (pthread_create(&d->worker, NULL, lb_worker_main, d) != 0) {
        goto fail;
    }
    d->worker_started = 1;
    return 0;

fail:
    for (uint32_t i = 0; i < n; i++) {
        if (d->origins != NULL && d->origins[i].map != NULL) {
            munmap(d->origins[i].map, d->origins[i].map_len);
        }
    }
    free(d->pending);
    free(d->queue_mem);
    free(d->origins);
    free(d);
    o->impl = NULL;
    return -EINVAL;
}

static int lb_fetch(sdf_origin_t *o, const sdf_fetch_req_t *req, sdf_fetch_done_fn cb, void *ud) {
    if (o == NULL || o->impl == NULL || req == NULL || cb == NULL) {
        return -EINVAL;
    }
    lb_impl_t *d = o->impl;
    if (req->origin_id >= d->n) {
        return -EINVAL;
    }
    if (req->len == 0 || req->len > SDF_ORIGIN_UNIT_MAX) {
        return -EINVAL;
    }

    lb_origin_t *og = &d->origins[req->origin_id];
    if (!atomic_load_explicit(&og->enabled, memory_order_relaxed)) {
        return -ENOLINK;
    }

    lb_job_t j;
    memset(&j, 0, sizeof(j));
    j.req = *req;
    j.cb = cb;
    j.ud = ud;
    j.submit_ns = sdf_now_ns();
    /* Reserve completion visibility before the worker can pop the job. */
    atomic_fetch_add_explicit(&og->in_flight, 1, memory_order_relaxed);
    if (!spsc_try_push(d->queue, &j)) {
        atomic_fetch_sub_explicit(&og->in_flight, 1, memory_order_relaxed);
        return -EAGAIN;
    }

    atomic_fetch_add_explicit(&og->reqs_out, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&og->bytes_tx, req->len, memory_order_relaxed);
    return 0;
}

static int lb_stats(sdf_origin_t *o, uint32_t origin_id, sdf_origin_stats_t *st) {
    if (o == NULL || o->impl == NULL || st == NULL) {
        return -EINVAL;
    }
    lb_impl_t *d = o->impl;
    if (origin_id >= d->n) {
        return -EINVAL;
    }
    lb_origin_t *og = &d->origins[origin_id];

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

static int lb_set_enable(sdf_origin_t *o, uint32_t origin_id, int up) {
    if (o == NULL || o->impl == NULL) {
        return -EINVAL;
    }
    lb_impl_t *d = o->impl;
    if (origin_id >= d->n) {
        return -EINVAL;
    }
    atomic_store_explicit(&d->origins[origin_id].enabled, up ? 1 : 0, memory_order_relaxed);
    /* a drained origin is deterministically controller-visible: the
     * severity model reads hard_down, not warm-up latency luck
     * (found live: the mid-run redistribution whack was flaking on
     * whether the frozen EWMA sat above or below the healthy median) */
    atomic_store_explicit(&d->origins[origin_id].hard_down, up ? 0 : 1, memory_order_relaxed);
    return 0;
}

static void lb_close(sdf_origin_t *o) {
    if (o == NULL || o->impl == NULL) {
        return;
    }
    lb_impl_t *d = o->impl;
    atomic_store_explicit(&d->quit, 1, memory_order_relaxed);
    if (d->worker_started) {
        pthread_join(d->worker, NULL);
    }
    for (uint32_t i = 0; i < d->n; i++) {
        if (d->origins[i].map != NULL) {
            munmap(d->origins[i].map, d->origins[i].map_len);
        }
    }
    free(d->pending);
    free(d->queue_mem);
    free(d->origins);
    free(d);
    o->impl = NULL;
}

static const sdf_origin_ops_t LB_OPS = {
    .open = lb_open,
    .fetch = lb_fetch,
    .stats = lb_stats,
    .set_enable = lb_set_enable,
    .close = lb_close,
};

/* Compile-time link rule: the public name is a const vtable. */
const sdf_origin_ops_t *sdf_loopback_ops(void) {
    return &LB_OPS;
}

/* ---- harness control surface ------------------------------------------- */

int sdf_loopback_set_knob(sdf_origin_t *o, uint32_t origin_id, sdf_loopback_knob_t knob,
                          uint64_t value) {
    if (o == NULL || o->impl == NULL || o->ops != &LB_OPS) {
        return -EINVAL;
    }
    lb_impl_t *d = o->impl;
    if (origin_id >= d->n) {
        return -EINVAL;
    }
    lb_origin_t *og = &d->origins[origin_id];

    switch (knob) {
    case SDF_LB_THROTTLE_NS:
        atomic_store_explicit(&og->throttle_ns, value, memory_order_relaxed);
        return 0;
    case SDF_LB_JITTER_NS:
        atomic_store_explicit(&og->jitter_ns, value, memory_order_relaxed);
        return 0;
    case SDF_LB_LOSS_PCT_X100:
    case SDF_LB_ERR_PCT_X100:
        if (value > 10000u) {
            return -ERANGE;
        }
        atomic_store_explicit(knob == SDF_LB_LOSS_PCT_X100 ? &og->loss_x100 : &og->err_x100,
                              (uint32_t)value, memory_order_relaxed);
        return 0;
    default:
        return -EINVAL;
    }
}
