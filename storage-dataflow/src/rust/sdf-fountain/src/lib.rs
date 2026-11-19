// sdf-fountain — the Rust seam skeleton (plan prompt W5.3/W6.5, ADR-0003).
//
// The crate declares the COMPLETE public fountain ABI v2 so cbindgen's
// header diff is a real seam check: every exported typedef, enum,
// constant and function in include/sdf/fountain.h must appear here with
// the same name and ABI shape. `cargo build` runs cbindgen over these
// declarations and emits sdf_fountain_generated.h;
// scripts/test_rust_seam.sh compares the declaration surfaces. An empty
// diff proves the C header and the Rust view agree; any intentional
// delta must ride an ADR that bumps golden_vectors (ADR-0001).
//
// W6.8 implements the internal LT arm in lt.rs. The public fe_*/fd_*
// declarations below remain C-owned definitions; nanorq stays in C too.

#![allow(non_camel_case_types)]
#![deny(unsafe_op_in_unsafe_fn)]

use std::os::raw::c_int;

mod lt;

/// Encoder configuration (one block). Layout mirrors `fe_cfg_t`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FeCfg {
    pub block_len: usize,
    pub k: u32,
    pub overhead_pct: u32,
    pub codec: u32,
}

/// Decoder configuration (one block). Layout mirrors `fd_cfg_t`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FdCfg {
    pub k: u32,
    pub sym_len: usize,
}

/// Codec arms behind one ABI (ADR-0004).
#[repr(C)]
#[derive(Clone, Copy)]
pub enum sdf_fountain_codec_t {
    SDF_FOUNTAIN_LT = 0,
    SDF_FOUNTAIN_NANORQ = 1,
}

// Opaque handles (the C header forward-declares these). The zero-length
// body is a cbindgen device; consumers only ever see the pointer.
#[repr(C)]
pub struct fountain_enc {
    _private: [u8; 0],
}

#[repr(C)]
pub struct fountain_dec {
    _private: [u8; 0],
}

pub type fountain_enc_t = fountain_enc;
pub type fountain_dec_t = fountain_dec;

extern "C" {
    pub fn fe_sizeof(cfg: *const FeCfg) -> usize;
    pub fn fd_sizeof(cfg: *const FdCfg) -> usize;

    pub fn fe_init_in(e: *mut fountain_enc_t, cfg: *const FeCfg, block: *const u8) -> c_int;
    pub fn fd_init_in(d: *mut fountain_dec_t, cfg: *const FdCfg) -> c_int;
    pub fn fe_init(
        e: *mut fountain_enc_t,
        block: *const u8,
        block_len: usize,
        k: u32,
        overhead_pct: u32,
    ) -> c_int;
    pub fn fd_init(d: *mut fountain_dec_t, k: u32, sym_len: usize) -> c_int;

    pub fn fe_symbol_count(e: *const fountain_enc_t) -> u32;
    pub fn fe_generate(e: *mut fountain_enc_t, esi: u32, out: *mut u8, out_len: usize) -> c_int;
    pub fn fd_push(d: *mut fountain_dec_t, esi: u32, sym: *const u8, len: usize) -> c_int;
    pub fn fd_ready(d: *const fountain_dec_t) -> bool;
    pub fn fd_init_codec(d: *mut fountain_dec_t, cfg: *const FdCfg, codec: u32) -> c_int;
    pub fn fd_rank(d: *const fountain_dec_t) -> u32;
    pub fn fd_decode(d: *mut fountain_dec_t, out: *mut u8, block_len: usize) -> c_int;

    pub fn fe_destroy(e: *mut fountain_enc_t);
    pub fn fd_destroy(d: *mut fountain_dec_t);
}

#[cfg(test)]
mod tests {
    // The seam test proper is the header diff (scripts/test_rust_seam.sh);
    // this unit test just pins the enum values to the C header's.
    #[test]
    fn codec_enums_match_the_c_header() {
        assert_eq!(super::sdf_fountain_codec_t::SDF_FOUNTAIN_LT as i32, 0);
        assert_eq!(super::sdf_fountain_codec_t::SDF_FOUNTAIN_NANORQ as i32, 1);
    }
}
