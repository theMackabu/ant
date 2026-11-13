use core::sync::atomic::{AtomicU32, Ordering};

pub(crate) const MAC_LEN: usize = 6;
pub(crate) const ETH_LEN: usize = 14;
pub(crate) const ARP_LEN: usize = 28;
pub(crate) const IPV4_LEN: usize = 20;
pub(crate) const UDP_LEN: usize = 8;
pub(crate) const TCP_LEN: usize = 20;

pub(crate) const MAX_PACKET: usize = 2048;
const STANDARD_PACKET: usize = 1500;
pub(crate) const TCP_MSS: usize = 1380;
const DHCP_HEADER_LEN: usize = 240;
const DHCP_OPTIONS_LEN: usize = 34;
const DHCP_MAGIC: [u8; 4] = [0x63, 0x82, 0x53, 0x63];

pub(crate) const HOST_IP: u32 = ipv4(10, 0, 2, 2);
pub(crate) const DNS_IP: u32 = ipv4(10, 0, 2, 3);
pub(crate) const GUEST_IP: u32 = ipv4(10, 0, 2, 15);
const MASK_IP: u32 = ipv4(255, 255, 255, 0);
pub(crate) const BROADCAST_IP: u32 = ipv4(255, 255, 255, 255);

const ETH_IPV4: u16 = 0x0800;
const ETH_ARP: u16 = 0x0806;
const IP_ICMP: u8 = 1;
const IP_TCP: u8 = 6;
const IP_UDP: u8 = 17;
pub(crate) const TCP_FIN: u8 = 0x01;
pub(crate) const TCP_SYN: u8 = 0x02;
pub(crate) const TCP_RST: u8 = 0x04;
pub(crate) const TCP_PSH: u8 = 0x08;
pub(crate) const TCP_ACK: u8 = 0x10;
const DHCP_SERVER_PORT: u16 = 67;
const DHCP_CLIENT_PORT: u16 = 68;
pub(crate) const DNS_PORT: u16 = 53;

static NEXT_IP_ID: AtomicU32 = AtomicU32::new(0);

