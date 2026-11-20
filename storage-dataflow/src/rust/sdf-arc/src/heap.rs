//! ARC release-queue ordering (W6.18 slice).
//!
//! Mirrors the C min-heap key: `(T_n, seq)` with submit order as the stable
//! tie-break. Guard/deadline drain behavior and caller-owned storage are the
//! next slice; this module only owns the ordering primitive.

use std::cmp::Ordering;
use std::collections::BinaryHeap;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Release {
    pub t_n: u64,
    pub t_deadline: u64,
    pub block_n: u64,
    pub s_n: u64,
    pub a_n: u64,
    pub seq: u64,
    pub len: u32,
    pub slot: u32,
    pub parked: bool,
}

impl Ord for Release {
    fn cmp(&self, other: &Self) -> Ordering {
        // BinaryHeap is a max-heap: reverse the key so the smallest
        // (T_n, seq) is popped first.
        other
            .t_n
            .cmp(&self.t_n)
            .then_with(|| other.seq.cmp(&self.seq))
    }
}

impl PartialOrd for Release {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

#[derive(Debug)]
pub struct ReleaseQueue {
    heap: BinaryHeap<Release>,
    capacity: usize,
}

impl ReleaseQueue {
    pub fn new(capacity: usize) -> Self {
        Self {
            heap: BinaryHeap::with_capacity(capacity),
            capacity,
        }
    }

    pub fn push(&mut self, release: Release) -> Result<(), &'static str> {
        if self.capacity == 0 || self.heap.len() >= self.capacity {
            return Err("release queue full");
        }
        self.heap.push(release);
        Ok(())
    }

    pub fn pop(&mut self) -> Option<Release> {
        self.heap.pop()
    }

    pub fn peek_t_n(&self) -> Option<u64> {
        self.heap.peek().map(|release| release.t_n)
    }

    pub fn len(&self) -> usize {
        self.heap.len()
    }

    pub fn is_empty(&self) -> bool {
        self.heap.is_empty()
    }

    pub fn clear(&mut self) {
        self.heap.clear();
    }

    pub fn min_predecessor_t_n(&self, block_n: u64) -> Option<u64> {
        self.heap
            .iter()
            .filter(|release| release.block_n + 1 == block_n)
            .map(|release| release.t_n)
            .min()
    }

    pub fn wake_successor(&mut self, block_n: u64, now_ns: u64) -> bool {
        let mut items = std::mem::take(&mut self.heap).into_vec();
        let mut woken = false;
        for release in &mut items {
            if release.block_n == block_n && release.parked && release.t_n > now_ns {
                release.t_n = now_ns;
                release.parked = false;
                woken = true;
            }
        }
        self.heap = BinaryHeap::from(items);
        woken
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn release(t_n: u64, seq: u64, block_n: u64) -> Release {
        Release {
            t_n,
            t_deadline: t_n + 1_000,
            block_n,
            s_n: 0,
            a_n: 0,
            seq,
            len: 64,
            slot: block_n as u32,
            parked: false,
        }
    }

    #[test]
    fn pops_by_t_n_then_seq() {
        let mut queue = ReleaseQueue::new(8);
        queue.push(release(20, 0, 2)).unwrap();
        queue.push(release(10, 1, 1)).unwrap();
        queue.push(release(10, 0, 0)).unwrap();
        queue.push(release(5, 5, 5)).unwrap();

        let order: Vec<u64> = std::iter::from_fn(|| queue.pop())
            .map(|item| item.block_n)
            .collect();
        assert_eq!(order, [5, 0, 1, 2]);
        assert!(queue.is_empty());
    }

    #[test]
    fn capacity_and_peek() {
        let mut queue = ReleaseQueue::new(1);
        assert_eq!(queue.peek_t_n(), None);
        queue.push(release(7, 0, 0)).unwrap();
        assert_eq!(queue.peek_t_n(), Some(7));
        assert!(queue.push(release(8, 1, 1)).is_err());
        queue.clear();
        assert_eq!(queue.len(), 0);
    }

    #[test]
    fn random_insert_order_preserves_key_order() {
        let mut queue = ReleaseQueue::new(256);
        let mut state = 0x9e37_79b9_7f4a_7c15u64;
        for seq in 0..128u64 {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            queue.push(release(state % 97, seq, seq)).unwrap();
        }
        let mut previous = None;
        while let Some(item) = queue.pop() {
            if let Some((t_n, seq)) = previous {
                assert!((t_n, seq) < (item.t_n, item.seq));
            }
            previous = Some((item.t_n, item.seq));
        }
    }
}
