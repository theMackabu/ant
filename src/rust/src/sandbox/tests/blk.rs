use super::*;
use crate::sandbox::guest::testing::{TestMemory, BASE};
use crate::sandbox::vring::testing::*;
use crate::sandbox::vring::{DESC_F_NEXT, DESC_F_WRITE};
use core::cell::{Cell, RefCell};

const REQ: u64 = BASE + 0x2000;
const DATA: u64 = BASE + 0x3000;
const STATUS: u64 = BASE + 0x5000;

struct TestDisk {
  bytes: RefCell<Vec<u8>>,
  sectors: u64,
  synced: Cell<usize>,
  fail_sync: bool,
}

impl TestDisk {
  fn new(sectors: u64) -> Self {
    let bytes = (0..sectors * SECTOR_SIZE).map(|i| (i / SECTOR_SIZE) as u8 + 1).collect();
    Self { bytes: RefCell::new(bytes), sectors, synced: Cell::new(0), fail_sync: false }
  }
}

unsafe impl Disk for TestDisk {
  fn sectors(&self) -> u64 {
    self.sectors
  }

  unsafe fn read_at(&self, dest: *mut u8, len: usize, off: u64) -> Result<usize> {
    let bytes = self.bytes.borrow();
    let avail = bytes.get(off as usize..).unwrap_or(&[]);
    let n = len.min(avail.len()).min(700);
    core::ptr::copy_nonoverlapping(avail.as_ptr(), dest, n);
    Ok(n)
  }

  unsafe fn write_at(&self, src: *const u8, len: usize, off: u64) -> Result<usize> {
    let mut bytes = self.bytes.borrow_mut();
    let n = len.min(700);
    let Some(dest) = bytes.get_mut(off as usize..off as usize + n) else { return Err(EIO) };
    core::ptr::copy_nonoverlapping(src, dest.as_mut_ptr(), n);
    Ok(n)
  }

  fn sync(&self) -> Result<()> {
    self.synced.set(self.synced.get() + 1);
    if self.fail_sync {
      Err(EIO)
    } else {
      Ok(())
    }
  }
}

fn put_req(mem: &TestMemory, kind: u32, sector: u64) {
  mem.put(REQ, &kind.to_le_bytes());
  mem.put(REQ + 8, &sector.to_le_bytes());
}

fn three_part(mem: &TestMemory, kind: u32, sector: u64, data_len: u32, data_flags: u16) {
  put_req(mem, kind, sector);
  put_desc(mem, 0, REQ, 16, DESC_F_NEXT, 1);
  put_desc(mem, 1, DATA, data_len, data_flags | DESC_F_NEXT, 2);
  put_desc(mem, 2, STATUS, 1, DESC_F_WRITE, 0);
  mem.put(STATUS, &[0xaa]);
}

fn status(mem: &TestMemory) -> u8 {
  mem.get::<1>(STATUS)[0]
}

#[test]
fn config_layout() {
  let cfg = config(123);
  assert_eq!(cfg[0..8], 123u64.to_le_bytes());
  assert_eq!(cfg[12..16], 62u32.to_le_bytes());
  assert_eq!(cfg[20..24], 512u32.to_le_bytes());
}

#[test]
fn read_request_copies_sectors() {
  let mem = TestMemory::new(0x8000);
  let disk = TestDisk::new(8);
  three_part(&mem, T_IN, 2, 1024, DESC_F_WRITE);
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 0), Ok(1025));
  assert_eq!(status(&mem), S_OK);
  assert_eq!(mem.get::<512>(DATA), [3u8; 512]);
  assert_eq!(mem.get::<512>(DATA + 512), [4u8; 512]);
}

#[test]
fn write_request_reaches_disk() {
  let mem = TestMemory::new(0x8000);
  let disk = TestDisk::new(8);
  mem.put(DATA, &[0x5a; 1024]);
  three_part(&mem, T_OUT, 6, 1024, 0);
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 0), Ok(1));
  assert_eq!(status(&mem), S_OK);
  assert_eq!(disk.bytes.borrow()[6 * 512..], [0x5a; 1024]);
  assert_eq!(disk.bytes.borrow()[6 * 512 - 1], 6);
}

#[test]
fn multiple_data_descriptors_advance_the_sector() {
  let mem = TestMemory::new(0x8000);
  let disk = TestDisk::new(8);
  put_req(&mem, T_IN, 1);
  put_desc(&mem, 0, REQ, 16, DESC_F_NEXT, 1);
  put_desc(&mem, 1, DATA, 512, DESC_F_WRITE | DESC_F_NEXT, 2);
  put_desc(&mem, 2, DATA + 0x800, 512, DESC_F_WRITE | DESC_F_NEXT, 3);
  put_desc(&mem, 3, STATUS, 1, DESC_F_WRITE, 0);
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 0), Ok(1025));
  assert_eq!(mem.get::<512>(DATA), [2u8; 512]);
  assert_eq!(mem.get::<512>(DATA + 0x800), [3u8; 512]);
}

