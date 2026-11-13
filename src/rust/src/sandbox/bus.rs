use super::guest::ffi::Vm;
use super::host::ant_hvf_vm_devices;
use super::p9::{P9, MAX_MSIZE};
use super::netdev::VmNet;
use super::virtio::ffi::{new_handle, with_device, Handle};
use super::virtio::{select_width, Kind, Params};
use core::ffi::{c_char, c_uint, CStr};
use core::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};

pub const ANT_HVF_VIRTIO_BAR_SIZE: u32 = 0x1000;
pub const ANT_HVF_9P_MAX_MSIZE: u32 = 65536;
pub const ANT_HVF_NET_MAX_PACKET: u32 = 2048;

const _: () = assert!(ANT_HVF_9P_MAX_MSIZE as usize == MAX_MSIZE);
const _: () = assert!(ANT_HVF_NET_MAX_PACKET as usize == super::net::MAX_PACKET);

struct Entry {
  slot: u32,
  kind: Kind,
  handle: Box<Handle>,
}

pub struct Bus {
  ecam_base: u64,
  ecam_size: u64,
  entries: Vec<Entry>,
  net: Mutex<Option<Arc<VmNet>>>,
  mac: Mutex<[u8; 6]>,
  capabilities: AtomicU32,
}


fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
  mutex.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

pub(crate) unsafe fn bus_of<'a>(vm: *const Vm) -> Option<&'a Bus> {
  if vm.is_null() {
    return None;
  }
  ant_hvf_vm_devices(vm).as_ref()
}

fn pointer(entry: &Entry) -> *mut Handle {
  (&raw const *entry.handle).cast_mut()
}

impl Bus {
  fn visible(&self, entry: &Entry) -> bool {
    entry.kind != Kind::Net || lock(&self.net).is_some()
  }

  pub(crate) fn device(&self, kind: Kind) -> *mut Handle {
    self.entries.iter().find(|entry| entry.kind == kind).map_or(core::ptr::null_mut(), pointer)
  }

  pub(crate) fn devices(&self, kind: Kind) -> impl Iterator<Item = *mut Handle> + '_ {
    self.entries.iter().filter(move |entry| entry.kind == kind).map(pointer)
  }

  fn at(&self, slot: u32) -> *mut Handle {
    self.entries.iter().find(|entry| entry.slot == slot && self.visible(entry)).map_or(core::ptr::null_mut(), pointer)
  }

  fn at_function(&self, bus: u32, slot: u32, function: u32) -> *mut Handle {
    if bus != 0 || function != 0 {
      return core::ptr::null_mut();
    }
    self.at(slot)
  }

  unsafe fn for_bar(&self, addr: u64) -> *mut Handle {
    for entry in self.entries.iter().filter(|entry| self.visible(entry)) {
      let dev = pointer(entry);
      let bar0 = with_device(dev, |device| device.bar0()).unwrap_or(u32::MAX);
      if bar0 != u32::MAX && (u64::from(bar0)..u64::from(bar0) + u64::from(ANT_HVF_VIRTIO_BAR_SIZE)).contains(&addr) {
        return dev;
      }
    }
    core::ptr::null_mut()
  }

  fn ecam(&self, addr: u64) -> Option<(u32, u32, u32, u32)> {
    let off = addr.checked_sub(self.ecam_base).filter(|&off| off < self.ecam_size)?;
    Some((((off >> 20) & 0xff) as u32, ((off >> 15) & 0x1f) as u32, ((off >> 12) & 0x7) as u32, (off & 0xfff) as u32))
  }

  unsafe fn config_read32(&self, bus: u32, slot: u32, function: u32, reg: u32) -> u32 {
    if bus == 0 && slot == 0 && function == 0 && reg & !3 == 0x0c {
      return 0;
    }
    with_device(self.at_function(bus, slot, function), |device| device.pci_read32(reg)).unwrap_or(u32::MAX)
  }

  unsafe fn config_write(&self, bus: u32, slot: u32, function: u32, reg: u32, size: u32, value: u64) {
    with_device(self.at_function(bus, slot, function), |device| device.pci_write(reg, size, value));
  }

  pub(crate) fn net(&self) -> Option<Arc<VmNet>> {
    lock(&self.net).clone()
  }

  pub(crate) fn set_net(&self, net: Option<Arc<VmNet>>) -> Option<Arc<VmNet>> {
    core::mem::replace(&mut lock(&self.net), net)
  }

  pub(crate) fn mac(&self) -> [u8; 6] {
    *lock(&self.mac)
  }

  pub(crate) fn set_mac(&self, mac: [u8; 6]) {
    *lock(&self.mac) = mac;
  }

  pub(crate) fn capabilities(&self) -> u32 {
    self.capabilities.load(Ordering::Relaxed)
  }
}

#[no_mangle]
pub unsafe extern "C" fn ant_devices_new(ecam_base: u64, ecam_size: u64) -> *mut Bus {
  Box::into_raw(Box::new(Bus {
    ecam_base,
    ecam_size,
    entries: Vec::new(),
    net: Mutex::new(None),
    mac: Mutex::new([0; 6]),
    capabilities: AtomicU32::new(0),
  }))
}

