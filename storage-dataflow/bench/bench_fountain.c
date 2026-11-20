/*
 * Fountain LT micro-bench (plan W6.12).
 *
 * Measures the public fountain ABI in phases: encoder/decoder init,
 * symbol generation, push, and decode. The same binary is built once
 * with the C LT arm and once with the Rust LT arm; the paired script
 * runs identical configurations and compares the recorded lines.
 *
 * RECORDED ONLY: no performance threshold is enforced here. nanorq is
 * available with --codec nanorq for context, but the C/Rust paired
 * comparison uses LT only.
 */

#include "bench_common.h"
#include "sdf/fountain.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

#ifndef SDF_BENCH_BUILD_TYPE
#define SDF_BENCH_BUILD_TYPE "unknown"
#endif

#ifndef SDF_BENCH_REVISION
#define SDF_BENCH_REVISION "unknown"
#endif

#ifndef SDF_BENCH_FOUNTAIN_VARIANT
#define SDF_BENCH_FOUNTAIN_VARIANT "unknown"
#endif

#if defined(__clang__)
#define BENCH_CC "clang"
#elif defined(__GNUC__)
#define BENCH_CC "gcc"
#else
#define BENCH_CC "cc"
#endif

#define FOUNTAIN_BENCH_DEFAULT_K 64u
#define FOUNTAIN_BENCH_DEFAULT_SYM 32u
#define FOUNTAIN_BENCH_DEFAULT_BLOCKS 512u
#define FOUNTAIN_BENCH_DEFAULT_OVERHEAD 100u

static int cmp_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    return 0;
}

static int is_dropped_source(uint32_t esi, uint32_t k, uint32_t loss_pct) {
    if (loss_pct == 0) {
        return 0;
    }
    return (uint64_t)esi * 100u / k < loss_pct;
}

