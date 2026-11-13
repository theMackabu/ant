use super::errno::{Errno, Result, EINVAL};
use super::guest::ffi::{Vm, VmMemory};
use super::guest::GuestMemory;
use super::host::{self, ant_hvf_vm_verbose, ant_hvf_wake_vcpu};
use super::nat::{Config, Host, Net, Policy};
use super::bus::bus_of;
use super::net::{MAC_LEN, MAX_PACKET};
use super::virtio::ffi::{ant_hvf_virtio_interrupt, with_device, Handle};
use super::virtio::Kind;
use super::vring::{add_used, next_head, read_chain, write_chain, Queue};
use core::ffi::{c_int, c_uint, c_void};
use std::sync::Arc;

const RX_QUEUE: u32 = 0;
const TX_QUEUE: u32 = 1;
const QUEUE_COUNT: u32 = 2;
const TX_CHAIN_LIMIT: u32 = 256;
const HDR_LEN: usize = 12;


pub(crate) struct VmHost(*mut Vm);

unsafe impl Send for VmHost {}
unsafe impl Sync for VmHost {}

pub(crate) type VmNet = Net<VmHost>;

impl Host for VmHost {
  fn rx_ready(&self, net: &Net<Self>) {
    unsafe { drain_rx(self.0, net) };
  }

  fn wake(&self) {
    unsafe { ant_hvf_wake_vcpu(self.0) }
  }

  fn verbose(&self) -> bool {
    unsafe { ant_hvf_vm_verbose(self.0) }
  }

  fn log(&self, message: &str) {
    unsafe { host::log(self.0, message) }
  }
}

pub struct Builder {
  vm: *mut Vm,
  config: Config,
}

fn deliver_rx<M: GuestMemory>(mem: &M, queue: &mut Queue, net: &VmNet) -> Result<bool> {
  if !queue.ready() {
    return Ok(false);
  }
  let mut packet = [0u8; HDR_LEN + MAX_PACKET];
  let mut delivered = false;
  loop {
    let Some((_, body)) = packet.split_first_chunk_mut::<HDR_LEN>() else { break };
    let Some(len) = net.rx_peek(body) else { break };
    let Some(head) = next_head(mem, queue)? else { break };
    let Some(frame) = packet.get(..HDR_LEN + len) else { break };
    let used = write_chain(mem, queue.desc, head, u32::from(queue.size), frame)?;
    add_used(mem, queue.used, u32::from(queue.size), head, used)?;
    queue.last_avail = queue.last_avail.wrapping_add(1);
    net.rx_pop();
    delivered = true;
  }
  Ok(delivered)
}

fn take_tx<M: GuestMemory>(mem: &M, queue: &mut Queue, packet: &mut [u8; MAX_PACKET]) -> Result<Option<usize>> {
  let Some(head) = next_head(mem, queue)? else { return Ok(None) };
  let total = read_chain(mem, queue.desc, head, TX_CHAIN_LIMIT, packet)?;
  add_used(mem, queue.used, u32::from(queue.size), head, total)?;
  queue.last_avail = queue.last_avail.wrapping_add(1);
  Ok(Some(total as usize))
}

fn status(result: Option<Result<bool>>) -> core::result::Result<bool, c_int> {
  match result {
    Some(Ok(done)) => Ok(done),
    Some(Err(Errno(code))) => Err(-code),
    None => Ok(false),
  }
}

unsafe fn drain_rx(vm: *mut Vm, net: &VmNet) -> c_int {
  let Some(bus) = bus_of(vm) else { return 0 };
  let dev = bus.device(Kind::Net);
  let mem = VmMemory(vm);
  let delivered = with_device(dev, |device| match device.queue_mut(RX_QUEUE) {
    Some(queue) => deliver_rx(&mem, queue, net),
    None => Ok(false),
  });
  match status(delivered) {
    Ok(true) => ant_hvf_virtio_interrupt(vm, dev, RX_QUEUE),
    Ok(false) => 0,
    Err(rc) => rc,
  }
}

unsafe fn net_ref<'a>(net: *const c_void) -> Option<&'a VmNet> {
  net.cast::<VmNet>().as_ref()
}

unsafe fn vm_net(vm: *const Vm) -> Option<Arc<VmNet>> {
  bus_of(vm)?.net()
}

#[no_mangle]
pub unsafe extern "C" fn ant_hvf_vm_net(vm: *const Vm) -> *const c_void {
  vm_net(vm).map_or(core::ptr::null(), |net| Arc::as_ptr(&net).cast())
}

