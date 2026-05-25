#include "sandbox_backend/net_internal.h"

#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(__aarch64__) && defined(__APPLE__)

static const uint8_t guest_mac[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };

static void test_vm_init(ant_hvf_vm_t *vm) {
  memset(vm, 0, sizeof(*vm));
  memcpy(vm->net_mac, (uint8_t[]){ 0x02, 0x41, 0x4e, 0x54, 0x00, 0x01 }, 6);
  assert(pthread_mutex_init(&vm->net_lock, NULL) == 0);
  vm->net_lock_init = true;
  vm->net_started = true;
}

static void test_vm_destroy(ant_hvf_vm_t *vm) {
  if (vm->net_nat) ant_hvf_net_stop(vm);
  if (vm->net_lock_init) pthread_mutex_destroy(&vm->net_lock);
}

static bool test_pop_packet(ant_hvf_vm_t *vm, ant_hvf_net_packet_t *out) {
  bool have = false;
  pthread_mutex_lock(&vm->net_lock);
  if (vm->net_rx_count > 0) {
    *out = vm->net_rx_packets[vm->net_rx_head];
    vm->net_rx_head = (vm->net_rx_head + 1u) % ANT_HVF_NET_RX_BACKLOG;
    vm->net_rx_count--;
    have = true;
  }
  pthread_mutex_unlock(&vm->net_lock);
  return have;
}

static bool test_wait_packet(ant_hvf_vm_t *vm, ant_hvf_net_packet_t *out) {
  for (int i = 0; i < 200; i++) {
    if (test_pop_packet(vm, out)) return true;
    usleep(10000);
  }
  return false;
}

static void test_sockaddr_loopback(struct sockaddr_in *addr, uint16_t port) {
  memset(addr, 0, sizeof(*addr));
  addr->sin_family = AF_INET;
  addr->sin_addr.s_addr = htonl(0x7f000001u);
  addr->sin_port = htons(port);
}

static uint16_t test_tcp_listener(int *fd_out) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr;
  test_sockaddr_loopback(&addr, 0);
  assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  assert(listen(fd, 8) == 0);
  socklen_t len = sizeof(addr);
  assert(getsockname(fd, (struct sockaddr *)&addr, &len) == 0);
  *fd_out = fd;
  return ntohs(addr.sin_port);
}

static uint16_t test_udp_socket(int *fd_out) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  assert(fd >= 0);
  struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  struct sockaddr_in addr;
  test_sockaddr_loopback(&addr, 0);
  assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  socklen_t len = sizeof(addr);
  assert(getsockname(fd, (struct sockaddr *)&addr, &len) == 0);
  *fd_out = fd;
  return ntohs(addr.sin_port);
}

static size_t test_emit_ipv4(unsigned char *frame,
                             uint8_t proto,
                             uint32_t src_ip,
                             uint32_t dst_ip,
                             const void *payload,
                             uint16_t payload_len) {
  ant_eth_t *eth = (ant_eth_t *)frame;
  memset(eth->dst, 0xff, sizeof(eth->dst));
  memcpy(eth->src, guest_mac, 6);
  eth->ethertype = htons(ANT_ETH_IPV4);

  ant_ipv4_t *ip = (ant_ipv4_t *)(frame + sizeof(*eth));
  memset(ip, 0, sizeof(*ip));
  ip->vhl = 0x45;
  ip->len = htons((uint16_t)(sizeof(*ip) + payload_len));
  ip->ttl = 64;
  ip->proto = proto;
  ip->src = htonl(src_ip);
  ip->dst = htonl(dst_ip);
  ip->sum = htons(ant_net_csum(ip, sizeof(*ip), 0));
  memcpy(frame + sizeof(*eth) + sizeof(*ip), payload, payload_len);
  return sizeof(*eth) + sizeof(*ip) + payload_len;
}

static size_t test_emit_udp(unsigned char *frame,
                            uint32_t src_ip,
                            uint32_t dst_ip,
                            uint16_t src_port,
                            uint16_t dst_port,
                            const void *payload,
                            uint16_t payload_len) {
  unsigned char udp_raw[sizeof(ant_udp_t) + 1500];
  ant_udp_t *udp = (ant_udp_t *)udp_raw;
  udp->src = htons(src_port);
  udp->dst = htons(dst_port);
  udp->len = htons((uint16_t)(sizeof(*udp) + payload_len));
  udp->sum = 0;
  memcpy(udp_raw + sizeof(*udp), payload, payload_len);
  return test_emit_ipv4(frame, ANT_IP_UDP, src_ip, dst_ip, udp_raw,
                        (uint16_t)(sizeof(*udp) + payload_len));
}

