/*
 * Fountain golden extractor (plan W3.2/W6.11, ADR-0009).
 *
 * Emits a real-codec roundtrip entry on stdout through the public
 * fountain ABI — the vectors are EXTRACTED from the arm implementation,
 * never hand-derived. With no geometry options it emits the original
 * K=16/T=16/100% entry byte-for-byte; the options add supported shapes
 * without re-deriving existing expectations.
 *
 *   cc -O2 -o /tmp/gen_fountain tools/gen_goldens_fountain.c \
 *      -Iinclude -Ithird_party/nanorq/include \
 *      build/libsdf_fountain.a third_party/nanorq/libnanorq.a \
 *      build/libsdf_common.a -lm
 *   /tmp/gen_fountain [--codec nanorq|lt] [--k N] [--sym-len N] \
 *                     [--overhead PCT] [--symbols N]
 */

#include "sdf/fountain.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--codec nanorq|lt] [--k N] [--sym-len N] "
            "[--overhead PCT] [--symbols N]\n",
            argv0);
}

static int parse_u32(const char *text, uint32_t *out) {
    if (text == NULL || text[0] == '\0' || text[0] == '-') {
        return -1;
    }
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (*end != '\0' || value > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

int main(int argc, char **argv) {
    uint32_t k = 16;
    uint32_t sym_len = 16;
    uint32_t overhead = 100;
    uint32_t nsym = 20; /* K + 4 repairs for the original shape */
    uint32_t codec = SDF_FOUNTAIN_NANORQ;
    const char *cname = "nanorq";

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--codec") == 0 && i + 1 < argc) {
            const char *value = argv[++i];
            if (strcmp(value, "nanorq") == 0) {
                codec = SDF_FOUNTAIN_NANORQ;
                cname = "nanorq";
            } else if (strcmp(value, "lt") == 0) {
                codec = SDF_FOUNTAIN_LT;
                cname = "lt";
            } else {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(arg, "--k") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &k) != 0) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(arg, "--sym-len") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &sym_len) != 0) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(arg, "--overhead") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &overhead) != 0) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(arg, "--symbols") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &nsym) != 0 || nsym == 0) {
                usage(argv[0]);
                return 2;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (k == 0 || sym_len == 0 || k > SIZE_MAX / sym_len) {
        usage(argv[0]);
        return 2;
    }
    size_t block_len = (size_t)k * sym_len;
    if (block_len > UINT32_MAX) {
        usage(argv[0]);
        return 2;
    }

    uint8_t *block = malloc(block_len);
    uint8_t *sym = malloc(sym_len);
    if (block == NULL || sym == NULL) {
        free(block);
        free(sym);
        return 1;
    }
    for (size_t i = 0; i < block_len; i++) {
        block[i] = (uint8_t)((i * 2654435761ull) >> 24);
    }

    fe_cfg_t cfg = {
        .block_len = (uint32_t)block_len,
        .k = k,
        .overhead_pct = overhead,
        .codec = codec,
    };
    size_t esz = fe_sizeof(&cfg);
    if (esz == 0) {
        free(block);
        free(sym);
        return 1;
    }
    void *enc = malloc(esz);
    if (enc == NULL || fe_init_in(enc, &cfg, block) != 0) {
        free(enc);
        free(block);
        free(sym);
        return 1;
    }

    printf("    {\"codec\": \"%s\", \"k\": %u, \"sym_len\": %u, \"overhead_pct\": %u, "
           "\"block\": [",
           cname, k, sym_len, overhead);
    for (size_t i = 0; i < block_len; i++) {
        printf("%s%u", i ? ", " : "", block[i]);
    }
    printf("], \"symbols\": [");
    for (uint32_t esi = 0; esi < nsym; esi++) {
        if (fe_generate(enc, esi, sym, sym_len) != 0) {
            free(enc);
            free(block);
            free(sym);
            return 1;
        }
        printf("%s{\"esi\": %u, \"bytes\": [", esi ? ", " : "", esi);
        for (size_t b = 0; b < sym_len; b++) {
            printf("%s%u", b ? ", " : "", sym[b]);
        }
        printf("]}");
    }
    printf("]}\n");

    fe_destroy(enc);
    free(enc);
    free(block);
    free(sym);
    return 0;
}
