#ifndef SDF_WM_SHM_H
#define SDF_WM_SHM_H

/*
 * WM profile publication over the C<->Python segment (ADR-0002, plan
 * prompt W2.3).
 *
 * Segment layout (offsets are ABI, shared with include/sdf/common/shm.h):
 *
 *   0x0000  sdf_seqlock_t          seqlock (pad byte 0 = active_buf)
 *   0x0040  buffer A              wm_shm_profile_buf_t
 *   0x0800  buffer B              wm_shm_profile_buf_t
 *   0x0FC0  telemetry header      magic, n_paths, down_mask (atomic)
 *   0x1000  telemetry ring         SPSC ring of wm_shm_telem_t slots
 *
 * Protocol (ADR-0002): the writer fills the INACTIVE buffer, then
 * flips active_buf inside the seqlock write section; the reader
 * reads active_buf and copies the active buffer under the read
 * seqlock, retrying on overlap. A stalled writer leaves the last
 * complete profile in place. Telemetry flows the other way through
 * the lock-free ring; the C fast path publishes its down_mask in the
 * telemetry header so the control loop can defer to hard-down
 * decisions (design 01 open question #3, resolved here).
 *
 * One writer and one reader per channel (the seqlock discipline).
 */

#include "sdf/common/seqlock.h"
#include "sdf/common/shm.h"
#include "sdf/origin.h"
#include "sdf/wm.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WM_SHM_MAGIC 0x53444631u /* "SDF1" */

/* Profile/knob buffer (fits the 0x7C0-byte A/B regions). */
typedef struct {
    uint32_t version; /* bumped by every publish */
    uint32_t m;
    uint32_t n;
    uint32_t _pad;
    uint64_t down_mask; /* last down_mask seen with this profile */
    uint32_t b[WM_MAX_PATHS];
} wm_shm_profile_buf_t;

/* Telemetry header at 0x0FC0 (64 bytes, cacheline). */
typedef struct {
    uint32_t magic;
    uint32_t n_paths;
    _Atomic uint64_t down_mask; /* C fast path's live hard-down set */
    uint32_t _pad[10];
} wm_shm_telem_hdr_t;

/* One telemetry slot (C -> Python), fixed layout for CFFI. */
typedef struct {
    uint32_t id;
    uint32_t _pad;
    uint64_t reqs_out;
    uint64_t reqs_done;
    uint64_t reqs_failed;
    uint64_t bytes_rx;
    uint64_t in_flight;
    double ewma_lat_ns;
    double ewma_err;
} wm_shm_telem_t;

/* Create (fails if the segment exists) / attach an existing one. */
int sdf_wm_shm_create(const char *name, uint32_t n_paths, sdf_shm_t *s);
int sdf_wm_shm_attach(const char *name, uint32_t n_paths, sdf_shm_t *s);

/* Writer: fill the inactive buffer, flip active under the seqlock.
 * prof->version is stored verbatim (callers bump it). Returns 0 or
 * -EINVAL on bad arguments / magic mismatch. */
int sdf_wm_shm_publish(sdf_shm_t *s, const wm_profile_t *prof);

/* Reader: snapshot the active profile under the read seqlock.
 * Returns 0 (prof/version filled) or -EINVAL. Never torn. */
int sdf_wm_shm_acquire(const sdf_shm_t *s, wm_profile_t *prof, uint32_t *version);

/* C data plane -> control loop. Returns 0, -EAGAIN when the ring is
 * full (caller drops or slows; telemetry is best-effort). */
int sdf_wm_shm_telemetry_push(sdf_shm_t *s, const sdf_origin_stats_t *st);

/* C fast path's hard-down set (atomic; design 01 Q3 deference). */
int sdf_wm_shm_down_mask_store(sdf_shm_t *s, uint64_t mask);
uint64_t sdf_wm_shm_down_mask_load(const sdf_shm_t *s);

/* Unmap + close (does not unlink). */
void sdf_wm_shm_close(sdf_shm_t *s);

#ifdef __cplusplus
}
#endif

#endif /* SDF_WM_SHM_H */
