use super::*;

const GUEST_MAC: [u8; MAC_LEN] = [0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee];
const HOST_MAC: [u8; MAC_LEN] = [0x02, 0x41, 0x4e, 0x54, 0x00, 0x01];

#[derive(Default)]
struct TestLink {
  guest_mac: Option<[u8; MAC_LEN]>,
  frames: Vec<Vec<u8>>,
  nat_udp: Vec<(Flow, Vec<u8>)>,
  nat_tcp: Vec<(Flow, TcpInfo, Vec<u8>)>,
  guest_packets: usize,
}

impl FrameSink for TestLink {
  fn host_mac(&self) -> [u8; MAC_LEN] {
    HOST_MAC
  }
  fn enqueue(&mut self, frame: &[u8]) -> bool {
    self.frames.push(frame.to_vec());
    true
  }
}

impl Stack for TestLink {
  fn set_guest_mac(&mut self, mac: &[u8; MAC_LEN]) {
    self.guest_mac = Some(*mac);
  }
  fn note_guest_packet(&mut self) {
    self.guest_packets += 1;
  }
  fn nat_udp(&mut self, flow: &Flow, payload: &[u8]) {
    self.nat_udp.push((*flow, payload.to_vec()));
  }
  fn nat_tcp(&mut self, flow: &Flow, tcp: &TcpInfo, payload: &[u8]) {
    self.nat_tcp.push((*flow, *tcp, payload.to_vec()));
  }
}

fn ipv4_frame(proto: u8, src: u32, dst: u32, l4: &[u8]) -> Vec<u8> {
  let mut frame = vec![0xff; MAC_LEN];
  frame.extend_from_slice(&GUEST_MAC);
  frame.extend_from_slice(&ETH_IPV4.to_be_bytes());
  let mut ip = [0u8; IPV4_LEN];
  ip[0] = 0x45;
  ip[2..4].copy_from_slice(&((IPV4_LEN + l4.len()) as u16).to_be_bytes());
  ip[8] = 64;
  ip[9] = proto;
  ip[12..16].copy_from_slice(&src.to_be_bytes());
  ip[16..20].copy_from_slice(&dst.to_be_bytes());
  frame.extend_from_slice(&ip);
  frame.extend_from_slice(l4);
  frame
}

fn udp_frame(src: u32, dst: u32, sport: u16, dport: u16, payload: &[u8]) -> Vec<u8> {
  let mut udp = Vec::new();
  udp.extend_from_slice(&sport.to_be_bytes());
  udp.extend_from_slice(&dport.to_be_bytes());
  udp.extend_from_slice(&((UDP_LEN + payload.len()) as u16).to_be_bytes());
  udp.extend_from_slice(&[0, 0]);
  udp.extend_from_slice(payload);
  ipv4_frame(IP_UDP, src, dst, &udp)
}

fn udp_payload(frame: &[u8]) -> &[u8] {
  &frame[ETH_LEN + IPV4_LEN + UDP_LEN..]
}

#[test]
fn checksums_match_reference_values() {
  assert_eq!(csum(&[0x00, 0x01], 0), 0xfffe);
  assert_eq!(csum(&[0xff, 0xff], 0), 0x0000);
  assert_eq!(csum(&[0x01], 0), 0xfeff);

  let mut tcp = [0u8; TCP_LEN];
  tcp[0..2].copy_from_slice(&1234u16.to_be_bytes());
  tcp[2..4].copy_from_slice(&443u16.to_be_bytes());
  tcp[12] = 5 << 4;
  tcp[13] = 0x02;
  assert_eq!(l4_csum(GUEST_IP, 0x5db8d822, IP_TCP, &tcp), 0x676c);
}

#[test]
fn emitted_headers_verify() {
  let mut link = TestLink::default();
  send_tcp(&mut link, &GUEST_MAC, HOST_IP, GUEST_IP, 80, 40000, 7, 9, TCP_ACK, b"hello");
  let frame = &link.frames[0];
  assert_eq!(frame.len(), ETH_LEN + IPV4_LEN + TCP_LEN + 5);
  assert_eq!(frame[..MAC_LEN], GUEST_MAC);
  assert_eq!(frame[MAC_LEN..2 * MAC_LEN], HOST_MAC);
  assert_eq!(csum(&frame[ETH_LEN..ETH_LEN + IPV4_LEN], 0), 0);
  assert_eq!(l4_csum(HOST_IP, GUEST_IP, IP_TCP, &frame[ETH_LEN + IPV4_LEN..]), 0);
}

#[test]
fn rst_carries_reset_and_ack() {
  let mut link = TestLink::default();
  send_rst(&mut link, &GUEST_MAC, HOST_IP, GUEST_IP, 80, 40000, 7, 9);
  let tcp = &link.frames[0][ETH_LEN + IPV4_LEN..];
  assert_eq!(tcp.len(), TCP_LEN);
  assert_eq!(tcp[13], TCP_RST | TCP_ACK);
  assert_eq!(l4_csum(HOST_IP, GUEST_IP, IP_TCP, tcp), 0);
}

