//! ARC-QG ordering and guard/deadline drain (W6.18 slice).
//!
//! This composes the offset estimator and release queue into the release-side
//! state machine from `arc_drain`. It has no callbacks, slab, or sidecar yet:
//! `drain` returns the releases that a future FFI adapter would apply.

use crate::heap::{Release, ReleaseQueue};
use crate::{ArcMode, ArcParams, OffsetEstimator, Schedule};

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SchedulerStats {
    pub releases: u64,
    pub guard_waits: u64,
    pub guard_timeouts: u64,
    pub idle_reanchors: u64,
    pub s_n_rejects: u64,
    pub heap_depth: u32,
    pub added_p50_ns: f64,
    pub added_p95_ns: f64,
    pub added_p99_ns: f64,
    pub interval_error_max_ns: u64,
}

pub struct Scheduler {
    params: ArcParams,
    estimator: OffsetEstimator,
    queue: ReleaseQueue,
    mode: ArcMode,
    last_released_n: u64,
    order_initialized: bool,
    next_seq: u64,
    stats: SchedulerStats,
    added_samples: [u64; 256],
    added_n: usize,
    added_pos: usize,
    last_release_ts: u64,
    last_release_s_ns: u64,
    have_last_release: bool,
}

fn percentile(samples: &[u64; 256], n: usize, p: f64) -> f64 {
    if n == 0 {
        return 0.0;
    }
    let mut copy = *samples;
    copy[..n].sort_unstable();
    let index = (((n - 1) as f64) * p + 0.5) as usize;
    copy[index.min(n - 1)] as f64
}

