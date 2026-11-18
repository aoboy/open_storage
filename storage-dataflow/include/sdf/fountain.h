#ifndef SDF_FOUNTAIN_H
#define SDF_FOUNTAIN_H

/*
 * Fountain codec ABI v2 (plan prompt W3.1; the ADR-0001 opaque-size
 * open item closes here).
 *
 * One codec object = ONE block of K symbols of sym_len bytes; the
 * caller pads the block to exactly K * sym_len and loops multi-block
 * objects at the repair coordinator. ESI semantics are explicit:
 * ESIs 0..K-1 address the systematic source symbols, ESIs >= K
 * address repair symbols generated on demand (the codec derives
 * repair content deterministically from the ESI alone — a decoder
 * fed the same ESI reproduces the same symbol without side
 * channels). fd_push rejects duplicates; fd_ready is the
 * decode-sufficiency gate the FLUID coordinator polls before
 * shipping a block (design 02).
 *
 * Size contract: fe_sizeof/fd_sizeof return the exact bytes the
 * caller-provided storage needs; init_in-place forms take that
 * storage. The v1 fe_init/fd_init remain as thin wrappers.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encoder configuration (one block). */
typedef struct {
    size_t block_len;   /* == k * sym_len exactly (caller pads) */
    uint32_t k;         /* source symbols per block (>= 1) */
    uint32_t overhead_pct; /* repair symbols exposed, percent of K */
} fe_cfg_t;

/* Decoder configuration (one block). */
typedef struct {
    uint32_t k;
    size_t sym_len;
} fd_cfg_t;

/* Bytes of storage a codec object needs (0 on bad configuration). */
size_t fe_sizeof(const fe_cfg_t *cfg);
size_t fd_sizeof(const fd_cfg_t *cfg);

typedef struct fountain_enc fountain_enc_t;
typedef struct fountain_dec fountain_dec_t;

/* In-place initialization over caller storage of exactly
 * fe_sizeof/fd_sizeof bytes. Return 0 or -EINVAL. */
int fe_init_in(fountain_enc_t *e, const fe_cfg_t *cfg, const uint8_t *block);
int fd_init_in(fountain_dec_t *d, const fd_cfg_t *cfg);

/* v1 entry points (wrappers over the in-place forms). */
int fe_init(fountain_enc_t *e, const uint8_t *block, size_t block_len,
            uint32_t k, uint32_t overhead_pct);
int fd_init(fountain_dec_t *d, uint32_t k, size_t sym_len);

/* Total symbols the encoder exposes: K systematic + overhead repair. */
uint32_t fe_symbol_count(const fountain_enc_t *e);

/*
 * Emit the symbol for an ESI into out (exactly the codec's sym_len
 * bytes). Repair ESIs >= fe_symbol_count() are -EINVAL. Returns 0
 * or -EINVAL.
 */
int fe_generate(fountain_enc_t *e, uint32_t esi, uint8_t *out, size_t out_len);

/*
 * Feed one received symbol. Returns 0 (added), 1 (duplicate —
 * counted, not an error) or -EINVAL (bad ESI/length).
 */
int fd_push(fountain_dec_t *d, uint32_t esi, const uint8_t *sym, size_t len);

/* Decode sufficiency: can fd_decode reconstruct the block now? */
bool fd_ready(const fountain_dec_t *d);

/* Number of distinct symbols held (telemetry: fluid_decode_rank). */
uint32_t fd_rank(const fountain_dec_t *d);

/* Reconstruct the block into out (block_len bytes). -EINVAL when
 * not fd_ready — the coordinator falls back to ARQ (design 02). */
int fd_decode(fountain_dec_t *d, uint8_t *out, size_t block_len);

#ifdef __cplusplus
}
#endif

#endif /* SDF_FOUNTAIN_H */
