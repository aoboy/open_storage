#ifndef SDF_ARC_H
#define SDF_ARC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*arc_apply_fn)(void *ctx, const void *entry, size_t len, uint64_t ts);

typedef struct {
    uint32_t capacity;
    uint32_t batch_max;
    uint64_t batch_deadline_ns;
    uint64_t offset_min_ns;
    uint64_t offset_max_ns;
    double increase_gain;
    double decrease_gain;
} arc_cfg_t;

typedef struct {
    uint64_t offset_ns;
    uint64_t heap_depth;
    uint64_t released;
    uint64_t dropped;
    int drained;
} arc_stats_t;

/*
 * Opaque receiver-side pacer. The concrete layout is private to the ARC
 * implementation; callers must allocate and align the object accordingly.
 */
typedef struct arc_pacer arc_pacer_t;

int arc_init(arc_pacer_t *p, const arc_cfg_t *cfg, arc_apply_fn fn, void *ctx);
int arc_submit(arc_pacer_t *p, const void *entry, size_t len, uint64_t producer_ts);
int arc_run(arc_pacer_t *p);
void arc_stop(arc_pacer_t *p);
arc_stats_t arc_stats(const arc_pacer_t *p);

#ifdef __cplusplus
}
#endif

#endif /* SDF_ARC_H */
