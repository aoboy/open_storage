/*
 * Live scrape hook (plan prompt W6.1).
 *
 * Adapted from wack_a_mole src/runtime/metrics_endpoint.c @ a0f0422
 * (K.1b): deliberately minimal — HTTP/1.1 GET -> Content-Length-framed
 * 200 with the render callback's bytes, close; no keep-alive, no
 * routing, no auth (ops surface — loopback by default). Same socket
 * discipline: MSG_NOSIGNAL sends, a 100 ms accept-poll bounding
 * stop()'s join, a 2 s per-request read timeout, and an exact
 * Content-Length so scrapers never guess.
 */

#define _POSIX_C_SOURCE 200809L

#include "sdf/common/metrics_endpoint.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MEP_POLL_MS 100    /* accept loop tick — bounds stop() join */
#define MEP_REQ_MAX 4096   /* request head cap (bytes) */
#define MEP_CLIENT_MS 2000 /* per-request read timeout */
#define MEP_BACKLOG 8

struct sdf_metrics_endpoint {
    int fd;
    pthread_t th;
    atomic_int stop;
    sdf_metrics_render_fn render;
    void *ud;
};

static void mep_send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0)
            return; /* client went away — nothing to salvage */
        p += n;
        len -= (size_t)n;
    }
}

static void mep_send_simple(int fd, const char *status) {
    char hdr[160];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %s\r\nContent-Type: text/plain; "
                     "version=0.0.4; charset=utf-8\r\n"
                     "Content-Length: 0\r\nConnection: close\r\n\r\n",
                     status);
    if (n > 0)
        mep_send_all(fd, hdr, (size_t)n);
}

/* one client: read the request head (GET serves, anything else 405),
 * render into a memory stream, reply with an exact Content-Length */
static void mep_handle(sdf_metrics_endpoint_t *ep, int c) {
    char req[MEP_REQ_MAX + 1];
    size_t got = 0;
    int complete = 0;
    while (got < MEP_REQ_MAX) {
        struct pollfd p = {c, POLLIN, 0};
        if (poll(&p, 1, MEP_CLIENT_MS) <= 0)
            break;
        ssize_t n = recv(c, req + got, MEP_REQ_MAX - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        req[got] = '\0';
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) {
            complete = 1;
            break;
        }
    }
    if (got == 0)
        return; /* nothing sent — just drop */
    if (!complete) {
        if (got >= MEP_REQ_MAX)
            mep_send_simple(c, "400 Bad Request");
        return; /* timeout / half request */
    }

    char method[8] = {0};
    if (sscanf(req, "%7s", method) != 1 || strcmp(method, "GET") != 0) {
        mep_send_simple(c, "405 Method Not Allowed");
        return;
    }

    char *buf = NULL;
    size_t len = 0;
    FILE *body = open_memstream(&buf, &len);
    if (!body)
        return;
    int rc = ep->render(ep->ud, body);
    fclose(body);
    if (rc != 0 || !buf) {
        free(buf);
        mep_send_simple(c, "500 Internal Server Error");
        return;
    }

    char hdr[192];
    int hn = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/plain; version=0.0.4; "
                      "charset=utf-8\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                      len);
    if (hn > 0)
        mep_send_all(c, hdr, (size_t)hn);
    mep_send_all(c, buf, len);
    free(buf);
}

static void *mep_thread(void *arg) {
    sdf_metrics_endpoint_t *ep = arg;
    while (!atomic_load(&ep->stop)) {
        struct pollfd p = {ep->fd, POLLIN, 0};
        int pr = poll(&p, 1, MEP_POLL_MS);
        if (pr <= 0)
            continue; /* timeout/EINTR — re-check stop */
        int c = accept(ep->fd, NULL, NULL);
        if (c < 0)
            continue;
        mep_handle(ep, c);
        close(c);
    }
    return NULL;
}

sdf_metrics_endpoint_t *sdf_metrics_endpoint_start(const char *addr,
                                                    sdf_metrics_render_fn render,
                                                    void *ud) {
    if (!addr || !*addr || !render) {
        fprintf(stderr, "sdf_metrics_endpoint: addr and render required\n");
        return NULL;
    }
    const char *colon = strrchr(addr, ':');
    if (!colon || colon == addr || !colon[1]) {
        fprintf(stderr, "sdf_metrics_endpoint: %s: want host:port\n", addr);
        return NULL;
    }
    char host[64];
    size_t hlen = (size_t)(colon - addr);
    if (hlen >= sizeof(host)) {
        fprintf(stderr, "sdf_metrics_endpoint: %s: host too long\n", addr);
        return NULL;
    }
    memcpy(host, addr, hlen);
    host[hlen] = '\0';
    char *end = NULL;
    long port = strtol(colon + 1, &end, 10);
    if (*end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "sdf_metrics_endpoint: %s: bad port\n", addr);
        return NULL;
    }
    if (host[0] == '\0')
        strcpy(host, "127.0.0.1");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("sdf_metrics_endpoint: socket");
        return NULL;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        fprintf(stderr, "sdf_metrics_endpoint: %s: bad host\n", addr);
        close(fd);
        return NULL;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, MEP_BACKLOG) < 0) {
        perror("sdf_metrics_endpoint: bind/listen");
        close(fd);
        return NULL;
    }

    sdf_metrics_endpoint_t *ep = calloc(1, sizeof(*ep));
    if (!ep) {
        close(fd);
        return NULL;
    }
    ep->fd = fd;
    ep->render = render;
    ep->ud = ud;
    atomic_init(&ep->stop, 0);
    if (pthread_create(&ep->th, NULL, mep_thread, ep) != 0) {
        fprintf(stderr, "sdf_metrics_endpoint: pthread_create failed\n");
        close(fd);
        free(ep);
        return NULL;
    }
    return ep;
}

void sdf_metrics_endpoint_stop(sdf_metrics_endpoint_t *ep) {
    if (!ep)
        return;
    atomic_store(&ep->stop, 1);
    pthread_join(ep->th, NULL);
    close(ep->fd);
    free(ep);
}
