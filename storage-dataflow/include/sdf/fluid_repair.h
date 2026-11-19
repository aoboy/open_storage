#ifndef SDF_FLUID_REPAIR_H
#define SDF_FLUID_REPAIR_H

/*
 * FLUID repair coordinator (plan prompts W3.5, W6.3; design 02).
 *
 * One repair job: an N-block object repaired across origins through
 * the ADR-0008 seam. Each block is its own fountain codec object
 * (include/sdf/fountain.h's one-block contract) with its own budget
 * and gate; the coordinator loops blocks and schedules symbols by
 * decode-rank deficit. The sender's symbol stream is precomputed into
 * the origin data files (symbols are deterministic from the ESI —
 * modeling the sender); the coordinator sprays fetches via wm_select
 * with the C controller's stats-driven profile, loopback loss_pct
 * models the WAN, the FLUID budget admits per wave, the gate
 * certifies, and — when the gate fires before decode sufficiency —
 * the mandatory per-block ARQ fallback re-fetches the missing
 * systematic ESIs (never ship an incomplete block; design 02's
 * safety).
 *
 * Arms (the W3.6 A/B):
 *   control — plain ARQ: fetch all K systematic ESIs with retry until
 *             every one lands (the baseline; eps is ignored).
 *   lt / nanorq — FLUID + the codec arm: budget N = ceil(K/(1-eps)),
 *             overhead mapped as eps/(1-eps) (the wam [fluid] rule).
 *
 * Block layout: uniform geometry. Block b's stream starts at
 * b * (stream_symbols * sym_len) in every origin file, and decoded
 * block b is byte-exact-verified against the deterministic sender
 * block for b. `n_blocks == 0` or 1 is the legacy single-block job.
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
#define SDF_REPAIR_MAX_BLOCKS 64

typedef struct {
    sdf_repair_arm_t arm;
    uint32_t k;       /* source symbols in each block */
    uint32_t sym_len; /* symbol bytes */
    double eps;       /* slack (ignored by the control arm) */
    double loss_pct;  /* loopback injected transport loss, 0..100 */

    /* origins: loopback files created by the harness caller */
    sdf_origin_cfg_t origins[SDF_REPAIR_MAX_ORIGINS];
    uint32_t n_origins;
    uint32_t wave; /* symbols per admit round (default 16) */

    /* W6.3: blocks in the object; 0 or 1 = the legacy single-block
     * job, up to SDF_REPAIR_MAX_BLOCKS. All blocks share k/sym_len. */
    uint32_t n_blocks;
} sdf_fluid_repair_cfg_t;

/* Per-block evidence (W6.3: the REPAIR line's per-block fields). */
typedef struct {
    uint64_t sent;     /* symbols submitted for this block */
    uint64_t received; /* symbols that landed for this block */
    uint64_t bytes_tx;
    uint64_t bytes_rx;
    uint64_t rounds;  /* coordinator rounds that admitted >= 1 symbol */
    int arq_fallback; /* 1 when this block needed the fallback */
    int ok;           /* 1 only when decoded bytes matched the sender */
    uint32_t decode_rank;
} sdf_fluid_repair_block_stats_t;

typedef struct {
    /* Aggregates over blocks (identical to the block for N == 1). */
    uint64_t sent;     /* symbols submitted */
    uint64_t received; /* symbols that landed (post-loss) */
    uint64_t bytes_tx;
    uint64_t bytes_rx;
    uint64_t rounds;      /* coordinator admit rounds */
    int arq_fallback;     /* 1 when any block fired the fallback */
    int ok;               /* 1 only when every block decoded byte-exact */
    uint32_t decode_rank; /* minimum rank across blocks (worst block) */
    uint64_t wall_ns;

    /* W6.3 per-block evidence, valid for indexes < n_blocks. */
    uint32_t n_blocks;
    sdf_fluid_repair_block_stats_t blocks[SDF_REPAIR_MAX_BLOCKS];
} sdf_fluid_repair_stats_t;

/*
 * Run one repair job synchronously. The caller pre-creates the origin
 * data files (at least sdf_fluid_repair_stream_bytes — see below).
 * Returns 0 and fills out. `out.ok` is the byte-exact gate: every
 * block was decoded AND compared against the expected sender block.
 */
int sdf_fluid_repair_run(const sdf_fluid_repair_cfg_t *cfg, sdf_fluid_repair_stats_t *out);

/* Bytes the origin files must hold for the arm's symbol stream
 * (all blocks: n_blocks * per-block stream bytes). */
uint64_t sdf_fluid_repair_stream_bytes(const sdf_fluid_repair_cfg_t *cfg);

/* Encode the symbol stream into buf (stream_bytes) — the harness
 * writes it to the origin files before the run. Each block carries a
 * distinct deterministic payload so a mis-routed block cannot pass
 * the byte-exact gate. Returns 0 or -EINVAL. */
int sdf_fluid_repair_encode_stream(const sdf_fluid_repair_cfg_t *cfg, uint8_t *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* SDF_FLUID_REPAIR_H */
