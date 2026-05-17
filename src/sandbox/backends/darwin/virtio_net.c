#include "backend.h"

#if defined(__aarch64__)

#define ANT_NET_IP(a, b, c, d) \
  ((((uint32_t)(a)) << 24u) | (((uint32_t)(b)) << 16u) | (((uint32_t)(c)) << 8u) | ((uint32_t)(d)))

#define ANT_NET_HOST_IP ANT_NET_IP(10, 0, 2, 2)
#define ANT_NET_DNS_IP ANT_NET_IP(10, 0, 2, 3)
#define ANT_NET_GUEST_IP ANT_NET_IP(10, 0, 2, 15)
#define ANT_NET_MASK_IP ANT_NET_IP(255, 255, 255, 0)
#define ANT_NET_BROADCAST_IP ANT_NET_IP(255, 255, 255, 255)

#define ANT_ETH_IPV4 0x0800u
#define ANT_ETH_ARP 0x0806u
#define ANT_IP_ICMP 1u
#define ANT_IP_TCP 6u
#define ANT_IP_UDP 17u
#define ANT_TCP_FIN 0x01u
#define ANT_TCP_SYN 0x02u
#define ANT_TCP_RST 0x04u
#define ANT_TCP_PSH 0x08u
#define ANT_TCP_ACK 0x10u
#define ANT_TCP_MSS 1380u
#define ANT_TCP_GUEST_WINDOW 32768u
#define ANT_TCP_HOST_BUFFER 65536u
#define ANT_DHCP_SERVER_PORT 67u
#define ANT_DHCP_CLIENT_PORT 68u
#define ANT_DNS_PORT 53u

typedef struct ant_hvf_nat_tcp ant_hvf_nat_tcp_t;

typedef struct {
  int fd;
  uint16_t host_port;
  uint16_t guest_port;
} ant_hvf_nat_forward_t;

struct ant_hvf_nat_tcp {
  bool used;
  bool inbound;
  bool connecting_host;
  bool connecting_guest;
  int fd;
  uint32_t guest_ip;
  uint16_t guest_port;
  uint32_t peer_ip;
  uint16_t peer_port;
  uint32_t guest_next;
  uint32_t nat_next;
  uint32_t nat_iss;
  uint32_t guest_ack;
  uint32_t guest_window;
  bool guest_fin_seen;
  bool host_eof;
  bool host_fin_sent;
  uint8_t guest_mac[6];
  unsigned char pending[65536];
  size_t pending_off;
  size_t pending_len;
  unsigned char host_buf[ANT_TCP_HOST_BUFFER];
  uint32_t host_base;
  size_t host_len;
  size_t host_sent;
};

struct ant_hvf_nat {
  ant_hvf_vm_t *vm;
  pthread_mutex_t lock;
  bool lock_init;
  pthread_t thread;
  bool thread_started;
  bool stop;
  int wake_pipe[2];
  ant_hvf_nat_tcp_t tcp[ANT_HVF_NET_TCP_MAX];
  ant_hvf_nat_forward_t forwards[32];
  size_t forward_count;
};

typedef struct __attribute__((packed)) {
  uint8_t dst[6];
  uint8_t src[6];
  uint16_t ethertype;
} ant_eth_t;

typedef struct __attribute__((packed)) {
  uint16_t htype;
  uint16_t ptype;
  uint8_t hlen;
  uint8_t plen;
  uint16_t op;
  uint8_t sha[6];
  uint32_t spa;
  uint8_t tha[6];
  uint32_t tpa;
} ant_arp_t;

typedef struct __attribute__((packed)) {
  uint8_t vhl;
  uint8_t tos;
  uint16_t len;
  uint16_t id;
  uint16_t off;
  uint8_t ttl;
  uint8_t proto;
  uint16_t sum;
  uint32_t src;
  uint32_t dst;
} ant_ipv4_t;

typedef struct __attribute__((packed)) {
  uint16_t src;
  uint16_t dst;
  uint16_t len;
  uint16_t sum;
} ant_udp_t;

typedef struct __attribute__((packed)) {
  uint16_t src;
  uint16_t dst;
  uint32_t seq;
  uint32_t ack;
  uint8_t off;
  uint8_t flags;
  uint16_t win;
  uint16_t sum;
  uint16_t urg;
} ant_tcp_t;

static uint32_t ant_net_id;

static uint16_t ant_net_load16(const unsigned char *p) {
  return (uint16_t)(((uint16_t)p[0] << 8u) | p[1]);
}

static void ant_net_store16(unsigned char *p, uint16_t value) {
  p[0] = (uint8_t)(value >> 8u);
  p[1] = (uint8_t)value;
}

static void ant_net_store32(unsigned char *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24u);
  p[1] = (uint8_t)(value >> 16u);
  p[2] = (uint8_t)(value >> 8u);
  p[3] = (uint8_t)value;
}

