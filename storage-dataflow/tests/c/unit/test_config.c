/*
 * Strict TOML loader tests (plan prompt W0.3, ADR-0005).
 *
 * Positive legs: every shipped configs/<env> TOML file loads; flat keys and
 * canonical values are spot-checked (single tables, array-of-tables,
 * arrays); absent-key fallback implements the presence-merge rule.
 * Rejection legs: unknown tables/keys, type mismatches, range and enum
 * violations, missing required keys, duplicate keys, table redefinition,
 * parser syntax errors — each must fail the load and leave the config
 * clean. Adapted from wack_a_mole tests/test_config.c @ a0f0422.
 *
 * The source tree path is injected at compile time (SDF_SOURCE_DIR) so
 * the shipped examples load regardless of the test's CWD.
 */

#include "sdf/common/config.h"

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

#ifndef SDF_SOURCE_DIR
#define SDF_SOURCE_DIR "."
#endif

/* Runtime join: SRC_PATH cannot be a concatenation macro because most
 * call sites pass a runtime string (files[i]), and string-literal
 * concatenation only works on literals. */
static char g_src_path[512];
static const char *src_path(const char *rel) {
    snprintf(g_src_path, sizeof(g_src_path), "%s/%s", SDF_SOURCE_DIR, rel);
    return g_src_path;
}

static int g_case_no;
static char g_tmp[160];

/* Write text to a unique temp file, load it, unlink it, return the
 * loader's rc. cfg is zeroed before each load. */
static int load_text(const char *text, sdf_config_t *cfg) {
    snprintf(g_tmp, sizeof(g_tmp), "/tmp/sdf-test-config-%d-%02d.toml",
             (int)getpid(), ++g_case_no);
    FILE *f = fopen(g_tmp, "wb");
    CHECK(f != NULL);
    CHECK(fputs(text, f) >= 0);
    CHECK(fclose(f) == 0);
    memset(cfg, 0, sizeof(*cfg));
    int rc = sdf_config_load(cfg, g_tmp);
    CHECK(unlink(g_tmp) == 0);
    return rc;
}

static size_t count_keys_with(const sdf_config_t *cfg, const char *prefix,
                              const char *suffix) {
    size_t n = 0;
    size_t plen = strlen(prefix), slen = strlen(suffix);
    for (size_t i = 0; i < cfg->count; i++) {
        size_t klen = strlen(cfg->entries[i].key);
        if (klen >= plen + slen && strncmp(cfg->entries[i].key, prefix, plen) == 0 &&
            strcmp(cfg->entries[i].key + klen - slen, suffix) == 0)
            n++;
    }
    return n;
}

