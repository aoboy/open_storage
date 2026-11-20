/*
 * fluid-repair — the W3.6/W6.3 e2e driver (design 02).
 *
 * One repair job per invocation with the Pareto numbers on stdout:
 * the three-arm A/B (control ARQ, FLUID+LT, FLUID+nanorq) across the
 * loss ladder, and — since W6.3 — N-block objects with per-block
 * evidence. Exit nonzero when a block fails to complete byte-exact;
 * the script gate trusts the exit code.
 *
 * W6.15 origin modes: without -C (or with --harness) the shell generates
 * the deterministic loopback stream files. With -C and no --harness it
 * uses the configured [[origin]] entries exactly — including HTTP URLs —
 * and never rewrites their class or destination.
 */

#define _POSIX_C_SOURCE 200809L

#include "sdf/common/config.h"
#include "sdf/common/metrics_endpoint.h"
#include "sdf/fluid_repair.h"
#include "sdf/origin/config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int repair_render(void *ud, FILE *out) {
    return sdf_fluid_repair_render_metrics(out, ud);
}

static void remove_paths(char paths[][160], uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        unlink(paths[i]);
    }
}

int main(int argc, char **argv) {
    const char *config_path = NULL;
    int harness_mode = 0;
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
                          : (strcmp(a, "nanorq") == 0 ? SDF_REPAIR_NANORQ : SDF_REPAIR_LT);
        } else if (strcmp(argv[i], "-C") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--harness") == 0) {
            harness_mode = 1;
        } else if (strcmp(argv[i], "--eps") == 0 && i + 1 < argc) {
            cfg.eps = atof(argv[++i]);
        } else if (strcmp(argv[i], "--loss") == 0 && i + 1 < argc) {
            cfg.loss_pct = atof(argv[++i]);
        } else if (strcmp(argv[i], "--k") == 0 && i + 1 < argc) {
            cfg.k = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--sym") == 0 && i + 1 < argc) {
            cfg.sym_len = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--blocks") == 0 && i + 1 < argc) {
            cfg.n_blocks = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--wave") == 0 && i + 1 < argc) {
            cfg.wave = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else {
            fprintf(stderr,
                    "usage: %s [-C fluid.toml] [--harness] "
                    "[--arm control|lt|nanorq] [--eps E] [--loss PCT] "
                    "[--k K] [--sym T] [--blocks N] [--wave W]\n"
                    "  SDF_METRICS_ENDPOINT=host:port serves the live scrape\n"
                    "  Without -C, generated loopback origins are used; with\n"
                    "  -C, [[origin]] entries are used unless --harness is set\n",
                    argv[0]);
            return 2;
        }
    }

    sdf_config_t file_cfg;
    memset(&file_cfg, 0, sizeof(file_cfg));
    if (config_path != NULL) {
        if (sdf_config_load(&file_cfg, config_path) != 0) {
            fprintf(stderr, "fluid-repair: cannot load strict TOML %s\n", config_path);
            return 1;
        }
    }

    char paths[SDF_REPAIR_MAX_ORIGINS][160];
    uint32_t generated_count = 0;
    int generated_origins = config_path == NULL || harness_mode;
    int cleanup_rc = 1;

    if (!generated_origins) {
        if (sdf_origin_config_load(&file_cfg, cfg.origins, SDF_REPAIR_MAX_ORIGINS,
                                   &cfg.n_origins) != 0) {
            fprintf(stderr,
                    "fluid-repair: invalid, absent, sparse, or oversized [[origin]] "
                    "configuration (max %u)\n",
                    SDF_REPAIR_MAX_ORIGINS);
            goto cleanup;
        }
    } else {
        uint64_t bytes = sdf_fluid_repair_stream_bytes(&cfg);
        if (bytes == 0) {
            goto cleanup;
        }
        uint8_t *stream = malloc(bytes);
        if (stream == NULL) {
            goto cleanup;
        }
        if (sdf_fluid_repair_encode_stream(&cfg, stream, bytes) != 0) {
            free(stream);
            goto cleanup;
        }
        for (uint32_t i = 0; i < cfg.n_origins; i++) {
            snprintf(paths[i], sizeof(paths[i]), "/tmp/sdf-fluid-repair-%d-%u.bin", (int)getpid(),
                     i);
            FILE *f = fopen(paths[i], "wb");
            if (f == NULL) {
                free(stream);
                goto cleanup;
            }
            if (fwrite(stream, 1, bytes, f) != bytes || fclose(f) != 0) {
                unlink(paths[i]);
                free(stream);
                goto cleanup;
            }
            generated_count++;
            cfg.origins[i].id = i;
            cfg.origins[i].cls = SDF_ORIGIN_LOOPBACK;
            size_t plen = strlen(paths[i]) + 1;
            if (plen > sizeof(cfg.origins[i].dst)) {
                free(stream);
                goto cleanup;
            }
            memcpy(cfg.origins[i].dst, paths[i], plen);
            cfg.origins[i].max_inflight = 64;
        }
        free(stream);
    }

    sdf_fluid_repair_stats_t st;
    memset(&st, 0, sizeof(st));
    st.arm = (uint32_t)cfg.arm;
    st.eps = cfg.arm == SDF_REPAIR_CONTROL ? 0.0 : cfg.eps;
    sdf_metrics_endpoint_t *ep = NULL;
    const char *mep = getenv("SDF_METRICS_ENDPOINT");
    if (mep != NULL && mep[0] != '\0') {
        ep = sdf_metrics_endpoint_start(mep, repair_render, &st);
        if (ep == NULL) {
            goto cleanup;
        }
    }
    int run_rc = sdf_fluid_repair_run(&cfg, &st);
    if (ep != NULL) {
        sdf_metrics_endpoint_stop(ep);
    }
    if (run_rc != 0) {
        fprintf(stderr, "fluid-repair: run failed: %s (%d)\n", strerror(-run_rc), run_rc);
        goto cleanup;
    }

    const char *arm = cfg.arm == SDF_REPAIR_CONTROL
                          ? "control"
                          : (cfg.arm == SDF_REPAIR_NANORQ ? "nanorq" : "lt");
    printf("REPAIR arm=%s eps=%.2f loss=%.0f k=%u sent=%llu received=%llu "
           "bytes_tx=%llu rounds=%llu fallback=%d rank=%u ok=%d "
           "overhead=%.2f wall_ms=%llu",
           arm, cfg.eps, cfg.loss_pct, cfg.k, (unsigned long long)st.sent,
           (unsigned long long)st.received, (unsigned long long)st.bytes_tx,
           (unsigned long long)st.rounds, st.arq_fallback, st.decode_rank, st.ok,
           cfg.k > 0 && cfg.sym_len > 0 && st.n_blocks > 0
               ? (double)st.bytes_tx / ((double)cfg.k * (double)cfg.sym_len * (double)st.n_blocks)
               : 0.0,
           (unsigned long long)(st.wall_ns / 1000000ull));
    /* Multi-block evidence (W6.3): per-block rounds, fallback, rank
     * and the byte-exact gate. Single-block lines stay unchanged. */
    if (st.n_blocks > 1) {
        printf(" blocks=%u block_rounds=", st.n_blocks);
        for (uint32_t b = 0; b < st.n_blocks; b++) {
            printf("%s%llu", b ? "," : "", (unsigned long long)st.blocks[b].rounds);
        }
        printf(" block_fallback=");
        for (uint32_t b = 0; b < st.n_blocks; b++) {
            printf("%s%d", b ? "," : "", st.blocks[b].arq_fallback);
        }
        printf(" block_rank=");
        for (uint32_t b = 0; b < st.n_blocks; b++) {
            printf("%s%u", b ? "," : "", st.blocks[b].decode_rank);
        }
        printf(" block_ok=");
        for (uint32_t b = 0; b < st.n_blocks; b++) {
            printf("%s%d", b ? "," : "", st.blocks[b].ok);
        }
    }
    printf("\n");
    cleanup_rc = st.ok == 1 ? 0 : 1;

cleanup:
    remove_paths(paths, generated_count);
    sdf_config_free(&file_cfg);
    return cleanup_rc;
}
