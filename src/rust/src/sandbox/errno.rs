#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Errno(pub i32);

pub(crate) type Result<T> = core::result::Result<T, Errno>;

pub(crate) const EIO: Errno = Errno(libc::EIO);
pub(crate) const EFAULT: Errno = Errno(libc::EFAULT);
pub(crate) const EINVAL: Errno = Errno(libc::EINVAL);
pub(crate) const ENOSPC: Errno = Errno(libc::ENOSPC);
pub(crate) const ENOTSUP: Errno = Errno(libc::ENOTSUP);
pub(crate) const ELOOP: Errno = Errno(libc::ELOOP);
pub(crate) const EOVERFLOW: Errno = Errno(libc::EOVERFLOW);

#[cfg_attr(test, allow(dead_code))]
impl Errno {
  pub(crate) fn last() -> Errno {
    Errno(std::io::Error::last_os_error().raw_os_error().unwrap_or(EIO.0))
  }

  pub(crate) fn from_io(err: &std::io::Error) -> Errno {
    Errno(err.raw_os_error().unwrap_or(EIO.0))
  }
}

#[cfg_attr(test, allow(dead_code))]
pub(crate) fn to_c(result: Result<()>) -> i32 {
  match result {
    Ok(()) => 0,
    Err(Errno(code)) => -code,
  }
}
