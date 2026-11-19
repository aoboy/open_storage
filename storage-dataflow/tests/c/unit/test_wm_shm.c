/*
 * WM SHM publication tests (plan prompt W2.3, ADR-0002).
 *
 * Pins the publication protocol on the real segment: publish/acquire
 * roundtrips across buffer flips, version verbatim, telemetry slots
 * through the ring, the C fast path's down_mask, lifecycle
 * (close-without-unlink, re-create), and the no-torn-read contention
 * leg (a writer hammering publishes vs an acquiring reader — every
 * accepted snapshot must be exactly one complete generation: sum(b)
 * == m and a coherent (version, b[]) pair).
 */

#define _POSIX_C_SOURCE 200809L

#include "sdf/common/ring.h"
#include "sdf/wm_shm.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static char g_name[96];

static void mk_profile(wm_profile_t *p, uint32_t version, uint32_t seed) {
    memset(p, 0, sizeof(*p));
    p->version = version;
    p->m = 1024;
    p->n = 4;
    /* deterministic per-version allocation, sum == m */
    uint32_t base = 1024 / 4;
    p->b[0] = base + (seed % 4);
    p->b[1] = base;
    p->b[2] = base;
    p->b[3] = 1024 - p->b[0] - p->b[1] - p->b[2];
}

/* ---- publish / acquire -------------------------------------------------- */

static void test_roundtrip(void) {
    sdf_shm_t s;
    CHECK(sdf_wm_shm_create(g_name, 4, &s) == 0);

    /* create-again fails while it exists */
    sdf_shm_t dup;
    CHECK(sdf_wm_shm_create(g_name, 4, &dup) < 0);

    /* empty segment: acquire yields a zero profile (no publish yet) */
    wm_profile_t got;
    uint32_t ver = 0;
    CHECK(sdf_wm_shm_acquire(&s, &got, &ver) == 0);
    CHECK(got.version == 0 && got.n == 0);

    /* publish v1, acquire matches */
    wm_profile_t p1;
    mk_profile(&p1, 1, 10);
    CHECK(sdf_wm_shm_publish(&s, &p1) == 0);
    CHECK(sdf_wm_shm_acquire(&s, &got, &ver) == 0);
    CHECK(ver == 1 && got.m == 1024 && got.n == 4);
    CHECK(memcmp(got.b, p1.b, sizeof(p1.b)) == 0);

    /* publish v2 through the other buffer; acquire reflects it */
    wm_profile_t p2;
    mk_profile(&p2, 2, 20);
    CHECK(sdf_wm_shm_publish(&s, &p2) == 0);
    CHECK(sdf_wm_shm_acquire(&s, &got, &ver) == 0);
    CHECK(ver == 2);
    CHECK(memcmp(got.b, p2.b, sizeof(p2.b)) == 0);
    CHECK(memcmp(got.b, p1.b, sizeof(p1.b)) != 0); /* really changed */

    /* publish the same profile repeatedly: idempotent readback */
    for (int i = 0; i < 8; i++) {
        CHECK(sdf_wm_shm_publish(&s, &p2) == 0);
    }
    CHECK(sdf_wm_shm_acquire(&s, &got, &ver) == 0);
    CHECK(ver == 2 && memcmp(got.b, p2.b, sizeof(p2.b)) == 0);

    /* argument validation */
    CHECK(sdf_wm_shm_publish(NULL, &p1) == -EINVAL);
    CHECK(sdf_wm_shm_publish(&s, NULL) == -EINVAL);
    CHECK(sdf_wm_shm_acquire(NULL, &got, &ver) == -EINVAL);

    /* down_mask roundtrip (the C fast path's live hard-down set) */
    CHECK(sdf_wm_shm_down_mask_store(&s, 0x5) == 0);
    CHECK(sdf_wm_shm_down_mask_load(&s) == 0x5);
    CHECK(sdf_wm_shm_down_mask_store(&s, 0) == 0);
    CHECK(sdf_wm_shm_down_mask_load(&s) == 0);

    /* telemetry: push slots, pop through the ring, field-exact */
    sdf_origin_stats_t st;
    memset(&st, 0, sizeof(st));
    st.id = 2;
    st.reqs_out = 100;
    st.reqs_done = 90;
    st.reqs_failed = 10;
    st.bytes_rx = 9000;
    st.in_flight = 3;
    st.ewma_lat_ns = 1234.5;
    st.ewma_err = 0.1;
    CHECK(sdf_wm_shm_telemetry_push(&s, &st) == 0);
    st.id = 9;
    CHECK(sdf_wm_shm_telemetry_push(&s, &st) == -EINVAL); /* bad path id */

    spsc_ring_t *ring = (spsc_ring_t *)((uint8_t *)s.base + SDF_SHM_OFF_TELEMETRY_RING);
    wm_shm_telem_t slot;
    CHECK(spsc_try_pop(ring, &slot));
    CHECK(slot.id == 2 && slot.reqs_out == 100 && slot.reqs_done == 90 && slot.reqs_failed == 10 &&
          slot.bytes_rx == 9000 && slot.in_flight == 3 && slot.ewma_lat_ns == 1234.5 &&
          slot.ewma_err == 0.1);
    CHECK(!spsc_try_pop(ring, &slot)); /* drained */

    /* ring overflow policy: -EAGAIN, telemetry is best-effort */
    for (int i = 0; i < 40; i++) {
        st.id = 0;
        int rc = sdf_wm_shm_telemetry_push(&s, &st);
        CHECK(rc == 0 || rc == -EAGAIN);
    }

    sdf_wm_shm_close(&s);
    CHECK(s.base == NULL);
    CHECK(sdf_shm_unlink(g_name) == 0); /* free the name for the next leg */
}