static void test_shipped_examples(void) {
    static const char *files[] = {
        "configs/dev/wm.toml",      "configs/dev/fluid.toml",
        "configs/dev/arc.toml",     "configs/staging/wm.toml",
        "configs/staging/fluid.toml", "configs/staging/arc.toml",
        "configs/prod/wm.toml",     "configs/prod/fluid.toml",
        "configs/prod/arc.toml",
    };

    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        sdf_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        if (sdf_config_load(&cfg, src_path(files[i])) != 0) {
            fprintf(stderr, "FAIL: shipped example rejected: %s\n", files[i]);
            exit(1);
        }
        sdf_config_free(&cfg);
    }

    /* dev/wm.toml — single tables, array-of-tables, arrays */
    sdf_config_t wm;
    CHECK(sdf_config_load(&wm, src_path("configs/dev/wm.toml")) == 0);
    CHECK(strcmp(sdf_config_get(&wm, "wm.m", "?"), "1024") == 0);
    uint64_t u = 0;
    CHECK(sdf_config_get_u64(&wm, "wm.m", &u) == 0 && u == 1024);
    CHECK(strcmp(sdf_config_get(&wm, "wm.method", "?"), "shuffle1") == 0);
    CHECK(strcmp(sdf_config_get(&wm, "wm.init_fracs", "?"), "[0.5, 0.5]") == 0);
    CHECK(strcmp(sdf_config_get(&wm, "wm.kill_switch", "?"), "false") == 0);
    CHECK(strcmp(sdf_config_get(&wm, "origin[0].dst", "?"), "dev-objects.bin") == 0);
    CHECK(strcmp(sdf_config_get(&wm, "origin[1].throttle_ns", "?"), "500000") == 0);
    CHECK(count_keys_with(&wm, "origin[", ".cls") == 2);
    /* presence merge: keys the file never set fall back */
    CHECK(strcmp(sdf_config_get(&wm, "origin[0].timeout_ms", "absent"), "absent") == 0);
    CHECK(strcmp(sdf_config_get(&wm, "arc.mode", "absent"), "absent") == 0);
    CHECK(sdf_config_get_u64(&wm, "wm.alpha", &u) == -1); /* float, not u64 */
    sdf_config_free(&wm);

    /* staging/wm.toml — rollout gate at 25, mixed origin classes */
    sdf_config_t wms;
    CHECK(sdf_config_load(&wms, src_path("configs/staging/wm.toml")) == 0);
    CHECK(strcmp(sdf_config_get(&wms, "wm.rollout_pct", "?"), "25") == 0);
    CHECK(strcmp(sdf_config_get(&wms, "origin[1].cls", "?"), "http") == 0);
    CHECK(strcmp(sdf_config_get(&wms, "origin[1].timeout_ms", "?"), "2000") == 0);
    sdf_config_free(&wms);

    /* prod/wm.toml — full rollout, three origins, knobs absent */
    sdf_config_t wmp;
    CHECK(sdf_config_load(&wmp, src_path("configs/prod/wm.toml")) == 0);
    CHECK(strcmp(sdf_config_get(&wmp, "wm.rollout_pct", "?"), "100") == 0);
    CHECK(strcmp(sdf_config_get(&wmp, "origin[2].cls", "?"), "peer") == 0);
    CHECK(count_keys_with(&wmp, "origin[", ".cls") == 3);
    CHECK(strcmp(sdf_config_get(&wmp, "origin[0].throttle_ns", "absent"), "absent") == 0);
    sdf_config_free(&wmp);

    /* fluid classes across environments */
    sdf_config_t fl;
    CHECK(sdf_config_load(&fl, src_path("configs/dev/fluid.toml")) == 0);
    CHECK(strcmp(sdf_config_get(&fl, "fluid.arq_fallback", "?"), "true") == 0);
    CHECK(strcmp(sdf_config_get(&fl, "fluid_class[0].name", "?"), "hot-metadata") == 0);
    CHECK(strcmp(sdf_config_get(&fl, "fluid_class[0].eps", "?"), "0.05") == 0);
    CHECK(strcmp(sdf_config_get(&fl, "fluid_class[0].codec", "?"), "lt") == 0);
    CHECK(strcmp(sdf_config_get(&fl, "fluid_class[1].name", "?"), "cold-checkpoint") == 0);
    CHECK(strcmp(sdf_config_get(&fl, "fluid_class[1].eps", "?"), "0.15") == 0);
    CHECK(strcmp(sdf_config_get(&fl, "fluid_class[1].codec", "?"), "nanorq") == 0);
    CHECK(count_keys_with(&fl, "fluid_class[", ".name") == 2);
    sdf_config_free(&fl);
    sdf_config_t fls;
    CHECK(sdf_config_load(&fls, src_path("configs/staging/fluid.toml")) == 0);
    CHECK(strcmp(sdf_config_get(&fls, "fluid_class[1].rollout_pct", "?"), "50") == 0);
    sdf_config_free(&fls);

    /* arc parameter set (ADR-0006 ranges) + shadow progression */
    sdf_config_t arc;
    CHECK(sdf_config_load(&arc, src_path("configs/dev/arc.toml")) == 0);
    CHECK(strcmp(sdf_config_get(&arc, "arc.mode", "?"), "arc_qg") == 0);
    CHECK(strcmp(sdf_config_get(&arc, "arc.shadow_mode", "?"), "true") == 0);
    CHECK(strcmp(sdf_config_get(&arc, "arc.budget_ms", "?"), "10") == 0);
    CHECK(strcmp(sdf_config_get(&arc, "arc.gamma_ns", "?"), "16666666") == 0);
    CHECK(sdf_config_get_u64(&arc, "arc.T_idle_ns", &u) == 0 && u == 1000000000ull);
    sdf_config_free(&arc);
    sdf_config_t arcp;
    CHECK(sdf_config_load(&arcp, src_path("configs/prod/arc.toml")) == 0);
    CHECK(strcmp(sdf_config_get(&arcp, "arc.shadow_mode", "?"), "false") == 0);
    sdf_config_free(&arcp);
}

static void test_parser_capabilities(void) {
    sdf_config_t cfg;
    /* ';' key separator and comments (wam plan K.2 grammar) */
    CHECK(load_text("[wm]\nm = 1024; kill_switch = true  # inline\n", &cfg) == 0);
    CHECK(strcmp(sdf_config_get(&cfg, "wm.m", "?"), "1024") == 0);
    CHECK(strcmp(sdf_config_get(&cfg, "wm.kill_switch", "?"), "true") == 0);
    sdf_config_free(&cfg);
    /* multi-line numeric array */
    CHECK(load_text("[wm]\ninit_fracs = [0.5,\n 0.5]\n", &cfg) == 0);
    CHECK(strcmp(sdf_config_get(&cfg, "wm.init_fracs", "?"), "[0.5, 0.5]") == 0);
    sdf_config_free(&cfg);
}

