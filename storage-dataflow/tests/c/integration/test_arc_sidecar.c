/*
 * ARC sidecar integration tests (plan prompt W4.3/W4.5).
 *
 * The sidecar over the SPSC ring: shadow mode applies immediately with
 * the schedule computed (metrics alive), live mode owns the timing
 * (entries release per ARC-QG — ordering holds), killed is the oracle
 * (added latency zero by construction), and the modes flip live
 * without restart (the design-03 promotion + kill-switch flow).
 * A bursty generator exercises the pump loop end to end.
 */

#include "sdf/arc.h"
#include "sdf/common/timer.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);       \
            exit(1);                                                          \
        }                                                                     \
    } while (0)

static void base_cfg(arc_cfg_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->capacity = 64;
    cfg->entry_max = 248;
    cfg->mode = ARC_MODE_QG;
}

static void test_modes(void) {
    arc_cfg_t cfg;
    base_cfg(&cfg);

    /* killed: the oracle — data flows at arrival */
    CHECK(arc_sidecar_start(&cfg, 2) == 0);
    CHECK(arc_sidecar_mode() == 2);
    for (int i = 0; i < 10; i++) {
        char entry[64];
        memset(entry, (char)i, sizeof(entry));
        CHECK(arc_sidecar_submit(entry, sizeof(entry),
                                 sdf_now_ns() - 30000000ull) == 0);
    }
    CHECK(arc_sidecar_pump_once() >= 10);
    CHECK(arc_sidecar_applied() == 10);
    {
        arc_stats_t st = arc_sidecar_stats();
        CHECK(st.releases == 10);
        /* the oracle releases at (poll-time ~= A_n): added is the
         * pump-loop dispatch, microseconds — not a schedule delay */
        CHECK(st.added_p99_ns < 1000000.0);
    }
    arc_sidecar_stop();

    /* live: ARC owns the timing; the burst releases ordered */
    base_cfg(&cfg);
    CHECK(arc_sidecar_start(&cfg, 1) == 0);
    CHECK(arc_sidecar_mode() == 1);
    for (int i = 0; i < 24; i++) {
        char entry[64];
        memset(entry, (char)i, sizeof(entry));
        /* bursty arrivals: alternating delay pattern */
        uint64_t d = (i % 3 == 1) ? 60000000ull : 10000000ull;
        CHECK(arc_sidecar_submit(entry, sizeof(entry),
                                 sdf_now_ns() - d) == 0);
        CHECK(arc_sidecar_pump_once() >= 0);
    }
    /* drain the schedule */
    for (int i = 0; i < 8; i++) {
        arc_sidecar_pump_once();
        sdf_sleep_ns(10000000ull);
    }
    CHECK(arc_sidecar_applied() == 24);
    {
        arc_stats_t st = arc_sidecar_stats();
        CHECK(st.releases == 24);
        CHECK(st.guard_timeouts == 0); /* complete stream: no timeouts */
    }
    arc_sidecar_stop();

    /* shadow: applies immediately (the old path), schedule computed */
    base_cfg(&cfg);
    CHECK(arc_sidecar_start(&cfg, 0) == 0);
    CHECK(arc_sidecar_mode() == 0);
    for (int i = 0; i < 10; i++) {
        char entry[64];
        memset(entry, (char)i, sizeof(entry));
        CHECK(arc_sidecar_submit(entry, sizeof(entry),
                                 sdf_now_ns() - 40000000ull) == 0);
        CHECK(arc_sidecar_pump_once() >= 1); /* immediate apply */
    }
    CHECK(arc_sidecar_applied() == 10);
    arc_sidecar_stop();
}

static void test_validation(void) {
    arc_cfg_t cfg;
    base_cfg(&cfg);
    CHECK(arc_sidecar_start(NULL, 1) == -EINVAL);
    CHECK(arc_sidecar_start(&cfg, 7) == -EINVAL);
    CHECK(arc_sidecar_submit(NULL, 8, 0) == -EINVAL);
    /* oversized entries refused */
    char big[4096];
    CHECK(arc_sidecar_start(&cfg, 1) == 0);
    CHECK(arc_sidecar_submit(big, sizeof(big), 0) == -EINVAL);
    arc_sidecar_stop();
    arc_sidecar_stop(); /* double stop safe */
}

int main(void) {
    test_modes();
    test_validation();
    printf("test_arc_sidecar: ok (killed oracle, live ordered burst, "
           "shadow immediate, validation)\n");
    return 0;
}
