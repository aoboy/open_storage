/*
 * Origin class-selection and configuration tests (plan prompt W6.15).
 *
 * The selector must keep a configured set explicit: loopback and HTTP select
 * their driver, mixed/unknown sets fail -EINVAL, and homogeneous S3/peer
 * sets fail -ENOTSUP instead of being rewritten to loopback. The config
 * adapter materializes the strict-TOML [[origin]] entries exactly, including
 * every knob and the 32-bit fields that once had an incompatible-pointer
 * overflow (found by the W6.15 review).
 */

#define _POSIX_C_SOURCE 200809L

#include "sdf/common/config.h"
#include "sdf/origin/config.h"
#include "sdf/origin/http.h"
#include "sdf/origin/loopback.h"

#include <errno.h>
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

static void url_cfg(sdf_origin_cfg_t *c, uint32_t id, uint32_t cls, const char *dst) {
    memset(c, 0, sizeof(*c));
    c->id = id;
    c->cls = cls;
    snprintf(c->dst, sizeof(c->dst), "%s", dst);
}

static void check_status(sdf_origin_cfg_t *cfg, uint32_t n, int want_rc,
                         sdf_origin_select_status_t want_status, const sdf_origin_ops_t *want_ops) {
    const sdf_origin_ops_t *ops = (const sdf_origin_ops_t *)0xdeadbeef;
    sdf_origin_select_status_t status = SDF_ORIGIN_SELECT_OK;
    int rc = sdf_origin_select(cfg, n, &ops, &status);
    CHECK(rc == want_rc);
    CHECK(status == want_status);
    if (want_ops != NULL) {
        CHECK(ops == want_ops);
    } else {
        CHECK(ops == NULL);
    }
}

static int g_case_no;
static char g_tmp[160];

static int load_text(const char *text, sdf_config_t *cfg) {
    snprintf(g_tmp, sizeof(g_tmp), "/tmp/sdf-origin-cfg-%d-%02d.toml", (int)getpid(), ++g_case_no);
    FILE *f = fopen(g_tmp, "wb");
    CHECK(f != NULL);
    CHECK(fputs(text, f) >= 0);
    CHECK(fclose(f) == 0);
    memset(cfg, 0, sizeof(*cfg));
    int rc = sdf_config_load(cfg, g_tmp);
    CHECK(unlink(g_tmp) == 0);
    return rc;
}

static void test_class_parse_and_names(void) {
    uint32_t cls = 99;
    CHECK(sdf_origin_cls_parse("loopback", &cls) == 0 && cls == SDF_ORIGIN_LOOPBACK);
    CHECK(sdf_origin_cls_parse("http", &cls) == 0 && cls == SDF_ORIGIN_HTTP);
    CHECK(sdf_origin_cls_parse("s3", &cls) == 0 && cls == SDF_ORIGIN_S3);
    CHECK(sdf_origin_cls_parse("peer", &cls) == 0 && cls == SDF_ORIGIN_PEER);
    CHECK(sdf_origin_cls_parse("ftp", &cls) == -EINVAL);
    CHECK(sdf_origin_cls_parse(NULL, &cls) == -EINVAL);
    CHECK(sdf_origin_cls_parse("http", NULL) == -EINVAL);
    CHECK(strcmp(sdf_origin_cls_name(SDF_ORIGIN_S3), "s3") == 0);

    CHECK(strcmp(sdf_origin_select_status_name(SDF_ORIGIN_SELECT_OK), "ok") == 0);
    CHECK(strcmp(sdf_origin_select_status_name(SDF_ORIGIN_SELECT_MIXED),
                 "mixed origin classes are not supported") == 0);
    CHECK(strcmp(sdf_origin_select_status_name(SDF_ORIGIN_SELECT_UNSUPPORTED),
                 "origin class is reserved but not implemented") == 0);
    CHECK(strcmp(sdf_origin_select_status_name(SDF_ORIGIN_SELECT_UNKNOWN),
                 "unknown origin class") == 0);
    CHECK(strcmp(sdf_origin_select_status_name((sdf_origin_select_status_t)99),
                 "invalid origin selection status") == 0);
}

