use super::*;
use crate::sandbox::guest::testing::{TestMemory, BASE};
use crate::sandbox::vring::testing::{put_desc, DESC_BASE};
use crate::sandbox::vring::{DESC_F_NEXT, DESC_F_WRITE};
use core::cell::{Cell, RefCell};
use std::sync::Arc;

const PEER_PORT: u32 = 7000;

#[derive(Default)]
struct TestEnv {
  sent: RefCell<Vec<(Hdr, Vec<u8>)>>,
  frames: RefCell<Vec<(u8, Vec<u8>)>>,
  room: Cell<usize>,
  wake: Cell<bool>,
  logs: RefCell<Vec<String>>,
}

impl TestEnv {
  fn with_room(room: usize) -> TestEnv {
    let env = TestEnv::default();
    env.room.set(room);
    env
  }

  fn ops(&self) -> Vec<u16> {
    self.sent.borrow().iter().map(|(hdr, _)| hdr.op).collect()
  }

  fn sent_bytes(&self) -> Vec<u8> {
    self.sent.borrow().iter().filter(|(hdr, _)| hdr.op == OP_RW).flat_map(|(_, body)| body.clone()).collect()
  }
}

impl Env for TestEnv {
  fn send(&self, packet: &[u8]) -> Result<bool> {
    if self.room.get() == 0 {
      return Ok(false);
    }
    self.room.set(self.room.get() - 1);
    let (head, body) = packet.split_first_chunk::<HDR_LEN>().unwrap();
    self.sent.borrow_mut().push((Hdr::load(head), body.to_vec()));
    Ok(true)
  }

  fn frame(&self, kind: u8, payload: &[u8]) -> FrameOutcome {
    self.frames.borrow_mut().push((kind, payload.to_vec()));
    match kind {
      7 => FrameOutcome::Exit(i32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]])),
      99 => FrameOutcome::Rejected,
      _ => FrameOutcome::Handled,
    }
  }

  fn set_wake(&self, pending: bool) {
    self.wake.set(pending);
  }

  fn log(&self, message: &str) {
    self.logs.borrow_mut().push(message.to_string());
  }
}

fn guest_hdr(op: u16, len: u32, buf_alloc: u32, fwd_cnt: u32) -> Hdr {
  Hdr { src_cid: GUEST_CID, dst_cid: HOST_CID, src_port: PEER_PORT, dst_port: HOST_PORT, len, kind: TYPE_STREAM, op, flags: 0, buf_alloc, fwd_cnt }
}

fn frame(kind: u8, payload: &[u8]) -> Vec<u8> {
  let mut out = FRAME_MAGIC.to_vec();
  out.extend_from_slice(&[FRAME_VERSION, kind, 0, 0]);
  out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
  out.extend_from_slice(payload);
  out
}

fn connect(vsock: &Vsock, env: &TestEnv, buf_alloc: u32) {
  vsock.guest_packet(env, &guest_hdr(OP_REQUEST, 0, buf_alloc, 0), &[]).unwrap();
}

fn rw(vsock: &Vsock, env: &TestEnv, payload: &[u8]) -> Result<()> {
  vsock.guest_packet(env, &guest_hdr(OP_RW, payload.len() as u32, 65536, 0), payload)
}

#[test]
fn header_round_trips() {
  let hdr = Hdr { src_cid: 0x1_0000_0003, dst_cid: 2, src_port: 7, dst_port: 1024, len: 9, kind: 1, op: 5, flags: 3, buf_alloc: 65536, fwd_cnt: 11 };
  let raw = hdr.store();
  assert_eq!(raw[0..8], 0x1_0000_0003u64.to_le_bytes());
  assert_eq!(raw[30..32], 5u16.to_le_bytes());
  assert_eq!(Hdr::load(&raw), hdr);
}

