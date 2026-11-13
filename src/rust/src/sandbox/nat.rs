use super::net::{
  self, Flow, FrameSink, Stack, TcpInfo, BROADCAST_IP, DNS_IP, DNS_PORT, GUEST_IP, HOST_IP, MAC_LEN, MAX_PACKET,
  TCP_ACK, TCP_FIN, TCP_MSS, TCP_PSH, TCP_RST, TCP_SYN,
};
use std::collections::VecDeque;
use std::io::{self, ErrorKind, Read, Write};
use std::net::{Ipv4Addr, Shutdown, TcpListener, TcpStream, UdpSocket};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

const RX_BACKLOG: usize = 64;
const STREAM_BUFFER_LIMIT: usize = 1024 * 1024;
const READ_CHUNK: usize = 16384;
const INBOUND_PORT_FIRST: u16 = 49152;
const INBOUND_WINDOW: u32 = 65535;
const FALLBACK_DNS_IP: u32 = net::ipv4(8, 8, 8, 8);
const LOOPBACK_IP: u32 = net::ipv4(127, 0, 0, 1);
const DUP_ACK_THRESHOLD: u8 = 3;
const MAX_BACKOFF_SHIFT: u32 = 4;
const RETRANSMIT_AFTER: Duration = Duration::from_millis(if cfg!(test) { 150 } else { 1000 });
const TICK_MS: libc::c_int = if cfg!(test) { 50 } else { 250 };

pub(crate) trait Host: Send + Sync + Sized + 'static {
  fn rx_ready(&self, net: &Net<Self>);
  fn wake(&self);
  fn verbose(&self) -> bool;
  fn log(&self, message: &str);
}

#[derive(Clone, Default)]
pub(crate) struct Policy {
  pub allow_host_all: bool,
  pub allow_lan: bool,
  pub allow_host_ports: Vec<u16>,
}

fn ip_in(ip: u32, net: u32, bits: u32) -> bool {
  let mask = u32::MAX << (32 - bits);
  ip & mask == net & mask
}

fn is_host_ip(ip: u32) -> bool {
  ip == HOST_IP || ip_in(ip, net::ipv4(127, 0, 0, 0), 8) || ip_in(ip, 0, 8)
}

impl Policy {
  pub(crate) fn egress_allowed(&self, dst_ip: u32, dst_port: u16) -> bool {
    if dst_ip == DNS_IP {
      return dst_port == DNS_PORT;
    }
    if is_host_ip(dst_ip) {
      return self.allow_host_all || self.allow_host_ports.contains(&dst_port);
    }
    let private = ip_in(dst_ip, net::ipv4(10, 0, 0, 0), 8)
      || ip_in(dst_ip, net::ipv4(172, 16, 0, 0), 12)
      || ip_in(dst_ip, net::ipv4(192, 168, 0, 0), 16)
      || ip_in(dst_ip, net::ipv4(100, 64, 0, 0), 10)
      || ip_in(dst_ip, net::ipv4(169, 254, 0, 0), 16)
      || ip_in(dst_ip, net::ipv4(224, 0, 0, 0), 4)
      || ip_in(dst_ip, net::ipv4(240, 0, 0, 0), 4);
    !private || self.allow_lan
  }
}

#[derive(Clone, Default)]
pub(crate) struct Config {
  pub mac: [u8; MAC_LEN],
  pub policy: Policy,
  pub forwards: Vec<(u16, u16)>,
}

fn parse_dns_ip(text: &str) -> Option<u32> {
  let ip = u32::from(text.parse::<Ipv4Addr>().ok()?);
  (ip != 0 && ip != BROADCAST_IP).then_some(ip)
}

fn env_dns_ip() -> Option<u32> {
  let value = std::env::var("ANT_SANDBOX_DNS_IP")
    .ok()
    .filter(|value| !value.is_empty())
    .or_else(|| std::env::var("ANT_DNS_IP").ok().filter(|value| !value.is_empty()))?;
  parse_dns_ip(&value)
}

fn env_drop_interval() -> u64 {
  std::env::var("ANT_SANDBOX_NET_DROP").ok().and_then(|value| value.parse().ok()).unwrap_or(0)
}

fn resolv_conf_dns_ip(contents: &str) -> Option<u32> {
  contents.lines().find_map(|line| {
    let rest = line.trim_start().strip_prefix("nameserver")?;
    if !rest.starts_with(char::is_whitespace) {
      return None;
    }
    let value = rest.trim_start().split(|c: char| c.is_whitespace() || c == '#').next()?;
    parse_dns_ip(value)
  })
}