static uint16_t ant_net_csum(const void *data, size_t len, uint32_t sum) {
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

static uint16_t ant_net_l4_csum(uint32_t src, uint32_t dst, uint8_t proto, const void *data, size_t len) {
  uint32_t sum = 0;
  sum += (src >> 16u) & 0xffffu;
  sum += src & 0xffffu;
  sum += (dst >> 16u) & 0xffffu;
  sum += dst & 0xffffu;
  sum += proto;
  sum += (uint32_t)len;
  return ant_net_csum(data, len, sum);
}

static void ant_net_wake(ant_hvf_nat_t *nat) {
  if (!nat || nat->wake_pipe[1] < 0) return;
  unsigned char byte = 0;
  ssize_t ignored = write(nat->wake_pipe[1], &byte, sizeof(byte));
  (void)ignored;
}

static void ant_net_drain_wake(ant_hvf_nat_t *nat) {
  unsigned char buf[64];
  while (nat->wake_pipe[0] >= 0) {
    ssize_t n = read(nat->wake_pipe[0], buf, sizeof(buf));
    if (n > 0) continue;
    if (n < 0 && errno == EINTR) continue;
    break;
  }
}

void ant_hvf_net_note_rx(ant_hvf_vm_t *vm) {
  if (!vm->net_lock_init) return;
  pthread_mutex_lock(&vm->net_lock);
  vm->net_rx_wake = true;
  pthread_mutex_unlock(&vm->net_lock);
  if (vm->vcpu) hv_vcpus_exit(&vm->vcpu, 1);
}

static void ant_net_enqueue(ant_hvf_vm_t *vm, const void *data, uint32_t len) {
  if (len == 0 || len > ANT_HVF_NET_MAX_PACKET) return;
  ant_hvf_net_packet_t packet;
  packet.len = len;
  memcpy(packet.data, data, len);

  bool queued = false;
  pthread_mutex_lock(&vm->net_lock);
  if (vm->net_rx_count < ANT_HVF_NET_RX_BACKLOG) {
    uint32_t slot = (vm->net_rx_head + vm->net_rx_count) % ANT_HVF_NET_RX_BACKLOG;
    vm->net_rx_packets[slot] = packet;
    vm->net_rx_count++;
    queued = true;
  }
  pthread_mutex_unlock(&vm->net_lock);
  if (vm->trace) fprintf(stderr, "sandbox vm: net enqueue len=%u queued=%d backlog=%u\n",
                         len, queued, vm->net_rx_count);
  if (queued) {
    int rc = ant_hvf_virtio_net_drain_rx(vm);
    if (vm->trace && rc != 0) fprintf(stderr, "sandbox vm: net immediate rx drain failed rc=%d\n", rc);
    ant_hvf_net_note_rx(vm);
  }
}

static void ant_net_set_guest(ant_hvf_vm_t *vm, const uint8_t mac[6]) {
  memcpy(vm->net_guest_mac, mac, 6);
  vm->net_guest_mac_seen = true;
}

static size_t ant_net_emit_ipv4(ant_hvf_vm_t *vm,
                                unsigned char *out,
                                const uint8_t dst_mac[6],
                                uint32_t src_ip,
                                uint32_t dst_ip,
                                uint8_t proto,
                                const void *payload,
                                uint16_t payload_len) {
  ant_eth_t *eth = (ant_eth_t *)out;
  memcpy(eth->dst, dst_mac, 6);
  memcpy(eth->src, vm->net_mac, 6);
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

static void ant_net_send_udp(ant_hvf_vm_t *vm,
                             const uint8_t dst_mac[6],
                             uint32_t src_ip,
                             uint32_t dst_ip,
                             uint16_t src_port,
                             uint16_t dst_port,
                             const void *payload,
                             uint16_t payload_len) {
  unsigned char frame[ANT_HVF_NET_MAX_PACKET];
  unsigned char udp_buf[sizeof(ant_udp_t) + 1500];
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

static void ant_net_send_tcp(ant_hvf_vm_t *vm,
                             const uint8_t dst_mac[6],
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

static void ant_net_send_rst(ant_hvf_vm_t *vm,
                             const uint8_t dst_mac[6],
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
      req->hlen != 6 || req->plen != 4 || ntohs(req->op) != 1) {
    return;
  }

  uint32_t target = ntohl(req->tpa);
  if (target != ANT_NET_HOST_IP && target != ANT_NET_DNS_IP) return;
  ant_net_set_guest(vm, eth->src);
  if (vm->trace) fprintf(stderr, "sandbox vm: net arp reply target=%u.%u.%u.%u\n",
                         (target >> 24u) & 0xffu, (target >> 16u) & 0xffu,
                         (target >> 8u) & 0xffu, target & 0xffu);

  unsigned char frame[sizeof(ant_eth_t) + sizeof(ant_arp_t)];
  ant_eth_t *resp_eth = (ant_eth_t *)frame;
  memcpy(resp_eth->dst, eth->src, 6);
  memcpy(resp_eth->src, vm->net_mac, 6);
  resp_eth->ethertype = htons(ANT_ETH_ARP);

  ant_arp_t *resp = (ant_arp_t *)(frame + sizeof(*resp_eth));
  memset(resp, 0, sizeof(*resp));
  resp->htype = htons(1);
  resp->ptype = htons(ANT_ETH_IPV4);
  resp->hlen = 6;
  resp->plen = 4;
  resp->op = htons(2);
  memcpy(resp->sha, vm->net_mac, 6);
  resp->spa = htonl(target);
  memcpy(resp->tha, req->sha, 6);
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
  if (vm->trace) fprintf(stderr, "sandbox vm: net dhcp %s xid=%02x%02x%02x%02x\n",
                         msg_type == 1 ? "discover" : "request",
                         payload[4], payload[5], payload[6], payload[7]);

  unsigned char resp[548];
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

  uint8_t dst_mac[6];
  memset(dst_mac, 0xff, sizeof(dst_mac));
  uint32_t dst_ip = ANT_NET_BROADCAST_IP;
  if (!(payload[10] & 0x80u)) {
    memcpy(dst_mac, eth->src, 6);
    dst_ip = ANT_NET_GUEST_IP;
  }
  ant_net_send_udp(vm, dst_mac, ANT_NET_HOST_IP, dst_ip,
                   ANT_DHCP_SERVER_PORT, ntohs(udp->src),
                   resp, (uint16_t)(opt - resp));
  if (vm->trace) fprintf(stderr, "sandbox vm: net dhcp reply type=%u len=%zu\n",
                         type, (size_t)(opt - resp));
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
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *res = NULL;
  if (getaddrinfo(name, NULL, &hints, &res) != 0 || !res) return false;
  struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
  *out = ntohl(sin->sin_addr.s_addr);
  freeaddrinfo(res);
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

  char name[256];
  if (!ant_dns_read_name(query, query_len, 12, name, sizeof(name))) return;

  uint32_t answer_ip = 0;
  bool answered = qtype == 1 && ant_dns_lookup_a(name, &answer_ip);

  unsigned char resp[1500];
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

static int ant_socket_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -errno;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -errno;
}

static ant_hvf_nat_tcp_t *ant_nat_find_tcp(ant_hvf_nat_t *nat,
                                           uint32_t guest_ip,
                                           uint16_t guest_port,
                                           uint32_t peer_ip,
                                           uint16_t peer_port) {
  for (size_t i = 0; i < ANT_HVF_NET_TCP_MAX; i++) {
    ant_hvf_nat_tcp_t *c = &nat->tcp[i];
    if (!c->used) continue;
    if (c->guest_ip == guest_ip && c->guest_port == guest_port &&
        c->peer_ip == peer_ip && c->peer_port == peer_port) {
      return c;
    }
  }
  return NULL;
}

static ant_hvf_nat_tcp_t *ant_nat_alloc_tcp(ant_hvf_nat_t *nat) {
  for (size_t i = 0; i < ANT_HVF_NET_TCP_MAX; i++) {
    if (!nat->tcp[i].used) {
      ant_hvf_nat_tcp_t *c = &nat->tcp[i];
      memset(c, 0, sizeof(*c));
      c->used = true;
      c->fd = -1;
      return c;
    }
  }
  return NULL;
}

static void ant_nat_close_tcp(ant_hvf_nat_tcp_t *c) {
  if (c->fd >= 0) close(c->fd);
  memset(c, 0, sizeof(*c));
  c->fd = -1;
}

static void ant_nat_close_fd(ant_hvf_nat_tcp_t *c) {
  if (c->fd >= 0) close(c->fd);
  c->fd = -1;
  c->connecting_host = false;
  c->pending_off = 0;
  c->pending_len = 0;
}

static uint32_t ant_nat_host_sent_seq(const ant_hvf_nat_tcp_t *c) {
  return c->host_base + (uint32_t)c->host_sent;
}

static void ant_nat_drop_guest_acked(ant_hvf_nat_tcp_t *c) {
  if (c->host_len == 0) {
    if ((int32_t)(c->guest_ack - c->host_base) > 0) c->host_base = c->guest_ack;
    c->host_sent = 0;
    return;
  }

  if ((int32_t)(c->guest_ack - c->host_base) <= 0) return;

  uint32_t acked32 = c->guest_ack - c->host_base;
  size_t acked = acked32 > c->host_len ? c->host_len : (size_t)acked32;
  if (acked > 0) {
    if (acked < c->host_len) memmove(c->host_buf, c->host_buf + acked, c->host_len - acked);
    c->host_len -= acked;
    c->host_base += (uint32_t)acked;
    c->host_sent = c->host_sent > acked ? c->host_sent - acked : 0;
  }

  if (c->host_len == 0) {
    c->host_base = c->guest_ack;
    c->host_sent = 0;
  }
}

static void ant_nat_send_host_buffer(ant_hvf_nat_t *nat, ant_hvf_nat_tcp_t *c) {
  ant_hvf_vm_t *vm = nat->vm;
  ant_nat_drop_guest_acked(c);

  while (c->host_sent < c->host_len) {
    uint32_t sent_seq = ant_nat_host_sent_seq(c);
    uint32_t in_flight = sent_seq - c->guest_ack;
    uint32_t guest_window = c->guest_window ? c->guest_window : ANT_TCP_GUEST_WINDOW;
    if (in_flight >= guest_window) break;

    size_t chunk = c->host_len - c->host_sent;
    if (chunk > ANT_TCP_MSS) chunk = ANT_TCP_MSS;
    uint32_t allowed = guest_window - in_flight;
    if (chunk > allowed) chunk = allowed;
    if (chunk == 0) break;

    ant_net_send_tcp(vm, c->guest_mac, c->peer_ip, c->guest_ip, c->peer_port, c->guest_port,
                     sent_seq, c->guest_next, ANT_TCP_ACK | ANT_TCP_PSH,
                     c->host_buf + c->host_sent, (uint16_t)chunk);
    c->host_sent += chunk;
    c->nat_next = sent_seq + (uint32_t)chunk;
  }

  if (c->host_eof && !c->host_fin_sent && c->host_sent == c->host_len) {
    uint32_t fin_seq = c->host_base + (uint32_t)c->host_len;
    ant_net_send_tcp(vm, c->guest_mac, c->peer_ip, c->guest_ip, c->peer_port, c->guest_port,
                     fin_seq, c->guest_next, ANT_TCP_ACK | ANT_TCP_FIN, NULL, 0);
    c->host_fin_sent = true;
    c->nat_next = fin_seq + 1u;
  }
}

static void ant_nat_flush_pending(ant_hvf_nat_tcp_t *c) {
  if (c->fd < 0) return;
  while (c->pending_len > 0 && !c->connecting_host) {
    ssize_t n = write(c->fd, c->pending + c->pending_off, c->pending_len);
    if (n > 0) {
      /* guest -> host */
      c->pending_off += (size_t)n;
      c->pending_len -= (size_t)n;
      if (c->pending_len == 0) c->pending_off = 0;
      continue;
    }
    if (n < 0 && (errno == EINTR)) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    ant_nat_close_tcp(c);
    break;
  }
}

static void ant_nat_tcp_from_host(ant_hvf_nat_t *nat, ant_hvf_nat_tcp_t *c) {
  ant_hvf_vm_t *vm = nat->vm;
  for (;;) {
    if (c->host_len >= sizeof(c->host_buf)) break;
    ssize_t n = read(c->fd, c->host_buf + c->host_len, sizeof(c->host_buf) - c->host_len);
    if (n > 0) {
      if (vm->trace) fprintf(stderr, "sandbox vm: nat host read %zd bytes peer=%u.%u.%u.%u:%u guest_port=%u\n",
                             n,
                             (c->peer_ip >> 24u) & 0xffu, (c->peer_ip >> 16u) & 0xffu,
                             (c->peer_ip >> 8u) & 0xffu, c->peer_ip & 0xffu,
                             c->peer_port, c->guest_port);
      if (c->host_len == 0 && c->host_sent == 0) c->host_base = c->nat_next;
      c->host_len += (size_t)n;
      ant_nat_send_host_buffer(nat, c);
      continue;
    }
    if (n == 0) {
      if (vm->trace) fprintf(stderr, "sandbox vm: nat host eof peer=%u.%u.%u.%u:%u guest_port=%u\n",
                             (c->peer_ip >> 24u) & 0xffu, (c->peer_ip >> 16u) & 0xffu,
                             (c->peer_ip >> 8u) & 0xffu, c->peer_ip & 0xffu,
                             c->peer_port, c->guest_port);
      c->host_eof = true;
      ant_nat_close_fd(c);
      ant_nat_send_host_buffer(nat, c);
      break;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
    if (vm->trace) fprintf(stderr, "sandbox vm: nat host read error %s peer=%u.%u.%u.%u:%u guest_port=%u\n",
                           strerror(errno),
                           (c->peer_ip >> 24u) & 0xffu, (c->peer_ip >> 16u) & 0xffu,
                           (c->peer_ip >> 8u) & 0xffu, c->peer_ip & 0xffu,
                           c->peer_port, c->guest_port);
    ant_nat_close_tcp(c);
    break;
  }
}

static void ant_nat_check_connect(ant_hvf_nat_tcp_t *c) {
  if (!c->connecting_host) return;
  int err = 0;
  socklen_t len = sizeof(err);
  if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
    ant_nat_close_tcp(c);
    return;
  }
  c->connecting_host = false;
  ant_nat_flush_pending(c);
}

static void *ant_nat_thread(void *arg) {
  ant_hvf_nat_t *nat = arg;
  while (!nat->stop) {
    struct pollfd fds[1 + ANT_HVF_NET_TCP_MAX + 32];
    ant_hvf_nat_tcp_t *tcp_map[ANT_HVF_NET_TCP_MAX];
    ant_hvf_nat_forward_t *fwd_map[32];
    nfds_t n = 0;
    memset(tcp_map, 0, sizeof(tcp_map));
    memset(fwd_map, 0, sizeof(fwd_map));
    fds[n++] = (struct pollfd){ .fd = nat->wake_pipe[0], .events = POLLIN };

    pthread_mutex_lock(&nat->lock);
    for (size_t i = 0; i < nat->forward_count && n < sizeof(fds) / sizeof(fds[0]); i++) {
      fds[n] = (struct pollfd){ .fd = nat->forwards[i].fd, .events = POLLIN };
      fwd_map[n] = &nat->forwards[i];
      n++;
    }
  for (size_t i = 0; i < ANT_HVF_NET_TCP_MAX && n < sizeof(fds) / sizeof(fds[0]); i++) {
      ant_hvf_nat_tcp_t *c = &nat->tcp[i];
      if (!c->used || c->fd < 0) continue;
      uint32_t in_flight = ant_nat_host_sent_seq(c) - c->guest_ack;
      uint32_t guest_window = c->guest_window ? c->guest_window : ANT_TCP_GUEST_WINDOW;
      short ev = in_flight < guest_window &&
                 c->host_len < sizeof(c->host_buf) &&
                 !c->host_eof ? POLLIN : 0;
      if (c->connecting_host || c->pending_len) ev |= POLLOUT;
      if (!ev) continue;
      fds[n] = (struct pollfd){ .fd = c->fd, .events = ev };
      tcp_map[n] = c;
      n++;
    }
    pthread_mutex_unlock(&nat->lock);

    int prc = poll(fds, n, 25);
    if (prc < 0 && errno == EINTR) continue;
    if (prc <= 0) continue;
    if (fds[0].revents & POLLIN) ant_net_drain_wake(nat);

    pthread_mutex_lock(&nat->lock);
    for (nfds_t i = 1; i < n; i++) {
      if (!fds[i].revents) continue;
      if (fwd_map[i]) {
        int fd = accept(fwd_map[i]->fd, NULL, NULL);
        if (fd >= 0) {
          ant_socket_nonblock(fd);
          ant_hvf_nat_tcp_t *c = ant_nat_alloc_tcp(nat);
          if (c) {
            c->inbound = true;
            c->fd = fd;
            c->guest_ip = ANT_NET_GUEST_IP;
            c->guest_port = fwd_map[i]->guest_port;
            c->peer_ip = ANT_NET_HOST_IP;
            c->peer_port = fwd_map[i]->host_port;
            c->nat_iss = 0x41000000u + (uint32_t)i;
            c->nat_next = c->nat_iss + 1u;
            c->guest_ack = c->nat_iss;
            c->guest_window = 65535u;
            c->host_base = c->nat_next;
            c->connecting_guest = true;
            memcpy(c->guest_mac, nat->vm->net_guest_mac_seen ? nat->vm->net_guest_mac :
                                  (uint8_t[]){ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, 6);
            ant_net_send_tcp(nat->vm, c->guest_mac, c->peer_ip, c->guest_ip, c->peer_port, c->guest_port,
                             c->nat_iss, 0, ANT_TCP_SYN, NULL, 0);
          } else {
            close(fd);
          }
        }
        continue;
      }
      ant_hvf_nat_tcp_t *c = tcp_map[i];
      if (!c || !c->used) continue;
      if (fds[i].revents & POLLOUT) {
        ant_nat_check_connect(c);
        if (c->used) ant_nat_flush_pending(c);
      }
      if (c->used && (fds[i].revents & (POLLIN | POLLHUP))) ant_nat_tcp_from_host(nat, c);
      if (c->used && (fds[i].revents & POLLERR)) ant_nat_close_tcp(c);
    }
    pthread_mutex_unlock(&nat->lock);
  }
  return NULL;
}

static void ant_net_handle_tcp(ant_hvf_vm_t *vm,
                               const ant_eth_t *eth,
                               const ant_ipv4_t *ip,
                               const ant_tcp_t *tcp,
                               const unsigned char *payload,
                               size_t payload_len) {
  ant_hvf_nat_t *nat = vm->net_nat;
  if (!nat) return;
  uint32_t src_ip = ntohl(ip->src);
  uint32_t dst_ip = ntohl(ip->dst);
  uint16_t src_port = ntohs(tcp->src);
  uint16_t dst_port = ntohs(tcp->dst);
  uint32_t seq = ntohl(tcp->seq);
  uint32_t ack = ntohl(tcp->ack);

  pthread_mutex_lock(&nat->lock);
  ant_hvf_nat_tcp_t *c = ant_nat_find_tcp(nat, src_ip, src_port, dst_ip, dst_port);
  if (!c) c = ant_nat_find_tcp(nat, dst_ip, dst_port, src_ip, src_port);

  if (!c && (tcp->flags & ANT_TCP_SYN) && !(tcp->flags & ANT_TCP_ACK)) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) goto done;
    ant_socket_nonblock(fd);
    c = ant_nat_alloc_tcp(nat);
    if (!c) {
      close(fd);
      goto done;
    }
    c->fd = fd;
    c->guest_ip = src_ip;
    c->guest_port = src_port;
    c->peer_ip = dst_ip;
    c->peer_port = dst_port;
    c->guest_next = seq + 1u;
    c->nat_iss = 0x51000000u + (uint32_t)(src_port << 4u);
    c->nat_next = c->nat_iss + 1u;
    c->guest_ack = c->nat_iss;
    c->guest_window = ntohs(tcp->win);
    c->host_base = c->nat_next;
    memcpy(c->guest_mac, eth->src, 6);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(dst_ip == ANT_NET_HOST_IP ? 0x7f000001u : dst_ip);
    addr.sin_port = htons(dst_port);
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS) {
      ant_net_send_rst(vm, eth->src, dst_ip, src_ip, dst_port, src_port, 0, seq + 1u);
      ant_nat_close_tcp(c);
      goto done;
    }
    c->connecting_host = rc != 0;
    ant_net_send_tcp(vm, eth->src, dst_ip, src_ip, dst_port, src_port, c->nat_iss, c->guest_next,
                     ANT_TCP_SYN | ANT_TCP_ACK, NULL, 0);
    ant_net_wake(nat);
    goto done;
  }

  if (!c) {
    if (!(tcp->flags & ANT_TCP_RST)) {
      ant_net_send_rst(vm, eth->src, dst_ip, src_ip, dst_port, src_port, ack, seq + (uint32_t)payload_len);
    }
    goto done;
  }

  if (tcp->flags & ANT_TCP_RST) {
    ant_nat_close_tcp(c);
    goto done;
  }

  uint32_t prev_window = c->guest_window;
  c->guest_window = ntohs(tcp->win);

  if (tcp->flags & ANT_TCP_ACK) {
    uint32_t prev = c->guest_ack;
    if ((int32_t)(ack - c->guest_ack) > 0) c->guest_ack = ack;
    if (c->guest_ack != prev || c->guest_window != prev_window) {
      ant_nat_drop_guest_acked(c);
      ant_nat_send_host_buffer(nat, c);
      ant_net_wake(nat);
    }
  }

  if (c->connecting_guest && (tcp->flags & ANT_TCP_SYN) && (tcp->flags & ANT_TCP_ACK)) {
    c->guest_next = seq + 1u;
    c->connecting_guest = false;
    c->guest_ack = ack;
    c->guest_window = ntohs(tcp->win);
    ant_nat_drop_guest_acked(c);
    ant_net_send_tcp(vm, c->guest_mac, c->peer_ip, c->guest_ip, c->peer_port, c->guest_port,
                     c->nat_next, c->guest_next, ANT_TCP_ACK, NULL, 0);
    ant_net_wake(nat);
    goto done;
  }

  if (payload_len > 0) {
    if (vm->trace) fprintf(stderr, "sandbox vm: nat guest payload %zu bytes peer=%u.%u.%u.%u:%u guest_port=%u pending=%zu\n",
                           payload_len,
                           (c->peer_ip >> 24u) & 0xffu, (c->peer_ip >> 16u) & 0xffu,
                           (c->peer_ip >> 8u) & 0xffu, c->peer_ip & 0xffu,
                           c->peer_port, c->guest_port, c->pending_len);
    c->guest_next = seq + (uint32_t)payload_len;
    if (c->fd >= 0 && !c->guest_fin_seen) {
      size_t cap = sizeof(c->pending) - c->pending_off - c->pending_len;
      if (payload_len <= cap) {
        memcpy(c->pending + c->pending_off + c->pending_len, payload, payload_len);
        c->pending_len += payload_len;
      }
    }
    ant_net_send_tcp(vm, c->guest_mac, c->peer_ip, c->guest_ip, c->peer_port, c->guest_port,
                     c->nat_next, c->guest_next, ANT_TCP_ACK, NULL, 0);
    ant_nat_flush_pending(c);
    ant_net_wake(nat);
  }

  if (tcp->flags & ANT_TCP_FIN) {
    if (vm->trace) fprintf(stderr, "sandbox vm: nat guest fin peer=%u.%u.%u.%u:%u guest_port=%u ack=%u nat_next=%u\n",
                           (c->peer_ip >> 24u) & 0xffu, (c->peer_ip >> 16u) & 0xffu,
                           (c->peer_ip >> 8u) & 0xffu, c->peer_ip & 0xffu,
                           c->peer_port, c->guest_port, c->guest_ack, c->nat_next);
    uint32_t fin_next = seq + (uint32_t)payload_len + 1u;
    if (fin_next > c->guest_next) c->guest_next = fin_next;
    c->guest_fin_seen = true;
    if (c->fd >= 0) shutdown(c->fd, SHUT_WR);
    ant_net_send_tcp(vm, c->guest_mac, c->peer_ip, c->guest_ip, c->peer_port, c->guest_port,
                     c->nat_next, c->guest_next, ANT_TCP_ACK, NULL, 0);
    if (c->host_fin_sent && c->guest_ack >= c->nat_next) ant_nat_close_tcp(c);
  } else if (c->host_fin_sent && c->guest_fin_seen && c->guest_ack >= c->nat_next) {
    ant_nat_close_tcp(c);
  }

done:
  pthread_mutex_unlock(&nat->lock);
}

static void ant_net_handle_icmp(ant_hvf_vm_t *vm,
                                const ant_eth_t *eth,
                                const ant_ipv4_t *ip,
                                const unsigned char *payload,
                                size_t payload_len) {
  if (payload_len < 8 || payload[0] != 8) return;
  uint32_t dst = ntohl(ip->dst);
  if (dst != ANT_NET_HOST_IP && dst != ANT_NET_DNS_IP) return;
  unsigned char reply[1500];
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
    if (vm->trace) fprintf(stderr, "sandbox vm: net udp %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u len=%zu\n",
                           (ntohl(ip->src) >> 24u) & 0xffu, (ntohl(ip->src) >> 16u) & 0xffu,
                           (ntohl(ip->src) >> 8u) & 0xffu, ntohl(ip->src) & 0xffu, sport,
                           (ntohl(ip->dst) >> 24u) & 0xffu, (ntohl(ip->dst) >> 16u) & 0xffu,
                           (ntohl(ip->dst) >> 8u) & 0xffu, ntohl(ip->dst) & 0xffu, dport,
                           l4_len);
    size_t udp_len = ntohs(udp->len);
    if (udp_len < sizeof(*udp) || udp_len > l4_len) return;
    const unsigned char *udp_payload = l4 + sizeof(*udp);
    size_t udp_payload_len = udp_len - sizeof(*udp);
    if (sport == ANT_DHCP_CLIENT_PORT && dport == ANT_DHCP_SERVER_PORT) {
      ant_net_handle_dhcp(vm, eth, udp, udp_payload, udp_payload_len);
    } else if (dport == ANT_DNS_PORT) {
      ant_net_handle_dns(vm, eth, ip, udp, udp_payload, udp_payload_len);
    }
  } else if (ip->proto == ANT_IP_TCP) {
    if (l4_len < sizeof(ant_tcp_t)) return;
    const ant_tcp_t *tcp = (const ant_tcp_t *)l4;
    if (vm->trace) fprintf(stderr, "sandbox vm: net tcp %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u flags=0x%x len=%zu\n",
                           (ntohl(ip->src) >> 24u) & 0xffu, (ntohl(ip->src) >> 16u) & 0xffu,
                           (ntohl(ip->src) >> 8u) & 0xffu, ntohl(ip->src) & 0xffu, ntohs(tcp->src),
                           (ntohl(ip->dst) >> 24u) & 0xffu, (ntohl(ip->dst) >> 16u) & 0xffu,
                           (ntohl(ip->dst) >> 8u) & 0xffu, ntohl(ip->dst) & 0xffu, ntohs(tcp->dst),
                           tcp->flags, l4_len);
    size_t thl = (size_t)(tcp->off >> 4u) * 4u;
    if (thl < sizeof(*tcp) || thl > l4_len) return;
    ant_net_handle_tcp(vm, eth, ip, tcp, l4 + thl, l4_len - thl);
  } else if (ip->proto == ANT_IP_ICMP) {
    ant_net_handle_icmp(vm, eth, ip, l4, l4_len);
  }
}

