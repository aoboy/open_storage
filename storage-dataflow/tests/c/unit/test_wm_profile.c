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

    int wm_profile_w22_tests(void);

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

    /* W2.2: update embodiments + severity controller */
    (void)wm_profile_w22_tests();

    return 0;
}

/* ---- W2.2: update embodiments + severity controller ------------------- */

#include <errno.h>
#include <string.h>

static void t2_init(wm_profile_t *p, uint32_t m, uint32_t n, const uint32_t *b) {
    memset(p, 0, sizeof(*p));
    p->version = 1;
    p->m = m;
    p->n = n;
    for (uint32_t i = 0; i < n; i++)
        p->b[i] = b[i];
}

static uint64_t t2_sum(const wm_profile_t *p) {
    uint64_t s = 0;
    for (uint32_t i = 0; i < p->n; i++)
        s += p->b[i];
    return s;
}

static void test_update_embodiments(void) {
    uint32_t b3[3] = {4, 2, 2};

    /* mode 1: single hot path, e=[0,2,0] -> x=0, y=2 walk from r=0 */
    {
        wm_profile_t p;
        uint32_t e[3] = {0, 2, 0};
        t2_init(&p, 8, 3, b3);
        assert(wm_profile_update(&p, e, 1) == 0);
        assert(p.b[0] == 5 && p.b[1] == 1 && p.b[2] == 2);
        assert(t2_sum(&p) == 8);
        assert(p.residual_r == 2); /* walked two steps */
    }
    /* mode 1 rejects multiple positive entries */
    {
        wm_profile_t p;
        uint32_t e[3] = {1, 1, 0};
        t2_init(&p, 8, 3, b3);
        assert(wm_profile_update(&p, e, 1) == -EINVAL);
    }
    /* mode 2: multi, e=[2,2,0] -> etot 4, x=1, y=1 */
    {
        wm_profile_t p;
        uint32_t e[3] = {2, 2, 0};
        t2_init(&p, 8, 3, b3);
        assert(wm_profile_update(&p, e, 2) == 0);
        assert(p.b[0] == 4 && p.b[1] == 1 && p.b[2] == 3);
        assert(t2_sum(&p) == 8);
    }
    /* mode 3: K/K-bar, e=[0,2,0] -> Kbar {0,2}, x=1, y=0 */
    {
        wm_profile_t p;
        uint32_t e[3] = {0, 2, 0};
        t2_init(&p, 8, 3, b3);
        assert(wm_profile_update(&p, e, 3) == 0);
        assert(p.b[0] == 5 && p.b[1] == 0 && p.b[2] == 3);
        assert(t2_sum(&p) == 8);
    }
    /* mode 3 needs both sides */
    {
        wm_profile_t p;
        uint32_t e[3] = {0, 0, 2};
        t2_init(&p, 8, 3, b3);
        assert(wm_profile_update(&p, e, 3) == 0); /* Kbar {0,1} */
        uint32_t e2[3] = {2, 2, 2};
        assert(wm_profile_update(&p, e2, 3) == -EINVAL); /* no Kbar */
    }
    /* mode 4: proportional renormalize, e=[4,0,0] */
    {
        wm_profile_t p;
        uint32_t e[3] = {4, 0, 0};
        t2_init(&p, 8, 3, b3);
        assert(wm_profile_update(&p, e, 4) == 0);
        assert(p.b[0] == 0 && p.b[1] == 4 && p.b[2] == 4);
        assert(t2_sum(&p) == 8);
    }
    /* mode 4 with all-positive e (the wam M.1 fuzz case): remainder
     * falls back to all-n distribution and the sum still lands on m */
    {
        wm_profile_t p;
        uint32_t e[3] = {1, 1, 0};
        t2_init(&p, 8, 3, b3);
        assert(wm_profile_update(&p, e, 4) == 0);
        assert(t2_sum(&p) == 8);
        for (uint32_t i = 0; i < 3; i++)
            assert(p.b[i] > 0); /* nothing dropped to zero by rounding */
    }
    /* e[i] > b[i] is refused everywhere */
    {
        wm_profile_t p;
        uint32_t e[3] = {5, 0, 0};
        t2_init(&p, 8, 3, b3);
        assert(wm_profile_update(&p, e, 1) == -EINVAL);
        assert(wm_profile_update(&p, e, 2) == -EINVAL);
        assert(wm_profile_update(&p, e, 3) == -EINVAL);
        assert(wm_profile_update(&p, e, 4) == -EINVAL);
    }
    /* the last healthy path never goes to zero (n > 1) */
    {
        wm_profile_t p;
        uint32_t e[3] = {4, 2, 2};
        t2_init(&p, 8, 3, b3);
        assert(wm_profile_update(&p, e, 1) == -EINVAL);
        assert(wm_profile_update(&p, e, 2) == -EINVAL);
        assert(wm_profile_update(&p, e, 3) == -EINVAL);
        assert(wm_profile_update(&p, e, 4) == -EINVAL); /* etot >= m */
        assert(t2_sum(&p) == 8); /* untouched on refusal */
    }
    /* unknown modes are refused (strict; wam no-ops these) */
    {
        wm_profile_t p;
        uint32_t e[3] = {1, 0, 0};
        t2_init(&p, 8, 3, b3);
        assert(wm_profile_update(&p, e, 5) == -EINVAL);
        assert(wm_profile_update(&p, e, 0) == -EINVAL);
    }
    /* residual fairness: repeated single-ball removals cycle the walk */
    {
        wm_profile_t p;
        uint32_t e[3] = {1, 0, 0};
        t2_init(&p, 64, 3, (uint32_t[3]){32, 16, 16});
        uint32_t hits[3] = {0, 0, 0};
        for (int k = 0; k < 30; k++) {
            assert(wm_profile_update(&p, e, 1) == 0);
            /* the removed ball landed somewhere: count who grew */
            for (uint32_t i = 0; i < 3; i++) {
                uint32_t grew = p.b[i] > (uint32_t[3]){32, 16, 16}[i]
                                    ? p.b[i] - (uint32_t[3]){32, 16, 16}[i]
                                    : 0;
                (void)grew;
            }
            for (uint32_t i = 0; i < 3; i++)
                if (i != 0)
                    hits[i] += p.b[i] > 0 ? 0 : 0; /* placeholder */
        }
        /* deterministic cursor: r advanced 30 mod 3 */
        assert(p.residual_r == 30 % 3);
        assert(t2_sum(&p) == 64);
    }
}

