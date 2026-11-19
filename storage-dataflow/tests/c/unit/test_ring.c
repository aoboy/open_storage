/*
 * SPSC ring tests (plan prompt W0.2).
 *
 * Single-thread legs: sizeof/init validation, FIFO order over many
 * wraps, full/empty edges, batch pop, count. Threaded leg: 2^22-slot
 * soak across two threads — adapted from wack_a_mole
 * tests/test_spsc.c @ a0f0422 (wrap order + 2^22 two-thread soak).
 */

#include "sdf/common/ring.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

typedef struct {
    uint64_t seq;
    uint64_t magic;
} slot_t;

#define SOAK_CAP 1024u
#define SOAK_ROUNDS (1u << 22)

static _Alignas(64) uint8_t g_soak_mem[192u + (size_t)SOAK_CAP * sizeof(slot_t)];
static spsc_ring_t *g_soak;

static void *producer_main(void *arg) {
    (void)arg;
    slot_t s = {.magic = 0xC0FFEE123456789ull, .seq = 0};
    for (uint64_t i = 0; i < SOAK_ROUNDS; i++) {
        s.seq = i;
        while (!spsc_try_push(g_soak, &s)) {
            sched_yield(); /* full: backpressure is the caller's policy */
        }
    }
    return NULL;
}

static void *consumer_main(void *arg) {
    (void)arg;
    uint64_t expect = 0;
    while (expect < SOAK_ROUNDS) {
        slot_t s;
        if (spsc_try_pop(g_soak, &s)) {
            CHECK(s.seq == expect); /* FIFO: exact arrival order */
            CHECK(s.magic == 0xC0FFEE123456789ull);
            expect++;
        } else {
            sched_yield();
        }
    }
    return (void *)(uintptr_t)expect;
}

static void test_validation(void) {
    CHECK(spsc_sizeof(0, 8) == 0);
    CHECK(spsc_sizeof(1, 8) == 0);
    CHECK(spsc_sizeof(3, 8) == 0); /* not a power of two */
    CHECK(spsc_sizeof(16, 0) == 0);
    CHECK(spsc_sizeof(16, 8) == 192u + 16u * 8u);

    static _Alignas(64) uint8_t mem[192u + 16u * 8u];
    static _Alignas(64) uint8_t host[192u + 16u * 8u + 64u];
    spsc_ring_t *r = (spsc_ring_t *)mem;

    CHECK(spsc_init(r, mem, 3, 8) == -EINVAL);                               /* not pow2 */
    CHECK(spsc_init(r, mem, 16, 0) == -EINVAL);                              /* no slot bytes */
    CHECK(spsc_init((spsc_ring_t *)(host + 8), host + 8, 16, 8) == -EINVAL); /* misaligned */
    CHECK(spsc_init(r, host, 16, 8) == -EINVAL);                             /* r must equal mem */
    CHECK(spsc_init(r, mem, 16, 8) == 0);

    CHECK(offsetof(spsc_ring_t, slots) == 192);
}

static void test_fifo_wrap_edges(void) {
    static _Alignas(64) uint8_t mem[192u + 16u * 8u];
    spsc_ring_t *r = (spsc_ring_t *)mem;
    CHECK(spsc_init(r, mem, 16, 8) == 0);

    /* fill to capacity: 16 succeed, the 17th fails */
    for (uint64_t i = 0; i < 16; i++) {
        uint64_t v = i * 7u;
        CHECK(spsc_try_push(r, &v));
    }
    CHECK(spsc_count(r) == 16);
    uint64_t scratch = 99;
    CHECK(!spsc_try_push(r, &scratch)); /* full: push fails */
    CHECK(spsc_try_pop(r, &scratch));   /* full, not empty: pop succeeds */
    CHECK(scratch == 0);                /* first element is 0*7 */

    /* drain the remaining 15: order preserved */
    for (uint64_t i = 1; i < 16; i++) {
        CHECK(spsc_try_pop(r, &scratch));
        CHECK(scratch == i * 7u);
    }
    CHECK(spsc_count(r) == 0);
    CHECK(!spsc_try_pop(r, &scratch)); /* empty */

    /* 1000 push/pop cycles on a 16-slot ring: 62 wraps, order intact */
    for (uint64_t i = 0; i < 1000; i++) {
        uint64_t v = 0xBEE0000u + i;
        CHECK(spsc_try_push(r, &v));
        CHECK(spsc_try_pop(r, &scratch));
        CHECK(scratch == v);
    }
}

static void test_batch_pop(void) {
    static _Alignas(64) uint8_t mem[192u + 16u * 8u];
    uint64_t out[16];
    spsc_ring_t *r = (spsc_ring_t *)mem;
    CHECK(spsc_init(r, mem, 16, 8) == 0);

    for (uint64_t i = 0; i < 16; i++) {
        CHECK(spsc_try_push(r, &i));
    }
    CHECK(spsc_batch_pop(r, out, 0) == 0);
    CHECK(spsc_batch_pop(r, out, 10) == 10);
    for (uint64_t i = 0; i < 10; i++) {
        CHECK(out[i] == i);
    }
    CHECK(spsc_batch_pop(r, out, 100) == 6); /* only what remains */
    for (uint64_t i = 0; i < 6; i++) {
        CHECK(out[i] == 10u + i);
    }
    CHECK(spsc_count(r) == 0);
    CHECK(spsc_batch_pop(r, out, 4) == 0);
}

static void test_two_thread_soak(void) {
    g_soak = (spsc_ring_t *)g_soak_mem;
    CHECK(spsc_init(g_soak, g_soak_mem, SOAK_CAP, (uint32_t)sizeof(slot_t)) == 0);

    pthread_t prod, cons;
    CHECK(pthread_create(&prod, NULL, producer_main, NULL) == 0);
    CHECK(pthread_create(&cons, NULL, consumer_main, NULL) == 0);

    void *pRet = NULL, *cRet = NULL;
    CHECK(pthread_join(prod, &pRet) == 0);
    CHECK(pthread_join(cons, &cRet) == 0);
    CHECK(pRet == NULL);
    CHECK((uint64_t)(uintptr_t)cRet == SOAK_ROUNDS);
    CHECK(spsc_count(g_soak) == 0);
}

int main(void) {
    test_validation();
    test_fifo_wrap_edges();
    test_batch_pop();
    test_two_thread_soak();
    printf("test_ring: ok (2^22 two-thread soak)\n");
    return 0;
}