/* ---- no-torn-read contention ------------------------------------------ */

static sdf_shm_t g_seg;
static _Atomic int g_stop;

static void *hammer_publish(void *arg) {
    (void)arg;
    wm_profile_t p;
    uint32_t v = 0;
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        v++;
        mk_profile(&p, v, v);
        CHECK(sdf_wm_shm_publish(&g_seg, &p) == 0);
    }
    return NULL;
}

static void test_no_torn_reads(void) {
    CHECK(sdf_wm_shm_create(g_name, 4, &g_seg) == 0);
    atomic_store(&g_stop, 0);

    wm_profile_t p0;
    mk_profile(&p0, 1, 1);
    CHECK(sdf_wm_shm_publish(&g_seg, &p0) == 0);

    pthread_t w;
    CHECK(pthread_create(&w, NULL, hammer_publish, NULL) == 0);

    uint64_t accepted = 0;
    wm_profile_t got;
    for (int i = 0; i < 200000; i++) {
        uint32_t ver = 0;
        CHECK(sdf_wm_shm_acquire(&g_seg, &got, &ver) == 0);
        /* every accepted snapshot is one complete generation: the
         * b[i] pattern is a function of version — a torn read would
         * break the (version, b[]) coherence */
        CHECK(got.n == 4 && got.m == 1024);
        uint32_t expect0 = 256 + (ver % 4);
        CHECK(got.b[0] == expect0);
        uint64_t sum = 0;
        for (uint32_t k = 0; k < 4; k++) {
            sum += got.b[k];
        }
        CHECK(sum == 1024);
        accepted++;
    }
    atomic_store(&g_stop, 1);
    CHECK(pthread_join(w, NULL) == 0);
    CHECK(accepted == 200000);
    printf("  contention leg: 200000 coherent snapshots under a hammering "
           "writer\n");

    sdf_wm_shm_close(&g_seg);
    CHECK(sdf_shm_unlink(g_name) == 0); /* free the name for the next leg */
}

/* ---- lifecycle --------------------------------------------------------- */

static void test_lifecycle(void) {
    sdf_shm_t s;
    CHECK(sdf_wm_shm_create(g_name, 4, &s) == 0);
    wm_profile_t p;
    mk_profile(&p, 7, 3);
    CHECK(sdf_wm_shm_publish(&s, &p) == 0);
    sdf_wm_shm_close(&s);

    /* close does not unlink: attach sees the published state */
    sdf_shm_t a;
    CHECK(sdf_wm_shm_attach(g_name, 4, &a) == 0);
    wm_profile_t got;
    uint32_t ver = 0;
    CHECK(sdf_wm_shm_acquire(&a, &got, &ver) == 0);
    CHECK(ver == 7);
    sdf_wm_shm_close(&a);

    /* wrong n_paths attach is refused */
    sdf_shm_t bad;
    CHECK(sdf_wm_shm_attach(g_name, 5, &bad) == -EINVAL);
    CHECK(bad.base == NULL);

    /* plain segment without the wm header is refused */
    CHECK(sdf_shm_unlink(g_name) == 0);
    {
        sdf_shm_t plain;
        CHECK(sdf_shm_create(&plain, g_name, SDF_SHM_MIN_SIZE) == 0);
        sdf_shm_t b2;
        CHECK(sdf_wm_shm_attach(g_name, 4, &b2) == -EINVAL); /* no magic */
        CHECK(sdf_shm_unlink(g_name) == 0);
    }

    /* re-create after unlink works, fresh state */
    CHECK(sdf_wm_shm_create(g_name, 4, &s) == 0);
    CHECK(sdf_wm_shm_acquire(&s, &got, &ver) == 0);
    CHECK(ver == 0 && got.n == 0); /* fresh, not the old v7 */
    sdf_wm_shm_close(&s);
    CHECK(sdf_shm_unlink(g_name) == 0);
    CHECK(sdf_shm_unlink(g_name) < 0);
}

int main(void) {
    snprintf(g_name, sizeof(g_name), "sdf-test-wm-shm-%d", (int)getpid());
    test_roundtrip();
    test_no_torn_reads();
    test_lifecycle();
    printf("test_wm_shm: ok (publication, telemetry, down_mask, torn-read "
           "contention, lifecycle)\n");
    return 0;
}