#[test]
fn oversized_payloads_are_dropped() {
  let mut link = TestLink::default();
  send_tcp(&mut link, &GUEST_MAC, HOST_IP, GUEST_IP, 1, 2, 0, 0, TCP_ACK, &[0; TCP_MSS + 1]);
  send_udp(&mut link, &GUEST_MAC, HOST_IP, GUEST_IP, 1, 2, &[0; STANDARD_PACKET + 1]);
  assert!(link.frames.is_empty());
  send_udp(&mut link, &GUEST_MAC, HOST_IP, GUEST_IP, 1, 2, &[0; STANDARD_PACKET]);
  assert_eq!(link.frames[0].len(), ETH_LEN + IPV4_LEN + UDP_LEN + STANDARD_PACKET);
}

#[test]
fn arp_request_for_host_gets_reply() {
  let mut frame = vec![0xff; MAC_LEN];
  frame.extend_from_slice(&GUEST_MAC);
  frame.extend_from_slice(&ETH_ARP.to_be_bytes());
  let mut arp = [0u8; ARP_LEN];
  arp[0..2].copy_from_slice(&1u16.to_be_bytes());
  arp[2..4].copy_from_slice(&ETH_IPV4.to_be_bytes());
  arp[4] = 6;
  arp[5] = 4;
  arp[6..8].copy_from_slice(&1u16.to_be_bytes());
  arp[8..14].copy_from_slice(&GUEST_MAC);
  arp[14..18].copy_from_slice(&GUEST_IP.to_be_bytes());
  arp[24..28].copy_from_slice(&HOST_IP.to_be_bytes());
  frame.extend_from_slice(&arp);

  let mut link = TestLink::default();
  handle_frame(&mut link, &frame);
  assert_eq!(link.guest_mac, Some(GUEST_MAC));
  let reply = &link.frames[0];
  assert_eq!(reply.len(), ETH_LEN + ARP_LEN);
  assert_eq!(reply[20..22], 2u16.to_be_bytes());
  assert_eq!(reply[22..28], HOST_MAC);
  assert_eq!(reply[28..32], HOST_IP.to_be_bytes());
  assert_eq!(reply[32..38], GUEST_MAC);
  assert_eq!(reply[38..42], GUEST_IP.to_be_bytes());

  let mut other = frame.clone();
  other[ETH_LEN + 24..ETH_LEN + 28].copy_from_slice(&ipv4(10, 0, 2, 9).to_be_bytes());
  let mut link = TestLink::default();
  handle_frame(&mut link, &other);
  assert!(link.frames.is_empty());
}

#[test]
fn dhcp_discover_gets_offer() {
  let mut dhcp = vec![0u8; 260];
  dhcp[0] = 1;
  dhcp[1] = 1;
  dhcp[2] = 6;
  dhcp[4..8].copy_from_slice(&[0x12, 0x34, 0x56, 0x78]);
  dhcp[28..34].copy_from_slice(&GUEST_MAC);
  dhcp[236..240].copy_from_slice(&DHCP_MAGIC);
  dhcp[240..244].copy_from_slice(&[53, 1, 1, 255]);

  let mut link = TestLink::default();
  handle_frame(&mut link, &udp_frame(0, BROADCAST_IP, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, &dhcp));
  let frame = &link.frames[0];
  assert_eq!(frame[..MAC_LEN], GUEST_MAC);
  let resp = udp_payload(frame);
  assert_eq!(resp.len(), DHCP_HEADER_LEN + DHCP_OPTIONS_LEN);
  assert_eq!(resp[0], 2);
  assert_eq!(resp[4..8], [0x12, 0x34, 0x56, 0x78]);
  assert_eq!(resp[16..20], GUEST_IP.to_be_bytes());
  assert_eq!(resp[240..243], [53, 1, 2]);
  assert_eq!(resp[resp.len() - 1], 255);

  dhcp[10] = 0x80;
  dhcp[242] = 3;
  let mut link = TestLink::default();
  handle_frame(&mut link, &udp_frame(0, BROADCAST_IP, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, &dhcp));
  let frame = &link.frames[0];
  assert_eq!(frame[..MAC_LEN], [0xff; MAC_LEN]);
  assert_eq!(udp_payload(frame)[240..243], [53, 1, 5]);
}

#[test]
fn dhcp_options_stop_at_truncation() {
  assert_eq!(dhcp_message_type(&[0, 0, 53, 1, 3, 255]), 3);
  assert_eq!(dhcp_message_type(&[12, 2, b'a', b'b', 53, 1, 1]), 1);
  assert_eq!(dhcp_message_type(&[255, 53, 1, 1]), 0);
  assert_eq!(dhcp_message_type(&[53, 0, 53, 1, 1]), 1);
  assert_eq!(dhcp_message_type(&[53, 4, 1]), 0);
  assert_eq!(dhcp_message_type(&[53]), 0);
  assert_eq!(dhcp_message_type(&[]), 0);
}

