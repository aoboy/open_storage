/*
 * Golden-vector parity harness (plan prompt W0.4 — the ADR-0009 gate).
 *
 * Validates every golden group whose required feature the CURRENT ABI
 * implements, and counts groups that await a future prompt (seed
 * methods → W2.1, fountain ABI → W3.1/W3.2, ARC ABI → W4.2) as
 * PENDING, printed loudly in the summary — pending is coverage debt
 * recorded in the ledger, never silently green. Fails on: any
 * mismatch, a failed JSON parse (broken golden), or zero validated
 * vectors (the gate ran against nothing).
 */

#include "golden_json.h"
#include "sdf/arc.h"
#include "sdf/common/timer.h"
#include "sdf/fountain.h"
#include "sdf/wm.h"

#include <errno.h>
#include <math.h>
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

#ifndef SDF_SOURCE_DIR
#define SDF_SOURCE_DIR "."
#endif

static int g_validated; /* individual golden comparisons that passed */
static int g_pending;   /* groups awaiting a future prompt */

/* Features implemented by the current tree. W2.1 added the ADR-0007
 * seeded spray methods, so the seed golden groups validate; W3.x will
 * add fountain.abi_v2, W4.x arc.abi_v2. */
static const char *IMPLEMENTED[] = {
    "core", "wm.method.shuffle1", "wm.method.shuffle2", "fountain.abi_v2",
    "fountain.codec.nanorq", "fountain.codec.lt", "arc.abi_v2", NULL};

static int feature_implemented(const char *feature) {
    if (feature == NULL)
        return 1;
    for (size_t i = 0; IMPLEMENTED[i] != NULL; i++)
        if (strcmp(IMPLEMENTED[i], feature) == 0)
            return 1;
    return 0;
}

static jval_t *load_golden(const char *rel) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", SDF_SOURCE_DIR, rel);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "FAIL: cannot open %s\n", path);
        exit(1);
    }
    static char buf[262144];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    CHECK(fclose(f) == 0);
    buf[n] = '\0';
    jval_t *root = golden_json_parse(buf);
    if (root == NULL || root->type != J_OBJ) {
        fprintf(stderr, "FAIL: golden does not parse as a JSON object: %s\n", rel);
        exit(1);
    }
    return root;
}

/* Compile the canonical profile from the file's provenance block. */
static int compile_canonical(const jval_t *root, wm_compiled_t *c) {
    const jval_t *prov = golden_json_get(root, "provenance");
    const jval_t *prof = golden_json_get(prov, "profile");
    const jval_t *bm = golden_json_get(prof, "m");
    const jval_t *bn = golden_json_get(prof, "n");
    const jval_t *bb = golden_json_get(prof, "b");
    CHECK(bm != NULL && bn != NULL && bb != NULL && bb->type == J_ARR);

    wm_profile_t p;
    memset(&p, 0, sizeof(p));
    p.version = 1;
    p.m = (uint32_t)golden_json_int(bm, 0);
    p.n = (uint32_t)golden_json_int(bn, 0);
    CHECK(p.m == 1024 && p.n == 5 && bb->n_items == 5);
    for (size_t i = 0; i < bb->n_items; i++)
        p.b[i] = (uint32_t)golden_json_int(bb->items[i], 0);
    CHECK(p.b[0] == 127 && p.b[1] == 400 && p.b[2] == 200 && p.b[3] == 173 &&
          p.b[4] == 124);
    return wm_profile_compile(&p, c);
}

static void validate_theta(const jval_t *root, wm_compiled_t *c) {
    const jval_t *th = golden_json_get(root, "theta_ell10");
    CHECK(th != NULL && th->type == J_ARR && th->n_items >= 3);
    for (size_t i = 0; i < th->n_items; i++) {
        const jval_t *pair = th->items[i];
        CHECK(pair != NULL && pair->type == J_ARR && pair->n_items == 2);
        uint32_t j = (uint32_t)golden_json_int(pair->items[0], -1);
        uint32_t expect = (uint32_t)golden_json_int(pair->items[1], -1);
        CHECK(j < c->m);
        /* perm[j] is the public bit-reversal table: the paper's theta */
        if (c->perm[j] != expect) {
            fprintf(stderr, "FAIL: theta(%u, %u): got %u, expected %u\n", j,
                    c->log2m, c->perm[j], expect);
            exit(1);
        }
        g_validated++;
    }
}

