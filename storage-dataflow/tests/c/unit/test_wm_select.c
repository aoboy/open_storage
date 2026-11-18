#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stdint.h>

#include "sdf/wm.h"

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

    return 0;
}
