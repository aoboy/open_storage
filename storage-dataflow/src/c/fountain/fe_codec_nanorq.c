/*
 * nanorq arm — the RaptorQ (RFC 6330) fountain backend (plan prompt
 * W3.2, ADR-0004; vendored per import map 7).
 *
 * Block-level adapter over the vendored nanorq's high-level API,
 * following the wack_a_mole src/codec/codec.c @ a0f0422 usage
 * pattern: one SBN per codec object (Z=1, Al=1 forced by the sdf
 * one-block contract, so the OTI is a pure function of (k, sym_len)
 * and the decoder derives it locally — no wire OTI needed), memory
 * ioctx over caller buffers, nanorq_encode for on-demand symbols
 * (systematic ESIs < K, repair >= K), decoder fed symbol-by-symbol
 * with nanorq_repair_block as the decode-sufficiency gate.
 *
 * Heap: the backend allocates the nanorq object + ioctx at init and
 * frees them in destroy — the v2 ABI's lifecycle pair exists exactly
 * for this arm.
 */

#include "fountain_internal.h"

#include <errno.h>

#include "io.h"
#include "nanorq.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    nanorq *rq;
    struct ioctx *io; /* read-only memory ctx over the caller block */
    uint32_t k;
    uint32_t overhead;
    size_t sym_len;
} nrq_enc_t;

typedef struct {
    nanorq *rq;
    struct ioctx *io; /* writable ctx over the repair scratch */
    uint8_t *scratch; /* k * sym_len, decoder-owned */
    uint32_t k;
    size_t sym_len;
    uint32_t pushed;    /* distinct symbols (sources + repairs) */
    uint8_t have[1024]; /* K <= 512 sources plus K repairs */
    int repaired;       /* repair_block already succeeded */
} nrq_dec_t;

size_t nrq_enc_sizeof(void) {
    return sizeof(nrq_enc_t);
}

size_t nrq_dec_sizeof(void) {
    return sizeof(nrq_dec_t);
}

int nrq_enc_init(void *mem, const fe_cfg_t *cfg, const uint8_t *block) {
    if (mem == NULL || cfg == NULL || block == NULL) {
        return -EINVAL;
    }
    if (cfg->k == 0 || cfg->k > 56403u || cfg->block_len == 0 || cfg->block_len % cfg->k != 0) {
        return -EINVAL;
    }
    size_t sym_len = cfg->block_len / cfg->k;
    if (sym_len == 0 || sym_len > 8192u) {
        return -EINVAL;
    }

    nrq_enc_t *e = mem;
    memset(e, 0, sizeof(*e));
    if (!fountain_overhead_count(cfg->k, cfg->overhead_pct, &e->overhead)) {
        return -EINVAL;
    }
    e->k = cfg->k;
    e->sym_len = sym_len;

    e->io = ioctx_from_mem_ro(block, cfg->block_len);
    if (e->io == NULL) {
        return -EINVAL;
    }
    e->rq = nanorq_encoder_new_ex(cfg->block_len, (uint16_t)sym_len, (uint16_t)cfg->k, 1u /* Z */,
                                  1u /* Al */);
    if (e->rq == NULL) {
        e->io->destroy(e->io);
        e->io = NULL;
        return -EINVAL;
    }
    if (nanorq_blocks(e->rq) != 1 || nanorq_block_symbols(e->rq, 0) != cfg->k) {
        nrq_enc_destroy(e);
        return -EINVAL; /* the one-block contract was violated */
    }
    if (!nanorq_generate_symbols(e->rq, 0, e->io)) {
        nrq_enc_destroy(e);
        return -EINVAL;
    }
    return 0;
}

uint32_t nrq_symbol_count(const void *enc) {
    const nrq_enc_t *e = enc;
    if (e == NULL) {
        return 0;
    }
    return e->k + e->overhead;
}

int nrq_generate(void *enc, uint32_t esi, uint8_t *out, size_t out_len) {
    nrq_enc_t *e = enc;
    if (e == NULL || e->rq == NULL || out == NULL || out_len != e->sym_len) {
        return -EINVAL;
    }
    if (esi >= nrq_symbol_count(enc)) {
        return -EINVAL;
    }
    size_t written = nanorq_encode(e->rq, out, esi, 0 /* sbn */, e->io);
    if (written != e->sym_len) {
        return -EINVAL;
    }
    return 0;
}

void nrq_enc_destroy(void *enc) {
    nrq_enc_t *e = enc;
    if (e == NULL) {
        return;
    }
    if (e->rq != NULL) {
        nanorq_encoder_cleanup(e->rq, 0);
        nanorq_free(e->rq);
        e->rq = NULL;
    }
    if (e->io != NULL) {
        e->io->destroy(e->io);
        e->io = NULL;
    }
}

/* The decoder derives the OTI from (k, sym_len) with the same forced
 * Z=1/Al=1 — a throwaway encoder pins the exact values nanorq would
 * put on the wire, so no OTI crosses the sdf ABI. */