/* A pick-sequence group: run wm_select over [from, from+count) and
 * compare paths one by one against the golden. Groups tagged with a
 * wm.method.* feature carry sa/sb and select through the seeded
 * methods (ADR-0007); untagged groups use the PLAIN default. */
static void validate_sequence(const char *name, const jval_t *root,
                              wm_compiled_t *c) {
    const jval_t *seq = golden_json_get(root, name);
    CHECK(seq != NULL && seq->type == J_OBJ);
    const jval_t *req = golden_json_get(seq, "requires");
    if (req != NULL && req->type == J_STR) {
        if (!feature_implemented(req->str)) {
            g_pending++;
            printf("  pending: %-16s awaits %s\n", name, req->str);
            return;
        }
    }

    /* recompile with the group's method/seeds when tagged */
    if (req != NULL && req->type == J_STR && req->str != NULL &&
        strncmp(req->str, "wm.method.shuffle", 17) == 0) {
        wm_profile_t p;
        memset(&p, 0, sizeof(p));
        p.version = 1;
        p.m = 1024;
        p.n = 5;
        p.b[0] = 127;
        p.b[1] = 400;
        p.b[2] = 200;
        p.b[3] = 173;
        p.b[4] = 124;
        wm_compile_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.method = req->str[17] == '1' ? WM_METHOD_SHUFFLE1
                                         : WM_METHOD_SHUFFLE2;
        cfg.sa = (uint32_t)golden_json_int(golden_json_get(seq, "sa"), 0);
        cfg.sb = (uint32_t)golden_json_int(golden_json_get(seq, "sb"), 1);
        if (wm_profile_compile_cfg(&p, c, &cfg) != 0) {
            fprintf(stderr, "FAIL: %s: seeded compile rejected\n", name);
            exit(1);
        }
    }

    uint32_t from = (uint32_t)golden_json_int(golden_json_get(seq, "from"), 0);
    uint32_t count = (uint32_t)golden_json_int(golden_json_get(seq, "count"), 0);
    const jval_t *paths = golden_json_get(seq, "paths");
    CHECK(count >= 32 && paths != NULL && paths->type == J_ARR &&
          paths->n_items == count);
    for (uint32_t k = 0; k < count; k++) {
        uint32_t expect = (uint32_t)golden_json_int(paths->items[k], 0xFFFF);
        uint32_t got = wm_select(c, (uint64_t)(from + k));
        if (got != expect) {
            fprintf(stderr, "FAIL: %s counter %u: got path %u, expected %u\n",
                    name, from + k, got, expect);
            exit(1);
        }
        g_validated++;
    }
}

static void validate_windows(const jval_t *root, wm_compiled_t *c) {
    const jval_t *wins = golden_json_get(root, "plain_windows");
    CHECK(wins != NULL && wins->type == J_ARR && wins->n_items >= 4);
    for (size_t i = 0; i < wins->n_items; i++) {
        const jval_t *w = wins->items[i];
        CHECK(w != NULL && w->type == J_OBJ);
        uint32_t from = (uint32_t)golden_json_int(golden_json_get(w, "from"), 0);
        uint32_t len = (uint32_t)golden_json_int(golden_json_get(w, "len"), 0);
        const jval_t *counts = golden_json_get(w, "counts");
        CHECK(len > 0 && counts != NULL && counts->type == J_ARR &&
              counts->n_items == c->n);
        uint32_t got[WM_MAX_PATHS] = {0};
        for (uint32_t k = 0; k < len; k++)
            got[wm_select(c, (uint64_t)(from + k))]++;
        for (uint32_t pi = 0; pi < c->n; pi++) {
            uint32_t expect = (uint32_t)golden_json_int(counts->items[pi],
                                                        0xFFFFFFFFu);
            if (got[pi] != expect) {
                fprintf(stderr,
                        "FAIL: window from=%u len=%u path %u: got %u, "
                        "expected %u\n",
                        from, len, pi, got[pi], expect);
                exit(1);
            }
            g_validated++;
        }
    }
}


