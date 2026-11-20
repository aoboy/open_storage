/*
 * Cache-fill proxy core (plan prompt W2.5, design 01 / 09).
 *
 * The P1 runtime in miniature: job admission (an object split into
 * byte-range units), seeded WM dispatch per job (ADR-0007 — two jobs
 * with equal profiles never emit correlated sequences), loopback
 * origins through the ADR-0008 seam, the C severity controller
 * (W2.2) sampled at control_hz and applied in the dispatch loop
 * (single-threaded profile swap: the dispatcher is the only writer),
 * and A/B bucketing per design 01 (treatment = seeded spray when
 * bucket < rollout_pct; control = round-robin; kill_switch forces
 * control). Metrics per design 01 §Metrics through the W0.5
 * primitives.
 *
 * Conventions imported from wack_a_mole: per-path exit tallies,
 * discrepancy-minded accounting, best-of nothing (this is the
 * workload, not a bench).
 */

#include "wm_internal.h"

#include "sdf/wm_proxy.h"

#include "sdf/common/log.h"
#include "sdf/common/metrics.h"
#include "sdf/common/timer.h"
#include "sdf/origin.h"
#include "sdf/wm_shm.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- xxhash64 (design 01 pins bucket = xxhash64(key) % 100) ------------ */

#define XXH_PRIME64_1 0x9E3779B185EBCA87ull
#define XXH_PRIME64_2 0xC2B2AE3D27D4EB4Full
#define XXH_PRIME64_5 0x165667B19E3779F9ull

static uint64_t xxh_round(uint64_t acc, uint64_t input) {
    acc += input * XXH_PRIME64_2;
    acc = (acc << 31) | (acc >> 33);
    acc *= XXH_PRIME64_1;
    return acc;
}

/* One-shot xxhash64 over a small key (job id in our case), seed 0. */
static uint64_t xxhash64(const void *data, size_t len) {
    const uint8_t *p = data;
    const uint8_t *end = p + len;
    uint64_t h64;
    if (len >= 32) {
        uint64_t v1 = XXH_PRIME64_1 + XXH_PRIME64_2;
        uint64_t v2 = XXH_PRIME64_2;
        uint64_t v3 = XXH_PRIME64_1;
        uint64_t v4 = XXH_PRIME64_5 - 1;
        do {
            uint64_t k;
            memcpy(&k, p, 8);
            v1 = xxh_round(v1, k);
            p += 8;
            memcpy(&k, p, 8);
            v2 = xxh_round(v2, k);
            p += 8;
            memcpy(&k, p, 8);
            v3 = xxh_round(v3, k);
            p += 8;
            memcpy(&k, p, 8);
            v4 = xxh_round(v4, k);
            p += 8;
        } while (p + 32 <= end);
        h64 = ((v1 << 1) | (v1 >> 63)) + ((v2 << 7) | (v2 >> 57)) + ((v3 << 12) | (v3 >> 52)) +
              ((v4 << 18) | (v4 >> 46));
        v1 *= XXH_PRIME64_2;
        v1 = (v1 << 31) | (v1 >> 33);
        v1 *= XXH_PRIME64_1;
        h64 ^= v1;
        h64 = h64 * XXH_PRIME64_1 + XXH_PRIME64_5;
        /* (compact one-shot: remaining tail handled below like <32) */
        v2 *= XXH_PRIME64_2;
        v2 = (v2 << 31) | (v2 >> 33);
        v2 *= XXH_PRIME64_1;
        h64 ^= v2;
        h64 = h64 * XXH_PRIME64_1 + XXH_PRIME64_5;
    } else {
        h64 = XXH_PRIME64_5;
    }
    h64 += (uint64_t)len;
    while (p + 8 <= end) {
        uint64_t k;
        memcpy(&k, p, 8);
        p += 8;
        k = xxh_round(0, k);
        h64 ^= k;
        h64 = (h64 << 27) | (h64 >> 37);
        h64 = h64 * XXH_PRIME64_1 + XXH_PRIME64_5;
    }
    if (p + 4 <= end) {
        uint32_t k32;
        memcpy(&k32, p, 4);
        p += 4;
        h64 ^= (uint64_t)k32 * XXH_PRIME64_1;
        h64 = (h64 << 23) | (h64 >> 41);
        h64 = h64 * XXH_PRIME64_2 + XXH_PRIME64_5 - 1;
    }
    while (p < end) {
        h64 ^= (*p++) * XXH_PRIME64_5;
        h64 = (h64 << 11) | (h64 >> 53);
        h64 *= XXH_PRIME64_1;
    }
    h64 ^= h64 >> 33;
    h64 *= XXH_PRIME64_2;
    h64 ^= h64 >> 29;
    h64 *= XXH_PRIME64_5 - 1;
    h64 ^= h64 >> 32;
    return h64;
}