#[test]
fn icmp_echo_to_host_is_answered() {
  let mut echo = vec![8, 0, 0, 0, 0x12, 0x34, 0, 1];
  echo.extend_from_slice(b"payload");
  let mut link = TestLink::default();
  handle_frame(&mut link, &ipv4_frame(IP_ICMP, GUEST_IP, HOST_IP, &echo));
  let frame = &link.frames[0];
  let reply = &frame[ETH_LEN + IPV4_LEN..];
  assert_eq!(reply.len(), echo.len());
  assert_eq!(reply[0], 0);
  assert_eq!(reply[4..], echo[4..]);
  assert_eq!(csum(reply, 0), 0);

  let mut link = TestLink::default();
  handle_frame(&mut link, &ipv4_frame(IP_ICMP, GUEST_IP, ipv4(1, 1, 1, 1), &echo));
  assert!(link.frames.is_empty());
}

#[test]
fn udp_and_tcp_go_to_nat_with_parsed_headers() {
  let mut link = TestLink::default();
  handle_frame(&mut link, &udp_frame(GUEST_IP, DNS_IP, 53000, DNS_PORT, b"query"));
  handle_frame(&mut link, &udp_frame(GUEST_IP, ipv4(1, 1, 1, 1), 53000, 9999, b"ping"));
  assert!(link.frames.is_empty());
  assert_eq!(link.guest_packets, 2);
  let flow = Flow { guest_mac: GUEST_MAC, src_ip: GUEST_IP, dst_ip: ipv4(1, 1, 1, 1), src_port: 53000, dst_port: 9999 };
  assert_eq!(link.nat_udp[0].0.dst_port, DNS_PORT);
  assert_eq!(link.nat_udp[1], (flow, b"ping".to_vec()));

  let mut tcp = [0u8; TCP_LEN + 4 + 3];
  tcp[0..2].copy_from_slice(&40000u16.to_be_bytes());
  tcp[2..4].copy_from_slice(&443u16.to_be_bytes());
  tcp[4..8].copy_from_slice(&7u32.to_be_bytes());
  tcp[8..12].copy_from_slice(&9u32.to_be_bytes());
  tcp[12] = 6 << 4;
  tcp[13] = TCP_ACK | TCP_PSH;
  tcp[14..16].copy_from_slice(&1234u16.to_be_bytes());
  tcp[TCP_LEN + 4..].copy_from_slice(b"abc");
  handle_frame(&mut link, &ipv4_frame(IP_TCP, GUEST_IP, ipv4(1, 1, 1, 1), &tcp));
  let (flow, info, body) = &link.nat_tcp[0];
  assert_eq!((flow.src_port, flow.dst_port), (40000, 443));
  assert_eq!(*info, TcpInfo { seq: 7, ack: 9, flags: TCP_ACK | TCP_PSH, window: 1234 });
  assert_eq!(body, b"abc");
}

#[test]
fn malformed_lengths_are_ignored() {
  let good = udp_frame(GUEST_IP, DNS_IP, 53000, DNS_PORT, b"payload");
  for len in 0..good.len() {
    let mut link = TestLink::default();
    handle_frame(&mut link, &good[..len]);
    assert!(link.frames.is_empty() && link.nat_udp.is_empty());
  }

  let mut mutations: Vec<Vec<u8>> = Vec::new();
  let mut bad_version = good.clone();
  bad_version[ETH_LEN] = 0x65;
  mutations.push(bad_version);
  let mut short_ihl = good.clone();
  short_ihl[ETH_LEN] = 0x44;
  mutations.push(short_ihl);
  let mut long_ihl = good.clone();
  long_ihl[ETH_LEN] = 0x4f;
  long_ihl[ETH_LEN + 2..ETH_LEN + 4].copy_from_slice(&40u16.to_be_bytes());
  mutations.push(long_ihl);
  let mut long_total = good.clone();
  long_total[ETH_LEN + 2..ETH_LEN + 4].copy_from_slice(&u16::MAX.to_be_bytes());
  mutations.push(long_total);
  let mut long_udp = good.clone();
  long_udp[ETH_LEN + IPV4_LEN + 4..ETH_LEN + IPV4_LEN + 6].copy_from_slice(&u16::MAX.to_be_bytes());
  mutations.push(long_udp);
  let mut short_udp = good.clone();
  short_udp[ETH_LEN + IPV4_LEN + 4..ETH_LEN + IPV4_LEN + 6].copy_from_slice(&7u16.to_be_bytes());
  mutations.push(short_udp);

  for frame in mutations {
    let mut link = TestLink::default();
    handle_frame(&mut link, &frame);
    assert!(link.frames.is_empty() && link.nat_udp.is_empty());
  }

  let mut tcp = [0u8; TCP_LEN];
  tcp[12] = 4 << 4;
  let mut link = TestLink::default();
  handle_frame(&mut link, &ipv4_frame(IP_TCP, GUEST_IP, HOST_IP, &tcp));
  tcp[12] = 15 << 4;
  handle_frame(&mut link, &ipv4_frame(IP_TCP, GUEST_IP, HOST_IP, &tcp));
  assert!(link.nat_tcp.is_empty());
}
