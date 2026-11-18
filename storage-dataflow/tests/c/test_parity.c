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
#include "sdf/wm.h"

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

/* Features implemented by the current tree. W2.1 adds
 * wm.method.shuffle1/shuffle2; W3.x adds fountain.abi_v2; W4.x adds
 * arc.abi_v2. A group's "requires" value must appear here (or be
 * absent) to be validated today. */
static const char *IMPLEMENTED[] = {"core", NULL};

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
 * compare paths one by one against the golden. */
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

/* Schema check for a future-wave golden file: every named group must
 * exist with an entries array; non-empty entries with an unimplemented
 * requires tag are pending, empty entries are schema-only (OK). */
static void check_schema_file(const char *rel, const char *const *groups,
                              size_t n_groups) {
    jval_t *root = load_golden(rel);
    for (size_t i = 0; i < n_groups; i++) {
        const jval_t *g = golden_json_get(root, groups[i]);
        if (g == NULL || g->type != J_OBJ) {
            fprintf(stderr, "FAIL: %s: missing group schema '%s'\n", rel,
                    groups[i]);
            exit(1);
        }
        const jval_t *entries = golden_json_get(g, "entries");
        if (entries == NULL || entries->type != J_ARR) {
            fprintf(stderr, "FAIL: %s: group '%s' lacks an entries array\n",
                    rel, groups[i]);
            exit(1);
        }
        const jval_t *req = golden_json_get(g, "requires");
        CHECK(req != NULL && req->type == J_STR);
        if (entries->n_items > 0 && !feature_implemented(req->str)) {
            g_pending++;
            printf("  pending: %-24s awaits %s (%zu entries present)\n",
                   groups[i], req->str, entries->n_items);
        }
    }
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

    {
        static const char *arc_groups[] = {"paper_12_3", "trace_replay_60fps"};
        check_schema_file("golden_vectors/arc_schedule.json", arc_groups, 2);
    }
    {
        static const char *fountain_groups[] = {"roundtrip", "lossy_repair"};
        check_schema_file("golden_vectors/fountain_esi.json", fountain_groups, 2);
    }

    if (g_validated == 0) {
        fprintf(stderr, "FAIL: parity gate validated nothing\n");
        return 1;
    }
    printf("test_parity: ok (validated %d golden comparisons; %d group(s) "
           "pending future waves)\n",
           g_validated, g_pending);
    return 0;
}
