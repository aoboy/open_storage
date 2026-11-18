/*
 * Fountain encoder front-end (plan prompt W3.2): the ONLY definition of
 * the public fe_* symbols. Dispatches on fe_cfg_t.codec to the arms
 * (ADR-0004: nanorq and LT behind one ABI); the public object is a
 * union of the arm states plus the arm tag.
 */

#include "fountain_internal.h"

#include <errno.h>

#include <string.h>

struct fountain_enc {
    uint32_t codec;
    uint32_t _pad;
    unsigned char arm[]; /* exactly max(lt, nrq) size */
};

size_t fe_sizeof(const fe_cfg_t *cfg) {
    if (cfg == NULL || cfg->k == 0 || cfg->block_len == 0)
        return 0;
    if (cfg->block_len % cfg->k != 0)
        return 0;
    size_t arm = cfg->codec == SDF_FOUNTAIN_NANORQ ? nrq_enc_sizeof()
                                                   : lt_enc_sizeof();
    return sizeof(struct fountain_enc) + arm;
}

int fe_init_in(fountain_enc_t *e, const fe_cfg_t *cfg, const uint8_t *block) {
    if (e == NULL || cfg == NULL || block == NULL || fe_sizeof(cfg) == 0)
        return -EINVAL;
    struct fountain_enc *fe = (struct fountain_enc *)e;
    memset(fe, 0, sizeof(*fe));
    fe->codec = cfg->codec;
    int rc = cfg->codec == SDF_FOUNTAIN_NANORQ
                 ? nrq_enc_init(fe->arm, cfg, block)
                 : lt_enc_init(fe->arm, cfg, block);
    if (rc != 0) {
        fe->codec = 0xffff; /* poisoned: only destroy is valid */
        return rc;
    }
    return 0;
}

int fe_init(fountain_enc_t *e, const uint8_t *block, size_t block_len,
            uint32_t k, uint32_t overhead_pct) {
    /* v1 wrapper: the LT reference arm (the simpler default). */
    fe_cfg_t cfg = {
        .block_len = block_len, .k = k, .overhead_pct = overhead_pct,
        .codec = SDF_FOUNTAIN_LT,
    };
    return fe_init_in(e, &cfg, block);
}

uint32_t fe_symbol_count(const fountain_enc_t *e) {
    const struct fountain_enc *fe = (const struct fountain_enc *)e;
    if (fe == NULL || fe->codec == 0xffff)
        return 0;
    return fe->codec == SDF_FOUNTAIN_NANORQ ? nrq_symbol_count(fe->arm)
                                            : lt_symbol_count(fe->arm);
}

int fe_generate(fountain_enc_t *e, uint32_t esi, uint8_t *out, size_t out_len) {
    struct fountain_enc *fe = (struct fountain_enc *)e;
    if (fe == NULL || fe->codec == 0xffff)
        return -EINVAL;
    return fe->codec == SDF_FOUNTAIN_NANORQ
               ? nrq_generate(fe->arm, esi, out, out_len)
               : lt_generate(fe->arm, esi, out, out_len);
}

void fe_destroy(fountain_enc_t *e) {
    struct fountain_enc *fe = (struct fountain_enc *)e;
    if (fe == NULL)
        return;
    if (fe->codec == SDF_FOUNTAIN_NANORQ)
        nrq_enc_destroy(fe->arm);
    else if (fe->codec == SDF_FOUNTAIN_LT)
        lt_enc_destroy(fe->arm);
    /* storage stays caller-owned */
}
