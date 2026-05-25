#ifndef ANT_HVF_NET_INTERNAL_H
#define ANT_HVF_NET_INTERNAL_H

#include "backend.h"  // IWYU pragma: keep

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
#define ANT_NAT_TCP_INITIAL_CAP 64u
#define ANT_NAT_STREAM_INITIAL_CAP 4096u
#define ANT_NAT_STREAM_GROW_CHUNK 16384u
#define ANT_NAT_STREAM_BUFFER_LIMIT (1024u * 1024u)
#define ANT_DHCP_SERVER_PORT 67u
#define ANT_DHCP_CLIENT_PORT 68u
#define ANT_DNS_PORT 53u
#define ANT_NAT_INBOUND_PORT_FIRST 49152u
#define ANT_NAT_INBOUND_PORT_LAST  65535u
#define ANT_NET_MAC_BYTES ANT_HVF_MAC_BYTES
#define ANT_DHCP_PACKET_BYTES 548u
#define ANT_DNS_NAME_BYTES 256u
#define ANT_NET_STANDARD_PACKET_BYTES 1500u
#define ANT_NAT_WAKE_DRAIN_BYTES 64u
#define ANT_NAT_WAKE_PIPE_FDS 2u

typedef struct __attribute__((packed)) {
  uint8_t dst[ANT_NET_MAC_BYTES];
  uint8_t src[ANT_NET_MAC_BYTES];
  uint16_t ethertype;
} ant_eth_t;

typedef struct __attribute__((packed)) {
  uint16_t htype;
  uint16_t ptype;
  uint8_t hlen;
  uint8_t plen;
  uint16_t op;
  uint8_t sha[ANT_NET_MAC_BYTES];
  uint32_t spa;
  uint8_t tha[ANT_NET_MAC_BYTES];
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

typedef struct ant_hvf_nat ant_hvf_nat_t;
typedef struct ant_hvf_nat_tcp ant_hvf_nat_tcp_t;

uint16_t ant_net_load16(const unsigned char *p);
uint16_t ant_net_csum(const void *data, size_t len, uint32_t sum);
uint16_t ant_net_l4_csum(uint32_t src, uint32_t dst, uint8_t proto, const void *data, size_t len);

void ant_nat_wake(ant_hvf_nat_t *nat);
void ant_nat_note_guest_packet(ant_hvf_nat_t *nat);
void ant_net_store16(unsigned char *p, uint16_t value);
void ant_net_store32(unsigned char *p, uint32_t value);

void ant_hvf_net_note_rx(ant_hvf_vm_t *vm);
void ant_net_enqueue(ant_hvf_vm_t *vm, const void *data, uint32_t len);
void ant_net_set_guest(ant_hvf_vm_t *vm, const uint8_t mac[ANT_NET_MAC_BYTES]);
void ant_net_handle_frame(ant_hvf_vm_t *vm, const unsigned char *frame, size_t frame_len);

void ant_net_send_udp(
  ant_hvf_vm_t *vm,
  const uint8_t dst_mac[ANT_NET_MAC_BYTES], 
  uint32_t src_ip, uint32_t dst_ip,
  uint16_t src_port, uint16_t dst_port,
  const void *payload, uint16_t payload_len
);

void ant_net_send_tcp(
  ant_hvf_vm_t *vm,
  const uint8_t dst_mac[ANT_NET_MAC_BYTES],
  uint32_t src_ip, uint32_t dst_ip,
  uint16_t src_port, uint16_t dst_port,
  uint32_t seq, uint32_t ack, uint8_t flags,
  const void *payload, uint16_t payload_len
);
  
void ant_net_send_rst(
  ant_hvf_vm_t *vm,
  const uint8_t dst_mac[ANT_NET_MAC_BYTES],
  uint32_t src_ip, uint32_t dst_ip,
  uint16_t src_port, uint16_t dst_port,
  uint32_t seq, uint32_t ack
);

void ant_nat_handle_tcp(
  ant_hvf_nat_t *nat, const ant_eth_t *eth, const ant_ipv4_t *ip,
  const ant_tcp_t *tcp, const unsigned char *payload, size_t payload_len
);

void ant_nat_handle_udp(
  ant_hvf_nat_t *nat, const ant_eth_t *eth, const ant_ipv4_t *ip,
  const ant_udp_t *udp, const unsigned char *payload, size_t payload_len
);

#endif
