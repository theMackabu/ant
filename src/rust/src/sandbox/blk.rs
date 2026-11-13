use super::errno::{Result, EFAULT, EINVAL, EIO, ENOTSUP, EOVERFLOW};
use super::guest::GuestMemory;
use super::vring::{read_desc, Desc};

const T_IN: u32 = 0;
const T_OUT: u32 = 1;
const T_FLUSH: u32 = 4;
const S_OK: u8 = 0;
const S_IOERR: u8 = 1;
const S_UNSUPP: u8 = 2;
const SECTOR_SIZE: u64 = 512;
const QUEUE_SIZE: usize = 64;
const SEG_MAX: u32 = QUEUE_SIZE as u32 - 2;
const CONFIG_LEN: usize = 24;
const REQUEST_BYTES: usize = 16;

pub(crate) unsafe trait Disk {
  fn sectors(&self) -> u64;
  unsafe fn read_at(&self, dest: *mut u8, len: usize, off: u64) -> Result<usize>;
  unsafe fn write_at(&self, src: *const u8, len: usize, off: u64) -> Result<usize>;
  fn sync(&self) -> Result<()>;
}

pub(crate) fn config(sectors: u64) -> [u8; CONFIG_LEN] {
  let mut cfg = [0u8; CONFIG_LEN];
  cfg[0..8].copy_from_slice(&sectors.to_le_bytes());
  cfg[12..16].copy_from_slice(&SEG_MAX.to_le_bytes());
  cfg[20..24].copy_from_slice(&(SECTOR_SIZE as u32).to_le_bytes());
  cfg
}

fn check_bounds<D: Disk>(disk: &D, sector: u64, len: u32) -> Result<u64> {
  if u64::from(len) % SECTOR_SIZE != 0 {
    return Err(EINVAL);
  }
  let byte_off = sector.checked_mul(SECTOR_SIZE).ok_or(EOVERFLOW)?;
  let end = byte_off.checked_add(u64::from(len)).ok_or(EOVERFLOW)?;
  let image_bytes = disk.sectors().checked_mul(SECTOR_SIZE).ok_or(EOVERFLOW)?;
  if end > image_bytes {
    return Err(EIO);
  }
  if byte_off > i64::MAX as u64 {
    return Err(EOVERFLOW);
  }
  Ok(byte_off)
}

pub(crate) fn disk_read<M: GuestMemory, D: Disk>(mem: &M, disk: &D, sector: u64, guest_addr: u64, len: u32) -> Result<()> {
  let len_bytes = len as usize;
  let dest = mem.ptr(guest_addr, len_bytes).ok_or(EFAULT)?;
  let off = check_bounds(disk, sector, len)?;
  let mut done = 0usize;
  while done < len_bytes {
    let n = unsafe { disk.read_at(dest.add(done), len_bytes - done, off + done as u64)? };
    if n == 0 {
      unsafe { core::ptr::write_bytes(dest.add(done), 0, len_bytes - done) };
      return Ok(());
    }
    done += n.min(len_bytes - done);
  }
  Ok(())
}

pub(crate) fn disk_write<M: GuestMemory, D: Disk>(mem: &M, disk: &D, sector: u64, guest_addr: u64, len: u32) -> Result<()> {
  let len_bytes = len as usize;
  let src = mem.ptr(guest_addr, len_bytes).ok_or(EFAULT)?;
  let off = check_bounds(disk, sector, len)?;
  let mut done = 0usize;
  while done < len_bytes {
    let n = unsafe { disk.write_at(src.add(done), len_bytes - done, off + done as u64)? };
    if n == 0 {
      return Err(EIO);
    }
    done += n.min(len_bytes - done);
  }
  Ok(())
}

fn transfer<M: GuestMemory, D: Disk>(mem: &M, disk: &D, kind: u32, sector: u64, total: u64, desc: &Desc) -> Result<()> {
  total.checked_add(u64::from(desc.len)).ok_or(EOVERFLOW)?;
  let at = sector.checked_add(total / SECTOR_SIZE).ok_or(EOVERFLOW)?;
  match kind {
    T_IN => disk_read(mem, disk, at, desc.addr, desc.len),
    T_OUT => disk_write(mem, disk, at, desc.addr, desc.len),
    _ => Err(ENOTSUP),
  }
}

