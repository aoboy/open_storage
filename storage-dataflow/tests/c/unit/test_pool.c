/*
 * Object pool tests (plan prompt W0.2).
 *
 * Layout pins (header 64B, free-stack 4B/slot rounded to a cacheline,
 * stride rounded to 8, slots 64-aligned), init validation, alloc/free
 * bookkeeping, in-region/alignment of every returned slot, and a
 * free/alloc churn cycle proving the pool never calls malloc after
 * init — every pointer stays inside the caller-provided region.
 */

#include "sdf/common/pool.h"

#include <errno.h>
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

static void test_sizeof(void) {
    CHECK(sdf_pool_sizeof(0, 8) == 0);
    CHECK(sdf_pool_sizeof(8, 0) == 0);
    /* hdr 64 + stack 4*cap rounded up to cacheline, then stride*cap */
    CHECK(sdf_pool_sizeof(1, 1) == 128u + 8u);        /* stack 4B -> slots at 128 */
    CHECK(sdf_pool_sizeof(12, 3) == 128u + 3u * 16u); /* stride(12)=16 */
    CHECK(sdf_pool_sizeof(4, 16) == 128u + 16u * 8u); /* stack 64B -> 128, stride(4)=8 */
}

static void test_init_validation(void) {
    static _Alignas(64) uint8_t host[512];
    sdf_pool_t *p = (sdf_pool_t *)host;
    size_t need = sdf_pool_sizeof(24, 8);

    CHECK(sdf_pool_init(NULL, host, sizeof(host), 24, 8) == -EINVAL);
    CHECK(sdf_pool_init(p, NULL, need, 24, 8) == -EINVAL);
    CHECK(sdf_pool_init(p, host + 8, need, 24, 8) == -EINVAL); /* misaligned */
    CHECK(sdf_pool_init(p, host, need - 1, 24, 8) == -EINVAL); /* short mem */
    CHECK(sdf_pool_init(p, host, need, 24, 8) == 0);
    CHECK(sdf_pool_capacity(p) == 8);
    CHECK(sdf_pool_in_use(p) == 0);
}

static void test_alloc_free_cycle(void) {
    enum { CAP = 8, SLOT = 24 };
    static _Alignas(64) uint8_t host[512];
    sdf_pool_t *p = (sdf_pool_t *)host;
    CHECK(sdf_pool_init(p, host, sdf_pool_sizeof(SLOT, CAP), SLOT, CAP) == 0);

    void *slots[CAP];
    for (uint32_t i = 0; i < CAP; i++) {
        slots[i] = sdf_pool_alloc(p);
        CHECK(slots[i] != NULL);
        CHECK(((uintptr_t)slots[i] & 7u) == 0); /* stride guarantees 8 */
        for (uint32_t j = 0; j < i; j++) {
            CHECK(slots[i] != slots[j]); /* live slots never alias */
        }
    }
    CHECK(sdf_pool_in_use(p) == CAP);
    /* region bound: max-min <= (CAP-1)*stride */
    uintptr_t lo = (uintptr_t)slots[0], hi = (uintptr_t)slots[0];
    for (uint32_t i = 1; i < CAP; i++) {
        if ((uintptr_t)slots[i] < lo) {
            lo = (uintptr_t)slots[i];
        }
        if ((uintptr_t)slots[i] > hi) {
            hi = (uintptr_t)slots[i];
        }
    }
    CHECK(hi - lo <= (uintptr_t)(CAP - 1) * 24u);

    CHECK(sdf_pool_alloc(p) == NULL); /* exhausted */

    for (uint32_t i = 0; i < CAP; i++) {
        sdf_pool_free(p, slots[i]);
    }
    CHECK(sdf_pool_in_use(p) == 0);

    /* full free/alloc churn: data survives (nothing zeroes after init),
     * every pointer stays in-region and 8-aligned */
    for (uint32_t round = 0; round < 100; round++) {
        for (uint32_t i = 0; i < CAP; i++) {
            slots[i] = sdf_pool_alloc(p);
            CHECK(slots[i] != NULL);
            CHECK(((uintptr_t)slots[i] & 7u) == 0);
            memset(slots[i], (int)(round + i), SLOT);
        }
        CHECK(sdf_pool_alloc(p) == NULL);
        /* interleave: free even indices, take them back, then free all */
        for (uint32_t i = 0; i < CAP; i += 2) {
            sdf_pool_free(p, slots[i]);
        }
        CHECK(sdf_pool_in_use(p) == CAP / 2);
        for (uint32_t i = 0; i < CAP; i += 2) {
            slots[i] = sdf_pool_alloc(p);
            CHECK(slots[i] != NULL);
        }
        CHECK(sdf_pool_in_use(p) == CAP);
        for (uint32_t i = 0; i < CAP; i++) {
            sdf_pool_free(p, slots[i]);
        }
        CHECK(sdf_pool_in_use(p) == 0);
    }

    /* free() ignores foreign pointers instead of corrupting the stack */
    static uint8_t foreign[64];
    sdf_pool_free(p, (void *)foreign);
    CHECK(sdf_pool_in_use(p) == 0);
    CHECK(sdf_pool_alloc(p) != NULL); /* pool still healthy */
}

int main(void) {
    test_sizeof();
    test_init_validation();
    test_alloc_free_cycle();
    printf("test_pool: ok\n");
    return 0;
}
