use super::*;
use crate::sandbox::net::{ETH_LEN, IPV4_LEN, TCP_LEN, UDP_LEN};
use std::time::{Duration, Instant};

const GUEST_MAC: [u8; MAC_LEN] = [0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee];
const HOST_MAC: [u8; MAC_LEN] = [0x02, 0x41, 0x4e, 0x54, 0x00, 0x01];
const GUEST_PORT: u16 = 42000;

struct TestHost;

impl Host for TestHost {
  fn rx_ready(&self, _: &Net<Self>) {}
  fn wake(&self) {}
  fn verbose(&self) -> bool {
    false
  }
  fn log(&self, _: &str) {}
}

struct Running(Arc<Net<TestHost>>);

impl Drop for Running {
  fn drop(&mut self) {
    self.0.stop();
  }
}

fn start(policy: Policy, forwards: Vec<(u16, u16)>) -> Running {
  Running(Net::start(TestHost, Config { mac: HOST_MAC, policy, forwards }).unwrap())
}

fn allow_host() -> Policy {
  Policy { allow_host_all: true, ..Policy::default() }
}

struct Segment {
  seq: u32,
  ack: u32,
  flags: u8,
  payload: Vec<u8>,
  src_port: u16,
  dst_port: u16,
}

fn guest_tcp_frame(dst_ip: u32, dst_port: u16, seq: u32, ack: u32, flags: u8, window: u16, payload: &[u8]) -> Vec<u8> {
  let mut frame = vec![0xff; MAC_LEN];
  frame.extend_from_slice(&GUEST_MAC);
  frame.extend_from_slice(&0x0800u16.to_be_bytes());
  let mut ip = [0u8; IPV4_LEN];
  ip[0] = 0x45;
  ip[2..4].copy_from_slice(&((IPV4_LEN + TCP_LEN + payload.len()) as u16).to_be_bytes());
  ip[8] = 64;
  ip[9] = 6;
  ip[12..16].copy_from_slice(&GUEST_IP.to_be_bytes());
  ip[16..20].copy_from_slice(&dst_ip.to_be_bytes());
  frame.extend_from_slice(&ip);
  let mut tcp = [0u8; TCP_LEN];
  tcp[0..2].copy_from_slice(&GUEST_PORT.to_be_bytes());
  tcp[2..4].copy_from_slice(&dst_port.to_be_bytes());
  tcp[4..8].copy_from_slice(&seq.to_be_bytes());
  tcp[8..12].copy_from_slice(&ack.to_be_bytes());
  tcp[12] = 5 << 4;
  tcp[13] = flags;
  tcp[14..16].copy_from_slice(&window.to_be_bytes());
  frame.extend_from_slice(&tcp);
  frame.extend_from_slice(payload);
  frame
}

fn guest_udp_frame(dst_ip: u32, src_port: u16, dst_port: u16, payload: &[u8]) -> Vec<u8> {
  let mut frame = vec![0xff; MAC_LEN];
  frame.extend_from_slice(&GUEST_MAC);
  frame.extend_from_slice(&0x0800u16.to_be_bytes());
  let mut ip = [0u8; IPV4_LEN];
  ip[0] = 0x45;
  ip[2..4].copy_from_slice(&((IPV4_LEN + UDP_LEN + payload.len()) as u16).to_be_bytes());
  ip[8] = 64;
  ip[9] = 17;
  ip[12..16].copy_from_slice(&GUEST_IP.to_be_bytes());
  ip[16..20].copy_from_slice(&dst_ip.to_be_bytes());
  frame.extend_from_slice(&ip);
  frame.extend_from_slice(&src_port.to_be_bytes());
  frame.extend_from_slice(&dst_port.to_be_bytes());
  frame.extend_from_slice(&((UDP_LEN + payload.len()) as u16).to_be_bytes());
  frame.extend_from_slice(&[0, 0]);
  frame.extend_from_slice(payload);
  frame
}

fn wait_frame(net: &Net<TestHost>, timeout_ms: u64) -> Option<Vec<u8>> {
  let deadline = Instant::now() + Duration::from_millis(timeout_ms);
  loop {
    if let Some(frame) = net.rx_pop() {
      return Some(frame);
    }
    if Instant::now() >= deadline {
      return None;
    }
    std::thread::sleep(Duration::from_millis(5));
  }
}

