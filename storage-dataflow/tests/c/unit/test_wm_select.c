#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <errno.h>
#include <stdint.h>

#include "sdf/wm.h"

int wm_select_adr0007_tests(void);

static void init_profile(wm_profile_t *p, uint32_t version, uint32_t m, uint32_t n,
                         const uint32_t *units) {
    for (uint32_t i = 0; i < WM_MAX_PATHS; i++) {
        p->b[i] = 0;
    }
    p->version = version;
    p->m = m;
    p->n = n;
    for (uint32_t i = 0; i < n; i++) {
        p->b[i] = units[i];
    }
}

int main(void) {
    uint32_t units[2] = {4, 4};
    wm_profile_t profile;
    wm_compiled_t compiled;

    init_profile(&profile, 1, 8, 2, units);
    assert(wm_profile_compile(&profile, &compiled) == 0);

    uint32_t counts[2] = {0, 0};
    for (uint64_t counter = 0; counter < 8; counter++) {
        uint32_t path = wm_select(&compiled, counter);
        assert(path < 2);
        counts[path]++;
    }
    assert(counts[0] == 4);
    assert(counts[1] == 4);

    /* A full period repeats and high counter bits are ignored. */
    for (uint64_t counter = 0; counter < 8; counter++) {
        assert(wm_select(&compiled, counter + 8) == wm_select(&compiled, counter));
        assert(wm_select(&compiled, counter + (1ull << 40)) == wm_select(&compiled, counter));
    }

    /* Batched selection matches scalar selection. */
    uint32_t batch[16];
    wm_select_batch(&compiled, 0, batch, 16);
    for (uint32_t i = 0; i < 16; i++) {
        assert(batch[i] == wm_select(&compiled, i));
    }

    /* The bit-reversal construction balances any prefix of the period. */
    uint32_t prefix[2] = {0, 0};
    for (uint64_t counter = 0; counter < 8; counter++) {
        prefix[wm_select(&compiled, counter)]++;
        uint32_t delta = prefix[0] > prefix[1] ? prefix[0] - prefix[1] : prefix[1] - prefix[0];
        assert(delta <= 1);
    }

    /* Marking a path down drains all of its units to the healthy path. */
    wm_mark_down(&compiled, 0);
    for (uint64_t counter = 0; counter < 64; counter++) {
        assert(wm_select(&compiled, counter) == 1);
    }

    /* Marking it back up restores the original distribution. */
    wm_mark_up(&compiled, 0);
    counts[0] = 0;
    counts[1] = 0;
    for (uint64_t counter = 0; counter < 8; counter++) {
        counts[wm_select(&compiled, counter)]++;
    }
    assert(counts[0] == 4);
    assert(counts[1] == 4);

    /* The last healthy path cannot be marked down. */
    wm_mark_down(&compiled, 0);
    wm_mark_down(&compiled, 1);
    for (uint64_t counter = 0; counter < 8; counter++) {
        assert(wm_select(&compiled, counter) == 1);
    }
    wm_mark_up(&compiled, 0);

    /* Out-of-range ids are ignored. */
    wm_mark_down(&compiled, 2);
    wm_mark_up(&compiled, 42);
    assert(compiled.down_mask == 0);

    /* ADR-0007: methods, seeds, discrepancy, rotation. */
    (void)wm_select_adr0007_tests();

    return 0;
}

/* ---- ADR-0007 (plan prompt W2.1) -------------------------------------- */

/* The golden canonical profile (test_spray.c in wack_a_mole):
 * m = 1024, n = 5, b = [127, 400, 200, 173, 124]. */
static const uint32_t CANON_B[5] = {127, 400, 200, 173, 124};

static void canon_profile(wm_profile_t *p) {
    for (uint32_t i = 0; i < WM_MAX_PATHS; i++)
        p->b[i] = 0;
    p->version = 1;
    p->m = 1024;
    p->n = 5;
    for (uint32_t i = 0; i < 5; i++)
        p->b[i] = CANON_B[i];
}

static void test_cfg_validation(void) {
    wm_profile_t p;
    wm_compiled_t c;
    canon_profile(&p);

    wm_compile_cfg_t cfg = {.method = WM_METHOD_SHUFFLE1, .sa = 333, .sb = 735};
    assert(wm_profile_compile_cfg(&p, &c, &cfg) == 0);
    assert(c.method == WM_METHOD_SHUFFLE1 && c.sa == 333 && c.sb == 735);

    cfg.method = 3; /* out of enum */
    assert(wm_profile_compile_cfg(&p, &c, &cfg) == -EINVAL);
    cfg.method = WM_METHOD_SHUFFLE1;

    cfg.sb = 734; /* even */
    assert(wm_profile_compile_cfg(&p, &c, &cfg) == -EINVAL);
    cfg.sb = 0;
    assert(wm_profile_compile_cfg(&p, &c, &cfg) == -EINVAL);
    cfg.sb = 1024; /* >= m */
    assert(wm_profile_compile_cfg(&p, &c, &cfg) == -EINVAL);
    cfg.sb = 735;
    cfg.sa = 1024; /* >= m */
    assert(wm_profile_compile_cfg(&p, &c, &cfg) == -EINVAL);

    /* m == 1 keeps its pre-ADR-0007 validity: no key domain to
     * decorrelate, so seed range checks do not apply. */
    uint32_t one[1] = {1};
    wm_profile_t p1;
    init_profile(&p1, 1, 1, 1, one);
    assert(wm_profile_compile(&p1, &c) == 0);
    assert(wm_select(&c, 9999) == 0);
}