static int derive_oti(uint32_t k, size_t sym_len, uint64_t *common, uint32_t *scheme) {
    nanorq *rq = nanorq_encoder_new_ex((size_t)k * sym_len, (uint16_t)sym_len, (uint16_t)k, 1, 1);
    if (rq == NULL) {
        return -EINVAL;
    }
    *common = nanorq_oti_common(rq);
    *scheme = nanorq_oti_scheme_specific(rq);
    nanorq_free(rq);
    return 0;
}

int nrq_dec_init(void *mem, const fd_cfg_t *cfg) {
    if (mem == NULL || cfg == NULL) {
        return -EINVAL;
    }
    if (cfg->k == 0 || cfg->k > 56403u || cfg->sym_len == 0 || cfg->sym_len > 8192u ||
        cfg->k > 512u) {
        return -EINVAL; /* have[] cap: rank telemetry over huge K */
    }

    nrq_dec_t *d = mem;
    memset(d, 0, sizeof(*d));
    d->k = cfg->k;
    d->sym_len = cfg->sym_len;

    uint64_t common = 0;
    uint32_t scheme = 0;
    if (derive_oti(cfg->k, cfg->sym_len, &common, &scheme) != 0) {
        return -EINVAL;
    }

    d->rq = nanorq_decoder_new(common, scheme);
    if (d->rq == NULL) {
        return -EINVAL;
    }
    /* accept repair ESIs (>= K) too: nanorq's default cap is the
     * systematic range — the sdf window is sources + K repairs */
    if (!nanorq_set_max_esi(d->rq, 1u << 20)) {
        nanorq_free(d->rq);
        d->rq = NULL;
        return -EINVAL;
    }

    d->scratch = malloc((size_t)cfg->k * cfg->sym_len);
    if (d->scratch == NULL) {
        nanorq_free(d->rq);
        d->rq = NULL;
        return -ENOMEM;
    }
    d->io = ioctx_from_mem(d->scratch, (size_t)cfg->k * cfg->sym_len);
    if (d->io == NULL) {
        free(d->scratch);
        d->scratch = NULL;
        nanorq_free(d->rq);
        d->rq = NULL;
        return -ENOMEM;
    }
    return 0;
}

int nrq_push(void *dec, uint32_t esi, const uint8_t *sym, size_t len) {
    nrq_dec_t *d = dec;
    if (d == NULL || d->rq == NULL || d->io == NULL || sym == NULL || len != d->sym_len) {
        return -EINVAL;
    }
    if (esi >= d->k + d->k) { /* sources + repair window we track */
        return -EINVAL;
    }
    if (d->have[esi]) {
        return 1; /* duplicate: counted, not an error */
    }
    uint32_t tag = nanorq_tag(0 /* sbn */, esi);
    int rc = nanorq_decoder_add_symbol(d->rq, (void *)sym, tag, d->io);
    if (rc == NANORQ_SYM_DUP) {
        return 1; /* duplicate: counted, not an error */
    }
    if (rc == NANORQ_SYM_IGN) {
        return 1; /* block already complete: the symbol is redundant
                   * (nanorq short-circuits add once no gaps remain —
                   * found live: pushing repairs after a full source set
                   * must not read as an error) */
    }
    if (rc != NANORQ_SYM_ADDED) {
        d->have[esi] = 1; /* reject counted so we never re-feed it */
        return -EINVAL;
    }
    d->have[esi] = 1;
    d->pushed++;
    return 0;
}

bool nrq_ready(const void *dec) {
    nrq_dec_t *d = (nrq_dec_t *)dec; /* repair_block mutates scratch */
    if (d == NULL || d->rq == NULL || d->scratch == NULL || d->io == NULL) {
        return false; /* never report readiness without backend storage */
    }
    if (d->repaired) {
        return true;
    }
    if (nanorq_num_missing(d->rq, 0) == 0) {
        d->repaired = 1;
        return true;
    }
    if (nanorq_repair_block(d->rq, d->io, 0)) {
        d->repaired = 1;
        return true;
    }
    return false;
}

uint32_t nrq_rank(const void *dec) {
    const nrq_dec_t *d = dec;
    return d == NULL ? 0 : d->pushed;
}

int nrq_decode(void *dec, uint8_t *out, size_t block_len) {
    nrq_dec_t *d = dec;
    if (d == NULL || d->rq == NULL || d->scratch == NULL || d->io == NULL || out == NULL ||
        block_len != (size_t)d->k * d->sym_len) {
        return -EINVAL;
    }
    if (!nrq_ready(dec)) {
        return -EINVAL; /* the ARQ-fallback condition (design 02) */
    }
    memcpy(out, d->scratch, block_len);
    return 0;
}

void nrq_dec_destroy(void *dec) {
    nrq_dec_t *d = dec;
    if (d == NULL) {
        return;
    }
    if (d->io != NULL) {
        d->io->destroy(d->io);
        d->io = NULL;
    }
    free(d->scratch);
    d->scratch = NULL;
    if (d->rq != NULL) {
        nanorq_free(d->rq);
        d->rq = NULL;
    }
}