int ant_hvf_net_start(ant_hvf_vm_t *vm) {
  ant_hvf_nat_t *nat = calloc(1, sizeof(*nat));
  if (!nat) return -ENOMEM;
  nat->vm = vm;
  nat->wake_pipe[0] = -1;
  nat->wake_pipe[1] = -1;
  for (size_t i = 0; i < ANT_HVF_NET_TCP_MAX; i++) nat->tcp[i].fd = -1;
  vm->net_nat = nat;

  if (pthread_mutex_init(&nat->lock, NULL) != 0) {
    vm->net_nat = NULL;
    free(nat);
    return -errno;
  }
  nat->lock_init = true;

  if (pipe(nat->wake_pipe) != 0) {
    int rc = -errno;
    ant_hvf_net_stop(vm);
    return rc;
  }
  ant_socket_nonblock(nat->wake_pipe[0]);
  ant_socket_nonblock(nat->wake_pipe[1]);

  for (size_t i = 0; i < vm->net_forward_count; i++) {
    if (nat->forward_count >= sizeof(nat->forwards) / sizeof(nat->forwards[0])) {
      ant_hvf_net_stop(vm);
      return -E2BIG;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      int rc = -errno;
      ant_hvf_net_stop(vm);
      return rc;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ant_socket_nonblock(fd);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001u);
    addr.sin_port = htons(vm->net_forwards[i].host_port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 64) != 0) {
      int rc = -errno;
      close(fd);
      ant_hvf_net_stop(vm);
      return rc;
    }
    nat->forwards[nat->forward_count++] = (ant_hvf_nat_forward_t){
      .fd = fd,
      .host_port = vm->net_forwards[i].host_port,
      .guest_port = vm->net_forwards[i].guest_port,
    };
  }

  int prc = pthread_create(&nat->thread, NULL, ant_nat_thread, nat);
  if (prc != 0) {
    ant_hvf_net_stop(vm);
    return -prc;
  }
  nat->thread_started = true;
  vm->net_started = true;
  vm->net_max_packet_size = 1518u;
  if (vm->trace) fprintf(stderr, "sandbox vm: native NAT network started forwards=%zu\n", nat->forward_count);
  return 0;
}

