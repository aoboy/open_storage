/*
 * FLUID repair coordinator (plan prompt W3.5, design 02's loop).
 *
 * One repair job over the origin seam: the sender's symbol stream is
 * precomputed into the origin files (symbols are deterministic from
 * the ESI — the harness models the sender), the coordinator sprays
 * fetches via wm_select, per-unit completion events carry the ESI
 * through a W0.2 SPSC ring back to the coordinator, landed symbols
 * are re-read from the stream and fed to the decoder, the FLUID
 * budget admits per wave and the gate certifies — and when the gate
 * fires before decode sufficiency the mandatory ARQ fallback
 * re-fetches the missing systematic ESIs with retry (never ship an
 * incomplete block; design 02's safety). The control arm is pure ARQ:
 * every systematic ESI retried until it lands.
 *
 * All origin files hold the same stream (sdf_fluid_repair_encode_stream
 * writes it; the harness copies it to each origin), so a landed unit
 * re-reads from any of them by ESI offset.
 */

#include "sdf/fluid_repair.h"

#include "sdf/common/ring.h"
#include "sdf/common/timer.h"
#include "sdf/fountain.h"
#include "sdf/origin/loopback.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ESI rides in obj_offset (the stream position); the cb only accounts. */
typedef struct {
    uint32_t esi;
    uint32_t landed; /* 1 when the symbol arrived */
} repair_event_t;

typedef struct {
    spsc_ring_t *ring;
    uint8_t *mem;
} repair_ctx_t;

static void repair_cb(void *ud, const sdf_fetch_req_t *req, const uint8_t *out, uint32_t len,
                      int status) {
    (void)out;
    (void)len;
    repair_ctx_t *ctx = ud;
    repair_event_t ev = {
        .esi = (uint32_t)(req->obj_offset / req->len),
        .landed = status == 0 ? 1u : 0u,
    };
    (void)spsc_try_push(ctx->ring, &ev);
}

static uint32_t stream_symbols(const sdf_fluid_repair_cfg_t *cfg) {
    if (cfg->arm == SDF_REPAIR_CONTROL || !(cfg->eps > 0.0))
        return cfg->k;
    /* the stream IS the budget: exactly N symbols (the eps/(1-eps)
     * percent-of-K mapping undershoots N by rounding — found live:
     * N = 72 needed 8 repairs, floor(k*ov/100) gave 7) */
    return sdf_fluid_budget_n(cfg->k, cfg->eps);
}

/* overhead_pct that exposes exactly (count - k) repair symbols */
static uint32_t stream_overhead_pct(const sdf_fluid_repair_cfg_t *cfg) {
    uint32_t count = stream_symbols(cfg);
    if (count <= cfg->k)
        return 0;
    uint64_t repairs = (uint64_t)(count - cfg->k);
    return (uint32_t)((100u * repairs + cfg->k - 1u) / cfg->k);
}

uint64_t sdf_fluid_repair_stream_bytes(const sdf_fluid_repair_cfg_t *cfg) {
    if (cfg == NULL || cfg->k == 0 || cfg->sym_len == 0)
        return 0;
    return (uint64_t)stream_symbols(cfg) * cfg->sym_len;
}

