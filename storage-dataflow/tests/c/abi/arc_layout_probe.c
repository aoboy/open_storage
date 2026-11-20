/*
 * Compiled twice by scripts/test_rust_arc.sh with ARC_HEADER pointing at the
 * hand-written C header and at the cbindgen-generated header. ARC_SUFFIX
 * gives each copy distinct symbols; arc_layout_check.c compares them.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include ARC_HEADER
#include "arc_layout.h"

#define ARC_CAT_(a, b) a##b
#define ARC_CAT(a, b) ARC_CAT_(a, b)
#define ARC_SYM(name) ARC_CAT(name, ARC_SUFFIX)

#define ARC_FIELD(type, field) {"field", #type "." #field, sizeof(type), offsetof(type, field)}
#define ARC_ENUM(name) {"enum", #name, 0, (size_t)(name)}
#define ARC_DEFINE(name) {"define", #name, 0, (size_t)(name)}

const arc_layout_entry ARC_SYM(arc_layout_entries)[] = {
    ARC_FIELD(arc_cfg_t, capacity),
    ARC_FIELD(arc_cfg_t, entry_max),
    ARC_FIELD(arc_cfg_t, mode),
    ARC_FIELD(arc_cfg_t, rho_u),
    ARC_FIELD(arc_cfg_t, rho_l),
    ARC_FIELD(arc_cfg_t, lambda_u),
    ARC_FIELD(arc_cfg_t, lambda_l),
    ARC_FIELD(arc_cfg_t, U),
    ARC_FIELD(arc_cfg_t, delta),
    ARC_FIELD(arc_cfg_t, gamma),
    ARC_FIELD(arc_cfg_t, J),
    ARC_FIELD(arc_cfg_t, G),
    ARC_FIELD(arc_cfg_t, T_idle_ns),
    ARC_FIELD(arc_cfg_t, auto_params),
    ARC_FIELD(arc_cfg_t, c_U),
    ARC_FIELD(arc_cfg_t, c_delta),
    ARC_FIELD(arc_cfg_t, rtt_sm_ns),
    ARC_FIELD(arc_cfg_t, min_smooth_bytes),
    ARC_FIELD(arc_cfg_t, budget_ns),
    ARC_FIELD(arc_cfg_t, batch_max),
    ARC_FIELD(arc_cfg_t, batch_deadline_ns),
    ARC_FIELD(arc_stats_t, D),
    ARC_FIELD(arc_stats_t, E),
    ARC_FIELD(arc_stats_t, releases),
    ARC_FIELD(arc_stats_t, in_flight),
    ARC_FIELD(arc_stats_t, heap_depth),
    ARC_FIELD(arc_stats_t, idle_reanchors),
    ARC_FIELD(arc_stats_t, guard_waits),
    ARC_FIELD(arc_stats_t, guard_timeouts),
    ARC_FIELD(arc_stats_t, s_n_rejects),
    ARC_FIELD(arc_stats_t, delta_clamps),
    ARC_FIELD(arc_stats_t, budget_alerts),
    ARC_FIELD(arc_stats_t, added_count),
    ARC_FIELD(arc_stats_t, added_p50_ns),
    ARC_FIELD(arc_stats_t, added_p95_ns),
    ARC_FIELD(arc_stats_t, added_p99_ns),
    ARC_FIELD(arc_stats_t, interval_error_max_ns),
    ARC_FIELD(arc_shadow_record_t, block_n),
    ARC_FIELD(arc_shadow_record_t, seq),
    ARC_FIELD(arc_shadow_record_t, S_n),
    ARC_FIELD(arc_shadow_record_t, A_n),
    ARC_FIELD(arc_shadow_record_t, T_n),
    ARC_FIELD(arc_shadow_record_t, release_ns),
    ARC_FIELD(arc_shadow_record_t, len),
    ARC_FIELD(arc_shadow_record_t, released),
    ARC_FIELD(arc_shadow_record_t, immediate_applied),
    ARC_FIELD(arc_shadow_record_t, reserved),
    ARC_FIELD(arc_shadow_stats_t, epoch),
    ARC_FIELD(arc_shadow_stats_t, schedule_releases),
    ARC_FIELD(arc_shadow_stats_t, immediate_applies),
    ARC_FIELD(arc_shadow_stats_t, schedule_samples),
    ARC_FIELD(arc_shadow_stats_t, budget_alerts),
    ARC_FIELD(arc_shadow_stats_t, last_schedule_release_ns),
    ARC_FIELD(arc_shadow_stats_t, last_immediate_apply_ns),
    ARC_FIELD(arc_shadow_stats_t, scheduled_backlog),
    ARC_FIELD(arc_shadow_stats_t, record_total),
    ARC_FIELD(arc_shadow_stats_t, record_dropped),
    ARC_FIELD(arc_shadow_stats_t, added_p50_ns),
    ARC_FIELD(arc_shadow_stats_t, added_p95_ns),
    ARC_FIELD(arc_shadow_stats_t, added_p99_ns),
    ARC_FIELD(arc_shadow_stats_t, record_count),
    ARC_FIELD(arc_shadow_stats_t, active),
    ARC_FIELD(arc_shadow_stats_t, schedule_mode),
    ARC_FIELD(arc_shadow_stats_t, reserved),
    ARC_ENUM(ARC_MODE_BASE),
    ARC_ENUM(ARC_MODE_Q),
    ARC_ENUM(ARC_MODE_QG),
    ARC_ENUM(ARC_MODE_IMMEDIATE),
    ARC_DEFINE(ARC_SHADOW_RECORD_CAP),
};

const size_t ARC_SYM(arc_layout_count) = sizeof(ARC_SYM(arc_layout_entries)) /
                                         sizeof(ARC_SYM(arc_layout_entries)[0]);

const size_t ARC_SYM(arc_type_sizes)[] = {
    sizeof(arc_cfg_t),          sizeof(arc_stats_t),  sizeof(arc_shadow_record_t),
    sizeof(arc_shadow_stats_t), sizeof(arc_apply_fn), sizeof(arc_mode_t),
};

const size_t ARC_SYM(arc_type_size_count) = sizeof(ARC_SYM(arc_type_sizes)) /
                                            sizeof(ARC_SYM(arc_type_sizes)[0]);
