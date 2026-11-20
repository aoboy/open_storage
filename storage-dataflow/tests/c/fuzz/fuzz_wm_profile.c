/*
 * Fuzz target: wm_profile_update embodiment vectors (plan prompt W5.1/W6.11).
 *
 * Shape adapted from wack_a_mole tests/test_fuzz.c @ a0f0422 (the
 * M.1 embodiment fuzzer that found the mode-4 empty-kbar remainder
 * loss): untrusted e[] vectors over all four modes. Invariants: Σb = m
 * after every successful update, the last healthy path never zeroes
 * the fleet, e[i] > b[i] refused without mutation, and no crash.
 *
 * With file arguments the target replays checked-in directed vectors
 * with exact expected return codes and post-update profiles.
 */

#include "sdf/wm.h"

#include "fuzz_common.h"

#include <errno.h>
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

static int read_u32(FILE *f, uint32_t *out) {
    uint8_t b[4];
    if (fread(b, 1, sizeof(b), f) != sizeof(b)) {
        return -1;
    }
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}

static int read_i32(FILE *f, int32_t *out) {
    uint32_t raw = 0;
    if (read_u32(f, &raw) != 0) {
        return -1;
    }
    *out = (int32_t)raw;
    return 0;
}

static int replay_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "WM REPLAY FAIL %s: cannot open\n", path);
        return 1;
    }
    char magic[8];
    uint32_t n = 0;
    uint32_t m = 0;
    if (fread(magic, 1, sizeof(magic), f) != sizeof(magic) || memcmp(magic, "WMPROF1", 7) != 0 ||
        read_u32(f, &n) != 0 || read_u32(f, &m) != 0) {
        fclose(f);
        fprintf(stderr, "WM REPLAY FAIL %s: bad header\n", path);
        return 1;
    }
    if (n < 2 || n > WM_MAX_PATHS || m == 0) {
        fclose(f);
        fprintf(stderr, "WM REPLAY FAIL %s: n/m out of range\n", path);
        return 1;
    }

    wm_profile_t p;
    memset(&p, 0, sizeof(p));
    p.version = 1;
    p.m = m;
    p.n = n;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (read_u32(f, &p.b[i]) != 0) {
            fclose(f);
            fprintf(stderr, "WM REPLAY FAIL %s: truncated b[]\n", path);
            return 1;
        }
        sum += p.b[i];
    }
    if (sum != m) {
        fclose(f);
        fprintf(stderr, "WM REPLAY FAIL %s: sum(b) != m\n", path);
        return 1;
    }

    uint32_t e[WM_MAX_PATHS] = {0};
    for (uint32_t i = 0; i < n; i++) {
        if (read_u32(f, &e[i]) != 0) {
            fclose(f);
            fprintf(stderr, "WM REPLAY FAIL %s: truncated e[]\n", path);
            return 1;
        }
    }
    int32_t mode = 0;
    int32_t expected_rc = 0;
    if (read_i32(f, &mode) != 0 || read_i32(f, &expected_rc) != 0) {
        fclose(f);
        fprintf(stderr, "WM REPLAY FAIL %s: truncated mode/expected_rc\n", path);
        return 1;
    }
    uint32_t expected_b[WM_MAX_PATHS] = {0};
    for (uint32_t i = 0; i < n; i++) {
        if (read_u32(f, &expected_b[i]) != 0) {
            fclose(f);
            fprintf(stderr, "WM REPLAY FAIL %s: truncated expected_b[]\n", path);
            return 1;
        }
    }
    uint8_t trailing = 0;
    if (fread(&trailing, 1, 1, f) != 0 || ferror(f)) {
        fclose(f);
        fprintf(stderr, "WM REPLAY FAIL %s: trailing bytes\n", path);
        return 1;
    }
    fclose(f);

    wm_profile_t before = p;
    int rc = wm_profile_update(&p, e, (int)mode);
    if (rc != expected_rc) {
        fprintf(stderr, "WM REPLAY FAIL %s: rc=%d expected %d\n", path, rc, expected_rc);
        return 1;
    }
    if (rc == 0) {
        if (sum_b(&p) != m) {
            fprintf(stderr, "WM REPLAY FAIL %s: sum(b) != m after update\n", path);
            return 1;
        }
        uint32_t alive = 0;
        for (uint32_t i = 0; i < n; i++) {
            alive += p.b[i] > 0 ? 1u : 0u;
            if (p.b[i] != expected_b[i]) {
                fprintf(stderr, "WM REPLAY FAIL %s: b[%u]=%u expected %u\n", path, i, p.b[i],
                        expected_b[i]);
                return 1;
            }
        }
        if (alive == 0) {
            fprintf(stderr, "WM REPLAY FAIL %s: fleet zeroed\n", path);
            return 1;
        }
    } else if (memcmp(&p, &before, sizeof(p)) != 0) {
        fprintf(stderr, "WM REPLAY FAIL %s: refused update mutated profile\n", path);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (replay_file(argv[i]) != 0) {
                return 1;
            }
        }
        printf("fuzz_wm_profile: ok (%d corpus vectors)\n", argc - 1);
        return 0;
    }

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
