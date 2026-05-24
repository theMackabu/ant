#include "sandbox_backend/net_internal.h"


static uint32_t ant_net_id;

uint16_t ant_net_load16(const unsigned char *p) {
  return (uint16_t)(((uint16_t)p[0] << 8u) | p[1]);
}

void ant_net_store16(unsigned char *p, uint16_t value) {
  p[0] = (uint8_t)(value >> 8u);
  p[1] = (uint8_t)value;
}

void ant_net_store32(unsigned char *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24u);
  p[1] = (uint8_t)(value >> 16u);
  p[2] = (uint8_t)(value >> 8u);
  p[3] = (uint8_t)value;
}

uint16_t ant_net_csum(const void *data, size_t len, uint32_t sum) {
  const uint8_t *p = data;
  while (len > 1) {
    sum += ((uint16_t)p[0] << 8u) | p[1];
    p += 2;
    len -= 2;
  }
  if (len) sum += (uint16_t)p[0] << 8u;
  while (sum >> 16u) sum = (sum & 0xffffu) + (sum >> 16u);
  return (uint16_t)~sum;
}

uint16_t ant_net_l4_csum(uint32_t src, uint32_t dst, uint8_t proto, const void *data, size_t len) {
  uint32_t sum = 0;
  sum += (src >> 16u) & 0xffffu;
  sum += src & 0xffffu;
  sum += (dst >> 16u) & 0xffffu;
  sum += dst & 0xffffu;
  sum += proto;
  sum += (uint32_t)len;
  return ant_net_csum(data, len, sum);
}

void ant_net_set_guest(ant_hvf_vm_t *vm, const uint8_t mac[ANT_NET_MAC_BYTES]) {
  memcpy(vm->net_guest_mac, mac, ANT_NET_MAC_BYTES);
  vm->net_guest_mac_seen = true;
}

static size_t ant_net_emit_ipv4(ant_hvf_vm_t *vm,
                                unsigned char *out,
                                const uint8_t dst_mac[ANT_NET_MAC_BYTES],
                                uint32_t src_ip,
                                uint32_t dst_ip,
                                uint8_t proto,
                                const void *payload,
                                uint16_t payload_len) {
  ant_eth_t *eth = (ant_eth_t *)out;
  memcpy(eth->dst, dst_mac, ANT_NET_MAC_BYTES);
  memcpy(eth->src, vm->net_mac, ANT_NET_MAC_BYTES);
  eth->ethertype = htons(ANT_ETH_IPV4);

  ant_ipv4_t *ip = (ant_ipv4_t *)(out + sizeof(*eth));
  memset(ip, 0, sizeof(*ip));
  ip->vhl = 0x45;
  ip->len = htons((uint16_t)(sizeof(*ip) + payload_len));
  ip->id = htons((uint16_t)__atomic_add_fetch(&ant_net_id, 1, __ATOMIC_RELAXED));
  ip->ttl = 64;
  ip->proto = proto;
  ip->src = htonl(src_ip);
  ip->dst = htonl(dst_ip);
  ip->sum = htons(ant_net_csum(ip, sizeof(*ip), 0));
  memcpy(out + sizeof(*eth) + sizeof(*ip), payload, payload_len);
  return sizeof(*eth) + sizeof(*ip) + payload_len;
}

void ant_net_send_udp(ant_hvf_vm_t *vm,
                      const uint8_t dst_mac[ANT_NET_MAC_BYTES],
                      uint32_t src_ip,
                      uint32_t dst_ip,
                      uint16_t src_port,
                      uint16_t dst_port,
                      const void *payload,
                      uint16_t payload_len) {
  unsigned char frame[ANT_HVF_NET_MAX_PACKET];
  unsigned char udp_buf[sizeof(ant_udp_t) + ANT_NET_STANDARD_PACKET_BYTES];
  if (payload_len > sizeof(udp_buf) - sizeof(ant_udp_t)) return;
  ant_udp_t *udp = (ant_udp_t *)udp_buf;
  udp->src = htons(src_port);
  udp->dst = htons(dst_port);
  udp->len = htons((uint16_t)(sizeof(*udp) + payload_len));
  udp->sum = 0;
  memcpy(udp_buf + sizeof(*udp), payload, payload_len);
  size_t len = ant_net_emit_ipv4(vm, frame, dst_mac, src_ip, dst_ip, ANT_IP_UDP,
                                 udp_buf, (uint16_t)(sizeof(*udp) + payload_len));
  ant_net_enqueue(vm, frame, (uint32_t)len);
}