/* ---- toy fountain codec (ABI v2 plumbing proof, plan W3.1) -------------
 *
 * Systematic source symbols (ESI < k) plus deterministic XOR repair
 * symbols (ESI >= k). It implements the full fountain.h surface —
 * toy_fe_sizeof/toy_fd_sizeof, in-place init, generate at any ESI, push with
 * duplicate accounting, ready/rank/decode — so test_parity proves the
 * ABI is implementable and the goldens are satisfiable BEFORE the real
 * codecs (nanorq W3.2, LT W3.3) land. */

#define TOY_MAX_K 64

struct fountain_enc {
    uint32_t k;
    uint32_t overhead; /* repair symbols exposed */
    size_t sym_len;
    const uint8_t *block; /* k * sym_len, caller-owned */
};

struct fountain_dec {
    uint32_t k;
    size_t sym_len;
    uint8_t have[TOY_MAX_K * 2]; /* received bitmap (sources+repair) */
    uint8_t syms[TOY_MAX_K * 2][256];
};

size_t toy_fe_sizeof(const fe_cfg_t *cfg) {
    if (cfg == NULL || cfg->k == 0 || cfg->k > TOY_MAX_K || cfg->block_len == 0)
        return 0;
    if (cfg->block_len % cfg->k != 0)
        return 0;
    return sizeof(struct fountain_enc);
}

size_t toy_fd_sizeof(const fd_cfg_t *cfg) {
    if (cfg == NULL || cfg->k == 0 || cfg->k > TOY_MAX_K ||
        cfg->sym_len == 0 || cfg->sym_len > 256)
        return 0;
    return sizeof(struct fountain_dec);
}

int toy_fe_init_in(fountain_enc_t *e, const fe_cfg_t *cfg, const uint8_t *block) {
    if (e == NULL || cfg == NULL || block == NULL || toy_fe_sizeof(cfg) == 0)
        return -EINVAL;
    /* the toy ignores cfg->codec: it exists to pin the ABI surface,
     * not to be an arm */
    struct fountain_enc *t = (struct fountain_enc *)e;
    t->k = cfg->k;
    t->overhead = cfg->k * cfg->overhead_pct / 100u;
    t->sym_len = cfg->block_len / cfg->k;
    t->block = block;
    return 0;
}

int toy_fd_init_in(fountain_dec_t *d, const fd_cfg_t *cfg) {
    if (d == NULL || cfg == NULL || toy_fd_sizeof(cfg) == 0)
        return -EINVAL;
    struct fountain_dec *t = (struct fountain_dec *)d;
    memset(t, 0, sizeof(*t));
    t->k = cfg->k;
    t->sym_len = cfg->sym_len;
    return 0;
}

int toy_fe_init(fountain_enc_t *e, const uint8_t *block, size_t block_len,
            uint32_t k, uint32_t overhead_pct) {
    fe_cfg_t cfg = {.block_len = block_len, .k = k, .overhead_pct = overhead_pct};
    return toy_fe_init_in(e, &cfg, block);
}

int toy_fd_init(fountain_dec_t *d, uint32_t k, size_t sym_len) {
    fd_cfg_t cfg = {.k = k, .sym_len = sym_len};
    return toy_fd_init_in(d, &cfg);
}

uint32_t toy_fe_symbol_count(const fountain_enc_t *e) {
    const struct fountain_enc *t = (const struct fountain_enc *)e;
    return t == NULL ? 0 : t->k + t->overhead;
}

int toy_fe_generate(fountain_enc_t *e, uint32_t esi, uint8_t *out, size_t out_len) {
    struct fountain_enc *t = (struct fountain_enc *)e;
    if (t == NULL || out == NULL || out_len != t->sym_len)
        return -EINVAL;
    if (esi >= toy_fe_symbol_count(e))
        return -EINVAL;
    if (esi < t->k) {
        memcpy(out, t->block + (size_t)esi * t->sym_len, t->sym_len);
    } else {
        memset(out, 0, t->sym_len);
        for (uint32_t j = 0; j < t->k; j++)
            for (size_t b = 0; b < t->sym_len; b++)
                out[b] ^= t->block[(size_t)j * t->sym_len + b];
    }
    return 0;
}

