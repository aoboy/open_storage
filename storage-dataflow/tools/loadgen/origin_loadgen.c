/*
 * origin_loadgen — multi-origin dispatch harness (plan prompt W1.4).
 *
 * Drives K units through a compiled WM profile across N loopback
 * origins, applies the canonical degradation timelines, and prints
 * per-origin tallies with the accounting gate built in:
 *
 *   steady   : every unit completes — per-origin completions must
 *              equal the wm_select assignment counts EXACTLY (the
 *              W1.4 acceptance), zero failures
 *   slow     : origin 1 throttled at 25% of units (5 ms/unit)
 *   dies     : origin 2 drained (set_enable down) at 50% of units —
 *              subsequent picks fail fast with -ENOLINK
 *   flapping : origin 3 toggled every 512 units
 *
 * All profiles assert full accounting: ok + failed + enolink == K
 * and per-origin == assigned. Exit is nonzero on any gate failure,
 * so scripts (W2.6 e2e) can trust the exit code. Timelines are also
 * described declaratively in tests/system/profiles/ for reuse.
 */

#define _POSIX_C_SOURCE 200809L

#include "sdf/common/timer.h"
#include "sdf/origin.h"
#include "sdf/origin/loopback.h"
#include "sdf/wm.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LG_FILE_BYTES (1u << 20)
#define LG_MAX_ORIGINS 16

#define CHECK0(x)                                                             \
    do {                                                                      \
        if ((x) != 0) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);       \
            exit(1);                                                          \
        }                                                                     \
    } while (0)

typedef enum { P_STEADY, P_SLOW, P_DIES, P_FLAPPING } profile_t;

static profile_t g_profile = P_STEADY;
static uint32_t g_n = 4;
static uint64_t g_units = 4096;
static uint32_t g_unit_bytes = 4096;
static uint32_t g_m = 1024;

static _Atomic uint64_t g_ok[LG_MAX_ORIGINS];
static _Atomic uint64_t g_failed[LG_MAX_ORIGINS];
static uint64_t g_assigned[LG_MAX_ORIGINS];
static uint64_t g_enolink[LG_MAX_ORIGINS];

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--profile steady|slow|dies|flapping] "
                   "[--origins N] [--units K] [--unit-bytes B] [--m M]\n",
            argv0);
}

static int parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "steady") == 0)
                g_profile = P_STEADY;
            else if (strcmp(argv[i], "slow") == 0)
                g_profile = P_SLOW;
            else if (strcmp(argv[i], "dies") == 0)
                g_profile = P_DIES;
            else if (strcmp(argv[i], "flapping") == 0)
                g_profile = P_FLAPPING;
            else
                return -1;
        } else if (strcmp(argv[i], "--origins") == 0 && i + 1 < argc) {
            g_n = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--units") == 0 && i + 1 < argc) {
            g_units = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--unit-bytes") == 0 && i + 1 < argc) {
            g_unit_bytes = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--m") == 0 && i + 1 < argc) {
            g_m = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else {
            return -1;
        }
    }
    if (g_n < 1 || g_n > LG_MAX_ORIGINS || g_units == 0 ||
        g_unit_bytes == 0 || g_unit_bytes > SDF_ORIGIN_UNIT_MAX ||
        g_m < g_n || (g_m & (g_m - 1u)) != 0)
        return -1;
    return 0;
}

static uint8_t byte_at(uint64_t off) {
    return (uint8_t)((off * 2654435761ull) >> 24);
}

static void gen_cb(void *ud, const sdf_fetch_req_t *req, const uint8_t *out,
                   uint32_t len, int status) {
    (void)ud;
    uint32_t i = req->origin_id;
    if (status == 0) {
        /* light integrity check: deterministic pattern, one byte per
         * cache line of the unit */
        for (uint32_t k = 0; k < len; k += 64)
            if (out[k] != byte_at(req->obj_offset + k)) {
                fprintf(stderr, "FAIL: data mismatch origin %u off %llu\n", i,
                        (unsigned long long)(req->obj_offset + k));
                exit(1);
            }
        atomic_fetch_add(&g_ok[i], 1);
    } else {
        atomic_fetch_add(&g_failed[i], 1);
    }
}