fn connect_ip(dst_ip: u32) -> u32 {
  if dst_ip == HOST_IP {
    return LOOPBACK_IP;
  }
  if dst_ip != DNS_IP {
    return dst_ip;
  }
  env_dns_ip()
    .or_else(|| resolv_conf_dns_ip(&std::fs::read_to_string("/etc/resolv.conf").ok()?))
    .unwrap_or(FALLBACK_DNS_IP)
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
  mutex.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn set_nonblocking(fd: RawFd) -> io::Result<()> {
  unsafe {
    let flags = libc::fcntl(fd, libc::F_GETFL, 0);
    if flags < 0 || libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK) != 0 {
      return Err(io::Error::last_os_error());
    }
  }
  Ok(())
}

fn wake_pipe() -> io::Result<(OwnedFd, OwnedFd)> {
  let mut fds = [0 as libc::c_int; 2];
  if unsafe { libc::pipe(fds.as_mut_ptr()) } != 0 {
    return Err(io::Error::last_os_error());
  }
  let pair = unsafe { (OwnedFd::from_raw_fd(fds[0]), OwnedFd::from_raw_fd(fds[1])) };
  set_nonblocking(pair.0.as_raw_fd())?;
  set_nonblocking(pair.1.as_raw_fd())?;
  Ok(pair)
}

enum ConnectError {
  Socket,
  Refused,
}

fn connect_tcp(ip: u32, port: u16) -> Result<(TcpStream, bool), ConnectError> {
  let fd = unsafe { libc::socket(libc::AF_INET, libc::SOCK_STREAM, 0) };
  if fd < 0 {
    return Err(ConnectError::Socket);
  }
  let stream = unsafe { TcpStream::from_raw_fd(fd) };
  let _ = set_nonblocking(fd);

  let mut addr: libc::sockaddr_in = unsafe { core::mem::zeroed() };
  addr.sin_family = libc::AF_INET as libc::sa_family_t;
  addr.sin_port = port.to_be();
  addr.sin_addr.s_addr = ip.to_be();
  let len = core::mem::size_of::<libc::sockaddr_in>() as libc::socklen_t;
  let rc = unsafe { libc::connect(fd, (&raw const addr).cast(), len) };
  if rc == 0 {
    return Ok((stream, false));
  }
  if io::Error::last_os_error().raw_os_error() == Some(libc::EINPROGRESS) {
    return Ok((stream, true));
  }
  Err(ConnectError::Refused)
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum TcpState {
  ConnectingHost,
  ConnectingGuest,
  Established,
  HalfClosed,
}

struct Tcp {
  id: u64,
  state: TcpState,
  inbound: bool,
  sock: Option<TcpStream>,
  guest_ip: u32,
  guest_port: u16,
  peer_ip: u32,
  peer_port: u16,
  guest_next: u32,
  nat_next: u32,
  nat_iss: u32,
  guest_ack: u32,
  guest_window: u32,
  guest_fin_seen: bool,
  host_eof: bool,
  host_fin_sent: bool,
  guest_mac: [u8; MAC_LEN],
  pending: Vec<u8>,
  pending_off: usize,
  host_buf: Vec<u8>,
  host_base: u32,
  host_sent: usize,
  dup_acks: u8,
  timeouts: u32,
  progress_at: Instant,
}

struct Udp {
  id: u64,
  sock: UdpSocket,
  guest_ip: u32,
  guest_port: u16,
  peer_ip: u32,
  peer_port: u16,
  guest_mac: [u8; MAC_LEN],
}

struct Forward {
  listener: TcpListener,
  guest_port: u16,
}

#[derive(Default)]
struct Nat {
  tcp: Vec<Option<Tcp>>,
  udp: Vec<Option<Udp>>,
  forwards: Vec<Forward>,
  next_inbound_port: u16,
  next_id: u64,
}

#[derive(Default)]
struct Rx {
  packets: VecDeque<Vec<u8>>,
  wake: bool,
}

#[derive(Default)]
struct Stats {
  guest_packets: AtomicU64,
  guest_drops: AtomicU64,
  host_packets: AtomicU64,
  tcp_opened: AtomicU64,
  udp_opened: AtomicU64,
}

pub(crate) struct Net<H: Host> {
  host: H,
  mac: [u8; MAC_LEN],
  policy: Policy,
  guest_mac: Mutex<Option<[u8; MAC_LEN]>>,
  rx: Mutex<Rx>,
  nat: Mutex<Nat>,
  wake_read: OwnedFd,
  wake_write: OwnedFd,
  stop: AtomicBool,
  rx_blocked: AtomicBool,
  drop_interval: u64,
  offered: AtomicU64,
  thread: Mutex<Option<JoinHandle<()>>>,
  stats: Stats,
}

struct Port<'a, H: Host>(&'a Net<H>);

impl<H: Host> FrameSink for Port<'_, H> {
  fn host_mac(&self) -> [u8; MAC_LEN] {
    self.0.mac
  }

  fn enqueue(&mut self, frame: &[u8]) -> bool {
    self.0.enqueue(frame)
  }
}

