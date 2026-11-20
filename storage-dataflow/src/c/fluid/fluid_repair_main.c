/*
 * FLUID repair coordinator (plan prompts W3.5, W6.3, design 02's loop).
 *
 * One repair job over the origin seam. W6.3 generalizes the W3.5
 * single-block loop to an N-block object without touching the fountain
 * contract: the codec still sees ONE block per object, and the
 * coordinator owns one codec object, one budget and one gate per
 * block. Symbols are scheduled by decode-rank deficit (the block
 * furthest from decodable gets the next symbol; ties by block index),
 * so a slow block cannot starve behind a nearly-complete one.
 *
 * The sender's symbol stream is precomputed into the origin files
 * (symbols are deterministic from the ESI — the harness models the
 * sender), the coordinator sprays fetches via wm_select, per-unit
 * completion callbacks deliver symbols directly into their block's
 * decoder through a bounded MPSC-safe handoff, the per-block FLUID
 * budget admits per
 * round and the gate certifies — and when the gate fires before
 * decode sufficiency the mandatory per-block ARQ fallback re-fetches
 * the systematic ESIs with retry (never ship an incomplete block;
 * design 02's safety). The control arm is pure ARQ: every systematic
 * ESI retried until it lands.
 *
 * All origins hold the same stream (sdf_fluid_repair_encode_stream
 * writes it for the harness), so each successful completion carries the
 * symbol bytes directly into the matching block decoder.
 *
 * The byte-exact gate: after decode, every block is compared against
 * the deterministic sender block for its index. `ok` is only 1 when
 * every comparison matches — fd_decode returning 0 proves rank, not
 * content.
 */

#include "sdf/fluid_repair.h"

#include "sdf/common/log.h"
#include "sdf/common/timer.h"
#include "sdf/fountain.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Absolute stream index rides in obj_offset / len: idx = block * count +
 * esi, so the callback recovers both without extra request fields. */
typedef struct repair_block_s repair_block_t;

/*
 * W6.15: HTTP owns one worker per origin, so completions are MPSC. The
 * coordinator no longer reads symbols back from a local file: each callback
 * copies its symbol into the matching decoder while holding one bounded
 * mutex. The per-index landed map makes retries idempotent and the mutex
 * serializes decoder mutation across origin workers.
 */
typedef struct {
    pthread_mutex_t lock;
    repair_block_t *blocks;
    uint8_t *landed;
    uint32_t count;   /* symbols per block */
    uint32_t nblocks; /* blocks in this repair object */
    uint32_t sym_len; /* exact bytes in one symbol */
} repair_ctx_t;

static void repair_cb(void *ud, const sdf_fetch_req_t *req, const uint8_t *out, uint32_t len,
                      int status);

static uint32_t object_blocks(const sdf_fluid_repair_cfg_t *cfg) {
    return cfg->n_blocks ? cfg->n_blocks : 1u;
}

static uint32_t stream_symbols(const sdf_fluid_repair_cfg_t *cfg) {
    if (cfg->arm == SDF_REPAIR_CONTROL || !(cfg->eps > 0.0)) {
        return cfg->k;
    }
    /* the stream IS the budget: exactly N symbols (the eps/(1-eps)
     * percent-of-K mapping undershoots N by rounding — found live:
     * N = 72 needed 8 repairs, floor(k*ov/100) gave 7) */
    return sdf_fluid_budget_n(cfg->k, cfg->eps);
}

/* overhead_pct that exposes exactly (count - k) repair symbols */
static uint32_t stream_overhead_pct(const sdf_fluid_repair_cfg_t *cfg) {
    uint32_t count = stream_symbols(cfg);
    if (count <= cfg->k) {
        return 0;
    }
    uint64_t repairs = (uint64_t)(count - cfg->k);
    return (uint32_t)((100u * repairs + cfg->k - 1u) / cfg->k);
}

/* The sender's deterministic block. Block 0 keeps the W3.5 formula
 * byte-for-byte; later blocks add a per-block salt so a block routed
 * to the wrong decoder cannot pass the byte-exact gate. */
