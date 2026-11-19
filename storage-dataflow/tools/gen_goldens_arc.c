/*
 * One-time ARC golden extractor (plan prompt W6.2, ADR-0009).
 *
 * Emits the paper_12_3 entries for golden_vectors/arc_schedule.json
 * on stdout, measured THROUGH the public ARC ABI on the real receiver
 * clock — the same drive test_arc_core proved against the paper's
 * analytic cycle (W4.2): S_n = now − d with the alternating
 * dH=100ms/dL=70ms delay pattern, so A_n − S_n = d + microseconds.
 *
 * Two entries, both ARC_MODE_BASE with auto_params off (the goldens
 * pin the Algorithm-1 fixed point, not the auto-tuning):
 *   linear_cycle   — rho_u=rho_l=1.0, the analytic cycle
 *                    {94.7934, 93.8016} ms
 *   exponent_band  — rho_u=0.5, rho_l=2.0, the paper's second example
 *                    (D band ~[97.9, 100.5] ms)
 *
 * Each entry records the steady-state D trajectory: `sample_blocks`
 * consecutive submit indices with the measured D after the arrival.
 * The validator (test_parity) replays the identical drive and checks
 * every sample within tolerance_ns — the clock epsilon perturbs D by
 * O(lambda*epsilon), far inside the 500 us band.
 *
 * Extracted, never hand-derived:
 *
 *   cc -O2 -o /tmp/gen_goldens_arc tools/gen_goldens_arc.c \
 *      -Iinclude build/libsdf_arc.a build/libsdf_common.a -lm
 *   /tmp/gen_goldens_arc > /tmp/arc_entries.json
 *   (merge into golden_vectors/arc_schedule.json)
 */

#include "sdf/arc.h"
#include "sdf/common/timer.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    double rho_u, rho_l;
    double lambda_u, lambda_l;
    int drive_blocks;
    int sample_blocks;
} entry_spec_t;

static uint64_t d_of(int n) {
    /* the paper's alternating pattern: high on even, low on odd */
    return (n % 2 == 0) ? 100000000ull : 70000000ull;
}

static int emit_entry(const entry_spec_t *spec) {
    arc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.capacity = 64;
    cfg.entry_max = 64;
    cfg.mode = ARC_MODE_BASE;
    cfg.auto_params = 0;
    cfg.min_smooth_bytes = 0; /* everything through the scheduler */
    cfg.rho_u = spec->rho_u;
    cfg.rho_l = spec->rho_l;
    cfg.lambda_u = spec->lambda_u;
    cfg.lambda_l = spec->lambda_l;
    cfg.U = 100000000ull;
    cfg.delta = 100000000ull;
    cfg.J = 0;
    cfg.G = 50000000ull;
    cfg.T_idle_ns = 1000000000000ull; /* no mid-drive re-anchors */

    size_t sz = arc_sizeof(&cfg);
    if (sz == 0)
        return 1;
    void *p = malloc(sz);
    if (p == NULL || arc_init(p, &cfg, NULL, NULL) != 0) {
        free(p);
        return 1;
    }

    printf("    {\"name\": \"%s\", \"drive_blocks\": %d, "
           "\"sample_blocks\": %d,\n"
           "     \"d_high_ns\": 100000000, \"d_low_ns\": 70000000,\n"
           "     \"rho_u\": %.3f, \"rho_l\": %.3f, "
           "\"lambda_u\": %.3f, \"lambda_l\": %.3f,\n"
           "     \"U_ns\": 100000000, \"delta_ns\": 100000000, "
           "\"J_ns\": 0,\n"
           "     \"tolerance_ns\": 500000,\n"
           "     \"samples\": [",
           spec->name, spec->drive_blocks, spec->sample_blocks,
           spec->rho_u, spec->rho_l, spec->lambda_u, spec->lambda_l);

    int first = 1;
    int sample_at = spec->drive_blocks - spec->sample_blocks;
    for (int n = 0; n < spec->drive_blocks; n++) {
        uint64_t d = d_of(n);
        uint64_t Sn = sdf_now_ns() - d;
        uint8_t entry[64];
        memset(entry, (uint8_t)n, sizeof(entry));
        if (arc_submit(p, entry, sizeof(entry), Sn) != 0) {
            free(p);
            return 1;
        }
        (void)arc_poll(p, UINT64_MAX / 4);
        if (n >= sample_at) {
            arc_stats_t st = arc_stats(p);
            printf("%s{\"n\": %d, \"D_ns\": %.0f}", first ? "" : ", ", n,
                   st.D);
            first = 0;
        }
    }
    printf("]}\n");
    free(p);
    return 0;
}

int main(void) {
    static const entry_spec_t specs[] = {
        {"linear_cycle", 1.0, 1.0, 0.16, 0.04, 2000, 100},
        {"exponent_band", 0.5, 2.0, 0.16, 0.04, 3000, 100},
    };

    printf("[\n");
    for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
        if (emit_entry(&specs[i]) != 0)
            return 1;
        if (i + 1 < sizeof(specs) / sizeof(specs[0]))
            printf("    ,\n");
    }
    printf("]\n");
    return 0;
}
