/*
 * Fountain PRNG (plan prompt W3.3): xorshift64* — deterministic and
 * reproducible per ESI so the decoder derives the same neighbor sets
 * the encoder used, without any side channel (the fountain.h ESI
 * contract). Same generator family as the loopback harness's
 * deterministic knobs.
 */

#include "fountain_internal.h"

uint64_t lt_prng_next(uint64_t *state) {
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 2685821657736338717ull;
}