static void test_selector(void) {
    sdf_origin_cfg_t cfg[3];

    url_cfg(&cfg[0], 0, SDF_ORIGIN_LOOPBACK, "/tmp/a.bin");
    url_cfg(&cfg[1], 1, SDF_ORIGIN_LOOPBACK, "/tmp/b.bin");
    check_status(cfg, 2, 0, SDF_ORIGIN_SELECT_OK, sdf_loopback_ops());

    url_cfg(&cfg[0], 0, SDF_ORIGIN_HTTP, "http://127.0.0.1:1/obj");
    url_cfg(&cfg[1], 1, SDF_ORIGIN_HTTP, "http://127.0.0.2:1/obj");
    check_status(cfg, 2, 0, SDF_ORIGIN_SELECT_OK, sdf_http_ops());

    /* Mixed classes never choose a driver — even when one side is reserved. */
    url_cfg(&cfg[0], 0, SDF_ORIGIN_LOOPBACK, "/tmp/a.bin");
    url_cfg(&cfg[1], 1, SDF_ORIGIN_HTTP, "http://127.0.0.1:1/obj");
    check_status(cfg, 2, -EINVAL, SDF_ORIGIN_SELECT_MIXED, NULL);
    url_cfg(&cfg[0], 0, SDF_ORIGIN_HTTP, "http://127.0.0.1:1/obj");
    url_cfg(&cfg[1], 1, SDF_ORIGIN_S3, "s3://bucket/object");
    check_status(cfg, 2, -EINVAL, SDF_ORIGIN_SELECT_MIXED, NULL);

    url_cfg(&cfg[0], 0, SDF_ORIGIN_S3, "s3://bucket/object");
    url_cfg(&cfg[1], 1, SDF_ORIGIN_S3, "s3://bucket/object");
    check_status(cfg, 2, -ENOTSUP, SDF_ORIGIN_SELECT_UNSUPPORTED, NULL);
    url_cfg(&cfg[0], 0, SDF_ORIGIN_PEER, "peer-dc-a");
    check_status(cfg, 1, -ENOTSUP, SDF_ORIGIN_SELECT_UNSUPPORTED, NULL);

    /* Unknown wins over mixed, so a bad class is never misdiagnosed. */
    url_cfg(&cfg[0], 0, 99, "unknown-class");
    url_cfg(&cfg[1], 1, SDF_ORIGIN_HTTP, "http://127.0.0.1:1/obj");
    check_status(cfg, 2, -EINVAL, SDF_ORIGIN_SELECT_UNKNOWN, NULL);

    url_cfg(&cfg[0], 3, SDF_ORIGIN_LOOPBACK, "/tmp/a.bin");
    url_cfg(&cfg[1], 1, SDF_ORIGIN_LOOPBACK, "/tmp/b.bin");
    check_status(cfg, 2, -EINVAL, SDF_ORIGIN_SELECT_INVALID, NULL);
    url_cfg(&cfg[0], 0, SDF_ORIGIN_LOOPBACK, "");
    check_status(cfg, 1, -EINVAL, SDF_ORIGIN_SELECT_INVALID, NULL);

    const sdf_origin_ops_t *ops = NULL;
    sdf_origin_select_status_t status = SDF_ORIGIN_SELECT_OK;
    CHECK(sdf_origin_select(NULL, 1, &ops, &status) == -EINVAL);
    CHECK(status == SDF_ORIGIN_SELECT_INVALID);
    CHECK(sdf_origin_select(cfg, 0, &ops, &status) == -EINVAL);
    CHECK(sdf_origin_select(cfg, SDF_ORIGIN_MAX + 1, &ops, &status) == -EINVAL);
    CHECK(sdf_origin_select(cfg, 1, NULL, &status) == -EINVAL);

    /* The harness throttle is loopback-only: a real HTTP configuration must
     * fail loudly rather than silently lose a degradation step. */
    sdf_origin_t http = {.ops = sdf_http_ops(), .impl = NULL};
    CHECK(sdf_origin_set_harness_throttle_ns(&http, 0, 1000) == -ENOTSUP);
}

static void test_config_load(void) {
    sdf_config_t cfg;
    const char *text = "[wm]\nm = 1024\ninit_fracs = [0.5, 0.5]\n\n"
                       "[[origin]]\nid = 0\ncls = \"http\"\ndst = \"http://127.0.0.1:8000/obj\"\n"
                       "throttle_ns = 1000\nloss_pct = 5.5\nerr_pct = 6.5\nlat_jitter_ns = 2000\n"
                       "max_inflight = 1024\ntimeout_ms = 600000\n\n"
                       "[[origin]]\ncls = \"loopback\"\ndst = \"/tmp/origin.bin\"\n";
    CHECK(load_text(text, &cfg) == 0);

    sdf_origin_cfg_t origins[2];
    uint32_t n = 0;
    CHECK(sdf_origin_config_load(&cfg, origins, 2, &n) == 0);
    CHECK(n == 2);
    CHECK(origins[0].id == 0 && origins[0].cls == SDF_ORIGIN_HTTP);
    CHECK(strcmp(origins[0].dst, "http://127.0.0.1:8000/obj") == 0);
    CHECK(origins[0].throttle_ns == 1000);
    CHECK(origins[0].loss_pct == 5.5);
    CHECK(origins[0].err_pct == 6.5);
    CHECK(origins[0].lat_jitter_ns == 2000);
    CHECK(origins[0].max_inflight == 1024);
    CHECK(origins[0].timeout_ms == 600000);
    CHECK(origins[1].id == 1 && origins[1].cls == SDF_ORIGIN_LOOPBACK);
    CHECK(strcmp(origins[1].dst, "/tmp/origin.bin") == 0);
    CHECK(origins[1].max_inflight == 64);
    CHECK(origins[1].timeout_ms == 0);

    const sdf_origin_ops_t *ops = NULL;
    sdf_origin_select_status_t status = SDF_ORIGIN_SELECT_OK;
    CHECK(sdf_origin_select(origins, 2, &ops, &status) == -EINVAL);
    CHECK(status == SDF_ORIGIN_SELECT_MIXED);
    sdf_config_free(&cfg);
}