void ant_hvf_net_stop(ant_hvf_vm_t *vm) {
  ant_hvf_nat_t *nat = vm->net_nat;
  if (!nat) return;
  nat->stop = true;
  ant_net_wake(nat);
  if (nat->thread_started) pthread_join(nat->thread, NULL);
  for (size_t i = 0; i < ANT_HVF_NET_TCP_MAX; i++) {
    if (nat->tcp[i].used) ant_nat_close_tcp(&nat->tcp[i]);
  }
  for (size_t i = 0; i < nat->forward_count; i++) {
    if (nat->forwards[i].fd >= 0) close(nat->forwards[i].fd);
  }
  if (nat->wake_pipe[0] >= 0) close(nat->wake_pipe[0]);
  if (nat->wake_pipe[1] >= 0) close(nat->wake_pipe[1]);
  if (nat->lock_init) pthread_mutex_destroy(&nat->lock);
  free(nat);
  vm->net_nat = NULL;
  vm->net_started = false;
}

int ant_hvf_virtio_net_tx(ant_hvf_vm_t *vm,
                                 uint64_t desc_base,
                                 uint16_t head,
                                 uint32_t *used_len) {
  unsigned char packet[ANT_HVF_NET_MAX_PACKET];
  uint32_t total = 0;
  int rc = ant_hvf_vring_read_chain(vm,
                                    desc_base,
                                    head,
                                    ANT_VIRTIO_NET_QUEUE_SIZE,
                                    packet,
                                    sizeof(packet),
                                    &total);
  if (rc != 0) return rc;

  if (vm->net_started && total > ANT_VIRTIO_NET_HDR_LEN) {
    const unsigned char *frame = packet + ANT_VIRTIO_NET_HDR_LEN;
    size_t frame_len = total - ANT_VIRTIO_NET_HDR_LEN;
    if (frame_len >= sizeof(ant_eth_t)) {
      const ant_eth_t *eth = (const ant_eth_t *)frame;
      uint16_t ethertype = ntohs(eth->ethertype);
      if (ethertype == ANT_ETH_ARP) {
        ant_net_handle_arp(vm, eth, frame + sizeof(*eth), frame_len - sizeof(*eth));
      } else if (ethertype == ANT_ETH_IPV4) {
        ant_net_handle_ipv4(vm, eth, frame + sizeof(*eth), frame_len - sizeof(*eth));
      }
    }
  }

  if (vm->trace) fprintf(stderr, "sandbox vm: net tx complete head=%u used_len=%u\n", head, total);
  *used_len = total;
  return 0;
}

