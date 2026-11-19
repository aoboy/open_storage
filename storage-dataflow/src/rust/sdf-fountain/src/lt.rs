// Rust LT arm (plan prompt W6.8, ADR-0003).
//
// This is a behavioral port of fe_codec_lt.c + fe_prng.c, not a new codec.
// The C frontends continue to own fe_*/fd_* and nanorq stays in C. These
// exports implement only the internal lt_* contract over caller-owned storage.
// No heap allocation occurs in the codec; all arrays have the C arm's limits.

use crate::{FdCfg, FeCfg};
use std::ffi::c_void;
use std::os::raw::c_int;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::slice;

const LT_MAX_K: usize = 64;
const LT_MAX_SYM: usize = 256;
const EINVAL: c_int = 22; // Linux errno value used by the C ABI.
const PRNG_MULTIPLIER: u64 = 2_685_821_657_736_338_717;
const ESI_MULTIPLIER: u64 = 0x9E37_79B9_7F4A_7C15;

// repr(C), field order and integer widths mirror fe_codec_lt.c exactly. The
// public C frontend asks lt_*_sizeof before allocating its flexible arm.
#[repr(C)]
struct LtEnc {
    k: u32,
    overhead: u32,
    sym_len: usize,
    block: *const u8,
}

#[repr(C)]
struct LtDec {
    k: u32,
    sym_len: usize,
    have_src: [u8; LT_MAX_K],
    src: [[u8; LT_MAX_SYM]; LT_MAX_K],
    have_rep: [u8; LT_MAX_K],
    rep_deg: [u32; LT_MAX_K],
    rep_nb: [[u16; LT_MAX_K]; LT_MAX_K],
    rep: [[u8; LT_MAX_SYM]; LT_MAX_K],
}

// A Rust panic must never cross an extern "C" boundary. The C ABI has no
// error value for "internal panic", so the boundary contract is abort.
fn ffi_boundary<T>(f: impl FnOnce() -> T) -> T {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(value) => value,
        Err(_) => std::process::abort(),
    }
}

fn prng_next(state: &mut u64) -> u64 {
    let mut x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    x.wrapping_mul(PRNG_MULTIPLIER)
}

/// Advance the same xorshift64* stream as fe_prng.c.
///
/// # Safety
/// `state` must be non-null, aligned and valid for reads and writes.
#[no_mangle]
pub unsafe extern "C" fn lt_prng_next(state: *mut u64) -> u64 {
    ffi_boundary(|| prng_next(unsafe { &mut *state }))
}

fn draw_degree(state: &mut u64, k: u32) -> u32 {
    if k < 8 {
        return 1 + (prng_next(state) % u64::from(k)) as u32;
    }

    let c = 0.1_f64;
    let delta = 0.5_f64;
    let r = c * (f64::from(k) / delta).ln() * f64::from(k).sqrt();
    let kr = f64::from(k) / r;
    let k_r = if kr >= 1.0 { kr as u32 } else { 1 };

    let mut rho = [0.0_f64; LT_MAX_K + 1];
    let mut tau = [0.0_f64; LT_MAX_K + 1];
    let mut mu = [0.0_f64; LT_MAX_K + 1];
    let mut z = 0.0_f64;
    for d in 1..=k as usize {
        rho[d] = if d == 1 {
            1.0 / k as f64
        } else {
            1.0 / (d as f64 * (d - 1) as f64)
        };
        if d < k_r as usize {
            tau[d] = r / (d as f64 * k as f64);
        } else if d == k_r as usize {
            tau[d] = r / k as f64 * (r / delta).ln();
        }
        mu[d] = rho[d] + tau[d];
        z += mu[d];
    }

    let u = (prng_next(state) % 1_000_000) as f64 / 1_000_000.0 * z;
    let mut acc = 0.0_f64;
    for (d, &weight) in mu.iter().enumerate().take(k as usize + 1).skip(1) {
        acc += weight;
        if u <= acc {
            return d as u32;
        }
    }
    k // rounding tail
}

fn repair_neighbors(k: u32, r: u32) -> ([u16; LT_MAX_K], u32) {
    let mut state = u64::from(r).wrapping_mul(ESI_MULTIPLIER).wrapping_add(1);
    let degree = draw_degree(&mut state, k);
    let mut picked = 0_usize;
    let mut seen = [0_u8; LT_MAX_K];
    let mut neighbors = [0_u16; LT_MAX_K];
    while picked < degree as usize && picked < k as usize {
        let p = (prng_next(&mut state) % u64::from(k)) as usize;
        if seen[p] != 0 {
            continue;
        }
        seen[p] = 1;
        neighbors[picked] = p as u16;
        picked += 1;
    }
    (neighbors, picked as u32)
}