fn parse_tcp(frame: &[u8]) -> Segment {
  assert_eq!(frame[ETH_LEN + 9], 6);
  let tcp = &frame[ETH_LEN + IPV4_LEN..];
  Segment {
    src_port: u16::from_be_bytes([tcp[0], tcp[1]]),
    dst_port: u16::from_be_bytes([tcp[2], tcp[3]]),
    seq: u32::from_be_bytes([tcp[4], tcp[5], tcp[6], tcp[7]]),
    ack: u32::from_be_bytes([tcp[8], tcp[9], tcp[10], tcp[11]]),
    flags: tcp[13],
    payload: tcp[TCP_LEN..].to_vec(),
  }
}

fn wait_tcp(net: &Net<TestHost>) -> Segment {
  parse_tcp(&wait_frame(net, 2000).expect("expected a TCP segment for the guest"))
}

fn wait_tcp_where(net: &Net<TestHost>, wanted: impl Fn(&Segment) -> bool) -> Segment {
  let deadline = Instant::now() + Duration::from_secs(3);
  loop {
    let segment = wait_tcp(net);
    if wanted(&segment) {
      return segment;
    }
    assert!(Instant::now() < deadline, "expected segment never arrived");
  }
}

fn drain_data(net: &Net<TestHost>, mut expected_seq: u32, out: &mut Vec<u8>) -> u32 {
  while let Some(frame) = wait_frame(net, 300) {
    let segment = parse_tcp(&frame);
    if segment.payload.is_empty() {
      continue;
    }
    if (expected_seq.wrapping_sub(segment.seq) as i32) > 0 {
      break;
    }
    assert_eq!(segment.seq, expected_seq);
    expected_seq = expected_seq.wrapping_add(segment.payload.len() as u32);
    out.extend_from_slice(&segment.payload);
  }
  expected_seq
}

fn handshake(net: &Net<TestHost>, port: u16, window: u16) -> u32 {
  net.handle_frame(&guest_tcp_frame(HOST_IP, port, 1000, 0, TCP_SYN, window, &[]));
  let synack = wait_tcp(net);
  assert_eq!((synack.src_port, synack.dst_port), (port, GUEST_PORT));
  assert_eq!(synack.flags & (TCP_SYN | TCP_ACK), TCP_SYN | TCP_ACK);
  assert_eq!(synack.ack, 1001);
  let data_seq = synack.seq.wrapping_add(1);
  net.handle_frame(&guest_tcp_frame(HOST_IP, port, 1001, data_seq, TCP_ACK, window, &[]));
  data_seq
}

#[test]
fn egress_policy() {
  let ip = net::ipv4;
  let mut policy = Policy::default();
  assert!(policy.egress_allowed(DNS_IP, 53));
  assert!(!policy.egress_allowed(DNS_IP, 80));
  for (addr, port) in [
    (HOST_IP, 5432),
    (ip(127, 0, 0, 1), 5432),
    (ip(127, 1, 2, 3), 22),
    (ip(0, 0, 0, 0), 5432),
    (ip(169, 254, 169, 254), 80),
    (ip(10, 1, 2, 3), 80),
    (ip(172, 16, 0, 1), 80),
    (ip(172, 31, 255, 255), 80),
    (ip(192, 168, 1, 1), 80),
    (ip(100, 100, 100, 100), 80),
    (ip(224, 0, 0, 251), 5353),
    (BROADCAST_IP, 9),
  ] {
    assert!(!policy.egress_allowed(addr, port), "{}:{port}", Ipv4Addr::from(addr));
  }
  assert!(policy.egress_allowed(ip(172, 32, 0, 1), 80));
  assert!(policy.egress_allowed(ip(100, 128, 0, 1), 80));
  assert!(policy.egress_allowed(ip(1, 1, 1, 1), 443));

  policy.allow_host_ports = vec![5432, 6379];
  assert!(policy.egress_allowed(HOST_IP, 5432));
  assert!(policy.egress_allowed(ip(127, 0, 0, 1), 6379));
  assert!(!policy.egress_allowed(HOST_IP, 22));
  assert!(!policy.egress_allowed(ip(169, 254, 169, 254), 80));

  policy.allow_host_all = true;
  assert!(policy.egress_allowed(HOST_IP, 22));
  assert!(!policy.egress_allowed(ip(192, 168, 1, 1), 80));

  policy.allow_lan = true;
  assert!(policy.egress_allowed(ip(192, 168, 1, 1), 80));
  assert!(policy.egress_allowed(ip(169, 254, 169, 254), 80));
  assert!(!policy.egress_allowed(DNS_IP, 80));
}

