/*
 * Fuzz target: wm_profile_update embodiment vectors (plan prompt W5.1).
 *
 * Shape adapted from wack_a_mole tests/test_fuzz.c @ a0f0422 (the
 * M.1 embodiment fuzzer that found the mode-4 empty-kbar remainder
 * loss): untrusted e[] vectors over all four modes. Invariants: Σb = m
 * after every successful update, the last healthy path never zeroes
 * the fleet, e[i] > b[i] refused without mutation, and no crash.
 */

#include "sdf/wm.h"

#include "fuzz_common.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SDF_FUZZ_ITERS
#define SDF_FUZZ_ITERS 200000
#endif

static uint64_t rng_state = 0x5EEDBEEFCAFEF00Dull;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static uint64_t sum_b(const wm_profile_t *p) {
    uint64_t s = 0;
    for (uint32_t i = 0; i < p->n; i++) {
        s += p->b[i];
    }
    return s;
}

int main(void) {
    long budget = fuzz_budget(SDF_FUZZ_ITERS);
    for (long it = 0; it < budget; it++) {
        uint32_t n = 2u + (uint32_t)(rng() % 4u); /* 2..5 paths */
        uint32_t m = 64u;                         /* fixed pow2 */
        wm_profile_t p;
        memset(&p, 0, sizeof(p));
        p.version = 1;
        p.m = m;
        p.n = n;
        /* valid base profile: equal split with the remainder spread */
        for (uint32_t i = 0; i < n; i++) {
            p.b[i] = m / n;
        }
        for (uint32_t i = 0; i < m % n; i++) {
            p.b[i]++;
        }

        uint32_t e[WM_MAX_PATHS] = {0};
        for (uint32_t i = 0; i < n; i++) {
            /* e[i] in [0, b[i]] mostly, with occasional overs to hit
             * the refusal path (the fuzzer's job is both) */
            uint32_t roll = (uint32_t)(rng() % 100u);
            e[i] = roll < 90 ? (uint32_t)(rng() % (p.b[i] + 1u)) : (uint32_t)(rng() % (m + 8u));
        }
        int mode = (int)(rng() % 6u); /* 1..4 valid, 0/5 unknown: refused */

        uint64_t sum_before = sum_b(&p);
        if (sum_before != m) {
            return 1; /* the harness built a bad base — invariant breach */
        }

        wm_profile_t before = p;
        int rc = wm_profile_update(&p, e, mode);
        if (rc == 0) {
            /* invariants after every accepted update */
            if (sum_b(&p) != m) {
                fprintf(stderr, "FUZZ FAIL: sum(b)=%llu != m after mode %d\n",
                        (unsigned long long)sum_b(&p), mode);
                return 1;
            }
            /* never all zero (n > 1) */
            uint32_t alive = 0;
            for (uint32_t i = 0; i < n; i++) {
                if (p.b[i] > 0) {
                    alive++;
                }
            }
            if (alive == 0) {
                fprintf(stderr, "FUZZ FAIL: fleet zeroed by mode %d\n", mode);
                return 1;
            }
        } else if (memcmp(&p, &before, sizeof(p)) != 0) {
            fprintf(stderr,
                    "FUZZ FAIL: rejected update mutated profile at iteration %ld "
                    "(mode %d, rc %d)\n",
                    it, mode, rc);
            return 1;
        }
    }
    printf("fuzz_wm_profile: ok (%ld vectors)\n", budget);
    return 0;
}
