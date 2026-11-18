/*
 * WM profile publication over POSIX SHM (plan prompt W2.3, ADR-0002).
 *
 * Publication protocol adapted from wack_a_mole include/wam/profpub.h
 * @ a0f0422 (seqlock snapshot discipline: a reader sees exactly one
 * complete generation or retries) laid onto the ADR-0002 segment map:
 * double-buffered profile with an active_buf flip inside the seqlock
 * write section, an atomic down_mask in the telemetry header (the C
 * fast path's hard-down set — the control loop defers to it for one
 * interval, design 01 open question #3), and the W0.2 SPSC ring for
 * telemetry slots.
 */

#include "sdf/wm_shm.h"

#include "sdf/common/ring.h"

#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

/* active_buf lives in the seqlock's padding: byte 4 of the segment.
 *
 * Every cross-thread word is accessed atomically (relaxed; the
 * seqlock's seq_cst markers supply the ordering) so the publication
 * is TSan-clean by construction — no plain word is raced, the
 * wam profpub discipline. The wire layout is unchanged: atomics
 * have the same representation, so Python/ctypes see the same
 * bytes. */
#define WM_SHM_ACTIVE_OFF 4

static uint32_t au32_load(const uint32_t *p) {
    return atomic_load_explicit((const _Atomic uint32_t *)p,
                                memory_order_relaxed);
}

static void au32_store(uint32_t *p, uint32_t v) {
    atomic_store_explicit((_Atomic uint32_t *)p, v, memory_order_relaxed);
}

static void au64_store(uint64_t *p, uint64_t v) {
    atomic_store_explicit((_Atomic uint64_t *)p, v, memory_order_relaxed);
}

static unsigned char active_load(const sdf_shm_t *s) {
    return atomic_load_explicit(
        (const _Atomic unsigned char *)((uint8_t *)s->base + WM_SHM_ACTIVE_OFF),
        memory_order_relaxed);
}

static void active_store(const sdf_shm_t *s, unsigned char v) {
    atomic_store_explicit(
        (_Atomic unsigned char *)((uint8_t *)s->base + WM_SHM_ACTIVE_OFF), v,
        memory_order_relaxed);
}

static sdf_seqlock_t *seg_seqlock(const sdf_shm_t *s) {
    return (sdf_seqlock_t *)((uint8_t *)s->base + SDF_SHM_OFF_SEQLOCK);
}

static wm_shm_profile_buf_t *seg_buf(const sdf_shm_t *s, int which) {
    return (wm_shm_profile_buf_t *)((uint8_t *)s->base +
                                   (which == 0 ? SDF_SHM_OFF_PROFILE_A
                                               : SDF_SHM_OFF_PROFILE_B));
}

static wm_shm_telem_hdr_t *seg_hdr(const sdf_shm_t *s) {
    return (wm_shm_telem_hdr_t *)((uint8_t *)s->base + SDF_SHM_OFF_TELEMETRY_HDR);
}

static spsc_ring_t *seg_ring(const sdf_shm_t *s) {
    return (spsc_ring_t *)((uint8_t *)s->base + SDF_SHM_OFF_TELEMETRY_RING);
}

/* Telemetry ring: 32 slots of wm_shm_telem_t (192-byte header +
 * 32*72 = 2496 bytes, inside the 0x1000 region of SDF_SHM_MIN_SIZE). */
#define WM_SHM_TELEM_CAP 32u

static int init_segment(sdf_shm_t *s, uint32_t n_paths) {
    if (n_paths == 0 || n_paths > WM_MAX_PATHS)
        return -EINVAL;
    if (s->size < SDF_SHM_MIN_SIZE)
        return -EINVAL;
    memset(s->base, 0, SDF_SHM_MIN_SIZE);
    sdf_seqlock_init(seg_seqlock(s));
    active_store(s, 0);
    wm_shm_telem_hdr_t *hdr = seg_hdr(s);
    hdr->magic = WM_SHM_MAGIC;
    hdr->n_paths = n_paths;
    atomic_store_explicit(&hdr->down_mask, 0, memory_order_relaxed);
    return spsc_init(seg_ring(s), (uint8_t *)s->base + SDF_SHM_OFF_TELEMETRY_RING,
                     WM_SHM_TELEM_CAP, (uint32_t)sizeof(wm_shm_telem_t));
}

static int check_segment(const sdf_shm_t *s, uint32_t n_paths) {
    if (s == NULL || s->base == NULL || s->size < SDF_SHM_MIN_SIZE)
        return -EINVAL;
    const wm_shm_telem_hdr_t *hdr = seg_hdr(s);
    if (hdr->magic != WM_SHM_MAGIC || hdr->n_paths != n_paths)
        return -EINVAL;
    return 0;
}

/* magic-only validation for calls that carry no expected n (the
 * profile itself declares n) */