void ant_net_send_tcp(ant_hvf_vm_t *vm,
                             const uint8_t dst_mac[ANT_NET_MAC_BYTES],
                             uint32_t src_ip,
                             uint32_t dst_ip,
                             uint16_t src_port,
                             uint16_t dst_port,
                             uint32_t seq,
                             uint32_t ack,
                             uint8_t flags,
                             const void *payload,
                             uint16_t payload_len) {
  unsigned char frame[ANT_HVF_NET_MAX_PACKET];
  unsigned char tcp_buf[sizeof(ant_tcp_t) + ANT_TCP_MSS];
  if (payload_len > ANT_TCP_MSS) return;
  ant_tcp_t *tcp = (ant_tcp_t *)tcp_buf;
  memset(tcp, 0, sizeof(*tcp));
  tcp->src = htons(src_port);
  tcp->dst = htons(dst_port);
  tcp->seq = htonl(seq);
  tcp->ack = htonl(ack);
  tcp->off = (uint8_t)(5u << 4u);
  tcp->flags = flags;
  tcp->win = htons(65535);
  memcpy(tcp_buf + sizeof(*tcp), payload, payload_len);
  tcp->sum = htons(ant_net_l4_csum(src_ip, dst_ip, ANT_IP_TCP, tcp_buf, sizeof(*tcp) + payload_len));
  size_t len = ant_net_emit_ipv4(vm, frame, dst_mac, src_ip, dst_ip, ANT_IP_TCP,
                                 tcp_buf, (uint16_t)(sizeof(*tcp) + payload_len));
  ant_net_enqueue(vm, frame, (uint32_t)len);
}

void ant_net_send_rst(ant_hvf_vm_t *vm,
                             const uint8_t dst_mac[ANT_NET_MAC_BYTES],
                             uint32_t src_ip,
                             uint32_t dst_ip,
                             uint16_t src_port,
                             uint16_t dst_port,
                             uint32_t seq,
                             uint32_t ack) {
  ant_net_send_tcp(vm, dst_mac, src_ip, dst_ip, src_port, dst_port, seq, ack, ANT_TCP_RST | ANT_TCP_ACK, NULL, 0);
}

static void ant_net_handle_arp(ant_hvf_vm_t *vm, const ant_eth_t *eth, const unsigned char *payload, size_t len) {
  if (len < sizeof(ant_arp_t)) return;
  const ant_arp_t *req = (const ant_arp_t *)payload;
  if (ntohs(req->htype) != 1 || ntohs(req->ptype) != ANT_ETH_IPV4 ||
      req->hlen != ANT_NET_MAC_BYTES || req->plen != 4 || ntohs(req->op) != 1) {
    return;
  }

  uint32_t target = ntohl(req->tpa);
  if (target != ANT_NET_HOST_IP && target != ANT_NET_DNS_IP) return;
  ant_net_set_guest(vm, eth->src);

  unsigned char frame[sizeof(ant_eth_t) + sizeof(ant_arp_t)];
  ant_eth_t *resp_eth = (ant_eth_t *)frame;
  memcpy(resp_eth->dst, eth->src, ANT_NET_MAC_BYTES);
  memcpy(resp_eth->src, vm->net_mac, ANT_NET_MAC_BYTES);
  resp_eth->ethertype = htons(ANT_ETH_ARP);

  ant_arp_t *resp = (ant_arp_t *)(frame + sizeof(*resp_eth));
  memset(resp, 0, sizeof(*resp));
  resp->htype = htons(1);
  resp->ptype = htons(ANT_ETH_IPV4);
  resp->hlen = ANT_NET_MAC_BYTES;
  resp->plen = 4;
  resp->op = htons(2);
  memcpy(resp->sha, vm->net_mac, ANT_NET_MAC_BYTES);
  resp->spa = htonl(target);
  memcpy(resp->tha, req->sha, ANT_NET_MAC_BYTES);
  resp->tpa = req->spa;
  ant_net_enqueue(vm, frame, sizeof(frame));
}

