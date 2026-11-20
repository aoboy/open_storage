//! ARC sidecar-mode semantics (W6.18 slice).
//!
//! Shadow applies each accepted entry immediately and later drains the QG
//! schedule without applying again; live applies on schedule; killed applies
//! the accepted backlog immediately. The C sidecar remains the production
//! implementation until the FFI/consumer slices land.

use crate::pacer::Pacer;
use crate::{ArcMode, ArcParams, Schedule};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SidecarMode {
    Shadow,
    Live,
    Killed,
}

pub struct Sidecar {
    pacer: Pacer,
    mode: SidecarMode,
}

impl Sidecar {
    pub fn new(params: ArcParams, capacity: usize, entry_max: usize) -> Result<Self, &'static str> {
        Ok(Self {
            pacer: Pacer::new(params, capacity, entry_max)?,
            mode: SidecarMode::Shadow,
        })
    }

    pub fn mode(&self) -> SidecarMode {
        self.mode
    }

    pub fn set_mode(&mut self, mode: SidecarMode) {
        self.mode = mode;
        self.pacer.set_mode(match mode {
            SidecarMode::Killed => ArcMode::Immediate,
            SidecarMode::Shadow | SidecarMode::Live => ArcMode::QG,
        });
    }

    pub fn submit(
        &mut self,
        entry: &[u8],
        s_n: u64,
        a_n: u64,
        now_ns: u64,
        apply: &mut impl FnMut(&[u8], u64),
    ) -> Result<Schedule, &'static str> {
        let schedule = self.pacer.submit(entry, s_n, a_n)?;
        match self.mode {
            SidecarMode::Shadow => {
                self.pacer
                    .apply_immediate(schedule.block_n, now_ns, apply)?;
            }
            SidecarMode::Killed => {
                self.pacer.apply_due(now_ns, apply)?;
            }
            SidecarMode::Live => {}
        }
        Ok(schedule)
    }

    pub fn poll(
        &mut self,
        now_ns: u64,
        apply: &mut impl FnMut(&[u8], u64),
    ) -> Result<usize, &'static str> {
        if self.mode == SidecarMode::Shadow {
            self.pacer.apply_due(now_ns, &mut |_bytes, _now| {})
        } else {
            self.pacer.apply_due(now_ns, apply)
        }
    }

    pub fn stats(&self) -> crate::scheduler::SchedulerStats {
        self.pacer.stats()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn live_applies_on_schedule() {
        let mut sidecar = Sidecar::new(ArcParams::default(), 8, 16).unwrap();
        sidecar.set_mode(SidecarMode::Live);
        let mut applied = 0;
        sidecar
            .submit(b"a", 0, 50_000_000, 50_000_000, &mut |_, _| applied += 1)
            .unwrap();
        assert_eq!(applied, 0);
        assert_eq!(
            sidecar.poll(20_000_000, &mut |_, _| applied += 1).unwrap(),
            0
        );
        assert_eq!(
            sidecar.poll(60_000_000, &mut |_, _| applied += 1).unwrap(),
            1
        );
        assert_eq!(applied, 1);
    }

    #[test]
    fn shadow_applies_once_immediately() {
        let mut sidecar = Sidecar::new(ArcParams::default(), 8, 16).unwrap();
        let mut applied = Vec::new();
        sidecar
            .submit(b"a", 0, 0, 5, &mut |bytes, now| {
                applied.push((bytes.to_vec(), now))
            })
            .unwrap();
        assert_eq!(applied, vec![(b"a".to_vec(), 5)]);
        let released = sidecar.poll(100_000_000, &mut |_, _| {
            panic!("shadow poll must not apply")
        });
        assert_eq!(released.unwrap(), 1);
        assert_eq!(applied.len(), 1);
        assert_eq!(sidecar.stats().releases, 1);
    }

    #[test]
    fn killed_drains_backlog_and_new_entry() {
        let mut sidecar = Sidecar::new(ArcParams::default(), 8, 16).unwrap();
        sidecar.set_mode(SidecarMode::Live);
        let mut applied = Vec::new();
        sidecar
            .submit(b"old", 0, 0, 0, &mut |bytes, _| {
                applied.push(bytes.to_vec())
            })
            .unwrap();
        sidecar.set_mode(SidecarMode::Killed);
        sidecar
            .submit(b"new", 0, 1, 1, &mut |bytes, _| {
                applied.push(bytes.to_vec())
            })
            .unwrap();
        assert_eq!(applied, vec![b"old".to_vec(), b"new".to_vec()]);
        assert_eq!(sidecar.stats().releases, 2);
    }
}