static size_t test_emit_tcp(unsigned char *frame,
                            uint32_t src_ip,
                            uint32_t dst_ip,
                            uint16_t src_port,
                            uint16_t dst_port,
                            uint32_t seq,
                            uint32_t ack,
                            uint8_t flags,
                            const void *payload,
                            uint16_t payload_len) {
  unsigned char tcp_raw[sizeof(ant_tcp_t) + 1500];
  ant_tcp_t *tcp = (ant_tcp_t *)tcp_raw;
  memset(tcp, 0, sizeof(*tcp));
  tcp->src = htons(src_port);
  tcp->dst = htons(dst_port);
  tcp->seq = htonl(seq);
  tcp->ack = htonl(ack);
  tcp->off = (uint8_t)(5u << 4u);
  tcp->flags = flags;
  tcp->win = htons(65535);
  memcpy(tcp_raw + sizeof(*tcp), payload, payload_len);
  tcp->sum = htons(ant_net_l4_csum(src_ip, dst_ip, ANT_IP_TCP, tcp_raw,
                                   sizeof(*tcp) + payload_len));
  return test_emit_ipv4(frame, ANT_IP_TCP, src_ip, dst_ip, tcp_raw,
                        (uint16_t)(sizeof(*tcp) + payload_len));
}

static const ant_ipv4_t *test_packet_ip(const ant_hvf_net_packet_t *packet) {
  assert(packet->len >= sizeof(ant_eth_t) + sizeof(ant_ipv4_t));
  return (const ant_ipv4_t *)(packet->data + sizeof(ant_eth_t));
}

static const ant_udp_t *test_packet_udp(const ant_hvf_net_packet_t *packet) {
  const ant_ipv4_t *ip = test_packet_ip(packet);
  assert(ip->proto == ANT_IP_UDP);
  return (const ant_udp_t *)((const unsigned char *)ip + ((ip->vhl & 0x0fu) * 4u));
}

static const ant_tcp_t *test_packet_tcp(const ant_hvf_net_packet_t *packet) {
  const ant_ipv4_t *ip = test_packet_ip(packet);
  assert(ip->proto == ANT_IP_TCP);
  return (const ant_tcp_t *)((const unsigned char *)ip + ((ip->vhl & 0x0fu) * 4u));
}

static void test_byte_order_and_checksums(void) {
  unsigned char raw16[2] = { 0x12, 0x34 };
  assert(ant_net_load16(raw16) == 0x1234);

  unsigned char out16[2] = { 0 };
  ant_net_store16(out16, 0xabcd);
  assert(out16[0] == 0xab);
  assert(out16[1] == 0xcd);

  unsigned char out32[4] = { 0 };
  ant_net_store32(out32, 0x01020304);
  assert(out32[0] == 0x01);
  assert(out32[1] == 0x02);
  assert(out32[2] == 0x03);
  assert(out32[3] == 0x04);

  unsigned char one_word[2] = { 0x00, 0x01 };
  assert(ant_net_csum(one_word, sizeof(one_word), 0) == 0xfffe);

  unsigned char all_ones[2] = { 0xff, 0xff };
  assert(ant_net_csum(all_ones, sizeof(all_ones), 0) == 0x0000);

  unsigned char odd_byte[1] = { 0x01 };
  assert(ant_net_csum(odd_byte, sizeof(odd_byte), 0) == 0xfeff);

  unsigned char tcp[20] = { 0 };
  ant_net_store16(tcp, 1234);
  ant_net_store16(tcp + 2, 443);
  tcp[12] = 5u << 4u;
  tcp[13] = ANT_TCP_SYN;
  assert(ant_net_l4_csum(ANT_NET_GUEST_IP, 0x5db8d822u, ANT_IP_TCP, tcp, sizeof(tcp)) == 0x676c);
}