static void test_config_rejections(void) {
    sdf_config_t cfg;
    sdf_origin_cfg_t origins[2];
    uint32_t n = 99;

    /* No origin entries at all. */
    CHECK(load_text("[wm]\nm = 1024\n", &cfg) == 0);
    memset(origins, 0xa5, sizeof(origins));
    CHECK(sdf_origin_config_load(&cfg, origins, 2, &n) == -EINVAL);
    CHECK(n == 0);
    CHECK(origins[1].id == 0xa5a5a5a5u); /* failed materialization never overruns */
    sdf_config_free(&cfg);

    /* Strict TOML rejects sparse entries; malformed flat values are caught
     * even when a caller constructs them by hand. */
    static const struct {
        const char *name;
        const char *text;
        int load_rc; /* 0 = strict TOML loads, the adapter must reject */
    } strict_rej[] = {
        {"sparse origin set", "[[origin]]\nid = 1\ncls = \"loopback\"\ndst = \"x\"\n", 0},
        {"wrong id", "[[origin]]\nid = 1\ncls = \"loopback\"\ndst = \"x\"\n", 0},
        {"malformed float", "[[origin]]\ncls = \"loopback\"\ndst = \"x\"\nloss_pct = 1.2.3\n", -1},
        {"u32 range", "[[origin]]\ncls = \"loopback\"\ndst = \"x\"\nmax_inflight = 1025\n", -1},
    };
    for (size_t i = 0; i < sizeof(strict_rej) / sizeof(strict_rej[0]); i++) {
        CHECK(load_text(strict_rej[i].text, &cfg) == strict_rej[i].load_rc);
        if (strict_rej[i].load_rc == 0) {
            memset(origins, 0, sizeof(origins));
            n = 99;
            CHECK(sdf_origin_config_load(&cfg, origins, 2, &n) == -EINVAL);
            CHECK(n == 0);
        } else {
            CHECK(cfg.entries == NULL && cfg.count == 0);
        }
        sdf_config_free(&cfg);
    }

    sdf_config_entry_t flat[2] = {0};
    sdf_config_t manual = {.entries = flat, .count = 0, .capacity = 2};
    static const struct {
        const char *name;
        const char *key;
        const char *value;
        uint32_t count;
    } manual_rej[] = {
        {"malformed index", "origin[]", "x", 1},
        {"malformed suffix", "origin[0]", "x", 1},
        {"malformed class", "origin[0].cls", "unknown", 1},
        {"missing class", "origin[0].dst", "http://x/y", 1},
        {"missing destination", "origin[0].cls", "http", 1},
        {"empty destination", "origin[0].dst", "", 1},
        {"non-numeric throttle", "origin[0].throttle_ns", "fast", 1},
        {"NaN loss", "origin[0].loss_pct", "nan", 1},
        {"out-of-range loss", "origin[0].loss_pct", "100.5", 1},
        {"wrong id", "origin[0].id", "2", 1},
        {"bad max_inflight", "origin[0].max_inflight", "4294967296", 1},
        {"bad timeout", "origin[0].timeout_ms", "4294967296", 1},
    };
    for (size_t i = 0; i < sizeof(manual_rej) / sizeof(manual_rej[0]); i++) {
        snprintf(flat[0].key, sizeof(flat[0].key), "%s", manual_rej[i].key);
        snprintf(flat[0].value, sizeof(flat[0].value), "%s", manual_rej[i].value);
        flat[0].key[sizeof(flat[0].key) - 1] = '\0';
        flat[0].value[sizeof(flat[0].value) - 1] = '\0';
        manual.count = manual_rej[i].count;
        memset(origins, 0, sizeof(origins));
        n = 99;
        if (sdf_origin_config_load(&manual, origins, 2, &n) != -EINVAL || n != 0) {
            fprintf(stderr, "FAIL: manual config accepted: %s\n", manual_rej[i].name);
            exit(1);
        }
    }

    /* Capacity bounds and argument validation. */
    snprintf(flat[0].key, sizeof(flat[0].key), "origin[0].cls");
    snprintf(flat[0].value, sizeof(flat[0].value), "http");
    snprintf(flat[1].key, sizeof(flat[1].key), "origin[1].cls");
    snprintf(flat[1].value, sizeof(flat[1].value), "http");
    manual.count = 2;
    CHECK(sdf_origin_config_load(&manual, origins, 1, &n) == -EINVAL);
    CHECK(sdf_origin_config_load(&manual, origins, 0, &n) == -EINVAL);
    CHECK(sdf_origin_config_load(NULL, origins, 2, &n) == -EINVAL);
    CHECK(sdf_origin_config_load(&manual, NULL, 2, &n) == -EINVAL);
    CHECK(sdf_origin_config_load(&manual, origins, 2, NULL) == -EINVAL);
}

int main(void) {
    test_class_parse_and_names();
    test_selector();
    test_config_load();
    test_config_rejections();
    printf("test_origin_registry: ok (class selection precedence + strict origin "
           "materialization)\n");
    return 0;
}
