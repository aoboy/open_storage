//! ARC slab and apply boundary (W6.18 slice).
//!
//! Owns caller payload storage and invokes an apply callback exactly once per
//! released entry. The C pacer remains the default; this is the Rust-side
//! shape that a future `arc_*` FFI adapter will wrap.

use crate::scheduler::{Scheduler, SchedulerStats};
use crate::{ArcMode, ArcParams, Schedule};

pub struct Pacer {
    scheduler: Scheduler,
    slots: Vec<Option<Vec<u8>>>,
    free: Vec<u32>,
    entry_max: usize,
}

impl Pacer {
    pub fn new(params: ArcParams, capacity: usize, entry_max: usize) -> Result<Self, &'static str> {
        if entry_max == 0 {
            return Err("entry_max must be positive");
        }
        let mut free = (0..capacity as u32).rev().collect::<Vec<_>>();
        free.shrink_to_fit();
        Ok(Self {
            scheduler: Scheduler::new(params, capacity)?,
            slots: (0..capacity).map(|_| None).collect(),
            free,
            entry_max,
        })
    }

    pub fn submit(&mut self, entry: &[u8], s_n: u64, a_n: u64) -> Result<Schedule, &'static str> {
        if entry.len() > self.entry_max {
            return Err("entry too large");
        }
        let slot = self.free.pop().ok_or("slab full")?;
        self.slots[slot as usize] = Some(entry.to_vec());
        match self.scheduler.submit_with_slot(s_n, a_n, entry.len(), slot) {
            Ok(schedule) => Ok(schedule),
            Err(error) => {
                self.slots[slot as usize] = None;
                self.free.push(slot);
                Err(error)
            }
        }
    }

    pub fn apply_due(
        &mut self,
        now_ns: u64,
        apply: &mut impl FnMut(&[u8], u64),
    ) -> Result<usize, &'static str> {
        let releases = self.scheduler.drain(now_ns)?;
        let released = releases.len();
        for release in releases {
            let slot = release.slot as usize;
            let bytes = self.slots[slot].take().ok_or("released slot missing")?;
            apply(&bytes, now_ns);
            self.free.push(release.slot);
        }
        Ok(released)
    }

    pub fn stats(&self) -> SchedulerStats {
        self.scheduler.stats()
    }

    pub fn mode(&self) -> ArcMode {
        self.scheduler.mode()
    }

    pub fn set_mode(&mut self, mode: ArcMode) {
        self.scheduler.set_mode(mode);
    }

    pub fn free_slots(&self) -> usize {
        self.free.len()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn apply_once_and_reuse_slot() {
        let mut pacer = Pacer::new(ArcParams::default(), 2, 16).unwrap();
        pacer.submit(b"aaa", 0, 0).unwrap();
        pacer.submit(b"bbb", 0, 1).unwrap();
        assert_eq!(pacer.free_slots(), 0);
        assert_eq!(pacer.submit(b"ccc", 0, 2), Err("slab full"));

        let mut seen = Vec::new();
        let count = pacer
            .apply_due(60_000_000, &mut |bytes, _| seen.push(bytes.to_vec()))
            .unwrap();
        assert_eq!(count, 2);
        assert_eq!(seen, vec![b"aaa".to_vec(), b"bbb".to_vec()]);
        assert_eq!(pacer.free_slots(), 2);
        assert_eq!(pacer.stats().releases, 2);

        pacer.submit(b"ccc", 0, 3).unwrap();
        let mut again = Vec::new();
        pacer
            .apply_due(120_000_000, &mut |bytes, _| again.push(bytes.to_vec()))
            .unwrap();
        assert_eq!(again, vec![b"ccc".to_vec()]);
    }

    #[test]
    fn rejects_oversize_and_invalid_capacity() {
        let mut pacer = Pacer::new(ArcParams::default(), 1, 4).unwrap();
        assert_eq!(pacer.submit(b"12345", 0, 0), Err("entry too large"));
        assert!(Pacer::new(ArcParams::default(), 1, 0).is_err());
        assert!(Pacer::new(ArcParams::default(), 0, 4).is_err());
    }
}