#[test]
fn resolv_conf_parsing() {
  assert_eq!(resolv_conf_dns_ip("# comment\nsearch lan\nnameserver 192.168.1.1\nnameserver 1.1.1.1\n"), Some(net::ipv4(192, 168, 1, 1)));
  assert_eq!(resolv_conf_dns_ip("nameserver fe80::1\n  nameserver\t9.9.9.9#x\n"), Some(net::ipv4(9, 9, 9, 9)));
  assert_eq!(resolv_conf_dns_ip("nameserverx 1.1.1.1\nnameserver 0.0.0.0\nnameserver 255.255.255.255\n"), None);
  assert_eq!(resolv_conf_dns_ip(""), None);
}

#[test]
fn tcp_round_trip_and_close() {
  let net = start(allow_host(), Vec::new());
  let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
  let port = listener.local_addr().unwrap().port();
  let data_seq = handshake(&net.0, port, 65535);
  let (mut peer, _) = listener.accept().unwrap();
  peer.set_read_timeout(Some(Duration::from_secs(2))).unwrap();

  net.0.handle_frame(&guest_tcp_frame(HOST_IP, port, 1001, data_seq, TCP_ACK | TCP_PSH, 65535, b"hello host"));
  let ack = wait_tcp(&net.0);
  assert_eq!((ack.flags, ack.ack), (TCP_ACK, 1011));
  let mut received = [0u8; 10];
  peer.read_exact(&mut received).unwrap();
  assert_eq!(&received, b"hello host");

  peer.write_all(b"hello guest").unwrap();
  let mut body = Vec::new();
  let next_seq = drain_data(&net.0, data_seq, &mut body);
  assert_eq!(body, b"hello guest");

  net.0.handle_frame(&guest_tcp_frame(HOST_IP, port, 1011, next_seq, TCP_ACK, 65535, &[]));
  drop(peer);
  let fin = wait_tcp_where(&net.0, |segment| segment.flags & TCP_FIN != 0);
  assert_eq!(fin.seq, next_seq);

  net.0.handle_frame(&guest_tcp_frame(HOST_IP, port, 1011, next_seq.wrapping_add(1), TCP_FIN | TCP_ACK, 65535, &[]));
  let last = wait_tcp_where(&net.0, |segment| segment.flags == TCP_ACK);
  assert_eq!(last.ack, 1012);
  assert!(lock(&net.0.nat).tcp.iter().all(Option::is_none));

  net.0.handle_frame(&guest_tcp_frame(HOST_IP, port, 1012, 0, TCP_ACK, 65535, &[]));
  assert_eq!(wait_tcp(&net.0).flags, TCP_RST | TCP_ACK);
}

#[test]
fn guest_reset_drops_the_connection() {
  let net = start(allow_host(), Vec::new());
  let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
  let port = listener.local_addr().unwrap().port();
  let data_seq = handshake(&net.0, port, 65535);
  let (mut peer, _) = listener.accept().unwrap();
  peer.set_read_timeout(Some(Duration::from_secs(2))).unwrap();

  net.0.handle_frame(&guest_tcp_frame(HOST_IP, port, 1001, data_seq, TCP_RST, 0, &[]));
  assert!(lock(&net.0.nat).tcp.iter().all(Option::is_none));
  let mut byte = [0u8; 1];
  assert!(matches!(peer.read(&mut byte), Ok(0) | Err(_)));
  assert!(wait_frame(&net.0, 100).is_none());
}

