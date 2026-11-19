// sdf-fountain — the Rust seam skeleton (plan prompt W5.3, ADR-0003).
//
// NO functional Rust yet: this crate exists to validate the C ABI seam
// per ADR-0003's migration order (fountain first). `cargo build` runs
// cbindgen over the extern declarations below and emits the header that
// `scripts/test_rust_seam.sh` diffs against `include/sdf/fountain.h`.
// An empty diff proves the C header and the Rust view of it agree; any
// intentional delta must ride an ADR that bumps golden_vectors
// (ADR-0001's process). The build.rs invokes cbindgen with
// bindings/cbindgen.toml.

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FeCfg {
    pub block_len: usize,
    pub k: u32,
    pub overhead_pct: u32,
    pub codec: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FdCfg {
    pub k: u32,
    pub sym_len: usize,
}

pub const SDF_FOUNTAIN_LT: u32 = 0;
pub const SDF_FOUNTAIN_NANORQ: u32 = 1;

// The admissible surface ADR-0003 names (order: fountain first). Only
// the pure-compute entry points the Rust module would reimplement are
// declared; telemetry/lifecycle stay C-owned until a module ships.
extern "C" {
    pub fn fe_sizeof(cfg: *const FeCfg) -> usize;
    pub fn fd_sizeof(cfg: *const FdCfg) -> usize;
    pub fn fe_symbol_count(e: *const std::ffi::c_void) -> u32;
    pub fn fe_generate(
        e: *mut std::ffi::c_void,
        esi: u32,
        out: *mut u8,
        out_len: usize,
    ) -> i32;
    pub fn fd_push(
        d: *mut std::ffi::c_void,
        esi: u32,
        sym: *const u8,
        len: usize,
    ) -> i32;
    pub fn fd_ready(d: *const std::ffi::c_void) -> bool;
    pub fn fd_rank(d: *const std::ffi::c_void) -> u32;
    pub fn fd_decode(d: *mut std::ffi::c_void, out: *mut u8, block_len: usize) -> i32;
}

#[cfg(test)]
mod tests {
    // The seam test proper is the header diff (scripts/test_rust_seam.sh);
    // this unit test just pins the enum values to the C header's.
    #[test]
    fn codec_enums_match_the_c_header() {
        assert_eq!(super::SDF_FOUNTAIN_LT, 0);
        assert_eq!(super::SDF_FOUNTAIN_NANORQ, 1);
    }
}
