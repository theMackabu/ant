#pragma once

#include <compat.h> // IWYU pragma: keep

#include "forward.h"
#include "virtio.h"
#include <stdint.h>

#define ANT_VIRTIO_NET_F_MAC 0x20u
#define ANT_VIRTIO_NET_HDR_LEN 12u
#define ANT_HVF_NET_MAX_PACKET 2048u
#define ANT_HVF_NET_RX_BACKLOG 64u
#define ANT_HVF_NET_HOST_RING 1024u
#define ANT_VIRTIO_NET_RX_QUEUE 0u
#define ANT_VIRTIO_NET_TX_QUEUE 1u
#define ANT_VIRTIO_NET_QUEUE_COUNT 2u
#define ANT_VIRTIO_NET_QUEUE_SIZE 256u

typedef struct {
  uint32_t len;
  unsigned char data[ANT_HVF_NET_MAX_PACKET];
} ant_hvf_net_packet_t;

void ant_hvf_net_note_rx(ant_hvf_vm_t *vm);
void ant_hvf_net_stop(ant_hvf_vm_t *vm);

int ant_hvf_net_start(ant_hvf_vm_t *vm);
int ant_hvf_virtio_net_drain_rx(ant_hvf_vm_t *vm);
int ant_hvf_virtio_net_drain_rx_if_wake(ant_hvf_vm_t *vm);
int ant_hvf_virtio_net_tx(ant_hvf_vm_t *vm, uint64_t desc_base, uint16_t head, uint32_t *used_len);
int ant_hvf_virtio_net_notify(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev, unsigned queue);
