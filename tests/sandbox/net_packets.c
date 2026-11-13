#include "shared/backend.h"

#define ANT_NET_IP(a, b, c, d) \
  ((((uint32_t)(a)) << 24u) | (((uint32_t)(b)) << 16u) | (((uint32_t)(c)) << 8u) | ((uint32_t)(d)))
#define ANT_NET_HOST_IP ANT_NET_IP(10, 0, 2, 2)
#define ANT_NET_GUEST_IP ANT_NET_IP(10, 0, 2, 15)
#define ANT_NET_BROADCAST_IP ANT_NET_IP(255, 255, 255, 255)
#define ANT_ETH_IPV4 0x0800u
#define ANT_IP_TCP 6u
#define ANT_IP_UDP 17u
#define ANT_TCP_SYN 0x02u
#define ANT_TCP_RST 0x04u
#define ANT_TCP_ACK 0x10u
#define ANT_DHCP_SERVER_PORT 67u
#define ANT_DHCP_CLIENT_PORT 68u

#include <assert.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(__aarch64__) && defined(__APPLE__)

#define TEST_ETH_BYTES 14u
#define TEST_IPV4_BYTES 20u
#define TEST_UDP_BYTES 8u
#define TEST_TCP_BYTES 20u
#define TEST_GUEST_PORT 42000u

typedef struct {
  size_t len;
  unsigned char data[ANT_HVF_NET_MAX_PACKET];
} test_packet_t;

static const uint8_t guest_mac[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };

static void test_put16(unsigned char *p, uint16_t value) {
  p[0] = (unsigned char)(value >> 8u);
  p[1] = (unsigned char)value;
}

static void test_put32(unsigned char *p, uint32_t value) {
  test_put16(p, (uint16_t)(value >> 16u));
  test_put16(p + 2, (uint16_t)value);
}

static uint16_t test_get16(const unsigned char *p) {
  return (uint16_t)(((uint16_t)p[0] << 8u) | p[1]);
}

static uint32_t test_get32(const unsigned char *p) {
  return ((uint32_t)test_get16(p) << 16u) | test_get16(p + 2);
}

static void test_vm_start(ant_hvf_vm_t *vm) {
  memcpy(vm->net_mac, (uint8_t[]){ 0x02, 0x41, 0x4e, 0x54, 0x00, 0x01 }, 6);
  assert(ant_hvf_net_start(vm) == 0);
  assert(ant_hvf_vm_net(vm) != NULL);
}

static bool test_wait_packet(ant_hvf_vm_t *vm, test_packet_t *out, int ms) {
  for (int i = 0; i < ms; i += 5) {
    out->len = ant_net_rx_pop(ant_hvf_vm_net(vm), out->data, sizeof(out->data));
    if (out->len > 0) return true;
    usleep(5000);
  }
  return false;
}

static size_t test_emit_ipv4(unsigned char *frame, uint8_t proto, uint32_t dst_ip, size_t l4_len) {
  memset(frame, 0, TEST_ETH_BYTES + TEST_IPV4_BYTES + l4_len);
  memset(frame, 0xff, 6);
  memcpy(frame + 6, guest_mac, 6);
  test_put16(frame + 12, ANT_ETH_IPV4);

  unsigned char *ip = frame + TEST_ETH_BYTES;
  ip[0] = 0x45;
  test_put16(ip + 2, (uint16_t)(TEST_IPV4_BYTES + l4_len));
  ip[8] = 64;
  ip[9] = proto;
  test_put32(ip + 12, ANT_NET_GUEST_IP);
  test_put32(ip + 16, dst_ip);
  return TEST_ETH_BYTES + TEST_IPV4_BYTES + l4_len;
}

static void test_guest_udp(
  ant_hvf_vm_t *vm, uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
  const void *payload, uint16_t payload_len
) {
  unsigned char frame[ANT_HVF_NET_MAX_PACKET];
  size_t len = test_emit_ipv4(frame, ANT_IP_UDP, dst_ip, TEST_UDP_BYTES + payload_len);
  unsigned char *udp = frame + TEST_ETH_BYTES + TEST_IPV4_BYTES;
  test_put16(udp, src_port);
  test_put16(udp + 2, dst_port);
  test_put16(udp + 4, (uint16_t)(TEST_UDP_BYTES + payload_len));
  memcpy(udp + TEST_UDP_BYTES, payload, payload_len);
  ant_net_inject(ant_hvf_vm_net(vm), frame, len);
}

static void test_guest_tcp(
  ant_hvf_vm_t *vm, uint32_t dst_ip, uint16_t dst_port,
  uint32_t seq, uint32_t ack, uint8_t flags, uint16_t window
) {
  unsigned char frame[ANT_HVF_NET_MAX_PACKET];
  size_t len = test_emit_ipv4(frame, ANT_IP_TCP, dst_ip, TEST_TCP_BYTES);
  unsigned char *tcp = frame + TEST_ETH_BYTES + TEST_IPV4_BYTES;
  test_put16(tcp, TEST_GUEST_PORT);
  test_put16(tcp + 2, dst_port);
  test_put32(tcp + 4, seq);
  test_put32(tcp + 8, ack);
  tcp[12] = (unsigned char)(5u << 4u);
  tcp[13] = flags;
  test_put16(tcp + 14, window);
  ant_net_inject(ant_hvf_vm_net(vm), frame, len);
}