int toy_fd_push(fountain_dec_t *d, uint32_t esi, const uint8_t *sym, size_t len) {
    struct fountain_dec *t = (struct fountain_dec *)d;
    if (t == NULL || sym == NULL || len != t->sym_len)
        return -EINVAL;
    if (esi >= t->k + t->k) /* sources + (toy) repair window */
        return -EINVAL;
    if (t->have[esi])
        return 1; /* duplicate: counted, not an error */
    t->have[esi] = 1;
    memcpy(t->syms[esi], sym, len);
    return 0;
}

static uint32_t toy_sources(const struct fountain_dec *t) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < t->k; i++)
        n += t->have[i] ? 1u : 0u;
    return n;
}

bool toy_fd_ready(const fountain_dec_t *d) {
    const struct fountain_dec *t = (const struct fountain_dec *)d;
    if (t == NULL)
        return false;
    if (toy_sources(t) == t->k)
        return true;
    /* one missing source + at least one repair recovers it */
    if (toy_sources(t) == t->k - 1) {
        for (uint32_t r = t->k; r < 2 * t->k; r++)
            if (t->have[r])
                return true;
    }
    return false;
}

uint32_t toy_fd_rank(const fountain_dec_t *d) {
    const struct fountain_dec *t = (const struct fountain_dec *)d;
    if (t == NULL)
        return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < 2 * t->k; i++)
        n += t->have[i] ? 1u : 0u;
    return n;
}

int toy_fd_decode(fountain_dec_t *d, uint8_t *out, size_t block_len) {
    struct fountain_dec *t = (struct fountain_dec *)d;
    if (t == NULL || out == NULL || block_len != t->k * t->sym_len)
        return -EINVAL;
    if (!toy_fd_ready(d))
        return -EINVAL;

    uint8_t blk[TOY_MAX_K * 256];
    int missing = -1;
    for (uint32_t j = 0; j < t->k; j++) {
        if (t->have[j]) {
            memcpy(blk + (size_t)j * t->sym_len, t->syms[j], t->sym_len);
        } else {
            missing = (int)j;
        }
    }
    if (missing >= 0) {
        /* repair XOR of all symbols: missing = repair ^ others */
        const uint8_t *rep = NULL;
        for (uint32_t r = t->k; r < 2 * t->k; r++)
            if (t->have[r])
                rep = t->syms[r];
        if (rep == NULL)
            return -EINVAL;
        for (size_t b = 0; b < t->sym_len; b++) {
            uint8_t acc = rep[b];
            for (uint32_t j = 0; j < t->k; j++)
                if (t->have[j])
                    acc ^= t->syms[j][b];
            blk[(size_t)missing * t->sym_len + b] = acc;
        }
    }
    memcpy(out, blk, block_len);
    return 0;
}

void toy_fe_destroy(fountain_enc_t *e) { (void)e; }
void toy_fd_destroy(fountain_dec_t *d) { (void)d; }

/* ---- fountain goldens (plan W3.1) -------------------------------------- */

static int bytes_eq(const jval_t *arr, const uint8_t *buf, size_t n) {
    if (arr == NULL || arr->type != J_ARR || arr->n_items != n)
        return 0;
    for (size_t i = 0; i < n; i++)
        if ((uint8_t)golden_json_int(arr->items[i], -1) != buf[i])
            return 0;
    return 1;
}

/* Run one golden entry through the LOCAL toy codec (the ABI-plumbing
 * proof): encoder reproduces every golden symbol, decoder reconstructs
 * the block from exactly the golden symbol set. */