#[test]
fn tx_packet_bounds() {
  let mem = TestMemory::new(0x8000);
  let hdr = Hdr { len: 4, op: OP_RW, ..Hdr::default() };
  mem.put(BASE + 0x2000, &hdr.store());
  mem.put(BASE + 0x3000, b"ping");
  put_desc(&mem, 0, BASE + 0x2000, HDR_LEN as u32, DESC_F_NEXT, 1);
  put_desc(&mem, 1, BASE + 0x3000, 4, 0, 0);
  let (out, payload, used) = read_tx_packet(&mem, DESC_BASE, 0).unwrap();
  assert_eq!((out.len, payload.as_slice(), used), (4, &b"ping"[..], HDR_LEN as u32 + 4));

  put_desc(&mem, 1, BASE + 0x3000, u32::MAX, 0, 0);
  assert_eq!(read_tx_packet(&mem, DESC_BASE, 0).err(), Some(E2BIG));
  put_desc(&mem, 1, BASE + 0x3000, BUF_ALLOC + 1, 0, 0);
  assert_eq!(read_tx_packet(&mem, DESC_BASE, 0).err(), Some(E2BIG));

  put_desc(&mem, 1, BASE + 0x3000, 3, 0, 0);
  assert_eq!(read_tx_packet(&mem, DESC_BASE, 0).err(), Some(EINVAL));
  put_desc(&mem, 0, BASE + 0x2000, HDR_LEN as u32 - 1, 0, 0);
  assert_eq!(read_tx_packet(&mem, DESC_BASE, 0).err(), Some(EINVAL));
}

#[test]
fn write_iov_uses_only_writable_descriptors() {
  let mem = TestMemory::new(0x8000);
  put_desc(&mem, 0, BASE + 0x2000, 8, DESC_F_NEXT, 1);
  put_desc(&mem, 1, BASE + 0x3000, 4, DESC_F_WRITE | DESC_F_NEXT, 2);
  put_desc(&mem, 2, BASE + 0x4000, 8, DESC_F_WRITE, 0);
  assert_eq!(write_iov(&mem, DESC_BASE, 0, b"abcdefgh"), Ok(8));
  assert_eq!(mem.get::<4>(BASE + 0x3000), *b"abcd");
  assert_eq!(mem.get::<4>(BASE + 0x4000), *b"efgh");
  assert_eq!(mem.get::<8>(BASE + 0x2000), [0u8; 8]);
  assert_eq!(write_iov(&mem, DESC_BASE, 0, &[0u8; 13]), Err(ENOSPC));
}

#[test]
fn request_is_held_until_connected_then_chunked() {
  let vsock = Vsock::default();
  let env = TestEnv::with_room(100);
  let request: Vec<u8> = (0..5000u32).map(|i| i as u8).collect();
  vsock.queue_frame(&env, &request).unwrap();
  assert!(env.wake.get());
  vsock.maybe_send_request(&env).unwrap();
  assert!(env.sent.borrow().is_empty());
  assert!(!vsock.request_sent());

  connect(&vsock, &env, 65536);
  assert_eq!(env.ops(), [OP_RESPONSE, OP_RW, OP_RW, OP_RW]);
  assert_eq!(env.sent_bytes(), request);
  assert_eq!(env.sent.borrow()[1].0.dst_port, PEER_PORT);
  assert!(vsock.request_sent());
  assert!(!env.wake.get());
  assert_eq!(vsock.pending_frames(), 0);
  assert!(env.logs.borrow().iter().any(|line| line == "sent daemon frame (5000 bytes)"));
}

#[test]
fn peer_credit_limits_what_is_sent() {
  let vsock = Vsock::default();
  let env = TestEnv::with_room(100);
  vsock.queue_frame(&env, &[7u8; 3000]).unwrap();
  connect(&vsock, &env, 2500);
  assert_eq!(env.sent_bytes().len(), 2500);
  assert!(!vsock.request_sent());
  assert_eq!(vsock.pending_frames(), 1);

  vsock.guest_packet(&env, &guest_hdr(OP_CREDIT_UPDATE, 0, 2500, 2500), &[]).unwrap();
  assert_eq!(env.sent_bytes().len(), 3000);
  assert!(vsock.request_sent());
}

#[test]
fn no_guest_buffers_means_retry_later() {
  let vsock = Vsock::default();
  let env = TestEnv::with_room(0);
  vsock.queue_frame(&env, b"hello").unwrap();
  connect(&vsock, &env, 65536);
  assert!(env.sent.borrow().is_empty());
  env.room.set(1);
  vsock.maybe_send(&env).unwrap();
  assert_eq!(env.ops(), [OP_RESPONSE]);
  env.room.set(10);
  vsock.maybe_send(&env).unwrap();
  assert_eq!(env.ops(), [OP_RESPONSE, OP_RW]);
  assert!(vsock.request_sent());
}