static const unsigned char *test_packet_l4(const test_packet_t *packet, uint8_t proto) {
  assert(packet->len >= TEST_ETH_BYTES + TEST_IPV4_BYTES);
  const unsigned char *ip = packet->data + TEST_ETH_BYTES;
  assert(ip[9] == proto);
  return ip + (size_t)(ip[0] & 0x0fu) * 4u;
}

static size_t test_tcp_payload_len(const test_packet_t *packet) {
  const unsigned char *tcp = test_packet_l4(packet, ANT_IP_TCP);
  size_t header = (size_t)(tcp - packet->data) + (size_t)(tcp[12] >> 4u) * 4u;
  return packet->len - header;
}

static uint32_t test_drain_tcp_data(ant_hvf_vm_t *vm, uint32_t expected_seq, size_t *total) {
  test_packet_t packet;
  while (test_wait_packet(vm, &packet, 300)) {
    size_t len = test_tcp_payload_len(&packet);
    if (len == 0) continue;
    assert(test_get32(test_packet_l4(&packet, ANT_IP_TCP) + 4) == expected_seq);
    expected_seq += (uint32_t)len;
    *total += len;
  }
  return expected_seq;
}

static uint16_t test_tcp_listener(int *fd_out) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(0x7f000001u) };
  assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  assert(listen(fd, 8) == 0);
  socklen_t len = sizeof(addr);
  assert(getsockname(fd, (struct sockaddr *)&addr, &len) == 0);
  *fd_out = fd;
  return ntohs(addr.sin_port);
}

static uint32_t test_handshake(ant_hvf_vm_t *vm, uint16_t port, uint16_t window) {
  test_guest_tcp(vm, ANT_NET_HOST_IP, port, 1000, 0, ANT_TCP_SYN, window);
  test_packet_t packet;
  assert(test_wait_packet(vm, &packet, 2000));
  const unsigned char *synack = test_packet_l4(&packet, ANT_IP_TCP);
  assert(test_get16(synack) == port);
  assert(test_get16(synack + 2) == TEST_GUEST_PORT);
  assert((synack[13] & (ANT_TCP_SYN | ANT_TCP_ACK)) == (ANT_TCP_SYN | ANT_TCP_ACK));
  uint32_t data_seq = test_get32(synack + 4) + 1u;
  test_guest_tcp(vm, ANT_NET_HOST_IP, port, 1001, data_seq, ANT_TCP_ACK, window);
  return data_seq;
}

static void test_dhcp_response(void) {
  ant_hvf_vm_t vm;
  memset(&vm, 0, sizeof(vm));
  test_vm_start(&vm);

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
  test_guest_udp(&vm, ANT_NET_BROADCAST_IP, ANT_DHCP_CLIENT_PORT, ANT_DHCP_SERVER_PORT, dhcp, sizeof(dhcp));

  test_packet_t packet;
  assert(test_wait_packet(&vm, &packet, 2000));
  const unsigned char *udp = test_packet_l4(&packet, ANT_IP_UDP);
  assert(test_get16(udp) == ANT_DHCP_SERVER_PORT);
  const unsigned char *resp = udp + TEST_UDP_BYTES;
  assert(resp[0] == 2);
  assert(test_get32(resp + 16) == ANT_NET_GUEST_IP);
  assert(resp[240] == 53 && resp[241] == 1 && resp[242] == 2);

  ant_hvf_net_stop(&vm);
  assert(ant_hvf_vm_net(&vm) == NULL);
  ant_devices_free(vm.devices);
}

static void test_tcp_handshake_and_zero_window(void) {
  ant_hvf_vm_t vm;
  memset(&vm, 0, sizeof(vm));
  vm.net_allow_host_all = true;
  test_vm_start(&vm);

  int listener = -1;
  uint16_t port = test_tcp_listener(&listener);
  uint32_t data_seq = test_handshake(&vm, port, 2000);

  int peer = accept(listener, NULL, NULL);
  assert(peer >= 0);
  unsigned char body[6000];
  memset(body, 0x61, sizeof(body));
  assert(write(peer, body, sizeof(body)) == (ssize_t)sizeof(body));

  size_t received = 0;
  uint32_t next_seq = test_drain_tcp_data(&vm, data_seq, &received);
  assert(received == 2000);

  test_guest_tcp(&vm, ANT_NET_HOST_IP, port, 1001, next_seq, ANT_TCP_ACK, 0);
  next_seq = test_drain_tcp_data(&vm, next_seq, &received);
  assert(received == 2000);

  test_guest_tcp(&vm, ANT_NET_HOST_IP, port, 1001, next_seq, ANT_TCP_ACK, 4000);
  next_seq = test_drain_tcp_data(&vm, next_seq, &received);
  assert(received == 6000);
  assert(next_seq == data_seq + 6000u);

  test_guest_tcp(&vm, ANT_NET_HOST_IP, port, 1001, next_seq, ANT_TCP_RST, 0);
  close(peer);
  close(listener);
  ant_hvf_net_stop(&vm);
  assert(ant_hvf_vm_net(&vm) == NULL);
  ant_devices_free(vm.devices);
}

