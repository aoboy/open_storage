#include "sdf/wm.h"

uint32_t wm_select(const wm_compiled_t *c, uint64_t counter) {
    return c->unit_to_path[counter & (uint64_t)(c->m - 1u)];
}

void wm_select_batch(const wm_compiled_t *c, uint64_t base, uint32_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        out[i] = wm_select(c, base + i);
    }
}