static int run_config(uint32_t k, size_t sym_len, uint32_t overhead_pct, uint32_t loss_pct,
                      uint32_t blocks, uint32_t codec, const char *runner) {
    size_t block_len = (size_t)k * sym_len;
    uint8_t *block = malloc(block_len);
    uint8_t *out = malloc(block_len);
    if (block == NULL || out == NULL) {
        fprintf(stderr, "bench_fountain: out of memory\n");
        free(block);
        free(out);
        return -ENOMEM;
    }

    fe_cfg_t ecfg = {
        .block_len = (uint32_t)block_len,
        .k = k,
        .overhead_pct = overhead_pct,
        .codec = codec,
    };
    fd_cfg_t dcfg = {.k = k, .sym_len = sym_len};
    size_t esz = fe_sizeof(&ecfg);
    size_t dsz = fd_sizeof(&dcfg);
    if (esz == 0 || dsz == 0) {
        fprintf(stderr, "bench_fountain: codec rejected K=%u sym_len=%zu\n", k, sym_len);
        free(block);
        free(out);
        return -EINVAL;
    }

    void *enc = malloc(esz);
    void *dec = malloc(dsz);
    if (enc == NULL || dec == NULL) {
        fprintf(stderr, "bench_fountain: out of memory\n");
        free(enc);
        free(dec);
        free(block);
        free(out);
        return -ENOMEM;
    }

    for (size_t i = 0; i < block_len; i++) {
        block[i] = (uint8_t)((i * 31u + 7u) & 0xffu);
    }
    if (fe_init_in(enc, &ecfg, block) != 0) {
        fprintf(stderr, "bench_fountain: fe_init_in failed\n");
        free(enc);
        free(dec);
        free(block);
        free(out);
        return -EIO;
    }
    uint32_t symbols = fe_symbol_count(enc);
    if (symbols < k) {
        fprintf(stderr, "bench_fountain: encoder exposed %u symbols for K=%u\n", symbols, k);
        fe_destroy(enc);
        free(enc);
        free(dec);
        free(block);
        free(out);
        return -EIO;
    }

    size_t sym_bytes = (size_t)symbols * sym_len;
    uint8_t *sym = malloc(sym_bytes == 0 ? 1u : sym_bytes);
    if (sym == NULL) {
        fe_destroy(enc);
        free(enc);
        free(dec);
        free(block);
        free(out);
        return -ENOMEM;
    }
    uint64_t *totals = calloc(blocks, sizeof(*totals));
    if (totals == NULL) {
        free(sym);
        fe_destroy(enc);
        free(enc);
        free(dec);
        free(block);
        free(out);
        return -ENOMEM;
    }

    uint64_t init_ns = 0;
    uint64_t generate_ns = 0;
    uint64_t push_ns = 0;
    uint64_t decode_ns = 0;
    uint64_t total_ns = 0;
    uint64_t pushed_symbols = 0;
    uint64_t checksum = 0;

    for (uint32_t b = 0; b < blocks; b++) {
        uint64_t t0 = sdf_bench_now_ns();
        if (fe_init_in(enc, &ecfg, block) != 0 || fd_init_codec(dec, &dcfg, codec) != 0) {
            fprintf(stderr, "bench_fountain: block %u init failed\n", b);
            free(totals);
            free(sym);
            fe_destroy(enc);
            fd_destroy(dec);
            free(enc);
            free(dec);
            free(block);
            free(out);
            return -EIO;
        }
        uint64_t t1 = sdf_bench_now_ns();
        init_ns += t1 - t0;

        for (uint32_t esi = 0; esi < symbols; esi++) {
            if (fe_generate(enc, esi, sym + (size_t)esi * sym_len, sym_len) != 0) {
                fprintf(stderr, "bench_fountain: block %u generate ESI %u failed\n", b, esi);
                free(totals);
                free(sym);
                fe_destroy(enc);
                fd_destroy(dec);
                free(enc);
                free(dec);
                free(block);
                free(out);
                return -EIO;
            }
        }
        uint64_t t2 = sdf_bench_now_ns();
        generate_ns += t2 - t1;

        for (uint32_t esi = 0; esi < symbols; esi++) {
            if (esi < k && is_dropped_source(esi, k, loss_pct)) {
                continue;
            }
            int rc = fd_push(dec, esi, sym + (size_t)esi * sym_len, sym_len);
            if (rc != 0 && rc != 1) {
                fprintf(stderr, "bench_fountain: block %u push ESI %u returned %d\n", b, esi, rc);
                free(totals);
                free(sym);
                fe_destroy(enc);
                fd_destroy(dec);
                free(enc);
                free(dec);
                free(block);
                free(out);
                return -EIO;
            }
            pushed_symbols++;
        }
        uint64_t t3 = sdf_bench_now_ns();
        push_ns += t3 - t2;

        if (!fd_ready(dec) || fd_decode(dec, out, block_len) != 0 ||
            memcmp(out, block, block_len) != 0) {
            fprintf(stderr, "bench_fountain: block %u loss=%u%% decode mismatch\n", b, loss_pct);
            free(totals);
            free(sym);
            fe_destroy(enc);
            fd_destroy(dec);
            free(enc);
            free(dec);
            free(block);
            free(out);
            return -EIO;
        }
        uint64_t t4 = sdf_bench_now_ns();
        decode_ns += t4 - t3;
        totals[b] = t4 - t0;
        total_ns += totals[b];
        checksum += (uint64_t)out[0] + (uint64_t)out[block_len - 1];
        fe_destroy(enc);
        fd_destroy(dec);
    }

    qsort(totals, blocks, sizeof(*totals), cmp_u64);
    uint64_t p99_index = (uint64_t)((double)blocks * 0.99);
    if (p99_index >= blocks) {
        p99_index = blocks - 1;
    }

    double per_block = (double)total_ns / (double)blocks;
    double p99 = (double)totals[p99_index];
    double throughput = total_ns == 0 ? 0.0 : (double)blocks * 1e9 / (double)total_ns;
    const char *codec_name = codec == SDF_FOUNTAIN_NANORQ ? "nanorq" : "lt";

    printf("BENCH fountain variant=%s revision=%s runner=%s build=%s cc=%s "
           "codec=%s k=%u sym_len=%zu overhead_pct=%u loss_pct=%u blocks=%u symbols=%u "
           "pushed_symbols=%" PRIu64 " init_ns_per_block=%.2f generate_ns_per_symbol=%.2f "
           "push_ns_per_symbol=%.2f decode_ns_per_block=%.2f ns_per_block=%.2f "
           "p99_ns_per_block=%.2f blocks_per_s=%.2f recorded_s=%lld sink=%" PRIu64 "\n",
           SDF_BENCH_FOUNTAIN_VARIANT, SDF_BENCH_REVISION, runner, SDF_BENCH_BUILD_TYPE, BENCH_CC,
           codec_name, k, sym_len, overhead_pct, loss_pct, blocks, symbols, pushed_symbols,
           (double)init_ns / (double)blocks,
           (double)generate_ns / ((double)blocks * (double)symbols),
           (double)push_ns / (double)pushed_symbols, (double)decode_ns / (double)blocks, per_block,
           p99, throughput, (long long)time(NULL), (uint64_t)(sdf_bench_sink + checksum));

    free(totals);
    free(sym);
    fe_destroy(enc);
    fd_destroy(dec);
    free(enc);
    free(dec);
    free(block);
    free(out);
    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--k N] [--sym-len N] [--overhead N] [--loss N] [--blocks N] "
            "[--codec lt|nanorq] [--runner NAME]\n"
            "  defaults: k=%u sym_len=%u overhead=%u loss=0 blocks=%u codec=lt\n",
            argv0, FOUNTAIN_BENCH_DEFAULT_K, FOUNTAIN_BENCH_DEFAULT_SYM,
            FOUNTAIN_BENCH_DEFAULT_OVERHEAD, FOUNTAIN_BENCH_DEFAULT_BLOCKS);
}