static void validate_toy_entry(const jval_t *e, const char *group,
                                int lossy) {
    CHECK(e != NULL && e->type == J_OBJ);
    uint32_t k = (uint32_t)golden_json_int(golden_json_get(e, "k"), 0);
    size_t sym_len =
        (size_t)golden_json_int(golden_json_get(e, "sym_len"), 0);
    uint32_t overhead =
        (uint32_t)golden_json_int(golden_json_get(e, "overhead_pct"), 0);
    const jval_t *block = golden_json_get(e, "block");
    const jval_t *symbols = golden_json_get(e, "symbols");
    CHECK(k > 0 && sym_len > 0 && block != NULL && symbols != NULL &&
          block->type == J_ARR && block->n_items == k * sym_len);

    uint8_t blk[64 * 256];
    for (size_t i = 0; i < block->n_items; i++)
        blk[i] = (uint8_t)golden_json_int(block->items[i], -1);

    fe_cfg_t ecfg = {.block_len = k * (uint32_t)sym_len,
                     .k = k,
                     .overhead_pct = overhead};
    size_t esz = toy_fe_sizeof(&ecfg);
    CHECK(esz > 0);
    void *emem = malloc(esz);
    CHECK(emem != NULL);
    CHECK(toy_fe_init_in(emem, &ecfg, blk) == 0);
    CHECK(toy_fe_symbol_count(emem) >= k);

    /* every golden symbol reproduced byte-exact through the ABI */
    CHECK(symbols->type == J_ARR);
    for (size_t si = 0; si < symbols->n_items; si++) {
        const jval_t *sym = symbols->items[si];
        uint32_t esi = (uint32_t)golden_json_int(golden_json_get(sym, "esi"),
                                                 0xFFFF);
        uint8_t got[256];
        CHECK(toy_fe_generate(emem, esi, got, sym_len) == 0);
        if (!bytes_eq(golden_json_get(sym, "bytes"), got, sym_len)) {
            fprintf(stderr, "FAIL: %s toy symbol esi=%u mismatch\n", group,
                    esi);
            exit(1);
        }
        g_validated++;
    }
    CHECK(toy_fe_generate(emem, toy_fe_symbol_count(emem), blk, sym_len) ==
              -EINVAL);

    fd_cfg_t dcfg = {.k = k, .sym_len = sym_len};
    size_t dsz = toy_fd_sizeof(&dcfg);
    CHECK(dsz > 0);
    void *dmem = malloc(dsz);
    CHECK(dmem != NULL);
    CHECK(toy_fd_init_in(dmem, &dcfg) == 0);

    uint32_t dropped = 0;
    if (lossy) {
        dropped =
            (uint32_t)golden_json_int(golden_json_get(e, "dropped_esi"), 0);
        CHECK(dropped < k);
    }

    for (size_t si = 0; si < symbols->n_items; si++) {
        const jval_t *sym = symbols->items[si];
        uint32_t esi = (uint32_t)golden_json_int(golden_json_get(sym, "esi"),
                                                 0xFFFF);
        const jval_t *bytes = golden_json_get(sym, "bytes");
        CHECK(bytes != NULL && bytes->type == J_ARR &&
              bytes->n_items == sym_len);
        uint8_t buf[256];
        for (size_t b = 0; b < sym_len; b++)
            buf[b] = (uint8_t)golden_json_int(bytes->items[b], -1);
        CHECK(toy_fd_push(dmem, esi, buf, sym_len) == 0);
    }

    CHECK(toy_fd_ready(dmem)); /* decode sufficiency before shipping */
    uint8_t out[64 * 256];
    CHECK(toy_fd_decode(dmem, out, k * sym_len) == 0);
    if (!bytes_eq(block, out, k * sym_len)) {
        fprintf(stderr, "FAIL: %s toy decode mismatch\n", group);
        exit(1);
    }
    g_validated++;

    if (lossy) {
        /* the dropped source was genuinely absent: rank reflects it */
        CHECK(toy_fd_rank(dmem) == symbols->n_items);
    }

    /* duplicate pushes are counted, not errors */
    uint8_t buf[256];
    CHECK(toy_fe_generate(emem, 0, buf, sym_len) == 0);
    CHECK(toy_fd_push(dmem, 0, buf, sym_len) == 1);

    free(emem);
    free(dmem);
}


/* Run one golden entry through the REAL codec library: the arm comes
 * from the entry's "codec" field (nanorq now; lt with W3.3). */