/// Return the C LT encoder arm's storage size.
#[no_mangle]
pub extern "C" fn lt_enc_sizeof() -> usize {
    std::mem::size_of::<LtEnc>()
}

/// Return the C LT decoder arm's storage size.
#[no_mangle]
pub extern "C" fn lt_dec_sizeof() -> usize {
    std::mem::size_of::<LtDec>()
}

/// Initialize an LT encoder in caller-owned storage.
///
/// # Safety
/// Non-null pointers must satisfy the C fountain ABI: `mem` has
/// `lt_enc_sizeof()` aligned writable bytes and `block` has `cfg.block_len`
/// readable bytes that outlive the encoder.
#[no_mangle]
pub unsafe extern "C" fn lt_enc_init(
    mem: *mut c_void,
    cfg: *const FeCfg,
    block: *const u8,
) -> c_int {
    ffi_boundary(|| {
        if mem.is_null() || cfg.is_null() || block.is_null() {
            return -EINVAL;
        }
        let cfg = unsafe { *cfg };
        if cfg.k == 0
            || cfg.k as usize > LT_MAX_K
            || cfg.block_len == 0
            || cfg.block_len % cfg.k as usize != 0
        {
            return -EINVAL;
        }
        let sym_len = cfg.block_len / cfg.k as usize;
        if sym_len == 0 || sym_len > LT_MAX_SYM {
            return -EINVAL;
        }

        let enc = mem.cast::<LtEnc>();
        unsafe {
            ptr::write_bytes(enc, 0, 1);
            ptr::write(
                enc,
                LtEnc {
                    k: cfg.k,
                    overhead: cfg.k.wrapping_mul(cfg.overhead_pct) / 100,
                    sym_len,
                    block,
                },
            );
        }
        0
    })
}

/// Initialize an LT decoder in caller-owned storage.
///
/// # Safety
/// Non-null `mem` must provide `lt_dec_sizeof()` aligned writable bytes;
/// `cfg` must be readable.
#[no_mangle]
pub unsafe extern "C" fn lt_dec_init(mem: *mut c_void, cfg: *const FdCfg) -> c_int {
    ffi_boundary(|| {
        if mem.is_null() || cfg.is_null() {
            return -EINVAL;
        }
        let cfg = unsafe { *cfg };
        if cfg.k == 0 || cfg.k as usize > LT_MAX_K || cfg.sym_len == 0 || cfg.sym_len > LT_MAX_SYM {
            return -EINVAL;
        }
        let dec = mem.cast::<LtDec>();
        unsafe {
            ptr::write_bytes(dec, 0, 1);
            (*dec).k = cfg.k;
            (*dec).sym_len = cfg.sym_len;
        }
        0
    })
}

fn symbol_count(enc: &LtEnc) -> u32 {
    enc.k.wrapping_add(enc.overhead)
}

/// Return K plus the configured repair overhead, including C uint32 wraparound.
///
/// # Safety
/// Non-null `enc` must point to an initialized LT encoder.
#[no_mangle]
pub unsafe extern "C" fn lt_symbol_count(enc: *const c_void) -> u32 {
    ffi_boundary(|| {
        if enc.is_null() {
            0
        } else {
            symbol_count(unsafe { &*enc.cast::<LtEnc>() })
        }
    })
}

/// Generate one systematic or deterministic repair symbol.
///
/// # Safety
/// Non-null pointers must identify an initialized encoder and an output
/// buffer of exactly `out_len` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn lt_generate(
    enc: *mut c_void,
    esi: u32,
    out: *mut u8,
    out_len: usize,
) -> c_int {
    ffi_boundary(|| {
        if enc.is_null() || out.is_null() {
            return -EINVAL;
        }
        let enc = unsafe { &*enc.cast::<LtEnc>() };
        if out_len != enc.sym_len || esi >= symbol_count(enc) {
            return -EINVAL;
        }
        let out = unsafe { slice::from_raw_parts_mut(out, out_len) };
        if esi < enc.k {
            let source = unsafe {
                slice::from_raw_parts(enc.block.add(esi as usize * enc.sym_len), enc.sym_len)
            };
            out.copy_from_slice(source);
            return 0;
        }

        let (neighbors, degree) = repair_neighbors(enc.k, esi);
        out.fill(0);
        for &neighbor in &neighbors[..degree as usize] {
            let source = unsafe {
                slice::from_raw_parts(enc.block.add(neighbor as usize * enc.sym_len), enc.sym_len)
            };
            for (dst, &src) in out.iter_mut().zip(source) {
                *dst ^= src;
            }
        }
        0
    })
}