static uint8_t ant_dhcp_type(const unsigned char *opts, size_t len) {
  for (size_t i = 0; i < len;) {
    uint8_t opt = opts[i++];
    if (opt == 255) break;
    if (opt == 0) continue;
    if (i >= len) break;
    uint8_t olen = opts[i++];
    if (i + olen > len) break;
    if (opt == 53 && olen > 0) return opts[i];
    i += olen;
  }
  return 0;
}

static unsigned char *ant_dhcp_opt(unsigned char *p, uint8_t opt, uint8_t len, const void *data) {
  *p++ = opt;
  *p++ = len;
  memcpy(p, data, len);
  return p + len;
}

static void ant_net_handle_dhcp(ant_hvf_vm_t *vm,
                                const ant_eth_t *eth,
                                const ant_udp_t *udp,
                                const unsigned char *payload,
                                size_t len) {
  if (len < 240 || payload[0] != 1) return;
  ant_net_set_guest(vm, eth->src);
  if (memcmp(payload + 236, "\x63\x82\x53\x63", 4) != 0) return;
  uint8_t msg_type = ant_dhcp_type(payload + 240, len - 240);
  if (msg_type != 1 && msg_type != 3) return;

  unsigned char resp[ANT_DHCP_PACKET_BYTES];
  memset(resp, 0, sizeof(resp));
  resp[0] = 2;
  resp[1] = payload[1];
  resp[2] = payload[2];
  resp[3] = payload[3];
  memcpy(resp + 4, payload + 4, 4);
  memcpy(resp + 16, &(uint32_t){ htonl(ANT_NET_GUEST_IP) }, 4);
  memcpy(resp + 20, &(uint32_t){ htonl(ANT_NET_HOST_IP) }, 4);
  memcpy(resp + 28, payload + 28, 16);
  memcpy(resp + 236, "\x63\x82\x53\x63", 4);

  unsigned char *opt = resp + 240;
  uint8_t type = msg_type == 1 ? 2 : 5;
  opt = ant_dhcp_opt(opt, 53, 1, &type);
  uint32_t server = htonl(ANT_NET_HOST_IP);
  uint32_t mask = htonl(ANT_NET_MASK_IP);
  uint32_t router = htonl(ANT_NET_HOST_IP);
  uint32_t dns = htonl(ANT_NET_DNS_IP);
  uint32_t lease = htonl(86400);
  opt = ant_dhcp_opt(opt, 54, 4, &server);
  opt = ant_dhcp_opt(opt, 1, 4, &mask);
  opt = ant_dhcp_opt(opt, 3, 4, &router);
  opt = ant_dhcp_opt(opt, 6, 4, &dns);
  opt = ant_dhcp_opt(opt, 51, 4, &lease);
  *opt++ = 255;

  uint8_t dst_mac[ANT_NET_MAC_BYTES];
  memset(dst_mac, 0xff, sizeof(dst_mac));
  uint32_t dst_ip = ANT_NET_BROADCAST_IP;
  if (!(payload[10] & 0x80u)) {
    memcpy(dst_mac, eth->src, ANT_NET_MAC_BYTES);
    dst_ip = ANT_NET_GUEST_IP;
  }
  ant_net_send_udp(vm, dst_mac, ANT_NET_HOST_IP, dst_ip,
                   ANT_DHCP_SERVER_PORT, ntohs(udp->src),
                   resp, (uint16_t)(opt - resp));
}

