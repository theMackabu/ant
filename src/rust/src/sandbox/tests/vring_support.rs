use super::*;
use crate::sandbox::guest::testing::{TestMemory, BASE};

pub(crate) const DESC_BASE: u64 = BASE + 0x1000;
pub(crate) const AVAIL_BASE: u64 = BASE + 0x2000;
pub(crate) const USED_BASE: u64 = BASE + 0x3000;

pub(crate) fn put_desc(mem: &TestMemory, index: u16, addr: u64, len: u32, flags: u16, next: u16) {
  let at = DESC_BASE + u64::from(index) * DESC_BYTES;
  mem.put(at, &addr.to_le_bytes());
  mem.put(at + 8, &len.to_le_bytes());
  mem.put(at + 12, &flags.to_le_bytes());
  mem.put(at + 14, &next.to_le_bytes());
}

pub(crate) fn put_avail(mem: &TestMemory, heads: &[u16]) {
  mem.put(AVAIL_BASE + 2, &(heads.len() as u16).to_le_bytes());
  for (slot, head) in heads.iter().enumerate() {
    mem.put(AVAIL_BASE + 4 + slot as u64 * 2, &head.to_le_bytes());
  }
}

pub(crate) fn queue(size: u16) -> Queue {
  Queue {
    desc: DESC_BASE,
    avail: AVAIL_BASE,
    used: USED_BASE,
    size,
    max_size: size,
    last_avail: 0,
    msix_vector: 0xffff,
    notify_off: 0,
    enabled: true,
  }
}

pub(crate) fn used(mem: &TestMemory, slot: u64) -> (u16, u32, u32) {
  let idx = u16::from_le_bytes(mem.get(USED_BASE + 2));
  let head = u32::from_le_bytes(mem.get(USED_BASE + 4 + slot * USED_ELEM_BYTES));
  let len = u32::from_le_bytes(mem.get(USED_BASE + 8 + slot * USED_ELEM_BYTES));
  (idx, head, len)
}