static void validate_codec_entry(const jval_t *e, const char *group) {
    CHECK(e != NULL && e->type == J_OBJ);
    const jval_t *cj = golden_json_get(e, "codec");
    CHECK(cj != NULL && cj->type == J_STR);
    uint32_t codec = strcmp(cj->str, "nanorq") == 0 ? SDF_FOUNTAIN_NANORQ
                                                   : SDF_FOUNTAIN_LT;

    uint32_t k = (uint32_t)golden_json_int(golden_json_get(e, "k"), 0);
    size_t sym_len = (size_t)golden_json_int(golden_json_get(e, "sym_len"), 0);
    uint32_t overhead =
        (uint32_t)golden_json_int(golden_json_get(e, "overhead_pct"), 0);
    const jval_t *block = golden_json_get(e, "block");
    const jval_t *symbols = golden_json_get(e, "symbols");
    CHECK(k > 0 && sym_len > 0 && block != NULL && symbols != NULL &&
          block->type == J_ARR && block->n_items == k * sym_len);

    uint8_t *blk = malloc(k * sym_len);
    CHECK(blk != NULL);
    for (size_t i = 0; i < block->n_items; i++)
        blk[i] = (uint8_t)golden_json_int(block->items[i], -1);

    fe_cfg_t ecfg = {
        .block_len = k * sym_len, .k = k, .overhead_pct = overhead,
        .codec = codec,
    };
    size_t esz = fe_sizeof(&ecfg);
    CHECK(esz > 0);
    void *emem = malloc(esz);
    CHECK(emem != NULL);
    CHECK(fe_init_in(emem, &ecfg, blk) == 0);

    CHECK(symbols->type == J_ARR);
    for (size_t si = 0; si < symbols->n_items; si++) {
        const jval_t *sym = symbols->items[si];
        uint32_t esi =
            (uint32_t)golden_json_int(golden_json_get(sym, "esi"), 0xFFFF);
        uint8_t got[8192];
        CHECK(fe_generate(emem, esi, got, sym_len) == 0);
        if (!bytes_eq(golden_json_get(sym, "bytes"), got, sym_len)) {
            fprintf(stderr, "FAIL: %s codec symbol esi=%u mismatch\n", group,
                    esi);
            exit(1);
        }
        g_validated++;
    }

    fd_cfg_t dcfg = {.k = k, .sym_len = sym_len};
    size_t dsz = fd_sizeof(&dcfg);
    CHECK(dsz > 0);
    void *dmem = malloc(dsz);
    CHECK(dmem != NULL);
    CHECK(fd_init_codec(dmem, &dcfg, codec) == 0);

    for (size_t si = 0; si < symbols->n_items; si++) {
        const jval_t *sym = symbols->items[si];
        uint32_t esi =
            (uint32_t)golden_json_int(golden_json_get(sym, "esi"), 0xFFFF);
        const jval_t *bytes = golden_json_get(sym, "bytes");
        CHECK(bytes != NULL && bytes->type == J_ARR &&
              bytes->n_items == sym_len);
        uint8_t *buf = malloc(sym_len);
        CHECK(buf != NULL);
        for (size_t b = 0; b < sym_len; b++)
            buf[b] = (uint8_t)golden_json_int(bytes->items[b], -1);
        int prc = fd_push(dmem, esi, buf, sym_len);
        CHECK(prc == 0 || prc == 1); /* added, or redundant after complete */
        free(buf);
    }

    CHECK(fd_ready(dmem));
    uint8_t *out = malloc(k * sym_len);
    CHECK(out != NULL);
    CHECK(fd_decode(dmem, out, k * sym_len) == 0);
    if (!bytes_eq(block, out, k * sym_len)) {
        fprintf(stderr, "FAIL: %s codec decode mismatch\n", group);
        exit(1);
    }
    g_validated++;

    fe_destroy(emem);
    fd_destroy(dmem);
    free(blk);
    free(out);
    free(emem);
    free(dmem);
}

