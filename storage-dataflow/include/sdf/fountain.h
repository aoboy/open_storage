#ifndef SDF_FOUNTAIN_H
#define SDF_FOUNTAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opaque fountain encoder / decoder. The concrete layout is private to the
 * codec implementation; callers must allocate and align the object as
 * required by that implementation.
 */
typedef struct fountain_enc fountain_enc_t;
typedef struct fountain_dec fountain_dec_t;

int fe_init(fountain_enc_t *e, const uint8_t *block, size_t block_len, uint32_t k,
            uint32_t overhead_pct);
uint32_t fe_symbol_count(const fountain_enc_t *e);
int fe_generate(fountain_enc_t *e, uint32_t esi, uint8_t *out, size_t out_len);

int fd_init(fountain_dec_t *d, uint32_t k, size_t sym_len);
int fd_push(fountain_dec_t *d, uint32_t esi, const uint8_t *sym, size_t len);
bool fd_ready(const fountain_dec_t *d);
int fd_decode(fountain_dec_t *d, uint8_t *out, size_t block_len);

#ifdef __cplusplus
}
#endif

#endif /* SDF_FOUNTAIN_H */
