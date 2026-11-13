use super::GuestMemory;
use core::cell::UnsafeCell;

pub(crate) const BASE: u64 = 0x4000_0000;

pub(crate) struct TestMemory {
  bytes: Box<[UnsafeCell<u8>]>,
}

impl TestMemory {
  pub(crate) fn new(len: usize) -> Self {
    Self { bytes: (0..len).map(|_| UnsafeCell::new(0)).collect() }
  }

  pub(crate) fn put(&self, addr: u64, src: &[u8]) {
    self.write(addr, src).unwrap();
  }

  pub(crate) fn get<const N: usize>(&self, addr: u64) -> [u8; N] {
    self.read_array(addr).unwrap()
  }
}

unsafe impl GuestMemory for TestMemory {
  fn ptr(&self, addr: u64, len: usize) -> Option<*mut u8> {
    let off = usize::try_from(addr.checked_sub(BASE)?).ok()?;
    if off > self.bytes.len() || len > self.bytes.len() - off {
      return None;
    }
    Some(UnsafeCell::raw_get(self.bytes.as_ptr()).wrapping_add(off))
  }
}