static void test_dhcp_response(void) {
  ant_hvf_vm_t vm;
  test_vm_init(&vm);

  unsigned char dhcp[260];
  memset(dhcp, 0, sizeof(dhcp));
  dhcp[0] = 1;
  dhcp[1] = 1;
  dhcp[2] = 6;
  memcpy(dhcp + 4, "\x12\x34\x56\x78", 4);
  memcpy(dhcp + 28, guest_mac, 6);
  memcpy(dhcp + 236, "\x63\x82\x53\x63", 4);
  dhcp[240] = 53;
  dhcp[241] = 1;
  dhcp[242] = 1;
  dhcp[243] = 255;

  unsigned char frame[ANT_HVF_NET_MAX_PACKET];
  size_t frame_len = test_emit_udp(frame, 0, ANT_NET_BROADCAST_IP,
                                   ANT_DHCP_CLIENT_PORT, ANT_DHCP_SERVER_PORT,
                                   dhcp, sizeof(dhcp));
  ant_net_handle_frame(&vm, frame, frame_len);

  ant_hvf_net_packet_t packet;
  assert(test_pop_packet(&vm, &packet));
  const ant_udp_t *udp = test_packet_udp(&packet);
  assert(ntohs(udp->src) == ANT_DHCP_SERVER_PORT);
  const unsigned char *resp = (const unsigned char *)udp + sizeof(*udp);
  uint32_t yiaddr;
  memcpy(&yiaddr, resp + 16, sizeof(yiaddr));
  assert(resp[0] == 2);
  assert(ntohl(yiaddr) == ANT_NET_GUEST_IP);
  assert(resp[240] == 53 && resp[241] == 1 && resp[242] == 2);

  test_vm_destroy(&vm);
}

static void test_dns_response(void) {
  ant_hvf_vm_t vm;
  test_vm_init(&vm);

  unsigned char query[128];
  memset(query, 0, sizeof(query));
  ant_net_store16(query, 0x4444);
  ant_net_store16(query + 4, 1);
  unsigned char *p = query + 12;
  *p++ = 9;
  memcpy(p, "127.0.0.1", 9);
  p += 9;
  *p++ = 0;
  ant_net_store16(p, 1);
  p += 2;
  ant_net_store16(p, 1);
  p += 2;
  size_t query_len = (size_t)(p - query);

  unsigned char frame[ANT_HVF_NET_MAX_PACKET];
  size_t frame_len = test_emit_udp(frame, ANT_NET_GUEST_IP, ANT_NET_DNS_IP,
                                   53000, ANT_DNS_PORT, query, (uint16_t)query_len);
  ant_net_handle_frame(&vm, frame, frame_len);

  ant_hvf_net_packet_t packet;
  assert(test_pop_packet(&vm, &packet));
  const ant_udp_t *udp = test_packet_udp(&packet);
  const unsigned char *resp = (const unsigned char *)udp + sizeof(*udp);
  assert(ant_net_load16(resp) == 0x4444);
  assert(resp[2] == 0x81 && resp[3] == 0x80);
  assert(ant_net_load16(resp + 6) == 1);
  assert(ant_net_load16(resp + query_len + 10) == 4);
  assert(ant_net_load16(resp + query_len + 12) == 0x7f00);
  assert(ant_net_load16(resp + query_len + 14) == 0x0001);

  test_vm_destroy(&vm);
}

