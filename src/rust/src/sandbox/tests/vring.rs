use super::testing::*;
use super::*;
use crate::sandbox::errno::EFAULT;
use crate::sandbox::guest::testing::{TestMemory, BASE};

const DATA: u64 = BASE + 0x4000;

#[test]
fn guest_memory_bounds() {
  let mem = TestMemory::new(0x100);
  assert!(mem.ptr(BASE, 0x100).is_some());
  assert!(mem.ptr(BASE + 0x100, 0).is_some());
  assert!(mem.ptr(BASE + 0xff, 2).is_none());
  assert!(mem.ptr(BASE - 1, 1).is_none());
  assert!(mem.ptr(u64::MAX, 1).is_none());
  assert_eq!(mem.read(BASE + 0x101, &mut []), Err(EFAULT));
}

#[test]
fn read_chain_concatenates_readable_descriptors() {
  let mem = TestMemory::new(0x8000);
  mem.put(DATA, b"hello ");
  mem.put(DATA + 0x100, b"world");
  put_desc(&mem, 0, DATA, 6, DESC_F_NEXT, 1);
  put_desc(&mem, 1, DATA + 0x100, 5, 0, 0);

  let mut out = [0u8; 16];
  assert_eq!(read_chain(&mem, DESC_BASE, 0, 8, &mut out), Ok(11));
  assert_eq!(&out[..11], b"hello world");

  let mut small = [0u8; 10];
  assert_eq!(read_chain(&mem, DESC_BASE, 0, 8, &mut small), Err(ENOSPC));

  put_desc(&mem, 1, DATA + 0x100, 5, DESC_F_WRITE, 0);
  assert_eq!(read_chain(&mem, DESC_BASE, 0, 8, &mut out), Err(EINVAL));
}

#[test]
fn read_chain_rejects_loops_and_wild_addresses() {
  let mem = TestMemory::new(0x8000);
  put_desc(&mem, 0, DATA, 1, DESC_F_NEXT, 1);
  put_desc(&mem, 1, DATA, 1, DESC_F_NEXT, 0);
  let mut out = [0u8; 64];
  assert_eq!(read_chain(&mem, DESC_BASE, 0, 8, &mut out), Err(ELOOP));
  assert_eq!(read_chain(&mem, DESC_BASE, 0, 0, &mut out), Err(ELOOP));

  put_desc(&mem, 0, BASE + 0x7fff, 2, 0, 0);
  assert_eq!(read_chain(&mem, DESC_BASE, 0, 8, &mut out), Err(EFAULT));
  put_desc(&mem, 0, u64::MAX, 1, 0, 0);
  assert_eq!(read_chain(&mem, DESC_BASE, 0, 8, &mut out), Err(EFAULT));
  put_desc(&mem, 0, DATA, u32::MAX, 0, 0);
  assert_eq!(read_chain(&mem, DESC_BASE, 0, 8, &mut out), Err(ENOSPC));
  assert_eq!(read_chain(&mem, u64::MAX - 4, 3, 8, &mut out), Err(EFAULT));
}

#[test]
fn write_chain_splits_across_writable_descriptors() {
  let mem = TestMemory::new(0x8000);
  put_desc(&mem, 0, DATA, 4, DESC_F_WRITE | DESC_F_NEXT, 1);
  put_desc(&mem, 1, DATA + 0x100, 8, DESC_F_WRITE, 0);
  assert_eq!(write_chain(&mem, DESC_BASE, 0, 8, b"abcdefg"), Ok(7));
  assert_eq!(mem.get::<4>(DATA), *b"abcd");
  assert_eq!(mem.get::<3>(DATA + 0x100), *b"efg");

  assert_eq!(write_chain(&mem, DESC_BASE, 0, 8, &[0; 13]), Err(ENOSPC));
  assert_eq!(write_chain(&mem, DESC_BASE, 0, 1, b"abcdefg"), Err(ENOSPC));
  assert_eq!(write_chain(&mem, DESC_BASE, 0, 8, &[]), Err(ENOSPC));

  put_desc(&mem, 0, DATA, 4, DESC_F_NEXT, 1);
  assert_eq!(write_chain(&mem, DESC_BASE, 0, 8, b"abc"), Err(EINVAL));
}

#[test]
fn add_used_wraps_ring_slot() {
  let mem = TestMemory::new(0x8000);
  mem.put(USED_BASE + 2, &5u16.to_le_bytes());
  assert_eq!(add_used(&mem, USED_BASE, 4, 3, 99), Ok(()));
  assert_eq!(used(&mem, 1), (6, 3, 99));
  assert_eq!(add_used(&mem, USED_BASE, 0, 3, 99), Err(EINVAL));
}

#[test]
fn drain_walks_available_heads() {
  let mem = TestMemory::new(0x8000);
  put_avail(&mem, &[2, 0]);
  let mut q = queue(4);
  let mut seen = Vec::new();
  let ready = drain(&mem, &mut q, |_, _, head| {
    seen.push(head);
    Ok(u32::from(head) + 10)
  });
  assert_eq!(ready, Ok(true));
  assert_eq!(seen, [2, 0]);
  assert_eq!(q.last_avail, 2);
  assert_eq!(used(&mem, 0), (2, 2, 12));
  assert_eq!(used(&mem, 1), (2, 0, 10));

  let mut idle = queue(4);
  idle.enabled = false;
  assert_eq!(drain(&mem, &mut idle, |_, _, _| Ok(0)), Ok(false));

  put_avail(&mem, &[4]);
  assert_eq!(drain(&mem, &mut queue(4), |_, _, _| Ok(0)), Err(EINVAL));
  let mut zero = queue(0);
  assert_eq!(drain(&mem, &mut zero, |_, _, _| Ok(0)), Err(EINVAL));
}
