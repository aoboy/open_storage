/*
 * LT arm — placeholder until plan prompt W3.3 (the in-repo reference
 * codec; the documented import-first exception). The front-end needs
 * the symbols to link, so the sizes/destroy are real and every
 * operation returns -ENOSYS; W3.3 replaces this file with the
 * systematic LT + peeling decoder and fills the golden entries.
 */

#include "fountain_internal.h"

#include <errno.h>

size_t lt_enc_sizeof(void) { return 8; }
size_t lt_dec_sizeof(void) { return 8; }

int lt_enc_init(void *mem, const fe_cfg_t *cfg, const uint8_t *block) {
    (void)mem;
    (void)cfg;
    (void)block;
    return -ENOSYS;
}

int lt_dec_init(void *mem, const fd_cfg_t *cfg) {
    (void)mem;
    (void)cfg;
    return -ENOSYS;
}

uint32_t lt_symbol_count(const void *enc) {
    (void)enc;
    return 0;
}

int lt_generate(void *enc, uint32_t esi, uint8_t *out, size_t out_len) {
    (void)enc;
    (void)esi;
    (void)out;
    (void)out_len;
    return -ENOSYS;
}

int lt_push(void *dec, uint32_t esi, const uint8_t *sym, size_t len) {
    (void)dec;
    (void)esi;
    (void)sym;
    (void)len;
    return -ENOSYS;
}

bool lt_ready(const void *dec) {
    (void)dec;
    return false;
}

uint32_t lt_rank(const void *dec) {
    (void)dec;
    return 0;
}

int lt_decode(void *dec, uint8_t *out, size_t block_len) {
    (void)dec;
    (void)out;
    (void)block_len;
    return -ENOSYS;
}

void lt_enc_destroy(void *enc) { (void)enc; }
void lt_dec_destroy(void *dec) { (void)dec; }