static void fill_expected_block(uint8_t *dst, uint32_t k, uint32_t sym_len, uint32_t block) {
    size_t block_len = (size_t)k * sym_len;
    uint64_t salt = (uint64_t)block * 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < block_len; i++) {
        dst[i] = (uint8_t)(((i * 2654435761ull) + salt) >> 24);
    }
}

uint64_t sdf_fluid_repair_stream_bytes(const sdf_fluid_repair_cfg_t *cfg) {
    if (cfg == NULL || cfg->k == 0 || cfg->sym_len == 0) {
        return 0;
    }
    uint32_t blocks = object_blocks(cfg);
    if (blocks > SDF_REPAIR_MAX_BLOCKS) {
        return 0;
    }
    return (uint64_t)blocks * stream_symbols(cfg) * cfg->sym_len;
}

int sdf_fluid_repair_encode_stream(const sdf_fluid_repair_cfg_t *cfg, uint8_t *buf,
                                   size_t buf_len) {
    if (cfg == NULL || buf == NULL || cfg->k == 0 || cfg->sym_len == 0) {
        return -EINVAL;
    }
    uint32_t blocks = object_blocks(cfg);
    if (blocks > SDF_REPAIR_MAX_BLOCKS) {
        return -EINVAL;
    }
    uint64_t total = sdf_fluid_repair_stream_bytes(cfg);
    if (buf_len < total) {
        return -EINVAL;
    }

    uint32_t count = stream_symbols(cfg);
    uint64_t block_bytes = (uint64_t)count * cfg->sym_len;
    size_t block_len = (size_t)cfg->k * cfg->sym_len;
    uint8_t *block = malloc(block_len);
    if (block == NULL) {
        return -ENOMEM;
    }

    uint32_t codec = cfg->arm == SDF_REPAIR_NANORQ ? SDF_FOUNTAIN_NANORQ : SDF_FOUNTAIN_LT;
    uint32_t overhead_pct = cfg->arm == SDF_REPAIR_CONTROL ? 0u : stream_overhead_pct(cfg);
    fe_cfg_t ecfg = {
        .block_len = block_len,
        .k = cfg->k,
        .overhead_pct = overhead_pct,
        .codec = codec,
    };
    size_t esz = fe_sizeof(&ecfg);
    int rc = 0;

    for (uint32_t b = 0; b < blocks && rc == 0; b++) {
        fill_expected_block(block, cfg->k, cfg->sym_len, b);
        void *enc = esz ? malloc(esz) : NULL;
        if (enc == NULL || fe_init_in(enc, &ecfg, block) != 0) {
            free(enc);
            rc = -EINVAL;
            break;
        }
        for (uint32_t esi = 0; esi < count && rc == 0; esi++) {
            uint8_t *dst = buf + (size_t)b * block_bytes + (size_t)esi * cfg->sym_len;
            rc = fe_generate(enc, esi, dst, cfg->sym_len) == 0 ? 0 : -EINVAL;
        }
        fe_destroy(enc);
        free(enc);
    }
    free(block);
    return rc;
}

/* ---- helpers ------------------------------------------------------------ */

static int wait_quiet(sdf_origin_t *o, uint32_t n) {
    for (int spin = 0; spin < 120000; spin++) {
        uint64_t inflight = 0;
        for (uint32_t i = 0; i < n; i++) {
            sdf_origin_stats_t st;
            if (o->ops->stats(o, i, &st) == 0) {
                inflight += st.in_flight;
            }
        }
        if (inflight == 0) {
            return 0;
        }
        sdf_sleep_ns(200000);
    }
    return -ETIMEDOUT;
}

/* Submit one symbol with the retry loop the control/FLUID arms share.
 * Returns 0 when the origin accepted the fetch (and accounts it). */
