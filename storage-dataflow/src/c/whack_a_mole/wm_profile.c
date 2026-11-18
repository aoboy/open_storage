#include "wm_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define WM_COUNTS_TMP WM_MAX_PATHS

static uint32_t bit_reverse(uint32_t value, uint32_t bits) {
    uint32_t reversed = 0;
    for (uint32_t i = 0; i < bits; i++) {
        reversed = (reversed << 1) | (value & 1u);
        value >>= 1;
    }
    return reversed;
}

static bool is_power_of_two(uint32_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

/* Build perm (theta), unit_to_path (counter -> path) and path_of_key
 * (key -> path) from a cumulative allocation. perm is a bijection over
 * [0, m), so one pass fills path_of_key for every possible key — the
 * shuffle methods resolve through it. */
static void build_unit_table(wm_compiled_t *c, const uint32_t *cdf) {
    for (uint32_t j = 0; j < c->m; j++) {
        uint32_t x = bit_reverse(j, c->log2m);
        uint32_t path = 0;

        c->perm[j] = x;
        while (path + 1 < c->n && x >= cdf[path]) {
            path++;
        }
        c->unit_to_path[j] = path;
        c->path_of_key[x] = path;
    }
}

int wm_profile_compile(const wm_profile_t *p, wm_compiled_t *out) {
    /* PLAIN default: seeds (0, 1) — sa=0 is the identity start, sb=1
     * is the (odd) identity stride, matching pre-ADR-0007 behavior. */
    static const wm_compile_cfg_t plain = {
        .method = WM_METHOD_PLAIN, .sa = 0, .sb = 1};
    return wm_profile_compile_cfg(p, out, &plain);
}

int wm_profile_compile_cfg(const wm_profile_t *p, wm_compiled_t *out,
                           const wm_compile_cfg_t *cfg) {
    if (p == NULL || out == NULL || cfg == NULL) {
        return -EINVAL;
    }
    if (p->m == 0 || p->m > WM_MAX_UNITS || !is_power_of_two(p->m)) {
        return -EINVAL;
    }
    if (p->n == 0 || p->n > WM_MAX_PATHS) {
        return -EINVAL;
    }
    if (cfg->method > WM_METHOD_SHUFFLE2) {
        return -EINVAL;
    }
    if (p->m >= 2 && (cfg->sa >= p->m || cfg->sb == 0 || cfg->sb >= p->m ||
                      (cfg->sb & 1u) == 0)) {
        return -EINVAL; /* sb odd and both seeds < m (ADR-0007);
                         * m == 1 has no key domain to decorrelate */
    }

    uint64_t sum = 0;
    for (uint32_t i = 0; i < p->n; i++) {
        sum += p->b[i];
    }
    if (sum != p->m) {
        return -EINVAL;
    }

    memset(out, 0, sizeof(*out));
    out->m = p->m;
    out->n = p->n;

    uint32_t log2m = 0;
    while ((UINT32_C(1) << log2m) < p->m) {
        log2m++;
    }
    out->log2m = log2m;
    out->down_mask = 0;
    out->method = cfg->method;
    out->sa = cfg->sa;
    out->sb = cfg->sb;

    uint32_t cumulative = 0;
    for (uint32_t i = 0; i < p->n; i++) {
        cumulative += p->b[i];
        out->cdf[i] = cumulative;
    }

    build_unit_table(out, out->cdf);
    return 0;
}

void wm_seed_rotate(wm_compile_cfg_t *cfg, uint32_t m) {
    if (cfg == NULL || m < 2 || (m & (m - 1u)) != 0) {
        return;
    }
    uint32_t mask = m - 1u;
    cfg->sa = (cfg->sa + cfg->sb) & mask;
    cfg->sb = ((cfg->sb + 2u) & mask) | 1u; /* stays odd, nonzero, < m */
}

int sdf_wm_rebuild(wm_compiled_t *c) {
    if (c == NULL || c->m == 0 || c->n == 0 || c->n > WM_MAX_PATHS) {
        return -EINVAL;
    }

    uint32_t base[WM_COUNTS_TMP] = {0};
    uint32_t active[WM_COUNTS_TMP] = {0};
    uint32_t scaled[WM_COUNTS_TMP] = {0};
    uint32_t fractions[WM_COUNTS_TMP] = {0};
    uint32_t local_cdf[WM_COUNTS_TMP] = {0};
    bool used[WM_COUNTS_TMP] = {false};

    uint32_t previous = 0;
    uint64_t total = 0;
    for (uint32_t i = 0; i < c->n; i++) {
        base[i] = c->cdf[i] - previous;
        previous = c->cdf[i];
        if ((c->down_mask & (UINT64_C(1) << i)) == 0) {
            active[i] = base[i];
            total += base[i];
        }
    }

    /* If every path is down (or down paths held every unit), fall back to the
     * original profile rather than leaving selection with no target. */
    if (total == 0) {
        for (uint32_t i = 0; i < c->n; i++) {
            active[i] = base[i];
            total += base[i];
        }
    }
    if (total == 0) {
        return -EINVAL;
    }

    uint64_t assigned = 0;
    for (uint32_t i = 0; i < c->n; i++) {
        uint64_t product = (uint64_t)active[i] * c->m;
        scaled[i] = (uint32_t)(product / total);
        fractions[i] = (uint32_t)(product % total);
        assigned += scaled[i];
    }

    uint32_t remainder = c->m - (uint32_t)assigned;
    while (remainder > 0) {
        uint32_t best = WM_MAX_PATHS;
        for (uint32_t i = 0; i < c->n; i++) {
            if (active[i] == 0 || used[i]) {
                continue;
            }
            if (best == WM_MAX_PATHS || fractions[i] > fractions[best] ||
                (fractions[i] == fractions[best] && i < best)) {
                best = i;
            }
        }
        if (best == WM_MAX_PATHS) {
            break;
        }
        scaled[best]++;
        used[best] = true;
        remainder--;
    }

    /* The largest-remainder pass should always finish; this is a defensive
     * fallback that keeps the table a valid allocation. */
    for (uint32_t i = 0; remainder > 0 && i < c->n; i++) {
        if (active[i] != 0) {
            scaled[i]++;
            remainder--;
        }
    }
    if (remainder != 0) {
        return -EINVAL;
    }

    uint32_t cumulative = 0;
    for (uint32_t i = 0; i < c->n; i++) {
        cumulative += scaled[i];
        local_cdf[i] = cumulative;
    }
    if (cumulative != c->m) {
        return -EINVAL;
    }

    build_unit_table(c, local_cdf);
    return 0;
}
