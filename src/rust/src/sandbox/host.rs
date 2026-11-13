use super::bus::Bus;
use super::guest::ffi::Vm;
use core::ffi::{c_char, c_int, c_void};

#[allow(improper_ctypes)]
extern "C" {
  pub(crate) fn ant_hvf_vm_devices(vm: *const Vm) -> *mut Bus;
  pub(crate) fn ant_hvf_guest_ptr(vm: *mut Vm, guest_addr: u64, len: usize) -> *mut c_void;
  pub(crate) fn ant_hvf_vm_image_fd(vm: *const Vm) -> c_int;
  pub(crate) fn ant_hvf_vm_image_sectors(vm: *const Vm) -> u64;
  pub(crate) fn ant_hvf_vm_verbose(vm: *const Vm) -> bool;
  pub(crate) fn ant_hvf_verbose(vm: *mut Vm, message: *const c_char);
  pub(crate) fn ant_hvf_wake_vcpu(vm: *mut Vm);
  pub(crate) fn ant_hvf_send_msi(vm: *mut Vm, addr: u64, data: u32) -> c_int;
  pub(crate) fn ant_hvf_vm_set_vsock_wake(vm: *mut Vm, pending: bool);
  pub(crate) fn ant_hvf_vm_kick_if_running(vm: *mut Vm);
  pub(crate) fn ant_hvf_vm_frame_handler(vm: *mut Vm, kind: u8, payload: *const u8, len: u32) -> bool;
  pub(crate) fn ant_sandbox_vm_display_frame(capabilities: u32, kind: u8, payload: *const u8, len: u32) -> c_int;
}

pub(crate) unsafe fn log(vm: *mut Vm, message: &str) {
  if !ant_hvf_vm_verbose(vm) {
    return;
  }
  if let Ok(message) = std::ffi::CString::new(message) {
    ant_hvf_verbose(vm, message.as_ptr());
  }
}