#[test]
fn zero_window_stops_host_data() {
  let net = start(allow_host(), Vec::new());
  let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
  let port = listener.local_addr().unwrap().port();
  let data_seq = handshake(&net.0, port, 2000);
  let (mut peer, _) = listener.accept().unwrap();
  peer.write_all(&[0x61; 6000]).unwrap();

  let mut body = Vec::new();
  let mut next_seq = drain_data(&net.0, data_seq, &mut body);
  assert_eq!(body.len(), 2000);

  net.0.handle_frame(&guest_tcp_frame(HOST_IP, port, 1001, next_seq, TCP_ACK, 0, &[]));
  next_seq = drain_data(&net.0, next_seq, &mut body);
  assert_eq!(body.len(), 2000);

  net.0.handle_frame(&guest_tcp_frame(HOST_IP, port, 1001, next_seq, TCP_ACK, 4000, &[]));
  next_seq = drain_data(&net.0, next_seq, &mut body);
  assert_eq!(body.len(), 6000);
  assert_eq!(next_seq, data_seq.wrapping_add(6000));
}

#[test]
fn host_ports_are_blocked_by_default() {
  let net = start(Policy::default(), Vec::new());
  let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
  listener.set_nonblocking(true).unwrap();
  let port = listener.local_addr().unwrap().port();

  for target in [HOST_IP, LOOPBACK_IP] {
    net.0.handle_frame(&guest_tcp_frame(target, port, 1000, 0, TCP_SYN, 65535, &[]));
    let rst = wait_tcp(&net.0);
    assert_eq!(rst.src_port, port);
    assert_eq!(rst.flags & TCP_RST, TCP_RST);
    assert_eq!(rst.ack, 1001);
  }
  std::thread::sleep(Duration::from_millis(100));
  assert_eq!(listener.accept().err().map(|err| err.kind()), Some(ErrorKind::WouldBlock));
  assert!(lock(&net.0.nat).tcp.is_empty());
}

#[test]
fn refused_connection_resets_the_guest() {
  let net = start(allow_host(), Vec::new());
  let port = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap().local_addr().unwrap().port();
  net.0.handle_frame(&guest_tcp_frame(HOST_IP, port, 1000, 0, TCP_SYN, 65535, &[]));
  let first = wait_tcp(&net.0);
  if first.flags & TCP_RST == 0 {
    assert_eq!(first.flags & (TCP_SYN | TCP_ACK), TCP_SYN | TCP_ACK);
    let deadline = Instant::now() + Duration::from_secs(2);
    while lock(&net.0.nat).tcp.iter().any(Option::is_some) {
      assert!(Instant::now() < deadline, "refused connection was never reaped");
      std::thread::sleep(Duration::from_millis(5));
    }
  }
}

#[test]
fn forwarded_port_opens_a_guest_connection() {
  let host_port = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap().local_addr().unwrap().port();
  let net = start(Policy::default(), vec![(host_port, 3000)]);
  assert_eq!(net.0.forward_count(), 1);

  let mut client = TcpStream::connect((Ipv4Addr::LOCALHOST, host_port)).unwrap();
  client.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
  let syn = wait_tcp(&net.0);
  assert_eq!(syn.dst_port, 3000);
  assert_eq!(syn.flags, TCP_SYN);
  assert!(syn.src_port >= INBOUND_PORT_FIRST);

  let mut reply = guest_tcp_frame(HOST_IP, syn.src_port, 7000, syn.seq.wrapping_add(1), TCP_SYN | TCP_ACK, 65535, &[]);
  reply[ETH_LEN + IPV4_LEN..ETH_LEN + IPV4_LEN + 2].copy_from_slice(&3000u16.to_be_bytes());
  net.0.handle_frame(&reply);
  let ack = wait_tcp(&net.0);
  assert_eq!((ack.flags, ack.ack), (TCP_ACK, 7001));

  client.write_all(b"ping").unwrap();
  let mut body = Vec::new();
  drain_data(&net.0, syn.seq.wrapping_add(1), &mut body);
  assert_eq!(body, b"ping");

  let mut data = guest_tcp_frame(HOST_IP, syn.src_port, 7001, syn.seq.wrapping_add(5), TCP_ACK | TCP_PSH, 65535, b"pong");
  data[ETH_LEN + IPV4_LEN..ETH_LEN + IPV4_LEN + 2].copy_from_slice(&3000u16.to_be_bytes());
  net.0.handle_frame(&data);
  let mut received = [0u8; 4];
  client.read_exact(&mut received).unwrap();
  assert_eq!(&received, b"pong");
}