impl<H: Host> Stack for Port<'_, H> {
  fn set_guest_mac(&mut self, mac: &[u8; MAC_LEN]) {
    *lock(&self.0.guest_mac) = Some(*mac);
  }

  fn note_guest_packet(&mut self) {
    self.0.stats.guest_packets.fetch_add(1, Ordering::Relaxed);
  }

  fn nat_udp(&mut self, flow: &Flow, payload: &[u8]) {
    self.0.guest_udp(flow, payload);
  }

  fn nat_tcp(&mut self, flow: &Flow, tcp: &TcpInfo, payload: &[u8]) {
    self.0.guest_tcp(flow, tcp, payload);
  }
}

impl Tcp {
  fn new(id: u64, sock: TcpStream, state: TcpState) -> Tcp {
    Tcp {
      id,
      state,
      inbound: false,
      sock: Some(sock),
      guest_ip: 0,
      guest_port: 0,
      peer_ip: 0,
      peer_port: 0,
      guest_next: 0,
      nat_next: 0,
      nat_iss: 0,
      guest_ack: 0,
      guest_window: 0,
      guest_fin_seen: false,
      host_eof: false,
      host_fin_sent: false,
      guest_mac: [0xff; MAC_LEN],
      pending: Vec::new(),
      pending_off: 0,
      host_buf: Vec::new(),
      host_base: 0,
      host_sent: 0,
      dup_acks: 0,
      timeouts: 0,
      progress_at: Instant::now(),
    }
  }

  fn matches(&self, guest_ip: u32, guest_port: u16, peer_ip: u32, peer_port: u16) -> bool {
    self.guest_ip == guest_ip && self.guest_port == guest_port && self.peer_ip == peer_ip && self.peer_port == peer_port
  }

  fn host_sent_seq(&self) -> u32 {
    self.host_base.wrapping_add(self.host_sent as u32)
  }

  fn in_flight(&self) -> u32 {
    self.host_sent_seq().wrapping_sub(self.guest_ack)
  }

  fn pending_len(&self) -> usize {
    self.pending.len() - self.pending_off
  }

  fn wants_host_data(&self) -> bool {
    self.state != TcpState::ConnectingGuest
      && self.in_flight() < self.guest_window
      && self.host_buf.len() < STREAM_BUFFER_LIMIT
      && !self.host_eof
  }

  fn emit<H: Host>(&self, net: &Net<H>, seq: u32, flags: u8, payload: &[u8]) -> bool {
    net::send_tcp(
      &mut Port(net),
      &self.guest_mac,
      self.peer_ip,
      self.guest_ip,
      self.peer_port,
      self.guest_port,
      seq,
      self.guest_next,
      flags,
      payload,
    )
  }

  fn awaiting_guest(&self) -> bool {
    self.state == TcpState::ConnectingGuest || self.host_sent > 0 || (self.host_fin_sent && self.guest_ack != self.nat_next)
  }

  fn has_unsent(&self) -> bool {
    self.host_sent < self.host_buf.len() || (self.host_eof && !self.host_fin_sent)
  }

  fn retransmit<H: Host>(&mut self, net: &Net<H>) {
    self.dup_acks = 0;
    self.progress_at = Instant::now();
    if self.state == TcpState::ConnectingGuest {
      self.emit(net, self.nat_iss, TCP_SYN, &[]);
      return;
    }
    self.drop_guest_acked();
    self.host_sent = 0;
    if self.host_fin_sent && self.guest_ack != self.nat_next {
      self.host_fin_sent = false;
    }
    self.send_host_buffer(net);
  }

  fn tick<H: Host>(&mut self, net: &Net<H>, now: Instant) {
    let patience = RETRANSMIT_AFTER * (1 << self.timeouts.min(MAX_BACKOFF_SHIFT));
    if self.awaiting_guest() && now.duration_since(self.progress_at) >= patience {
      self.timeouts += 1;
      self.retransmit(net);
    } else if self.has_unsent() {
      self.send_host_buffer(net);
    }
  }

  fn append_pending(&mut self, data: &[u8]) -> bool {
    if data.len() > STREAM_BUFFER_LIMIT - self.pending_len().min(STREAM_BUFFER_LIMIT) {
      return false;
    }
    if self.pending_off > 0 {
      self.pending.drain(..self.pending_off);
      self.pending_off = 0;
    }
    self.pending.extend_from_slice(data);
    true
  }

  fn drop_guest_acked(&mut self) {
    let acked = self.guest_ack.wrapping_sub(self.host_base);
    let advanced = (acked as i32) > 0;
    if self.host_buf.is_empty() {
      if advanced {
        self.host_base = self.guest_ack;
      }
      self.host_sent = 0;
      return;
    }
    if !advanced {
      return;
    }
    let acked = (acked as usize).min(self.host_buf.len());
    self.host_buf.drain(..acked);
    self.host_base = self.host_base.wrapping_add(acked as u32);
    self.host_sent = self.host_sent.saturating_sub(acked);
    if self.host_buf.is_empty() {
      self.host_base = self.guest_ack;
      self.host_sent = 0;
    }
  }

