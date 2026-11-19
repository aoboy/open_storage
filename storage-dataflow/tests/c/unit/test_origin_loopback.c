/*
 * Loopback origin driver tests (plan prompt W1.2).
 *
 * Pins the ADR-0008 seam contract on the harness driver: data
 * correctness (zero-copy pointers into the mapping), per-origin
 * stats exactness under every degradation knob, live knob reload
 * mid-run, drain/restore without losing in-flight units, submit-time
 * rejections (-EINVAL/-ENOLINK/-EAGAIN), and close flushing pending
 * completions. The driver's own workers are the completion context;
 * callbacks use atomics to tally from that context.
 */

#include "sdf/common/timer.h"
#include "sdf/origin.h"
#include "sdf/origin/loopback.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define FILE_BYTES (128u * 1024u)
#define WAIT_NS 200000
#define WAIT_MAX_MS 4000

static uint8_t g_file[FILE_BYTES];
static char g_path[2][160];

typedef struct {
    _Atomic uint64_t done_ok;
    _Atomic uint64_t done_loss;
    _Atomic uint64_t done_err;
    _Atomic uint64_t done_range;
    _Atomic uint64_t bytes_ok;
    _Atomic uint64_t min_lat_ns; /* smallest submit-to-cb latency seen */
} tally_t;

static tally_t g_tally;

static void tally_reset(void) {
    atomic_store(&g_tally.done_ok, 0);
    atomic_store(&g_tally.done_loss, 0);
    atomic_store(&g_tally.done_err, 0);
    atomic_store(&g_tally.done_range, 0);
    atomic_store(&g_tally.bytes_ok, 0);
    atomic_store(&g_tally.min_lat_ns, UINT64_MAX);
}

