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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
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
        CHECK(arc_sidecar_submit(entry, sizeof(entry), sdf_now_ns() - 30000000ull) == 0);
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
        CHECK(arc_sidecar_submit(entry, sizeof(entry), sdf_now_ns() - d) == 0);
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
        CHECK(arc_sidecar_submit(entry, sizeof(entry), sdf_now_ns() - 40000000ull) == 0);
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

/* W6.4: mode flips land within one pump interval. A future producer
 * stamp parks an entry under QG; the kill request must drain it on the
 * very next pump (design 03's kill switch, always armed). */
static void test_mode_flip(void) {
    arc_cfg_t cfg;
    base_cfg(&cfg);
    CHECK(arc_sidecar_start(&cfg, 1) == 0);
    CHECK(arc_sidecar_set_mode(7) == -EINVAL);

    char entry[64];
    memset(entry, 3, sizeof(entry));
    CHECK(arc_sidecar_submit(entry, sizeof(entry), sdf_now_ns() + 40000000ull) == 0);
    CHECK(arc_sidecar_pump_once() >= 1); /* submitted, held by QG */
    CHECK(arc_sidecar_mode() == 1);
    CHECK(arc_sidecar_applied() == 0);
    CHECK(arc_sidecar_stats().in_flight == 1);

    CHECK(arc_sidecar_set_mode(2) == 0);
    CHECK(arc_sidecar_mode() == 2);
    CHECK(arc_sidecar_pump_once() >= 1); /* one interval: flip + drain */
    CHECK(arc_sidecar_applied() == 1);
    CHECK(arc_sidecar_stats().in_flight == 0);
    CHECK(arc_sidecar_stats().releases == 1);

    /* shadow is the immediate path again; the flip is reversible */
    CHECK(arc_sidecar_set_mode(0) == 0);
    CHECK(arc_sidecar_pump_once() >= 0);
    CHECK(arc_sidecar_mode() == 0);
    CHECK(arc_sidecar_submit(entry, sizeof(entry), sdf_now_ns()) == 0);
    CHECK(arc_sidecar_pump_once() >= 1);
    CHECK(arc_sidecar_applied() == 2);
    arc_sidecar_stop();
    CHECK(arc_sidecar_set_mode(1) == -EINVAL); /* no sidecar to flip */
}

static void test_scrape(void);

int main(void) {
    test_modes();
    test_mode_flip();
    test_scrape();
    test_validation();
    printf("test_arc_sidecar: ok (killed oracle, live ordered burst, "
           "shadow immediate, W6.4 one-pump mode flips, validation)\n");
    return 0;
}

/* ---- W6.1: the live scrape over the sidecar --------------------------- */

static void test_scrape(void) {
    arc_cfg_t cfg;
    base_cfg(&cfg);
    CHECK(arc_sidecar_start(&cfg, 1) == 0);

    /* serve requires the running sidecar; a bad port fails */
    CHECK(arc_sidecar_serve(NULL) == -EINVAL);
    int port = 22600 + (int)(getpid() % 400);
    char addr[32];
    snprintf(addr, sizeof(addr), "127.0.0.1:%d", port);
    CHECK(arc_sidecar_serve(addr) == 0);
    CHECK(arc_sidecar_serve(addr) == -EEXIST); /* one endpoint */

    /* drive one release so the scrape carries live numbers */
    char entry[64];
    memset(entry, 7, sizeof(entry));
    CHECK(arc_sidecar_submit(entry, sizeof(entry), sdf_now_ns() - 30000000ull) == 0);
    for (int i = 0; i < 8; i++) {
        arc_sidecar_pump_once();
        sdf_sleep_ns(10000000ull);
    }

    /* scrape: GET answers the sidecar's own series */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    CHECK(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) == 1);
    CHECK(connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    const char *get = "GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK(send(fd, get, strlen(get), 0) == (ssize_t)strlen(get));
    static char buf[4096];
    size_t got = 0;
    while (got + 1 < sizeof(buf)) {
        ssize_t n = recv(fd, buf + got, sizeof(buf) - 1 - got, 0);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    buf[got] = '\0';
    close(fd);
    CHECK(strstr(buf, "HTTP/1.1 200 OK\r\n") != NULL);
    CHECK(strstr(buf, "sdf_arc_releases_total") != NULL);
    CHECK(strstr(buf, "sdf_arc_mode 1") != NULL);
    CHECK(strstr(buf, "sdf_arc_added_latency_p99_ns") != NULL);

    arc_sidecar_stop();                        /* stops the endpoint with the sidecar */
    CHECK(arc_sidecar_serve(addr) == -EINVAL); /* stopped: refuse */
}
