/*
 * ARC pacer micro-bench (plan W6.12).
 *
 * Measures the per-block cost of the public ARC path in two named
 * workloads: the IMMEDIATE oracle (submit then release at arrival) and
 * the production QG path (a full batch queued, then released under the
 * ordered schedule). The batch size is also the queue capacity, and the
 * line names mode, batch, capacity, entry size, block count, revision,
 * runner, build, and collection time so records cannot silently compare
 * different configurations.
 *
 * RECORDED ONLY: scripts/run_bench.sh records the lines; the enforced
 * 400 ns/block gate activates only for a named reference runner via
 * scripts/bench_arc_gate.py. No hosted or cross-machine claim is made.
 */

#include "bench_common.h"
#include "sdf/arc.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

#ifndef SDF_BENCH_BUILD_TYPE
#define SDF_BENCH_BUILD_TYPE "unknown"
#endif

#ifndef SDF_BENCH_REVISION
#define SDF_BENCH_REVISION "unknown"
#endif

#if defined(__clang__)
#define BENCH_CC "clang"
#elif defined(__GNUC__)
#define BENCH_CC "gcc"
#else
#define BENCH_CC "cc"
#endif

#define ARC_BENCH_DEFAULT_BATCH 64u
#define ARC_BENCH_DEFAULT_BLOCKS 65536u
#define ARC_BENCH_DEFAULT_ENTRY 1024u
#define ARC_BENCH_MAX_BATCH 4096u
#define ARC_BENCH_MAX_ENTRY (1u << 20)
#define ARC_BENCH_BATCH_DEADLINE_NS 1000000000ull
#define ARC_BENCH_GAMMA_NS 16666667ull

typedef struct {
    uint64_t applies;
    uint64_t checksum;
} bench_apply_ctx_t;

static int bench_apply(void *ctx, const void *entry, size_t len, uint64_t release_ts) {
    bench_apply_ctx_t *a = ctx;
    const uint8_t *bytes = entry;
    a->applies++;
    if (bytes != NULL && len > 0) {
        a->checksum += (uint64_t)bytes[0] + (uint64_t)bytes[len - 1];
    }
    a->checksum += (uint64_t)len + (release_ts & 0xffu);
    return 0;
}

static const char *mode_name(arc_mode_t mode) {
    switch (mode) {
    case ARC_MODE_IMMEDIATE:
        return "IMMEDIATE";
    case ARC_MODE_QG:
        return "QG";
    case ARC_MODE_Q:
        return "Q";
    case ARC_MODE_BASE:
        return "BASE";
    default:
        return "UNKNOWN";
    }
}

static int submit_batch(arc_pacer_t *p, const uint8_t *entry, uint32_t entry_bytes,
                        uint32_t batch) {
    for (uint32_t i = 0; i < batch; i++) {
        int rc = arc_submit(p, entry, entry_bytes, sdf_bench_now_ns());
        if (rc != 0 && rc != 1) {
            fprintf(stderr, "bench_arc: arc_submit returned %d\n", rc);
            return rc;
        }
    }
    return 0;
}

