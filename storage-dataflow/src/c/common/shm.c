/*
 * POSIX shared-memory segment management for the C<->Python handoff.
 *
 * Layout (offsets are ABI, pinned in include/sdf/common/shm.h and
 * tests/c/test_abi.c; protocol in ADR-0002):
 *
 *   offset 0x0000 : sdf_seqlock_t (64 bytes)
 *   offset 0x0040 : profile/knob buffer A
 *   offset 0x0800 : profile/knob buffer B
 *   offset 0x0FC0 : telemetry header
 *   offset 0x1000 : telemetry SPSC ring
 *
 * Segment management pattern follows the wack_a_mole control-plane SHM
 * work @ a0f0422 (shm_open + ftruncate + MAP_SHARED, one writer and
 * one reader per channel, no remap after init); the fixed offset map
 * is this repository's own ADR-0002.
 *
 * Authorized by plan prompt W0.2 and ADR-0009 (provenance + parity).
 */

#define _POSIX_C_SOURCE 200809L

#include "sdf/common/shm.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* shm_open/shm_unlink require a leading '/'; normalize once per call
 * and store the normalized name in the handle. */
static int sdf_shm_norm_name(char *out, size_t out_sz, const char *name) {
    if (out == NULL || name == NULL || name[0] == '\0')
        return -EINVAL;
    size_t len = strlen(name);
    size_t need = (name[0] == '/') ? len : len + 1u;
    if (need >= out_sz)
        return -ENAMETOOLONG;
    if (name[0] == '/') {
        memcpy(out, name, len + 1u);
    } else {
        out[0] = '/';
        memcpy(out + 1, name, len + 1u);
    }
    return 0;
}

int sdf_shm_create(sdf_shm_t *s, const char *name, size_t size) {
    if (s == NULL)
        return -EINVAL;
    if (size < SDF_SHM_MIN_SIZE)
        return -EINVAL;

    memset(s, 0, sizeof(*s));
    s->fd = -1;

    char norm[SDF_SHM_NAME_MAX];
    int rc = sdf_shm_norm_name(norm, sizeof(norm), name);
    if (rc != 0)
        return rc;

    int fd = shm_open(norm, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd < 0)
        return -errno; /* EEXIST surfaced to the caller on re-create */

    if (ftruncate(fd, (off_t)size) != 0) {
        rc = -errno;
        close(fd);
        (void)shm_unlink(norm);
        return rc;
    }

    /* fresh shm objects are zero-filled: seqlock starts even (stable) */
    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        rc = -errno;
        close(fd);
        (void)shm_unlink(norm);
        return rc;
    }

    s->base = base;
    s->size = size;
    s->fd = fd;
    memcpy(s->name, norm, strlen(norm) + 1u);
    return 0;
}

int sdf_shm_open(sdf_shm_t *s, const char *name, size_t size) {
    if (s == NULL)
        return -EINVAL;
    if (size < SDF_SHM_MIN_SIZE)
        return -EINVAL;

    memset(s, 0, sizeof(*s));
    s->fd = -1;

    char norm[SDF_SHM_NAME_MAX];
    int rc = sdf_shm_norm_name(norm, sizeof(norm), name);
    if (rc != 0)
        return rc;

    int fd = shm_open(norm, O_RDWR, 0);
    if (fd < 0)
        return -errno;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        rc = -errno;
        close(fd);
        return rc;
    }
    if ((size_t)st.st_size < size) {
        close(fd);
        return -EINVAL; /* caller expects at least `size` bytes */
    }

    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        rc = -errno;
        close(fd);
        return rc;
    }

    s->base = base;
    s->size = size;
    s->fd = fd;
    memcpy(s->name, norm, strlen(norm) + 1u);
    return 0;
}

void sdf_shm_close(sdf_shm_t *s) {
    if (s == NULL || s->base == NULL)
        return;
    munmap(s->base, s->size);
    close(s->fd);
    s->base = NULL;
    s->size = 0;
    s->fd = -1;
    s->name[0] = '\0';
}

int sdf_shm_unlink(const char *name) {
    char norm[SDF_SHM_NAME_MAX];
    int rc = sdf_shm_norm_name(norm, sizeof(norm), name);
    if (rc != 0)
        return rc;
    return shm_unlink(norm) == 0 ? 0 : -errno;
}
