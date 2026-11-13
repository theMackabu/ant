use super::errno::{Errno, Result, EINVAL, ENOSPC};
use super::guest::GuestMemory;
use super::vring::read_desc;
use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};
use std::sync::{Mutex, MutexGuard};

pub(crate) const HDR_LEN: usize = 44;
const QUEUE_SIZE: u32 = 32;
const TYPE_STREAM: u16 = 1;
const OP_REQUEST: u16 = 1;
const OP_RESPONSE: u16 = 2;
const OP_RST: u16 = 3;
const OP_SHUTDOWN: u16 = 4;
const OP_RW: u16 = 5;
const OP_CREDIT_UPDATE: u16 = 6;
const OP_CREDIT_REQUEST: u16 = 7;
const GUEST_CID: u64 = 3;
const HOST_CID: u64 = 2;
const HOST_PORT: u32 = 1024;
const BUF_ALLOC: u32 = 65536;
const MAX_PAYLOAD: usize = 2048;
const MAX_TX_PACKET: usize = HDR_LEN + BUF_ALLOC as usize;

const FRAME_MAGIC: &[u8; 4] = b"ANTF";
const FRAME_VERSION: u8 = 1;
const FRAME_HEADER_LEN: usize = 12;
pub(crate) const FRAME_MAX_SIZE: usize = 16 * 1024 * 1024;

const E2BIG: Errno = Errno(libc::E2BIG);

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct Hdr {
  pub src_cid: u64,
  pub dst_cid: u64,
  pub src_port: u32,
  pub dst_port: u32,
  pub len: u32,
  pub kind: u16,
  pub op: u16,
  pub flags: u32,
  pub buf_alloc: u32,
  pub fwd_cnt: u32,
}

impl Hdr {
  pub(crate) fn load(raw: &[u8; HDR_LEN]) -> Hdr {
    let u16_at = |at: usize| u16::from_le_bytes([raw[at], raw[at + 1]]);
    let u32_at = |at: usize| u32::from_le_bytes([raw[at], raw[at + 1], raw[at + 2], raw[at + 3]]);
    let u64_at = |at: usize| u64::from(u32_at(at)) | (u64::from(u32_at(at + 4)) << 32);
    Hdr {
      src_cid: u64_at(0),
      dst_cid: u64_at(8),
      src_port: u32_at(16),
      dst_port: u32_at(20),
      len: u32_at(24),
      kind: u16_at(28),
      op: u16_at(30),
      flags: u32_at(32),
      buf_alloc: u32_at(36),
      fwd_cnt: u32_at(40),
    }
  }

