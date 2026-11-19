/*
 * LT arm — the in-repo systematic Luby-Transform reference codec
 * (plan prompt W3.3, ADR-0004; the documented import-first exception:
 * no wack_a_mole LT exists — import map 3.2).
 *
 * Scope (documented contract of the reference arm): small blocks,
 * K <= 64, sym_len <= 256 — production sizes ride the nanorq arm.
 * This is the A/B baseline that isolates the codec effect (design 02
 * / ADR-0004): identical fountain semantics, higher reception
 * overhead than RaptorQ.
 *
 * Symbol contract: ESIs 0..K-1 are the systematic sources. A repair
 * at ESI r >= K is DETERMINISTIC from r alone: xorshift64* seeded
 * with r draws a degree from the Robust Soliton distribution (with a
 * uniform small-K fallback), then that many distinct source
 * positions; the symbol is their XOR. The decoder derives the same
 * neighbor set from the ESI, so no coefficient side channel exists.
 *
 * Decode is peeling: subtract known neighbors from each repair;
 * when a repair's unresolved count hits one, its last unknown source
 * is recovered; iterate to a fixed point. fd_ready is the fixed
 * point with all K sources known.
 */

#include "fountain_internal.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LT_MAX_K 64u
#define LT_MAX_SYM 256u

/* Repair window: ESIs K..2K-1 (mirrors the nanorq arm's push bound). */
#define LT_REPAIR_MAX (LT_MAX_K)

typedef struct {
    uint32_t k;
    uint32_t overhead;
    size_t sym_len;
    const uint8_t *block;
} lt_enc_t;

typedef struct {
    uint32_t k;
    size_t sym_len;
    uint8_t have_src[LT_MAX_K]; /* received source bitmap */
    uint8_t src[LT_MAX_K][LT_MAX_SYM];
    uint8_t have_rep[LT_REPAIR_MAX]; /* repair bitmap (index r-K) */
    uint32_t rep_deg[LT_REPAIR_MAX]; /* current unresolved degree */
    uint16_t rep_nb[LT_REPAIR_MAX][LT_MAX_K];
    uint8_t rep[LT_REPAIR_MAX][LT_MAX_SYM];
} lt_dec_t;

size_t lt_enc_sizeof(void) {
    return sizeof(lt_enc_t);
}

size_t lt_dec_sizeof(void) {
    return sizeof(lt_dec_t);
}

/* Robust Soliton degree for K, drawn from the PRNG stream. Falls
 * back to uniform 1..K for the tiny-K regime where the distribution
 * degenerates (documented; the reference arm's lab sizes). */
static uint32_t draw_degree(uint64_t *state, uint32_t k) {
    if (k < 8u)
        return 1u + (uint32_t)(lt_prng_next(state) % k);

    const double c = 0.1, delta = 0.5;
    double R = c * log((double)k / delta) * sqrt((double)k);
    double kr = (double)k / R;
    uint32_t k_r = kr >= 1.0 ? (uint32_t)kr : 1u;

    double rho[LT_MAX_K + 1];
    double tau[LT_MAX_K + 1];
    double mu[LT_MAX_K + 1];
    double z = 0.0;
    for (uint32_t d = 1; d <= k; d++) {
        rho[d] = d == 1 ? 1.0 / (double)k : 1.0 / ((double)d * (double)(d - 1));
        tau[d] = 0.0;
        if (d < k_r)
            tau[d] = R / ((double)d * (double)k);
        else if (d == k_r)
            tau[d] = R / (double)k * log(R / delta);
        mu[d] = rho[d] + tau[d];
        z += mu[d];
    }
    double u = (double)(lt_prng_next(state) % 1000000u) / 1000000.0 * z;
    double acc = 0.0;
    for (uint32_t d = 1; d <= k; d++) {
        acc += mu[d];
        if (u <= acc)
            return d;
    }
    return k; /* rounding tail */
}

/* The deterministic neighbor set for repair ESI r (shared by encoder
 * and decoder — the ESI contract). */
static void repair_neighbors(uint32_t k, uint32_t r, uint16_t *nb, uint32_t *deg_out) {
    uint64_t state = (uint64_t)r * 0x9E3779B97F4A7C15ull + 1u;
    uint32_t d = draw_degree(&state, k);
    uint32_t picked = 0;
    uint8_t seen[LT_MAX_K] = {0};
    while (picked < d && picked < k) {
        uint32_t p = (uint32_t)(lt_prng_next(&state) % k);
        if (seen[p])
            continue;
        seen[p] = 1;
        nb[picked++] = (uint16_t)p;
    }
    *deg_out = picked;
}

int lt_enc_init(void *mem, const fe_cfg_t *cfg, const uint8_t *block) {
    if (mem == NULL || cfg == NULL || block == NULL)
        return -EINVAL;
    if (cfg->k == 0 || cfg->k > LT_MAX_K || cfg->block_len == 0 || cfg->block_len % cfg->k != 0)
        return -EINVAL;
    size_t sym_len = cfg->block_len / cfg->k;
    if (sym_len == 0 || sym_len > LT_MAX_SYM)
        return -EINVAL;

    lt_enc_t *e = mem;
    memset(e, 0, sizeof(*e));
    e->k = cfg->k;
    e->sym_len = sym_len;
    e->overhead = cfg->k * cfg->overhead_pct / 100u;
    e->block = block;
    return 0;
}

