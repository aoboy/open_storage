/*
 * Cache-fill proxy integration tests (plan prompt W2.5).
 *
 * In-process runs over loopback origins through the ADR-0008 seam:
 * steady dispatch inside the paper's discrepancy bound with every
 * unit completing; the control arm is exact round-robin; the kill
 * switch forces control; mid-run degradation redistributes within
 * one control interval (the W2.5 acceptance) with the controller's
 * whack visible in the end profile; A/B split lands in both arms.
 */

#define _POSIX_C_SOURCE 200809L

#include "sdf/origin.h"
#include "sdf/wm.h"
#include "sdf/wm_proxy.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);       \
            exit(1);                                                          \
        }                                                                     \
    } while (0)

#define N_ORIGINS 4
#define OBJ_BYTES SDF_WM_PROXY_OBJ_BYTES

static char g_path[N_ORIGINS][160];
static uint8_t g_obj[OBJ_BYTES];

static void make_files(void) {
    for (size_t i = 0; i < sizeof(g_obj); i++)
        g_obj[i] = (uint8_t)(i * 13u + 7u);
    for (int k = 0; k < N_ORIGINS; k++) {
        snprintf(g_path[k], sizeof(g_path[k]), "/tmp/sdf-proxy-test-%d-%d.bin",
                 (int)getpid(), k);
        FILE *f = fopen(g_path[k], "wb");
        CHECK(f != NULL);
        CHECK(fwrite(g_obj, 1, sizeof(g_obj), f) == sizeof(g_obj));
        CHECK(fclose(f) == 0);
    }
}

static void rm_files(void) {
    for (int k = 0; k < N_ORIGINS; k++)
        CHECK(unlink(g_path[k]) == 0);
}

static void base_cfg(sdf_wm_proxy_cfg_t *pc, uint64_t units) {
    memset(pc, 0, sizeof(*pc));
    pc->units = units;
    pc->unit_bytes = 4096;
    pc->jobs = 16;
    pc->control_hz = 8;
    pc->alpha = 0.25;
    pc->beta = 0.02;
    pc->profile.version = 1;
    pc->profile.m = 1024;
    pc->profile.n = N_ORIGINS;
    for (uint32_t i = 0; i < N_ORIGINS; i++)
        pc->profile.b[i] = 256;
    pc->spray.method = WM_METHOD_SHUFFLE1;
    pc->spray.sa = 333;
    pc->spray.sb = 735;
    pc->n_origins = N_ORIGINS;
    for (uint32_t i = 0; i < N_ORIGINS; i++) {
        size_t plen = strlen(g_path[i]) + 1;
        CHECK(plen <= sizeof(pc->origins[i].dst));
        pc->origins[i].id = i;
        pc->origins[i].cls = SDF_ORIGIN_LOOPBACK;
        memcpy(pc->origins[i].dst, g_path[i], plen);
        pc->origins[i].max_inflight = 64;
    }
}

static void test_steady_discrepancy_bound(void) {
    sdf_wm_proxy_cfg_t pc;
    sdf_wm_proxy_stats_t st;
    base_cfg(&pc, 8192);
    pc.rollout_pct = 100;
    CHECK(sdf_wm_proxy_run(&pc, &st) == 0);

    uint64_t total_ok = 0;
    for (uint32_t i = 0; i < N_ORIGINS; i++) {
        double expect = 256.0 / 1024.0 * 8192.0;
        double dev = (double)st.dispatched[i] > expect
                         ? (double)st.dispatched[i] - expect
                         : expect - (double)st.dispatched[i];
        CHECK(dev <= 10.0 + 0.02 * expect); /* the discrepancy bound */
        CHECK(st.failed[i] == 0);
        CHECK(sdf_counter_read(&st.enolink[i]) == 0);
        total_ok += st.ok[i];
    }
    CHECK(total_ok == 8192);
    CHECK(st.bytes_rx == 8192ull * 4096);
    CHECK(st.dispatch_treatment == 8192 && st.dispatch_control == 0);
    CHECK(st.updates_applied == 0); /* healthy: no whacks */
}

static void test_control_arm_and_kill_switch(void) {
    sdf_wm_proxy_cfg_t pc;
    sdf_wm_proxy_stats_t st;

    /* rollout 0: pure control, exact round-robin */
    base_cfg(&pc, 8192);
    pc.rollout_pct = 0;
    CHECK(sdf_wm_proxy_run(&pc, &st) == 0);
    for (uint32_t i = 0; i < N_ORIGINS; i++)
        CHECK(st.dispatched[i] == 8192 / N_ORIGINS);
    CHECK(st.dispatch_control == 8192 && st.dispatch_treatment == 0);

    /* kill switch forces control even at 100% rollout */
    base_cfg(&pc, 8192);
    pc.rollout_pct = 100;
    pc.kill_switch = 1;
    CHECK(sdf_wm_proxy_run(&pc, &st) == 0);
    CHECK(st.dispatch_control == 8192 && st.dispatch_treatment == 0);
    for (uint32_t i = 0; i < N_ORIGINS; i++)
        CHECK(st.dispatched[i] == 8192 / N_ORIGINS);
}

static void test_ab_split(void) {
    sdf_wm_proxy_cfg_t pc;
    sdf_wm_proxy_stats_t st;
    base_cfg(&pc, 8192);
    pc.rollout_pct = 50;
    CHECK(sdf_wm_proxy_run(&pc, &st) == 0);
    /* deterministic xxhash64 buckets: both arms carry traffic */
    CHECK(st.dispatch_treatment > 0);
    CHECK(st.dispatch_control > 0);
    CHECK(st.dispatch_treatment + st.dispatch_control == 8192);
}

static void test_midrun_degradation_redistributes(void) {
    sdf_wm_proxy_cfg_t pc;
    sdf_wm_proxy_stats_t st;
    base_cfg(&pc, 4096);
    pc.rollout_pct = 100;
    /* origin 2 dies at half-run: the controller must whack it and the
     * healthy paths must absorb — within one control interval */
    pc.steps[0].at_unit = 2048;
    pc.steps[0].origin = 2;
    pc.steps[0].enable = 0;
    pc.n_steps = 1;
    CHECK(sdf_wm_proxy_run(&pc, &st) == 0);

    CHECK(st.updates_applied >= 1); /* the whack landed */
    CHECK(sdf_counter_read(&st.enolink[2]) > 0); /* post-drain picks refused */
    CHECK(st.end_profile_b[2] < 256); /* the whacked path shed units */
    uint64_t absorb = 0;
    for (uint32_t i = 0; i < N_ORIGINS; i++) {
        if (i == 2)
            continue;
        absorb += st.end_profile_b[i];
        CHECK(st.end_profile_b[i] > 256); /* and the healthy grew */
    }
    CHECK(absorb + (uint64_t)st.end_profile_b[2] == 1024);
    /* every unit is accounted: completed or refused-with-enolink */
    uint64_t total = 0;
    for (uint32_t i = 0; i < N_ORIGINS; i++)
        total += st.ok[i] + sdf_counter_read(&st.enolink[i]);
    CHECK(total == 4096);
}

int main(void) {
    make_files();
    test_steady_discrepancy_bound();
    test_control_arm_and_kill_switch();
    test_ab_split();
    test_midrun_degradation_redistributes();
    rm_files();
    printf("test_wm_proxy: ok (discrepancy bound, control arm, kill "
           "switch, A/B split, mid-run redistribution)\n");
    return 0;
}