  pub(crate) fn store(&self) -> [u8; HDR_LEN] {
    let mut out = [0u8; HDR_LEN];
    out[0..8].copy_from_slice(&self.src_cid.to_le_bytes());
    out[8..16].copy_from_slice(&self.dst_cid.to_le_bytes());
    out[16..20].copy_from_slice(&self.src_port.to_le_bytes());
    out[20..24].copy_from_slice(&self.dst_port.to_le_bytes());
    out[24..28].copy_from_slice(&self.len.to_le_bytes());
    out[28..30].copy_from_slice(&self.kind.to_le_bytes());
    out[30..32].copy_from_slice(&self.op.to_le_bytes());
    out[32..36].copy_from_slice(&self.flags.to_le_bytes());
    out[36..40].copy_from_slice(&self.buf_alloc.to_le_bytes());
    out[40..44].copy_from_slice(&self.fwd_cnt.to_le_bytes());
    out
  }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum FrameOutcome {
  Handled,
  Exit(i32),
  Rejected,
}

pub(crate) trait Env {
  fn send(&self, packet: &[u8]) -> Result<bool>;
  fn frame(&self, kind: u8, payload: &[u8]) -> FrameOutcome;
  fn set_wake(&self, pending: bool);
  fn log(&self, message: &str);
}

struct Outgoing {
  data: Vec<u8>,
  off: usize,
}

#[derive(Default)]
struct Inner {
  connected: bool,
  response_sent: bool,
  peer_port: u32,
  peer_buf_alloc: u32,
  peer_fwd_cnt: u32,
  tx_cnt: u32,
  fwd_cnt: u32,
  rx_stream: Vec<u8>,
}

#[derive(Default)]
pub struct Vsock {
  inner: Mutex<Inner>,
  outgoing: Mutex<VecDeque<Outgoing>>,
  request_sent: AtomicBool,
  exit_received: AtomicBool,
  exit_code: AtomicI32,
  protocol_error: AtomicBool,
  transport_error: AtomicBool,
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
  mutex.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

pub(crate) fn read_tx_packet<M: GuestMemory>(mem: &M, desc_base: u64, head: u16) -> Result<(Hdr, Vec<u8>, u32)> {
  let mut readable = Vec::new();
  let mut total = 0usize;
  let mut index = head;
  for _ in 0..QUEUE_SIZE {
    let desc = read_desc(mem, desc_base, index)?;
    if !desc.writable() {
      let len = desc.len as usize;
      if len > MAX_TX_PACKET - total {
        return Err(E2BIG);
      }
      total += len;
      readable.push(desc);
    }
    if !desc.has_next() {
      break;
    }
    index = desc.next;
  }

  if total < HDR_LEN {
    return Err(EINVAL);
  }
  let mut raw = vec![0u8; total];
  let mut done = 0usize;
  for desc in &readable {
    let end = done + desc.len as usize;
    mem.read(desc.addr, raw.get_mut(done..end).ok_or(EINVAL)?)?;
    done = end;
  }

  let (head_bytes, body) = raw.split_first_chunk::<HDR_LEN>().ok_or(EINVAL)?;
  let hdr = Hdr::load(head_bytes);
  let payload = body.get(..hdr.len as usize).ok_or(EINVAL)?.to_vec();
  Ok((hdr, payload, total as u32))
}

pub(crate) fn write_iov<M: GuestMemory>(mem: &M, desc_base: u64, head: u16, data: &[u8]) -> Result<u32> {
  let mut index = head;
  let mut rest = data;
  for _ in 0..QUEUE_SIZE {
    if rest.is_empty() {
      break;
    }
    let desc = read_desc(mem, desc_base, index)?;
    if desc.writable() {
      let (chunk, after) = rest.split_at(rest.len().min(desc.len as usize));
      mem.write(desc.addr, chunk)?;
      rest = after;
    }
    if !desc.has_next() {
      break;
    }
    index = desc.next;
  }
  if !rest.is_empty() {
    return Err(ENOSPC);
  }
  u32::try_from(data.len()).map_err(|_| ENOSPC)
}

impl Inner {
  fn peer_space(&self) -> u32 {
    let in_flight = self.tx_cnt.wrapping_sub(self.peer_fwd_cnt);
    self.peer_buf_alloc.saturating_sub(in_flight)
  }

  fn update_peer_credit(&mut self, hdr: &Hdr) {
    self.peer_buf_alloc = hdr.buf_alloc;
    self.peer_fwd_cnt = hdr.fwd_cnt;
  }

  fn send<E: Env>(&mut self, env: &E, op: u16, payload: &[u8]) -> Result<bool> {
    let len = u32::try_from(payload.len()).map_err(|_| E2BIG)?;
    let hdr = Hdr {
      src_cid: HOST_CID,
      dst_cid: GUEST_CID,
      src_port: HOST_PORT,
      dst_port: self.peer_port,
      len,
      kind: TYPE_STREAM,
      op,
      flags: 0,
      buf_alloc: BUF_ALLOC,
      fwd_cnt: self.fwd_cnt,
    };
    let mut packet = Vec::with_capacity(HDR_LEN + payload.len());
    packet.extend_from_slice(&hdr.store());
    packet.extend_from_slice(payload);
    let sent = env.send(&packet)?;
    if sent && op == OP_RW {
      self.tx_cnt = self.tx_cnt.wrapping_add(len);
    }
    Ok(sent)
  }