static void apply_timeline(sdf_origin_t *o, uint64_t submitted) {
    switch (g_profile) {
    case P_SLOW:
        if (submitted == g_units / 4)
            CHECK0(sdf_loopback_set_knob(o, 1, SDF_LB_THROTTLE_NS, 5000000));
        break;
    case P_DIES:
        if (submitted == g_units / 2)
            CHECK0(o->ops->set_enable(o, 2, 0));
        break;
    case P_FLAPPING:
        if ((submitted & 511ull) == 0 && submitted > 0) {
            int up = (submitted & 1023ull) == 0 ? 1 : 0;
            CHECK0(o->ops->set_enable(o, 3, up));
        }
        break;
    case P_STEADY:
        break;
    }
}

static int wait_quiet(sdf_origin_t *o) {
    for (int iter = 0; iter < 120000; iter++) {
        uint64_t inflight = 0;
        for (uint32_t i = 0; i < g_n; i++) {
            sdf_origin_stats_t st;
            if (o->ops->stats(o, i, &st) != 0)
                return -1;
            inflight += st.in_flight;
        }
        if (inflight == 0)
            return 0;
        sdf_sleep_ns(200000);
    }
    return -1;
}

int main(int argc, char **argv) {
    if (parse_args(argc, argv) != 0) {
        usage(argv[0]);
        return 2;
    }
    const char *names[] = {"steady", "slow", "dies", "flapping"};

    /* data files */
    char paths[LG_MAX_ORIGINS][160];
    for (uint32_t i = 0; i < g_n; i++) {
        snprintf(paths[i], sizeof(paths[i]), "/tmp/sdf-loadgen-%d-%u.bin",
                 (int)getpid(), i);
        FILE *f = fopen(paths[i], "wb");
        if (f == NULL)
            return 1;
        for (uint64_t off = 0; off < LG_FILE_BYTES; off += 4096) {
            uint8_t blk[4096];
            for (uint32_t k = 0; k < sizeof(blk); k++)
                blk[k] = byte_at(off + k);
            if (fwrite(blk, 1, sizeof(blk), f) != sizeof(blk))
                return 1;
        }
        fclose(f);
    }

    /* equal-weight profile: b[i] = m/N, remainder to the low indices */
    wm_profile_t p;
    memset(&p, 0, sizeof(p));
    p.version = 1;
    p.m = g_m;
    p.n = g_n;
    for (uint32_t i = 0; i < g_n; i++)
        p.b[i] = g_m / g_n + (i < g_m % g_n ? 1 : 0);
    wm_compiled_t c;
    if (wm_profile_compile(&p, &c) != 0) {
        fprintf(stderr, "loadgen: profile compile failed\n");
        return 1;
    }

    /* origins */
    sdf_origin_cfg_t cfg[LG_MAX_ORIGINS];
    for (uint32_t i = 0; i < g_n; i++) {
        memset(&cfg[i], 0, sizeof(cfg[i]));
        cfg[i].id = i;
        cfg[i].cls = SDF_ORIGIN_LOOPBACK;
        snprintf(cfg[i].dst, sizeof(cfg[i].dst), "%s", paths[i]);
        cfg[i].max_inflight = 64;
    }
    sdf_origin_t o = {.ops = sdf_loopback_ops(), .impl = NULL};
    if (o.ops->open(&o, cfg, g_n) != 0) {
        fprintf(stderr, "loadgen: origin open failed\n");
        return 1;
    }

    uint64_t t0 = sdf_now_ns();

    /* assign + submit; per-origin assignment counted from the same
     * wm_select decisions that drive the fetches */
    uint64_t submitted = 0, eagain = 0;
    while (submitted < g_units) {
        uint32_t batch[256];
        uint32_t take = (uint32_t)((g_units - submitted) > 256 ? 256
                                                              : (g_units - submitted));
        wm_select_batch(&c, submitted, batch, take);
        for (uint32_t k = 0; k < take; k++) {
            uint32_t org = batch[k];
            g_assigned[org]++;

            sdf_fetch_req_t req;
            memset(&req, 0, sizeof(req));
            req.origin_id = org;
            req.job_id = 1;
            req.obj_offset =
                ((uint64_t)(submitted + k) * g_unit_bytes) %
                (LG_FILE_BYTES - g_unit_bytes);
            req.len = g_unit_bytes;

            int rc;
            do {
                rc = o.ops->fetch(&o, &req, gen_cb, NULL);
                if (rc == -EAGAIN) {
                    eagain++;
                    sdf_sleep_ns(100000);
                }
            } while (rc == -EAGAIN);

            if (rc == -ENOLINK)
                g_enolink[org]++;
            else if (rc != 0) {
                fprintf(stderr, "loadgen: fetch rc=%d\n", rc);
                return 1;
            }
            apply_timeline(&o, submitted + k + 1);
        }
        submitted += take;
    }

    if (wait_quiet(&o) != 0) {
        fprintf(stderr, "loadgen: origins never went quiet\n");
        return 1;
    }
    uint64_t wall_ms = (sdf_now_ns() - t0) / 1000000ull;

    /* ---- tallies + gates ------------------------------------------------ */
    int gate_ok = 1;
    uint64_t total_ok = 0, total_fail = 0, total_link = 0, total_assign = 0;
    printf("LOADGEN profile=%s origins=%u units=%llu unit_bytes=%u "
           "wall_ms=%llu eagain_retries=%llu\n",
           names[g_profile], g_n, (unsigned long long)g_units, g_unit_bytes,
           (unsigned long long)wall_ms, (unsigned long long)eagain);
    for (uint32_t i = 0; i < g_n; i++) {
        uint64_t ok = atomic_load(&g_ok[i]);
        uint64_t fail = atomic_load(&g_failed[i]);
        uint64_t link = g_enolink[i];
        uint64_t assign = g_assigned[i];
        sdf_origin_stats_t st;
        o.ops->stats(&o, i, &st);
        printf("ORIGIN %u assigned=%llu ok=%llu failed=%llu enolink=%llu "
               "ewma_lat_ms=%.2f\n",
               i, (unsigned long long)assign, (unsigned long long)ok,
               (unsigned long long)fail, (unsigned long long)link,
               st.ewma_lat_ns / 1000000.0);
        total_ok += ok;
        total_fail += fail;
        total_link += link;
        total_assign += assign;

        if (ok + fail + link != assign) {
            printf("GATE FAIL origin %u: ok+failed+enolink != assigned\n", i);
            gate_ok = 0;
        }
        if (g_profile == P_STEADY &&
            (ok != assign || fail != 0 || link != 0)) {
            printf("GATE FAIL origin %u: steady profile must complete every "
                   "assigned unit exactly\n",
                   i);
            gate_ok = 0;
        }
        if (g_profile == P_DIES && i == 2 && link == 0) {
            printf("GATE FAIL origin 2: dies timeline produced no -ENOLINK\n");
            gate_ok = 0;
        }
        if (g_profile == P_FLAPPING && i == 3 && link == 0) {
            printf("GATE FAIL origin 3: flapping timeline produced no "
                   "-ENOLINK\n");
            gate_ok = 0;
        }
    }
    if (total_ok + total_fail + total_link != g_units ||
        total_assign != g_units) {
        printf("GATE FAIL: total accounting mismatch\n");
        gate_ok = 0;
    }

    o.ops->close(&o);
    for (uint32_t i = 0; i < g_n; i++)
        unlink(paths[i]);

    if (!gate_ok) {
        fprintf(stderr, "loadgen: GATES FAILED (profile %s)\n",
                names[g_profile]);
        return 1;
    }
    printf("GATES OK\n");
    return 0;
}