#[test]
fn udp_round_trip() {
  let net = start(allow_host(), Vec::new());
  let server = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
  server.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
  let port = server.local_addr().unwrap().port();

  net.0.handle_frame(&guest_udp_frame(HOST_IP, 54000, port, b"ping"));
  let mut buf = [0u8; 16];
  let (len, peer) = server.recv_from(&mut buf).unwrap();
  assert_eq!(&buf[..len], b"ping");
  server.send_to(b"pong", peer).unwrap();

  let frame = wait_frame(&net.0, 2000).unwrap();
  assert_eq!(frame[..MAC_LEN], GUEST_MAC);
  assert_eq!(frame[ETH_LEN + 9], 17);
  let udp = &frame[ETH_LEN + IPV4_LEN..];
  assert_eq!(u16::from_be_bytes([udp[0], udp[1]]), port);
  assert_eq!(u16::from_be_bytes([udp[2], udp[3]]), 54000);
  assert_eq!(&udp[UDP_LEN..], b"pong");

  let blocked = start(Policy::default(), Vec::new());
  blocked.0.handle_frame(&guest_udp_frame(HOST_IP, 54000, port, b"ping"));
  assert!(lock(&blocked.0.nat).udp.is_empty());
}

#[test]
fn backlog_overflow_is_counted_not_queued() {
  let net = start(Policy::default(), Vec::new());
  for _ in 0..RX_BACKLOG + 3 {
    net.0.enqueue(&[0u8; 60]);
  }
  net.0.enqueue(&[]);
  net.0.enqueue(&[0u8; MAX_PACKET + 1]);
  assert_eq!(lock(&net.0.rx).packets.len(), RX_BACKLOG);
  assert_eq!(net.0.stats.guest_drops.load(Ordering::Relaxed), 3);
  assert!(net.0.take_rx_wake());
  assert!(!net.0.take_rx_wake());

  let mut out = [0u8; 16];
  assert_eq!(net.0.rx_peek(&mut out), None);
  let mut out = [0u8; MAX_PACKET];
  assert_eq!(net.0.rx_peek(&mut out), Some(60));
}

fn data_ack(net: &Net<TestHost>, port: u16, seq: u32, ack: u32) {
  net.handle_frame(&guest_tcp_frame(HOST_IP, port, seq, ack, TCP_ACK, 65535, &[]));
}

#[test]
fn full_backlog_delays_data_without_losing_it() {
  let net = start(allow_host(), Vec::new());
  let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
  let port = listener.local_addr().unwrap().port();
  let data_seq = handshake(&net.0, port, 65535);
  let (mut peer, _) = listener.accept().unwrap();

  let filler = [0u8; 60];
  while net.0.enqueue(&filler) {}
  let sent: Vec<u8> = (0..20000u32).map(|i| (i % 251) as u8).collect();
  peer.write_all(&sent).unwrap();
  std::thread::sleep(Duration::from_millis(100));
  assert_eq!(lock(&net.0.rx).packets.len(), RX_BACKLOG);
  assert!(lock(&net.0.rx).packets.iter().all(|packet| packet.len() == filler.len()));

  let mut body = Vec::new();
  let mut expected = data_seq;
  let deadline = Instant::now() + Duration::from_secs(5);
  while body.len() < sent.len() {
    assert!(Instant::now() < deadline, "stalled after {} bytes", body.len());
    let Some(frame) = wait_frame(&net.0, 500) else { continue };
    if frame.len() == filler.len() {
      continue;
    }
    let segment = parse_tcp(&frame);
    if segment.payload.is_empty() || segment.seq != expected {
      continue;
    }
    expected = expected.wrapping_add(segment.payload.len() as u32);
    body.extend_from_slice(&segment.payload);
    data_ack(&net.0, port, 1001, expected);
  }
  assert_eq!(body, sent);
  assert!(net.0.stats.guest_drops.load(Ordering::Relaxed) > 0);
}