/// Accept one source/repair symbol; duplicates return 1 as in the C arm.
///
/// # Safety
/// Non-null pointers must identify an initialized decoder and a readable
/// symbol of exactly `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn lt_push(dec: *mut c_void, esi: u32, sym: *const u8, len: usize) -> c_int {
    ffi_boundary(|| {
        if dec.is_null() || sym.is_null() {
            return -EINVAL;
        }
        let dec = unsafe { &mut *dec.cast::<LtDec>() };
        if len != dec.sym_len || esi >= 2_u32.wrapping_mul(dec.k) {
            return -EINVAL;
        }
        let sym = unsafe { slice::from_raw_parts(sym, len) };
        if esi < dec.k {
            let index = esi as usize;
            if dec.have_src[index] != 0 {
                return 1;
            }
            dec.have_src[index] = 1;
            dec.src[index][..len].copy_from_slice(sym);
            return 0;
        }

        let repair = (esi - dec.k) as usize;
        if dec.have_rep[repair] != 0 {
            return 1;
        }
        dec.have_rep[repair] = 1;
        let (neighbors, degree) = repair_neighbors(dec.k, esi);
        dec.rep_nb[repair] = neighbors;
        dec.rep_deg[repair] = degree;
        dec.rep[repair][..len].copy_from_slice(sym);
        0
    })
}

fn peel(dec: &mut LtDec) {
    let mut progress = true;
    while progress {
        progress = false;
        for repair in 0..dec.k as usize {
            if dec.have_rep[repair] == 0 {
                continue;
            }
            let mut unknown = 0_u32;
            let mut last = 0_u16;
            for i in 0..dec.rep_deg[repair] as usize {
                let neighbor = dec.rep_nb[repair][i];
                if dec.have_src[neighbor as usize] == 0 {
                    unknown += 1;
                    last = neighbor;
                }
            }
            if unknown == 1 {
                let mut acc = [0_u8; LT_MAX_SYM];
                acc[..dec.sym_len].copy_from_slice(&dec.rep[repair][..dec.sym_len]);
                for i in 0..dec.rep_deg[repair] as usize {
                    let neighbor = dec.rep_nb[repair][i];
                    if neighbor != last {
                        for b in 0..dec.sym_len {
                            acc[b] ^= dec.src[neighbor as usize][b];
                        }
                    }
                }
                dec.src[last as usize][..dec.sym_len].copy_from_slice(&acc[..dec.sym_len]);
                dec.have_src[last as usize] = 1;
                progress = true;
            }
        }
    }
}

fn ready_mut(dec: &mut LtDec) -> bool {
    peel(dec);
    dec.have_src[..dec.k as usize].iter().all(|&have| have != 0)
}

fn ready(dec: *const LtDec) -> bool {
    // lt_ready is const at the C seam but peeling intentionally mutates the
    // recovery state. Cast the raw pointer directly, matching the C contract;
    // never manufacture a mutable reference from a shared Rust reference.
    ready_mut(unsafe { &mut *dec.cast_mut() })
}

/// Peel to a fixed point and report whether all K sources are known.
///
/// # Safety
/// Non-null `dec` must point to an initialized, exclusively used LT decoder.
#[no_mangle]
pub unsafe extern "C" fn lt_ready(dec: *const c_void) -> bool {
    ffi_boundary(|| !dec.is_null() && ready(dec.cast::<LtDec>()))
}

/// Count known sources plus stored repairs, without forcing a peel.
///
/// # Safety
/// Non-null `dec` must point to an initialized LT decoder.
#[no_mangle]
pub unsafe extern "C" fn lt_rank(dec: *const c_void) -> u32 {
    ffi_boundary(|| {
        if dec.is_null() {
            return 0;
        }
        let dec = unsafe { &*dec.cast::<LtDec>() };
        let k = dec.k as usize;
        let sources = dec.have_src[..k]
            .iter()
            .map(|&have| u32::from(have != 0))
            .sum::<u32>();
        let repairs = dec.have_rep[..k]
            .iter()
            .map(|&have| u32::from(have != 0))
            .sum::<u32>();
        sources + repairs
    })
}

/// Decode a ready block, or return -EINVAL as the ARQ fallback condition.
///
/// # Safety
/// Non-null pointers must identify an initialized, exclusively used decoder
/// and an output buffer of exactly `block_len` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn lt_decode(dec: *mut c_void, out: *mut u8, block_len: usize) -> c_int {
    ffi_boundary(|| {
        if dec.is_null() || out.is_null() {
            return -EINVAL;
        }
        let dec = unsafe { &mut *dec.cast::<LtDec>() };
        if block_len != dec.k as usize * dec.sym_len || !ready_mut(dec) {
            return -EINVAL;
        }
        for (source, chunk) in dec.src[..dec.k as usize]
            .iter()
            .zip(unsafe { slice::from_raw_parts_mut(out, block_len) }.chunks_mut(dec.sym_len))
        {
            chunk.copy_from_slice(&source[..dec.sym_len]);
        }
        0
    })
}

