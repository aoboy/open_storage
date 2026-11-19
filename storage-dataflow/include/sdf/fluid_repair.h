#ifndef SDF_FLUID_REPAIR_H
#define SDF_FLUID_REPAIR_H

/*
 * FLUID repair coordinator (plan prompt W3.5, design 02).
 *
 * One repair job: a K-symbol block repaired across origins through the
 * ADR-0008 seam. The sender's symbol stream is precomputed into the
 * origin data files (symbols are deterministic from the ESI — modeling
 * the sender); the coordinator sprays fetches via wm_select with the
 * C controller's stats-driven profile, loopback loss_pct models the
 * WAN, the FLUID budget admits per wave, the gate certifies, and —
 * when the gate fires before decode sufficiency — the mandatory ARQ
 * fallback re-fetches the missing systematic ESIs (never ship an
 * incomplete block; design 02's safety).
 *
 * Arms (the W3.6 A/B):
 *   control — plain ARQ: fetch all K systematic ESIs with retry until
 *             every one lands (the baseline; eps is ignored).
 *   lt / nanorq — FLUID + the codec arm: budget N = ceil(K/(1-eps)),
 *             overhead mapped as eps/(1-eps) (the wam [fluid] rule).
 */

#include "sdf/fluid.h"
#include "sdf/origin.h"
#include "sdf/wm.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SDF_REPAIR_CONTROL = 0, /* plain ARQ baseline */
    SDF_REPAIR_LT = 1,
    SDF_REPAIR_NANORQ = 2,
} sdf_repair_arm_t;

#define SDF_REPAIR_MAX_ORIGINS 8

typedef struct {
    sdf_repair_arm_t arm;
    uint32_t k;       /* source symbols in the block */
    uint32_t sym_len; /* symbol bytes */
    double eps;       /* slack (ignored by the control arm) */
    double loss_pct;  /* loopback injected transport loss, 0..100 */

    /* origins: loopback files created by the harness caller */
    sdf_origin_cfg_t origins[SDF_REPAIR_MAX_ORIGINS];
    uint32_t n_origins;
    uint32_t wave; /* symbols per admit wave (default 16) */
} sdf_fluid_repair_cfg_t;

typedef struct {
    uint64_t sent;     /* symbols submitted */
    uint64_t received; /* symbols that landed (post-loss) */
    uint64_t bytes_tx;
    uint64_t bytes_rx;
    uint64_t rounds;  /* admit waves */
    int arq_fallback; /* 1 when the gate fired before sufficiency */
    int ok;           /* 0 when even ARQ could not complete */
    uint32_t decode_rank;
    uint64_t wall_ns;
} sdf_fluid_repair_stats_t;

/*
 * Run one repair job synchronously. The caller pre-creates the origin
 * data files (at least K * sym_len + repairs worth of encoded stream —
 * see sdf_fluid_repair_stream_bytes). Returns 0 and fills out.
 */
int sdf_fluid_repair_run(const sdf_fluid_repair_cfg_t *cfg, sdf_fluid_repair_stats_t *out);

/* Bytes the origin files must hold for the arm's symbol stream. */
uint64_t sdf_fluid_repair_stream_bytes(const sdf_fluid_repair_cfg_t *cfg);

/* Encode the symbol stream into buf (stream_bytes) — the harness
 * writes it to the origin files before the run. Returns 0 or -EINVAL. */
int sdf_fluid_repair_encode_stream(const sdf_fluid_repair_cfg_t *cfg, uint8_t *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* SDF_FLUID_REPAIR_H */