int sdf_fluid_repair_encode_stream(const sdf_fluid_repair_cfg_t *cfg, uint8_t *buf,
                                   size_t buf_len) {
    if (cfg == NULL || buf == NULL || buf_len < sdf_fluid_repair_stream_bytes(cfg))
        return -EINVAL;

    size_t block_len = (size_t)cfg->k * cfg->sym_len;
    uint8_t *block = malloc(block_len);
    if (block == NULL)
        return -ENOMEM;
    for (size_t i = 0; i < block_len; i++)
        block[i] = (uint8_t)((i * 2654435761ull) >> 24);

    uint32_t codec = cfg->arm == SDF_REPAIR_NANORQ ? SDF_FOUNTAIN_NANORQ : SDF_FOUNTAIN_LT;
    uint32_t overhead_pct = cfg->arm == SDF_REPAIR_CONTROL ? 0u : stream_overhead_pct(cfg);
    fe_cfg_t ecfg = {
        .block_len = block_len,
        .k = cfg->k,
        .overhead_pct = overhead_pct,
        .codec = codec,
    };
    size_t esz = fe_sizeof(&ecfg);
    void *enc = esz ? malloc(esz) : NULL;
    if (enc == NULL || fe_init_in(enc, &ecfg, block) != 0) {
        free(enc);
        free(block);
        return -EINVAL;
    }
    int rc = 0;
    for (uint32_t esi = 0; esi < stream_symbols(cfg) && rc == 0; esi++)
        rc = fe_generate(enc, esi, buf + (size_t)esi * cfg->sym_len, cfg->sym_len) == 0 ? 0
                                                                                        : -EINVAL;
    fe_destroy(enc);
    free(enc);
    free(block);
    return rc;
}

/* ---- helpers ------------------------------------------------------------ */

static int wait_quiet(sdf_origin_t *o, uint32_t n) {
    for (int spin = 0; spin < 120000; spin++) {
        uint64_t inflight = 0;
        for (uint32_t i = 0; i < n; i++) {
            sdf_origin_stats_t st;
            if (o->ops->stats(o, i, &st) == 0)
                inflight += st.in_flight;
        }
        if (inflight == 0)
            return 0;
        sdf_sleep_ns(200000);
    }
    return -ETIMEDOUT;
}

/* Feed one landed ESI to the decoder from the stream file. */
static int feed_symbol(void *dec, FILE *stream, uint32_t esi, const sdf_fluid_repair_cfg_t *cfg,
                       uint64_t *rx_bytes) {
    uint8_t sym[8192];
    if (cfg->sym_len > sizeof(sym))
        return -EINVAL;
    if (fseek(stream, (long)((uint64_t)esi * cfg->sym_len), SEEK_SET) != 0 ||
        fread(sym, 1, cfg->sym_len, stream) != cfg->sym_len)
        return -EIO;
    *rx_bytes += cfg->sym_len;
    (void)fd_push(dec, esi, sym, cfg->sym_len); /* duplicates counted */
    return 0;
}

/* ---- the run ------------------------------------------------------------ */