  fn send_host_buffer<H: Host>(&mut self, net: &Net<H>) {
    self.drop_guest_acked();

    while self.host_sent < self.host_buf.len() {
      let in_flight = self.in_flight();
      if in_flight >= self.guest_window {
        break;
      }
      let allowed = (self.guest_window - in_flight) as usize;
      let chunk = (self.host_buf.len() - self.host_sent).min(TCP_MSS).min(allowed);
      let Some(data) = self.host_buf.get(self.host_sent..self.host_sent + chunk) else { break };
      if data.is_empty() {
        break;
      }
      let seq = self.host_sent_seq();
      if !self.emit(net, seq, TCP_ACK | TCP_PSH, data) {
        return;
      }
      if self.host_sent == 0 {
        self.progress_at = Instant::now();
      }
      self.host_sent += chunk;
      let next = seq.wrapping_add(chunk as u32);
      if (next.wrapping_sub(self.nat_next) as i32) > 0 {
        self.nat_next = next;
      }
    }

    if self.host_eof && !self.host_fin_sent && self.host_sent == self.host_buf.len() {
      let fin_seq = self.host_base.wrapping_add(self.host_buf.len() as u32);
      if !self.emit(net, fin_seq, TCP_ACK | TCP_FIN, &[]) {
        return;
      }
      self.host_fin_sent = true;
      self.nat_next = fin_seq.wrapping_add(1);
      self.progress_at = Instant::now();
    }
  }

  fn flush_pending(&mut self) -> bool {
    let Some(sock) = self.sock.as_ref() else { return true };
    while self.pending_len() > 0 && self.state != TcpState::ConnectingHost {
      let Some(data) = self.pending.get(self.pending_off..) else { break };
      match (&*sock).write(data) {
        Ok(0) => return false,
        Ok(written) => {
          self.pending_off += written;
          if self.pending_len() == 0 {
            self.pending.clear();
            self.pending_off = 0;
          }
        }
        Err(err) if err.kind() == ErrorKind::Interrupted => {}
        Err(err) if err.kind() == ErrorKind::WouldBlock => break,
        Err(_) => return false,
      }
    }
    true
  }

  fn close_sock(&mut self) {
    self.sock = None;
    self.state = TcpState::HalfClosed;
    self.pending.clear();
    self.pending_off = 0;
  }

  fn read_from_host<H: Host>(&mut self, net: &Net<H>) -> bool {
    let mut chunk = [0u8; READ_CHUNK];
    loop {
      let room = READ_CHUNK.min(STREAM_BUFFER_LIMIT.saturating_sub(self.host_buf.len()));
      let Some(buf) = chunk.get_mut(..room).filter(|buf| !buf.is_empty()) else { break };
      let Some(sock) = self.sock.as_ref() else { break };
      match (&*sock).read(buf) {
        Ok(0) => {
          self.host_eof = true;
          self.close_sock();
          self.send_host_buffer(net);
          break;
        }
        Ok(read) => {
          if self.host_buf.is_empty() && self.host_sent == 0 {
            self.host_base = self.nat_next;
          }
          self.host_buf.extend_from_slice(&buf[..read.min(room)]);
          self.send_host_buffer(net);
        }
        Err(err) if err.kind() == ErrorKind::Interrupted => {}
        Err(err) if err.kind() == ErrorKind::WouldBlock => break,
        Err(_) => return false,
      }
    }
    true
  }

  fn check_connect(&mut self) -> bool {
    if self.state != TcpState::ConnectingHost {
      return true;
    }
    let connected = matches!(self.sock.as_ref().map(TcpStream::take_error), Some(Ok(None)));
    if !connected {
      return false;
    }
    self.state = TcpState::Established;
    self.flush_pending()
  }