static size_t ant_dns_skip_name(const unsigned char *packet, size_t len, size_t off) {
  while (off < len) {
    uint8_t c = packet[off++];
    if (c == 0) return off;
    if ((c & 0xc0u) == 0xc0u) return off + 1 <= len ? off + 1 : len;
    off += c;
  }
  return len;
}

static bool ant_dns_read_name(const unsigned char *packet, size_t len, size_t off, char *out, size_t out_len) {
  size_t pos = 0;
  while (off < len) {
    uint8_t n = packet[off++];
    if (n == 0) {
      if (pos == 0) {
        if (out_len > 1) strcpy(out, ".");
      } else {
        out[pos - 1] = '\0';
      }
      return true;
    }
    if ((n & 0xc0u) || n > 63 || off + n > len || pos + n + 1 >= out_len) return false;
    memcpy(out + pos, packet + off, n);
    pos += n;
    out[pos++] = '.';
    off += n;
  }
  return false;
}

static bool ant_dns_lookup_a(const char *name, uint32_t *out) {
  struct in_addr addr;
  if (inet_pton(AF_INET, name, &addr) != 1) return false;
  *out = ntohl(addr.s_addr);
  return true;
}

static void ant_net_handle_dns(ant_hvf_vm_t *vm,
                               const ant_eth_t *eth,
                               const ant_ipv4_t *ip,
                               const ant_udp_t *udp,
                               const unsigned char *query,
                               size_t query_len) {
  if (query_len < 12) return;
  uint16_t qd = ant_net_load16(query + 4);
  if (qd == 0) return;
  size_t qend = ant_dns_skip_name(query, query_len, 12);
  if (qend + 4 > query_len) return;
  uint16_t qtype = ant_net_load16(query + qend);

  char name[ANT_DNS_NAME_BYTES];
  if (!ant_dns_read_name(query, query_len, 12, name, sizeof(name))) return;

  uint32_t answer_ip = 0;
  bool answered = qtype == 1 && ant_dns_lookup_a(name, &answer_ip);

  unsigned char resp[ANT_NET_STANDARD_PACKET_BYTES];
  if (query_len > sizeof(resp) - 32) return;
  memcpy(resp, query, query_len);
  resp[2] = 0x81;
  resp[3] = 0x80;
  resp[6] = 0;
  resp[7] = answered ? 1 : 0;
  resp[8] = resp[9] = resp[10] = resp[11] = 0;
  size_t resp_len = query_len;
  if (answered) {
    resp[resp_len++] = 0xc0;
    resp[resp_len++] = 0x0c;
    ant_net_store16(resp + resp_len, 1); resp_len += 2;
    ant_net_store16(resp + resp_len, 1); resp_len += 2;
    ant_net_store32(resp + resp_len, 60); resp_len += 4;
    ant_net_store16(resp + resp_len, 4); resp_len += 2;
    ant_net_store32(resp + resp_len, answer_ip); resp_len += 4;
  }

  ant_net_send_udp(vm, eth->src, ntohl(ip->dst), ntohl(ip->src), ANT_DNS_PORT, ntohs(udp->src),
                   resp, (uint16_t)resp_len);
}

static void ant_net_handle_icmp(ant_hvf_vm_t *vm,
                                const ant_eth_t *eth,
                                const ant_ipv4_t *ip,
                                const unsigned char *payload,
                                size_t payload_len) {
  if (payload_len < 8 || payload[0] != 8) return;
  uint32_t dst = ntohl(ip->dst);
  if (dst != ANT_NET_HOST_IP && dst != ANT_NET_DNS_IP) return;
  unsigned char reply[ANT_NET_STANDARD_PACKET_BYTES];
  if (payload_len > sizeof(reply)) return;
  memcpy(reply, payload, payload_len);
  reply[0] = 0;
  reply[2] = reply[3] = 0;
  ant_net_store16(reply + 2, ant_net_csum(reply, payload_len, 0));
  size_t len;
  unsigned char frame[ANT_HVF_NET_MAX_PACKET];
  len = ant_net_emit_ipv4(vm, frame, eth->src, dst, ntohl(ip->src), ANT_IP_ICMP, reply, (uint16_t)payload_len);
  ant_net_enqueue(vm, frame, (uint32_t)len);
}