unsafe fn ant_hvf_virtio_net_drain_rx(vm: *mut Vm) -> c_int {
  match vm_net(vm) {
    Some(net) => drain_rx(vm, &net),
    None => 0,
  }
}

#[no_mangle]
pub unsafe extern "C" fn ant_hvf_virtio_net_drain_rx_if_wake(vm: *mut Vm) -> c_int {
  match vm_net(vm) {
    Some(net) if net.take_rx_wake() => drain_rx(vm, &net),
    _ => 0,
  }
}

pub(crate) unsafe fn ant_hvf_virtio_net_notify(vm: *mut Vm, dev: *mut Handle, queue: c_uint) -> c_int {
  if queue >= QUEUE_COUNT {
    return 0;
  }
  if queue == RX_QUEUE {
    return ant_hvf_virtio_net_drain_rx(vm);
  }

  let ready = with_device(dev, |device| device.queue(TX_QUEUE).is_some_and(Queue::ready));
  if ready != Some(true) {
    return 0;
  }
  let mem = VmMemory(vm);
  let mut packet = [0u8; MAX_PACKET];
  loop {
    let taken = with_device(dev, |device| match device.queue_mut(TX_QUEUE) {
      Some(queue) => take_tx(&mem, queue, &mut packet),
      None => Ok(None),
    });
    let total = match taken {
      Some(Ok(Some(total))) => total,
      Some(Err(Errno(code))) => return -code,
      _ => break,
    };
    let frame = packet.get(HDR_LEN..total).filter(|frame| !frame.is_empty());
    if let (Some(frame), Some(net)) = (frame, vm_net(vm)) {
      net.handle_frame(frame);
    }
  }
  ant_hvf_virtio_interrupt(vm, dev, TX_QUEUE)
}

#[no_mangle]
pub unsafe extern "C" fn ant_net_new(vm: *mut Vm, mac: *const u8, allow_host_all: bool, allow_lan: bool) -> *mut Builder {
  let config = Config {
    mac: *mac.cast::<[u8; MAC_LEN]>(),
    policy: Policy { allow_host_all, allow_lan, allow_host_ports: Vec::new() },
    forwards: Vec::new(),
  };
  Box::into_raw(Box::new(Builder { vm, config }))
}

#[no_mangle]
pub unsafe extern "C" fn ant_net_add_forward(builder: *mut Builder, host_port: u16, guest_port: u16) {
  if let Some(builder) = builder.as_mut() {
    builder.config.forwards.push((host_port, guest_port));
  }
}

#[no_mangle]
pub unsafe extern "C" fn ant_net_allow_host_port(builder: *mut Builder, port: u16) {
  if let Some(builder) = builder.as_mut() {
    builder.config.policy.allow_host_ports.push(port);
  }
}

#[no_mangle]
pub unsafe extern "C" fn ant_net_start(builder: *mut Builder) -> c_int {
  if builder.is_null() {
    return -EINVAL.0;
  }
  let builder = Box::from_raw(builder);
  let Some(bus) = bus_of(builder.vm) else { return -EINVAL.0 };
  bus.set_mac(builder.config.mac);
  match Net::start(VmHost(builder.vm), builder.config) {
    Ok(net) => {
      let forwards = net.forward_count();
      if let Some(previous) = bus.set_net(Some(net)) {
        previous.stop();
      }
      VmHost(builder.vm).log(&format!("network backend ready (native NAT, forwards={forwards})"));
      0
    }
    Err(err) => -Errno::from_io(&err).0,
  }
}

#[no_mangle]
pub unsafe extern "C" fn ant_hvf_net_stop(vm: *mut Vm) {
  if let Some(net) = bus_of(vm).and_then(|bus| bus.set_net(None)) {
    net.stop();
  }
}

#[no_mangle]
pub unsafe extern "C" fn ant_net_inject(net: *const c_void, frame: *const u8, len: usize) {
  if let (Some(net), false) = (net_ref(net), frame.is_null()) {
    net.handle_frame(core::slice::from_raw_parts(frame, len));
  }
}

#[no_mangle]
pub unsafe extern "C" fn ant_net_rx_pop(net: *const c_void, out: *mut u8, cap: usize) -> usize {
  let Some(net) = net_ref(net) else { return 0 };
  let Some(packet) = net.rx_pop() else { return 0 };
  if out.is_null() || packet.len() > cap {
    return 0;
  }
  core::ptr::copy_nonoverlapping(packet.as_ptr(), out, packet.len());
  packet.len()
}
