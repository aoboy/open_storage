#include "wm_internal.h"

#include <stdint.h>

static uint64_t active_path_mask(uint32_t n) {
    return n >= 64 ? UINT64_MAX : ((UINT64_C(1) << n) - 1u);
}

void wm_mark_down(wm_compiled_t *c, uint32_t path_id) {
    if (c == NULL || path_id >= c->n) {
        return;
    }

    uint64_t bit = UINT64_C(1) << path_id;
    uint64_t old_mask = c->down_mask;
    if ((old_mask & bit) != 0) {
        return;
    }

    /* Never take every path down: with no target, marking has no useful
     * meaning. The last healthy path stays available. */
    uint64_t all = active_path_mask(c->n);
    if (((old_mask | bit) & all) == all) {
        return;
    }

    c->down_mask = old_mask | bit;
    if (sdf_wm_rebuild(c) != 0) {
        c->down_mask = old_mask;
        (void)sdf_wm_rebuild(c);
    }
}

void wm_mark_up(wm_compiled_t *c, uint32_t path_id) {
    if (c == NULL || path_id >= c->n) {
        return;
    }

    uint64_t bit = UINT64_C(1) << path_id;
    uint64_t old_mask = c->down_mask;
    if ((old_mask & bit) == 0) {
        return;
    }

    c->down_mask = old_mask & ~bit;
    if (sdf_wm_rebuild(c) != 0) {
        c->down_mask = old_mask;
        (void)sdf_wm_rebuild(c);
    }
}