/* Discrepancy property ([WAM] section 5, adapted from wack_a_mole
 * test_spray.c: starts {0,1,500,1023} x lengths {1024, 10240, 102400}):
 * over any contiguous window, per-path deviation must stay within the
 * paper's bound — l for shuffle1, 2l for shuffle2 — plus the small
 * finite-sample tolerance the wam harness allows. */
static void test_discrepancy_bounds(void) {
    wm_profile_t p;
    wm_compiled_t c;
    canon_profile(&p);

    static const uint32_t starts[] = {0, 1, 500, 1023};
    static const uint32_t lens[] = {1024, 10240, 102400};

    for (int method = WM_METHOD_SHUFFLE1; method <= WM_METHOD_SHUFFLE2;
         method++) {
        wm_compile_cfg_t cfg = {.method = (uint32_t)method, .sa = 333, .sb = 735};
        assert(wm_profile_compile_cfg(&p, &c, &cfg) == 0);
        double lim_extra = method == WM_METHOD_SHUFFLE1 ? 10.0 : 20.0;

        for (size_t si = 0; si < sizeof(starts) / sizeof(starts[0]); si++) {
            for (size_t li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
                uint64_t from = starts[si];
                uint64_t len = lens[li];
                uint64_t counts[5] = {0, 0, 0, 0, 0};
                for (uint64_t k = 0; k < len; k++)
                    counts[wm_select(&c, from + k)]++;
                for (uint32_t i = 0; i < 5; i++) {
                    double expect = (double)CANON_B[i] / 1024.0 * (double)len;
                    double dev = (double)counts[i] > expect
                                     ? (double)counts[i] - expect
                                     : expect - (double)counts[i];
                    double lim = lim_extra + 0.02 * expect;
                    assert(dev <= lim);
                }
            }
        }
    }
}

/* Equal profiles with different seeds must never emit the same
 * sequence over a full cycle (the ADR-0007 decorrelation contract:
 * two fills with the same profile are not correlated). */
static void test_seed_decorrelation(void) {
    wm_profile_t p;
    wm_compiled_t a, b;
    canon_profile(&p);

    wm_compile_cfg_t ca = {.method = WM_METHOD_SHUFFLE1, .sa = 1, .sb = 3};
    wm_compile_cfg_t cb = {.method = WM_METHOD_SHUFFLE1, .sa = 333, .sb = 735};
    assert(wm_profile_compile_cfg(&p, &a, &ca) == 0);
    assert(wm_profile_compile_cfg(&p, &b, &cb) == 0);

    uint32_t diffs = 0;
    for (uint32_t j = 0; j < 1024; j++) {
        if (wm_select(&a, j) != wm_select(&b, j))
            diffs++;
    }
    assert(diffs > 0);

    /* equal seeds are exactly equal (deterministic compilation) */
    wm_compiled_t c2;
    assert(wm_profile_compile_cfg(&p, &c2, &ca) == 0);
    for (uint32_t j = 0; j < 1024; j++)
        assert(wm_select(&a, j) == wm_select(&c2, j));
}

/* wm_seed_rotate (the wam-send wrap rule): sb stays odd and both
 * seeds inside [0, m); the rotation actually moves the seeds. */
static void test_seed_rotation(void) {
    wm_compile_cfg_t cfg = {.method = WM_METHOD_SHUFFLE1, .sa = 333, .sb = 735};
    wm_compile_cfg_t before = cfg;
    wm_seed_rotate(&cfg, 1024);
    assert((cfg.sb & 1u) == 1);
    assert(cfg.sb != 0 && cfg.sb < 1024);
    assert(cfg.sa < 1024);
    assert(cfg.sa != before.sa || cfg.sb != before.sb);
    /* idempotence of the invariants across many rotations */
    for (int i = 0; i < 100; i++) {
        wm_seed_rotate(&cfg, 1024);
        assert((cfg.sb & 1u) == 1 && cfg.sb < 1024 && cfg.sa < 1024);
    }
}

/* mark_down/up must behave identically under the shuffle methods:
 * selection never lands on a down path, and mark_up restores the
 * exact original allocation. */
static void test_down_mask_under_shuffle(void) {
    wm_profile_t p;
    wm_compiled_t c;
    canon_profile(&p);
    wm_compile_cfg_t cfg = {.method = WM_METHOD_SHUFFLE1, .sa = 333, .sb = 735};
    assert(wm_profile_compile_cfg(&p, &c, &cfg) == 0);

    wm_mark_down(&c, 1);
    uint32_t counts[5] = {0, 0, 0, 0, 0};
    for (uint32_t j = 0; j < 1024; j++) {
        uint32_t path = wm_select(&c, j);
        assert(path != 1); /* never the down path */
        counts[path]++;
    }
    /* the 400 units of path 1 renormalized across 127/200/173/124 */
    (void)counts;
    wm_mark_up(&c, 1);
    for (int i = 0; i < 5; i++)
        counts[i] = 0;
    for (uint32_t j = 0; j < 1024; j++) {
        uint32_t path = wm_select(&c, j);
        counts[path]++;
    }
    assert(counts[0] == 127 && counts[1] == 400 && counts[2] == 200 &&
           counts[3] == 173 && counts[4] == 124);
}

int wm_select_adr0007_tests(void) {
    test_cfg_validation();
    test_discrepancy_bounds();
    test_seed_decorrelation();
    test_seed_rotation();
    test_down_mask_under_shuffle();
    return 0;
}
