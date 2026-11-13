use super::errno::{Result, EFAULT};

pub(crate) unsafe trait GuestMemory {
  fn ptr(&self, addr: u64, len: usize) -> Option<*mut u8>;

  fn read(&self, addr: u64, out: &mut [u8]) -> Result<()> {
    let src = self.ptr(addr, out.len()).ok_or(EFAULT)?;
    unsafe { core::ptr::copy_nonoverlapping(src, out.as_mut_ptr(), out.len()) };
    Ok(())
  }

  fn write(&self, addr: u64, src: &[u8]) -> Result<()> {
    let dest = self.ptr(addr, src.len()).ok_or(EFAULT)?;
    unsafe { core::ptr::copy_nonoverlapping(src.as_ptr(), dest, src.len()) };
    Ok(())
  }

  fn read_array<const N: usize>(&self, addr: u64) -> Result<[u8; N]> {
    let mut out = [0u8; N];
    self.read(addr, &mut out)?;
    Ok(out)
  }

  fn read_u16(&self, addr: u64) -> Result<u16> {
    Ok(u16::from_le_bytes(self.read_array(addr)?))
  }
}

#[cfg(not(test))]
pub(crate) mod ffi {
  use super::GuestMemory;
  use crate::sandbox::host::ant_hvf_guest_ptr;

  #[repr(C)]
  pub struct Vm {
    _opaque: [u8; 0],
  }


  pub(crate) struct VmMemory(pub *mut Vm);

  unsafe impl GuestMemory for VmMemory {
    fn ptr(&self, addr: u64, len: usize) -> Option<*mut u8> {
      let ptr = unsafe { ant_hvf_guest_ptr(self.0, addr, len) };
      (!ptr.is_null()).then_some(ptr.cast())
    }
  }
}

#[cfg(test)]
#[path = "tests/guest_support.rs"]
pub(crate) mod testing;