#[test]
fn out_of_range_and_misdirected_io_report_ioerr() {
  let mem = TestMemory::new(0x8000);
  let disk = TestDisk::new(8);

  three_part(&mem, T_IN, 8, 512, DESC_F_WRITE);
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 0), Ok(513));
  assert_eq!(status(&mem), S_IOERR);

  three_part(&mem, T_IN, u64::MAX, 512, DESC_F_WRITE);
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 0), Ok(513));
  assert_eq!(status(&mem), S_IOERR);

  three_part(&mem, T_IN, 0, 100, DESC_F_WRITE);
  request(&mem, &disk, DESC_BASE, 8, 0).unwrap();
  assert_eq!(status(&mem), S_IOERR);

  three_part(&mem, T_IN, 0, 512, 0);
  request(&mem, &disk, DESC_BASE, 8, 0).unwrap();
  assert_eq!(status(&mem), S_IOERR);

  three_part(&mem, T_OUT, 0, 512, DESC_F_WRITE);
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 0), Ok(1));
  assert_eq!(status(&mem), S_IOERR);
  assert_eq!(disk.bytes.borrow()[0], 1);

  three_part(&mem, T_OUT, 0, 512, 0);
  put_desc(&mem, 1, BASE + 0x7f00, 512, DESC_F_NEXT, 2);
  request(&mem, &disk, DESC_BASE, 8, 0).unwrap();
  assert_eq!(status(&mem), S_IOERR);
}

#[test]
fn flush_and_unknown_types() {
  let mem = TestMemory::new(0x8000);
  let mut disk = TestDisk::new(8);

  put_req(&mem, T_FLUSH, 0);
  put_desc(&mem, 0, REQ, 16, DESC_F_NEXT, 1);
  put_desc(&mem, 1, STATUS, 1, DESC_F_WRITE, 0);
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 0), Ok(1));
  assert_eq!(status(&mem), S_OK);
  assert_eq!(disk.synced.get(), 1);

  disk.fail_sync = true;
  request(&mem, &disk, DESC_BASE, 8, 0).unwrap();
  assert_eq!(status(&mem), S_IOERR);

  three_part(&mem, T_FLUSH, 0, 512, 0);
  request(&mem, &disk, DESC_BASE, 8, 0).unwrap();
  assert_eq!(status(&mem), S_IOERR);
  assert_eq!(disk.synced.get(), 2);

  three_part(&mem, 9, 0, 512, 0);
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 0), Ok(1));
  assert_eq!(status(&mem), S_UNSUPP);

  put_req(&mem, T_IN, 0);
  put_desc(&mem, 0, REQ, 16, DESC_F_NEXT, 1);
  put_desc(&mem, 1, STATUS, 1, DESC_F_WRITE, 0);
  request(&mem, &disk, DESC_BASE, 8, 0).unwrap();
  assert_eq!(status(&mem), S_IOERR);
}

#[test]
fn malformed_chains_are_rejected() {
  let mem = TestMemory::new(0x8000);
  let disk = TestDisk::new(8);

  three_part(&mem, T_IN, 0, 512, DESC_F_WRITE);
  assert_eq!(request(&mem, &disk, DESC_BASE, 0, 0), Err(EINVAL));
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 8), Err(EINVAL));
  assert_eq!(request(&mem, &disk, DESC_BASE, 128, 0), Err(EINVAL));

  put_desc(&mem, 0, REQ, 16, DESC_F_NEXT | DESC_F_WRITE, 1);
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 0), Err(EINVAL));
  put_desc(&mem, 0, REQ, 16, 0, 1);
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 0), Err(EINVAL));
  put_desc(&mem, 0, REQ, 16, DESC_F_NEXT, 8);
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 0), Err(EINVAL));
  put_desc(&mem, 0, REQ, 15, DESC_F_NEXT, 1);
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 0), Err(EINVAL));
  put_desc(&mem, 0, BASE + 0x7ff8, 16, DESC_F_NEXT, 1);
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 0), Err(EFAULT));

  three_part(&mem, T_IN, 0, 512, DESC_F_WRITE);
  put_desc(&mem, 1, DATA, 512, DESC_F_WRITE | DESC_F_NEXT, 0);
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 0), Err(EINVAL));
  put_desc(&mem, 1, DATA, 512, DESC_F_WRITE | DESC_F_NEXT, 1);
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 0), Err(EINVAL));
  put_desc(&mem, 1, DATA, 512, DESC_F_WRITE | DESC_F_NEXT, 9);
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 0), Err(EINVAL));

  three_part(&mem, T_IN, 0, 512, DESC_F_WRITE);
  put_desc(&mem, 2, STATUS, 1, 0, 0);
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 0), Err(EINVAL));
  put_desc(&mem, 2, STATUS, 0, DESC_F_WRITE, 0);
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 0), Err(EINVAL));
  put_desc(&mem, 2, 0, 1, DESC_F_WRITE, 0);
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 0), Err(EINVAL));
  put_desc(&mem, 2, BASE + 0x8000, 1, DESC_F_WRITE, 0);
  assert_eq!(request(&mem, &disk, DESC_BASE, 8, 0), Err(EFAULT));
}
