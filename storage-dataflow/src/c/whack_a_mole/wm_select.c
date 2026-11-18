/*
 * wm_select — spray-key path selection (plan prompt W2.1, ADR-0007).
 *
 * Key transforms per [WAM] section 4, adapted from wack_a_mole
 * include/wam/spray.h @ a0f0422 (wam_spray_key):
 *   PLAIN     key = theta(counter)                        one load
 *   SHUFFLE1  key = theta((sa + counter*sb) mod m)        seeded start
 *   SHUFFLE2  key = (sa + sb*theta(counter)) mod m        seeded stride
 * theta lives in the compiled perm table (perm[j] = theta(j)), and
 * every key resolves through path_of_key — both tables are rebuilt
 * under down_mask renormalization, so mark_down/up behave identically
 * across methods. 32-bit wrap in the seed arithmetic is equivalent to
 * the mod-2^ell reduction (m divides 2^32) — the same identity the
 * wam_spray_key arithmetic relies on.
 */

#include "sdf/wm.h"

uint32_t wm_select(const wm_compiled_t *c, uint64_t counter) {
    if (c == NULL || c->m < 2) {
        return 0;
    }
    uint32_t mask = c->m - 1u;
    uint32_t j = (uint32_t)(counter & (uint64_t)mask);

    switch (c->method) {
    case WM_METHOD_SHUFFLE1:
        return c->path_of_key[c->perm[(c->sa + j * c->sb) & mask]];
    case WM_METHOD_SHUFFLE2:
        return c->path_of_key[(c->sa + c->sb * c->perm[j]) & mask];
    case WM_METHOD_PLAIN:
    default:
        return c->unit_to_path[j];
    }
}

void wm_select_batch(const wm_compiled_t *c, uint64_t base, uint32_t *out, size_t n) {
    if (c == NULL || out == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = wm_select(c, base + i);
    }
}