  fn maybe_send_response<E: Env>(&mut self, env: &E) -> Result<()> {
    if self.connected && !self.response_sent && self.send(env, OP_RESPONSE, &[])? {
      self.response_sent = true;
    }
    Ok(())
  }
}

impl Vsock {
  pub(crate) fn request_sent(&self) -> bool {
    self.request_sent.load(Ordering::Acquire)
  }

  pub(crate) fn exit_received(&self) -> bool {
    self.exit_received.load(Ordering::Acquire)
  }

  pub(crate) fn exit_code(&self) -> i32 {
    self.exit_code.load(Ordering::Acquire)
  }

  pub(crate) fn protocol_error(&self) -> bool {
    self.protocol_error.load(Ordering::Acquire)
  }

  pub(crate) fn transport_error(&self) -> bool {
    self.transport_error.load(Ordering::Acquire)
  }

  pub(crate) fn fail_transport(&self) {
    self.transport_error.store(true, Ordering::Release);
  }

  pub(crate) fn begin_request(&self) {
    self.request_sent.store(false, Ordering::Release);
    self.exit_received.store(false, Ordering::Release);
    self.exit_code.store(0, Ordering::Release);
    self.protocol_error.store(false, Ordering::Release);
    self.transport_error.store(false, Ordering::Release);
  }

  pub(crate) fn reset(&self) {
    let mut inner = lock(&self.inner);
    inner.connected = false;
    inner.peer_port = 0;
    inner.fwd_cnt = 0;
    inner.rx_stream.clear();
    self.request_sent.store(false, Ordering::Release);
    self.exit_received.store(false, Ordering::Release);
    self.exit_code.store(0, Ordering::Release);
  }

  pub(crate) fn queue_frame<E: Env>(&self, env: &E, data: &[u8]) -> Result<()> {
    if data.is_empty() || data.len() > FRAME_MAX_SIZE {
      return Err(EINVAL);
    }
    let mut outgoing = lock(&self.outgoing);
    outgoing.push_back(Outgoing { data: data.to_vec(), off: 0 });
    env.set_wake(true);
    Ok(())
  }

  pub(crate) fn clear_frames<E: Env>(&self, env: &E) {
    let mut outgoing = lock(&self.outgoing);
    outgoing.clear();
    env.set_wake(false);
  }

  pub(crate) fn pending_frames(&self) -> usize {
    lock(&self.outgoing).len()
  }

  pub(crate) fn maybe_send<E: Env>(&self, env: &E) -> Result<()> {
    let mut inner = lock(&self.inner);
    inner.maybe_send_response(env)?;
    self.send_request(&mut inner, env)
  }

  pub(crate) fn maybe_send_request<E: Env>(&self, env: &E) -> Result<()> {
    self.send_request(&mut lock(&self.inner), env)
  }

  fn send_request<E: Env>(&self, inner: &mut Inner, env: &E) -> Result<()> {
    let mut outgoing = lock(&self.outgoing);
    let Some(frame) = outgoing.front_mut() else {
      env.set_wake(false);
      return Ok(());
    };
    if !inner.connected {
      return Ok(());
    }
    inner.maybe_send_response(env)?;
    if !inner.response_sent {
      return Ok(());
    }

    while let Some(rest) = frame.data.get(frame.off..).filter(|rest| !rest.is_empty()) {
      let space = inner.peer_space() as usize;
      let chunk = rest.len().min(space).min(MAX_PAYLOAD);
      let Some(data) = rest.get(..chunk).filter(|data| !data.is_empty()) else { return Ok(()) };
      if !inner.send(env, OP_RW, data)? {
        return Ok(());
      }
      frame.off += chunk;
    }

    let first = !self.request_sent.swap(true, Ordering::AcqRel);
    let sent_len = outgoing.pop_front().map_or(0, |frame| frame.data.len());
    env.set_wake(!outgoing.is_empty());
    drop(outgoing);
    env.log(&format!("{} daemon frame ({sent_len} bytes)", if first { "sent" } else { "sent queued" }));
    Ok(())
  }