static void validate_fountain(const char *rel, const char *group, int lossy,
                                int use_real) {
    jval_t *root = load_golden(rel);
    const jval_t *g = golden_json_get(root, group);
    CHECK(g != NULL && g->type == J_OBJ);
    const jval_t *entries = golden_json_get(g, "entries");
    CHECK(entries != NULL && entries->type == J_ARR);
    for (size_t i = 0; i < entries->n_items; i++) {
        if (use_real)
            validate_codec_entry(entries->items[i], group);
        else
            validate_toy_entry(entries->items[i], group, lossy);
    }
    if (entries->n_items == 0) {
        const jval_t *req = golden_json_get(g, "requires");
        CHECK(req != NULL && req->type == J_STR);
        if (!feature_implemented(req->str)) {
            g_pending++;
            printf("  pending: %-24s awaits %s (schema present)\n", group,
                   req->str);
        }
    }
    golden_json_free(root);
}

/* Validate a real-codec group (nanorq / lt): entries through the
 * library when the arm is implemented, pending otherwise. */
static void validate_codec_group(const char *rel, const char *group,
                                  const char *feature) {
    if (!feature_implemented(feature)) {
        jval_t *root = load_golden(rel);
        const jval_t *g = golden_json_get(root, group);
        const jval_t *entries = g ? golden_json_get(g, "entries") : NULL;
        int n = (entries && entries->type == J_ARR)
                    ? (int)entries->n_items : 0;
        if (n > 0) {
            g_pending++;
            printf("  pending: %-24s awaits %s (%d entries present)\n",
                   group, feature, n);
        }
        golden_json_free(root);
        return;
    }
    validate_fountain(rel, group, 0, 1);
}


/* ---- ARC goldens (plan W6.2) ------------------------------------------- */

/*
 * Replay one paper_12_3 entry through the public ABI on the REAL
 * receiver clock — the extractor's exact drive (S_n = now - d, the
 * alternating dH/dL pattern): every recorded D sample must reproduce
 * within tolerance_ns. A sample is one comparison.
 */
static void validate_arc_entry(const jval_t *e, const char *group) {
    CHECK(e != NULL && e->type == J_OBJ);
    int drive_blocks = (int)golden_json_int(golden_json_get(e, "drive_blocks"), 0);
    int sample_blocks =
        (int)golden_json_int(golden_json_get(e, "sample_blocks"), 0);
    uint64_t d_high =
        (uint64_t)golden_json_int(golden_json_get(e, "d_high_ns"), 0);
    uint64_t d_low = (uint64_t)golden_json_int(golden_json_get(e, "d_low_ns"), 0);
    const jval_t *v;
    double rho_u = (v = golden_json_get(e, "rho_u")) != NULL && v->type == J_NUM
                       ? v->num : 0.0;
    double rho_l = (v = golden_json_get(e, "rho_l")) != NULL && v->type == J_NUM
                       ? v->num : 0.0;
    double lambda_u =
        (v = golden_json_get(e, "lambda_u")) != NULL && v->type == J_NUM
            ? v->num : 0.0;
    double lambda_l =
        (v = golden_json_get(e, "lambda_l")) != NULL && v->type == J_NUM
            ? v->num : 0.0;
    uint64_t U_ns = (uint64_t)golden_json_int(golden_json_get(e, "U_ns"), 0);
    uint64_t delta_ns =
        (uint64_t)golden_json_int(golden_json_get(e, "delta_ns"), 0);
    uint64_t J_ns = (uint64_t)golden_json_int(golden_json_get(e, "J_ns"), 0);
    double tol = (double)golden_json_int(golden_json_get(e, "tolerance_ns"),
                                         500000);
    const jval_t *samples = golden_json_get(e, "samples");
    CHECK(drive_blocks > 0 && sample_blocks > 0 && d_high > d_low);
    CHECK(samples != NULL && samples->type == J_ARR &&
          samples->n_items == (size_t)sample_blocks);

    arc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.capacity = 64;
    cfg.entry_max = 64;
    cfg.mode = ARC_MODE_BASE;
    cfg.auto_params = 0; /* the goldens pin the Algorithm-1 fixed point */
    cfg.min_smooth_bytes = 0;
    cfg.rho_u = rho_u;
    cfg.rho_l = rho_l;
    cfg.lambda_u = lambda_u;
    cfg.lambda_l = lambda_l;
    cfg.U = U_ns;
    cfg.delta = delta_ns;
    cfg.J = J_ns;
    cfg.G = 50000000ull;
    cfg.T_idle_ns = 1000000000000ull; /* no mid-drive re-anchors */

    size_t sz = arc_sizeof(&cfg);
    CHECK(sz > 0);
    void *p = malloc(sz);
    CHECK(p != NULL);
    CHECK(arc_init(p, &cfg, NULL, NULL) == 0);

    /* index the samples by n for the replay */
    size_t sample_i = 0;
    int sample_at = drive_blocks - sample_blocks;
    for (int n = 0; n < drive_blocks; n++) {
        uint64_t d = (n % 2 == 0) ? d_high : d_low;
        uint64_t Sn = sdf_now_ns() - d;
        uint8_t entry[64];
        memset(entry, (uint8_t)n, sizeof(entry));
        CHECK(arc_submit(p, entry, sizeof(entry), Sn) == 0);
        (void)arc_poll(p, UINT64_MAX / 4);
        if (n >= sample_at && sample_i < samples->n_items) {
            const jval_t *smp = samples->items[sample_i++];
            int expect_n =
                (int)golden_json_int(golden_json_get(smp, "n"), -1);
            const jval_t *dv = golden_json_get(smp, "D_ns");
            CHECK(dv != NULL && dv->type == J_NUM);
            double expect_D = dv->num;
            CHECK(expect_n == n); /* the extractor's exact trajectory */
            arc_stats_t st = arc_stats(p);
            if (fabs(st.D - expect_D) > tol) {
                fprintf(stderr,
                        "FAIL: %s sample n=%d: D=%.1f expected %.1f "
                        "(tol %.0f)\n",
                        group, n, st.D, expect_D, tol);
                exit(1);
            }
            g_validated++;
        }
    }
    CHECK(sample_i == samples->n_items); /* every recorded sample seen */

    free(p);
}