static void test_tcp_handshake_fin_rst(void) {
  ant_hvf_vm_t vm;
  test_vm_init(&vm);
  assert(ant_hvf_net_start(&vm) == 0);

  int listener = -1;
  uint16_t port = test_tcp_listener(&listener);
  unsigned char frame[ANT_HVF_NET_MAX_PACKET];
  size_t frame_len = test_emit_tcp(frame, ANT_NET_GUEST_IP, ANT_NET_HOST_IP,
                                   40000, port, 1000, 0, ANT_TCP_SYN, NULL, 0);
  ant_net_handle_frame(&vm, frame, frame_len);

  ant_hvf_net_packet_t packet;
  assert(test_wait_packet(&vm, &packet));
  const ant_tcp_t *synack = test_packet_tcp(&packet);
  assert(ntohs(synack->src) == port);
  assert(ntohs(synack->dst) == 40000);
  assert((synack->flags & (ANT_TCP_SYN | ANT_TCP_ACK)) == (ANT_TCP_SYN | ANT_TCP_ACK));
  uint32_t nat_seq = ntohl(synack->seq);

  frame_len = test_emit_tcp(frame, ANT_NET_GUEST_IP, ANT_NET_HOST_IP,
                            40000, port, 1001, nat_seq + 1u, ANT_TCP_ACK, NULL, 0);
  ant_net_handle_frame(&vm, frame, frame_len);

  frame_len = test_emit_tcp(frame, ANT_NET_GUEST_IP, ANT_NET_HOST_IP,
                            40000, port, 1001, nat_seq + 1u, ANT_TCP_FIN | ANT_TCP_ACK, NULL, 0);
  ant_net_handle_frame(&vm, frame, frame_len);
  assert(test_wait_packet(&vm, &packet));
  const ant_tcp_t *finack = test_packet_tcp(&packet);
  assert((finack->flags & ANT_TCP_ACK) == ANT_TCP_ACK);

  frame_len = test_emit_tcp(frame, ANT_NET_GUEST_IP, ANT_NET_HOST_IP,
                            40000, port, 1002, nat_seq + 1u, ANT_TCP_RST, NULL, 0);
  ant_net_handle_frame(&vm, frame, frame_len);

  close(listener);
  test_vm_destroy(&vm);
}

static void test_forward_accept_path(void) {
  ant_hvf_vm_t vm;
  test_vm_init(&vm);
  memcpy(vm.net_guest_mac, guest_mac, 6);
  vm.net_guest_mac_seen = true;

  int probe = -1;
  uint16_t host_port = test_tcp_listener(&probe);
  close(probe);
  ant_sandbox_port_forward_t forward = {
    .host_port = host_port,
    .guest_port = 3000,
  };
  vm.net_forwards = &forward;
  vm.net_forward_count = 1;
  assert(ant_hvf_net_start(&vm) == 0);

  int client = socket(AF_INET, SOCK_STREAM, 0);
  assert(client >= 0);
  struct sockaddr_in addr;
  test_sockaddr_loopback(&addr, host_port);
  assert(connect(client, (struct sockaddr *)&addr, sizeof(addr)) == 0);

  ant_hvf_net_packet_t packet;
  assert(test_wait_packet(&vm, &packet));
  const ant_tcp_t *tcp = test_packet_tcp(&packet);
  assert(ntohs(tcp->dst) == 3000);
  assert((tcp->flags & ANT_TCP_SYN) == ANT_TCP_SYN);

  close(client);
  test_vm_destroy(&vm);
}

static void test_udp_nat_roundtrip(void) {
  ant_hvf_vm_t vm;
  test_vm_init(&vm);
  assert(ant_hvf_net_start(&vm) == 0);

  int udp_server = -1;
  uint16_t port = test_udp_socket(&udp_server);
  unsigned char frame[ANT_HVF_NET_MAX_PACKET];
  const char ping[] = "ping";
  size_t frame_len = test_emit_udp(frame, ANT_NET_GUEST_IP, ANT_NET_HOST_IP,
                                   54000, port, ping, (uint16_t)(sizeof(ping) - 1u));
  ant_net_handle_frame(&vm, frame, frame_len);

  char buf[16];
  struct sockaddr_in peer;
  socklen_t peer_len = sizeof(peer);
  ssize_t rn = recvfrom(udp_server, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &peer_len);
  assert(rn == 4);
  assert(memcmp(buf, "ping", 4) == 0);
  assert(sendto(udp_server, "pong", 4, 0, (struct sockaddr *)&peer, peer_len) == 4);

  ant_hvf_net_packet_t packet;
  assert(test_wait_packet(&vm, &packet));
  const ant_udp_t *udp = test_packet_udp(&packet);
  assert(ntohs(udp->src) == port);
  assert(ntohs(udp->dst) == 54000);
  const unsigned char *payload = (const unsigned char *)udp + sizeof(*udp);
  assert(memcmp(payload, "pong", 4) == 0);

  close(udp_server);
  test_vm_destroy(&vm);
}

#endif

int main(void) {
#if defined(__aarch64__) && defined(__APPLE__)
  test_byte_order_and_checksums();
  test_dhcp_response();
  test_dns_response();
  test_tcp_handshake_fin_rst();
  test_forward_accept_path();
  test_udp_nat_roundtrip();
#endif
  return 0;
}
