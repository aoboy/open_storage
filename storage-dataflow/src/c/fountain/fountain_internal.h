#ifndef SDF_FOUNTAIN_INTERNAL_H
#define SDF_FOUNTAIN_INTERNAL_H

/*
 * Internal codec-arm contract (plan W3.2/W3.3, ADR-0004).
 *
 * fe_enc.c and fe_dec.c are the ONLY definitions of the public encoder
 * and decoder symbols; they dispatch on fe_cfg_t.codec to the arms below.
 * Arms are
 * memory-layout compatible: each exposes its struct size and its
 * init/generate/push/ready/rank/decode/destroy over a caller-provided
 * region of exactly that size (the public structs are a union of the
 * arms, so one allocation serves either).
 */

#include "sdf/fountain.h"

#include <stddef.h>
#include <stdint.h>

/* ---- LT arm (plan W3.3, in-repo reference codec) ----------------------- */

size_t lt_enc_sizeof(void);
size_t lt_dec_sizeof(void);
int lt_enc_init(void *mem, const fe_cfg_t *cfg, const uint8_t *block);
int lt_dec_init(void *mem, const fd_cfg_t *cfg);
uint32_t lt_symbol_count(const void *enc);
int lt_generate(void *enc, uint32_t esi, uint8_t *out, size_t out_len);
int lt_push(void *dec, uint32_t esi, const uint8_t *sym, size_t len);
bool lt_ready(const void *dec);
uint32_t lt_rank(const void *dec);
int lt_decode(void *dec, uint8_t *out, size_t block_len);
void lt_enc_destroy(void *enc);
void lt_dec_destroy(void *dec);

/* ---- nanorq arm (plan W3.2, vendored RaptorQ per import map 7) -------- */

size_t nrq_enc_sizeof(void);
size_t nrq_dec_sizeof(void);
int nrq_enc_init(void *mem, const fe_cfg_t *cfg, const uint8_t *block);
int nrq_dec_init(void *mem, const fd_cfg_t *cfg);
uint32_t nrq_symbol_count(const void *enc);
int nrq_generate(void *enc, uint32_t esi, uint8_t *out, size_t out_len);
int nrq_push(void *dec, uint32_t esi, const uint8_t *sym, size_t len);
bool nrq_ready(const void *dec);
uint32_t nrq_rank(const void *dec);
int nrq_decode(void *dec, uint8_t *out, size_t block_len);
void nrq_enc_destroy(void *enc);
void nrq_dec_destroy(void *dec);

#endif /* SDF_FOUNTAIN_INTERNAL_H */

/* xorshift64* (fe_prng.c): deterministic per-ESI streams. */
uint64_t lt_prng_next(uint64_t *state);