  fn consume_frames<E: Env>(&self, inner: &mut Inner, env: &E, payload: &[u8]) -> Result<()> {
    if payload.is_empty() {
      return Ok(());
    }
    let result = self.parse_frames(inner, env, payload);
    if result.is_err() {
      self.protocol_error.store(true, Ordering::Release);
    }
    result
  }

  fn parse_frames<E: Env>(&self, inner: &mut Inner, env: &E, payload: &[u8]) -> Result<()> {
    if payload.len() > FRAME_MAX_SIZE || inner.rx_stream.len() > FRAME_MAX_SIZE - payload.len() {
      return Err(E2BIG);
    }
    inner.rx_stream.extend_from_slice(payload);

    let mut off = 0usize;
    while let Some((header, rest)) = inner.rx_stream.get(off..).and_then(|rest| rest.split_first_chunk::<FRAME_HEADER_LEN>()) {
      if header[..4] != *FRAME_MAGIC || header[4] != FRAME_VERSION || header[6] != 0 || header[7] != 0 {
        return Err(EINVAL);
      }
      let payload_len = u32::from_le_bytes([header[8], header[9], header[10], header[11]]) as usize;
      if payload_len > FRAME_MAX_SIZE - FRAME_HEADER_LEN {
        return Err(E2BIG);
      }
      let Some(body) = rest.get(..payload_len) else { break };
      match env.frame(header[5], body) {
        FrameOutcome::Handled => {}
        FrameOutcome::Exit(code) => {
          self.exit_code.store(code, Ordering::Release);
          self.exit_received.store(true, Ordering::Release);
        }
        FrameOutcome::Rejected => return Err(EINVAL),
      }
      off += FRAME_HEADER_LEN + payload_len;
    }
    inner.rx_stream.drain(..off);
    Ok(())
  }

  pub(crate) fn guest_packet<E: Env>(&self, env: &E, hdr: &Hdr, payload: &[u8]) -> Result<()> {
    let mut inner = lock(&self.inner);
    if hdr.src_cid == GUEST_CID && hdr.dst_cid == HOST_CID && hdr.src_port == inner.peer_port && hdr.dst_port == HOST_PORT {
      inner.update_peer_credit(hdr);
    }

    match hdr.op {
      OP_REQUEST if hdr.dst_port == HOST_PORT => {
        inner.connected = true;
        inner.peer_port = hdr.src_port;
        inner.response_sent = false;
        inner.update_peer_credit(hdr);
        env.log("daemon connected");
        inner.maybe_send_response(env)?;
        self.send_request(&mut inner, env)
      }
      OP_RW => {
        inner.fwd_cnt = inner.fwd_cnt.wrapping_add(hdr.len);
        self.consume_frames(&mut inner, env, payload)?;
        inner.send(env, OP_CREDIT_UPDATE, &[])?;
        self.send_request(&mut inner, env)
      }
      OP_CREDIT_UPDATE => self.send_request(&mut inner, env),
      OP_CREDIT_REQUEST => inner.send(env, OP_CREDIT_UPDATE, &[]).map(|_| ()),
      OP_RST => {
        inner.connected = false;
        inner.response_sent = false;
        self.request_sent.store(false, Ordering::Release);
        self.clear_frames(env);
        self.transport_error.store(!self.exit_received(), Ordering::Release);
        Ok(())
      }
      OP_SHUTDOWN => inner.send(env, OP_SHUTDOWN, &[]).map(|_| ()),
      _ => Ok(()),
    }
  }
}

#[cfg(not(test))]
pub(crate) mod ffi {
  use super::*;
  use crate::sandbox::bus::bus_of;
  use crate::sandbox::errno::to_c;
  use crate::sandbox::guest::ffi::{Vm, VmMemory};
  use crate::sandbox::host::{self, ant_hvf_vm_frame_handler, ant_hvf_vm_kick_if_running, ant_hvf_vm_set_vsock_wake, ant_sandbox_vm_display_frame};
  use crate::sandbox::virtio::ffi::{ant_hvf_virtio_interrupt, with_device, Handle};
  use crate::sandbox::virtio::Kind;
  use crate::sandbox::vring::{add_used, next_head, Queue};
  use core::ffi::{c_int, c_uint, c_void};
  use std::sync::Arc;