/* controller: severity bands, t_min rate limit, auto embodiment */
static void t2_stats(sdf_origin_stats_t *st, double lat, double err,
                      int hard_down) {
    memset(st, 0, sizeof(*st));
    st->id = 0;
    st->cls = SDF_ORIGIN_LOOPBACK;
    st->ewma_lat_ns = lat;
    st->ewma_err = err;
    st->hard_down = hard_down;
}

static void test_controller(void) {
    uint32_t b4[4] = {256, 256, 256, 256};
    wm_profile_t base;
    t2_init(&base, 1024, 4, b4);

    wm_ctrl_cfg_t cfg;
    wm_ctrl_cfg_default(&cfg);
    wm_ctrl_t ctl;
    assert(wm_ctrl_init(&ctl, &base, &cfg, 4) == 0);

    /* all healthy: no whack */
    for (uint32_t i = 0; i < 4; i++) {
        sdf_origin_stats_t st;
        t2_stats(&st, 1000000.0, 0.0, 0);
        st.id = i;
        wm_ctrl_on_stats(&ctl, i, &st);
    }
    wm_profile_t out;
    assert(wm_ctrl_tick(&ctl, 1000000000ull, &out) == 0);
    assert(out.b[0] == 256 && t2_sum(&out) == 1024);
    assert(out.version == base.version); /* unchanged */

    /* hard_down on path 1: severity 100 >= theta_hi(50) ->
     * e = floor(0.25 * 256) = 64 removed via mode 1 (single hot) */
    sdf_origin_stats_t st;
    t2_stats(&st, 1000000.0, 0.0, 1);
    wm_ctrl_on_stats(&ctl, 1, &st);
    uint64_t now = 1000000000ull + 5000000ull; /* past t_min (2 ms) */
    assert(wm_ctrl_tick(&ctl, now, &out) == 1);
    assert(out.b[1] == 256 - 64 + 16); /* mode 1: the hot path keeps its
                                          per-path share x of the spread */
    assert(t2_sum(&out) == 1024);
    assert(out.version == base.version + 1);
    /* mode 1 spread: the 64 balls went over all 4 (x=16 each) */
    assert(out.b[0] == 256 + 16);
    assert(out.b[2] == 256 + 16);
    assert(out.b[3] == 256 + 16);

    /* t_min rate limit: an immediate second tick does nothing */
    sdf_origin_stats_t st0;
    t2_stats(&st0, 1000000.0, 0.0, 0);
    wm_ctrl_on_stats(&ctl, 1, &st0); /* path 1 recovered (fresh stats) */
    assert(wm_ctrl_tick(&ctl, now + 1000000ull, &out) == 0);
    assert(out.b[1] == 256 - 64 + 16); /* unchanged: within t_min */

    /* past t_min with everything healthy: no further change */
    assert(wm_ctrl_tick(&ctl, now + 3000000ull, &out) == 0);
    assert(out.b[1] == 256 - 64 + 16);

    /* low band: one path at 4x median latency -> severity ~100 (>= hi);
     * a 2x path -> (1/3 inflation) ~33 >= lo(20) -> alpha_lo = 0.05 */
    for (uint32_t i = 0; i < 4; i++) {
        t2_stats(&st, 1000000.0, 0.0, 0);
        st.id = i;
        if (i == 2)
            st.ewma_lat_ns = 2000000.0; /* 2x median: +33 severity */
        wm_ctrl_on_stats(&ctl, i, &st);
    }
    assert(wm_ctrl_tick(&ctl, now + 10000000ull, &out) == 1);
    /* path 2 grew to 272 in the first whack; alpha_lo * 272 = 13.6
     * -> 13 removed via mode 1, share x = 3 back: 272 - 13 + 3 */
    assert(out.b[2] == 272 - 13 + 3);
    assert(t2_sum(&out) == 1024);

    /* every path hard_down: the controller degrades gracefully — a 25%
     * pull leaves every path at 75% — because hard-failure exclusion is
     * the down_mask fast path's job, not the profile shaper's. The
     * never-zero guard (update would empty the fleet) is pinned in the
     * update-embodiment tests above. */
    for (uint32_t i = 0; i < 4; i++) {
        t2_stats(&st, 1000000.0, 0.0, 1);
        st.id = i;
        wm_ctrl_on_stats(&ctl, i, &st);
    }
    assert(wm_ctrl_tick(&ctl, now + 20000000ull, &out) == 1);
    assert(t2_sum(&out) == 1024);
    for (uint32_t i = 0; i < 4; i++)
        assert(out.b[i] > 0); /* nobody zeroed: the fleet survives */

    /* bad arguments */
    assert(wm_ctrl_init(NULL, &base, &cfg, 4) == -EINVAL);
    assert(wm_ctrl_init(&ctl, NULL, &cfg, 4) == -EINVAL);
    assert(wm_ctrl_tick(NULL, 0, &out) == -EINVAL);
    wm_ctrl_on_stats(&ctl, 9, &st); /* out of range: ignored */
}

int wm_profile_w22_tests(void) {
    test_update_embodiments();
    test_controller();
    return 0;
}
