#pragma once

#include <compat.h> // IWYU pragma: keep

#include "sandbox/transport.h"
#include "virtio.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#define ANT_VIRTIO_VSOCK_F_STREAM 0x1u
#define ANT_VIRTIO_VSOCK_QUEUE_COUNT 3u
#define ANT_VIRTIO_VSOCK_QUEUE_SIZE 32u
#define ANT_VIRTIO_VSOCK_TYPE_STREAM 1u
#define ANT_VIRTIO_VSOCK_OP_REQUEST 1u
#define ANT_VIRTIO_VSOCK_OP_RESPONSE 2u
#define ANT_VIRTIO_VSOCK_OP_RST 3u
#define ANT_VIRTIO_VSOCK_OP_SHUTDOWN 4u
#define ANT_VIRTIO_VSOCK_OP_RW 5u
#define ANT_VIRTIO_VSOCK_OP_CREDIT_UPDATE 6u
#define ANT_VIRTIO_VSOCK_OP_CREDIT_REQUEST 7u
#define ANT_VIRTIO_VSOCK_EVENT_TRANSPORT_RESET 0u
#define ANT_HVF_VSOCK_GUEST_CID 3ull
#define ANT_HVF_VSOCK_HOST_CID ANT_SANDBOX_TRANSPORT_VSOCK_HOST_CID
#define ANT_HVF_VSOCK_HOST_PORT ANT_SANDBOX_TRANSPORT_VSOCK_PORT
#define ANT_HVF_VSOCK_BUF_ALLOC 65536u
#define ANT_HVF_VSOCK_MAX_PAYLOAD 2048u

typedef struct {
  uint64_t src_cid;
  uint64_t dst_cid;
  uint32_t src_port;
  uint32_t dst_port;
  uint32_t len;
  uint16_t type;
  uint16_t op;
  uint32_t flags;
  uint32_t buf_alloc;
  uint32_t fwd_cnt;
} __attribute__((packed)) ant_virtio_vsock_hdr_t;

typedef struct ant_vsock_outgoing_frame {
  unsigned char *data;
  size_t len;
  size_t off;
  struct ant_vsock_outgoing_frame *next;
} ant_vsock_outgoing_frame_t;

typedef struct {
  ant_hvf_virtio_device_t virtio;
  bool connected;
  bool response_sent;
  atomic_bool request_sent;
  ant_vsock_outgoing_frame_t *outgoing_head;
  ant_vsock_outgoing_frame_t *outgoing_tail;
  bool protocol_error;
  bool transport_error;
  uint32_t peer_port;
  uint32_t peer_buf_alloc;
  uint32_t peer_fwd_cnt;
  uint32_t tx_cnt;
  uint32_t fwd_cnt;
  bool event_transport_reset_pending;
  bool exit_received;
  int exit_code;
  uint32_t capabilities;
  unsigned char *rx_stream;
  size_t rx_stream_len;
  size_t rx_stream_cap;
} ant_hvf_vsock_device_t;

void ant_hvf_vsock_store_hdr(unsigned char *out, const ant_virtio_vsock_hdr_t *hdr);
ant_virtio_vsock_hdr_t ant_hvf_vsock_load_hdr(const unsigned char *raw);

int ant_hvf_vsock_maybe_send_request(ant_hvf_vm_t *vm);
int ant_hvf_vsock_queue_frame(ant_hvf_vm_t *vm, const void *data, size_t len);
void ant_hvf_vsock_clear_frames(ant_hvf_vm_t *vm);
int ant_hvf_virtio_vsock_notify(ant_hvf_vm_t *vm, unsigned queue);

int ant_hvf_vsock_send_packet(
  ant_hvf_vm_t *vm,
  uint16_t op,
  const void *payload,
  uint32_t payload_len
);

int ant_hvf_vsock_read_tx_packet(
  ant_hvf_vm_t *vm,
  uint64_t desc_base,
  uint16_t head,
  ant_virtio_vsock_hdr_t *hdr,
  unsigned char **payload,
  uint32_t *used_len
);

int ant_hvf_vsock_write_iov(
  ant_hvf_vm_t *vm,
  uint64_t desc_base,
  uint16_t head,
  const unsigned char *data,
  uint32_t len,
  uint32_t *used_len
);
