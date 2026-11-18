#ifndef SDF_WM_H
#define SDF_WM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WM_MAX_PATHS 64
#define WM_MAX_UNITS 1024
#define WM_PROFILE_VERSION 1

/*
 * A path profile is a discrete allocation of m selection units over n paths.
 * b[i] / m is the target fraction of traffic for path i. The sum of b[i]
 * must equal m.
 *
 * The version field lets readers detect a profile replacement while fills
 * are in flight. Bump it on every publish.
 */
typedef struct {
    uint32_t version;
    uint32_t m;
    uint32_t n;
    uint32_t b[WM_MAX_PATHS];
} wm_profile_t;

/*
 * Compiled, read-mostly form of a profile. Selection is a single table
 * lookup: unit_to_path[counter & (m - 1)].
 *
 * down_mask is maintained by wm_mark_down() / wm_mark_up() so the C fast
 * path can react to hard failures without waiting for the Python controller.
 * cdf always describes the original profile, not the temporarily renormalized
 * one, so marking a path back up restores the exact original allocation.
 */
typedef struct {
    uint32_t m;
    uint32_t n;
    uint32_t log2m;
    uint32_t reserved;
    uint64_t down_mask;
    uint32_t cdf[WM_MAX_PATHS];
    uint32_t perm[WM_MAX_UNITS];
    uint32_t unit_to_path[WM_MAX_UNITS];
} wm_compiled_t;

/* Compile a profile. Returns 0 on success, negative errno-style value on error. */
int wm_profile_compile(const wm_profile_t *p, wm_compiled_t *out);

/* Select one path for a monotonic unit counter. Lock-free and allocation-free. */
uint32_t wm_select(const wm_compiled_t *c, uint64_t counter);

/* Batched form of wm_select. Base is the counter for out[0]. */
void wm_select_batch(const wm_compiled_t *c, uint64_t base, uint32_t *out, size_t n);

/* Fast-path congestion response used before the Python controller reacts. */
void wm_mark_down(wm_compiled_t *c, uint32_t path_id);
void wm_mark_up(wm_compiled_t *c, uint32_t path_id);

#ifdef __cplusplus
}
#endif

#endif /* SDF_WM_H */
