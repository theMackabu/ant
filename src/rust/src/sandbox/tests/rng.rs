use super::*;
use crate::sandbox::guest::testing::{TestMemory, BASE};
use crate::sandbox::vring::testing::*;
use crate::sandbox::vring::{drain, DESC_F_NEXT, DESC_F_WRITE};

const DATA: u64 = BASE + 0x4000;

#[test]
fn fills_every_writable_descriptor() {
  let mem = TestMemory::new(0x8000);
  put_desc(&mem, 0, DATA, 64, DESC_F_WRITE | DESC_F_NEXT, 1);
  put_desc(&mem, 1, DATA + 0x100, 64, DESC_F_WRITE, 0);
  assert_eq!(request(&mem, DESC_BASE, 8, 0), Ok(128));
  assert_ne!(mem.get::<64>(DATA), [0u8; 64]);
  assert_ne!(mem.get::<64>(DATA + 0x100), [0u8; 64]);
  assert_eq!(mem.get::<8>(DATA + 64), [0u8; 8]);
}

#[test]
fn rejects_malformed_chains() {
  let mem = TestMemory::new(0x8000);
  put_desc(&mem, 0, DATA, 16, 0, 0);
  assert_eq!(request(&mem, DESC_BASE, 8, 0), Err(EINVAL));

  put_desc(&mem, 0, DATA, 16, DESC_F_WRITE | DESC_F_NEXT, 0);
  assert_eq!(request(&mem, DESC_BASE, 8, 0), Err(EINVAL));

  put_desc(&mem, 0, DATA, 16, DESC_F_WRITE | DESC_F_NEXT, 9);
  assert_eq!(request(&mem, DESC_BASE, 8, 0), Err(EINVAL));

  put_desc(&mem, 0, BASE + 0x7ff0, 32, DESC_F_WRITE, 0);
  assert_eq!(request(&mem, DESC_BASE, 8, 0), Err(EFAULT));

  put_desc(&mem, 0, DATA, u32::MAX, DESC_F_WRITE, 0);
  assert_eq!(request(&mem, DESC_BASE, 8, 0), Err(EFAULT));

  assert_eq!(request(&mem, DESC_BASE, 0, 0), Err(EINVAL));
  assert_eq!(request(&mem, DESC_BASE, 8, 8), Err(EINVAL));
  assert_eq!(request(&mem, DESC_BASE, 64, 0), Err(EINVAL));
}

#[test]
fn drains_queue_and_reports_length() {
  let mem = TestMemory::new(0x8000);
  put_desc(&mem, 3, DATA, 24, DESC_F_WRITE, 0);
  put_avail(&mem, &[3]);
  let mut q = queue(8);
  assert_eq!(drain(&mem, &mut q, |base, size, head| request(&mem, base, size, head)), Ok(true));
  assert_eq!(used(&mem, 0), (1, 3, 24));
}
