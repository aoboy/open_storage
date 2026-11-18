#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stdint.h>

#include "sdf/wm.h"

static void init_profile(wm_profile_t *p, uint32_t m, uint32_t n, const uint32_t *units) {
    for (uint32_t i = 0; i < WM_MAX_PATHS; i++) {
        p->b[i] = 0;
    }
    p->version = 1;
    p->m = m;
    p->n = n;
    for (uint32_t i = 0; i < n; i++) {
        p->b[i] = units[i];
    }
}

static void count_allocs(const wm_compiled_t *c, uint32_t *counts) {
    for (uint32_t i = 0; i < c->n; i++) {
        counts[i] = 0;
    }
    for (uint32_t counter = 0; counter < c->m; counter++) {
        counts[wm_select(c, counter)]++;
    }
}

int main(void) {
    uint32_t units[3] = {512, 256, 256};
    wm_profile_t profile;
    wm_compiled_t compiled;

    init_profile(&profile, 1024, 3, units);
    assert(wm_profile_compile(&profile, &compiled) == 0);
    assert(compiled.m == 1024);
    assert(compiled.n == 3);
    assert(compiled.log2m == 10);
    assert(compiled.cdf[0] == 512);
    assert(compiled.cdf[1] == 768);
    assert(compiled.cdf[2] == 1024);

    uint32_t counts[3];
    count_allocs(&compiled, counts);
    assert(counts[0] == 512);
    assert(counts[1] == 256);
    assert(counts[2] == 256);

    /* Dropping the dominant path redistributes its units evenly across the
     * remaining paths, but cdf keeps the original profile. */
    wm_mark_down(&compiled, 0);
    count_allocs(&compiled, counts);
    assert(counts[0] == 0);
    assert(counts[1] == 512);
    assert(counts[2] == 512);
    assert(compiled.cdf[0] == 512);
    assert(compiled.cdf[2] == 1024);

    wm_mark_up(&compiled, 0);
    count_allocs(&compiled, counts);
    assert(counts[0] == 512);
    assert(counts[1] == 256);
    assert(counts[2] == 256);

    /* A one-unit profile is valid and selects the only path. */
    uint32_t unit_one[1] = {1};
    init_profile(&profile, 1, 1, unit_one);
    assert(wm_profile_compile(&profile, &compiled) == 0);
    assert(wm_select(&compiled, 0) == 0);
    assert(wm_select(&compiled, 999) == 0);

    /* Invalid profiles are rejected without touching the output when the
     * failure happens during validation. */
    assert(wm_profile_compile(NULL, &compiled) != 0);
    assert(wm_profile_compile(&profile, NULL) != 0);

    init_profile(&profile, 0, 1, unit_one);
    assert(wm_profile_compile(&profile, &compiled) != 0);

    init_profile(&profile, 3, 1, unit_one);
    assert(wm_profile_compile(&profile, &compiled) != 0);

    init_profile(&profile, 8, 0, unit_one);
    assert(wm_profile_compile(&profile, &compiled) != 0);

    init_profile(&profile, 8, 1, unit_one);
    profile.n = WM_MAX_PATHS + 1;
    assert(wm_profile_compile(&profile, &compiled) != 0);

    uint32_t units_bad[2] = {1, 0};
    init_profile(&profile, 8, 2, units_bad);
    assert(wm_profile_compile(&profile, &compiled) != 0);

    init_profile(&profile, 8, 1, unit_one);
    assert(wm_profile_compile(&profile, &compiled) != 0);

    return 0;
}
