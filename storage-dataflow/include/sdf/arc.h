#ifndef SDF_ARC_H
#define SDF_ARC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ARC — Adaptive Release Control ABI v2 (ADR-0006; [ARC] section 4,
 * plan prompt W4.1/W4.2).
 *
 * The mandatory receiver-side release scheduler: per-block (S_n, A_n)
 * pairs adapt an offset D (Algorithm 1, asymmetric update clamped to
 * the observed envelope), ARC-Q quantizes it into E (Algorithm 2,
 * midpoint hysteresis: E >= D, 0 <= E-D < gamma), and ARC-QG orders
 * releases behind a bounded guard G (Algorithm 3: a held successor
 * wakes right after its predecessor; a missing predecessor parks
 * until T_n + G and then releases — data always flows). The
 * receiver clock alone: producer_ts is the sender's stamp S_n and no
 * clock synchronization is required (the paper's property, kept).
 *
 * Modes: BASE (Algorithm 1 only), Q (quantized E), QG (the production
 * default: ordered release), IMMEDIATE (the killed-mode oracle —
 * releases at arrival; test/experiment use only).
 *
 * v1's simplified estimator knobs (offset_min/max, increase/decrease
 * gains) are superseded by the paper parameter set below; the drain
 * knobs (batch_max, batch_deadline_ns) stay: they bound the sidecar's
 * poll loop, not the schedule. v1's arc_run/arc_stop are replaced by
 * the sidecar-owned arc_poll loop (W4.3).
 */

typedef enum {
    ARC_MODE_BASE = 0,
    ARC_MODE_Q = 1,
    ARC_MODE_QG = 2,       /* production default */
    ARC_MODE_IMMEDIATE = 3 /* the killed-mode oracle; never production */
} arc_mode_t;

/* Paper parameter set ([ARC] section 9 defaults). All times in ns. */
typedef struct {
    uint32_t capacity;      /* release-queue slots (power of two >= 2) */
    uint32_t entry_max;    /* slab slot bytes; larger submits -EINVAL */
    arc_mode_t mode;
    double rho_u;     /* update exponent, late arrivals (0.3..0.7) */
    double rho_l;     /* update exponent, early arrivals (1.0..2.0) */
    double lambda_u;  /* update gain, late (0.01..1.0) */
    double lambda_l;  /* update gain, early (0.01..0.10) */
    uint64_t U;       /* update clip window (50..200 ms) */
    uint64_t delta;   /* envelope clamp (30..100 ms) */
    uint64_t gamma;   /* ARC-Q step (8..20 ms) */
    uint64_t J;       /* neutral band (0..5 ms) */
    uint64_t G;       /* QG guard (20..200 ms) */
    uint64_t T_idle_ns; /* idle re-anchor threshold (0.5..2 s) */
    int auto_params;  /* U from RTT, delta from delta-window quantiles,
                         G from reorder quantiles (paper 9.2) */
    double c_U;       /* auto U multiplier (0.5..2.0) */
    double c_delta;   /* auto delta multiplier */
    uint64_t rtt_sm_ns; /* smoothed RTT for auto-U (0 = auto off) */
    uint64_t min_smooth_bytes; /* smaller entries bypass (release at A_n) */
    uint64_t budget_ns;  /* p99 added-delay alert bound (0 = off) */
    /* apply-side drain knobs (the sidecar's poll loop bound) */
    uint32_t batch_max;
    uint64_t batch_deadline_ns;
} arc_cfg_t;

/* Apply callback: entry bytes are slab-owned and valid only inside the
 * call; release_ts is the ARC release time (T_n at drain). */
typedef int (*arc_apply_fn)(void *ctx, const void *entry, size_t len,
                            uint64_t release_ts);

/* Stats snapshot (per design 03 §Metrics; the reservoir percentiles
 * are exact while count <= the internal window). */
typedef struct {
    double D;      /* current offset (ns) */
    double E;      /* quantized offset (BASE: == D) */
    uint64_t releases;
    uint64_t in_flight;
    uint32_t heap_depth;
    uint64_t idle_reanchors;
    uint64_t guard_waits;
    uint64_t guard_timeouts;
    uint64_t s_n_rejects; /* absurd-stamp blocks scheduled from A_n */
    uint64_t delta_clamps; /* schedules clamped to A_n + delta */
    uint64_t budget_alerts;
    uint64_t added_count;
    double added_p50_ns, added_p95_ns, added_p99_ns;
    uint64_t interval_error_max_ns; /* max |release gap - producer gap| */
} arc_stats_t;

typedef struct arc_pacer arc_pacer_t;

/* Bytes the caller-provided object needs (0 on bad configuration). */
size_t arc_sizeof(const arc_cfg_t *cfg);

/* In-place init over exactly arc_sizeof(cfg) bytes. 0 or -EINVAL. */
int arc_init(arc_pacer_t *p, const arc_cfg_t *cfg, arc_apply_fn fn,
             void *ctx);

/*
 * Submit one recovered entry. producer_ts IS the sender's stamp S_n
 * ([ARC] section 4.3); the arrival A_n is sampled on the receiver
 * clock inside submit. The entry bytes are copied into the slab.
 * Returns 0 when queued, 1 when small enough to bypass scheduling
 * (released at the next poll), -EAGAIN when the queue is full, -EINVAL
 * on bad arguments.
 */
int arc_submit(arc_pacer_t *p, const void *entry, size_t len,
               uint64_t producer_ts);

/*
 * Release every entry whose T_n <= now (in queue order under QG —
 * the ordering guarantee), bounded by batch_max or batch_deadline_ns
 * elapsed. Returns the number released. The sidecar calls this in
 * its poll loop (W4.3).
 */
int arc_poll(arc_pacer_t *p, uint64_t now_ns);

/* Snapshot the stats (safe any time). */
arc_stats_t arc_stats(const arc_pacer_t *p);

/* Feed the RTT estimator for auto_params (ARC auto-U). */
void arc_set_rtt(arc_pacer_t *p, uint64_t rtt_sm_ns);

#ifdef __cplusplus
}
#endif

#endif /* SDF_ARC_H */

/*
 * Sidecar (plan prompt W4.3, design 03): entries in over an SPSC ring,
 * released through the apply callback on the receiver clock. Modes:
 * shadow (0) computes the schedule + metrics, applies immediately;
 * live (1) ARC owns the timing; killed (2) the IMMEDIATE oracle —
 * the design-03 kill switch. One pacer per process (the sidecar is a
 * singleton, like the wam apps' ARC stage).
 */
int arc_sidecar_start(const arc_cfg_t *cfg, int mode);
int arc_sidecar_submit(const void *entry, size_t len, uint64_t producer_ts);
int arc_sidecar_pump_once(void);
void arc_sidecar_stop(void);
arc_stats_t arc_sidecar_stats(void);
uint64_t arc_sidecar_applied(void);
int arc_sidecar_mode(void);

/* W6.1: live scrape over the sidecar's own dump (start() first; the
 * endpoint stops with the sidecar or serve_stop()). */
int arc_sidecar_serve(const char *addr);
void arc_sidecar_serve_stop(void);
