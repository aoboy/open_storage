#ifndef SDF_COMMON_SHM_H
#define SDF_COMMON_SHM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Shared-memory layout used for the C<->Python handoff. Fixed offsets are
 * part of the ABI; see docs/adr/0002-shm-seqlock-handoff.md.
 */
#define SDF_SHM_OFF_SEQLOCK 0x0000u
#define SDF_SHM_OFF_PROFILE_A 0x0040u
#define SDF_SHM_OFF_PROFILE_B 0x0800u
#define SDF_SHM_OFF_TELEMETRY_HDR 0x0FC0u
#define SDF_SHM_OFF_TELEMETRY_RING 0x1000u
#define SDF_SHM_MIN_SIZE 0x2000u

#define SDF_SHM_NAME_MAX 64

typedef struct {
    void *base;
    size_t size;
    int fd;
    char name[SDF_SHM_NAME_MAX];
} sdf_shm_t;

/* Create a new segment (fails if it already exists). Returns 0 on success. */
int sdf_shm_create(sdf_shm_t *s, const char *name, size_t size);

/* Open an existing segment. Returns 0 on success. */
int sdf_shm_open(sdf_shm_t *s, const char *name, size_t size);

/* Unmap and close. Does not unlink the segment. */
void sdf_shm_close(sdf_shm_t *s);

/* Unlink a named segment. */
int sdf_shm_unlink(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* SDF_COMMON_SHM_H */
