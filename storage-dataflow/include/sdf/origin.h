#ifndef SDF_ORIGIN_H
#define SDF_ORIGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Origin driver seam — ADR-0008, design docs/design/09.
 *
 * The "paths" of Whack-a-Mole dispatch are ORIGINS: S3 blobs, CDN
 * edges, P2P peers, loopback harness files. This vtable is the only
 * origin I/O surface in the data plane, the storage analogue of
 * wack_a_mole's wam_transport_ops_t (imported as a pattern — the
 * UDP/RoCE drivers themselves are deliberately not imported, import
 * map 4).
 *
 * LINK RULE (ADR-0008): dispatch, controller, FLUID, ARC and the
 * Python plane include only this header; drivers never leak their
 * types above the seam.
 *
 * Unit model: one unit = one byte-range fetch of a fill/repair job.
 * Completion is a callback on the driver's completion context — a
 * stalled origin must never head-of-line-block the dispatch loop.
 * Data in *out is driver-owned until the callback returns.
 *
 * Status convention: callbacks deliver 0 on success and negative
 * errno-style values otherwise (-ERANGE beyond EOF, -EHOSTUNREACH
 * simulated transport loss, -EIO origin error, -ETIMEDOUT). Submit-time
 * returns: -EINVAL bad arguments, -ENOLINK origin drained/disabled,
 * -EAGAIN driver queue full — retry is caller policy.
 */

/** Origin classes (driver selector; also the TOML `cls` values). */
typedef enum {
    SDF_ORIGIN_LOOPBACK = 0, /* harness files with degradation knobs */
    SDF_ORIGIN_HTTP = 1,     /* HTTP/1.1 Range GET reference driver */
    SDF_ORIGIN_S3 = 2,      /* deferred (import map / design 09 §9) */
    SDF_ORIGIN_PEER = 3     /* deferred */
} sdf_origin_cls_t;

#define SDF_ORIGIN_MAX 64        /* matches WM_MAX_PATHS */
#define SDF_ORIGIN_DST_MAX 256   /* file path / URL / endpoint */
#define SDF_ORIGIN_UNIT_MAX (1u << 20) /* largest single fetch (1 MiB) */

/** Fetch request flags. */
#define SDF_FETCH_REPAIR (1u << 0) /* repair-symbol unit (P2) */

/** One unit of work: a byte-range request of a fill/repair job. */
typedef struct {
    uint32_t origin_id; /* 0..n-1, matches the dispatcher's pick */
    uint32_t flags;      /* SDF_FETCH_* */
    uint64_t job_id;    /* fill or repair job */
    uint64_t obj_offset; /* byte range [obj_offset, obj_offset+len) */
    uint32_t len;
    uint32_t _pad;
} sdf_fetch_req_t;

/*
 * Completion: called exactly once per accepted fetch, on the driver's
 * completion context. out/len valid only inside the callback (driver
 * owns the storage); status per the convention above.
 */
typedef void (*sdf_fetch_done_fn)(void *ud, const sdf_fetch_req_t *req,
                                  const uint8_t *out, uint32_t len,
                                  int status);

/** Per-origin configuration (the [[origin]] TOML block, ADR-0005). */
typedef struct {
    uint32_t id;    /* 0..n-1, dense and unique across one open() */
    uint32_t cls;   /* sdf_origin_cls_t */
    char dst[SDF_ORIGIN_DST_MAX]; /* path (loopback) / URL (http) */
    /* harness + driver knobs */
    uint64_t throttle_ns;  /* loopback: per-request artificial delay */
    double loss_pct;        /* loopback: simulated transport loss 0..100 */
    double err_pct;         /* loopback: simulated origin errors 0..100 */
    uint64_t lat_jitter_ns; /* loopback: uniform jitter added to throttle */
    uint32_t max_inflight;  /* queue bound (total slots: n * max_inflight) */
    uint32_t timeout_ms;    /* http: socket send/recv timeout */
} sdf_origin_cfg_t;

/*
 * Normalized per-origin stats — the ONLY signal the severity
 * controller and the Python loop consume (wam_path_stats_t analogue).
 * Snapshot semantics: filled by ops->stats, no locks held.
 *
 * goodput is the cumulative average bytes/s since open (driver-exact
 * and testable); windowed rates are the controller's job via deltas.
 */
typedef struct {
    uint32_t id;
    uint32_t cls;
    uint64_t reqs_out;   /* accepted submits */
    uint64_t reqs_done;  /* successful completions */
    uint64_t reqs_failed; /* failed completions (loss/err/timeout/range) */
    uint64_t bytes_tx;   /* request bytes accounted */
    uint64_t bytes_rx;   /* response bytes accounted */
    uint64_t in_flight;  /* accepted, not yet completed */
    double ewma_lat_ns;  /* submit-to-completion EWMA (throttle included) */
    double ewma_err;     /* failed/done EWMA over completions */
    double goodput;      /* bytes_rx / seconds since open */
    int hard_down;       /* unavailable for new work: driver-detected
                           failure (conn refused, DNS) OR administratively
                           drained via set_enable(0) — the controller's
                           deterministic down-signal */
} sdf_origin_stats_t;

/** Driver instance: vtable + private implementation. */
typedef struct sdf_origin sdf_origin_t;

typedef struct {
    /*
     * Open the driver for n origins. cfgs must be dense (cfgs[i].id
     * == i is the canonical layout; ids are validated unique and
     * < n). Returns 0, or negative errno-style on bad config / I/O
     * failure (loopback: dst must exist and be readable).
     */
    int (*open)(sdf_origin_t *o, const sdf_origin_cfg_t *cfgs, uint32_t n);

    /*
     * Submit one fetch unit. Returns 0 when accepted (the callback
     * will fire exactly once), -EINVAL on bad arguments/ranges,
     * -ENOLINK when the origin is drained, -EAGAIN when the driver
     * queue is full. Never blocks.
     */
    int (*fetch)(sdf_origin_t *o, const sdf_fetch_req_t *req,
                 sdf_fetch_done_fn cb, void *ud);

    /* Snapshot one origin's stats. Returns 0 or -EINVAL. */
    int (*stats)(sdf_origin_t *o, uint32_t origin_id, sdf_origin_stats_t *st);

    /*
     * Drain/restore an origin (the controlled counterpart to the C
     * fast path's wm_mark_down): down => new fetches fail fast with
     * -ENOLINK while in-flight units complete normally. Returns 0 or
     * -EINVAL.
     */
    int (*set_enable)(sdf_origin_t *o, uint32_t origin_id, int up);

    /* Stop the driver and release resources; in-flight completions
     * are flushed (callbacks fire) before return. NULL is a no-op. */
    void (*close)(sdf_origin_t *o);
} sdf_origin_ops_t;

struct sdf_origin {
    const sdf_origin_ops_t *ops;
    void *impl;
};

/** Class name for dumps/diagnostics ("loopback", "http", ...). */
const char *sdf_origin_cls_name(uint32_t cls);

#ifdef __cplusplus
}
#endif

#endif /* SDF_ORIGIN_H */