#[no_mangle]
pub unsafe extern "C" fn ant_devices_free(bus: *mut Bus) {
  if bus.is_null() {
    return;
  }
  let bus = Box::from_raw(bus);
  if let Some(net) = bus.set_net(None) {
    net.stop();
  }
}

#[no_mangle]
pub unsafe extern "C" fn ant_devices_set_capabilities(bus: *mut Bus, capabilities: u32) {
  if let Some(bus) = bus.as_ref() {
    bus.capabilities.store(capabilities, Ordering::Relaxed);
  }
}

#[no_mangle]
pub unsafe extern "C" fn ant_devices_at(bus: *const Bus, slot: c_uint) -> *mut Handle {
  bus.as_ref().map_or(core::ptr::null_mut(), |bus| bus.at(slot))
}

unsafe fn add(bus: *mut Bus, slot: u8, params: Params) -> *mut Handle {
  let Some(bus) = bus.as_mut() else { return core::ptr::null_mut() };
  let kind = params.kind;
  bus.entries.retain(|entry| entry.slot != u32::from(slot));
  bus.entries.push(Entry { slot: u32::from(slot), kind, handle: new_handle(params) });
  bus.entries.last().map_or(core::ptr::null_mut(), pointer)
}

#[no_mangle]
pub unsafe extern "C" fn ant_devices_add_block(bus: *mut Bus, slot: u8, bar0: u32) -> *mut Handle {
  add(bus, slot, Params::block(bar0))
}

#[no_mangle]
pub unsafe extern "C" fn ant_devices_add_net(bus: *mut Bus, slot: u8, bar0: u32) -> *mut Handle {
  add(bus, slot, Params::net(bar0))
}

#[no_mangle]
pub unsafe extern "C" fn ant_devices_add_vsock(bus: *mut Bus, slot: u8, bar0: u32) -> *mut Handle {
  add(bus, slot, Params::vsock(bar0))
}

#[no_mangle]
pub unsafe extern "C" fn ant_devices_add_rng(bus: *mut Bus, slot: u8, bar0: u32) -> *mut Handle {
  add(bus, slot, Params::rng(bar0))
}

#[no_mangle]
pub unsafe extern "C" fn ant_devices_add_share(
  bus: *mut Bus,
  slot: u8,
  bar0: u32,
  root: *const c_char,
  tag: *const c_char,
  readonly: bool,
) -> *mut Handle {
  if root.is_null() || tag.is_null() {
    return core::ptr::null_mut();
  }
  let tag = CStr::from_ptr(tag).to_bytes();
  let Some(share) = P9::new(CStr::from_ptr(root).to_bytes(), tag, readonly, MAX_MSIZE) else {
    return core::ptr::null_mut();
  };
  let dev = add(bus, slot, Params::share(bar0, tag.len()));
  with_device(dev, |device| device.set_p9(share));
  dev
}

#[no_mangle]
pub unsafe extern "C" fn ant_hvf_virtio_for_bar(vm: *mut Vm, addr: u64) -> *mut Handle {
  match bus_of(vm) {
    Some(bus) => bus.for_bar(addr),
    None => core::ptr::null_mut(),
  }
}

#[no_mangle]
pub unsafe extern "C" fn ant_hvf_pci_addr(
  vm: *mut Vm,
  addr: u64,
  bus_out: *mut c_uint,
  slot_out: *mut c_uint,
  function_out: *mut c_uint,
  reg_out: *mut c_uint,
) -> bool {
  let Some((bus, slot, function, reg)) = bus_of(vm).and_then(|devices| devices.ecam(addr)) else { return false };
  *bus_out = bus;
  *slot_out = slot;
  *function_out = function;
  *reg_out = reg;
  true
}

#[no_mangle]
pub unsafe extern "C" fn ant_hvf_pci_config_read32(vm: *mut Vm, bus: c_uint, slot: c_uint, function: c_uint, reg: c_uint) -> u32 {
  match bus_of(vm) {
    Some(devices) => devices.config_read32(bus, slot, function, reg),
    None => u32::MAX,
  }
}

#[no_mangle]
pub unsafe extern "C" fn ant_hvf_pci_config_write(
  vm: *mut Vm,
  bus: c_uint,
  slot: c_uint,
  function: c_uint,
  reg: c_uint,
  size: c_uint,
  value: u64,
) {
  if let Some(devices) = bus_of(vm) {
    devices.config_write(bus, slot, function, reg, size, value);
  }
}

#[no_mangle]
pub unsafe extern "C" fn ant_hvf_pci_mmio_read(vm: *mut Vm, addr: u64, size: c_uint, value: *mut u64) -> bool {
  let Some(devices) = bus_of(vm) else { return false };
  let Some((bus, slot, function, reg)) = devices.ecam(addr) else { return false };
  let word = devices.config_read32(bus, slot, function, reg);
  *value = select_width(u64::from(word), reg & 3, size);
  true
}

#[no_mangle]
pub unsafe extern "C" fn ant_hvf_pci_mmio_write(vm: *mut Vm, addr: u64, size: c_uint, value: u64) -> bool {
  let Some(devices) = bus_of(vm) else { return false };
  let Some((bus, slot, function, reg)) = devices.ecam(addr) else { return false };
  devices.config_write(bus, slot, function, reg, size, value);
  true
}
