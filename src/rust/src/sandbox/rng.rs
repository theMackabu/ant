use super::errno::{Result, EFAULT, EINVAL, ELOOP, EOVERFLOW};
use super::guest::GuestMemory;
use super::vring::read_desc;

const QUEUE_SIZE: usize = 32;

#[cfg(target_os = "macos")]
unsafe fn entropy_fill(dest: *mut u8, len: usize) -> Result<()> {
  libc::arc4random_buf(dest.cast(), len);
  Ok(())
}

#[cfg(target_os = "linux")]
unsafe fn entropy_fill(mut dest: *mut u8, mut len: usize) -> Result<()> {
  use super::errno::{Errno, EIO};
  use std::io::{ErrorKind, Read};

  while len > 0 {
    let n = libc::getrandom(dest.cast(), len, 0);
    if n < 0 {
      let err = std::io::Error::last_os_error();
      if err.kind() == ErrorKind::Interrupted {
        continue;
      }
      if err.raw_os_error() == Some(libc::ENOSYS) {
        break;
      }
      return Err(Errno::from_io(&err));
    }
    if n == 0 {
      break;
    }
    dest = dest.add(n as usize);
    len -= n as usize;
  }
  if len == 0 {
    return Ok(());
  }

  let mut urandom = std::fs::File::open("/dev/urandom").map_err(|err| Errno::from_io(&err))?;
  let mut chunk = [0u8; 256];
  while len > 0 {
    let want = len.min(chunk.len());
    match urandom.read(&mut chunk[..want]) {
      Ok(0) => return Err(EIO),
      Ok(n) => {
        core::ptr::copy_nonoverlapping(chunk.as_ptr(), dest, n);
        dest = dest.add(n);
        len -= n;
      }
      Err(err) if err.kind() == ErrorKind::Interrupted => {}
      Err(err) => return Err(Errno::from_io(&err)),
    }
  }
  Ok(())
}

pub(crate) fn request<M: GuestMemory>(mem: &M, desc_base: u64, queue_size: u16, head: u16) -> Result<u32> {
  if queue_size == 0 || head >= queue_size || usize::from(queue_size) > QUEUE_SIZE {
    return Err(EINVAL);
  }

  let mut visited = [false; QUEUE_SIZE];
  let mut index = head;
  let mut total = 0u32;
  for _ in 0..queue_size {
    if index >= queue_size {
      return Err(EINVAL);
    }
    let seen = visited.get_mut(usize::from(index)).ok_or(EINVAL)?;
    if core::mem::replace(seen, true) {
      return Err(EINVAL);
    }

    let desc = read_desc(mem, desc_base, index)?;
    if !desc.writable() {
      return Err(EINVAL);
    }
    let next_total = total.checked_add(desc.len).ok_or(EOVERFLOW)?;
    let dest = mem.ptr(desc.addr, desc.len as usize).ok_or(EFAULT)?;
    unsafe { entropy_fill(dest, desc.len as usize)? };
    total = next_total;

    if !desc.has_next() {
      return Ok(total);
    }
    index = desc.next;
  }
  Err(ELOOP)
}

#[cfg(not(test))]
pub(crate) mod ffi {
  use super::request;
  use crate::sandbox::guest::ffi::{Vm, VmMemory};
  use crate::sandbox::virtio::ffi::{notify_queue, Handle};
  use core::ffi::c_int;

  #[no_mangle]
  pub unsafe extern "C" fn ant_hvf_virtio_rng_notify(vm: *mut Vm, dev: *mut Handle) -> c_int {
    let mem = VmMemory(vm);
    notify_queue(vm, dev, 0, |desc_base, queue_size, head| request(&mem, desc_base, queue_size, head))
  }
}

#[cfg(test)]
#[path = "tests/rng.rs"]
mod tests;