static int submit_symbol(sdf_origin_t *o, wm_compiled_t *sel, uint64_t *counter,
                         uint64_t obj_offset, uint32_t len, repair_ctx_t *ctx, uint64_t *sent,
                         uint64_t *bytes_tx) {
    sdf_fetch_req_t req;
    memset(&req, 0, sizeof(req));
    req.origin_id = wm_select(sel, (*counter)++);
    req.obj_offset = obj_offset;
    req.len = len;
    int rc;
    do {
        rc = o->ops->fetch(o, &req, repair_cb, ctx);
        if (rc == -EAGAIN) {
            sdf_sleep_ns(100000);
        }
    } while (rc == -EAGAIN);
    if (rc != 0) {
        return -1;
    }
    (*sent)++;
    *bytes_tx += len;
    return 0;
}

/* ---- per-block coordinator state ---------------------------------------- */

struct repair_block_s {
    /* 8-byte fields first: the budget/gate tails are 8-aligned, so the
     * small fields packed at the end leave the struct padding-free
     * (clang-tidy performance check at the W6.4 gate). */
    uint64_t sent;
    uint64_t received;
    uint64_t bytes_tx;
    uint64_t bytes_rx;
    uint64_t rounds; /* rounds that admitted >= 1 symbol for this block */
    void *dec;
    sdf_fluid_budget_t budget;
    fluid_gate_t gate;
    uint32_t next_esi; /* next stream symbol to admit from this block */
    int arq_fallback;
    int ok;
    uint32_t decode_rank;
};

/* Deliver one successful completion into its block decoder. */
static void repair_cb(void *ud, const sdf_fetch_req_t *req, const uint8_t *out, uint32_t len,
                      int status) {
    repair_ctx_t *ctx = ud;
    if (status != 0 || out == NULL || req == NULL || req->len != ctx->sym_len || ctx->count == 0) {
        return;
    }

    uint64_t index = req->obj_offset / req->len;
    uint64_t total = (uint64_t)ctx->nblocks * ctx->count;
    if (index >= total) {
        return;
    }

    pthread_mutex_lock(&ctx->lock);
    if (!ctx->landed[index]) {
        uint32_t block = (uint32_t)(index / ctx->count);
        uint32_t esi = (uint32_t)(index % ctx->count);
        repair_block_t *bl = &ctx->blocks[block];
        /* Zero means added and one means redundant/already sufficient;
         * both are successful delivery. Only negative returns are errors. */
        if (fd_push(bl->dec, esi, out, len) >= 0) {
            ctx->landed[index] = 1;
            bl->received++;
            bl->bytes_rx += len;
        }
    }
    pthread_mutex_unlock(&ctx->lock);
}

static int repair_symbol_landed(repair_ctx_t *ctx, uint64_t index) {
    pthread_mutex_lock(&ctx->lock);
    int landed = ctx->landed[index] != 0;
    pthread_mutex_unlock(&ctx->lock);
    return landed;
}

static int block_active(const repair_block_t *b, uint32_t count) {
    return !fd_ready(b->dec) && !fg_done(&b->gate) && b->next_esi < count;
}

/* Decode the block and compare against the deterministic sender bytes.
 * `ok` is 1 only when BOTH rank sufficiency and content match hold. */
static int decode_verify(repair_block_t *b, const sdf_fluid_repair_cfg_t *cfg, uint32_t block) {
    size_t block_len = (size_t)cfg->k * cfg->sym_len;
    uint8_t *got = malloc(block_len);
    uint8_t *want = malloc(block_len);
    int ok = 0;
    if (got != NULL && want != NULL && fd_decode(b->dec, got, block_len) == 0) {
        fill_expected_block(want, cfg->k, cfg->sym_len, block);
        ok = memcmp(got, want, block_len) == 0;
    }
    free(got);
    free(want);
    b->decode_rank = fd_rank(b->dec);
    return ok;
}

/* Order the active blocks by decode deficit, descending (the block
 * furthest from decodable first); insertion sort over <= 64 entries. */
