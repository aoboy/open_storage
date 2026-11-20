//! ARC offset estimator (W6.18 first slice).
//!
//! This mirrors the Algorithm-1 update and Algorithm-2 E hysteresis from the
//! C pacer. It is not yet wired into the public ABI; the C pacer remains the
//! default and owns heap/order/callback behavior until the remaining W6.18
//! slices land.

#![forbid(unsafe_code)]

pub mod ffi;
pub mod heap;
pub mod pacer;
pub mod scheduler;
pub mod sidecar;

pub(crate) const ARC_SANE_NS: u64 = 10_000_000_000;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum ArcMode {
    Base,
    Q,
    QG,
    Immediate,
}

#[derive(Clone, Copy, Debug)]
pub struct ArcParams {
    pub rho_u: f64,
    pub rho_l: f64,
    pub lambda_u: f64,
    pub lambda_l: f64,
    pub u_ns: u64,
    pub delta_ns: u64,
    pub gamma_ns: u64,
    pub j_ns: u64,
    pub g_ns: u64,
    pub t_idle_ns: u64,
    pub mode: ArcMode,
    pub min_smooth_bytes: usize,
}

impl Default for ArcParams {
    fn default() -> Self {
        Self {
            rho_u: 0.5,
            rho_l: 1.5,
            lambda_u: 0.05,
            lambda_l: 0.03,
            u_ns: 100_000_000,
            delta_ns: 50_000_000,
            gamma_ns: 16_666_667,
            j_ns: 0,
            g_ns: 50_000_000,
            t_idle_ns: 1_000_000_000,
            mode: ArcMode::QG,
            min_smooth_bytes: 0,
        }
    }
}

impl ArcParams {
    fn validate(&self) -> Result<(), &'static str> {
        if !(0.0..=1.0).contains(&self.rho_u) || !(0.0..=2.0).contains(&self.rho_l) {
            return Err("rho out of range");
        }
        if !(0.0..=1.0).contains(&self.lambda_u) || !(0.0..=0.1).contains(&self.lambda_l) {
            return Err("lambda out of range");
        }
        if self.u_ns == 0 {
            return Err("U must be positive");
        }
        Ok(())
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Schedule {
    pub block_n: u64,
    pub s_n: u64,
    pub a_n: u64,
    pub d: f64,
    pub e: f64,
    pub t_n: u64,
}

pub struct OffsetEstimator {
    params: ArcParams,
    d: f64,
    e: f64,
    tau_last_ns: u64,
    initialized: bool,
    block_n: u64,
    idle_reanchors: u64,
    s_n_rejects: u64,
}

impl OffsetEstimator {
    pub fn new(params: ArcParams) -> Result<Self, &'static str> {
        params.validate()?;
        Ok(Self {
            params,
            d: 0.0,
            e: 0.0,
            tau_last_ns: 0,
            initialized: false,
            block_n: 0,
            idle_reanchors: 0,
            s_n_rejects: 0,
        })
    }

    pub fn d(&self) -> f64 {
        self.d
    }

    pub fn e(&self) -> f64 {
        self.e
    }

    pub fn idle_reanchors(&self) -> u64 {
        self.idle_reanchors
    }

    pub fn s_n_rejects(&self) -> u64 {
        self.s_n_rejects
    }

