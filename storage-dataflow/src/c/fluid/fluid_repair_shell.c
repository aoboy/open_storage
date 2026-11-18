/*
 * fluid-repair — the W3.6 e2e driver (plan prompt W3.6, design 02).
 *
 * One repair job per invocation with the Pareto numbers on stdout:
 * the three-arm A/B (control ARQ, FLUID+LT, FLUID+nanorq) across the
 * loss ladder. Exit nonzero when a block fails to complete — the
 * script gate trusts the exit code.
 */

#define _POSIX_C_SOURCE 200809L

#include "sdf/fluid_repair.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    sdf_fluid_repair_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.arm = SDF_REPAIR_LT;
    cfg.k = 64;
    cfg.sym_len = 32;
    cfg.eps = 0.10;
    cfg.loss_pct = 10.0;
    cfg.wave = 16;
    cfg.n_origins = 3;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--arm") == 0 && i + 1 < argc) {
            const char *a = argv[++i];
            cfg.arm = strcmp(a, "control") == 0
                          ? SDF_REPAIR_CONTROL
                          : (strcmp(a, "nanorq") == 0 ? SDF_REPAIR_NANORQ
                                                      : SDF_REPAIR_LT);
        } else if (strcmp(argv[i], "--eps") == 0 && i + 1 < argc) {
            cfg.eps = atof(argv[++i]);
        } else if (strcmp(argv[i], "--loss") == 0 && i + 1 < argc) {
            cfg.loss_pct = atof(argv[++i]);
        } else if (strcmp(argv[i], "--k") == 0 && i + 1 < argc) {
            cfg.k = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--sym") == 0 && i + 1 < argc) {
            cfg.sym_len = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else {
            fprintf(stderr,
                    "usage: %s [--arm control|lt|nanorq] [--eps E] "
                    "[--loss PCT] [--k K] [--sym T]\n",
                    argv[0]);
            return 2;
        }
    }

    /* stream files (all origins hold the same stream) */
    char paths[8][160];
    uint64_t bytes = sdf_fluid_repair_stream_bytes(&cfg);
    if (bytes == 0)
        return 1;
    /* the stream must cover the arm's own symbols; encode with this
     * cfg's arm directly */
    uint8_t *stream = malloc(bytes);
    if (stream == NULL)
        return 1;
    if (sdf_fluid_repair_encode_stream(&cfg, stream, bytes) != 0)
        return 1;
    for (uint32_t i = 0; i < cfg.n_origins; i++) {
        snprintf(paths[i], sizeof(paths[i]), "/tmp/sdf-fluid-repair-%d-%u.bin",
                 (int)getpid(), i);
        FILE *f = fopen(paths[i], "wb");
        if (f == NULL)
            return 1;
        if (fwrite(stream, 1, bytes, f) != bytes) {
            fclose(f);
            return 1;
        }
        fclose(f);
        cfg.origins[i].id = i;
        cfg.origins[i].cls = SDF_ORIGIN_LOOPBACK;
        size_t plen = strlen(paths[i]) + 1;
        if (plen > sizeof(cfg.origins[i].dst))
            return 1;
        memcpy(cfg.origins[i].dst, paths[i], plen);
        cfg.origins[i].max_inflight = 64;
    }

    sdf_fluid_repair_stats_t st;
    if (sdf_fluid_repair_run(&cfg, &st) != 0)
        return 1;

    for (uint32_t i = 0; i < cfg.n_origins; i++)
        unlink(paths[i]);

    const char *arm = cfg.arm == SDF_REPAIR_CONTROL
                          ? "control"
                          : (cfg.arm == SDF_REPAIR_NANORQ ? "nanorq" : "lt");
    printf("REPAIR arm=%s eps=%.2f loss=%.0f k=%u sent=%llu received=%llu "
           "bytes_tx=%llu rounds=%llu fallback=%d rank=%u ok=%d "
           "overhead=%.2f wall_ms=%llu\n",
           arm, cfg.eps, cfg.loss_pct, cfg.k, (unsigned long long)st.sent,
           (unsigned long long)st.received, (unsigned long long)st.bytes_tx,
           (unsigned long long)st.rounds, st.arq_fallback, st.decode_rank,
           st.ok, cfg.k > 0 ? (double)st.bytes_tx / (double)(cfg.k * cfg.sym_len)
                            : 0.0,
           (unsigned long long)(st.wall_ns / 1000000ull));
    return st.ok == 1 ? 0 : 1;
}