pub(crate) fn request<M: GuestMemory, D: Disk>(mem: &M, disk: &D, desc_base: u64, queue_size: u16, head: u16) -> Result<u32> {
  if queue_size == 0 || head >= queue_size {
    return Err(EINVAL);
  }
  let header = read_desc(mem, desc_base, head)?;
  if header.writable() || !header.has_next() || header.next >= queue_size {
    return Err(EINVAL);
  }
  if (header.len as usize) < REQUEST_BYTES {
    return Err(EINVAL);
  }
  let raw: [u8; REQUEST_BYTES] = mem.read_array(header.addr)?;
  let kind = u32::from_le_bytes([raw[0], raw[1], raw[2], raw[3]]);
  let sector = u64::from_le_bytes([raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]]);

  if usize::from(queue_size) > QUEUE_SIZE {
    return Err(EINVAL);
  }
  let mut visited = [false; QUEUE_SIZE];
  *visited.get_mut(usize::from(head)).ok_or(EINVAL)? = true;

  let mut status = S_OK;
  let mut next = header.next;
  let mut status_desc = Desc::default();
  let mut saw_data = false;
  let mut total = 0u64;

  for _ in 0..queue_size {
    let seen = visited.get_mut(usize::from(next)).ok_or(EINVAL)?;
    if core::mem::replace(seen, true) {
      return Err(EINVAL);
    }
    let desc = read_desc(mem, desc_base, next)?;
    if !desc.has_next() {
      status_desc = desc;
      break;
    }
    if desc.next >= queue_size {
      return Err(EINVAL);
    }
    saw_data = true;

    if (kind == T_IN && !desc.writable()) || (kind == T_OUT && desc.writable()) {
      status = S_IOERR;
    } else if kind == T_FLUSH {
      status = S_UNSUPP;
    }
    if status == S_OK {
      match transfer(mem, disk, kind, sector, total, &desc) {
        Ok(()) => {}
        Err(err) if err == ENOTSUP => status = S_UNSUPP,
        Err(_) => status = S_IOERR,
      }
    }
    total = total.wrapping_add(u64::from(desc.len));
    next = desc.next;
  }

  if status_desc.addr == 0 || status_desc.len < 1 || !status_desc.writable() {
    return Err(EINVAL);
  }
  match kind {
    T_IN | T_OUT => {
      if !saw_data {
        status = S_IOERR;
      }
    }
    T_FLUSH => {
      if saw_data || disk.sync().is_err() {
        status = S_IOERR;
      }
    }
    _ => status = S_UNSUPP,
  }
  mem.write(status_desc.addr, &[status])?;

  let response_len = if kind == T_IN { total } else { 0 }.checked_add(1).ok_or(EOVERFLOW)?;
  u32::try_from(response_len).map_err(|_| EOVERFLOW)
}

#[cfg(not(test))]
pub(crate) mod ffi {
  use super::*;
  use crate::sandbox::errno::{to_c, Errno};
  use crate::sandbox::guest::ffi::{Vm, VmMemory};
  use crate::sandbox::virtio::ffi::{notify_queue, Handle};
  use crate::sandbox::host::{ant_hvf_vm_image_fd, ant_hvf_vm_image_sectors};
  use core::ffi::c_int;


  struct Image {
    fd: c_int,
    sectors: u64,
  }

  impl Image {
    unsafe fn of(vm: *const Vm) -> Self {
      Self { fd: ant_hvf_vm_image_fd(vm), sectors: ant_hvf_vm_image_sectors(vm) }
    }
  }

  fn retry(mut call: impl FnMut() -> isize) -> Result<usize> {
    loop {
      let n = call();
      if n >= 0 {
        return Ok(n as usize);
      }
      let err = std::io::Error::last_os_error();
      if err.kind() != std::io::ErrorKind::Interrupted {
        return Err(Errno::from_io(&err));
      }
    }
  }

  unsafe impl Disk for Image {
    fn sectors(&self) -> u64 {
      self.sectors
    }

    unsafe fn read_at(&self, dest: *mut u8, len: usize, off: u64) -> Result<usize> {
      retry(|| libc::pread(self.fd, dest.cast(), len, off as libc::off_t))
    }

    unsafe fn write_at(&self, src: *const u8, len: usize, off: u64) -> Result<usize> {
      retry(|| libc::pwrite(self.fd, src.cast(), len, off as libc::off_t))
    }

    fn sync(&self) -> Result<()> {
      if unsafe { libc::fsync(self.fd) } == 0 {
        Ok(())
      } else {
        Err(Errno::last())
      }
    }
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_hvf_virtio_blk_config(vm: *const Vm, cfg: *mut u8, cfg_len: usize) {
    if cfg.is_null() || cfg_len < CONFIG_LEN {
      return;
    }
    let bytes = config(ant_hvf_vm_image_sectors(vm));
    let out = core::slice::from_raw_parts_mut(cfg, CONFIG_LEN);
    out[0..8].copy_from_slice(&bytes[0..8]);
    out[12..16].copy_from_slice(&bytes[12..16]);
    out[20..24].copy_from_slice(&bytes[20..24]);
  }



  #[no_mangle]
  pub unsafe extern "C" fn ant_hvf_virtio_blk_request(
    vm: *mut Vm,
    desc_base: u64,
    queue_size: u16,
    head: u16,
    used_len: *mut u32,
  ) -> c_int {
    to_c(request(&VmMemory(vm), &Image::of(vm), desc_base, queue_size, head).map(|len| *used_len = len))
  }

  pub(crate) unsafe fn ant_hvf_virtio_blk_notify(vm: *mut Vm, dev: *mut Handle) -> c_int {
    let mem = VmMemory(vm);
    let image = Image::of(vm);
    notify_queue(vm, dev, 0, |desc_base, queue_size, head| request(&mem, &image, desc_base, queue_size, head))
  }
}

#[cfg(test)]
#[path = "tests/blk.rs"]
mod tests;
