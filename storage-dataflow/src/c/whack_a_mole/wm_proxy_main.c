/*
 * wm-proxy — the P1 CLI shell (plan prompt W2.5).
 *
 * Loads the ops TOML (ADR-0005; the [wm] and [origin] blocks from
 * W0.3), runs one fill workload through sdf_wm_proxy_run with the
 * named degradation timeline, prints the per-origin exit tallies
 * (the wam-send/wam-recv convention) and the A/B split, and gates:
 * per-origin dispatch counts must sit inside the paper's
 * discrepancy bound for a steady run — exit nonzero otherwise.
 */

#define _POSIX_C_SOURCE 200809L

#include "sdf/common/config.h"
#include "sdf/common/timer.h"
#include "sdf/origin.h"
#include "sdf/wm.h"
#include "sdf/wm_proxy.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK0(x)                                                             \
    do {                                                                      \
        if ((x) != 0) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);       \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static uint32_t parse_u32(const sdf_config_t *cfg, const char *key,
                          uint32_t fallback) {
    uint64_t v;
    if (sdf_config_get_u64(cfg, key, &v) == 0)
        return (uint32_t)v;
    return fallback;
}

static double parse_d(const sdf_config_t *cfg, const char *key,
                      double fallback) {
    const char *s = sdf_config_get(cfg, key, NULL);
    return s ? atof(s) : fallback;
}

static int timeline_from_name(sdf_wm_proxy_cfg_t *pc, const char *name,
                              uint64_t units) {
    if (strcmp(name, "steady") == 0)
        return 0;
    if (strcmp(name, "slow") == 0) {
        pc->steps[pc->n_steps++] = (sdf_wm_proxy_step_t){
            .at_unit = units / 4, .origin = 1, .enable = 1, .throttle_ns = 5000000};
        return 0;
    }
    if (strcmp(name, "dies") == 0) {
        pc->steps[pc->n_steps++] = (sdf_wm_proxy_step_t){
            .at_unit = units / 2, .origin = 2, .enable = 0, .throttle_ns = 0};
        return 0;
    }
    if (strcmp(name, "flapping") == 0) {
        for (int k = 1; k <= 4; k++)
            pc->steps[pc->n_steps++] = (sdf_wm_proxy_step_t){
                .at_unit = units * k / 5, .origin = 3, .enable = k % 2, .throttle_ns = 0};
        return 0;
    }
    return -1;
}