static uint32_t rank_active(const repair_block_t *blocks, uint32_t *active, uint32_t nactive,
                            uint32_t k) {
    for (uint32_t i = 1; i < nactive; i++) {
        uint32_t key = active[i];
        uint32_t key_rank = fd_rank(blocks[key].dec);
        uint32_t key_def = key_rank < k ? k - key_rank : 0;
        uint32_t j = i;
        while (j > 0) {
            uint32_t prev = active[j - 1];
            uint32_t prev_rank = fd_rank(blocks[prev].dec);
            uint32_t prev_def = prev_rank < k ? k - prev_rank : 0;
            if (prev_def >= key_def) {
                break;
            }
            active[j] = prev;
            j--;
        }
        active[j] = key;
    }
    return nactive;
}

/* ---- the run ------------------------------------------------------------ */

int sdf_fluid_repair_run(const sdf_fluid_repair_cfg_t *cfg, sdf_fluid_repair_stats_t *out) {
    if (cfg == NULL || out == NULL) {
        return -EINVAL;
    }
    if (cfg->k == 0 || cfg->k > 64u * 1024u || cfg->sym_len == 0 || cfg->sym_len > 8192u) {
        return -EINVAL;
    }
    if (cfg->n_origins == 0 || cfg->n_origins > SDF_REPAIR_MAX_ORIGINS) {
        return -EINVAL;
    }
    for (uint32_t i = 0; i < cfg->n_origins; i++) {
        if (cfg->origins[i].id != i) {
            return -EINVAL;
        }
    }
    uint32_t nblocks = object_blocks(cfg);
    if (nblocks > SDF_REPAIR_MAX_BLOCKS) {
        return -EINVAL;
    }

    memset(out, 0, sizeof(*out));
    out->n_blocks = nblocks;
    out->arm = (uint32_t)cfg->arm;
    out->eps = cfg->arm == SDF_REPAIR_CONTROL ? 0.0 : cfg->eps;
    uint64_t t0 = sdf_now_ns();
    uint32_t count = stream_symbols(cfg);
    uint64_t per_block_bytes = (uint64_t)count * cfg->sym_len;
    double eps = cfg->arm == SDF_REPAIR_CONTROL ? 0.0 : cfg->eps;

    repair_block_t blocks[SDF_REPAIR_MAX_BLOCKS];
    memset(blocks, 0, sizeof(blocks));
    for (uint32_t b = 0; b < nblocks; b++) {
        sdf_fluid_budget_init(&blocks[b].budget, cfg->k, eps);
        fg_init(&blocks[b].gate, eps, blocks[b].budget.n);
    }

    const sdf_origin_ops_t *ops = NULL;
    sdf_origin_select_status_t select_status = SDF_ORIGIN_SELECT_INVALID;
    int select_rc = sdf_origin_select(cfg->origins, cfg->n_origins, &ops, &select_status);
    if (select_rc != 0) {
        SDF_LOGE("fluid repair origin selection failed: %s (first class=%s)",
                 sdf_origin_select_status_name(select_status),
                 sdf_origin_cls_name(cfg->origins[0].cls));
        return select_rc;
    }

    /* Preserve the configured class and destination. The harness loss knob
     * applies only to loopback; a real HTTP origin gets its loss from the
     * network. */
    sdf_origin_cfg_t ocopies[SDF_REPAIR_MAX_ORIGINS];
    for (uint32_t i = 0; i < cfg->n_origins; i++) {
        ocopies[i] = cfg->origins[i];
        if (ocopies[i].cls == SDF_ORIGIN_LOOPBACK) {
            ocopies[i].loss_pct = cfg->loss_pct;
        }
        if (ocopies[i].max_inflight == 0) {
            ocopies[i].max_inflight = 64;
        }
    }

    uint32_t wave_cfg = cfg->wave ? cfg->wave : sdf_fluid_budget_n(cfg->k, eps);
    if (wave_cfg == 0) {
        wave_cfg = 1;
    }
    uint32_t round_budget = wave_cfg;

    repair_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.blocks = blocks;
    ctx.count = count;
    ctx.nblocks = nblocks;
    ctx.sym_len = cfg->sym_len;
    size_t landed_count = (size_t)nblocks * count;
    ctx.landed = calloc(landed_count, sizeof(*ctx.landed));
    if (ctx.landed == NULL) {
        return -ENOMEM;
    }
    int mutex_rc = pthread_mutex_init(&ctx.lock, NULL);
    if (mutex_rc != 0) {
        free(ctx.landed);
        return -mutex_rc;
    }

    sdf_origin_t o = {.ops = ops, .impl = NULL};
    wm_profile_t prof;
    wm_compiled_t sel;
    int origins_open = 0;
    int rc = -EINVAL;

    int open_rc = o.ops->open(&o, ocopies, cfg->n_origins);
    if (open_rc != 0) {
        rc = open_rc;
        goto done;
    }
    origins_open = 1;

    uint32_t codec = cfg->arm == SDF_REPAIR_NANORQ ? SDF_FOUNTAIN_NANORQ : SDF_FOUNTAIN_LT;
    fd_cfg_t dcfg = {.k = cfg->k, .sym_len = cfg->sym_len};
    size_t dsz = fd_sizeof(&dcfg);
    for (uint32_t b = 0; b < nblocks; b++) {
        blocks[b].dec = dsz ? malloc(dsz) : NULL;
        if (blocks[b].dec == NULL || fd_init_codec(blocks[b].dec, &dcfg, codec) != 0) {
            rc = -EINVAL;
            goto done;
        }
    }

    /* equal-weight spray profile */
    memset(&prof, 0, sizeof(prof));
    prof.version = 1;
    prof.m = 1024;
    prof.n = cfg->n_origins;
    for (uint32_t i = 0; i < cfg->n_origins; i++) {
        prof.b[i] = 1024 / cfg->n_origins;
    }
    for (uint32_t i = 0; i < 1024 % cfg->n_origins; i++) {
        prof.b[i]++;
    }
    wm_compile_cfg_t spray = {.method = WM_METHOD_SHUFFLE1, .sa = 333, .sb = 735};
    if (wm_profile_compile_cfg(&prof, &sel, &spray) != 0) {
        goto done;
    }

    uint64_t counter = 0;
    uint64_t total_rounds = 0;

    if (cfg->arm == SDF_REPAIR_CONTROL) {
        /* ---- control arm: plain ARQ with retry, block by block -------- */
        for (uint32_t b = 0; b < nblocks; b++) {
            repair_block_t *bl = &blocks[b];
            for (uint32_t e = 0; e < cfg->k; e++) {
                uint64_t index = (uint64_t)b * count + e;
                int landed = repair_symbol_landed(&ctx, index);
                for (int attempt = 0; attempt < 64 && !landed; attempt++) {
                    uint64_t off = (uint64_t)b * per_block_bytes + (uint64_t)e * cfg->sym_len;
                    if (submit_symbol(&o, &sel, &counter, off, cfg->sym_len, &ctx, &bl->sent,
                                      &bl->bytes_tx) != 0) {
                        continue;
                    }
                    int wait_rc = wait_quiet(&o, cfg->n_origins);
                    if (wait_rc != 0) {
                        /* Decoder reads and final decode require a complete
                         * callback boundary; close drains before cleanup. */
                        rc = wait_rc;
                        goto done;
                    }
                    landed = repair_symbol_landed(&ctx, index);
                }
                if (!landed) { /* pathological loss beyond the retry cap */
                    break;
                }
            }
        }
    } else {
        /* ---- FLUID arms: the design-02 loop, per block ---------------- */
        for (;;) {
            uint32_t active[SDF_REPAIR_MAX_BLOCKS];
            uint32_t nactive = 0;
            for (uint32_t b = 0; b < nblocks; b++) {
                if (block_active(&blocks[b], count)) {
                    active[nactive++] = b;
                }
            }
            if (nactive == 0) {
                break;
            }

            rank_active(blocks, active, nactive, cfg->k);
            total_rounds++;

            /* Distribute one admit round across the ranked blocks,
             * one symbol each per pass: the deficit order decides who
             * gets the scarce budget, the pass keeps every block
             * moving. */
            uint32_t admitted = 0;
            uint8_t admitted_block[SDF_REPAIR_MAX_BLOCKS] = {0};
            int progress = 1;
            while (admitted < round_budget && progress) {
                progress = 0;
                for (uint32_t i = 0; i < nactive && admitted < round_budget; i++) {
                    uint32_t b = active[i];
                    repair_block_t *bl = &blocks[b];
                    if (bl->next_esi >= count) {
                        continue;
                    }
                    if (sdf_fluid_budget_admit(&bl->budget) != 0) {
                        continue;
                    }
                    uint64_t off =
                        (uint64_t)b * per_block_bytes + (uint64_t)bl->next_esi * cfg->sym_len;
                    if (submit_symbol(&o, &sel, &counter, off, cfg->sym_len, &ctx, &bl->sent,
                                      &bl->bytes_tx) == 0) {
                        bl->next_esi++;
                        admitted++;
                        admitted_block[b] = 1;
                        progress = 1;
                    }
                }
            }
            for (uint32_t b = 0; b < nblocks; b++) {
                if (admitted_block[b]) {
                    blocks[b].rounds++;
                }
            }
            if (admitted > 0) {
                sdf_histogram_observe(&out->round_size, admitted);
            }

            int wait_rc = wait_quiet(&o, cfg->n_origins);
            if (wait_rc != 0) {
                rc = wait_rc;
                goto done;
            }

            /* Per-block gate/budget accounting: only blocks that have
             * admitted something participate (a not-yet-scheduled
             * block must not certify on a vacuous 0/0 observation). */
            for (uint32_t b = 0; b < nblocks; b++) {
                repair_block_t *bl = &blocks[b];
                if (bl->sent == 0) {
                    continue;
                }
                fg_observe(&bl->gate, bl->sent, bl->received);
                sdf_fluid_budget_report(&bl->budget, bl->received);
            }
            if (admitted == 0) {
                break;
            }
        }

        /* ---- per-block ARQ fallback: gate fired before sufficiency ----
         * The systematic ESIs re-fetch through the origins with the
         * control arm's retry semantics — never an incomplete block. */
        for (uint32_t b = 0; b < nblocks; b++) {
            repair_block_t *bl = &blocks[b];
            if (fd_ready(bl->dec)) {
                continue;
            }
            bl->arq_fallback = 1;
            for (uint32_t e = 0; e < cfg->k; e++) {
                uint64_t index = (uint64_t)b * count + e;
                int landed = repair_symbol_landed(&ctx, index);
                for (int attempt = 0; attempt < 64 && !landed; attempt++) {
                    uint64_t off = (uint64_t)b * per_block_bytes + (uint64_t)e * cfg->sym_len;
                    if (submit_symbol(&o, &sel, &counter, off, cfg->sym_len, &ctx, &bl->sent,
                                      &bl->bytes_tx) != 0) {
                        continue;
                    }
                    int wait_rc = wait_quiet(&o, cfg->n_origins);
                    if (wait_rc != 0) {
                        /* Decoder reads and final decode require a complete
                         * callback boundary; close drains before cleanup. */
                        rc = wait_rc;
                        goto done;
                    }
                    landed = repair_symbol_landed(&ctx, index);
                }
            }
        }
    }

    /* ---- decode + byte-exact gate per block ------------------------- */
    uint32_t min_rank = UINT32_MAX;
    int all_ok = 1;
    for (uint32_t b = 0; b < nblocks; b++) {
        blocks[b].ok = decode_verify(&blocks[b], cfg, b);
        if (blocks[b].decode_rank < min_rank) {
            min_rank = blocks[b].decode_rank;
        }
        all_ok = all_ok && blocks[b].ok;
    }

    /* ---- aggregate (identical to blocks[0] for N == 1) ------------- */
    for (uint32_t b = 0; b < nblocks; b++) {
        const repair_block_t *bl = &blocks[b];
        out->sent += bl->sent;
        out->received += bl->received;
        out->bytes_tx += bl->bytes_tx;
        out->bytes_rx += bl->bytes_rx;
        out->arq_fallback = out->arq_fallback || bl->arq_fallback;
        out->blocks[b].sent = bl->sent;
        out->blocks[b].received = bl->received;
        out->blocks[b].bytes_tx = bl->bytes_tx;
        out->blocks[b].bytes_rx = bl->bytes_rx;
        out->blocks[b].rounds = bl->rounds;
        out->blocks[b].arq_fallback = bl->arq_fallback;
        out->blocks[b].ok = bl->ok;
        out->blocks[b].decode_rank = bl->decode_rank;
    }
    out->rounds = total_rounds;
    out->ok = all_ok;
    out->decode_rank = min_rank == UINT32_MAX ? 0 : min_rank;
    rc = 0;

done:
    if (origins_open) {
        o.ops->close(&o); /* flush callbacks before releasing their state */
    }
    pthread_mutex_destroy(&ctx.lock);
    free(ctx.landed);
    for (uint32_t b = 0; b < nblocks; b++) {
        if (blocks[b].dec != NULL) {
            fd_destroy(blocks[b].dec);
            free(blocks[b].dec);
        }
    }
    out->wall_ns = sdf_now_ns() - t0;
    sdf_histogram_observe(&out->repair_duration_ns, out->wall_ns);
    return rc;
}

