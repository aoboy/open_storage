/*
 * Fuzz target: fountain symbol streams (plan prompt W5.1/W6.11).
 *
 * Shapes adapted from wack_a_mole tests/test_fuzz.c @ a0f0422 (the
 * M.1/X4 codec-stream fuzzers). Each input derives a supported codec,
 * K and symbol length, then supplies untrusted ESIs, payloads and
 * malformed lengths. Invariants: pushes return only 0/1/-EINVAL; rank
 * is monotonic, bounded by 2K and sufficient for readiness; rejected
 * operations do not mutate rank; readiness and decode agree; failed
 * decodes preserve the output buffer; duplicate accepts preserve the
 * first payload; a complete source set decodes to those payloads.
 */

#include "sdf/fountain.h"

#include "fuzz_common.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SDF_FUZZ_ITERS
#define SDF_FUZZ_ITERS 200000
#endif

#define MAX_BLOCK_BYTES (32u * 1024u)
#define MAX_STREAM_BYTES (16u * 1024u)
#define CANARY 0xa5

static uint64_t rng_state = 0xC0FFEE123456789ull;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int failf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FUZZ FAIL: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return 1;
}

static int run_stream(const uint8_t *data, size_t size, long iteration) {
    if (size < 5) {
        return 0;
    }

    uint32_t codec = data[0] & 1u; /* LT / NANORQ */
    uint32_t k_max = codec == SDF_FOUNTAIN_NANORQ ? 512u : 64u;
    uint32_t t_max = codec == SDF_FOUNTAIN_NANORQ ? 8192u : 256u;
    uint32_t k = 1u + (uint32_t)read_u16(data + 1) % k_max;
    uint32_t t_limit = t_max;
    if (t_limit > MAX_BLOCK_BYTES / k) {
        t_limit = MAX_BLOCK_BYTES / k;
    }
    uint32_t t = 1u + (uint32_t)read_u16(data + 3) % t_limit;
    size_t block_len = (size_t)k * t;

    fd_cfg_t cfg = {.k = k, .sym_len = t};
    size_t dsz = fd_sizeof(&cfg);
    if (dsz == 0 || dsz > 16u * 1024u * 1024u) {
        return 0;
    }
    void *dec = malloc(dsz);
    uint8_t *out = malloc(block_len + 1u);
    uint8_t *source_block = malloc(block_len);
    uint8_t *source_seen = calloc(k, 1u);
    uint8_t *wrong_len = malloc((size_t)t + 1u);
    if (dec == NULL || out == NULL || source_block == NULL || source_seen == NULL ||
        wrong_len == NULL) {
        free(dec);
        free(out);
        free(source_block);
        free(source_seen);
        free(wrong_len);
        return 0;
    }
    if (fd_init_codec(dec, &cfg, (int)codec) != 0) {
        fd_destroy(dec);
        free(dec);
        free(out);
        free(source_block);
        free(source_seen);
        free(wrong_len);
        return 0;
    }

    memset(wrong_len, 0x3c, (size_t)t + 1u);
    uint32_t rank = fd_rank(dec);
    uint32_t source_count = 0;
    uint32_t records = 0;
    size_t off = 5;
    size_t record_len = 4u + t;
    while (off <= size && size - off >= record_len) {
        uint32_t esi = read_u32(data + off);
        if ((esi & 3u) == 0) {
            /* one quarter of records target the decoder's valid window;
             * the rest remain untrusted raw ESIs */
            esi = (esi >> 2) % (2u * k);
        }
        const uint8_t *payload = data + off + 4;
        uint32_t before = fd_rank(dec);
        int rc = fd_push(dec, esi, payload, t);
        if (rc != 0 && rc != 1 && rc != -EINVAL) {
            (void)failf("iteration %ld: push returned %d", iteration, rc);
            goto fail;
        }
        rank = fd_rank(dec);
        if (rank < before || rank > 2u * k) {
            (void)failf("iteration %ld: rank %u -> %u outside [%u, %u]", iteration, before, rank,
                        0u, 2u * k);
            goto fail;
        }
        if (rc == 0) {
            if (rank <= before) {
                (void)failf("iteration %ld: accepted ESI %u did not increase rank", iteration, esi);
                goto fail;
            }
            if (esi < k && !source_seen[esi]) {
                source_seen[esi] = 1;
                memcpy(source_block + (size_t)esi * t, payload, t);
                source_count++;
            }
        } else if (rank != before) {
            (void)failf("iteration %ld: refused ESI %u changed rank", iteration, esi);
            goto fail;
        }
        records++;
        off += record_len;
    }

    /* Malformed lengths and out-of-window ESIs are harmless refusals. */
    {
        uint32_t before = fd_rank(dec);
        if (fd_push(dec, 0, wrong_len, (size_t)t + 1u) != -EINVAL || fd_rank(dec) != before) {
            (void)failf("iteration %ld: wrong-length push mutated state", iteration);
            goto fail;
        }
        if (fd_push(dec, 2u * k, wrong_len, t) != -EINVAL || fd_rank(dec) != before) {
            (void)failf("iteration %ld: out-of-window ESI mutated state", iteration);
            goto fail;
        }
    }

    bool ready = fd_ready(dec);
    rank = fd_rank(dec);
    if (rank > 2u * k) {
        (void)failf("iteration %ld: rank %u exceeds 2K", iteration, rank);
        goto fail;
    }
    if (ready && rank < k) {
        (void)failf("iteration %ld: ready at rank %u < K=%u", iteration, rank, k);
        goto fail;
    }

    memset(out, CANARY, block_len + 1u);
    if (fd_decode(dec, out, block_len - 1u) != -EINVAL) {
        (void)failf("iteration %ld: wrong-length decode was accepted", iteration);
        goto fail;
    }
    for (size_t i = 0; i < block_len + 1u; i++) {
        if (out[i] != CANARY) {
            (void)failf("iteration %ld: failed decode wrote output", iteration);
            goto fail;
        }
    }

    if (!ready) {
        memset(out, CANARY, block_len + 1u);
        if (fd_decode(dec, out, block_len) != -EINVAL) {
            (void)failf("iteration %ld: not-ready decode succeeded", iteration);
            goto fail;
        }
        for (size_t i = 0; i < block_len; i++) {
            if (out[i] != CANARY) {
                (void)failf("iteration %ld: not-ready decode wrote output", iteration);
                goto fail;
            }
        }
    } else {
        uint8_t *again = malloc(block_len);
        if (again == NULL) {
            goto fail;
        }
        if (fd_decode(dec, out, block_len) != 0 || fd_decode(dec, again, block_len) != 0 ||
            memcmp(out, again, block_len) != 0) {
            free(again);
            (void)failf("iteration %ld: ready decode failed or was nondeterministic", iteration);
            goto fail;
        }
        free(again);
        if (source_count == k && memcmp(out, source_block, block_len) != 0) {
            (void)failf("iteration %ld: source-only decode differs from source payloads",
                        iteration);
            goto fail;
        }
    }

    fd_destroy(dec);
    free(dec);
    free(out);
    free(source_block);
    free(source_seen);
    free(wrong_len);
    return 0;

fail:
    fd_destroy(dec);
    free(dec);
    free(out);
    free(source_block);
    free(source_seen);
    free(wrong_len);
    return 1;
}