/// Destroy an LT encoder (no owned resources; storage remains caller-owned).
///
/// # Safety
/// `enc` must be null or a valid encoder that is no longer in use.
#[no_mangle]
pub unsafe extern "C" fn lt_enc_destroy(enc: *mut c_void) {
    let _ = enc;
}

/// Destroy an LT decoder (no owned resources; storage remains caller-owned).
///
/// # Safety
/// `dec` must be null or a valid decoder that is no longer in use.
#[no_mangle]
pub unsafe extern "C" fn lt_dec_destroy(dec: *mut c_void) {
    let _ = dec;
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::MaybeUninit;

    #[test]
    fn prng_matches_the_extracted_c_sequence() {
        // Extracted by calling fe_prng.c @ eb55583 from state 1; not derived
        // from the Rust implementation (ADR-0009's golden-source rule).
        const EXPECTED: [u64; 5] = [
            0x47E4_CE4B_896C_DD1D,
            0xABCF_A6A8_E079_651D,
            0xB9D1_0D8F_EB73_1F57,
            0x4DB4_18A0_BB1B_019D,
            0x0E61_99B0_4D5A_A600,
        ];
        let mut state = 1;
        for expected in EXPECTED {
            assert_eq!(prng_next(&mut state), expected);
        }
    }

    #[test]
    fn ffi_roundtrip_peels_a_lost_source_and_counts_duplicates() {
        const K: u32 = 8;
        const SYM_LEN: usize = 7;
        let block: Vec<u8> = (0..K as usize * SYM_LEN)
            .map(|i| (i as u8).wrapping_mul(37).wrapping_add(11))
            .collect();
        let mut enc = LtEnc {
            k: K,
            overhead: K,
            sym_len: SYM_LEN,
            block: block.as_ptr(),
        };
        let mut dec = Box::new(MaybeUninit::<LtDec>::uninit());
        let cfg = FdCfg {
            k: K,
            sym_len: SYM_LEN,
        };
        assert_eq!(unsafe { lt_dec_init(dec.as_mut_ptr().cast(), &cfg) }, 0);

        for esi in 0..2 * K {
            if esi == 5 {
                continue;
            }
            let mut symbol = vec![0_u8; SYM_LEN];
            assert_eq!(
                unsafe { lt_generate((&raw mut enc).cast(), esi, symbol.as_mut_ptr(), SYM_LEN) },
                0
            );
            assert_eq!(
                unsafe { lt_push(dec.as_mut_ptr().cast(), esi, symbol.as_ptr(), SYM_LEN) },
                0
            );
        }
        assert!(unsafe { lt_ready(dec.as_mut_ptr().cast()) });
        let mut decoded = vec![0_u8; block.len()];
        assert_eq!(
            unsafe { lt_decode(dec.as_mut_ptr().cast(), decoded.as_mut_ptr(), decoded.len(),) },
            0
        );
        assert_eq!(decoded, block);

        let mut source = vec![0_u8; SYM_LEN];
        assert_eq!(
            unsafe { lt_generate((&raw mut enc).cast(), 0, source.as_mut_ptr(), SYM_LEN) },
            0
        );
        assert_eq!(
            unsafe { lt_push(dec.as_mut_ptr().cast(), 0, source.as_ptr(), SYM_LEN) },
            1
        );
        assert_eq!(
            unsafe { lt_push(dec.as_mut_ptr().cast(), 0, source.as_ptr(), SYM_LEN - 1) },
            -EINVAL
        );
    }

    #[test]
    fn invalid_init_shapes_match_the_c_contract() {
        let byte = 0_u8;
        let mut enc = MaybeUninit::<LtEnc>::uninit();
        let cfg = FeCfg {
            block_len: 65,
            k: 64,
            overhead_pct: 0,
            codec: 0,
        };
        assert_eq!(
            unsafe { lt_enc_init(enc.as_mut_ptr().cast(), &cfg, &byte) },
            -EINVAL
        );

        let mut dec = MaybeUninit::<LtDec>::uninit();
        let cfg = FdCfg { k: 65, sym_len: 1 };
        assert_eq!(
            unsafe { lt_dec_init(dec.as_mut_ptr().cast(), &cfg) },
            -EINVAL
        );
        assert_eq!(unsafe { lt_symbol_count(ptr::null()) }, 0);
        assert!(!unsafe { lt_ready(ptr::null()) });
        assert_eq!(unsafe { lt_rank(ptr::null()) }, 0);
    }
}