static void test_forward_accept_path(void) {
  ant_hvf_vm_t vm;
  memset(&vm, 0, sizeof(vm));

  int probe = -1;
  uint16_t host_port = test_tcp_listener(&probe);
  close(probe);
  ant_sandbox_port_forward_t forward = { .host_port = host_port, .guest_port = 3000 };
  vm.net_forwards = &forward;
  vm.net_forward_count = 1;
  test_vm_start(&vm);

  int client = socket(AF_INET, SOCK_STREAM, 0);
  assert(client >= 0);
  struct sockaddr_in addr = {
    .sin_family = AF_INET, .sin_addr.s_addr = htonl(0x7f000001u), .sin_port = htons(host_port),
  };
  assert(connect(client, (struct sockaddr *)&addr, sizeof(addr)) == 0);

  test_packet_t packet;
  assert(test_wait_packet(&vm, &packet, 2000));
  const unsigned char *tcp = test_packet_l4(&packet, ANT_IP_TCP);
  assert(test_get16(tcp + 2) == 3000);
  assert((tcp[13] & ANT_TCP_SYN) == ANT_TCP_SYN);

  close(client);
  ant_hvf_net_stop(&vm);
  assert(ant_hvf_vm_net(&vm) == NULL);
  ant_devices_free(vm.devices);
}

static void test_udp_nat_roundtrip(void) {
  ant_hvf_vm_t vm;
  memset(&vm, 0, sizeof(vm));
  uint16_t allowed[1];
  vm.net_allow_host_ports = allowed;
  vm.net_allow_host_port_count = 1;

  int server = socket(AF_INET, SOCK_DGRAM, 0);
  assert(server >= 0);
  struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
  setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(0x7f000001u) };
  assert(bind(server, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  socklen_t addr_len = sizeof(addr);
  assert(getsockname(server, (struct sockaddr *)&addr, &addr_len) == 0);
  uint16_t port = ntohs(addr.sin_port);
  allowed[0] = port;
  test_vm_start(&vm);

  test_guest_udp(&vm, ANT_NET_HOST_IP, 54000, port, "ping", 4);
  char buf[16];
  struct sockaddr_in peer;
  socklen_t peer_len = sizeof(peer);
  assert(recvfrom(server, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &peer_len) == 4);
  assert(memcmp(buf, "ping", 4) == 0);
  assert(sendto(server, "pong", 4, 0, (struct sockaddr *)&peer, peer_len) == 4);

  test_packet_t packet;
  assert(test_wait_packet(&vm, &packet, 2000));
  const unsigned char *udp = test_packet_l4(&packet, ANT_IP_UDP);
  assert(test_get16(udp) == port);
  assert(test_get16(udp + 2) == 54000);
  assert(memcmp(udp + TEST_UDP_BYTES, "pong", 4) == 0);

  close(server);
  ant_hvf_net_stop(&vm);
  assert(ant_hvf_vm_net(&vm) == NULL);
  ant_devices_free(vm.devices);
}

static void test_tcp_host_blocked_by_default(void) {
  ant_hvf_vm_t vm;
  memset(&vm, 0, sizeof(vm));
  test_vm_start(&vm);

  int listener = -1;
  uint16_t port = test_tcp_listener(&listener);
  const uint32_t targets[] = { ANT_NET_HOST_IP, ANT_NET_IP(127, 0, 0, 1) };
  for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
    test_guest_tcp(&vm, targets[i], port, 1000, 0, ANT_TCP_SYN, 65535);
    test_packet_t packet;
    assert(test_wait_packet(&vm, &packet, 2000));
    const unsigned char *rst = test_packet_l4(&packet, ANT_IP_TCP);
    assert(test_get16(rst) == port);
    assert((rst[13] & ANT_TCP_RST) == ANT_TCP_RST);
  }

  struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(listener, &readable);
  assert(select(listener + 1, &readable, NULL, NULL, &tv) == 0);

  close(listener);
  ant_hvf_net_stop(&vm);
  assert(ant_hvf_vm_net(&vm) == NULL);
  ant_devices_free(vm.devices);
}

#endif

int main(void) {
#if defined(__aarch64__) && defined(__APPLE__)
  test_dhcp_response();
  test_tcp_handshake_and_zero_window();
  test_forward_accept_path();
  test_udp_nat_roundtrip();
  test_tcp_host_blocked_by_default();
#endif
  return 0;
}