  fn guest_segment<H: Host>(&mut self, net: &Net<H>, tcp: &TcpInfo, payload: &[u8]) -> bool {
    let syn = tcp.flags & TCP_SYN != 0;
    let ack = tcp.flags & TCP_ACK != 0;
    let fin = tcp.flags & TCP_FIN != 0;
    let payload_len = payload.len() as u32;

    let prev_window = self.guest_window;
    self.guest_window = u32::from(tcp.window);

    if syn && !ack {
      if self.state != TcpState::ConnectingGuest && tcp.seq.wrapping_add(1) == self.guest_next && self.host_base == self.nat_iss.wrapping_add(1) {
        self.emit(net, self.nat_iss, TCP_SYN | TCP_ACK, &[]);
      }
      return true;
    }

    if ack {
      let prev_ack = self.guest_ack;
      if (tcp.ack.wrapping_sub(self.guest_ack) as i32) > 0 && (self.nat_next.wrapping_sub(tcp.ack) as i32) >= 0 {
        self.guest_ack = tcp.ack;
      }
      if self.guest_ack != prev_ack {
        self.dup_acks = 0;
        self.timeouts = 0;
        self.progress_at = Instant::now();
        self.send_host_buffer(net);
      } else if self.guest_window != prev_window {
        self.send_host_buffer(net);
      } else if payload.is_empty() && !fin && self.host_sent > 0 && tcp.ack == self.guest_ack {
        self.dup_acks = self.dup_acks.saturating_add(1);
        if self.dup_acks == DUP_ACK_THRESHOLD {
          self.retransmit(net);
        }
      }
    }

    if self.state == TcpState::ConnectingGuest && syn && ack {
      self.guest_next = tcp.seq.wrapping_add(1);
      self.state = TcpState::Established;
      self.guest_ack = tcp.ack;
      self.progress_at = Instant::now();
      self.drop_guest_acked();
      self.emit(net, self.nat_next, TCP_ACK, &[]);
      return true;
    }

    let behind = self.guest_next.wrapping_sub(tcp.seq);
    if !payload.is_empty() || fin {
      if (behind as i32) < 0 || behind > payload_len {
        self.emit(net, self.nat_next, TCP_ACK, &[]);
        return true;
      }
    }
    let fresh = payload.get(behind as usize..).unwrap_or(&[]);

    if !payload.is_empty() {
      if !fresh.is_empty() && self.sock.is_some() && !self.guest_fin_seen && !self.append_pending(fresh) {
        return true;
      }
      self.guest_next = self.guest_next.wrapping_add(fresh.len() as u32);
      self.emit(net, self.nat_next, TCP_ACK, &[]);
      if !self.flush_pending() {
        return false;
      }
    }

    if fin {
      if !self.guest_fin_seen {
        self.guest_next = self.guest_next.wrapping_add(1);
      }
      self.guest_fin_seen = true;
      self.state = TcpState::HalfClosed;
      if let Some(sock) = self.sock.as_ref() {
        let _ = sock.shutdown(Shutdown::Write);
      }
      self.emit(net, self.nat_next, TCP_ACK, &[]);
    }
    !(self.host_fin_sent && self.guest_fin_seen && self.guest_ack >= self.nat_next)
  }
}

impl Nat {
  fn next_id(&mut self) -> u64 {
    self.next_id += 1;
    self.next_id
  }

  fn find_tcp(&self, guest_ip: u32, guest_port: u16, peer_ip: u32, peer_port: u16) -> Option<usize> {
    self.tcp.iter().position(|slot| slot.as_ref().is_some_and(|c| c.matches(guest_ip, guest_port, peer_ip, peer_port)))
  }

  fn insert_tcp(&mut self, tcp: Tcp) {
    match self.tcp.iter_mut().find(|slot| slot.is_none()) {
      Some(slot) => *slot = Some(tcp),
      None => self.tcp.push(Some(tcp)),
    }
  }

  fn find_udp(&self, flow: &Flow) -> Option<usize> {
    self.udp.iter().position(|slot| {
      slot.as_ref().is_some_and(|u| {
        u.guest_ip == flow.src_ip && u.guest_port == flow.src_port && u.peer_ip == flow.dst_ip && u.peer_port == flow.dst_port
      })
    })
  }

  fn insert_udp(&mut self, udp: Udp) -> usize {
    match self.udp.iter().position(Option::is_none) {
      Some(index) => {
        self.udp[index] = Some(udp);
        index
      }
      None => {
        self.udp.push(Some(udp));
        self.udp.len() - 1
      }
    }
  }

  fn alloc_inbound_port(&mut self) -> Option<u16> {
    for _ in INBOUND_PORT_FIRST..=u16::MAX {
      let port = self.next_inbound_port.max(INBOUND_PORT_FIRST);
      self.next_inbound_port = port.checked_add(1).unwrap_or(INBOUND_PORT_FIRST);
      let used = self.tcp.iter().flatten().any(|c| c.inbound && c.peer_port == port);
      if !used {
        return Some(port);
      }
    }
    None
  }
}

#[derive(Clone, Copy)]
enum Watch {
  Wake,
  Forward(usize),
  Tcp(usize, u64),
  Udp(usize, u64),
}