#[test]
fn lost_segments_are_retransmitted() {
  let net = start(allow_host(), Vec::new());
  let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
  let port = listener.local_addr().unwrap().port();
  let data_seq = handshake(&net.0, port, 65535);
  let (mut peer, _) = listener.accept().unwrap();
  peer.write_all(&[7u8; 4000]).unwrap();

  let first = wait_tcp_where(&net.0, |segment| !segment.payload.is_empty());
  assert_eq!(first.seq, data_seq);
  let after_first = data_seq.wrapping_add(first.payload.len() as u32);
  while wait_frame(&net.0, 60).is_some() {}

  for _ in 0..3 {
    data_ack(&net.0, port, 1001, after_first);
  }
  let resent = wait_tcp_where(&net.0, |segment| !segment.payload.is_empty());
  assert_eq!(resent.seq, after_first);

  while wait_frame(&net.0, 60).is_some() {}
  let timed_out = wait_tcp_where(&net.0, |segment| !segment.payload.is_empty());
  assert_eq!(timed_out.seq, after_first);

  let mut body = first.payload.clone();
  let mut expected = after_first;
  let deadline = Instant::now() + Duration::from_secs(5);
  while body.len() < 4000 {
    assert!(Instant::now() < deadline);
    let Some(frame) = wait_frame(&net.0, 500) else { continue };
    let segment = parse_tcp(&frame);
    if segment.payload.is_empty() || segment.seq != expected {
      continue;
    }
    expected = expected.wrapping_add(segment.payload.len() as u32);
    body.extend_from_slice(&segment.payload);
    data_ack(&net.0, port, 1001, expected);
  }
  assert_eq!(body, vec![7u8; 4000]);
}

#[test]
fn repeated_guest_segments_reach_the_host_once() {
  let net = start(allow_host(), Vec::new());
  let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
  let port = listener.local_addr().unwrap().port();
  let data_seq = handshake(&net.0, port, 65535);
  let (mut peer, _) = listener.accept().unwrap();
  peer.set_read_timeout(Some(Duration::from_millis(300))).unwrap();

  let send = |seq: u32, payload: &[u8]| {
    net.0.handle_frame(&guest_tcp_frame(HOST_IP, port, seq, data_seq, TCP_ACK | TCP_PSH, 65535, payload));
  };
  send(1001, b"abcde");
  send(1001, b"abcde");
  send(1003, b"cdefgh");
  send(1020, b"zzz");
  send(1009, b"ij");

  let mut received = Vec::new();
  let mut chunk = [0u8; 64];
  while let Ok(read) = peer.read(&mut chunk) {
    if read == 0 {
      break;
    }
    received.extend_from_slice(&chunk[..read]);
  }
  assert_eq!(received, b"abcdefghij");
  let acks: Vec<u32> = std::iter::from_fn(|| wait_frame(&net.0, 50)).map(|frame| parse_tcp(&frame).ack).collect();
  assert_eq!(acks.last(), Some(&1011));
  assert!(acks.contains(&1009));

  net.0.handle_frame(&guest_tcp_frame(HOST_IP, port, 1000, 0, TCP_SYN, 65535, &[]));
  assert!(wait_frame(&net.0, 100).is_none());
}

#[test]
fn repeated_syn_gets_the_same_syn_ack() {
  let net = start(allow_host(), Vec::new());
  let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
  let port = listener.local_addr().unwrap().port();
  net.0.handle_frame(&guest_tcp_frame(HOST_IP, port, 1000, 0, TCP_SYN, 65535, &[]));
  let first = wait_tcp(&net.0);
  net.0.handle_frame(&guest_tcp_frame(HOST_IP, port, 1000, 0, TCP_SYN, 65535, &[]));
  let second = wait_tcp(&net.0);
  assert_eq!(second.flags, TCP_SYN | TCP_ACK);
  assert_eq!((second.seq, second.ack), (first.seq, first.ack));
  assert_eq!(lock(&net.0.nat).tcp.iter().flatten().count(), 1);
}