int ant_hvf_virtio_net_drain_rx(ant_hvf_vm_t *vm) {
  ant_hvf_virtio_device_t *dev = &vm->net;
  ant_hvf_virtio_queue_t *q = &dev->queues[ANT_VIRTIO_NET_RX_QUEUE];
  if (!q->enabled || !q->desc || !q->avail || !q->used) return 0;

  uint64_t desc_base = q->desc;
  uint64_t avail_base = q->avail;
  uint64_t used_base = q->used;

  unsigned char idx_raw[2];
  int rc = ant_hvf_guest_read(vm, avail_base + 2, idx_raw, sizeof(idx_raw));
  if (rc != 0) return rc;
  uint16_t avail_idx = ant_hvf_load16(idx_raw);
  bool delivered = false;

  for (;;) {
    pthread_mutex_lock(&vm->net_lock);
    bool have_packet = vm->net_rx_count > 0;
    ant_hvf_net_packet_t packet;
    if (have_packet) packet = vm->net_rx_packets[vm->net_rx_head];
    vm->net_rx_wake = false;
    pthread_mutex_unlock(&vm->net_lock);

    if (!have_packet || q->last_avail == avail_idx) break;

    unsigned char guest_packet[ANT_VIRTIO_NET_HDR_LEN + ANT_HVF_NET_MAX_PACKET];
    memset(guest_packet, 0, ANT_VIRTIO_NET_HDR_LEN);
    memcpy(guest_packet + ANT_VIRTIO_NET_HDR_LEN, packet.data, packet.len);
    uint32_t guest_len = ANT_VIRTIO_NET_HDR_LEN + packet.len;

    uint16_t ring_slot = q->last_avail % q->size;
    unsigned char head_raw[2];
    rc = ant_hvf_guest_read(vm, avail_base + 4u + (uint64_t)ring_slot * 2u, head_raw, sizeof(head_raw));
    if (rc != 0) return rc;
    uint16_t head = ant_hvf_load16(head_raw);

    uint32_t chain_used = 0;
    rc = ant_hvf_vring_write_chain(vm, desc_base, head, q->size, guest_packet, guest_len, &chain_used);
    if (rc != 0) return rc;

    rc = ant_hvf_vring_add_used(vm, used_base, q->size, head, chain_used);
    if (rc != 0) return rc;
    q->last_avail++;

    pthread_mutex_lock(&vm->net_lock);
    if (vm->net_rx_count > 0) {
      vm->net_rx_head = (vm->net_rx_head + 1u) % ANT_HVF_NET_RX_BACKLOG;
      vm->net_rx_count--;
    }
    pthread_mutex_unlock(&vm->net_lock);

    delivered = true;
  }

  if (vm->trace && delivered) fprintf(stderr, "sandbox vm: net rx delivered queue=%u backlog=%u\n",
                                      ANT_VIRTIO_NET_RX_QUEUE, vm->net_rx_count);
  return delivered ? ant_hvf_virtio_interrupt(vm, dev, ANT_VIRTIO_NET_RX_QUEUE) : 0;
}