  const RX_QUEUE: u32 = 0;
  const TX_QUEUE: u32 = 1;
  const EVENT_QUEUE: u32 = 2;

  const FRAME_EXIT: u8 = 7;


  unsafe fn vsock_device(vm: *mut Vm) -> *mut Handle {
    bus_of(vm).map_or(core::ptr::null_mut(), |bus| bus.device(Kind::Vsock))
  }

  struct VmEnv(*mut Vm);

  impl Env for VmEnv {
    fn send(&self, packet: &[u8]) -> Result<bool> {
      unsafe {
        let dev = vsock_device(self.0);
        let mem = VmMemory(self.0);
        let sent = with_device(dev, |device| -> Result<bool> {
          let Some(queue) = device.queue_mut(RX_QUEUE).filter(|queue| queue.ready()) else { return Ok(false) };
          let Some(head) = next_head(&mem, queue)? else { return Ok(false) };
          let used = write_iov(&mem, queue.desc, head, packet)?;
          add_used(&mem, queue.used, u32::from(queue.size), head, used)?;
          queue.last_avail = queue.last_avail.wrapping_add(1);
          Ok(true)
        })
        .unwrap_or(Ok(false))?;
        if !sent {
          return Ok(false);
        }
        match ant_hvf_virtio_interrupt(self.0, dev, RX_QUEUE) {
          0 => Ok(true),
          rc => Err(Errno(-rc)),
        }
      }
    }

    fn frame(&self, kind: u8, payload: &[u8]) -> FrameOutcome {
      unsafe {
        if ant_hvf_vm_frame_handler(self.0, kind, payload.as_ptr(), payload.len() as u32) {
          return FrameOutcome::Handled;
        }
        if kind == FRAME_EXIT {
          let Some(code) = payload.first_chunk::<4>() else { return FrameOutcome::Rejected };
          let code = i32::from_le_bytes(*code);
          self.log(&format!("daemon exit code={code}"));
          return FrameOutcome::Exit(code);
        }
        let capabilities = bus_of(self.0).map_or(0, |bus| bus.capabilities());
        match ant_sandbox_vm_display_frame(capabilities, kind, payload.as_ptr(), payload.len() as u32) {
          0 => FrameOutcome::Handled,
          _ => FrameOutcome::Rejected,
        }
      }
    }

    fn set_wake(&self, pending: bool) {
      unsafe { ant_hvf_vm_set_vsock_wake(self.0, pending) }
    }

    fn log(&self, message: &str) {
      unsafe { host::log(self.0, message) }
    }
  }

  unsafe fn vsock_of(vm: *mut Vm) -> Option<Arc<Vsock>> {
    with_device(vsock_device(vm), |device| device.vsock()).flatten()
  }