    pub fn on_arrival(&mut self, s_n: u64, a_n: u64, len: usize) -> Schedule {
        let p = self.params;
        let block_n = self.block_n;
        self.block_n += 1;

        if p.mode == ArcMode::Immediate {
            return Schedule {
                block_n,
                s_n,
                a_n,
                d: self.d,
                e: self.e,
                t_n: a_n,
            };
        }
        if len < p.min_smooth_bytes {
            return Schedule {
                block_n,
                s_n,
                a_n,
                d: self.d,
                e: self.e,
                t_n: a_n,
            };
        }
        if a_n < s_n && s_n - a_n > ARC_SANE_NS {
            self.s_n_rejects += 1;
            return Schedule {
                block_n,
                s_n,
                a_n,
                d: self.d,
                e: self.e,
                t_n: a_n,
            };
        }

        if !self.initialized {
            self.d = (a_n.saturating_sub(s_n)) as f64;
            self.e = self.d + p.gamma_ns as f64 / 2.0;
            self.tau_last_ns = a_n;
            self.initialized = true;
        } else if a_n >= self.tau_last_ns && a_n - self.tau_last_ns >= p.t_idle_ns {
            self.d = (a_n.saturating_sub(s_n)) as f64;
            self.e = self.d + p.gamma_ns as f64 / 2.0;
            self.tau_last_ns = a_n;
            self.idle_reanchors += 1;
        } else {
            self.tau_last_ns = a_n;
        }

        let x = a_n as f64 - (s_n as f64 + self.d);
        let u = p.u_ns as f64;
        if x > 0.0 {
            let clipped = x.clamp(0.0, u);
            let y = (clipped / u).powf(p.rho_u) * u;
            self.d += p.lambda_u * y;
        } else if x + p.j_ns as f64 <= 0.0 {
            let clipped = (x + p.j_ns as f64).abs().clamp(0.0, u);
            let y = -((clipped / u).powf(p.rho_l) * u);
            self.d += p.lambda_l * y;
        }

        let dmax = (a_n.saturating_sub(s_n)) as f64 + p.delta_ns as f64;
        self.d = self.d.min(dmax).max(0.0);

        if p.mode >= ArcMode::Q && (self.d > self.e || self.e - p.gamma_ns as f64 > self.d) {
            self.e = self.d + p.gamma_ns as f64 / 2.0;
        }
        let offset = if p.mode >= ArcMode::Q { self.e } else { self.d };
        let projected = (s_n as f64 + offset + p.j_ns as f64).round();
        let t_n = a_n.max(projected.max(0.0) as u64);
        Schedule {
            block_n,
            s_n,
            a_n,
            d: self.d,
            e: self.e,
            t_n,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn solid() -> ArcParams {
        ArcParams {
            rho_u: 1.0,
            rho_l: 1.0,
            lambda_u: 0.16,
            lambda_l: 0.04,
            u_ns: 100_000_000,
            delta_ns: 100_000_000,
            j_ns: 0,
            gamma_ns: 16_666_667,
            g_ns: 50_000_000,
            t_idle_ns: 1_000_000_000_000,
            mode: ArcMode::QG,
            min_smooth_bytes: 0,
        }
    }

    #[test]
    fn linear_cycle_matches_c_fixed_point() {
        let mut sim = OffsetEstimator::new(solid()).unwrap();
        let mut schedules = Vec::new();
        for index in 0..500u64 {
            let delay = if index % 2 == 0 {
                100_000_000
            } else {
                70_000_000
            };
            let s_n = index * 1_000_000;
            schedules.push(sim.on_arrival(s_n, s_n + delay, 64));
        }
        let high = schedules[schedules.len() - 2].d;
        let low = schedules[schedules.len() - 1].d;
        assert!((high - 94_793_388.43).abs() < 1.0);
        assert!((low - 93_801_652.89).abs() < 1.0);
        assert!((schedules[schedules.len() - 1].e - 108_333_333.5).abs() < 1.0);
    }

    #[test]
    fn initialization_and_base_mode() {
        let mut sim = OffsetEstimator::new(solid()).unwrap();
        let schedule = sim.on_arrival(0, 50_000_000, 64);
        assert_eq!(schedule.d, 50_000_000.0);
        assert_eq!(schedule.e, 58_333_333.5);
        assert_eq!(schedule.t_n, 58_333_334);

        let base = ArcParams {
            mode: ArcMode::Base,
            ..solid()
        };
        let mut base_sim = OffsetEstimator::new(base).unwrap();
        assert_eq!(base_sim.on_arrival(0, 50_000_000, 64).t_n, 50_000_000);
    }

    #[test]
    fn idle_and_absurd_stamps() {
        let params = ArcParams {
            t_idle_ns: 100_000_000,
            ..solid()
        };
        let mut sim = OffsetEstimator::new(params).unwrap();
        sim.on_arrival(0, 50_000_000, 64);
        sim.on_arrival(1_000_000_000, 1_020_000_000, 64);
        assert_eq!(sim.idle_reanchors(), 1);
        assert_eq!(sim.d(), 20_000_000.0);

        let mut absurd = OffsetEstimator::new(solid()).unwrap();
        let first = absurd.on_arrival(0, 50_000_000, 64);
        let rejected = absurd.on_arrival(20_000_000_000, 1_000_000_000, 64);
        assert_eq!(absurd.s_n_rejects(), 1);
        assert_eq!(rejected.d, first.d);
        assert_eq!(rejected.t_n, 1_000_000_000);
    }

    #[test]
    fn small_entry_bypass() {
        let params = ArcParams {
            min_smooth_bytes: 1024,
            ..solid()
        };
        let mut sim = OffsetEstimator::new(params).unwrap();
        assert_eq!(sim.on_arrival(0, 50_000_000, 64).t_n, 50_000_000);
    }

    #[test]
    fn invalid_params_rejected() {
        assert!(OffsetEstimator::new(ArcParams { u_ns: 0, ..solid() }).is_err());
        assert!(OffsetEstimator::new(ArcParams {
            lambda_l: 1.0,
            ..solid()
        })
        .is_err());
    }
}