impl<H: Host> Net<H> {
  pub(crate) fn start(host: H, config: Config) -> io::Result<Arc<Net<H>>> {
    let (wake_read, wake_write) = wake_pipe()?;
    let mut nat = Nat::default();
    for &(host_port, guest_port) in &config.forwards {
      let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, host_port))?;
      listener.set_nonblocking(true)?;
      nat.forwards.push(Forward { listener, guest_port });
    }

    let net = Arc::new(Net {
      host,
      mac: config.mac,
      policy: config.policy,
      guest_mac: Mutex::new(None),
      rx: Mutex::new(Rx::default()),
      nat: Mutex::new(nat),
      wake_read,
      wake_write,
      stop: AtomicBool::new(false),
      rx_blocked: AtomicBool::new(false),
      drop_interval: env_drop_interval(),
      offered: AtomicU64::new(0),
      thread: Mutex::new(None),
      stats: Stats::default(),
    });

    let worker = Arc::clone(&net);
    let thread = std::thread::Builder::new().name("ant-sandbox-nat".into()).spawn(move || worker.run())?;
    *lock(&net.thread) = Some(thread);
    Ok(net)
  }

  pub(crate) fn stop(&self) {
    self.stop.store(true, Ordering::Release);
    self.wake();
    if let Some(thread) = lock(&self.thread).take() {
      let _ = thread.join();
    }

    let (tcp_slots, udp_slots) = {
      let mut nat = lock(&self.nat);
      let slots = (nat.tcp.len(), nat.udp.len());
      *nat = Nat::default();
      slots
    };
    if self.host.verbose() {
      self.host.log(&format!(
        "network backend stopped (guest_packets={} host_packets={} drops={} tcp_opened={} udp_opened={} tcp_slots={} udp_slots={})",
        self.stats.guest_packets.load(Ordering::Relaxed),
        self.stats.host_packets.load(Ordering::Relaxed),
        self.stats.guest_drops.load(Ordering::Relaxed),
        self.stats.tcp_opened.load(Ordering::Relaxed),
        self.stats.udp_opened.load(Ordering::Relaxed),
        tcp_slots,
        udp_slots,
      ));
    }
  }

  pub(crate) fn forward_count(&self) -> usize {
    lock(&self.nat).forwards.len()
  }

  pub(crate) fn handle_frame(&self, frame: &[u8]) {
    net::handle_frame(&mut Port(self), frame);
  }

  pub(crate) fn enqueue(&self, frame: &[u8]) -> bool {
    if frame.is_empty() || frame.len() > MAX_PACKET {
      return false;
    }
    if self.drop_interval != 0 && (self.offered.fetch_add(1, Ordering::Relaxed) + 1) % self.drop_interval == 0 {
      self.stats.guest_drops.fetch_add(1, Ordering::Relaxed);
      return true;
    }
    let queued = {
      let mut rx = lock(&self.rx);
      let room = rx.packets.len() < RX_BACKLOG;
      if room {
        rx.packets.push_back(frame.to_vec());
      }
      room
    };
    if !queued {
      self.stats.guest_drops.fetch_add(1, Ordering::Relaxed);
      self.rx_blocked.store(true, Ordering::Release);
      return false;
    }
    self.stats.host_packets.fetch_add(1, Ordering::Relaxed);
    self.host.rx_ready(self);
    lock(&self.rx).wake = true;
    self.host.wake();
    true
  }

  pub(crate) fn rx_peek(&self, out: &mut [u8]) -> Option<usize> {
    let mut rx = lock(&self.rx);
    rx.wake = false;
    let packet = rx.packets.front()?;
    out.get_mut(..packet.len())?.copy_from_slice(packet);
    Some(packet.len())
  }

  pub(crate) fn rx_pop(&self) -> Option<Vec<u8>> {
    let packet = lock(&self.rx).packets.pop_front();
    if packet.is_some() && self.rx_blocked.swap(false, Ordering::AcqRel) {
      self.wake();
    }
    packet
  }

  pub(crate) fn take_rx_wake(&self) -> bool {
    core::mem::take(&mut lock(&self.rx).wake)
  }

  fn wake(&self) {
    let byte = 0u8;
    unsafe { libc::write(self.wake_write.as_raw_fd(), (&raw const byte).cast(), 1) };
  }

  fn drain_wake(&self) {
    let mut buf = [0u8; 64];
    loop {
      let read = unsafe { libc::read(self.wake_read.as_raw_fd(), buf.as_mut_ptr().cast(), buf.len()) };
      if read > 0 {
        continue;
      }
      if read < 0 && io::Error::last_os_error().kind() == ErrorKind::Interrupted {
        continue;
      }
      break;
    }
  }

  fn log_blocked(&self, proto: &str, dst_ip: u32, dst_port: u16) {
    if !self.host.verbose() {
      return;
    }
    let hint = if is_host_ip(dst_ip) { "use --allow-host" } else { "use --allow-lan" };
    self.host.log(&format!("nat: blocked {proto} to {}:{dst_port} ({hint})", Ipv4Addr::from(dst_ip)));
  }

  fn send_rst(&self, flow: &Flow, seq: u32, ack: u32) {
    net::send_rst(&mut Port(self), &flow.guest_mac, flow.dst_ip, flow.src_ip, flow.dst_port, flow.src_port, seq, ack);
  }

  fn guest_udp(&self, flow: &Flow, payload: &[u8]) {
    if payload.len() > MAX_PACKET {
      return;
    }
    let mut nat = lock(&self.nat);
    let index = match nat.find_udp(flow) {
      Some(index) => index,
      None => {
        if !self.policy.egress_allowed(flow.dst_ip, flow.dst_port) {
          self.log_blocked("udp", flow.dst_ip, flow.dst_port);
          return;
        }
        let Ok(sock) = UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0)) else { return };
        let peer = (Ipv4Addr::from(connect_ip(flow.dst_ip)), flow.dst_port);
        if sock.set_nonblocking(true).is_err() || sock.connect(peer).is_err() {
          return;
        }
        let id = nat.next_id();
        self.stats.udp_opened.fetch_add(1, Ordering::Relaxed);
        nat.insert_udp(Udp {
          id,
          sock,
          guest_ip: flow.src_ip,
          guest_port: flow.src_port,
          peer_ip: flow.dst_ip,
          peer_port: flow.dst_port,
          guest_mac: flow.guest_mac,
        })
      }
    };

    let failed = match nat.udp.get(index).and_then(Option::as_ref).map(|u| u.sock.send(payload)) {
      Some(Err(err)) => !matches!(err.kind(), ErrorKind::WouldBlock | ErrorKind::Interrupted),
      _ => false,
    };
    if failed {
      if let Some(slot) = nat.udp.get_mut(index) {
        *slot = None;
      }
    }
    self.wake();
  }

  fn open_tcp(&self, nat: &mut Nat, flow: &Flow, tcp: &TcpInfo) {
    let guest_next = tcp.seq.wrapping_add(1);
    if !self.policy.egress_allowed(flow.dst_ip, flow.dst_port) {
      self.log_blocked("tcp", flow.dst_ip, flow.dst_port);
      self.send_rst(flow, 0, guest_next);
      return;
    }
    let (sock, in_progress) = match connect_tcp(connect_ip(flow.dst_ip), flow.dst_port) {
      Ok(connected) => connected,
      Err(ConnectError::Socket) => return,
      Err(ConnectError::Refused) => {
        self.send_rst(flow, 0, guest_next);
        return;
      }
    };

    let state = if in_progress { TcpState::ConnectingHost } else { TcpState::Established };
    let mut conn = Tcp::new(nat.next_id(), sock, state);
    conn.guest_ip = flow.src_ip;
    conn.guest_port = flow.src_port;
    conn.peer_ip = flow.dst_ip;
    conn.peer_port = flow.dst_port;
    conn.guest_next = guest_next;
    conn.nat_iss = 0x5100_0000u32.wrapping_add(u32::from(flow.src_port) << 4);
    conn.nat_next = conn.nat_iss.wrapping_add(1);
    conn.guest_ack = conn.nat_iss;
    conn.guest_window = u32::from(tcp.window);
    conn.host_base = conn.nat_next;
    conn.guest_mac = flow.guest_mac;
    conn.emit(self, conn.nat_iss, TCP_SYN | TCP_ACK, &[]);
    self.stats.tcp_opened.fetch_add(1, Ordering::Relaxed);
    nat.insert_tcp(conn);
  }

  fn guest_tcp(&self, flow: &Flow, tcp: &TcpInfo, payload: &[u8]) {
    let mut nat = lock(&self.nat);
    let index = nat
      .find_tcp(flow.src_ip, flow.src_port, flow.dst_ip, flow.dst_port)
      .or_else(|| nat.find_tcp(flow.dst_ip, flow.dst_port, flow.src_ip, flow.src_port));

    let Some(index) = index else {
      if tcp.flags & TCP_SYN != 0 && tcp.flags & TCP_ACK == 0 {
        self.open_tcp(&mut nat, flow, tcp);
        self.wake();
      } else if tcp.flags & TCP_RST == 0 {
        self.send_rst(flow, tcp.ack, tcp.seq.wrapping_add(payload.len() as u32));
      }
      return;
    };

    let alive = tcp.flags & TCP_RST == 0
      && nat.tcp.get_mut(index).and_then(Option::as_mut).is_some_and(|conn| conn.guest_segment(self, tcp, payload));
    if !alive {
      if let Some(slot) = nat.tcp.get_mut(index) {
        *slot = None;
      }
    }
    self.wake();
  }

  fn accept_forward(&self, nat: &mut Nat, index: usize) {
    let Some(forward) = nat.forwards.get(index) else { return };
    let guest_port = forward.guest_port;
    let Ok((sock, _)) = forward.listener.accept() else { return };
    let _ = sock.set_nonblocking(true);
    let Some(peer_port) = nat.alloc_inbound_port() else { return };

    let mut conn = Tcp::new(nat.next_id(), sock, TcpState::ConnectingGuest);
    conn.inbound = true;
    conn.guest_ip = GUEST_IP;
    conn.guest_port = guest_port;
    conn.peer_ip = HOST_IP;
    conn.peer_port = peer_port;
    conn.nat_iss = 0x4100_0000u32.wrapping_add(u32::from(peer_port) << 4);
    conn.nat_next = conn.nat_iss.wrapping_add(1);
    conn.guest_ack = conn.nat_iss;
    conn.guest_window = INBOUND_WINDOW;
    conn.host_base = conn.nat_next;
    if let Some(mac) = *lock(&self.guest_mac) {
      conn.guest_mac = mac;
    }
    conn.emit(self, conn.nat_iss, TCP_SYN, &[]);
    self.stats.tcp_opened.fetch_add(1, Ordering::Relaxed);
    nat.insert_tcp(conn);
  }

  fn service_tcp(&self, conn: &mut Tcp, revents: libc::c_short) -> bool {
    if revents & libc::POLLOUT != 0 && !(conn.check_connect() && conn.flush_pending()) {
      return false;
    }
    if revents & (libc::POLLIN | libc::POLLHUP) != 0 && !conn.read_from_host(self) {
      return false;
    }
    revents & libc::POLLERR == 0
  }

  fn service_udp(&self, udp: &Udp, revents: libc::c_short) -> bool {
    if revents & libc::POLLIN == 0 {
      return revents & libc::POLLERR == 0;
    }
    let mut buf = [0u8; MAX_PACKET];
    loop {
      match udp.sock.recv(&mut buf) {
        Ok(0) => return false,
        Ok(read) => {
          let Some(data) = buf.get(..read) else { return false };
          net::send_udp(&mut Port(self), &udp.guest_mac, udp.peer_ip, udp.guest_ip, udp.peer_port, udp.guest_port, data);
        }
        Err(err) if err.kind() == ErrorKind::Interrupted => {}
        Err(err) if err.kind() == ErrorKind::WouldBlock => return true,
        Err(_) => return false,
      }
    }
  }

  fn collect_watches(&self, fds: &mut Vec<libc::pollfd>, watches: &mut Vec<Watch>) {
    let mut watch = |fd: RawFd, events: libc::c_short, what: Watch| {
      fds.push(libc::pollfd { fd, events, revents: 0 });
      watches.push(what);
    };
    watch(self.wake_read.as_raw_fd(), libc::POLLIN, Watch::Wake);

    let nat = lock(&self.nat);
    for (index, forward) in nat.forwards.iter().enumerate() {
      watch(forward.listener.as_raw_fd(), libc::POLLIN, Watch::Forward(index));
    }
    for (index, conn) in nat.tcp.iter().enumerate() {
      let Some(conn) = conn else { continue };
      let Some(sock) = conn.sock.as_ref() else { continue };
      let mut events = 0;
      if conn.wants_host_data() {
        events |= libc::POLLIN;
      }
      if conn.state == TcpState::ConnectingHost || conn.pending_len() > 0 {
        events |= libc::POLLOUT;
      }
      if events != 0 {
        watch(sock.as_raw_fd(), events, Watch::Tcp(index, conn.id));
      }
    }
    for (index, udp) in nat.udp.iter().enumerate() {
      if let Some(udp) = udp {
        watch(udp.sock.as_raw_fd(), libc::POLLIN, Watch::Udp(index, udp.id));
      }
    }
  }

  fn run(&self) {
    let mut fds: Vec<libc::pollfd> = Vec::new();
    let mut watches: Vec<Watch> = Vec::new();

    while !self.stop.load(Ordering::Acquire) {
      fds.clear();
      watches.clear();
      self.collect_watches(&mut fds, &mut watches);

      let timeout = if self.needs_tick() { TICK_MS } else { -1 };
      let ready = unsafe { libc::poll(fds.as_mut_ptr(), fds.len() as libc::nfds_t, timeout) };
      if ready < 0 {
        continue;
      }

      let mut nat = lock(&self.nat);
      for (pfd, watch) in fds.iter().zip(&watches) {
        if pfd.revents == 0 {
          continue;
        }
        match *watch {
          Watch::Wake => self.drain_wake(),
          Watch::Forward(index) => self.accept_forward(&mut nat, index),
          Watch::Tcp(index, id) => {
            let Some(slot) = nat.tcp.get_mut(index) else { continue };
            let alive = match slot.as_mut().filter(|conn| conn.id == id) {
              Some(conn) => self.service_tcp(conn, pfd.revents),
              None => continue,
            };
            if !alive {
              *slot = None;
            }
          }
          Watch::Udp(index, id) => {
            let Some(slot) = nat.udp.get_mut(index) else { continue };
            let alive = match slot.as_ref().filter(|udp| udp.id == id) {
              Some(udp) => self.service_udp(udp, pfd.revents),
              None => continue,
            };
            if !alive {
              *slot = None;
            }
          }
        }
      }

      let now = Instant::now();
      for conn in nat.tcp.iter_mut().flatten() {
        conn.tick(self, now);
      }
    }
  }

  fn needs_tick(&self) -> bool {
    lock(&self.nat).tcp.iter().flatten().any(|conn| conn.awaiting_guest() || conn.has_unsent())
  }
}

#[cfg(test)]
#[path = "tests/nat.rs"]
mod tests;