#[test]
fn frames_are_reassembled_across_packets() {
  let vsock = Vsock::default();
  let env = TestEnv::with_room(100);
  connect(&vsock, &env, 65536);

  let mut stream = frame(3, b"hello");
  stream.extend_from_slice(&frame(9, b"{\"a\":1}"));
  stream.extend_from_slice(&frame(7, &42i32.to_le_bytes()));
  for chunk in stream.chunks(5) {
    rw(&vsock, &env, chunk).unwrap();
  }
  let frames = env.frames.borrow();
  assert_eq!(frames.len(), 3);
  assert_eq!(frames[0], (3, b"hello".to_vec()));
  assert_eq!(frames[1], (9, b"{\"a\":1}".to_vec()));
  assert!(vsock.exit_received());
  assert_eq!(vsock.exit_code(), 42);
  assert!(!vsock.protocol_error());
  assert_eq!(env.ops().iter().filter(|&&op| op == OP_CREDIT_UPDATE).count(), stream.chunks(5).count());
  assert_eq!(env.sent.borrow().last().unwrap().0.fwd_cnt, stream.len() as u32);
}

#[test]
fn malformed_frames_are_protocol_errors() {
  let mut bad_magic = frame(3, b"x");
  bad_magic[0] = b'X';
  let mut bad_version = frame(3, b"x");
  bad_version[4] = 2;
  let mut bad_reserved = frame(3, b"x");
  bad_reserved[6] = 1;
  let mut too_big = frame(3, b"");
  too_big[8..12].copy_from_slice(&(FRAME_MAX_SIZE as u32).to_le_bytes());
  for (stream, expected) in [(bad_magic, EINVAL), (bad_version, EINVAL), (bad_reserved, EINVAL), (too_big, E2BIG), (frame(99, b"x"), EINVAL)] {
    let vsock = Vsock::default();
    let env = TestEnv::with_room(100);
    connect(&vsock, &env, 65536);
    assert_eq!(rw(&vsock, &env, &stream), Err(expected));
    assert!(vsock.protocol_error());
  }
}

#[test]
fn reset_and_shutdown() {
  let vsock = Vsock::default();
  let env = TestEnv::with_room(100);
  connect(&vsock, &env, 0);
  vsock.queue_frame(&env, b"pending").unwrap();
  vsock.guest_packet(&env, &guest_hdr(OP_SHUTDOWN, 0, 0, 0), &[]).unwrap();
  assert_eq!(env.ops().last(), Some(&OP_SHUTDOWN));

  vsock.guest_packet(&env, &guest_hdr(OP_RST, 0, 0, 0), &[]).unwrap();
  assert!(vsock.transport_error());
  assert_eq!(vsock.pending_frames(), 0);
  assert!(!env.wake.get());

  vsock.begin_request();
  assert!(!vsock.transport_error());
  vsock.fail_transport();
  assert!(vsock.transport_error());
  vsock.begin_request();
  connect(&vsock, &env, 65536);
  rw(&vsock, &env, &frame(7, &3i32.to_le_bytes())).unwrap();
  vsock.guest_packet(&env, &guest_hdr(OP_RST, 0, 0, 0), &[]).unwrap();
  assert!(!vsock.transport_error());
  assert_eq!(vsock.exit_code(), 3);

  vsock.reset();
  assert!(!vsock.exit_received());
  assert!(vsock.queue_frame(&env, &[]).is_err());
}

#[test]
fn concurrent_producers_queue_every_frame() {
  struct QuietEnv;
  impl Env for QuietEnv {
    fn send(&self, _: &[u8]) -> Result<bool> {
      Ok(false)
    }
    fn frame(&self, _: u8, _: &[u8]) -> FrameOutcome {
      FrameOutcome::Handled
    }
    fn set_wake(&self, _: bool) {}
    fn log(&self, _: &str) {}
  }

  let vsock = Arc::new(Vsock::default());
  let producers: Vec<_> = (0..4u32)
    .map(|producer| {
      let vsock = Arc::clone(&vsock);
      std::thread::spawn(move || {
        for index in 0..1000u32 {
          vsock.queue_frame(&QuietEnv, &[producer.to_le_bytes(), index.to_le_bytes()].concat()).unwrap();
        }
      })
    })
    .collect();
  for producer in producers {
    producer.join().unwrap();
  }
  assert_eq!(vsock.pending_frames(), 4000);
  vsock.clear_frames(&QuietEnv);
  assert_eq!(vsock.pending_frames(), 0);
}