static void validate_arc_group(const char *rel, const char *group) {
    jval_t *root = load_golden(rel);
    const jval_t *g = golden_json_get(root, group);
    CHECK(g != NULL && g->type == J_OBJ);
    const jval_t *entries = golden_json_get(g, "entries");
    CHECK(entries != NULL && entries->type == J_ARR);
    if (entries->n_items == 0) {
        printf("  note: %-24s has no JSON entries by design (timing-based "
               "leg stays in C)\n",
               group);
        golden_json_free(root);
        return;
    }
    for (size_t i = 0; i < entries->n_items; i++)
        validate_arc_entry(entries->items[i], group);
    golden_json_free(root);
}


int main(void) {
    jval_t *root = load_golden("golden_vectors/wm_select.json");

    wm_compiled_t c;
    CHECK(compile_canonical(root, &c) == 0);

    validate_theta(root, &c);
    validate_sequence("plain_sequence", root, &c);
    validate_windows(root, &c);
    validate_sequence("seed_shuffle1", root, &c);
    validate_sequence("seed_shuffle2", root, &c);
    golden_json_free(root);

    validate_arc_group("golden_vectors/arc_schedule.json", "paper_12_3");
    validate_arc_group("golden_vectors/arc_schedule.json",
                        "trace_replay_60fps");
    validate_fountain("golden_vectors/fountain_esi.json", "roundtrip", 0, 0);
    validate_fountain("golden_vectors/fountain_esi.json", "lossy_repair", 1, 0);
    validate_codec_group("golden_vectors/fountain_esi.json", "nanorq",
                         "fountain.codec.nanorq");
    validate_codec_group("golden_vectors/fountain_esi.json", "lt",
                         "fountain.codec.lt");

    if (g_validated == 0) {
        fprintf(stderr, "FAIL: parity gate validated nothing\n");
        return 1;
    }
    printf("test_parity: ok (validated %d golden comparisons; %d group(s) "
           "pending future waves)\n",
           g_validated, g_pending);
    return 0;
}