int sdf_fluid_repair_run(const sdf_fluid_repair_cfg_t *cfg, sdf_fluid_repair_stats_t *out) {
    if (cfg == NULL || out == NULL)
        return -EINVAL;
    if (cfg->k == 0 || cfg->k > 64u * 1024u || cfg->sym_len == 0 || cfg->sym_len > 8192u)
        return -EINVAL;
    if (cfg->n_origins == 0 || cfg->n_origins > SDF_REPAIR_MAX_ORIGINS)
        return -EINVAL;
    for (uint32_t i = 0; i < cfg->n_origins; i++)
        if (cfg->origins[i].id != i)
            return -EINVAL;

    memset(out, 0, sizeof(*out));
    uint64_t t0 = sdf_now_ns();
    uint32_t count = stream_symbols(cfg);

    /* origins with the injected loss */
    sdf_origin_t o = {.ops = sdf_loopback_ops(), .impl = NULL};
    sdf_origin_cfg_t ocopies[SDF_REPAIR_MAX_ORIGINS];
    for (uint32_t i = 0; i < cfg->n_origins; i++) {
        ocopies[i] = cfg->origins[i];
        ocopies[i].cls = SDF_ORIGIN_LOOPBACK;
        ocopies[i].loss_pct = cfg->loss_pct;
        ocopies[i].max_inflight = 64;
    }
    if (o.ops->open(&o, ocopies, cfg->n_origins) != 0)
        return -EINVAL;

    /* decoder */
    uint32_t codec = cfg->arm == SDF_REPAIR_NANORQ ? SDF_FOUNTAIN_NANORQ : SDF_FOUNTAIN_LT;
    fd_cfg_t dcfg = {.k = cfg->k, .sym_len = cfg->sym_len};
    size_t dsz = fd_sizeof(&dcfg);
    void *dec = dsz ? malloc(dsz) : NULL;
    if (dec == NULL || fd_init_codec(dec, &dcfg, codec) != 0) {
        free(dec);
        o.ops->close(&o);
        return -EINVAL;
    }

    /* completion ring (cb -> coordinator) */
    repair_ctx_t ctx;
    size_t qsz = spsc_sizeof(256, (uint32_t)sizeof(repair_event_t));
    ctx.mem = qsz ? aligned_alloc(64, (qsz + 63u) & ~63u) : NULL;
    ctx.ring = (spsc_ring_t *)ctx.mem;
    if (ctx.mem == NULL ||
        spsc_init(ctx.ring, ctx.mem, 256, (uint32_t)sizeof(repair_event_t)) != 0) {
        free(ctx.mem);
        fd_destroy(dec);
        free(dec);
        o.ops->close(&o);
        return -EINVAL;
    }

    /* stream readback (all origin files hold the same stream) */
    FILE *stream = fopen(ocopies[0].dst, "rb");
    if (stream == NULL) {
        free(ctx.mem);
        fd_destroy(dec);
        free(dec);
        o.ops->close(&o);
        return -EINVAL;
    }

    /* equal-weight spray profile */
    wm_profile_t prof;
    memset(&prof, 0, sizeof(prof));
    prof.version = 1;
    prof.m = 1024;
    prof.n = cfg->n_origins;
    for (uint32_t i = 0; i < cfg->n_origins; i++)
        prof.b[i] = 1024 / cfg->n_origins;
    for (uint32_t i = 0; i < 1024 % cfg->n_origins; i++)
        prof.b[i]++;
    wm_compile_cfg_t spray = {.method = WM_METHOD_SHUFFLE1, .sa = 333, .sb = 735};
    wm_compiled_t sel;
    if (wm_profile_compile_cfg(&prof, &sel, &spray) != 0) {
        fclose(stream);
        free(ctx.mem);
        fd_destroy(dec);
        free(dec);
        o.ops->close(&o);
        return -EINVAL;
    }

    sdf_fluid_budget_t budget;
    double eps = cfg->arm == SDF_REPAIR_CONTROL ? 0.0 : cfg->eps;
    sdf_fluid_budget_init(&budget, cfg->k, eps);
    fluid_gate_t gate;
    fg_init(&gate, eps, budget.n);

    uint64_t counter = 0;
    uint32_t esi = 0;

    if (cfg->arm == SDF_REPAIR_CONTROL) {
        /* ---- control arm: plain ARQ with retry ---------------------- */
        uint8_t *sym = malloc(cfg->sym_len);
        for (uint32_t e = 0; e < cfg->k && sym != NULL; e++) {
            int landed = 0;
            for (int attempt = 0; attempt < 64 && !landed; attempt++) {
                sdf_fetch_req_t req;
                memset(&req, 0, sizeof(req));
                req.origin_id = wm_select(&sel, counter++);
                req.obj_offset = (uint64_t)e * cfg->sym_len;
                req.len = cfg->sym_len;
                int rc;
                do {
                    rc = o.ops->fetch(&o, &req, repair_cb, &ctx);
                    if (rc == -EAGAIN)
                        sdf_sleep_ns(100000);
                } while (rc == -EAGAIN);
                if (rc != 0)
                    continue;
                out->sent++;
                out->bytes_tx += cfg->sym_len;
                if (wait_quiet(&o, cfg->n_origins) == 0) {
                    repair_event_t ev;
                    while (spsc_try_pop(ctx.ring, &ev))
                        if (ev.landed && ev.esi == e)
                            landed = 1;
                }
            }
            if (!landed) {
                free(sym);
                out->ok = 0; /* pathological loss beyond the retry cap */
                goto done;
            }
            if (fseek(stream, (long)((uint64_t)e * cfg->sym_len), SEEK_SET) != 0 ||
                fread(sym, 1, cfg->sym_len, stream) != cfg->sym_len ||
                fd_push(dec, e, sym, cfg->sym_len) != 0) {
                free(sym);
                goto done;
            }
            out->received++;
            out->bytes_rx += cfg->sym_len;
        }
        free(sym);
    } else {
        /* ---- FLUID arms: the design-02 loop -------------------------- */
        while (!fd_ready(dec) && !fg_done(&gate) && esi < count) {
            fg_round_start(&gate);
            uint32_t admitted = 0;
            /* a round is the whole budget in flight (the wam blast
             * model) — small waves certify trivially early because the
             * cumulative-loss quotient is always within slack at the
             * start, and the fallback ends up doing the real work */
            uint32_t wave = cfg->wave ? cfg->wave : budget.n;
            while (admitted < wave && esi < count && sdf_fluid_budget_admit(&budget) == 0) {
                sdf_fetch_req_t req;
                memset(&req, 0, sizeof(req));
                req.origin_id = wm_select(&sel, counter++);
                req.obj_offset = (uint64_t)esi * cfg->sym_len;
                req.len = cfg->sym_len;
                int rc;
                do {
                    rc = o.ops->fetch(&o, &req, repair_cb, &ctx);
                    if (rc == -EAGAIN)
                        sdf_sleep_ns(100000);
                } while (rc == -EAGAIN);
                if (rc == 0) {
                    out->sent++;
                    out->bytes_tx += cfg->sym_len;
                    esi++;
                    admitted++;
                }
            }
            out->rounds++;

            if (wait_quiet(&o, cfg->n_origins) != 0)
                break;

            repair_event_t ev;
            while (spsc_try_pop(ctx.ring, &ev)) {
                if (ev.landed && feed_symbol(dec, stream, ev.esi, cfg, &out->bytes_rx) == 0)
                    out->received++;
            }
            fg_observe(&gate, out->sent, out->received);
            sdf_fluid_budget_report(&budget, out->received);
        }

        /* ---- ARQ fallback: gate fired before sufficiency -------------
         * The missing systematic ESIs re-fetch through the origins with
         * the control arm's retry semantics — never an incomplete block. */
        if (!fd_ready(dec)) {
            out->arq_fallback = 1;
            for (uint32_t e = 0; e < cfg->k; e++) {
                int landed = 0;
                for (int attempt = 0; attempt < 64 && !landed; attempt++) {
                    sdf_fetch_req_t req;
                    memset(&req, 0, sizeof(req));
                    req.origin_id = wm_select(&sel, counter++);
                    req.obj_offset = (uint64_t)e * cfg->sym_len;
                    req.len = cfg->sym_len;
                    int rc;
                    do {
                        rc = o.ops->fetch(&o, &req, repair_cb, &ctx);
                        if (rc == -EAGAIN)
                            sdf_sleep_ns(100000);
                    } while (rc == -EAGAIN);
                    if (rc != 0)
                        continue;
                    out->sent++;
                    out->bytes_tx += cfg->sym_len;
                    if (wait_quiet(&o, cfg->n_origins) == 0) {
                        repair_event_t ev;
                        while (spsc_try_pop(ctx.ring, &ev))
                            if (ev.landed && ev.esi == e)
                                landed = 1;
                    }
                }
                if (landed && feed_symbol(dec, stream, e, cfg, &out->bytes_rx) == 0)
                    out->received++;
            }
        }
    }

done:
    out->decode_rank = fd_rank(dec);
    {
        uint8_t *out_block = malloc((size_t)cfg->k * cfg->sym_len);
        if (out_block != NULL) {
            out->ok = fd_decode(dec, out_block, (size_t)cfg->k * cfg->sym_len) == 0;
            free(out_block);
        }
    }
    out->wall_ns = sdf_now_ns() - t0;

    fclose(stream);
    free(ctx.mem);
    fd_destroy(dec);
    free(dec);
    o.ops->close(&o);
    return 0;
}