int lt_dec_init(void *mem, const fd_cfg_t *cfg) {
    if (mem == NULL || cfg == NULL)
        return -EINVAL;
    if (cfg->k == 0 || cfg->k > LT_MAX_K || cfg->sym_len == 0 || cfg->sym_len > LT_MAX_SYM)
        return -EINVAL;
    lt_dec_t *d = mem;
    memset(d, 0, sizeof(*d));
    d->k = cfg->k;
    d->sym_len = cfg->sym_len;
    return 0;
}

uint32_t lt_symbol_count(const void *enc) {
    const lt_enc_t *e = enc;
    return e == NULL ? 0 : e->k + e->overhead;
}

int lt_generate(void *enc, uint32_t esi, uint8_t *out, size_t out_len) {
    lt_enc_t *e = enc;
    if (e == NULL || out == NULL || out_len != e->sym_len)
        return -EINVAL;
    if (esi >= lt_symbol_count(enc))
        return -EINVAL;
    if (esi < e->k) {
        memcpy(out, e->block + (size_t)esi * e->sym_len, e->sym_len);
        return 0;
    }
    uint16_t nb[LT_MAX_K];
    uint32_t deg = 0;
    repair_neighbors(e->k, esi, nb, &deg);
    memset(out, 0, e->sym_len);
    for (uint32_t i = 0; i < deg; i++)
        for (size_t b = 0; b < e->sym_len; b++)
            out[b] ^= e->block[(size_t)nb[i] * e->sym_len + b];
    return 0;
}

void lt_enc_destroy(void *enc) {
    (void)enc;
}
void lt_dec_destroy(void *dec) {
    (void)dec;
}

int lt_push(void *dec, uint32_t esi, const uint8_t *sym, size_t len) {
    lt_dec_t *d = dec;
    if (d == NULL || sym == NULL || len != d->sym_len)
        return -EINVAL;
    if (esi >= 2u * d->k)
        return -EINVAL;
    if (esi < d->k) {
        if (d->have_src[esi])
            return 1;
        d->have_src[esi] = 1;
        memcpy(d->src[esi], sym, len);
        return 0;
    }
    uint32_t r = esi - d->k;
    if (d->have_rep[r])
        return 1;
    d->have_rep[r] = 1;
    repair_neighbors(d->k, esi, d->rep_nb[r], &d->rep_deg[r]);
    memcpy(d->rep[r], sym, len);
    return 0;
}

/* Peel: resolve repairs whose unresolved-neighbor count hits one. */
static void peel(lt_dec_t *d) {
    int progress = 1;
    while (progress) {
        progress = 0;
        for (uint32_t r = 0; r < d->k && r < LT_REPAIR_MAX; r++) {
            if (!d->have_rep[r])
                continue;
            /* count unresolved neighbors */
            uint32_t unknown = 0;
            uint16_t last = 0;
            for (uint32_t i = 0; i < d->rep_deg[r]; i++) {
                if (!d->have_src[d->rep_nb[r][i]]) {
                    unknown++;
                    last = d->rep_nb[r][i];
                }
            }
            if (unknown == 1) {
                /* the repair minus knowns reveals the last unknown */
                uint8_t acc[LT_MAX_SYM];
                memcpy(acc, d->rep[r], d->sym_len);
                for (uint32_t i = 0; i < d->rep_deg[r]; i++) {
                    if (d->rep_nb[r][i] != last)
                        for (size_t b = 0; b < d->sym_len; b++)
                            acc[b] ^= d->src[d->rep_nb[r][i]][b];
                }
                memcpy(d->src[last], acc, d->sym_len);
                d->have_src[last] = 1;
                progress = 1;
            }
        }
    }
}

bool lt_ready(const void *dec) {
    const lt_dec_t *d = dec;
    if (d == NULL)
        return false;
    lt_dec_t *mut = (lt_dec_t *)d; /* peel mutates the recovery state */
    peel(mut);
    for (uint32_t i = 0; i < d->k; i++)
        if (!d->have_src[i])
            return false;
    return true;
}

uint32_t lt_rank(const void *dec) {
    const lt_dec_t *d = dec;
    if (d == NULL)
        return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->k; i++)
        n += d->have_src[i] ? 1u : 0u;
    for (uint32_t r = 0; r < d->k && r < LT_REPAIR_MAX; r++)
        n += d->have_rep[r] ? 1u : 0u;
    return n;
}

int lt_decode(void *dec, uint8_t *out, size_t block_len) {
    lt_dec_t *d = dec;
    if (d == NULL || out == NULL || block_len != (size_t)d->k * d->sym_len)
        return -EINVAL;
    if (!lt_ready(dec))
        return -EINVAL; /* the ARQ-fallback condition (design 02) */
    for (uint32_t j = 0; j < d->k; j++)
        memcpy(out + (size_t)j * d->sym_len, d->src[j], d->sym_len);
    return 0;
}
