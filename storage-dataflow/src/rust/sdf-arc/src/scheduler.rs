//! ARC-QG ordering and guard/deadline drain (W6.18 slice).
//!
//! This composes the offset estimator and release queue into the release-side
//! state machine from `arc_drain`. It has no callbacks, slab, or sidecar yet:
//! `drain` returns the releases that a future FFI adapter would apply.

use crate::heap::{Release, ReleaseQueue};
use crate::{ArcMode, ArcParams, OffsetEstimator, Schedule};

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct SchedulerStats {
    pub releases: u64,
    pub guard_waits: u64,
    pub guard_timeouts: u64,
    pub idle_reanchors: u64,
    pub s_n_rejects: u64,
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
            ..self.stats
        }
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
    fn submit_uses_estimator() {
        let mut scheduler = Scheduler::new(ArcParams::default(), 8).unwrap();
        let schedule = scheduler.submit(0, 50_000_000, 64).unwrap();
        assert_eq!(schedule.block_n, 0);
        assert_eq!(scheduler.drain(60_000_000).unwrap().len(), 1);
    }
}