/* ---- run state ---------------------------------------------------------- */

#define PROXY_LATENCY_SLOTS 4096u

typedef struct {
    _Atomic uint32_t used;
    uint64_t start_ns;
    uint32_t arm;
} proxy_latency_slot_t;

typedef struct {
    _Atomic uint64_t ok[WM_MAX_PATHS];
    _Atomic uint64_t failed[WM_MAX_PATHS];
    _Atomic uint64_t bytes_rx;
    sdf_wm_proxy_stats_t *stats;
    proxy_latency_slot_t latency[PROXY_LATENCY_SLOTS];
} proxy_cb_state_t;

/* The origin seam copies the complete request, including its padding word.
 * The proxy uses that word as a private slot cookie; no public layout or
 * driver contract changes, and callback lookup is O(1) without a mutex. */
static int proxy_latency_acquire(proxy_cb_state_t *st, uint64_t cursor, uint32_t arm,
                                 sdf_fetch_req_t *req) {
    for (uint32_t n = 0; n < PROXY_LATENCY_SLOTS; n++) {
        uint32_t idx = (uint32_t)((cursor + n) % PROXY_LATENCY_SLOTS);
        uint32_t expected = 0;
        if (!atomic_compare_exchange_weak_explicit(&st->latency[idx].used, &expected, 1,
                                                   memory_order_acquire, memory_order_relaxed)) {
            continue;
        }
        st->latency[idx].start_ns = sdf_now_ns();
        st->latency[idx].arm = arm;
        atomic_store_explicit(&st->latency[idx].used, 2, memory_order_release);
        req->_pad = idx + 1;
        return 0;
    }
    return -EAGAIN;
}

static void proxy_latency_release(proxy_cb_state_t *st, uint32_t cookie) {
    if (cookie == 0 || cookie > PROXY_LATENCY_SLOTS) {
        return;
    }
    atomic_store_explicit(&st->latency[cookie - 1].used, 0, memory_order_release);
}

static void proxy_cb(void *ud, const sdf_fetch_req_t *req, const uint8_t *out, uint32_t len,
                     int status) {
    (void)out;
    proxy_cb_state_t *st = ud;
    if (status == 0) {
        atomic_fetch_add_explicit(&st->ok[req->origin_id], 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&st->bytes_rx, len, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&st->failed[req->origin_id], 1, memory_order_relaxed);
    }

    uint32_t cookie = req->_pad;
    if (cookie > 0 && cookie <= PROXY_LATENCY_SLOTS) {
        proxy_latency_slot_t *slot = &st->latency[cookie - 1];
        if (atomic_load_explicit(&slot->used, memory_order_acquire) == 2) {
            uint64_t now = sdf_now_ns();
            uint64_t elapsed = now > slot->start_ns ? now - slot->start_ns : 0;
            uint32_t arm = slot->arm < 2 ? slot->arm : 0;
            sdf_histogram_observe(&st->stats->fill_latency_by_arm[arm], elapsed);
            if (status == 0) {
                atomic_fetch_add_explicit(&st->stats->fill_bytes_by_arm[arm], len,
                                          memory_order_relaxed);
            }
            atomic_store_explicit(&slot->used, 0, memory_order_release);
        }
    }
}