static int check_magic(const sdf_shm_t *s) {
    if (s == NULL || s->base == NULL || s->size < SDF_SHM_MIN_SIZE)
        return -EINVAL;
    if (seg_hdr(s)->magic != WM_SHM_MAGIC)
        return -EINVAL;
    return 0;
}

int sdf_wm_shm_create(const char *name, uint32_t n_paths, sdf_shm_t *s) {
    if (s == NULL)
        return -EINVAL;
    int rc = sdf_shm_create(s, name, SDF_SHM_MIN_SIZE);
    if (rc != 0)
        return rc;
    rc = init_segment(s, n_paths);
    if (rc != 0) {
        sdf_shm_close(s);
        return rc;
    }
    return 0;
}

int sdf_wm_shm_attach(const char *name, uint32_t n_paths, sdf_shm_t *s) {
    if (s == NULL)
        return -EINVAL;
    int rc = sdf_shm_open(s, name, SDF_SHM_MIN_SIZE);
    if (rc != 0)
        return rc;
    if (check_segment(s, n_paths) != 0) {
        sdf_shm_close(s);
        return -EINVAL;
    }
    return 0;
}

int sdf_wm_shm_publish(sdf_shm_t *s, const wm_profile_t *prof) {
    if (s == NULL || s->base == NULL || prof == NULL)
        return -EINVAL;
    if (prof->n == 0 || prof->n > WM_MAX_PATHS)
        return -EINVAL;

    int active = active_load(s) ? 1 : 0;
    wm_shm_profile_buf_t *dst = seg_buf(s, active ? 0 : 1); /* inactive */

    au32_store(&dst->version, prof->version);
    au32_store(&dst->m, prof->m);
    au32_store(&dst->n, prof->n);
    au64_store(&dst->down_mask, 0); /* advisory; live mask is in the header */
    for (uint32_t i = 0; i < WM_MAX_PATHS; i++)
        au32_store(&dst->b[i], prof->b[i]);

    /* flip inside the seqlock write section: readers that overlap the
     * flip retry; readers after it see the fully written buffer */
    sdf_seqlock_write_begin(seg_seqlock(s));
    active_store(s, (unsigned char)(active ? 0 : 1));
    sdf_seqlock_write_end(seg_seqlock(s));
    return 0;
}

int sdf_wm_shm_acquire(const sdf_shm_t *s, wm_profile_t *prof,
                       uint32_t *version) {
    if (s == NULL || s->base == NULL || prof == NULL)
        return -EINVAL;
    if (check_magic(s) != 0)
        return -EINVAL;

    for (;;) {
        uint32_t snap = sdf_seqlock_read_begin(seg_seqlock(s));
        int active = active_load(s) ? 1 : 0;
        const wm_shm_profile_buf_t *src = seg_buf(s, active);
        memset(prof, 0, sizeof(*prof));
        prof->version = au32_load(&src->version);
        prof->m = au32_load(&src->m);
        prof->n = au32_load(&src->n);
        for (uint32_t i = 0; i < WM_MAX_PATHS; i++)
            prof->b[i] = au32_load(&src->b[i]);
        if (!sdf_seqlock_read_retry(seg_seqlock(s), snap))
            break; /* exactly one complete generation */
    }
    if (version != NULL)
        *version = prof->version;
    return 0;
}

int sdf_wm_shm_telemetry_push(sdf_shm_t *s, const sdf_origin_stats_t *st) {
    if (s == NULL || s->base == NULL || st == NULL)
        return -EINVAL;
    if (check_magic(s) != 0)
        return -EINVAL;
    if (st->id >= seg_hdr(s)->n_paths)
        return -EINVAL;

    wm_shm_telem_t slot;
    memset(&slot, 0, sizeof(slot));
    slot.id = st->id;
    slot.reqs_out = st->reqs_out;
    slot.reqs_done = st->reqs_done;
    slot.reqs_failed = st->reqs_failed;
    slot.bytes_rx = st->bytes_rx;
    slot.in_flight = st->in_flight;
    slot.ewma_lat_ns = st->ewma_lat_ns;
    slot.ewma_err = st->ewma_err;

    if (!spsc_try_push(seg_ring(s), &slot))
        return -EAGAIN; /* control loop is slow: telemetry is best-effort */
    return 0;
}

int sdf_wm_shm_down_mask_store(sdf_shm_t *s, uint64_t mask) {
    if (s == NULL || s->base == NULL)
        return -EINVAL;
    atomic_store_explicit(&seg_hdr(s)->down_mask, mask,
                          memory_order_release);
    return 0;
}

uint64_t sdf_wm_shm_down_mask_load(const sdf_shm_t *s) {
    if (s == NULL || s->base == NULL)
        return 0;
    return atomic_load_explicit(&seg_hdr(s)->down_mask, memory_order_acquire);
}

void sdf_wm_shm_close(sdf_shm_t *s) {
    sdf_shm_close(s);
}