int sdf_fluid_repair_render_metrics(FILE *out, const sdf_fluid_repair_stats_t *stats) {
    if (out == NULL || stats == NULL) {
        return -EINVAL;
    }
    static const char *const arms[] = {"control", "lt", "nanorq"};
    if (stats->arm >= 3) {
        return -EINVAL;
    }
    const char *arm = arms[stats->arm];
    const char *codec = stats->arm == SDF_REPAIR_CONTROL
                            ? "arq"
                            : (stats->arm == SDF_REPAIR_NANORQ ? "nanorq" : "lt");
    char labels[96];
    snprintf(labels, sizeof(labels), "arm=\"%s\",class=\"default\"", arm);

    fprintf(out, "sdf_fluid_repair_bytes_total{%s,kind=\"sent\"} %llu\n", labels,
            (unsigned long long)stats->bytes_tx);
    fprintf(out, "sdf_fluid_repair_bytes_total{%s,kind=\"useful\"} %llu\n", labels,
            (unsigned long long)stats->bytes_rx);
    fprintf(out, "sdf_fluid_rounds_total{%s} %llu\n", labels, (unsigned long long)stats->rounds);
    fprintf(out, "sdf_fluid_arq_fallback_total{%s} %d\n", labels, stats->arq_fallback ? 1 : 0);
    fprintf(out, "sdf_fluid_eps{%s} %.17g\n", labels, stats->eps);
    fprintf(out, "sdf_fluid_decode_rank{%s} %u\n", labels, stats->decode_rank);
    fprintf(out, "sdf_fluid_codec{codec=\"%s\"} 1\n", codec);
    fprintf(out, "sdf_fluid_repair_ok{%s} %d\n", labels, stats->ok ? 1 : 0);

    fprintf(out, "# TYPE sdf_fluid_repair_duration_seconds histogram\n");
    if (sdf_histogram_render(out, "sdf_fluid_repair_duration_seconds", labels,
                             &stats->repair_duration_ns, 1e-9) != 0) {
        return -EINVAL;
    }
    fprintf(out, "# TYPE sdf_fluid_repair_round_size histogram\n");
    if (sdf_histogram_render(out, "sdf_fluid_repair_round_size", labels, &stats->round_size, 1.0) !=
        0) {
        return -EINVAL;
    }
    return 0;
}