static int timeline_validate(const sdf_wm_proxy_cfg_t *cfg) {
    for (uint32_t s = 0; s < cfg->n_steps && s < SDF_WM_PROXY_MAX_STEPS; s++) {
        const sdf_wm_proxy_step_t *st = &cfg->steps[s];
        if (st->origin >= cfg->n_origins) {
            return -EINVAL;
        }
        if (st->throttle_ns != 0 && cfg->origins[st->origin].cls != SDF_ORIGIN_LOOPBACK) {
            return -ENOTSUP;
        }
    }
    return 0;
}

static int timeline_apply(const sdf_wm_proxy_cfg_t *cfg, sdf_origin_t *o,
                          uint64_t dispatched_before, uint64_t dispatched_now) {
    for (uint32_t s = 0; s < cfg->n_steps && s < SDF_WM_PROXY_MAX_STEPS; s++) {
        const sdf_wm_proxy_step_t *st = &cfg->steps[s];
        if (st->at_unit > dispatched_before && st->at_unit <= dispatched_now) {
            if (st->throttle_ns != 0) {
                int rc = sdf_origin_set_harness_throttle_ns(o, st->origin, st->throttle_ns);
                if (rc != 0) {
                    return rc;
                }
            }
            int rc = o->ops->set_enable(o, st->origin, st->enable);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return 0;
}

int sdf_wm_proxy_run(const sdf_wm_proxy_cfg_t *cfg, sdf_wm_proxy_stats_t *out) {
    if (cfg == NULL || out == NULL) {
        return -EINVAL;
    }
    if (cfg->n_origins == 0 || cfg->n_origins > WM_MAX_PATHS) {
        return -EINVAL;
    }
    /* validated once, then frozen: every later use (including the
     * control modulo) sees the same non-zero count */
    const uint32_t n_origins = cfg->n_origins;
    if (cfg->units == 0 || cfg->unit_bytes == 0 || cfg->unit_bytes > SDF_ORIGIN_UNIT_MAX) {
        return -EINVAL;
    }
    if (cfg->profile.n != n_origins) {
        return -EINVAL;
    }

    memset(out, 0, sizeof(*out));
    out->n_origins = n_origins;
    out->kill_switch = cfg->kill_switch ? 1u : 0u;

    const sdf_origin_ops_t *ops = NULL;
    sdf_origin_select_status_t select_status = SDF_ORIGIN_SELECT_INVALID;
    int select_rc = sdf_origin_select(cfg->origins, n_origins, &ops, &select_status);
    if (select_rc != 0) {
        SDF_LOGE("WM proxy origin selection failed: %s (first class=%s)",
                 sdf_origin_select_status_name(select_status),
                 sdf_origin_cls_name(cfg->origins[0].cls));
        return select_rc;
    }
    int timeline_rc = timeline_validate(cfg);
    if (timeline_rc != 0) {
        return timeline_rc;
    }

    sdf_origin_t o = {.ops = ops, .impl = NULL};
    int open_rc = o.ops->open(&o, cfg->origins, n_origins);
    if (open_rc != 0) {
        return open_rc;
    }

    proxy_cb_state_t cbst;
    memset(&cbst, 0, sizeof(cbst));
    cbst.stats = out;

    /* base profile -> working compiled form (treatment arm) */
    wm_profile_t work = cfg->profile;
    work.version = 1;
    wm_compile_cfg_t spray = cfg->spray;
    wm_compiled_t compiled;
    if (wm_profile_compile_cfg(&work, &compiled, &spray) != 0) {
        o.ops->close(&o);
        return -EINVAL;
    }

    /* W6.1: the live chain — external loop authoritative when set.
     * Attach-or-create: the standalone proxy owns the segment's
     * lifecycle (creates it), while a supervisor or harness may
     * pre-create and pre-publish a profile before the run starts (the
     * in-process chain leg) — both flows are production-sane. A bad
     * name fails both ways and aborts the run with the API's own
     * errno (NOT a post-close -errno: close() can clobber it). */
    sdf_shm_t shm;
    int shm_on = 0;
    if (cfg->shm_name != NULL && cfg->shm_name[0] != '\0') {
        int rc = sdf_wm_shm_create(cfg->shm_name, n_origins, &shm);
        if (rc != 0) {
            rc = sdf_wm_shm_attach(cfg->shm_name, n_origins, &shm);
        }
        if (rc != 0) {
            o.ops->close(&o);
            return rc;
        }
        shm_on = 1;
    }

    /* controller (the C fast path; recovery stays with Python) */
    wm_ctrl_cfg_t kcfg;
    wm_ctrl_cfg_default(&kcfg);
    kcfg.alpha_lo = cfg->alpha > 0.0 ? cfg->alpha : kcfg.alpha_lo;
    kcfg.t_min_ns = cfg->control_hz > 0 ? 1000000000ull / cfg->control_hz : kcfg.t_min_ns;
    wm_ctrl_t ctl;
    if (wm_ctrl_init(&ctl, &work, &kcfg, n_origins) != 0) {
        if (shm_on) {
            sdf_wm_shm_close(&shm);
        }
        o.ops->close(&o);
        return -EINVAL;
    }

    uint64_t units_per_interval = cfg->control_hz > 0 ? cfg->units / cfg->control_hz : cfg->units;
    if (units_per_interval == 0) {
        units_per_interval = cfg->units;
    }

    /* per-job units: job k owns the range [k*per, (k+1)*per) of units */
    uint64_t per = cfg->units / (cfg->jobs ? cfg->jobs : 1);
    if (per == 0) {
        per = 1;
    }

    uint64_t t0 = sdf_now_ns();
    uint64_t dispatched = 0;
    uint64_t last_ctrl_at = 0;
    uint64_t control_rr = 0; /* control-arm round-robin cursor */

    /* job-local compiled table: rebuilt when the job changes OR the
     * controller updated the base profile (version bump) — one compile
     * per job, never per unit */
    wm_compiled_t job_compiled;
    uint64_t job_cached = UINT64_MAX;
    uint32_t base_ver_cached = work.version;

    while (dispatched < cfg->units) {
        /* ---- control tick: sample stats, maybe recompile ---------- */
        if (dispatched - last_ctrl_at >= units_per_interval) {
            last_ctrl_at = dispatched;
            out->control_ticks++;
            for (uint32_t i = 0; i < n_origins; i++) {
                sdf_origin_stats_t st;
                if (o.ops->stats(&o, i, &st) != 0) {
                    continue;
                }
                if (shm_on) {
                    /* the live chain: telemetry to the Python loop */
                    if (sdf_wm_shm_telemetry_push(&shm, &st) == 0) {
                        out->shm_telemetry_pushed++;
                    }
                }
                wm_ctrl_on_stats(&ctl, i, &st);
            }
            if (shm_on) {
                /* the external loop is authoritative (design 01): a
                 * version advance swaps the working profile; the
                 * in-process controller stays fed but never publishes */
                wm_profile_t next;
                uint32_t ver = 0;
                if (sdf_wm_shm_acquire(&shm, &next, &ver) == 0) {
                    out->shm_acquires++;
                    if (ver > out->shm_version_acquired && next.n == n_origins &&
                        next.m == work.m) {
                        out->shm_version_acquired = ver;
                        if (wm_profile_compile_cfg(&next, &compiled, &spray) == 0) {
                            work = next;
                        }
                    }
                }
            } else {
                wm_profile_t next;
                if (wm_ctrl_tick(&ctl, sdf_now_ns(), &next) == 1) {
                    /* the dispatcher is the only compiled writer: swap */
                    if (wm_profile_compile_cfg(&next, &compiled, &spray) == 0) {
                        work = next;
                        out->updates_applied++;
                        for (uint32_t i = 0; i < n_origins; i++) {
                            sdf_counter_inc(&out->shed_events[i]);
                        }
                    }
                }
            }
        }

        uint64_t job = dispatched / per;
        uint64_t job_unit = dispatched % per;

        /* A/B assignment: deterministic bucket over the job key */
        uint64_t key[1] = {job};
        uint32_t bucket = (uint32_t)(xxhash64(key, sizeof(key)) % 100ull);
        int treatment = !cfg->kill_switch && bucket < cfg->rollout_pct;

        uint32_t origin;
        if (treatment) {
            if (job != job_cached || work.version != base_ver_cached) {
                job_cached = job;
                base_ver_cached = work.version;
                /* per-job seeds: rotate the base pair per job so two
                 * jobs with the same profile never correlate
                 * (ADR-0007) */
                wm_compile_cfg_t jcfg = spray;
                for (uint64_t r = 0; r < job % 64u; r++) {
                    wm_seed_rotate(&jcfg, work.m);
                }
                if (wm_profile_compile_cfg(&work, &job_compiled, &jcfg) != 0) {
                    job_compiled = compiled; /* fallback: base table */
                }
            }
            origin = wm_select(&job_compiled, job_unit);
            out->dispatch_treatment++;
        } else {
            origin = (uint32_t)(control_rr++ % n_origins);
            out->dispatch_control++;
        }

        sdf_fetch_req_t req;
        memset(&req, 0, sizeof(req));
        req.origin_id = origin;
        req.job_id = job;
        req.obj_offset =
            (dispatched * cfg->unit_bytes) % (SDF_WM_PROXY_OBJ_BYTES - cfg->unit_bytes);
        req.len = cfg->unit_bytes;

        uint64_t before = dispatched;
        uint32_t arm = treatment ? 1u : 0u;
        int rc;
        do {
            rc = proxy_latency_acquire(&cbst, dispatched, arm, &req);
            if (rc == 0) {
                rc = o.ops->fetch(&o, &req, proxy_cb, &cbst);
            }
            if (rc == -EAGAIN) {
                proxy_latency_release(&cbst, req._pad);
                sdf_sleep_ns(100000);
            }
        } while (rc == -EAGAIN);
        if (rc != 0) {
            proxy_latency_release(&cbst, req._pad);
            /* -ENOLINK lands here: the drain contract keeps counts */
            sdf_counter_inc(&out->enolink[origin]);
        }
        dispatched++;
        if (cfg->dispatch_pace_ns > 0) {
            sdf_sleep_ns(cfg->dispatch_pace_ns);
        }
        out->dispatched[origin]++;
        out->dispatch_by_arm[arm][origin]++;
        int step_rc = timeline_apply(cfg, &o, before, dispatched);
        if (step_rc != 0) {
            if (shm_on) {
                sdf_wm_shm_close(&shm);
            }
            o.ops->close(&o);
            return step_rc;
        }
    }

    /* Drain before publishing final statistics. A slow transport must not
     * produce a successful run with completions still outstanding. */
    int quiet = 0;
    for (int iter = 0; iter < 60000; iter++) {
        uint64_t inflight = 0;
        for (uint32_t i = 0; i < n_origins; i++) {
            sdf_origin_stats_t st;
            if (o.ops->stats(&o, i, &st) == 0) {
                inflight += st.in_flight;
            }
        }
        if (inflight == 0) {
            quiet = 1;
            break;
        }
        sdf_sleep_ns(200000);
    }

    if (shm_on) {
        sdf_wm_shm_close(&shm); /* close does not unlink: the harness
                                 * owns the segment lifecycle */
    }
    o.ops->close(&o); /* also establishes a completion/callback boundary */

    out->wall_ns = sdf_now_ns() - t0;
    for (uint32_t i = 0; i < n_origins; i++) {
        out->ok[i] = atomic_load_explicit(&cbst.ok[i], memory_order_relaxed);
        out->failed[i] = atomic_load_explicit(&cbst.failed[i], memory_order_relaxed);
    }
    out->bytes_rx = atomic_load_explicit(&cbst.bytes_rx, memory_order_relaxed);
    out->end_profile_m = work.m;
    for (uint32_t i = 0; i < n_origins; i++) {
        out->end_profile_b[i] = work.b[i];
    }
    return quiet ? 0 : -ETIMEDOUT;
}

int sdf_wm_proxy_render_metrics(FILE *out, const sdf_wm_proxy_stats_t *stats) {
    if (out == NULL || stats == NULL) {
        return -EINVAL;
    }
    static const char *const arms[2] = {"control", "treatment"};
    uint32_t n_paths = stats->n_origins < WM_MAX_PATHS ? stats->n_origins : WM_MAX_PATHS;
    uint64_t total_dispatch = stats->dispatch_treatment + stats->dispatch_control;

    /* W6.1 aliases retained for existing scrape/runbook consumers. */
    fprintf(out, "sdf_proxy_units_total %llu\n", (unsigned long long)total_dispatch);
    fprintf(out, "sdf_proxy_control_heartbeat_ticks %llu\n",
            (unsigned long long)stats->control_ticks);
    fprintf(out, "sdf_proxy_shm_profile_version %u\n", stats->shm_version_acquired);
    fprintf(out, "sdf_proxy_shm_telemetry_pushed %llu\n",
            (unsigned long long)stats->shm_telemetry_pushed);
    fprintf(out, "sdf_wm_kill_switch %u\n", stats->kill_switch ? 1u : 0u);
    for (uint32_t i = 0; i < n_paths; i++) {
        fprintf(out, "sdf_proxy_origin_dispatched{origin=\"%u\"} %llu\n", i,
                (unsigned long long)stats->dispatched[i]);
    }

    for (uint32_t arm = 0; arm < 2; arm++) {
        for (uint32_t path = 0; path < n_paths; path++) {
            fprintf(out, "sdf_wm_dispatch_total{path=\"%u\",arm=\"%s\"} %llu\n", path, arms[arm],
                    (unsigned long long)stats->dispatch_by_arm[arm][path]);
        }
        uint64_t bytes = atomic_load_explicit(&stats->fill_bytes_by_arm[arm], memory_order_relaxed);
        fprintf(out, "sdf_wm_fill_bytes_total{arm=\"%s\"} %llu\n", arms[arm],
                (unsigned long long)bytes);
    }
    for (uint32_t path = 0; path < n_paths; path++) {
        double discrepancy = 0.0;
        if (stats->end_profile_m != 0) {
            double expected = (double)total_dispatch * (double)stats->end_profile_b[path] /
                              (double)stats->end_profile_m;
            discrepancy = (double)stats->dispatched[path] - expected;
        }
        fprintf(out, "sdf_wm_discrepancy{path=\"%u\"} %.17g\n", path, discrepancy);
        fprintf(out, "sdf_wm_shed_events_total{path=\"%u\"} %llu\n", path,
                (unsigned long long)sdf_counter_read(&stats->shed_events[path]));
        fprintf(out, "sdf_wm_enolink_total{path=\"%u\"} %llu\n", path,
                (unsigned long long)sdf_counter_read(&stats->enolink[path]));
    }

    fprintf(out, "# TYPE sdf_wm_fill_duration_seconds histogram\n");
    for (uint32_t arm = 0; arm < 2; arm++) {
        char labels[32];
        snprintf(labels, sizeof(labels), "arm=\"%s\"", arms[arm]);
        if (sdf_histogram_render(out, "sdf_wm_fill_duration_seconds", labels,
                                 &stats->fill_latency_by_arm[arm], 1e-9) != 0) {
            return -EINVAL;
        }
    }
    return 0;
}
