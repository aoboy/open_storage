/*
 * FLUID repair coordinator integration tests (plan prompt W3.5/W3.6).
 *
 * Runs the design-02 loop over loopback origins with injected loss:
 * the control arm completes under any ladder loss via retry; the
 * FLUID arms complete under 20% loss (decode sufficiency within the
 * budget's slack, byte-exact against the stream); a high-loss run
 * forces the ARQ fallback and still completes byte-exact — never an
 * incomplete block.
 */

#define _POSIX_C_SOURCE 200809L

#include "sdf/fluid_repair.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

enum { K = 64, T = 32, N_ORIGINS = 3 };
static char g_path[N_ORIGINS][160];

/* Stream files are arm-specific: repair ESIs >= K differ between the
 * LT and Nanorq arms. Feeding a Nanorq stream to the LT decoder used to
 * pass because only fd_decode's return was checked; the W6.3 byte-exact
 * gate exposed it. Encode with the exact arm/eps/blocks under test. */
static void make_stream_files(sdf_repair_arm_t arm, double eps, uint32_t blocks) {
    sdf_fluid_repair_cfg_t enc;
    memset(&enc, 0, sizeof(enc));
    enc.arm = arm;
    enc.k = K;
    enc.sym_len = T;
    enc.eps = eps;
    enc.n_blocks = blocks;
    uint64_t bytes = sdf_fluid_repair_stream_bytes(&enc);
    CHECK(bytes > 0);
    uint8_t *stream = malloc(bytes);
    CHECK(stream != NULL);
    CHECK(sdf_fluid_repair_encode_stream(&enc, stream, bytes) == 0);
    for (int i = 0; i < N_ORIGINS; i++) {
        snprintf(g_path[i], sizeof(g_path[i]), "/tmp/sdf-repair-%d-%d.bin", (int)getpid(), i);
        FILE *f = fopen(g_path[i], "wb");
        CHECK(f != NULL);
        CHECK(fwrite(stream, 1, bytes, f) == bytes);
        CHECK(fclose(f) == 0);
    }
    free(stream);
}

static void base_cfg(sdf_fluid_repair_cfg_t *cfg, sdf_repair_arm_t arm, double eps, double loss) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->arm = arm;
    cfg->k = K;
    cfg->sym_len = T;
    cfg->eps = eps;
    cfg->loss_pct = loss;
    cfg->wave = 16;
    cfg->n_origins = N_ORIGINS;
    for (uint32_t i = 0; i < N_ORIGINS; i++) {
        size_t plen = strlen(g_path[i]) + 1;
        CHECK(plen <= sizeof(cfg->origins[i].dst));
        cfg->origins[i].id = i;
        cfg->origins[i].cls = SDF_ORIGIN_LOOPBACK;
        memcpy(cfg->origins[i].dst, g_path[i], plen);
        cfg->origins[i].max_inflight = 64;
    }
}

static char *render_metrics(const sdf_fluid_repair_stats_t *st) {
    char *body = NULL;
    size_t len = 0;
    FILE *out = open_memstream(&body, &len);
    CHECK(out != NULL);
    CHECK(sdf_fluid_repair_render_metrics(out, st) == 0);
    CHECK(fclose(out) == 0);
    CHECK(body != NULL);

    const char *dir = getenv("SDF_METRICS_FIXTURE_DIR");
    if (dir != NULL && dir[0] != '\0') {
        char path[4096];
        CHECK(snprintf(path, sizeof(path), "%s/fluid.prom", dir) > 0);
        FILE *fixture = fopen(path, "w");
        CHECK(fixture != NULL);
        CHECK(fwrite(body, 1, len, fixture) == len);
        CHECK(fclose(fixture) == 0);
    }
    return body;
}

static uint64_t metric_value(const char *body, const char *series) {
    const char *p = strstr(body, series);
    CHECK(p != NULL);
    return strtoull(p + strlen(series), NULL, 10);
}

static void test_control_arm(void) {
    sdf_fluid_repair_cfg_t cfg;
    make_stream_files(SDF_REPAIR_CONTROL, 0.0, 1); /* fills g_path */
    base_cfg(&cfg, SDF_REPAIR_CONTROL, 0.0, 30.0); /* heavy loss */
    sdf_fluid_repair_stats_t st;
    CHECK(sdf_fluid_repair_run(&cfg, &st) == 0);
    CHECK(st.ok == 1); /* retry semantics complete under any loss */
    CHECK(st.received >= K);
    CHECK(st.arq_fallback == 0); /* control never uses the gate */
    printf("  control: sent=%llu received=%llu wall_ms=%llu\n", (unsigned long long)st.sent,
           (unsigned long long)st.received, (unsigned long long)(st.wall_ns / 1000000ull));
}

static void test_fluid_arm_20pct_loss(void) {
    for (int arm = 1; arm <= 2; arm++) {
        sdf_fluid_repair_cfg_t cfg;
        make_stream_files((sdf_repair_arm_t)arm, 0.10, 1);
        base_cfg(&cfg, (sdf_repair_arm_t)arm, 0.10, 20.0);
        sdf_fluid_repair_stats_t st;
        CHECK(sdf_fluid_repair_run(&cfg, &st) == 0);
        CHECK(st.ok == 1); /* decode sufficiency within the slack */
        CHECK(st.received >= cfg.k);
        printf("  arm=%s: sent=%llu received=%llu rounds=%llu fallback=%d "
               "rank=%u wall_ms=%llu\n",
               arm == 1 ? "lt" : "nanorq", (unsigned long long)st.sent,
               (unsigned long long)st.received, (unsigned long long)st.rounds, st.arq_fallback,
               st.decode_rank, (unsigned long long)(st.wall_ns / 1000000ull));
    }
}