  unsafe fn take_tx(vm: *mut Vm, dev: *mut Handle) -> Result<Option<(Hdr, Vec<u8>)>> {
    let mem = VmMemory(vm);
    with_device(dev, |device| -> Result<Option<(Hdr, Vec<u8>)>> {
      let Some(queue) = device.queue_mut(TX_QUEUE).filter(|queue| queue.ready()) else { return Ok(None) };
      let Some(head) = next_head(&mem, queue)? else { return Ok(None) };
      let (hdr, payload, used) = read_tx_packet(&mem, queue.desc, head)?;
      add_used(&mem, queue.used, u32::from(queue.size), head, used)?;
      queue.last_avail = queue.last_avail.wrapping_add(1);
      Ok(Some((hdr, payload)))
    })
    .unwrap_or(Ok(None))
  }

  pub(crate) unsafe fn ant_hvf_virtio_vsock_notify(vm: *mut Vm, queue: c_uint) -> c_int {
    let dev = vsock_device(vm);
    let Some(vsock) = vsock_of(vm) else { return 0 };
    let ready = with_device(dev, |device| device.queue(queue).is_some_and(Queue::ready));
    if ready != Some(true) || queue == EVENT_QUEUE {
      return 0;
    }
    let env = VmEnv(vm);
    if queue == RX_QUEUE {
      return to_c(vsock.maybe_send(&env));
    }

    loop {
      let (hdr, payload) = match take_tx(vm, dev) {
        Ok(Some(packet)) => packet,
        Ok(None) => break,
        Err(Errno(code)) => return -code,
      };
      if let Err(Errno(code)) = vsock.guest_packet(&env, &hdr, &payload) {
        return -code;
      }
    }
    ant_hvf_virtio_interrupt(vm, dev, TX_QUEUE)
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_hvf_vsock_maybe_send_request(vm: *mut Vm) -> c_int {
    match vsock_of(vm) {
      Some(vsock) => to_c(vsock.maybe_send_request(&VmEnv(vm))),
      None => 0,
    }
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_hvf_vsock_queue_frame(vm: *mut Vm, data: *const c_void, len: usize) -> c_int {
    if vm.is_null() || data.is_null() {
      return -EINVAL.0;
    }
    let Some(vsock) = vsock_of(vm) else { return -EINVAL.0 };
    let queued = vsock.queue_frame(&VmEnv(vm), core::slice::from_raw_parts(data.cast::<u8>(), len));
    if queued.is_ok() {
      ant_hvf_vm_kick_if_running(vm);
    }
    to_c(queued)
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_hvf_vsock_clear_frames(vm: *mut Vm) {
    if vm.is_null() {
      return;
    }
    if let Some(vsock) = vsock_of(vm) {
      vsock.clear_frames(&VmEnv(vm));
    }
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_hvf_vsock_pending_frames(vm: *mut Vm) -> usize {
    vsock_of(vm).map_or(0, |vsock| vsock.pending_frames())
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_hvf_vsock_begin_request(vm: *mut Vm) {
    if let Some(vsock) = vsock_of(vm) {
      vsock.begin_request();
    }
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_hvf_vsock_fail_transport(vm: *mut Vm) {
    if let Some(vsock) = vsock_of(vm) {
      vsock.fail_transport();
    }
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_hvf_vsock_request_sent(vm: *mut Vm) -> bool {
    vsock_of(vm).is_some_and(|vsock| vsock.request_sent())
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_hvf_vsock_exit_received(vm: *mut Vm) -> bool {
    vsock_of(vm).is_some_and(|vsock| vsock.exit_received())
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_hvf_vsock_exit_code(vm: *mut Vm) -> c_int {
    vsock_of(vm).map_or(0, |vsock| vsock.exit_code())
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_hvf_vsock_protocol_error(vm: *mut Vm) -> bool {
    vsock_of(vm).is_some_and(|vsock| vsock.protocol_error())
  }

  #[no_mangle]
  pub unsafe extern "C" fn ant_hvf_vsock_transport_error(vm: *mut Vm) -> bool {
    vsock_of(vm).is_some_and(|vsock| vsock.transport_error())
  }
}

#[cfg(test)]
#[path = "tests/vsock.rs"]
mod tests;