int ant_hvf_virtio_net_notify(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev, unsigned queue) {
  if (queue >= ANT_VIRTIO_NET_QUEUE_COUNT) return 0;
  ant_hvf_virtio_queue_t *q = &dev->queues[queue];
  if (!q->enabled || !q->desc || !q->avail || !q->used) return 0;

  uint64_t desc_base = q->desc;
  uint64_t avail_base = q->avail;
  uint64_t used_base = q->used;

  unsigned char idx_raw[2];
  int rc = ant_hvf_guest_read(vm, avail_base + 2, idx_raw, sizeof(idx_raw));
  if (rc != 0) return rc;
  uint16_t avail_idx = ant_hvf_load16(idx_raw);

  if (queue == ANT_VIRTIO_NET_RX_QUEUE) return ant_hvf_virtio_net_drain_rx(vm);

  while (q->last_avail != avail_idx) {
    uint16_t ring_slot = q->last_avail % q->size;
    unsigned char head_raw[2];
    rc = ant_hvf_guest_read(vm, avail_base + 4u + (uint64_t)ring_slot * 2u, head_raw, sizeof(head_raw));
    if (rc != 0) return rc;
    uint16_t head = ant_hvf_load16(head_raw);

    uint32_t chain_used = 0;
    rc = ant_hvf_virtio_net_tx(vm, desc_base, head, &chain_used);
    if (rc != 0) return rc;

    rc = ant_hvf_vring_add_used(vm, used_base, q->size, head, chain_used);
    if (rc != 0) return rc;
    q->last_avail++;
  }

  return ant_hvf_virtio_interrupt(vm, dev, queue);
}

#endif
