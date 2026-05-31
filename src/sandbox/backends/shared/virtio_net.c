#include "sandbox_backend/net_internal.h" // IWYU pragma: keep

int ant_hvf_virtio_net_tx(
  ant_hvf_vm_t *vm,
  uint64_t desc_base,
  uint16_t head,
  uint32_t *used_len
) {
  unsigned char packet[ANT_HVF_NET_MAX_PACKET];
  uint32_t total = 0;
  int rc = ant_hvf_vring_read_chain(
    vm,
    desc_base,
    head,
    ANT_VIRTIO_NET_QUEUE_SIZE,
    packet,
    sizeof(packet),
    &total
  );
  if (rc != 0) return rc;

  if (vm->net_started && total > ANT_VIRTIO_NET_HDR_LEN) {
    ant_net_handle_frame(vm, packet + ANT_VIRTIO_NET_HDR_LEN, total - ANT_VIRTIO_NET_HDR_LEN);
  }

  *used_len = total;
  return 0;
}

static int ant_hvf_virtio_net_drain_rx_unlocked(ant_hvf_vm_t *vm) {
  ant_hvf_virtio_device_t *dev = &vm->net;
  ant_hvf_virtio_queue_t *q = &dev->queues[ANT_VIRTIO_NET_RX_QUEUE];
  if (!q->enabled || !q->desc || !q->avail || !q->used) return 0;

  uint64_t desc_base = q->desc;
  uint64_t avail_base = q->avail;
  uint64_t used_base = q->used;

  unsigned char idx_raw[ANT_HVF_BYTES_U16];
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
    unsigned char head_raw[ANT_HVF_BYTES_U16];
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

  return delivered ? ant_hvf_virtio_interrupt(vm, dev, ANT_VIRTIO_NET_RX_QUEUE) : 0;
}

int ant_hvf_virtio_net_drain_rx(ant_hvf_vm_t *vm) {
  if (!vm->virtio_lock_init) return ant_hvf_virtio_net_drain_rx_unlocked(vm);
  pthread_mutex_lock(&vm->virtio_lock);
  int rc = ant_hvf_virtio_net_drain_rx_unlocked(vm);
  pthread_mutex_unlock(&vm->virtio_lock);
  return rc;
}

int ant_hvf_virtio_net_drain_rx_if_wake(ant_hvf_vm_t *vm) {
  if (!vm->net_lock_init) return 0;
  pthread_mutex_lock(&vm->net_lock);
  bool wake = vm->net_rx_wake;
  if (wake) vm->net_rx_wake = false;
  pthread_mutex_unlock(&vm->net_lock);
  return wake ? ant_hvf_virtio_net_drain_rx(vm) : 0;
}

int ant_hvf_virtio_net_notify(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev, unsigned queue) {
  if (queue >= ANT_VIRTIO_NET_QUEUE_COUNT) return 0;
  ant_hvf_virtio_queue_t *q = &dev->queues[queue];
  if (!q->enabled || !q->desc || !q->avail || !q->used) return 0;

  uint64_t desc_base = q->desc;
  uint64_t avail_base = q->avail;
  uint64_t used_base = q->used;

  unsigned char idx_raw[ANT_HVF_BYTES_U16];
  int rc = ant_hvf_guest_read(vm, avail_base + 2, idx_raw, sizeof(idx_raw));
  if (rc != 0) return rc;
  uint16_t avail_idx = ant_hvf_load16(idx_raw);

  if (queue == ANT_VIRTIO_NET_RX_QUEUE) return ant_hvf_virtio_net_drain_rx(vm);

  while (q->last_avail != avail_idx) {
    uint16_t ring_slot = q->last_avail % q->size;
    unsigned char head_raw[ANT_HVF_BYTES_U16];
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
