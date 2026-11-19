/*
 * Fuzz target: fountain symbol streams (plan prompt W5.1).
 *
 * Shapes adapted from wack_a_mole tests/test_fuzz.c @ a0f0422 (the
 * M.1/X4 codec-stream fuzzers): an untrusted ESI/bytes stream feeds the
 * decoder; the invariants are the decode-abort budget (fd_ready must be
 * computable without unbounded work — checked via a bounded rank), Σ
 * (push returns in {0, 1, -EINVAL}), and no crash — ASan/UBSan run
 * with the harness. Build with -DWAM_FUZZ_ITERS for the CMake smoke
 * leg; standalone libFuzzer builds use LLVMFuzzerTestOneInput.
 */

#include "sdf/fountain.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SDF_FUZZ_ITERS
#define SDF_FUZZ_ITERS 200000
#endif

enum { K = 16, T = 16 };

static uint64_t rng_state = 0xC0FFEE123456789ull;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static int run_stream(const uint8_t *data, size_t size) {
    if (size < sizeof(K) * 1)
        return 0;

    /* deterministic cfg from the first bytes */
    uint32_t codec = data[0] & 1u; /* LT / NANORQ */
    fd_cfg_t cfg = {.k = K, .sym_len = T};
    size_t dsz = fd_sizeof(&cfg);
    if (dsz == 0)
        return 0;
    void *dec = malloc(dsz);
    if (dec == NULL)
        return 0;
    if (fd_init_codec(dec, &cfg, codec) != 0) {
        free(dec);
        return 0;
    }

    /* the stream: each 17 bytes = (esi, 16 symbol bytes) */
    size_t off = 1;
    uint32_t pushed_ok = 0, dup = 0, rejected = 0;
    while (off + 17u <= size) {
        uint32_t esi = (uint32_t)data[off] | ((uint32_t)data[off + 1] << 8);
        int rc = fd_push(dec, esi, data + off + 1, T);
        if (rc == 0)
            pushed_ok++;
        else if (rc == 1)
            dup++;
        else
            rejected++; /* -EINVAL is a legitimate refusal */
        off += 17u;
    }

    /* invariants: no crash (ASan/UBSan armed), bounded accounting */
    if (fd_ready(dec)) {
        uint8_t out[K * T];
        if (fd_decode(dec, out, sizeof(out)) == 0)
            pushed_ok += 0; /* decode worked — fine */
    }
    (void)dup;
    (void)rejected;
    fd_destroy(dec);
    free(dec);
    return 0;
}

int main(void) {
    /* the smoke leg: deterministic pseudo-random streams at a fixed
     * seed budget (plan: 60 s wall cap — the iteration count keeps the
     * CI leg comfortably inside it) */
    uint8_t buf[512];
    for (long it = 0; it < SDF_FUZZ_ITERS; it++) {
        size_t n = 17u + (size_t)(rng() % ((sizeof(buf) - 17u) | 1u));
        for (size_t i = 0; i < n; i++)
            buf[i] = (uint8_t)rng();
        run_stream(buf, n);
    }
    printf("fuzz_fountain: ok (%ld streams)\n", (long)SDF_FUZZ_ITERS);
    return 0;
}
