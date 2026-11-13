use super::p9::P9;
use super::vring::Queue;
use super::vsock::Vsock;
use std::sync::Arc;

pub(crate) const MAX_QUEUES: usize = 3;
const MSIX_VECTORS: usize = 4;
const NO_VECTOR: u16 = 0xffff;
const F_VERSION_1: u64 = 1 << 32;

const BAR_SIZE: u32 = 0x1000;
const COMMON_CFG: u32 = 0x000;
const COMMON_CFG_SIZE: usize = 0x038;
const NOTIFY_CFG: u64 = 0x100;
const ISR_CFG: u64 = 0x200;
const DEVICE_CFG: u64 = 0x300;
const MSIX_TABLE: u64 = 0x800;
const MSIX_PBA: u64 = 0x900;
const MSIX_ENTRY_SIZE: u64 = 16;
#[cfg_attr(test, allow(dead_code))]
pub(crate) const DEVICE_CONFIG_BYTES: usize = 1200;

const PCI_VENDOR: u16 = 0x1af4;
const PCI_DEVICE_MODERN_BASE: u16 = 0x1040;
const PCI_SUBVENDOR: u16 = 0x1af4;
const PCI_CONFIG_SIZE: usize = 256;
const PCI_STATUS_CAP_LIST: u16 = 0x0010;
const PCI_COMMAND_MASK: u32 = 0x0007;
const PCI_CAP_VENDOR: u8 = 0x09;
const PCI_CAP_MSIX: u8 = 0x11;
const PCI_MSIX_ENABLE: u16 = 0x8000;
const PCI_MSIX_MASK_ALL: u16 = 0x4000;
const PCI_MSIX_ENTRY_MASKED: u32 = 1;
const PCI_CONFIG_COMMAND: u32 = 0x04;
const PCI_CONFIG_BAR0: u32 = 0x10;

const CAP_COMMON_CFG: u8 = 1;
const CAP_NOTIFY_CFG: u8 = 2;
const CAP_ISR_CFG: u8 = 3;
const CAP_DEVICE_CFG: u8 = 4;
const CAP_COMMON_POS: usize = 0x40;
const CAP_NOTIFY_POS: usize = 0x50;
const CAP_ISR_POS: usize = 0x70;
const CAP_DEVICE_POS: usize = 0x80;
const CAP_MSIX_POS: usize = 0x90;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Kind {
  Block,
  Net,
  P9,
  Vsock,
  Rng,
}

impl Kind {
}

