/*
 * nanorq fountain arm tests (plan prompt W3.2).
 *
 * The RaptorQ arm over the public ABI: full roundtrip; recovery
 * under 20% injected symbol loss with 30% overhead; duplicates
 * counted (return 1); the NOT-ready decode that triggers the
 * coordinator's ARQ fallback (design 02's safety); lifecycle
 * destroy; validation of size/init contracts.
 */

#include "sdf/fountain.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define K 64u
#define T 32u

static uint8_t g_block[K * T];

static uint8_t byte_at(size_t off) {
    return (uint8_t)((off * 2654435761ull) >> 24);
}

static void enc_setup(void *emem, uint32_t overhead) {
    for (size_t i = 0; i < sizeof(g_block); i++) {
        g_block[i] = byte_at(i);
    }
    fe_cfg_t cfg = {
        .block_len = sizeof(g_block),
        .k = K,
        .overhead_pct = overhead,
        .codec = SDF_FOUNTAIN_NANORQ,
    };
    CHECK(fe_sizeof(&cfg) > 0);
    CHECK(fe_init_in(emem, &cfg, g_block) == 0);
    CHECK(fe_symbol_count(emem) >= K);
}

static void dec_setup(void *dmem) {
    fd_cfg_t cfg = {.k = K, .sym_len = T};
    CHECK(fd_sizeof(&cfg) > 0);
    CHECK(fd_init_codec(dmem, &cfg, SDF_FOUNTAIN_NANORQ) == 0);
}

static void test_validation(void) {
    fe_cfg_t bad = {
        .block_len = K * T,
        .k = K,
        .overhead_pct = 10,
        .codec = SDF_FOUNTAIN_NANORQ,
    };
    bad.block_len += 1; /* not divisible by k */
    CHECK(fe_sizeof(&bad) == 0);

    fe_cfg_t ok = {
        .block_len = K * T,
        .k = K,
        .overhead_pct = 10,
        .codec = SDF_FOUNTAIN_NANORQ,
    };
    size_t sz = fe_sizeof(&ok);
    CHECK(sz > 0);
    void *mem = malloc(sz);
    CHECK(mem != NULL);
    CHECK(fe_init_in(mem, &ok, g_block) == 0);
    CHECK(fe_generate(mem, 999999, g_block, T) == -EINVAL);
    fe_destroy(mem);
    fe_destroy(NULL);
    fd_destroy(NULL);
    free(mem);
}

static void test_roundtrip(void) {
    size_t esz = fe_sizeof(
        &(fe_cfg_t){.block_len = K * T, .k = K, .overhead_pct = 0, .codec = SDF_FOUNTAIN_NANORQ});
    size_t dsz = fd_sizeof(&(fd_cfg_t){.k = K, .sym_len = T});
    void *emem = malloc(esz);
    void *dmem = malloc(dsz);
    CHECK(emem != NULL && dmem != NULL);
    enc_setup(emem, 0);
    dec_setup(dmem);

    /* all K systematic symbols: every byte generated on the encoder
     * side is fed to the decoder and the block comes back exact */
    for (uint32_t esi = 0; esi < K; esi++) {
        uint8_t sym[T];
        CHECK(fe_generate(emem, esi, sym, T) == 0);
        CHECK(fd_push(dmem, esi, sym, T) == 0);
    }
    CHECK(fd_ready(dmem));
    uint8_t out[K * T];
    CHECK(fd_decode(dmem, out, sizeof(out)) == 0);
    CHECK(memcmp(out, g_block, sizeof(g_block)) == 0);
    CHECK(fd_rank(dmem) == K);

    /* duplicate pushes: counted, not errors */
    uint8_t sym[T];
    CHECK(fe_generate(emem, 0, sym, T) == 0);
    CHECK(fd_push(dmem, 0, sym, T) == 1);

    fe_destroy(emem);
    fd_destroy(dmem);
    free(emem);
    free(dmem);
}

static void test_lossy_recovery(void) {
    /* 20% source loss, 30% overhead: RaptorQ recovers from any
     * K + eps distinct symbols (the design-02 repair premise) */
    size_t esz = fe_sizeof(
        &(fe_cfg_t){.block_len = K * T, .k = K, .overhead_pct = 30, .codec = SDF_FOUNTAIN_NANORQ});
    size_t dsz = fd_sizeof(&(fd_cfg_t){.k = K, .sym_len = T});
    void *emem = malloc(esz);
    void *dmem = malloc(dsz);
    CHECK(emem != NULL && dmem != NULL);
    enc_setup(emem, 30);
    dec_setup(dmem);

    uint32_t count = fe_symbol_count(emem);
    CHECK(count >= K + K * 30 / 100);
    uint32_t dropped = 0;
    for (uint32_t esi = 0; esi < count; esi++) {
        if (esi < K && (esi % 5u) == 1u) {
            dropped++; /* drop 20% of the sources */
            continue;
        }
        uint8_t sym[T];
        CHECK(fe_generate(emem, esi, sym, T) == 0);
        CHECK(fd_push(dmem, esi, sym, T) == 0);
    }
    CHECK(dropped == 13); /* K/5 rounded: the deterministic drop set */

    CHECK(fd_ready(dmem)); /* repair_block succeeds via repair symbols */
    uint8_t out[K * T];
    CHECK(fd_decode(dmem, out, sizeof(out)) == 0);
    CHECK(memcmp(out, g_block, sizeof(g_block)) == 0);

    fe_destroy(emem);
    fd_destroy(dmem);
    free(emem);
    free(dmem);
}

