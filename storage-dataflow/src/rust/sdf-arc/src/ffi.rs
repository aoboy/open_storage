//! Public ARC ABI v2 declaration surface (W6.18).
//!
//! This module mirrors `include/sdf/arc.h` exactly. The functions below are
//! C-owned definitions: the crate only declares them so cbindgen can emit the
//! Rust view and `scripts/test_rust_arc.sh` can diff the declaration surfaces.
//! Implementing them in Rust is a later slice; the C pacer remains the
//! default until then.
//!
//! `arc_sidecar_render_metrics` takes `FILE *`, so `FILE` is declared here as
//! an opaque type that cbindgen re-exports without a definition (the C header
//! gets it from <stdio.h>).

#![allow(non_camel_case_types, non_snake_case)]

use std::os::raw::{c_char, c_double, c_int, c_void};

/// Release-queue slots for the shadow-record ring (mirrors the C #define).
pub const ARC_SHADOW_RECORD_CAP: u32 = 256;

/// Opaque stdio handle; cbindgen emits only the forward declaration.
#[repr(C)]
pub struct FILE {
    _private: [u8; 0],
}

#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum arc_mode_t {
    ARC_MODE_BASE = 0,
    ARC_MODE_Q = 1,
    ARC_MODE_QG = 2,
    ARC_MODE_IMMEDIATE = 3,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct arc_cfg_t {
    pub capacity: u32,
    pub entry_max: u32,
    pub mode: arc_mode_t,
    pub rho_u: c_double,
    pub rho_l: c_double,
    pub lambda_u: c_double,
    pub lambda_l: c_double,
    pub U: u64,
    pub delta: u64,
    pub gamma: u64,
    pub J: u64,
    pub G: u64,
    pub T_idle_ns: u64,
    pub auto_params: c_int,
    pub c_U: c_double,
    pub c_delta: c_double,
    pub rtt_sm_ns: u64,
    pub min_smooth_bytes: u64,
    pub budget_ns: u64,
    pub batch_max: u32,
    pub batch_deadline_ns: u64,
}

pub type arc_apply_fn = Option<
    unsafe extern "C" fn(
        ctx: *mut c_void,
        entry: *const c_void,
        len: usize,
        release_ts: u64,
    ) -> c_int,
>;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct arc_stats_t {
    pub D: c_double,
    pub E: c_double,
    pub releases: u64,
    pub in_flight: u64,
    pub heap_depth: u32,
    pub idle_reanchors: u64,
    pub guard_waits: u64,
    pub guard_timeouts: u64,
    pub s_n_rejects: u64,
    pub delta_clamps: u64,
    pub budget_alerts: u64,
    pub added_count: u64,
    pub added_p50_ns: c_double,
    pub added_p95_ns: c_double,
    pub added_p99_ns: c_double,
    pub interval_error_max_ns: u64,
}

#[repr(C)]
pub struct arc_pacer {
    _private: [u8; 0],
}

pub type arc_pacer_t = arc_pacer;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct arc_shadow_record_t {
    pub block_n: u64,
    pub seq: u64,
    pub S_n: u64,
    pub A_n: u64,
    pub T_n: u64,
    pub release_ns: u64,
    pub len: u32,
    pub released: u32,
    pub immediate_applied: u32,
    pub reserved: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct arc_shadow_stats_t {
    pub epoch: u64,
    pub schedule_releases: u64,
    pub immediate_applies: u64,
    pub schedule_samples: u64,
    pub budget_alerts: u64,
    pub last_schedule_release_ns: u64,
    pub last_immediate_apply_ns: u64,
    pub scheduled_backlog: u64,
    pub record_total: u64,
    pub record_dropped: u64,
    pub added_p50_ns: c_double,
    pub added_p95_ns: c_double,
    pub added_p99_ns: c_double,
    pub record_count: u32,
    pub active: u32,
    pub schedule_mode: u32,
    pub reserved: u32,
}

extern "C" {
    pub fn arc_sizeof(cfg: *const arc_cfg_t) -> usize;
    pub fn arc_init(
        p: *mut arc_pacer_t,
        cfg: *const arc_cfg_t,
        fn_: arc_apply_fn,
        ctx: *mut c_void,
    ) -> c_int;
    pub fn arc_submit(
        p: *mut arc_pacer_t,
        entry: *const c_void,
        len: usize,
        producer_ts: u64,
    ) -> c_int;
    pub fn arc_poll(p: *mut arc_pacer_t, now_ns: u64) -> c_int;
    pub fn arc_stats(p: *const arc_pacer_t) -> arc_stats_t;
    pub fn arc_set_rtt(p: *mut arc_pacer_t, rtt_sm_ns: u64);
    pub fn arc_set_mode(p: *mut arc_pacer_t, mode: arc_mode_t) -> c_int;

    pub fn arc_sidecar_start(cfg: *const arc_cfg_t, mode: c_int) -> c_int;
    pub fn arc_sidecar_submit(entry: *const c_void, len: usize, producer_ts: u64) -> c_int;
    pub fn arc_sidecar_pump_once() -> c_int;
    pub fn arc_sidecar_stop();
    pub fn arc_sidecar_stats() -> arc_stats_t;
    pub fn arc_sidecar_applied() -> u64;
    pub fn arc_sidecar_mode() -> c_int;
    pub fn arc_sidecar_set_mode(mode: c_int) -> c_int;
    pub fn arc_sidecar_shadow_stats(out: *mut arc_shadow_stats_t) -> c_int;
    pub fn arc_sidecar_shadow_records(out: *mut arc_shadow_record_t, max: u32) -> u32;
    pub fn arc_sidecar_serve(addr: *const c_char) -> c_int;
    pub fn arc_sidecar_serve_stop();
    pub fn arc_sidecar_render_metrics(out: *mut FILE) -> c_int;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mode_values_match_the_c_header() {
        assert_eq!(arc_mode_t::ARC_MODE_BASE as i32, 0);
        assert_eq!(arc_mode_t::ARC_MODE_Q as i32, 1);
        assert_eq!(arc_mode_t::ARC_MODE_QG as i32, 2);
        assert_eq!(arc_mode_t::ARC_MODE_IMMEDIATE as i32, 3);
    }

    #[test]
    fn shadow_record_cap_matches_the_c_header() {
        assert_eq!(ARC_SHADOW_RECORD_CAP, 256);
    }
}
