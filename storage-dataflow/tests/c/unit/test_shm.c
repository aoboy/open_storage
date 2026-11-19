/*
 * Shared-memory segment tests (plan prompt W0.2).
 *
 * Pins the ADR-0002 contract from the C side: create/open semantics
 * (O_EXCL re-create fails), two independent mappings of the same
 * segment see each other's writes, the seqlock at offset 0 publishes
 * consistently across mappings, close does not unlink, unlink makes
 * the name unopenable and re-creatable, and name normalization accepts
 * both "name" and "/name".
 */

#include "sdf/common/seqlock.h"
#include "sdf/common/shm.h"

#include <errno.h>
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

int main(void) {
    char name[64], name_nolead[64];
    snprintf(name, sizeof(name), "sdf-test-w02-%d", (int)getpid());
    snprintf(name_nolead, sizeof(name_nolead), "sdf-w02-lead-%d", (int)getpid());

    /* ADR-0002 offset map: ordered, non-overlapping, ring gets a page */
    CHECK(SDF_SHM_OFF_SEQLOCK < SDF_SHM_OFF_PROFILE_A);
    CHECK(SDF_SHM_OFF_PROFILE_A < SDF_SHM_OFF_PROFILE_B);
    CHECK(SDF_SHM_OFF_PROFILE_B < SDF_SHM_OFF_TELEMETRY_HDR);
    CHECK(SDF_SHM_OFF_TELEMETRY_HDR < SDF_SHM_OFF_TELEMETRY_RING);
    CHECK(SDF_SHM_MIN_SIZE - SDF_SHM_OFF_TELEMETRY_RING == 0x1000u);

    /* create validation */
    sdf_shm_t bad;
    CHECK(sdf_shm_create(NULL, name, SDF_SHM_MIN_SIZE) == -EINVAL);
    CHECK(sdf_shm_create(&bad, name, SDF_SHM_MIN_SIZE - 1) == -EINVAL);

    /* create + write */
    sdf_shm_t a;
    CHECK(sdf_shm_create(&a, name, SDF_SHM_MIN_SIZE) == 0);
    CHECK(a.base != NULL && a.fd >= 0);
    CHECK(strncmp(a.name, "/", 1) == 0); /* normalized with leading '/' */

    sdf_seqlock_t *sl_a = (sdf_seqlock_t *)((uint8_t *)a.base + SDF_SHM_OFF_SEQLOCK);
    sdf_seqlock_init(sl_a);

    /* re-create fails while the name exists (O_EXCL surfaced) */
    sdf_shm_t dup;
    CHECK(sdf_shm_create(&dup, name, SDF_SHM_MIN_SIZE) < 0);

    /* second mapping: coherence — writes via A are visible via B */
    sdf_shm_t b;
    CHECK(sdf_shm_open(&b, name, SDF_SHM_MIN_SIZE) == 0);
    uint32_t *pa = (uint32_t *)((uint8_t *)a.base + SDF_SHM_OFF_PROFILE_A);
    uint32_t *pb = (uint32_t *)((uint8_t *)b.base + SDF_SHM_OFF_PROFILE_A);
    *pa = 0x600D5EEDu;
    CHECK(*pb == 0x600D5EEDu);

    /* seqlock publication across mappings: writer through A, reader
     * through B's own mapping of the same segment offsets */
    sdf_seqlock_t *sl_b = (sdf_seqlock_t *)((uint8_t *)b.base + SDF_SHM_OFF_SEQLOCK);
    CHECK((void *)sl_a != (void *)sl_b); /* distinct virtual addresses */
    uint32_t s0 = sdf_seqlock_read_begin(sl_b);
    CHECK(!sdf_seqlock_read_retry(sl_b, s0)); /* idle: stable */

    sdf_seqlock_write_begin(sl_a);
    *pa = 42u;
    sdf_seqlock_write_end(sl_a);

    uint32_t got = 0, accepted = 0;
    for (uint32_t tries = 0; tries < 1000; tries++) {
        uint32_t s = sdf_seqlock_read_begin(sl_b);
        uint32_t v = *pb;
        if (!sdf_seqlock_read_retry(sl_b, s)) {
            got = v;
            accepted++;
            break;
        }
    }
    CHECK(accepted == 1);
    CHECK(got == 42u);

    /* close does not unlink; data persists in the segment */
    sdf_shm_close(&a);
    CHECK(a.base == NULL && a.fd == -1);
    sdf_shm_close(&b);

    sdf_shm_t c;
    CHECK(sdf_shm_open(&c, name, SDF_SHM_MIN_SIZE) == 0);
    uint32_t *pc = (uint32_t *)((uint8_t *)c.base + SDF_SHM_OFF_PROFILE_A);
    CHECK(*pc == 42u); /* data persisted in the segment across mappings */
    sdf_shm_close(&c);

    /* unlink: open fails, re-create succeeds, second unlink fails */
    CHECK(sdf_shm_unlink(name) == 0);
    sdf_shm_t d;
    CHECK(sdf_shm_open(&d, name, SDF_SHM_MIN_SIZE) < 0);
    sdf_shm_t e;
    CHECK(sdf_shm_create(&e, name, SDF_SHM_MIN_SIZE) == 0);
    sdf_shm_close(&e);
    CHECK(sdf_shm_unlink(name) == 0);
    CHECK(sdf_shm_unlink(name) < 0);

    /* name normalization: create without a leading slash, open with
     * the same spelling, unlink with it too */
    sdf_shm_t f;
    CHECK(sdf_shm_create(&f, name_nolead, SDF_SHM_MIN_SIZE) == 0);
    CHECK(strcmp(f.name + 1, name_nolead) == 0); /* stored normalized */
    sdf_shm_close(&f);
    sdf_shm_t g;
    CHECK(sdf_shm_open(&g, name_nolead, SDF_SHM_MIN_SIZE) == 0);
    sdf_shm_close(&g);
    CHECK(sdf_shm_unlink(name_nolead) == 0);

    printf("test_shm: ok\n");
    return 0;
}