static void test_fluid_forces_arq_fallback(void) {
    /* loss 60% with eps 0.05: the budget's slack is quickly consumed
     * (gate fires early) — the fallback still completes the block */
    sdf_fluid_repair_cfg_t cfg;
    make_stream_files(SDF_REPAIR_LT, 0.05, 1);
    base_cfg(&cfg, SDF_REPAIR_LT, 0.05, 60.0);
    sdf_fluid_repair_stats_t st;
    CHECK(sdf_fluid_repair_run(&cfg, &st) == 0);
    CHECK(st.ok == 1); /* NEVER an incomplete block */
    CHECK(st.arq_fallback == 1);
    printf("  forced fallback: sent=%llu received=%llu fallback=%d ok=%d\n",
           (unsigned long long)st.sent, (unsigned long long)st.received, st.arq_fallback, st.ok);
}

/* W6.3: a 4-block checkpoint object. Each block is its own codec
 * object; the run schedules by decode deficit and verifies every block
 * byte-exact. eps=0.02 with a 64-symbol round keeps the gate open for
 * several rounds at 30% loss, so per-block round accounting is
 * exercised rather than the trivial one-round-then-fallback case. */
static void test_multi_block_checkpoint(void) {
    enum { BLOCKS = 4 };
    sdf_fluid_repair_cfg_t cfg;
    make_stream_files(SDF_REPAIR_NANORQ, 0.02, BLOCKS);
    base_cfg(&cfg, SDF_REPAIR_NANORQ, 0.02, 30.0);
    cfg.n_blocks = BLOCKS;
    cfg.wave = 64;

    sdf_fluid_repair_stats_t st;
    CHECK(sdf_fluid_repair_run(&cfg, &st) == 0);
    CHECK(st.n_blocks == BLOCKS);
    CHECK(st.ok == 1); /* every block decoded byte-exact */

    uint64_t sent = 0, received = 0, rounds = 0;
    for (uint32_t b = 0; b < st.n_blocks; b++) {
        CHECK(st.blocks[b].ok == 1);
        CHECK(st.blocks[b].rounds >= 2); /* the scheduler ran >1 round */
        sent += st.blocks[b].sent;
        received += st.blocks[b].received;
        rounds += st.blocks[b].rounds;
    }
    CHECK(sent == st.sent);
    CHECK(received == st.received);
    CHECK(st.rounds >= 2);
    CHECK(rounds >= st.rounds); /* blocks share the coordinator rounds */

    char *metrics = render_metrics(&st);
    CHECK(metric_value(
              metrics,
              "sdf_fluid_repair_duration_seconds_count{arm=\"nanorq\",class=\"default\"} ") == 1);
    CHECK(metric_value(metrics, "sdf_fluid_repair_duration_seconds_bucket{arm=\"nanorq\",class="
                                "\"default\",le=\"+Inf\"} ") == 1);
    CHECK(metric_value(metrics,
                       "sdf_fluid_repair_round_size_count{arm=\"nanorq\",class=\"default\"} ") >=
          2);
    CHECK(metric_value(metrics,
                       "sdf_fluid_repair_round_size_sum{arm=\"nanorq\",class=\"default\"} ") > 0);
    CHECK(metric_value(
              metrics,
              "sdf_fluid_repair_bytes_total{arm=\"nanorq\",class=\"default\",kind=\"sent\"} ") ==
          st.bytes_tx);
    const char *sum_series =
        "sdf_fluid_repair_duration_seconds_sum{arm=\"nanorq\",class=\"default\"} ";
    const char *sum_at = strstr(metrics, sum_series);
    CHECK(sum_at != NULL);
    double exported_ns = strtod(sum_at + strlen(sum_series), NULL) * 1e9;
    double expected_ns = (double)st.wall_ns;
    double sum_error =
        exported_ns > expected_ns ? exported_ns - expected_ns : expected_ns - exported_ns;
    CHECK(sum_error <= expected_ns * 1e-12 + 1e-6);
    free(metrics);
    CHECK(sdf_fluid_repair_render_metrics(NULL, &st) != 0);
    CHECK(sdf_fluid_repair_render_metrics(stdout, NULL) != 0);
    sdf_fluid_repair_stats_t invalid = st;
    invalid.arm = 99;
    CHECK(sdf_fluid_repair_render_metrics(stdout, &invalid) == -EINVAL);

    printf("  multi-block: blocks=%u sent=%llu received=%llu rounds=%llu "
           "block_rounds=%llu,%llu,%llu,%llu ok=%d\n",
           st.n_blocks, (unsigned long long)st.sent, (unsigned long long)st.received,
           (unsigned long long)st.rounds, (unsigned long long)st.blocks[0].rounds,
           (unsigned long long)st.blocks[1].rounds, (unsigned long long)st.blocks[2].rounds,
           (unsigned long long)st.blocks[3].rounds, st.ok);
}

int main(void) {
    test_control_arm();
    test_fluid_arm_20pct_loss();
    test_fluid_forces_arq_fallback();
    test_multi_block_checkpoint();
    for (int i = 0; i < N_ORIGINS; i++) {
        CHECK(unlink(g_path[i]) == 0);
    }
    printf("test_fluid_repair: ok (control retry, both FLUID arms at 20%% "
           "loss, forced ARQ fallback, 4-block checkpoint byte-exact)\n");
    return 0;
}