static int run_workload(const char *workload, arc_mode_t mode, uint32_t batch, uint32_t entry_bytes,
                        uint32_t blocks, const char *runner) {
    arc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.capacity = batch;
    cfg.entry_max = entry_bytes;
    cfg.mode = mode;
    cfg.auto_params = 0;
    cfg.min_smooth_bytes = 0; /* every entry takes the arrival path */
    cfg.J = 0;
    cfg.gamma = ARC_BENCH_GAMMA_NS;
    cfg.batch_max = batch;
    cfg.batch_deadline_ns = ARC_BENCH_BATCH_DEADLINE_NS;

    /* QG holds the batch behind the paper's quantized offset. Poll with
     * a simulated future timestamp so the microbench measures scheduling
     * and release cost without sleeping through the 16.7 ms gamma. */
    uint64_t poll_ahead_ns = mode == ARC_MODE_QG ? 2u * ARC_BENCH_GAMMA_NS : 0;

    size_t pacer_size = arc_sizeof(&cfg);
    if (pacer_size == 0) {
        fprintf(stderr, "bench_arc: arc_sizeof rejected the configuration\n");
        return -EINVAL;
    }
    arc_pacer_t *p = malloc(pacer_size);
    uint8_t *entry = malloc(entry_bytes);
    if (p == NULL || entry == NULL) {
        fprintf(stderr, "bench_arc: out of memory\n");
        free(p);
        free(entry);
        return -ENOMEM;
    }
    for (uint32_t i = 0; i < entry_bytes; i++) {
        entry[i] = (uint8_t)(i * 13u + 7u);
    }

    bench_apply_ctx_t apply_ctx = {0};
    if (arc_init(p, &cfg, bench_apply, &apply_ctx) != 0) {
        fprintf(stderr, "bench_arc: arc_init failed\n");
        free(p);
        free(entry);
        return -EINVAL;
    }

    if (submit_batch(p, entry, entry_bytes, batch) != 0 ||
        arc_poll(p, sdf_bench_now_ns() + poll_ahead_ns) != (int)batch) {
        fprintf(stderr, "bench_arc: warmup batch did not drain\n");
        free(p);
        free(entry);
        return -EIO;
    }

    uint64_t rounds = blocks / batch;
    uint64_t actual_blocks = rounds * batch;
    uint64_t submit_ns = 0;
    uint64_t poll_ns = 0;
    uint64_t released_total = 0;

    for (uint64_t r = 0; r < rounds; r++) {
        uint64_t t0 = sdf_bench_now_ns();
        if (submit_batch(p, entry, entry_bytes, batch) != 0) {
            free(p);
            free(entry);
            return -EIO;
        }
        uint64_t t1 = sdf_bench_now_ns();

        uint64_t before = apply_ctx.applies;
        int released = arc_poll(p, sdf_bench_now_ns() + poll_ahead_ns);
        uint64_t t2 = sdf_bench_now_ns();
        if (released != (int)batch || apply_ctx.applies - before != batch) {
            fprintf(stderr, "bench_arc: poll released %d of %u blocks\n", released, batch);
            free(p);
            free(entry);
            return -EIO;
        }
        submit_ns += t1 - t0;
        poll_ns += t2 - t1;
        released_total += (uint64_t)released;
    }

    double per_block = (double)(submit_ns + poll_ns) / (double)actual_blocks;
    printf("BENCH arc driver=arc-pacer revision=%s runner=%s build=%s cc=%s "
           "workload=%s mode=%s batch=%u capacity=%u entry_bytes=%u blocks=%" PRIu64
           " submit_ns_per_block=%.2f poll_ns_per_block=%.2f ns_per_block=%.2f "
           "releases=%" PRIu64 " recorded_s=%lld sink=%" PRIu64 "\n",
           SDF_BENCH_REVISION, runner, SDF_BENCH_BUILD_TYPE, BENCH_CC, workload, mode_name(mode),
           batch, batch, entry_bytes, actual_blocks, (double)submit_ns / (double)actual_blocks,
           (double)poll_ns / (double)actual_blocks, per_block, released_total,
           (long long)time(NULL), (uint64_t)(sdf_bench_sink + apply_ctx.checksum));

    free(p);
    free(entry);
    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--batch N] [--blocks N] [--entry-bytes N] [--runner NAME]\n"
            "  --batch        batch size and queue capacity, power of two (default %u)\n"
            "  --blocks       measured blocks per workload (default %u)\n"
            "  --entry-bytes  copied entry bytes per block (default %u, max %u)\n",
            argv0, ARC_BENCH_DEFAULT_BATCH, ARC_BENCH_DEFAULT_BLOCKS, ARC_BENCH_DEFAULT_ENTRY,
            ARC_BENCH_MAX_ENTRY);
}

static int parse_u32(const char *s, uint32_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

int main(int argc, char **argv) {
    uint32_t batch = ARC_BENCH_DEFAULT_BATCH;
    uint32_t blocks = ARC_BENCH_DEFAULT_BLOCKS;
    uint32_t entry_bytes = ARC_BENCH_DEFAULT_ENTRY;
    char runner[256];

    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(runner, sizeof(runner), "%s-%s-%s", u.sysname, u.release, u.machine);
    } else {
        snprintf(runner, sizeof(runner), "unknown");
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (i + 1 >= argc) {
            usage(argv[0]);
            return 1;
        }
        const char *val = argv[++i];
        if (strcmp(argv[i - 1], "--batch") == 0) {
            if (parse_u32(val, &batch) != 0) {
                usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i - 1], "--blocks") == 0) {
            if (parse_u32(val, &blocks) != 0) {
                usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i - 1], "--entry-bytes") == 0) {
            if (parse_u32(val, &entry_bytes) != 0) {
                usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i - 1], "--runner") == 0) {
            if (strlen(val) >= sizeof(runner)) {
                fprintf(stderr, "bench_arc: --runner too long\n");
                return 1;
            }
            memcpy(runner, val, strlen(val) + 1);
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (batch < 2 || batch > ARC_BENCH_MAX_BATCH || (batch & (batch - 1)) != 0) {
        fprintf(stderr, "bench_arc: --batch must be a power of two in [2, %u]\n",
                ARC_BENCH_MAX_BATCH);
        return 1;
    }
    if (entry_bytes == 0 || entry_bytes > ARC_BENCH_MAX_ENTRY) {
        fprintf(stderr, "bench_arc: --entry-bytes must be in [1, %u]\n", ARC_BENCH_MAX_ENTRY);
        return 1;
    }
    if (blocks < batch) {
        fprintf(stderr, "bench_arc: --blocks must be at least one batch\n");
        return 1;
    }

    if (run_workload("immediate", ARC_MODE_IMMEDIATE, batch, entry_bytes, blocks, runner) != 0) {
        return 1;
    }
    if (run_workload("qg", ARC_MODE_QG, batch, entry_bytes, blocks, runner) != 0) {
        return 1;
    }
    printf("# ARC target is 200 ns/block; the gate limit is 400 ns/block on a named "
           "reference runner; recorded only\n");
    return 0;
}