static void test_repair_window_bounds(void) {
    /* The public window contains K sources plus K repairs. At the maximum
     * decoder K, the first repair must not alias adjacent decoder state. */
    fd_cfg_t cfg = {.k = 512, .sym_len = 1};
    size_t dsz = fd_sizeof(&cfg);
    void *dmem = malloc(dsz);
    CHECK(dmem != NULL);
    CHECK(fd_init_codec(dmem, &cfg, SDF_FOUNTAIN_NANORQ) == 0);

    uint8_t symbol = 0x5a;
    CHECK(fd_push(dmem, 512, &symbol, sizeof(symbol)) == 0);
    CHECK(fd_rank(dmem) == 1);
    CHECK(!fd_ready(dmem));
    uint8_t out[512];
    memset(out, 0xa5, sizeof(out));
    CHECK(fd_decode(dmem, out, sizeof(out)) == -EINVAL);
    for (size_t i = 0; i < sizeof(out); i++) {
        CHECK(out[i] == 0xa5);
    }

    fd_destroy(dmem);
    free(dmem);
}

static void test_insufficient_is_the_arq_condition(void) {
    /* 40% of sources dropped, no repairs: NOT ready — the coordinator
     * must fall back to ARQ rather than ship an incomplete block */
    size_t esz = fe_sizeof(
        &(fe_cfg_t){.block_len = K * T, .k = K, .overhead_pct = 0, .codec = SDF_FOUNTAIN_NANORQ});
    size_t dsz = fd_sizeof(&(fd_cfg_t){.k = K, .sym_len = T});
    void *emem = malloc(esz);
    void *dmem = malloc(dsz);
    CHECK(emem != NULL && dmem != NULL);
    enc_setup(emem, 0);
    dec_setup(dmem);

    for (uint32_t esi = 0; esi < K; esi++) {
        if (esi % 5u == 1u || esi % 5u == 2u) {
            continue; /* 40% dropped */
        }
        uint8_t sym[T];
        CHECK(fe_generate(emem, esi, sym, T) == 0);
        CHECK(fd_push(dmem, esi, sym, T) == 0);
    }
    CHECK(!fd_ready(dmem));
    uint8_t out[K * T];
    CHECK(fd_decode(dmem, out, sizeof(out)) == -EINVAL);

    fe_destroy(emem);
    fd_destroy(dmem);
    free(emem);
    free(dmem);
}

/* ---- LT arm (plan prompt W3.3) ---------------------------------------- */

static void lt_run(int drop_source5, uint32_t overhead) {
    size_t esz = fe_sizeof(&(fe_cfg_t){
        .block_len = K * T, .k = K, .overhead_pct = overhead, .codec = SDF_FOUNTAIN_LT});
    size_t dsz = fd_sizeof(&(fd_cfg_t){.k = K, .sym_len = T});
    void *emem = malloc(esz);
    void *dmem = malloc(dsz);
    CHECK(emem != NULL && dmem != NULL);

    for (size_t i = 0; i < sizeof(g_block); i++) {
        g_block[i] = byte_at(i);
    }
    fe_cfg_t ecfg = {
        .block_len = sizeof(g_block), .k = K, .overhead_pct = overhead, .codec = SDF_FOUNTAIN_LT};
    CHECK(fe_init_in(emem, &ecfg, g_block) == 0);
    CHECK(fe_symbol_count(emem) >= K);

    fd_cfg_t dcfg = {.k = K, .sym_len = T};
    CHECK(fd_init_codec(dmem, &dcfg, SDF_FOUNTAIN_LT) == 0);

    uint32_t count = fe_symbol_count(emem);
    for (uint32_t esi = 0; esi < count; esi++) {
        if (drop_source5 && esi == 5) {
            continue; /* the single dropped source */
        }
        uint8_t sym[T];
        CHECK(fe_generate(emem, esi, sym, T) == 0);
        CHECK(fd_push(dmem, esi, sym, T) == 0);
    }

    if (drop_source5) {
        /* the deterministic neighbor sets make this peel resolvable
         * with the full repair set: pinned by goldens, stable */
        CHECK(fd_ready(dmem));
    } else {
        CHECK(fd_ready(dmem));
    }
    uint8_t out[K * T];
    CHECK(fd_decode(dmem, out, sizeof(out)) == 0);
    CHECK(memcmp(out, g_block, sizeof(g_block)) == 0);

    /* duplicates counted */
    uint8_t sym[T];
    CHECK(fe_generate(emem, 0, sym, T) == 0);
    CHECK(fd_push(dmem, 0, sym, T) == 1);

    fe_destroy(emem);
    fd_destroy(dmem);
    free(emem);
    free(dmem);
}

static void test_lt(void) {
    lt_run(0, 0);   /* all sources: roundtrip */
    lt_run(0, 100); /* sources + repairs */
    lt_run(1, 100); /* one dropped source, recovered via the repairs */
}

int main(void) {
    test_validation();
    test_roundtrip();
    test_lossy_recovery();
    test_repair_window_bounds();
    test_insufficient_is_the_arq_condition();
    test_lt();

    test_roundtrip();
    test_lossy_recovery();
    test_repair_window_bounds();
    test_insufficient_is_the_arq_condition();
    printf("test_fe_codec: ok (nanorq: roundtrip, 20%% loss, ARQ condition; "
           "LT: roundtrip, repairs, peel recovery)\n");
    return 0;
}
