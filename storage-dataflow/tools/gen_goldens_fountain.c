/*
 * One-time nanorq golden extractor (plan prompt W3.2, ADR-0009).
 *
 * Emits the nanorq group's roundtrip entry on stdout through the public
 * fountain ABI — the vectors are EXTRACTED from the vendored RaptorQ
 * implementation, never hand-derived:
 *
 *   cc -O2 -o /tmp/gen_fountain tools/gen_goldens_fountain.c \
 *      -Iinclude -Ithird_party/nanorq/include third_party/nanorq/libnanorq.a \
 *      build/libsdf_fountain.a build/libsdf_common.a -lm
 *   /tmp/gen_fountain >> merge into golden_vectors/fountain_esi.json
 */

#include "sdf/fountain.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    enum { K = 16, T = 16, OVERHEAD = 100, NSYM = 20 }; /* K + 4 repairs */
    uint32_t codec = SDF_FOUNTAIN_NANORQ;
    const char *cname = "nanorq";
    if (argc == 3 && strcmp(argv[1], "--codec") == 0 &&
        strcmp(argv[2], "lt") == 0) {
        codec = SDF_FOUNTAIN_LT;
        cname = "lt";
    }
    static uint8_t block[K * T];
    for (size_t i = 0; i < sizeof(block); i++)
        block[i] = (uint8_t)((i * 2654435761ull) >> 24);

    fe_cfg_t cfg = {
        .block_len = sizeof(block), .k = K, .overhead_pct = OVERHEAD,
        .codec = codec,
    };
    void *enc = malloc(fe_sizeof(&cfg));
    if (enc == NULL || fe_init_in(enc, &cfg, block) != 0)
        return 1;

    printf("    {\"codec\": \"%s\", \"k\": %d, \"sym_len\": %d, "
           "\"overhead_pct\": %d, \"block\": [",
           cname, (int)K, (int)T, (int)OVERHEAD);
    for (size_t i = 0; i < sizeof(block); i++)
        printf("%s%u", i ? ", " : "", block[i]);
    printf("], \"symbols\": [");
    for (uint32_t esi = 0; esi < NSYM; esi++) {
        uint8_t sym[T];
        if (fe_generate(enc, esi, sym, T) != 0)
            return 1;
        printf("%s{\"esi\": %u, \"bytes\": [", esi ? ", " : "", esi);
        for (size_t b = 0; b < T; b++)
            printf("%s%u", b ? ", " : "", sym[b]);
        printf("]}");
    }
    printf("]}\n");

    fe_destroy(enc);
    free(enc);
    return 0;
}