pub(crate) const fn ipv4(a: u8, b: u8, c: u8, d: u8) -> u32 {
  u32::from_be_bytes([a, b, c, d])
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Flow {
  pub guest_mac: [u8; MAC_LEN],
  pub src_ip: u32,
  pub dst_ip: u32,
  pub src_port: u16,
  pub dst_port: u16,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct TcpInfo {
  pub seq: u32,
  pub ack: u32,
  pub flags: u8,
  pub window: u16,
}

pub(crate) trait FrameSink {
  fn host_mac(&self) -> [u8; MAC_LEN];
  fn enqueue(&mut self, frame: &[u8]) -> bool;
}

pub(crate) trait Stack: FrameSink {
  fn set_guest_mac(&mut self, mac: &[u8; MAC_LEN]);
  fn note_guest_packet(&mut self);
  fn nat_udp(&mut self, flow: &Flow, payload: &[u8]);
  fn nat_tcp(&mut self, flow: &Flow, tcp: &TcpInfo, payload: &[u8]);
}

fn eth_src(eth: &[u8; ETH_LEN]) -> [u8; MAC_LEN] {
  [eth[6], eth[7], eth[8], eth[9], eth[10], eth[11]]
}

fn ip_src(ip: &[u8; IPV4_LEN]) -> u32 {
  u32::from_be_bytes([ip[12], ip[13], ip[14], ip[15]])
}

fn ip_dst(ip: &[u8; IPV4_LEN]) -> u32 {
  u32::from_be_bytes([ip[16], ip[17], ip[18], ip[19]])
}

pub(crate) fn csum(data: &[u8], mut sum: u32) -> u16 {
  let mut words = data.chunks_exact(2);
  for word in &mut words {
    sum = sum.wrapping_add(u32::from(u16::from_be_bytes([word[0], word[1]])));
  }
  if let [last] = words.remainder() {
    sum = sum.wrapping_add(u32::from(*last) << 8);
  }
  while sum >> 16 != 0 {
    sum = (sum & 0xffff) + (sum >> 16);
  }
  !(sum as u16)
}

pub(crate) fn l4_csum(src: u32, dst: u32, proto: u8, data: &[u8]) -> u16 {
  let sum = (src >> 16)
    .wrapping_add(src & 0xffff)
    .wrapping_add(dst >> 16)
    .wrapping_add(dst & 0xffff)
    .wrapping_add(u32::from(proto))
    .wrapping_add(data.len() as u32);
  csum(data, sum)
}

fn send_ipv4<L: FrameSink>(
  link: &mut L,
  dst_mac: &[u8; MAC_LEN],
  src_ip: u32,
  dst_ip: u32,
  proto: u8,
  l4_len: usize,
  fill: impl FnOnce(&mut [u8]),
) -> bool {
  let mut frame = [0u8; MAX_PACKET];
  let Some(out) = frame.get_mut(..ETH_LEN + IPV4_LEN + l4_len) else { return false };
  let Some((eth, rest)) = out.split_first_chunk_mut::<ETH_LEN>() else { return false };
  let Some((ip, l4)) = rest.split_first_chunk_mut::<IPV4_LEN>() else { return false };
  let Ok(total_len) = u16::try_from(IPV4_LEN + l4_len) else { return false };

  let host_mac = link.host_mac();
  eth[..MAC_LEN].copy_from_slice(dst_mac);
  eth[MAC_LEN..2 * MAC_LEN].copy_from_slice(&host_mac);
  eth[12..].copy_from_slice(&ETH_IPV4.to_be_bytes());

  let id = NEXT_IP_ID.fetch_add(1, Ordering::Relaxed).wrapping_add(1) as u16;
  ip[0] = 0x45;
  ip[2..4].copy_from_slice(&total_len.to_be_bytes());
  ip[4..6].copy_from_slice(&id.to_be_bytes());
  ip[8] = 64;
  ip[9] = proto;
  ip[12..16].copy_from_slice(&src_ip.to_be_bytes());
  ip[16..20].copy_from_slice(&dst_ip.to_be_bytes());
  let sum = csum(ip, 0);
  ip[10..12].copy_from_slice(&sum.to_be_bytes());

  fill(l4);
  link.enqueue(out)
}

pub(crate) fn send_udp<L: FrameSink>(
  link: &mut L,
  dst_mac: &[u8; MAC_LEN],
  src_ip: u32,
  dst_ip: u32,
  src_port: u16,
  dst_port: u16,
  payload: &[u8],
) -> bool {
  if payload.len() > STANDARD_PACKET {
    return false;
  }
  let udp_len = UDP_LEN + payload.len();
  send_ipv4(link, dst_mac, src_ip, dst_ip, IP_UDP, udp_len, |l4| {
    let Some((udp, body)) = l4.split_first_chunk_mut::<UDP_LEN>() else { return };
    udp[0..2].copy_from_slice(&src_port.to_be_bytes());
    udp[2..4].copy_from_slice(&dst_port.to_be_bytes());
    udp[4..6].copy_from_slice(&(udp_len as u16).to_be_bytes());
    body.copy_from_slice(payload);
  })
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn send_tcp<L: FrameSink>(
  link: &mut L,
  dst_mac: &[u8; MAC_LEN],
  src_ip: u32,
  dst_ip: u32,
  src_port: u16,
  dst_port: u16,
  seq: u32,
  ack: u32,
  flags: u8,
  payload: &[u8],
) -> bool {
  if payload.len() > TCP_MSS {
    return false;
  }
  send_ipv4(link, dst_mac, src_ip, dst_ip, IP_TCP, TCP_LEN + payload.len(), |l4| {
    {
      let Some((tcp, body)) = l4.split_first_chunk_mut::<TCP_LEN>() else { return };
      tcp[0..2].copy_from_slice(&src_port.to_be_bytes());
      tcp[2..4].copy_from_slice(&dst_port.to_be_bytes());
      tcp[4..8].copy_from_slice(&seq.to_be_bytes());
      tcp[8..12].copy_from_slice(&ack.to_be_bytes());
      tcp[12] = 5 << 4;
      tcp[13] = flags;
      tcp[14..16].copy_from_slice(&u16::MAX.to_be_bytes());
      body.copy_from_slice(payload);
    }
    let sum = l4_csum(src_ip, dst_ip, IP_TCP, l4);
    if let Some(field) = l4.get_mut(16..18) {
      field.copy_from_slice(&sum.to_be_bytes());
    }
  })
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn send_rst<L: FrameSink>(
  link: &mut L,
  dst_mac: &[u8; MAC_LEN],
  src_ip: u32,
  dst_ip: u32,
  src_port: u16,
  dst_port: u16,
  seq: u32,
  ack: u32,
) -> bool {
  send_tcp(link, dst_mac, src_ip, dst_ip, src_port, dst_port, seq, ack, TCP_RST | TCP_ACK, &[])
}

fn handle_arp<L: Stack>(link: &mut L, eth: &[u8; ETH_LEN], payload: &[u8]) {
  let Some(req) = payload.first_chunk::<ARP_LEN>() else { return };
  let htype = u16::from_be_bytes([req[0], req[1]]);
  let ptype = u16::from_be_bytes([req[2], req[3]]);
  let op = u16::from_be_bytes([req[6], req[7]]);
  if htype != 1 || ptype != ETH_IPV4 || usize::from(req[4]) != MAC_LEN || req[5] != 4 || op != 1 {
    return;
  }

  let target = u32::from_be_bytes([req[24], req[25], req[26], req[27]]);
  if target != HOST_IP && target != DNS_IP {
    return;
  }
  let guest_mac = eth_src(eth);
  link.set_guest_mac(&guest_mac);

  let host_mac = link.host_mac();
  let mut frame = [0u8; ETH_LEN + ARP_LEN];
  frame[0..6].copy_from_slice(&guest_mac);
  frame[6..12].copy_from_slice(&host_mac);
  frame[12..14].copy_from_slice(&ETH_ARP.to_be_bytes());
  frame[14..16].copy_from_slice(&1u16.to_be_bytes());
  frame[16..18].copy_from_slice(&ETH_IPV4.to_be_bytes());
  frame[18] = MAC_LEN as u8;
  frame[19] = 4;
  frame[20..22].copy_from_slice(&2u16.to_be_bytes());
  frame[22..28].copy_from_slice(&host_mac);
  frame[28..32].copy_from_slice(&target.to_be_bytes());
  frame[32..38].copy_from_slice(&req[8..14]);
  frame[38..42].copy_from_slice(&req[14..18]);
  link.enqueue(&frame);
}

fn dhcp_message_type(options: &[u8]) -> u8 {
  let mut rest = options;
  while let Some((&opt, after_opt)) = rest.split_first() {
    rest = after_opt;
    if opt == 255 {
      break;
    }
    if opt == 0 {
      continue;
    }
    let Some((&len, after_len)) = rest.split_first() else { break };
    let Some((value, after_value)) = after_len.split_at_checked(usize::from(len)) else { break };
    if opt == 53 {
      if let Some(&kind) = value.first() {
        return kind;
      }
    }
    rest = after_value;
  }
  0
}

fn handle_dhcp<L: Stack>(link: &mut L, eth: &[u8; ETH_LEN], client_port: u16, payload: &[u8]) {
  let Some((head, options)) = payload.split_first_chunk::<DHCP_HEADER_LEN>() else { return };
  if head[0] != 1 {
    return;
  }
  let guest_mac = eth_src(eth);
  link.set_guest_mac(&guest_mac);
  if head[236..240] != DHCP_MAGIC {
    return;
  }
  let reply_type = match dhcp_message_type(options) {
    1 => 2,
    3 => 5,
    _ => return,
  };

  let host = HOST_IP.to_be_bytes();
  let mask = MASK_IP.to_be_bytes();
  let dns = DNS_IP.to_be_bytes();
  let lease = 86400u32.to_be_bytes();
  let reply_options: [u8; DHCP_OPTIONS_LEN] = [
    53, 1, reply_type,
    54, 4, host[0], host[1], host[2], host[3],
    1, 4, mask[0], mask[1], mask[2], mask[3],
    3, 4, host[0], host[1], host[2], host[3],
    6, 4, dns[0], dns[1], dns[2], dns[3],
    51, 4, lease[0], lease[1], lease[2], lease[3],
    255,
  ];

  let mut resp = [0u8; DHCP_HEADER_LEN + DHCP_OPTIONS_LEN];
  resp[0] = 2;
  resp[1..8].copy_from_slice(&head[1..8]);
  resp[16..20].copy_from_slice(&GUEST_IP.to_be_bytes());
  resp[20..24].copy_from_slice(&host);
  resp[28..44].copy_from_slice(&head[28..44]);
  resp[236..240].copy_from_slice(&DHCP_MAGIC);
  resp[DHCP_HEADER_LEN..].copy_from_slice(&reply_options);

  let (dst_mac, dst_ip) = if head[10] & 0x80 != 0 {
    ([0xff; MAC_LEN], BROADCAST_IP)
  } else {
    (guest_mac, GUEST_IP)
  };
  send_udp(link, &dst_mac, HOST_IP, dst_ip, DHCP_SERVER_PORT, client_port, &resp);
}

fn handle_icmp<L: Stack>(link: &mut L, eth: &[u8; ETH_LEN], ip: &[u8; IPV4_LEN], payload: &[u8]) {
  if payload.len() < 8 || payload[0] != 8 {
    return;
  }
  let dst = ip_dst(ip);
  if dst != HOST_IP && dst != DNS_IP {
    return;
  }
  if payload.len() > STANDARD_PACKET {
    return;
  }
  send_ipv4(link, &eth_src(eth), dst, ip_src(ip), IP_ICMP, payload.len(), |l4| {
    l4.copy_from_slice(payload);
    l4[0] = 0;
    l4[2] = 0;
    l4[3] = 0;
    let sum = csum(l4, 0);
    l4[2..4].copy_from_slice(&sum.to_be_bytes());
  });
}

fn handle_ipv4<L: Stack>(link: &mut L, eth: &[u8; ETH_LEN], payload: &[u8]) {
  let Some(ip) = payload.first_chunk::<IPV4_LEN>() else { return };
  let ihl = usize::from(ip[0] & 0x0f) * 4;
  let ip_len = usize::from(u16::from_be_bytes([ip[2], ip[3]]));
  if ip[0] >> 4 != 4 || ihl < IPV4_LEN || ip_len > payload.len() || ip_len < ihl {
    return;
  }
  link.set_guest_mac(&eth_src(eth));

  let Some(l4) = payload.get(ihl..ip_len) else { return };
  let mut flow = Flow { guest_mac: eth_src(eth), src_ip: ip_src(ip), dst_ip: ip_dst(ip), src_port: 0, dst_port: 0 };
  match ip[9] {
    IP_UDP => {
      let Some((udp, _)) = l4.split_first_chunk::<UDP_LEN>() else { return };
      let sport = u16::from_be_bytes([udp[0], udp[1]]);
      let dport = u16::from_be_bytes([udp[2], udp[3]]);
      let udp_len = usize::from(u16::from_be_bytes([udp[4], udp[5]]));
      let Some(body) = l4.get(UDP_LEN..udp_len) else { return };
      if sport == DHCP_CLIENT_PORT && dport == DHCP_SERVER_PORT {
        handle_dhcp(link, eth, sport, body);
      } else {
        flow.src_port = sport;
        flow.dst_port = dport;
        link.nat_udp(&flow, body);
      }
    }
    IP_TCP => {
      let Some(tcp) = l4.first_chunk::<TCP_LEN>() else { return };
      let thl = usize::from(tcp[12] >> 4) * 4;
      if thl < TCP_LEN {
        return;
      }
      let Some(body) = l4.get(thl..) else { return };
      flow.src_port = u16::from_be_bytes([tcp[0], tcp[1]]);
      flow.dst_port = u16::from_be_bytes([tcp[2], tcp[3]]);
      let info = TcpInfo {
        seq: u32::from_be_bytes([tcp[4], tcp[5], tcp[6], tcp[7]]),
        ack: u32::from_be_bytes([tcp[8], tcp[9], tcp[10], tcp[11]]),
        flags: tcp[13],
        window: u16::from_be_bytes([tcp[14], tcp[15]]),
      };
      link.nat_tcp(&flow, &info, body);
    }
    IP_ICMP => handle_icmp(link, eth, ip, l4),
    _ => {}
  }
}

pub(crate) fn handle_frame<L: Stack>(link: &mut L, frame: &[u8]) {
  let Some((eth, payload)) = frame.split_first_chunk::<ETH_LEN>() else { return };
  link.note_guest_packet();
  match u16::from_be_bytes([eth[12], eth[13]]) {
    ETH_ARP => handle_arp(link, eth, payload),
    ETH_IPV4 => handle_ipv4(link, eth, payload),
    _ => {}
  }
}

#[cfg(test)]
#[path = "tests/net.rs"]
mod tests;