static void ant_net_handle_ipv4(ant_hvf_vm_t *vm, const ant_eth_t *eth, const unsigned char *payload, size_t len) {
  if (len < sizeof(ant_ipv4_t)) return;
  const ant_ipv4_t *ip = (const ant_ipv4_t *)payload;
  size_t ihl = (size_t)(ip->vhl & 0x0fu) * 4u;
  size_t ip_len = ntohs(ip->len);
  if ((ip->vhl >> 4u) != 4 || ihl < sizeof(*ip) || ip_len > len || ip_len < ihl) return;
  ant_net_set_guest(vm, eth->src);

  const unsigned char *l4 = payload + ihl;
  size_t l4_len = ip_len - ihl;
  if (ip->proto == ANT_IP_UDP) {
    if (l4_len < sizeof(ant_udp_t)) return;
    const ant_udp_t *udp = (const ant_udp_t *)l4;
    uint16_t sport = ntohs(udp->src);
    uint16_t dport = ntohs(udp->dst);
    size_t udp_len = ntohs(udp->len);
    if (udp_len < sizeof(*udp) || udp_len > l4_len) return;
    const unsigned char *udp_payload = l4 + sizeof(*udp);
    size_t udp_payload_len = udp_len - sizeof(*udp);
    if (sport == ANT_DHCP_CLIENT_PORT && dport == ANT_DHCP_SERVER_PORT) {
      ant_net_handle_dhcp(vm, eth, udp, udp_payload, udp_payload_len);
    } else if (dport == ANT_DNS_PORT && vm->net_nat) {
      ant_nat_handle_udp(vm->net_nat, eth, ip, udp, udp_payload, udp_payload_len);
    } else if (dport == ANT_DNS_PORT) {
      ant_net_handle_dns(vm, eth, ip, udp, udp_payload, udp_payload_len);
    } else if (vm->net_nat) {
      ant_nat_handle_udp(vm->net_nat, eth, ip, udp, udp_payload, udp_payload_len);
    }
  } else if (ip->proto == ANT_IP_TCP) {
    if (l4_len < sizeof(ant_tcp_t)) return;
    const ant_tcp_t *tcp = (const ant_tcp_t *)l4;
    size_t thl = (size_t)(tcp->off >> 4u) * 4u;
    if (thl < sizeof(*tcp) || thl > l4_len) return;
    if (vm->net_nat) ant_nat_handle_tcp(vm->net_nat, eth, ip, tcp, l4 + thl, l4_len - thl);
  } else if (ip->proto == ANT_IP_ICMP) {
    ant_net_handle_icmp(vm, eth, ip, l4, l4_len);
  }
}


void ant_net_handle_frame(ant_hvf_vm_t *vm, const unsigned char *frame, size_t frame_len) {
  if (frame_len < sizeof(ant_eth_t)) return;
  if (vm->net_nat) ant_nat_note_guest_packet(vm->net_nat);
  const ant_eth_t *eth = (const ant_eth_t *)frame;
  uint16_t ethertype = ntohs(eth->ethertype);
  if (ethertype == ANT_ETH_ARP) {
    ant_net_handle_arp(vm, eth, frame + sizeof(*eth), frame_len - sizeof(*eth));
  } else if (ethertype == ANT_ETH_IPV4) {
    ant_net_handle_ipv4(vm, eth, frame + sizeof(*eth), frame_len - sizeof(*eth));
  }
}
