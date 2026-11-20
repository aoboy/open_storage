#ifndef SDF_GENERATED_H
#define SDF_GENERATED_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * Release-queue slots for the shadow-record ring (mirrors the C #define).
 */
#define ARC_SHADOW_RECORD_CAP 256

typedef enum {
  ARC_MODE_BASE = 0,
  ARC_MODE_Q = 1,
  ARC_MODE_QG = 2,
  ARC_MODE_IMMEDIATE = 3,
} arc_mode_t;

typedef struct {
  uint32_t capacity;
  uint32_t entry_max;
  arc_mode_t mode;
  double rho_u;
  double rho_l;
  double lambda_u;
  double lambda_l;
  uint64_t U;
  uint64_t delta;
  uint64_t gamma;
  uint64_t J;
  uint64_t G;
  uint64_t T_idle_ns;
  int auto_params;
  double c_U;
  double c_delta;
  uint64_t rtt_sm_ns;
  uint64_t min_smooth_bytes;
  uint64_t budget_ns;
  uint32_t batch_max;
  uint64_t batch_deadline_ns;
} arc_cfg_t;

typedef struct {
  uint8_t _private[0];
} arc_pacer;

typedef arc_pacer arc_pacer_t;

typedef int (*arc_apply_fn)(void *ctx, const void *entry, size_t len, uint64_t release_ts);

typedef struct {
  double D;
  double E;
  uint64_t releases;
  uint64_t in_flight;
  uint32_t heap_depth;
  uint64_t idle_reanchors;
  uint64_t guard_waits;
  uint64_t guard_timeouts;
  uint64_t s_n_rejects;
  uint64_t delta_clamps;
  uint64_t budget_alerts;
  uint64_t added_count;
  double added_p50_ns;
  double added_p95_ns;
  double added_p99_ns;
  uint64_t interval_error_max_ns;
} arc_stats_t;

typedef struct {
  uint64_t epoch;
  uint64_t schedule_releases;
  uint64_t immediate_applies;
  uint64_t schedule_samples;
  uint64_t budget_alerts;
  uint64_t last_schedule_release_ns;
  uint64_t last_immediate_apply_ns;
  uint64_t scheduled_backlog;
  uint64_t record_total;
  uint64_t record_dropped;
  double added_p50_ns;
  double added_p95_ns;
  double added_p99_ns;
  uint32_t record_count;
  uint32_t active;
  uint32_t schedule_mode;
  uint32_t reserved;
} arc_shadow_stats_t;

typedef struct {
  uint64_t block_n;
  uint64_t seq;
  uint64_t S_n;
  uint64_t A_n;
  uint64_t T_n;
  uint64_t release_ns;
  uint32_t len;
  uint32_t released;
  uint32_t immediate_applied;
  uint32_t reserved;
} arc_shadow_record_t;

extern size_t arc_sizeof(const arc_cfg_t *cfg);

extern int arc_init(arc_pacer_t *p, const arc_cfg_t *cfg, arc_apply_fn fn_, void *ctx);

extern int arc_submit(arc_pacer_t *p, const void *entry, size_t len, uint64_t producer_ts);

extern int arc_poll(arc_pacer_t *p, uint64_t now_ns);

extern arc_stats_t arc_stats(const arc_pacer_t *p);

extern void arc_set_rtt(arc_pacer_t *p, uint64_t rtt_sm_ns);

extern int arc_set_mode(arc_pacer_t *p, arc_mode_t mode);

extern int arc_sidecar_start(const arc_cfg_t *cfg, int mode);

extern int arc_sidecar_submit(const void *entry, size_t len, uint64_t producer_ts);

extern int arc_sidecar_pump_once(void);

extern void arc_sidecar_stop(void);

extern arc_stats_t arc_sidecar_stats(void);

extern uint64_t arc_sidecar_applied(void);

extern int arc_sidecar_mode(void);

extern int arc_sidecar_set_mode(int mode);

extern int arc_sidecar_shadow_stats(arc_shadow_stats_t *out);

extern uint32_t arc_sidecar_shadow_records(arc_shadow_record_t *out, uint32_t max);

extern int arc_sidecar_serve(const char *addr);

extern void arc_sidecar_serve_stop(void);

extern int arc_sidecar_render_metrics(FILE *out);

#endif  /* SDF_GENERATED_H */