static void test_cb(void *ud, const sdf_fetch_req_t *req, const uint8_t *out, uint32_t len,
                    int status) {
    (void)ud;
    tally_t *t = &g_tally;

    if (status == 0) {
        atomic_fetch_add(&t->done_ok, 1);
        atomic_fetch_add(&t->bytes_ok, len);
        CHECK(out != NULL);
        for (uint32_t i = 0; i < len; i++) {
            CHECK(out[i] == g_file[req->obj_offset + i]);
        }
    } else if (status == -EHOSTUNREACH) {
        atomic_fetch_add(&t->done_loss, 1);
        CHECK(out == NULL && len == 0);
    } else if (status == -EIO) {
        atomic_fetch_add(&t->done_err, 1);
        CHECK(out == NULL && len == 0);
    } else if (status == -ERANGE) {
        atomic_fetch_add(&t->done_range, 1);
        CHECK(out == NULL && len == 0);
    } else {
        fprintf(stderr, "FAIL: unexpected cb status %d\n", status);
        exit(1);
    }
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void wait_quiet(sdf_origin_t *o, uint32_t n) {
    for (int ms = 0; ms < WAIT_MAX_MS; ms++) {
        uint64_t inflight = 0;
        for (uint32_t i = 0; i < n; i++) {
            sdf_origin_stats_t st;
            CHECK(o->ops->stats(o, i, &st) == 0);
            inflight += st.in_flight;
        }
        if (inflight == 0) {
            return;
        }
        sdf_sleep_ns(WAIT_NS);
    }
    fprintf(stderr, "FAIL: origins never went quiet\n");
    exit(1);
}

static void make_files(void) {
    for (size_t i = 0; i < sizeof(g_file); i++) {
        g_file[i] = (uint8_t)(i * 7u + 3u);
    }
    for (int k = 0; k < 2; k++) {
        snprintf(g_path[k], sizeof(g_path[k]), "/tmp/sdf-lb-test-%d-%d.bin", (int)getpid(), k);
        FILE *f = fopen(g_path[k], "wb");
        CHECK(f != NULL);
        CHECK(fwrite(g_file, 1, sizeof(g_file), f) == sizeof(g_file));
        CHECK(fclose(f) == 0);
    }
}

static void rm_files(void) {
    for (int k = 0; k < 2; k++) {
        CHECK(unlink(g_path[k]) == 0);
    }
}

static void fill_cfg(sdf_origin_cfg_t *c, uint32_t id) {
    memset(c, 0, sizeof(*c));
    c->id = id;
    c->cls = SDF_ORIGIN_LOOPBACK;
    snprintf(c->dst, sizeof(c->dst), "%s", g_path[id]);
}

static void test_open_validation(void) {
    sdf_origin_cfg_t cfg[2];
    fill_cfg(&cfg[0], 0);
    fill_cfg(&cfg[1], 1);

    sdf_origin_t o = {.ops = sdf_loopback_ops(), .impl = NULL};

    CHECK(o.ops->open(NULL, cfg, 1) == -EINVAL);
    CHECK(o.ops->open(&o, NULL, 1) == -EINVAL);
    CHECK(o.ops->open(&o, cfg, 0) == -EINVAL);
    CHECK(o.ops->open(&o, cfg, SDF_ORIGIN_MAX + 1) == -EINVAL);

    sdf_origin_cfg_t bad_id = cfg[0];
    bad_id.id = 7; /* non-canonical: ids must equal the slot index */
    CHECK(o.ops->open(&o, &bad_id, 1) == -EINVAL);

    sdf_origin_cfg_t bad_dst = cfg[0];
    bad_dst.dst[0] = '\0';
    CHECK(o.ops->open(&o, &bad_dst, 1) == -EINVAL);

    snprintf(bad_dst.dst, sizeof(bad_dst.dst), "/tmp/sdf-lb-no-such-%d", (int)getpid());
    CHECK(o.ops->open(&o, &bad_dst, 1) == -EINVAL);
    CHECK(o.impl == NULL); /* failed open leaves no state */
}

static void test_data_and_stats(void) {
    sdf_origin_cfg_t cfg[2];
    fill_cfg(&cfg[0], 0);
    fill_cfg(&cfg[1], 1);

    sdf_origin_t o = {.ops = sdf_loopback_ops(), .impl = NULL};
    CHECK(o.ops->open(&o, cfg, 2) == 0);
    tally_reset();

    /* basic fetch: data correct through the zero-copy pointer */
    sdf_fetch_req_t req = {.origin_id = 0, .job_id = 1, .obj_offset = 4096, .len = 1024};
    CHECK(o.ops->fetch(&o, &req, test_cb, NULL) == 0);
    req.origin_id = 1;
    req.obj_offset = FILE_BYTES - 64;
    req.len = 64;
    CHECK(o.ops->fetch(&o, &req, test_cb, NULL) == 0);

    /* submit-time rejections */
    req.origin_id = 5;
    CHECK(o.ops->fetch(&o, &req, test_cb, NULL) == -EINVAL);
    req.origin_id = 0;
    req.len = 0;
    CHECK(o.ops->fetch(&o, &req, test_cb, NULL) == -EINVAL);
    req.len = SDF_ORIGIN_UNIT_MAX + 1;
    CHECK(o.ops->fetch(&o, &req, test_cb, NULL) == -EINVAL);
    req.len = 64;
    CHECK(o.ops->fetch(&o, &req, NULL, NULL) == -EINVAL); /* no callback */

    /* range beyond EOF: completed with -ERANGE */
    req.obj_offset = FILE_BYTES - 8;
    req.len = 64;
    CHECK(o.ops->fetch(&o, &req, test_cb, NULL) == 0);

    wait_quiet(&o, 2);
    CHECK(atomic_load(&g_tally.done_ok) == 2);
    CHECK(atomic_load(&g_tally.done_range) == 1);
    CHECK(atomic_load(&g_tally.bytes_ok) == 1024 + 64);

    sdf_origin_stats_t st;
    CHECK(o.ops->stats(&o, 0, &st) == 0);
    CHECK(st.id == 0 && st.cls == SDF_ORIGIN_LOOPBACK);
    CHECK(st.reqs_out == 2); /* one ok + one out-of-range, both to id 0 */
    CHECK(st.reqs_done == 1);
    CHECK(st.reqs_failed == 1);
    CHECK(st.bytes_tx == 1024 + 64);
    CHECK(st.bytes_rx == 1024); /* the range unit delivered nothing */
    CHECK(st.in_flight == 0);
    CHECK(st.hard_down == 0);
    CHECK(st.ewma_lat_ns > 0.0);
    CHECK(st.goodput > 0.0);
    CHECK(o.ops->stats(&o, 2, &st) == -EINVAL); /* bad id */

    /* stats snapshots are plain copies; mutating them is safe */
    st.reqs_out = 12345;
    sdf_origin_stats_t st2;
    CHECK(o.ops->stats(&o, 0, &st2) == 0);
    CHECK(st2.reqs_out == 2);

    o.ops->close(&o);
    CHECK(o.impl == NULL);
}

static void test_throttle_latency(void) {
    sdf_origin_cfg_t cfg;
    fill_cfg(&cfg, 0);
    cfg.throttle_ns = 3000000; /* 3 ms per unit */

    sdf_origin_t o = {.ops = sdf_loopback_ops(), .impl = NULL};
    CHECK(o.ops->open(&o, &cfg, 1) == 0);
    tally_reset();

    uint64_t submit[8];
    for (int i = 0; i < 8; i++) {
        sdf_fetch_req_t req = {
            .origin_id = 0, .job_id = 1, .obj_offset = (uint64_t)i * 100, .len = 100};
        submit[i] = now_ns();
        CHECK(o.ops->fetch(&o, &req, test_cb, NULL) == 0);
    }
    wait_quiet(&o, 1);

    /* every unit observed at least the throttle after its submit */
    uint64_t done_at[8];
    for (int i = 0; i < 8; i++) {
        done_at[i] = now_ns(); /* upper bound: completion <= this read */
    }
    for (int i = 0; i < 8; i++) {
        CHECK(done_at[i] >= submit[i] + 2800000); /* 3ms - slack */
    }
    CHECK(atomic_load(&g_tally.done_ok) == 8);

    sdf_origin_stats_t st;
    CHECK(o.ops->stats(&o, 0, &st) == 0);
    /*
     * EWMA gain is 1/8: after 8 samples of >= 3 ms the EWMA is at least
     * 2.8 ms * (1 - (7/8)^8) = ~1.82 ms. Pin the gain's math, then run
     * the scheduler longer (all units fire ~3 ms after submit — the
     * deadline scheduler does not serialize) and assert convergence.
     */
    CHECK(st.ewma_lat_ns >= 1800000.0);

    for (int i = 0; i < 64; i++) {
        sdf_fetch_req_t req = {
            .origin_id = 0, .job_id = 2, .obj_offset = (uint64_t)i * 100, .len = 100};
        /* fast submits saturate the queue: -EAGAIN is the documented
         * contract and retry is caller policy (W2.5's proxy does this) */
        int rc;
        do {
            rc = o.ops->fetch(&o, &req, test_cb, NULL);
            if (rc == -EAGAIN) {
                sdf_sleep_ns(100000);
            }
        } while (rc == -EAGAIN);
        CHECK(rc == 0);
    }
    wait_quiet(&o, 1);
    CHECK(o.ops->stats(&o, 0, &st) == 0);
    CHECK(st.ewma_lat_ns >= 2700000.0); /* converged toward the 3 ms band */
    o.ops->close(&o);
}

static void test_loss_err_stats(void) {
    sdf_origin_cfg_t cfg;
    fill_cfg(&cfg, 0);
    cfg.loss_pct = 100.0;
    cfg.err_pct = 0.0;

    sdf_origin_t o = {.ops = sdf_loopback_ops(), .impl = NULL};
    CHECK(o.ops->open(&o, &cfg, 1) == 0);
    tally_reset();

    for (int i = 0; i < 10; i++) {
        sdf_fetch_req_t req = {
            .origin_id = 0, .job_id = 1, .obj_offset = (uint64_t)i * 10, .len = 10};
        CHECK(o.ops->fetch(&o, &req, test_cb, NULL) == 0);
    }
    wait_quiet(&o, 1);
    CHECK(atomic_load(&g_tally.done_loss) == 10);
    CHECK(atomic_load(&g_tally.done_ok) == 0);

    sdf_origin_stats_t st;
    CHECK(o.ops->stats(&o, 0, &st) == 0);
    CHECK(st.reqs_failed == 10 && st.reqs_done == 0 && st.in_flight == 0);
    /* gain 1/8 after 10 all-failed completions: 1 - (7/8)^10 ~= 0.739 */
    CHECK(st.ewma_err >= 0.73 && st.ewma_err <= 1.0);
    CHECK(st.bytes_rx == 0);

    /* live reload: err replaces loss without restart */
    CHECK(sdf_loopback_set_knob(&o, 0, SDF_LB_LOSS_PCT_X100, 0) == 0);
    CHECK(sdf_loopback_set_knob(&o, 0, SDF_LB_ERR_PCT_X100, 10000) == 0);
    for (int i = 0; i < 10; i++) {
        sdf_fetch_req_t req = {
            .origin_id = 0, .job_id = 1, .obj_offset = (uint64_t)i * 10, .len = 10};
        CHECK(o.ops->fetch(&o, &req, test_cb, NULL) == 0);
    }
    wait_quiet(&o, 1);
    CHECK(atomic_load(&g_tally.done_err) == 10);
    CHECK(atomic_load(&g_tally.done_loss) == 10); /* unchanged from leg 1 */

    /* knob bounds enforced */
    CHECK(sdf_loopback_set_knob(&o, 0, SDF_LB_LOSS_PCT_X100, 10001) == -ERANGE);
    CHECK(sdf_loopback_set_knob(&o, 9, SDF_LB_THROTTLE_NS, 1) == -EINVAL);
    CHECK(sdf_loopback_set_knob(&o, 0, (sdf_loopback_knob_t)99, 1) == -EINVAL);

    o.ops->close(&o);
}

static void test_drain_and_restore(void) {
    sdf_origin_cfg_t cfg;
    fill_cfg(&cfg, 0);
    cfg.throttle_ns = 5000000; /* 5 ms: in-flight window is wide */

    sdf_origin_t o = {.ops = sdf_loopback_ops(), .impl = NULL};
    CHECK(o.ops->open(&o, &cfg, 1) == 0);
    tally_reset();

    for (int i = 0; i < 10; i++) {
        sdf_fetch_req_t req = {
            .origin_id = 0, .job_id = 1, .obj_offset = (uint64_t)i * 10, .len = 10};
        CHECK(o.ops->fetch(&o, &req, test_cb, NULL) == 0);
    }
    /* drain: new fetches fail fast, the 10 in flight complete fine */
    CHECK(o.ops->set_enable(&o, 0, 0) == 0);
    sdf_fetch_req_t req = {.origin_id = 0, .job_id = 1, .obj_offset = 0, .len = 10};
    CHECK(o.ops->fetch(&o, &req, test_cb, NULL) == -ENOLINK);
    wait_quiet(&o, 1);
    CHECK(atomic_load(&g_tally.done_ok) == 10); /* none lost to the drain */

    /* restore: submissions work again */
    CHECK(o.ops->set_enable(&o, 0, 1) == 0);
    CHECK(o.ops->fetch(&o, &req, test_cb, NULL) == 0);
    wait_quiet(&o, 1);
    CHECK(atomic_load(&g_tally.done_ok) == 11);

    CHECK(o.ops->set_enable(&o, 5, 1) == -EINVAL);
    o.ops->close(&o);
}

static void test_eagain_and_close_flush(void) {
    sdf_origin_cfg_t cfg;
    fill_cfg(&cfg, 0);
    cfg.throttle_ns = 2000000000ull; /* 2 s: nothing completes on its own */
    cfg.max_inflight = 1;            /* queue capacity 2 (pow2 of 1x1) */

    sdf_origin_t o = {.ops = sdf_loopback_ops(), .impl = NULL};
    CHECK(o.ops->open(&o, &cfg, 1) == 0);
    tally_reset();

    sdf_fetch_req_t req = {.origin_id = 0, .job_id = 1, .obj_offset = 0, .len = 16};
    CHECK(o.ops->fetch(&o, &req, test_cb, NULL) == 0);
    CHECK(o.ops->fetch(&o, &req, test_cb, NULL) == 0);
    CHECK(o.ops->fetch(&o, &req, test_cb, NULL) == -EAGAIN); /* queue full */

    sdf_origin_stats_t st;
    CHECK(o.ops->stats(&o, 0, &st) == 0);
    CHECK(st.in_flight == 2);

    /* close must flush: the two pending units complete immediately */
    o.ops->close(&o);
    CHECK(atomic_load(&g_tally.done_ok) == 2);
    o.ops->close(&o); /* double close is a no-op */
}

int main(void) {
    make_files();
    test_open_validation();
    test_data_and_stats();
    test_throttle_latency();
    test_loss_err_stats();
    test_drain_and_restore();
    test_eagain_and_close_flush();
    rm_files();
    printf("test_origin_loopback: ok (data, stats exactness, knobs, drain, "
           "EAGAIN, close flush)\n");
    return 0;
}
