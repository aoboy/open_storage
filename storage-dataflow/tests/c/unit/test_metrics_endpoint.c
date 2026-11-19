/*
 * Live scrape endpoint tests (plan prompt W6.1; the wam
 * test_metrics_endpoint.c contract, adapted): bad addrs refuse, GET
 * answers 200 + Content-Type + exact Content-Length over the render
 * callback's bytes, the accept loop keeps serving (two scrapes = two
 * renders), non-GET answers 405, a failing render answers 500, and
 * stop() actually closes the listener (connect gets ECONNREFUSED).
 */

#define _POSIX_C_SOURCE 200809L

#include "sdf/common/metrics_endpoint.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define MEP_TEST_PORT (22100 + (getpid() % 500))

static int renders;

static int demo_render(void *ud, FILE *out) {
    int *called = ud;
    (*called)++;
    fprintf(out, "sdf_demo_metric %d\n", 40 + 2);
    return 0;
}

static int fail_render(void *ud, FILE *out) {
    (void)ud;
    (void)out;
    return -1;
}

/* connect, send req, read the full response until server close */
static size_t scrape(int port, const char *req, char *buf, size_t cap) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    CHECK(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) == 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return 0; /* listener gone — caller asserts on len */
    }
    CHECK(send(fd, req, strlen(req), 0) == (ssize_t)strlen(req));
    size_t got = 0;
    while (got + 1 < cap) {
        ssize_t n = recv(fd, buf + got, cap - 1 - got, 0);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    buf[got] = '\0';
    close(fd);
    return got;
}

static int connect_only(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    CHECK(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) == 1);
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    close(fd);
    return rc == 0 ? 1 : 0;
}

int main(void) {
    /* bad addrs refuse without binding anything */
    CHECK(sdf_metrics_endpoint_start(NULL, demo_render, &renders) == NULL);
    CHECK(sdf_metrics_endpoint_start("", demo_render, &renders) == NULL);
    CHECK(sdf_metrics_endpoint_start("noport", demo_render, &renders) == NULL);
    CHECK(sdf_metrics_endpoint_start("127.0.0.1:0", demo_render, &renders) == NULL);
    CHECK(sdf_metrics_endpoint_start("127.0.0.1:70000", demo_render, &renders) == NULL);
    CHECK(sdf_metrics_endpoint_start("127.0.0.1:abc", demo_render, &renders) == NULL);
    CHECK(sdf_metrics_endpoint_start("127.0.0.1:9108", NULL, NULL) == NULL);

    char addr[32];
    snprintf(addr, sizeof(addr), "127.0.0.1:%d", MEP_TEST_PORT);
    sdf_metrics_endpoint_t *ep = sdf_metrics_endpoint_start(addr, demo_render, &renders);
    CHECK(ep != NULL);
    CHECK(connect_only(MEP_TEST_PORT) == 1);

    static char buf[8192];
    const char *get = "GET /metrics HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    size_t n = scrape(MEP_TEST_PORT, get, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(strstr(buf, "HTTP/1.1 200 OK\r\n"));
    CHECK(strstr(buf, "Content-Type: text/plain; version=0.0.4"));
    CHECK(strstr(buf, "Connection: close\r\n"));
    CHECK(strstr(buf, "sdf_demo_metric 42\n"));
    /* Content-Length must match the rendered body exactly */
    const char *cl = strstr(buf, "Content-Length: ");
    CHECK(cl != NULL);
    long want = strtol(cl + 16, NULL, 10);
    const char *head_end = strstr(buf, "\r\n\r\n");
    CHECK(head_end != NULL);
    long body_len = (long)n - (long)((head_end + 4) - buf);
    CHECK(want == body_len);
    CHECK(want == (long)strlen("sdf_demo_metric 42\n"));
    CHECK(renders == 1);

    /* the accept loop keeps serving — a second scrape re-renders */
    n = scrape(MEP_TEST_PORT, get, buf, sizeof(buf));
    CHECK(n > 0 && strstr(buf, "HTTP/1.1 200 OK\r\n"));
    CHECK(renders == 2);

    /* non-GET is refused, not served */
    n = scrape(MEP_TEST_PORT, "POST / HTTP/1.1\r\n\r\n", buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(strstr(buf, "HTTP/1.1 405 Method Not Allowed\r\n"));

    /* a failing render answers 500 (same listener, second handle) */
    sdf_metrics_endpoint_stop(ep);
    CHECK(connect_only(MEP_TEST_PORT) == 0);
    sdf_metrics_endpoint_t *ep2 = sdf_metrics_endpoint_start(addr, fail_render, NULL);
    CHECK(ep2 != NULL);
    n = scrape(MEP_TEST_PORT, get, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(strstr(buf, "HTTP/1.1 500 Internal Server Error\r\n"));
    sdf_metrics_endpoint_stop(ep2);
    CHECK(connect_only(MEP_TEST_PORT) == 0);

    sdf_metrics_endpoint_stop(NULL); /* NULL-safe */
    printf("test_metrics_endpoint: ok\n");
    return 0;
}