static int parse_u32(const char *text, uint32_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

int main(int argc, char **argv) {
    uint32_t k = FOUNTAIN_BENCH_DEFAULT_K;
    uint32_t sym_len = FOUNTAIN_BENCH_DEFAULT_SYM;
    uint32_t overhead = FOUNTAIN_BENCH_DEFAULT_OVERHEAD;
    uint32_t loss = 0;
    uint32_t blocks = FOUNTAIN_BENCH_DEFAULT_BLOCKS;
    uint32_t codec = SDF_FOUNTAIN_LT;
    char runner[256];

    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(runner, sizeof(runner), "%s-%s-%s", u.sysname, u.release, u.machine);
    } else {
        snprintf(runner, sizeof(runner), "unknown");
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (i + 1 >= argc) {
            usage(argv[0]);
            return 2;
        }
        const char *value = argv[++i];
        if (strcmp(argv[i - 1], "--k") == 0) {
            if (parse_u32(value, &k) != 0) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i - 1], "--sym-len") == 0) {
            if (parse_u32(value, &sym_len) != 0) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i - 1], "--overhead") == 0) {
            if (parse_u32(value, &overhead) != 0) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i - 1], "--loss") == 0) {
            if (parse_u32(value, &loss) != 0) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i - 1], "--blocks") == 0) {
            if (parse_u32(value, &blocks) != 0) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i - 1], "--runner") == 0) {
            if (strlen(value) >= sizeof(runner)) {
                usage(argv[0]);
                return 2;
            }
            memcpy(runner, value, strlen(value) + 1);
        } else if (strcmp(argv[i - 1], "--codec") == 0) {
            if (strcmp(value, "lt") == 0) {
                codec = SDF_FOUNTAIN_LT;
            } else if (strcmp(value, "nanorq") == 0) {
                codec = SDF_FOUNTAIN_NANORQ;
            } else {
                usage(argv[0]);
                return 2;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (k == 0 || sym_len == 0 || blocks == 0 || loss > 100) {
        usage(argv[0]);
        return 2;
    }
    return run_config(k, sym_len, overhead, loss, blocks, codec, runner) == 0 ? 0 : 1;
}