#[derive(Clone, Copy, Default)]
struct MsixEntry {
  msg_addr_lo: u32,
  msg_addr_hi: u32,
  msg_data: u32,
  vector_control: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Msi {
  pub addr: u64,
  pub data: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Effect {
  None,
  Notify(u32),
  Reset,
  Msi(Msi),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Read {
  Value(u64),
  DeviceConfig(u64),
}

pub(crate) struct Params {
  pub kind: Kind,
  pub virtio_id: u16,
  pub subsystem_id: u16,
  pub class_code: u8,
  pub subclass: u8,
  pub bar0: u32,
  pub features: u64,
  pub queue_count: u16,
  pub queue_size: u16,
  pub device_config_len: u16,
}

impl Params {
  fn preset(kind: Kind, id: u16, class_code: u8, bar0: u32, features: u64, queues: (u16, u16), config_len: u16) -> Params {
    Params {
      kind,
      virtio_id: id,
      subsystem_id: id,
      class_code,
      subclass: 0,
      bar0,
      features,
      queue_count: queues.0,
      queue_size: queues.1,
      device_config_len: config_len,
    }
  }

  pub(crate) fn block(bar0: u32) -> Params {
    Params::preset(Kind::Block, 2, 0x01, bar0, (1 << 2) | (1 << 6) | (1 << 9), (1, 64), 24)
  }

  pub(crate) fn net(bar0: u32) -> Params {
    Params::preset(Kind::Net, 1, 0x02, bar0, 0x20, (2, 256), 8)
  }

  pub(crate) fn vsock(bar0: u32) -> Params {
    Params::preset(Kind::Vsock, 19, 0x08, bar0, 0x1, (3, 32), 8)
  }

  pub(crate) fn rng(bar0: u32) -> Params {
    Params::preset(Kind::Rng, 4, 0xff, bar0, 0, (1, 32), 0)
  }

  pub(crate) fn share(bar0: u32, tag_len: usize) -> Params {
    let config_len = u16::try_from(tag_len.saturating_add(2)).unwrap_or(u16::MAX);
    Params::preset(Kind::P9, 9, 0x01, bar0, 0x1, (1, 32), config_len)
  }
}

pub struct Device {
  kind: Kind,
  virtio_id: u16,
  subsystem_id: u16,
  class_code: u8,
  subclass: u8,
  bar0: u32,
  pci_command: u32,
  msix_control: u16,
  msix_pba: u32,
  device_features: u64,
  driver_features: u64,
  device_feature_select: u32,
  driver_feature_select: u32,
  config_msix_vector: u16,
  queue_sel: u16,
  queue_count: u16,
  status: u8,
  isr: u8,
  config_generation: u8,
  device_config_len: u16,
  queues: [Queue; MAX_QUEUES],
  msix: [MsixEntry; MSIX_VECTORS],
  vsock: Option<Arc<Vsock>>,
  p9: Option<Box<P9>>,
}

fn width_mask(size: u32) -> u64 {
  1u64.checked_shl(size.saturating_mul(8)).map_or(u64::MAX, |bit| bit - 1)
}

pub(crate) fn select_width(value: u64, offset: u32, size: u32) -> u64 {
  value.checked_shr(offset.saturating_mul(8)).unwrap_or(0) & width_mask(size)
}

fn assign_width(target: u64, bits: u32, offset: u32, size: u32, value: u64) -> u64 {
  let full = width_mask(bits / 8);
  let mask = width_mask(size) & full;
  let shift = offset.saturating_mul(8);
  let field = mask.checked_shl(shift).unwrap_or(0) & full;
  let placed = (value & mask).checked_shl(shift).unwrap_or(0) & full;
  (target & !field) | placed
}

fn assign32(target: &mut u32, offset: u32, size: u32, value: u64) {
  *target = assign_width(u64::from(*target), 32, offset, size, value) as u32;
}

fn assign16(target: &mut u16, offset: u32, size: u32, value: u64) {
  *target = assign_width(u64::from(*target), 16, offset, size, value) as u16;
}

fn assign64(target: &mut u64, offset: u32, size: u32, value: u64) {
  *target = assign_width(*target, 64, offset, size, value);
}

fn word_at(bytes: &[u8], off: u64, size: u32) -> u64 {
  let mut word = 0u64;
  for i in 0..size.min(8) {
    let byte = usize::try_from(off + u64::from(i)).ok().and_then(|at| bytes.get(at));
    if let Some(&byte) = byte {
      word |= u64::from(byte) << (i * 8);
    }
  }
  word
}

fn put16(cfg: &mut [u8; PCI_CONFIG_SIZE], at: usize, value: u16) {
  if let Some(field) = cfg.get_mut(at..at + 2) {
    field.copy_from_slice(&value.to_le_bytes());
  }
}

fn put32(cfg: &mut [u8; PCI_CONFIG_SIZE], at: usize, value: u32) {
  if let Some(field) = cfg.get_mut(at..at + 4) {
    field.copy_from_slice(&value.to_le_bytes());
  }
}

fn put_cap(cfg: &mut [u8; PCI_CONFIG_SIZE], pos: usize, next: usize, kind: u8, offset: u32, length: u32, notify: bool) {
  let header = [PCI_CAP_VENDOR, next as u8, if notify { 20 } else { 16 }, kind];
  if let Some(field) = cfg.get_mut(pos..pos + 4) {
    field.copy_from_slice(&header);
  }
  put32(cfg, pos + 4, 0);
  put32(cfg, pos + 8, offset);
  put32(cfg, pos + 12, length);
  if notify {
    put32(cfg, pos + 16, 2);
  }
}

fn queue_size_ok(queue: &Queue, size: u16) -> bool {
  size != 0 && size <= queue.max_size && size.is_power_of_two()
}

impl Device {
  pub(crate) fn new(params: Params) -> Device {
    let mut queues: [Queue; MAX_QUEUES] = Default::default();
    for (index, queue) in queues.iter_mut().enumerate().take(usize::from(params.queue_count)) {
      queue.size = params.queue_size;
      queue.max_size = params.queue_size;
      queue.msix_vector = NO_VECTOR;
      queue.notify_off = index as u16;
    }
    Device {
      kind: params.kind,
      virtio_id: params.virtio_id,
      subsystem_id: params.subsystem_id,
      class_code: params.class_code,
      subclass: params.subclass,
      bar0: params.bar0,
      pci_command: 0,
      msix_control: 0,
      msix_pba: 0,
      device_features: params.features | F_VERSION_1,
      driver_features: 0,
      device_feature_select: 0,
      driver_feature_select: 0,
      config_msix_vector: NO_VECTOR,
      queue_sel: 0,
      queue_count: params.queue_count,
      status: 0,
      isr: 0,
      config_generation: 0,
      device_config_len: params.device_config_len,
      queues,
      msix: [MsixEntry { vector_control: PCI_MSIX_ENTRY_MASKED, ..MsixEntry::default() }; MSIX_VECTORS],
      vsock: (params.kind == Kind::Vsock).then(|| Arc::new(Vsock::default())),
      p9: None,
    }
  }

  pub(crate) fn set_p9(&mut self, p9: P9) {
    self.p9 = Some(Box::new(p9));
  }

  pub(crate) fn p9_mut(&mut self) -> Option<&mut P9> {
    self.p9.as_deref_mut()
  }

  pub(crate) fn p9_queue(&mut self) -> Option<(&mut Queue, &mut P9)> {
    Some((self.queues.first_mut()?, self.p9.as_deref_mut()?))
  }

  pub(crate) fn vsock(&self) -> Option<Arc<Vsock>> {
    self.vsock.clone()
  }

  pub(crate) fn kind(&self) -> Kind {
    self.kind
  }

  pub(crate) fn bar0(&self) -> u32 {
    self.bar0
  }

  fn live_queues(&self) -> usize {
    usize::from(self.queue_count).min(MAX_QUEUES)
  }

  pub(crate) fn queue(&self, index: u32) -> Option<&Queue> {
    self.queues.get(usize::try_from(index).ok()?)
  }

  pub(crate) fn queue_mut(&mut self, index: u32) -> Option<&mut Queue> {
    self.queues.get_mut(usize::try_from(index).ok()?)
  }

  fn selected_queue(&mut self) -> Option<&mut Queue> {
    let index = usize::from(self.queue_sel);
    if index >= self.live_queues() {
      return None;
    }
    self.queues.get_mut(index)
  }

  pub(crate) fn reset(&mut self) {
    self.driver_features = 0;
    self.device_feature_select = 0;
    self.driver_feature_select = 0;
    self.config_msix_vector = NO_VECTOR;
    self.queue_sel = 0;
    self.status = 0;
    self.isr = 0;
    let live = self.live_queues();
    for queue in self.queues.iter_mut().take(live) {
      queue.desc = 0;
      queue.avail = 0;
      queue.used = 0;
      queue.last_avail = 0;
      queue.size = queue.max_size;
      queue.enabled = false;
      queue.msix_vector = NO_VECTOR;
    }
    if let Some(vsock) = &self.vsock {
      vsock.reset();
    }
    if let Some(p9) = &mut self.p9 {
      p9.reset();
    }
  }

  fn common_bytes(&mut self) -> [u8; COMMON_CFG_SIZE] {
    let device_features = if self.device_feature_select == 0 {
      self.device_features as u32
    } else {
      (self.device_features >> 32) as u32
    };
    let driver_features = if self.driver_feature_select == 0 {
      self.driver_features as u32
    } else {
      (self.driver_features >> 32) as u32
    };
    let mut out = [0u8; COMMON_CFG_SIZE];
    out[0..4].copy_from_slice(&self.device_feature_select.to_le_bytes());
    out[4..8].copy_from_slice(&device_features.to_le_bytes());
    out[8..12].copy_from_slice(&self.driver_feature_select.to_le_bytes());
    out[12..16].copy_from_slice(&driver_features.to_le_bytes());
    out[16..18].copy_from_slice(&self.config_msix_vector.to_le_bytes());
    out[18..20].copy_from_slice(&self.queue_count.to_le_bytes());
    out[20] = self.status;
    out[21] = self.config_generation;
    out[22..24].copy_from_slice(&self.queue_sel.to_le_bytes());
    out[26..28].copy_from_slice(&NO_VECTOR.to_le_bytes());
    if let Some(queue) = self.selected_queue() {
      out[24..26].copy_from_slice(&queue.size.to_le_bytes());
      out[26..28].copy_from_slice(&queue.msix_vector.to_le_bytes());
      out[28..30].copy_from_slice(&u16::from(queue.enabled).to_le_bytes());
      out[30..32].copy_from_slice(&queue.notify_off.to_le_bytes());
      out[32..40].copy_from_slice(&queue.desc.to_le_bytes());
      out[40..48].copy_from_slice(&queue.avail.to_le_bytes());
      out[48..56].copy_from_slice(&queue.used.to_le_bytes());
    }
    out
  }

  fn msix_enabled(&self) -> bool {
    self.msix_control & PCI_MSIX_ENABLE != 0 && self.msix_control & PCI_MSIX_MASK_ALL == 0
  }

  fn msix_masked(&self, vector: usize) -> bool {
    match self.msix.get(vector) {
      Some(entry) => !self.msix_enabled() || entry.vector_control & PCI_MSIX_ENTRY_MASKED != 0,
      None => true,
    }
  }

  fn msix_notify(&mut self, vector: u16) -> Option<Msi> {
    let index = usize::from(vector);
    let entry = *self.msix.get(index)?;
    if self.msix_masked(index) {
      self.msix_pba |= 1 << index;
      return None;
    }
    Some(Msi { addr: u64::from(entry.msg_addr_lo) | (u64::from(entry.msg_addr_hi) << 32), data: entry.msg_data })
  }

  pub(crate) fn interrupt(&mut self, queue: u32) -> Option<Msi> {
    let live = self.live_queues();
    let vector = usize::try_from(queue)
      .ok()
      .filter(|&index| index < live)
      .and_then(|index| self.queues.get(index))
      .map_or(NO_VECTOR, |queue| queue.msix_vector);
    self.isr |= 1;
    self.msix_notify(vector)
  }

  fn msix_slot(off: u64) -> Option<(usize, u32)> {
    let rel = off.checked_sub(MSIX_TABLE)?;
    if rel >= MSIX_VECTORS as u64 * MSIX_ENTRY_SIZE {
      return None;
    }
    Some(((rel / MSIX_ENTRY_SIZE) as usize, (rel % MSIX_ENTRY_SIZE) as u32))
  }

  fn msix_read(&self, off: u64, size: u32) -> Option<u64> {
    if let Some((vector, entry_off)) = Self::msix_slot(off) {
      let entry = self.msix.get(vector)?;
      let word = match entry_off & !3 {
        0 => entry.msg_addr_lo,
        4 => entry.msg_addr_hi,
        8 => entry.msg_data,
        _ => entry.vector_control,
      };
      return Some(select_width(u64::from(word), entry_off & 3, size));
    }
    let pba_off = off.checked_sub(MSIX_PBA).filter(|&rel| rel < 8)?;
    Some(select_width(u64::from(self.msix_pba), pba_off as u32, size))
  }

  fn msix_write(&mut self, off: u64, size: u32, value: u64) -> Option<Effect> {
    let (vector, entry_off) = Self::msix_slot(off)?;
    let was_masked = self.msix_masked(vector);
    let entry = self.msix.get_mut(vector)?;
    let field = match entry_off & !3 {
      0 => &mut entry.msg_addr_lo,
      4 => &mut entry.msg_addr_hi,
      8 => &mut entry.msg_data,
      _ => &mut entry.vector_control,
    };
    assign32(field, entry_off & 3, size, value);
    let pending = self.msix_pba & (1 << vector) != 0;
    if was_masked && !self.msix_masked(vector) && pending {
      self.msix_pba &= !(1 << vector);
      if let Some(msi) = self.msix_notify(vector as u16) {
        return Some(Effect::Msi(msi));
      }
    }
    Some(Effect::None)
  }

  fn in_notify_region(&self, off: u64) -> Option<u32> {
    let rel = off.checked_sub(NOTIFY_CFG)?;
    (rel < u64::from(self.queue_count) * 2).then_some((rel / 2) as u32)
  }

  fn in_device_config(&self, off: u64) -> Option<u64> {
    off.checked_sub(DEVICE_CFG).filter(|&rel| rel < u64::from(self.device_config_len))
  }

  pub(crate) fn common_read(&mut self, off: u64, size: u32) -> Read {
    if off < COMMON_CFG_SIZE as u64 {
      return Read::Value(word_at(&self.common_bytes(), off, size));
    }
    if off == ISR_CFG {
      return Read::Value(u64::from(core::mem::take(&mut self.isr)));
    }
    if let Some(rel) = self.in_device_config(off) {
      return Read::DeviceConfig(rel);
    }
    Read::Value(self.msix_read(off, size).unwrap_or(0))
  }

  pub(crate) fn common_write(&mut self, off: u64, size: u32, value: u64) -> Effect {
    if let Some(effect) = self.msix_write(off, size, value) {
      return effect;
    }
    if let Some(queue) = self.in_notify_region(off) {
      return Effect::Notify(queue);
    }
    if self.in_device_config(off).is_some() || off >= COMMON_CFG_SIZE as u64 {
      return Effect::None;
    }

    match off {
      0 => self.device_feature_select = value as u32,
      8 => self.driver_feature_select = value as u32,
      12 => match self.driver_feature_select {
        0 => assign64(&mut self.driver_features, 0, 4, value),
        1 => self.driver_features = (self.driver_features & u64::from(u32::MAX)) | (value << 32),
        _ => {}
      },
      16 => self.config_msix_vector = value as u16,
      20 => {
        if value as u8 == 0 {
          self.reset();
          return Effect::Reset;
        }
        self.status = value as u8;
      }
      22 => self.queue_sel = value as u16,
      _ => {
        let Some(queue) = self.selected_queue() else { return Effect::None };
        match off {
          24 => {
            if queue_size_ok(queue, value as u16) {
              queue.size = value as u16;
            }
          }
          26 => queue.msix_vector = value as u16,
          28 => queue.enabled = value as u16 != 0 && queue.size != 0,
          32..=39 => assign64(&mut queue.desc, (off - 32) as u32, size, value),
          40..=47 => assign64(&mut queue.avail, (off - 40) as u32, size, value),
          48..=55 => assign64(&mut queue.used, (off - 48) as u32, size, value),
          _ => {}
        }
      }
    }
    Effect::None
  }

  fn pci_config(&self) -> [u8; PCI_CONFIG_SIZE] {
    let mut cfg = [0u8; PCI_CONFIG_SIZE];
    put16(&mut cfg, 0x00, PCI_VENDOR);
    put16(&mut cfg, 0x02, PCI_DEVICE_MODERN_BASE.wrapping_add(self.virtio_id));
    put16(&mut cfg, 0x04, self.pci_command as u16);
    put16(&mut cfg, 0x06, PCI_STATUS_CAP_LIST);
    cfg[0x08] = 1;
    cfg[0x0a] = self.subclass;
    cfg[0x0b] = self.class_code;
    put32(&mut cfg, PCI_CONFIG_BAR0 as usize, self.bar0);
    put16(&mut cfg, 0x2c, PCI_SUBVENDOR);
    put16(&mut cfg, 0x2e, self.subsystem_id);
    cfg[0x34] = CAP_COMMON_POS as u8;
    cfg[0x3d] = 1;

    let has_device_config = self.device_config_len != 0;
    put_cap(&mut cfg, CAP_COMMON_POS, CAP_NOTIFY_POS, CAP_COMMON_CFG, COMMON_CFG, COMMON_CFG_SIZE as u32, false);
    put_cap(&mut cfg, CAP_NOTIFY_POS, CAP_ISR_POS, CAP_NOTIFY_CFG, NOTIFY_CFG as u32, u32::from(self.queue_count) * 2, true);
    let after_isr = if has_device_config { CAP_DEVICE_POS } else { CAP_MSIX_POS };
    put_cap(&mut cfg, CAP_ISR_POS, after_isr, CAP_ISR_CFG, ISR_CFG as u32, 1, false);
    if has_device_config {
      let len = u32::from(self.device_config_len);
      put_cap(&mut cfg, CAP_DEVICE_POS, CAP_MSIX_POS, CAP_DEVICE_CFG, DEVICE_CFG as u32, len, false);
    }

    cfg[CAP_MSIX_POS] = PCI_CAP_MSIX;
    let control = (self.msix_control & (PCI_MSIX_ENABLE | PCI_MSIX_MASK_ALL)) | (MSIX_VECTORS as u16 - 1);
    put16(&mut cfg, CAP_MSIX_POS + 2, control);
    put32(&mut cfg, CAP_MSIX_POS + 4, MSIX_TABLE as u32);
    put32(&mut cfg, CAP_MSIX_POS + 8, MSIX_PBA as u32);
    cfg
  }

  pub(crate) fn pci_read32(&self, reg: u32) -> u32 {
    let aligned = reg & !3;
    if aligned == PCI_CONFIG_BAR0 && self.bar0 == u32::MAX {
      return !(BAR_SIZE - 1);
    }
    word_at(&self.pci_config(), u64::from(aligned), 4) as u32
  }

  pub(crate) fn pci_write(&mut self, reg: u32, size: u32, value: u64) {
    const MSIX_CONTROL: u32 = CAP_MSIX_POS as u32 + 2;
    if reg & !3 == PCI_CONFIG_BAR0 {
      if reg == PCI_CONFIG_BAR0 && size == 4 {
        let next = value as u32;
        self.bar0 = if next == u32::MAX { u32::MAX } else { next & !(BAR_SIZE - 1) };
      } else {
        assign32(&mut self.bar0, reg & 3, size, value);
      }
    } else if reg & !3 == PCI_CONFIG_COMMAND && reg & 3 < 2 {
      assign32(&mut self.pci_command, reg & 3, size, value);
      self.pci_command &= PCI_COMMAND_MASK;
    } else if (MSIX_CONTROL..MSIX_CONTROL + 2).contains(&reg) {
      assign16(&mut self.msix_control, (reg - MSIX_CONTROL) & 1, size, value);
      self.msix_control &= PCI_MSIX_ENABLE | PCI_MSIX_MASK_ALL;
    }
  }
}

#[cfg(not(test))]
pub(crate) mod ffi {
  use super::*;
  use crate::sandbox::blk;
  use crate::sandbox::bus::bus_of;
  use crate::sandbox::errno::{Errno, Result};
  use crate::sandbox::guest::ffi::{Vm, VmMemory};
  use crate::sandbox::host::{self, ant_hvf_send_msi};
  use crate::sandbox::netdev;
  use crate::sandbox::p9;
  use crate::sandbox::rng;
  use crate::sandbox::vring::drain;
  use crate::sandbox::vsock;
  use core::ffi::{c_int, c_uint};
  use std::sync::Mutex;

  const VSOCK_GUEST_CID: u64 = 3;

  pub struct Handle(Mutex<Device>);


  pub(crate) fn new_handle(params: Params) -> Box<Handle> {
    Box::new(Handle(Mutex::new(Device::new(params))))
  }

  pub(crate) unsafe fn with_device<R>(dev: *const Handle, f: impl FnOnce(&mut Device) -> R) -> Option<R> {
    let mut device = dev.as_ref()?.0.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    Some(f(&mut device))
  }

  unsafe fn deliver(vm: *mut Vm, msi: Option<Msi>) -> c_int {
    match msi {
      Some(msi) => ant_hvf_send_msi(vm, msi.addr, msi.data),
      None => 0,
    }
  }

  pub(crate) unsafe fn notify_queue(
    vm: *mut Vm,
    dev: *mut Handle,
    index: u32,
    request: impl FnMut(u64, u16, u16) -> Result<u32>,
  ) -> c_int {
    let mem = VmMemory(vm);
    let drained = with_device(dev, |device| match device.queue_mut(index) {
      Some(queue) => drain(&mem, queue, request),
      None => Ok(false),
    });
    match drained {
      Some(Ok(true)) => ant_hvf_virtio_interrupt(vm, dev, index),
      Some(Ok(false)) | None => 0,
      Some(Err(Errno(code))) => -code,
    }
  }

  unsafe fn dispatch_notify(vm: *mut Vm, dev: *mut Handle, kind: Kind, queue: u32) {
    let rc = match kind {
      Kind::Block => blk::ffi::ant_hvf_virtio_blk_notify(vm, dev),
      Kind::Net => netdev::ant_hvf_virtio_net_notify(vm, dev, queue),
      Kind::P9 => p9::ffi::notify(vm, dev),
      Kind::Vsock => vsock::ffi::ant_hvf_virtio_vsock_notify(vm, queue),
      Kind::Rng => rng::ffi::ant_hvf_virtio_rng_notify(vm, dev),
    };
    if rc != 0 {
      host::log(vm, &format!("virtio {kind:?} queue {queue} failed: {}", std::io::Error::from_raw_os_error(-rc)));
    }
  }

  unsafe fn device_config(vm: *mut Vm, dev: *mut Handle, kind: Kind, cfg: &mut [u8; DEVICE_CONFIG_BYTES]) -> bool {
    match kind {
      Kind::Block => blk::ffi::ant_hvf_virtio_blk_config(vm, cfg.as_mut_ptr(), cfg.len()),
      Kind::Net => {
        let Some(bus) = bus_of(vm) else { return false };
        cfg[..6].copy_from_slice(&bus.mac());
      }
      Kind::P9 => {
        let Some(Some(tag)) = with_device(dev, |device| device.p9_mut().map(|p9| p9.tag().to_vec())) else {
          return false;
        };
        cfg[..2].copy_from_slice(&(tag.len() as u16).to_le_bytes());
        let copied = tag.len().min(cfg.len() - 2);
        cfg[2..2 + copied].copy_from_slice(&tag[..copied]);
      }
      Kind::Vsock => cfg[..8].copy_from_slice(&VSOCK_GUEST_CID.to_le_bytes()),
      Kind::Rng => {}
    }
    true
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_hvf_virtio_bar0(dev: *const Handle) -> u32 {
    with_device(dev, |device| device.bar0()).unwrap_or(u32::MAX)
  }



  pub(crate) unsafe fn ant_hvf_virtio_interrupt(vm: *mut Vm, dev: *mut Handle, queue: c_uint) -> c_int {
    let msi = with_device(dev, |device| device.interrupt(queue)).flatten();
    deliver(vm, msi)
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_hvf_virtio_common_read(
    vm: *mut Vm,
    dev: *mut Handle,
    off: u64,
    size: c_uint,
    value: *mut u64,
  ) -> bool {
    let Some((kind, read)) = with_device(dev, |device| (device.kind(), device.common_read(off, size))) else {
      return false;
    };
    match read {
      Read::Value(word) => *value = word,
      Read::DeviceConfig(rel) => {
        let mut cfg = [0u8; DEVICE_CONFIG_BYTES];
        if !device_config(vm, dev, kind, &mut cfg) {
          return false;
        }
        *value = word_at(&cfg, rel, size);
      }
    }
    true
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_hvf_virtio_common_write(
    vm: *mut Vm,
    dev: *mut Handle,
    off: u64,
    size: c_uint,
    value: u64,
  ) -> bool {
    let Some((kind, effect)) = with_device(dev, |device| (device.kind(), device.common_write(off, size, value))) else {
      return false;
    };
    match effect {
      Effect::None => {}
      Effect::Notify(queue) => dispatch_notify(vm, dev, kind, queue),
      Effect::Reset => {}
      Effect::Msi(msi) => {
        deliver(vm, Some(msi));
      }
    }
    true
  }


}

#[cfg(test)]
#[path = "tests/virtio.rs"]
mod tests;