impl Scheduler {
    pub fn new(params: ArcParams, capacity: usize) -> Result<Self, &'static str> {
        if capacity == 0 {
            return Err("capacity must be positive");
        }
        let mode = params.mode;
        Ok(Self {
            params,
            estimator: OffsetEstimator::new(params)?,
            queue: ReleaseQueue::new(capacity),
            mode,
            last_released_n: 0,
            order_initialized: false,
            next_seq: 0,
            stats: SchedulerStats::default(),
            added_samples: [0; 256],
            added_n: 0,
            added_pos: 0,
            last_release_ts: 0,
            last_release_s_ns: 0,
            have_last_release: false,
        })
    }

    pub fn mode(&self) -> ArcMode {
        self.mode
    }

    pub fn set_mode(&mut self, mode: ArcMode) {
        self.mode = mode;
    }

    pub fn stats(&self) -> SchedulerStats {
        SchedulerStats {
            idle_reanchors: self.estimator.idle_reanchors(),
            s_n_rejects: self.estimator.s_n_rejects(),
            heap_depth: self.queue.len() as u32,
            added_p50_ns: percentile(&self.added_samples, self.added_n, 0.50),
            added_p95_ns: percentile(&self.added_samples, self.added_n, 0.95),
            added_p99_ns: percentile(&self.added_samples, self.added_n, 0.99),
            ..self.stats
        }
    }

    fn note_release(&mut self, release: &Release, now_ns: u64) {
        let added = now_ns.saturating_sub(release.a_n);
        if self.added_n < self.added_samples.len() {
            self.added_samples[self.added_n] = added;
            self.added_n += 1;
        } else {
            self.added_samples[self.added_pos] = added;
            self.added_pos = (self.added_pos + 1) % self.added_samples.len();
        }

        if self.have_last_release {
            let rel_gap = now_ns as i64 - self.last_release_ts as i64;
            let prod_gap = release.s_n as i64 - self.last_release_s_ns as i64;
            self.stats.interval_error_max_ns = self
                .stats
                .interval_error_max_ns
                .max((rel_gap - prod_gap).unsigned_abs());
        }
        self.last_release_ts = now_ns;
        self.last_release_s_ns = release.s_n;
        self.have_last_release = true;
    }

    pub fn submit(&mut self, s_n: u64, a_n: u64, len: usize) -> Result<Schedule, &'static str> {
        let schedule = self.estimator.on_arrival(s_n, a_n, len);
        self.queue.push(Release {
            t_n: schedule.t_n,
            t_deadline: schedule.t_n.saturating_add(self.params.g_ns),
            block_n: schedule.block_n,
            s_n,
            a_n,
            seq: self.next_seq,
            len: u32::try_from(len).map_err(|_| "entry too large")?,
            parked: false,
        })?;
        self.next_seq += 1;
        Ok(schedule)
    }

    #[cfg(test)]
    fn push_test_release(&mut self, release: Release) -> Result<(), &'static str> {
        self.queue.push(release)
    }

    fn drain_inner(&mut self, now_ns: u64, out: &mut Vec<Release>) -> Result<(), &'static str> {
        while let Some(t_n) = self.queue.peek_t_n() {
            if self.mode != ArcMode::Immediate && t_n > now_ns {
                break;
            }
            let mut release = self.queue.pop().ok_or("heap unexpectedly empty")?;

            if self.mode >= ArcMode::QG {
                let ordered = if self.order_initialized {
                    release.block_n <= self.last_released_n + 1
                } else {
                    release.block_n == 0
                };
                if !ordered {
                    if let Some(pred_t) = self.queue.min_predecessor_t_n(release.block_n) {
                        release.t_n = pred_t.saturating_add(1);
                        self.stats.guard_waits += 1;
                        self.queue.push(release)?;
                        continue;
                    }
                    if now_ns < release.t_deadline {
                        release.t_n = release.t_deadline;
                        release.parked = true;
                        self.stats.guard_waits += 1;
                        self.queue.push(release)?;
                        continue;
                    }
                    self.stats.guard_timeouts += 1;
                }
            }

            let released_n = release.block_n;
            release.parked = false;
            self.last_released_n = released_n;
            self.order_initialized = true;
            self.stats.releases += 1;
            self.note_release(&release, now_ns);
            out.push(release);
            let _ = self.queue.wake_successor(released_n + 1, now_ns);
        }
        Ok(())
    }

    pub fn drain(&mut self, now_ns: u64) -> Result<Vec<Release>, &'static str> {
        let mut out = Vec::new();
        self.drain_inner(now_ns, &mut out)?;
        Ok(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn release(t_n: u64, deadline: u64, block_n: u64, parked: bool) -> Release {
        Release {
            t_n,
            t_deadline: deadline,
            block_n,
            s_n: 0,
            a_n: 0,
            seq: block_n,
            len: 64,
            parked,
        }
    }

    #[test]
    fn drains_due_blocks_in_order() {
        let mut scheduler = Scheduler::new(ArcParams::default(), 8).unwrap();
        scheduler
            .push_test_release(release(10, 100, 0, false))
            .unwrap();
        scheduler
            .push_test_release(release(5, 100, 1, false))
            .unwrap();
        scheduler
            .push_test_release(release(20, 100, 2, false))
            .unwrap();
        let out = scheduler.drain(15).unwrap();
        assert_eq!(out.iter().map(|r| r.block_n).collect::<Vec<_>>(), [0, 1]);
        assert_eq!(scheduler.stats().releases, 2);
        assert_eq!(scheduler.queue.peek_t_n(), Some(20));
    }

    #[test]
    fn predecessor_requeues_behind_it() {
        let mut scheduler = Scheduler::new(ArcParams::default(), 8).unwrap();
        scheduler
            .push_test_release(release(5, 100, 1, false))
            .unwrap();
        scheduler
            .push_test_release(release(10, 100, 0, false))
            .unwrap();
        let first = scheduler.drain(10).unwrap();
        assert_eq!(first.iter().map(|r| r.block_n).collect::<Vec<_>>(), [0]);
        assert_eq!(scheduler.stats().guard_waits, 1);
        let second = scheduler.drain(11).unwrap();
        assert_eq!(second.iter().map(|r| r.block_n).collect::<Vec<_>>(), [1]);
    }

    #[test]
    fn missing_predecessor_parks_to_deadline() {
        let mut scheduler = Scheduler::new(ArcParams::default(), 8).unwrap();
        scheduler
            .push_test_release(release(5, 100, 2, false))
            .unwrap();
        assert!(scheduler.drain(10).unwrap().is_empty());
        assert_eq!(scheduler.stats().guard_waits, 1);
        let out = scheduler.drain(100).unwrap();
        assert_eq!(out.iter().map(|r| r.block_n).collect::<Vec<_>>(), [2]);
        assert_eq!(scheduler.stats().guard_timeouts, 1);
    }

    #[test]
    fn release_wakes_parked_successor() {
        let mut scheduler = Scheduler::new(ArcParams::default(), 8).unwrap();
        scheduler
            .push_test_release(release(10, 100, 0, false))
            .unwrap();
        scheduler
            .push_test_release(release(100, 100, 1, true))
            .unwrap();
        let out = scheduler.drain(10).unwrap();
        assert_eq!(out.iter().map(|r| r.block_n).collect::<Vec<_>>(), [0, 1]);
        assert!(!out[1].parked);
    }

    #[test]
    fn immediate_mode_releases_all() {
        let mut scheduler = Scheduler::new(ArcParams::default(), 8).unwrap();
        scheduler.set_mode(ArcMode::Immediate);
        scheduler
            .push_test_release(release(100, 200, 0, false))
            .unwrap();
        assert_eq!(scheduler.drain(0).unwrap().len(), 1);
    }

    #[test]
    fn release_statistics_track_added_delay_and_interval_error() {
        let mut scheduler = Scheduler::new(ArcParams::default(), 8).unwrap();
        scheduler
            .push_test_release(release(0, 100, 0, false))
            .unwrap();
        scheduler
            .push_test_release(release(15, 100, 1, false))
            .unwrap();
        let first = scheduler.drain(10).unwrap();
        assert_eq!(first.len(), 1);
        let second = scheduler.drain(20).unwrap();
        assert_eq!(second.len(), 1);
        let stats = scheduler.stats();
        assert_eq!(stats.releases, 2);
        assert_eq!(stats.heap_depth, 0);
        assert_eq!(stats.added_p50_ns, 20.0);
        assert_eq!(stats.added_p99_ns, 20.0);
        assert_eq!(stats.interval_error_max_ns, 10);
    }

    #[test]
    fn submit_uses_estimator() {
        let mut scheduler = Scheduler::new(ArcParams::default(), 8).unwrap();
        let schedule = scheduler.submit(0, 50_000_000, 64).unwrap();
        assert_eq!(schedule.block_n, 0);
        assert_eq!(scheduler.drain(60_000_000).unwrap().len(), 1);
    }
}