static void test_getter_semantics(void) {
    CHECK(strcmp(sdf_config_get(NULL, "wm.m", "fb"), "fb") == 0);
    CHECK(sdf_config_get_u64(NULL, "wm.m", NULL) == -1);
    sdf_config_free(NULL); /* no-op */

    sdf_config_t cfg;
    CHECK(load_text("[ab]\nbucket_mod = 100\n", &cfg) == 0);
    uint64_t u = 0;
    CHECK(sdf_config_get_u64(&cfg, "ab.bucket_mod", &u) == 0 && u == 100);
    CHECK(sdf_config_get_u64(&cfg, "ab.absent", &u) == -1);
    CHECK(sdf_config_get_u64(&cfg, "ab.bucket_mod", NULL) == -1); /* bad out */
    sdf_config_free(&cfg);
}

static void test_rejections(void) {
    static const struct {
        const char *name;
        const char *text;
    } rej[] = {
        {"unknown table", "[wmx]\nzz = 1\n"},
        {"unknown key", "[wm]\nzzz = 1\n"},
        {"unknown array key", "[[origin]]\nzzz = 1\n"},
        {"type mismatch (string for int)", "[wm]\nm = \"1024\"\n"},
        {"type mismatch (bool for int)", "[wm]\nm = true\n"},
        {"int out of range", "[wm]\nrollout_pct = 101\n"},
        {"not a power of two", "[wm]\nm = 1000\n"},
        {"even sb", "[wm]\nsb = 100\n"},
        {"bad method enum", "[wm]\nmethod = \"shuffle3\"\n"},
        {"bad cls enum", "[[origin]]\nid = 0\ncls = \"ftp\"\ndst = \"x\"\n"},
        {"missing required cls", "[[origin]]\nid = 0\ndst = \"x\"\n"},
        {"missing required dst", "[[origin]]\nid = 0\ncls = \"loopback\"\n"},
        {"missing required name", "[[fluid_class]]\neps = 0.05\n"},
        {"eps out of range", "[[fluid_class]]\nname = \"x\"\neps = 1.5\n"},
        {"abort ratio below 1", "[fluid]\noverhead_abort_ratio = 0.5\n"},
        {"budget_ms out of range", "[arc]\nbudget_ms = 86400001\n"},
        {"rho_u below band", "[arc]\nrho_u = 0.1\n"},
        {"duplicate key", "[wm]\nm = 1024\nm = 512\n"},
        {"table redefined", "[wm]\nm = 1024\n[wm]\nsb = 3\n"},
        {"missing value", "[wm]\nm =\n"},
        {"unterminated string", "[arc]\npipeline = \"shard\n"},
        {"string array", "[wm]\ninit_fracs = [\"a\", \"b\"]\n"},
        {"key outside any table", "m = 1024\n"},
        {"dotted key", "[wm]\na.b = 1\n"},
        {"garbage after value", "[wm]\nm = 1024 junk\n"},
        {"fracs do not sum", "[wm]\ninit_fracs = [0.5, 0.3]\n"},
        {"fracs value out of range", "[wm]\ninit_fracs = [0.5, 0.6, -0.1]\n"},
    };

    for (size_t i = 0; i < sizeof(rej) / sizeof(rej[0]); i++) {
        sdf_config_t cfg;
        if (load_text(rej[i].text, &cfg) != -1) {
            fprintf(stderr, "FAIL: rejection case accepted: %s\n", rej[i].name);
            exit(1);
        }
        /* a failed load leaves the config clean (callers may retry) */
        if (cfg.entries != NULL || cfg.count != 0) {
            fprintf(stderr, "FAIL: rejected load left state: %s\n", rej[i].name);
            exit(1);
        }
    }

    /* parser cap: 65-element array */
    {
        char big[1024];
        int off = snprintf(big, sizeof(big), "[wm]\ninit_fracs = [");
        for (int n = 0; n < 65; n++)
            off += snprintf(big + off, sizeof(big) - (size_t)off, "%s1.0",
                            n ? ", " : "");
        snprintf(big + off, sizeof(big) - (size_t)off, "]\n");
        sdf_config_t cfg;
        CHECK(load_text(big, &cfg) == -1);
        CHECK(cfg.entries == NULL && cfg.count == 0);
    }

    /* schema bound: a 65th [[origin]] block */
    {
        char big[4096];
        int off = 0;
        for (int b = 0; b < 65; b++)
            off += snprintf(big + off, sizeof(big) - (size_t)off,
                            "[[origin]]\ncls = \"loopback\"\ndst = \"x\"\n");
        sdf_config_t cfg;
        CHECK(load_text(big, &cfg) == -1);
        CHECK(cfg.entries == NULL && cfg.count == 0);
    }
}

int main(void) {
    test_shipped_examples();
    test_parser_capabilities();
    test_getter_semantics();
    test_rejections();
    printf("test_config: ok (9 shipped examples, %d rejection cases)\n",
           g_case_no);
    return 0;
}