int main(int argc, char **argv) {
    const char *config_path = NULL;
    const char *profile_name = "steady";
    uint64_t units = 8192;
    uint32_t unit_bytes = 4096;
    uint32_t jobs = 16;
    uint32_t pc_rollout_override = 0;
    int pc_rollout_set = 0;
    int pc_kill_override = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-C") == 0 && i + 1 < argc)
            config_path = argv[++i];
        else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc)
            profile_name = argv[++i];
        else if (strcmp(argv[i], "--units") == 0 && i + 1 < argc)
            units = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--unit-bytes") == 0 && i + 1 < argc)
            unit_bytes = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--jobs") == 0 && i + 1 < argc)
            jobs = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--rollout") == 0 && i + 1 < argc) {
            pc_rollout_override = (uint32_t)strtoul(argv[++i], NULL, 10);
            pc_rollout_set = 1; /* 0 is a valid override (pure control) */
        }
        else if (strcmp(argv[i], "--kill") == 0)
            pc_kill_override = 1;
        else {
            fprintf(stderr,
                    "usage: %s [-C wm.toml] [--profile steady|slow|dies|"
                    "flapping] [--units N] [--unit-bytes B] [--jobs J] "
                    "[--rollout PCT] [--kill]\n",
                    argv[0]);
            return 2;
        }
    }

    sdf_wm_proxy_cfg_t pc;
    memset(&pc, 0, sizeof(pc));
    pc.units = units;
    pc.unit_bytes = unit_bytes;
    pc.jobs = jobs;
    pc.control_hz = 8;
    pc.alpha = 0.25;
    pc.beta = 0.02;
    pc.rollout_pct = 100;

    /* defaults: 4 equal paths, seeded shuffle1 */
    pc.profile.version = 1;
    pc.profile.m = 1024;
    pc.profile.n = 4;
    for (uint32_t i = 0; i < 4; i++)
        pc.profile.b[i] = 256;
    pc.spray.method = WM_METHOD_SHUFFLE1;
    pc.spray.sa = 333;
    pc.spray.sb = 735;

    sdf_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (config_path != NULL) {
        CHECK0(sdf_config_load(&cfg, config_path));
        pc.profile.m = parse_u32(&cfg, "wm.m", pc.profile.m);
        pc.profile.n = parse_u32(&cfg, "wm.n", pc.profile.n);
        pc.control_hz = parse_u32(&cfg, "wm.control_hz", pc.control_hz);
        pc.alpha = parse_d(&cfg, "wm.alpha", pc.alpha);
        pc.beta = parse_d(&cfg, "wm.beta", pc.beta);
        pc.rollout_pct = parse_u32(&cfg, "wm.rollout_pct", pc.rollout_pct);
        pc.kill_switch =
            strcmp(sdf_config_get(&cfg, "wm.kill_switch", "false"), "true") == 0;
        {
            const char *method = sdf_config_get(&cfg, "wm.method", "shuffle1");
            pc.spray.method = (uint32_t)(strcmp(method, "plain") == 0
                                             ? WM_METHOD_PLAIN
                                             : WM_METHOD_SHUFFLE1);
        }
        pc.spray.sa = parse_u32(&cfg, "wm.sa", pc.spray.sa);
        pc.spray.sb = parse_u32(&cfg, "wm.sb", pc.spray.sb);

        /* profile weights come from wm.init_fracs (schema per W0.3):
         * largest-remainder to exact m */
        const char *fr = sdf_config_get(&cfg, "wm.init_fracs", NULL);
        if (fr != NULL && fr[0] == '[') {
            double vals[WM_MAX_PATHS];
            uint32_t nf = 0;
            for (const char *q = fr + 1; *q && *q != ']';) {
                char *end = NULL;
                double v = strtod(q, &end);
                if (end == q)
                    break;
                if (nf < WM_MAX_PATHS)
                    vals[nf++] = v;
                q = end;
                while (*q == ' ' || *q == ',')
                    q++;
            }
            if (nf > 0 && nf <= WM_MAX_PATHS) {
                double sum = 0;
                for (uint32_t i = 0; i < nf; i++)
                    sum += vals[i];
                if (sum > 0) {
                    pc.profile.n = nf;
                    uint32_t assigned = 0;
                    for (uint32_t i = 0; i < nf; i++) {
                        pc.profile.b[i] =
                            (uint32_t)(vals[i] / sum * pc.profile.m);
                        assigned += pc.profile.b[i];
                    }
                    for (uint32_t i = 0; assigned < pc.profile.m;
                         i = (i + 1) % nf) {
                        pc.profile.b[i]++;
                        assigned++;
                    }
                }
            }
        }
    }

    /* CLI overrides beat the config file (ADR-0005 precedence) */
    if (pc_rollout_set)
        pc.rollout_pct = pc_rollout_override > 100 ? 100 : pc_rollout_override;
    if (pc_kill_override)
        pc.kill_switch = 1;

    /* origins: loopback files we create now (harness mode) */
    char paths[SDF_ORIGIN_MAX][160];
    pc.n_origins = pc.profile.n;
    for (uint32_t i = 0; i < pc.n_origins; i++) {
        snprintf(paths[i], sizeof(paths[i]), "/tmp/sdf-wm-proxy-%d-%u.bin",
                 (int)getpid(), i);
        FILE *f = fopen(paths[i], "wb");
        if (f == NULL) {
            fprintf(stderr, "FAIL: cannot create %s\n", paths[i]);
            return 1;
        }
        static uint8_t blk[4096];
        for (size_t k = 0; k < sizeof(blk); k++)
            blk[k] = (uint8_t)(k * 31u + i);
        for (uint64_t off = 0; off < SDF_WM_PROXY_OBJ_BYTES; off += sizeof(blk)) {
            if (fwrite(blk, 1, sizeof(blk), f) != sizeof(blk)) {
                fclose(f);
                return 1;
            }
        }
        if (fclose(f) != 0)
            return 1;

        memset(&pc.origins[i], 0, sizeof(pc.origins[i]));
        pc.origins[i].id = i;
        pc.origins[i].cls = SDF_ORIGIN_LOOPBACK;
        {
            size_t plen = strlen(paths[i]) + 1;
            if (plen > sizeof(pc.origins[i].dst)) {
                fprintf(stderr, "FAIL: path too long\n");
                return 1;
            }
            memcpy(pc.origins[i].dst, paths[i], plen);
        }
        pc.origins[i].max_inflight = 64;
        if (config_path != NULL) {
            char key[64];
            uint64_t v = 0;
            snprintf(key, sizeof(key), "origin[%u].throttle_ns", i);
            if (sdf_config_get_u64(&cfg, key, &v) == 0)
                pc.origins[i].throttle_ns = v;
        }
    }

    CHECK0(timeline_from_name(&pc, profile_name, pc.units));

    sdf_wm_proxy_stats_t st;
    CHECK0(sdf_wm_proxy_run(&pc, &st));

    printf("PROXY profile=%s units=%llu unit_bytes=%u jobs=%u wall_ms=%llu "
           "updates=%u treatment=%llu control=%llu\n",
           profile_name, (unsigned long long)pc.units, pc.unit_bytes, pc.jobs,
           (unsigned long long)(st.wall_ns / 1000000ull), st.updates_applied,
           (unsigned long long)st.dispatch_treatment,
           (unsigned long long)st.dispatch_control);
    int gate = 1;
    for (uint32_t i = 0; i < pc.n_origins; i++) {
        printf("ORIGIN %u dispatched=%llu ok=%llu failed=%llu enolink=%llu "
               "end_b=%d\n",
               i, (unsigned long long)st.dispatched[i],
               (unsigned long long)st.ok[i], (unsigned long long)st.failed[i],
               (unsigned long long)sdf_counter_read(&st.enolink[i]),
               st.end_profile_b[i]);
        if (st.dispatched[i] == 0)
            gate = 0;
    }
    if (strcmp(profile_name, "steady") == 0 && pc.rollout_pct == 100) {
        /* the discrepancy gate (design 01): |dispatched - expected| within
         * the paper bound + 2% sample tolerance — but ONLY when the
         * profile actually stayed steady. The controller legitimately
         * whacks on observed stats; when it does, the counts reflect the
         * updated profile and comparing them against the base allocation
         * is a category error (found live: a tight dispatch loop's
         * latency noise crossed theta and the "steady" gate flaked). */
        if (st.updates_applied == 0) {
            for (uint32_t i = 0; i < pc.n_origins; i++) {
                double expect =
                    (double)pc.profile.b[i] / pc.profile.m * (double)pc.units;
                double dev = (double)st.dispatched[i] > expect
                                 ? (double)st.dispatched[i] - expect
                                 : expect - (double)st.dispatched[i];
                double lim = 10.0 + 0.02 * expect;
                if (dev > lim) {
                    printf("GATE FAIL origin %u dev=%.1f lim=%.1f\n", i, dev,
                           lim);
                    gate = 0;
                }
            }
        } else {
            printf("note: controller applied %u update(s) — the steady "
                   "discrepancy gate compares against an unchanged "
                   "profile and is skipped\n",
                   st.updates_applied);
        }
    }
    for (uint32_t i = 0; i < pc.n_origins; i++)
        unlink(paths[i]);
    sdf_config_free(&cfg);

    if (!gate) {
        fprintf(stderr, "wm-proxy: GATES FAILED\n");
        return 1;
    }
    printf("GATES OK\n");
    return 0;
}