/* Directed source-only roundtrips at public boundary geometries. */
static int run_geometry(uint32_t codec, uint32_t k, uint32_t t, long marker) {
    size_t block_len = (size_t)k * t;
    uint8_t *block = malloc(block_len);
    uint8_t *out = malloc(block_len);
    if (block == NULL || out == NULL) {
        free(block);
        free(out);
        return 0;
    }
    for (size_t i = 0; i < block_len; i++) {
        block[i] = (uint8_t)((i * 131u + (size_t)marker) & 0xffu);
    }

    fd_cfg_t cfg = {.k = k, .sym_len = t};
    size_t dsz = fd_sizeof(&cfg);
    if (dsz == 0) {
        free(block);
        free(out);
        return failf("geometry case %ld: fd_sizeof failed", marker);
    }
    void *dec = malloc(dsz);
    if (dec == NULL) {
        free(block);
        free(out);
        return 0;
    }
    if (fd_init_codec(dec, &cfg, (int)codec) != 0) {
        free(dec);
        free(block);
        free(out);
        return failf("geometry case %ld: init failed (codec %u K %u T %u)", marker, codec, k, t);
    }
    for (uint32_t esi = 0; esi < k; esi++) {
        if (fd_push(dec, esi, block + (size_t)esi * t, t) != 0) {
            fd_destroy(dec);
            free(dec);
            free(block);
            free(out);
            return failf("geometry case %ld: source ESI %u refused", marker, esi);
        }
    }
    if (!fd_ready(dec) || fd_rank(dec) < k || fd_decode(dec, out, block_len) != 0 ||
        memcmp(out, block, block_len) != 0) {
        fd_destroy(dec);
        free(dec);
        free(block);
        free(out);
        return failf("geometry case %ld: source roundtrip failed", marker);
    }
    fd_destroy(dec);
    free(dec);
    free(block);
    free(out);
    return 0;
}

int main(void) {
    long budget = fuzz_budget(SDF_FUZZ_ITERS);

    if (run_geometry(SDF_FOUNTAIN_LT, 64, 256, 1) != 0 ||
        run_geometry(SDF_FOUNTAIN_NANORQ, 1, 8192, 2) != 0 ||
        run_geometry(SDF_FOUNTAIN_NANORQ, 257, 1, 3) != 0 ||
        run_geometry(SDF_FOUNTAIN_NANORQ, 512, 1, 4) != 0) {
        return 1;
    }

    uint8_t buf[MAX_STREAM_BYTES];
    for (long it = 0; it < budget; it++) {
        size_t n = 5u + (size_t)(rng() % (sizeof(buf) - 4u));
        for (size_t i = 0; i < n; i++) {
            buf[i] = (uint8_t)rng();
        }
        if (run_stream(buf, n, it) != 0) {
            return 1;
        }
    }
    printf("fuzz_fountain: ok (%ld streams, boundary geometries both codecs)\n", budget);
    return 0;
}
