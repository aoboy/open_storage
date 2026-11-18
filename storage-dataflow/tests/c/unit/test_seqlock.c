/*
 * Seqlock tests (plan prompt W0.2).
 *
 * API legs: init, odd/even marker transitions, retry semantics.
 * Contention leg: a writer publishing complete generations while a
 * reader accepts snapshots — every accepted snapshot must be exactly
 * one generation, never a mix. Adapted from wack_a_mole
 * tests/test_profpub.c @ a0f0422 (no torn read under contention).
 */

#include "sdf/common/seqlock.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);       \
            exit(1);                                                          \
        }                                                                     \
    } while (0)

#define WORDS 64u
#define GENS 100000u
#define WARM_TARGET 100u

typedef struct {
    sdf_seqlock_t lock; /* first member: segment offset 0 per ADR-0002 */
    _Atomic uint32_t gen;
    _Atomic uint32_t w[WORDS];
} payload_t;

static payload_t g_pay;
static _Atomic int g_writer_done;
static _Atomic uint32_t g_reader_warm; /* accepted generation-0 snapshots */
static _Atomic uint64_t g_torn_retries;

static void publish(uint32_t gen) {
    sdf_seqlock_write_begin(&g_pay.lock);
    atomic_store_explicit(&g_pay.gen, gen, memory_order_relaxed);
    for (uint32_t i = 0; i < WORDS; i++)
        atomic_store_explicit(&g_pay.w[i], gen * WORDS + i, memory_order_relaxed);
    sdf_seqlock_write_end(&g_pay.lock);
}

static void *writer_main(void *arg) {
    (void)arg;
    /* Do not start until the reader is demonstrably spinning: kills the
     * scheduling flake where the reader is descheduled for the whole
     * publishing window and the test proves nothing. */
    while (atomic_load_explicit(&g_reader_warm, memory_order_acquire) < WARM_TARGET)
        sched_yield();
    for (uint32_t g = 1; g <= GENS; g++) {
        publish(g);
        if ((g & 15u) == 0)
            sched_yield(); /* keep the reader fed on any scheduling */
    }
    atomic_store_explicit(&g_writer_done, 1, memory_order_release);
    return NULL;
}

static void *reader_main(void *arg) {
    (void)arg;
    uint64_t accepted = 0;
    int warmed = 0;
    while (!warmed || !atomic_load_explicit(&g_writer_done, memory_order_acquire)) {
        uint32_t s = sdf_seqlock_read_begin(&g_pay.lock);
        uint32_t gen = atomic_load_explicit(&g_pay.gen, memory_order_relaxed);
        int consistent = 1;
        for (uint32_t i = 0; i < WORDS; i++) {
            uint32_t v = atomic_load_explicit(&g_pay.w[i], memory_order_relaxed);
            if (v != gen * WORDS + i)
                consistent = 0;
        }
        if (sdf_seqlock_read_retry(&g_pay.lock, s)) {
            atomic_fetch_add_explicit(&g_torn_retries, 1, memory_order_relaxed);
            continue; /* publish overlapped the copy: discard */
        }
        if (!consistent) {
            fprintf(stderr, "FAIL: accepted torn snapshot (gen=%u)\n", gen);
            exit(1);
        }
        accepted++;
        if (gen == 0)
            atomic_fetch_add_explicit(&g_reader_warm, 1, memory_order_relaxed);
        if (atomic_load_explicit(&g_reader_warm, memory_order_relaxed) >= WARM_TARGET)
            warmed = 1;
    }
    /* The warm-up gate proves the reader was live during publishing; the
     * quantity assertions are scheduling-independent: no torn snapshot
     * was ever accepted (checked per read above) and at least one read
     * overlapped a publish (retry observed). */
    return (void *)(uintptr_t)accepted;
}

static void test_api_single_thread(void) {
    sdf_seqlock_t l;
    sdf_seqlock_init(&l);
    CHECK(sdf_seqlock_read_begin(&l) == 0); /* even: stable */
    CHECK(!sdf_seqlock_read_retry(&l, 0));

    /* writer markers: odd during, even after */
    sdf_seqlock_write_begin(&l);
    CHECK((atomic_load_explicit(&l.seq, memory_order_seq_cst) & 1u) != 0);
    sdf_seqlock_write_end(&l);
    CHECK((atomic_load_explicit(&l.seq, memory_order_seq_cst) & 1u) == 0);

    /* retry fires when the sequence moved between begin and recheck */
    uint32_t s = sdf_seqlock_read_begin(&l);
    sdf_seqlock_write_begin(&l);
    sdf_seqlock_write_end(&l);
    CHECK(sdf_seqlock_read_retry(&l, s)); /* changed */
    CHECK(sdf_seqlock_read_retry(&l, s)); /* still != start */

    /* begin during an in-flight publish observes the odd marker and
     * the reader must discard via retry */
    sdf_seqlock_write_begin(&l);
    uint32_t odd = sdf_seqlock_read_begin(&l);
    CHECK((odd & 1u) != 0);
    CHECK(sdf_seqlock_read_retry(&l, odd));
    sdf_seqlock_write_end(&l);
    CHECK(!sdf_seqlock_read_retry(&l, sdf_seqlock_read_begin(&l)));

    /* seq advances by exactly 2 per publish (overflow-safe uint32) */
    uint32_t before = sdf_seqlock_read_begin(&l);
    sdf_seqlock_write_begin(&l);
    sdf_seqlock_write_end(&l);
    CHECK(sdf_seqlock_read_begin(&l) == before + 2u);
}

static void test_contention(void) {
    sdf_seqlock_init(&g_pay.lock);
    atomic_store_explicit(&g_writer_done, 0, memory_order_relaxed);
    atomic_store_explicit(&g_reader_warm, 0, memory_order_relaxed);
    atomic_store_explicit(&g_torn_retries, 0, memory_order_relaxed);
    /* Initial state is a properly published generation 0, so any
     * snapshot the reader accepts before the first writer generation
     * is internally consistent. */
    publish(0);

    pthread_t w, r;
    CHECK(pthread_create(&w, NULL, writer_main, NULL) == 0);
    CHECK(pthread_create(&r, NULL, reader_main, NULL) == 0);

    void *wRet = NULL, *rRet = NULL;
    CHECK(pthread_join(w, &wRet) == 0);
    CHECK(pthread_join(r, &rRet) == 0);

    uint64_t accepted = (uint64_t)(uintptr_t)rRet;
    CHECK(accepted >= WARM_TARGET); /* reader was live and accepting */
    CHECK(atomic_load(&g_torn_retries) > 0); /* a read overlapped a publish */
    printf("test_seqlock: ok (%llu accepted snapshots, %llu retries)\n",
           (unsigned long long)accepted,
           (unsigned long long)atomic_load(&g_torn_retries));
}

int main(void) {
    test_api_single_thread();
    test_contention();
    return 0;
}
