#include "backend.h"

#if defined(__aarch64__)

void ant_hvf_net_note_rx(ant_hvf_vm_t *vm) {
  if (!vm->net_lock_init) return;
  pthread_mutex_lock(&vm->net_lock);
  vm->net_rx_wake = true;
  pthread_mutex_unlock(&vm->net_lock);
  if (vm->vcpu) hv_vcpus_exit(&vm->vcpu, 1);
}

void ant_hvf_net_read_available(ant_hvf_vm_t *vm) {
  if (!vm->net_started || !vm->net_iface) return;

  bool queued = false;
  for (unsigned i = 0; i < ANT_HVF_NET_RX_BACKLOG; i++) {
    ant_hvf_net_packet_t packet;
    memset(&packet, 0, sizeof(packet));

    struct iovec iov = {
      .iov_base = packet.data,
      .iov_len = vm->net_max_packet_size ?
                 vm->net_max_packet_size : (uint32_t)sizeof(packet.data),
    };
    if (iov.iov_len > sizeof(packet.data)) iov.iov_len = sizeof(packet.data);

    struct vmpktdesc desc = {
      .vm_pkt_size = iov.iov_len,
      .vm_pkt_iov = &iov,
      .vm_pkt_iovcnt = 1,
      .vm_flags = 0,
    };
    int pktcnt = 1;
    vmnet_return_t rc = vmnet_read(vm->net_iface, &desc, &pktcnt);
    if (rc != VMNET_SUCCESS || pktcnt < 1 || desc.vm_pkt_size == 0) break;
    if (desc.vm_pkt_size > sizeof(packet.data)) continue;
    packet.len = (uint32_t)desc.vm_pkt_size;

    pthread_mutex_lock(&vm->net_lock);
    if (vm->net_rx_count < ANT_HVF_NET_RX_BACKLOG) {
      uint32_t slot = (vm->net_rx_head + vm->net_rx_count) % ANT_HVF_NET_RX_BACKLOG;
      vm->net_rx_packets[slot] = packet;
      vm->net_rx_count++;
      queued = true;
    }
    pthread_mutex_unlock(&vm->net_lock);
  }

  if (queued) ant_hvf_net_note_rx(vm);
}

bool ant_hvf_parse_mac(const char *mac, uint8_t out[6]) {
  if (!mac) return false;
  unsigned int octets[6];
  if (sscanf(mac,
             "%02x:%02x:%02x:%02x:%02x:%02x",
             &octets[0],
             &octets[1],
             &octets[2],
             &octets[3],
             &octets[4],
             &octets[5]) != 6) {
    return false;
  }
  for (size_t i = 0; i < 6; i++) out[i] = (uint8_t)octets[i];
  return true;
}

int ant_hvf_net_start(ant_hvf_vm_t *vm) {
  xpc_object_t interface_desc = xpc_dictionary_create(NULL, NULL, 0);
  if (!interface_desc) return -ENOMEM;
  xpc_dictionary_set_uint64(interface_desc, vmnet_operation_mode_key, VMNET_SHARED_MODE);
  xpc_dictionary_set_bool(interface_desc, vmnet_allocate_mac_address_key, true);

  dispatch_queue_t start_queue = dispatch_queue_create("ant.sandbox.vmnet.start", DISPATCH_QUEUE_SERIAL);
  dispatch_semaphore_t started = dispatch_semaphore_create(0);
  if (!start_queue || !started) {
    if (start_queue) dispatch_release(start_queue);
    if (started) dispatch_release(started);
    xpc_release(interface_desc);
    return -ENOMEM;
  }

  __block vmnet_return_t start_status = VMNET_FAILURE;
  __block interface_ref iface = NULL;
  __block uint32_t max_packet_size = 1518u;
  __block uint64_t mac_bits = 0x0001544e4102ull;

  iface = vmnet_start_interface(interface_desc, start_queue,
    ^(vmnet_return_t status, xpc_object_t interface_param) {
      start_status = status;
      if (status == VMNET_SUCCESS && interface_param) {
        const char *mac_str = xpc_dictionary_get_string(interface_param, vmnet_mac_address_key);
        uint8_t parsed_mac[6];
        if (ant_hvf_parse_mac(mac_str, parsed_mac)) {
          mac_bits = 0;
          for (unsigned i = 0; i < sizeof(parsed_mac); i++) {
            mac_bits |= (uint64_t)parsed_mac[i] << (i * 8u);
          }
        }
        uint64_t vmnet_max = xpc_dictionary_get_uint64(interface_param, vmnet_max_packet_size_key);
        if (vmnet_max > 0 && vmnet_max <= ANT_HVF_NET_MAX_PACKET) {
          max_packet_size = (uint32_t)vmnet_max;
        }
      }
      dispatch_semaphore_signal(started);
    });

  dispatch_semaphore_wait(started, DISPATCH_TIME_FOREVER);
  xpc_release(interface_desc);
  dispatch_release(start_queue);
  dispatch_release(started);

  if (!iface || start_status != VMNET_SUCCESS) {
    fprintf(stderr,
            "sandbox vm: vmnet network start failed (%u); check com.apple.vm.networking entitlement\n",
            start_status);
    return -EIO;
  }

  vm->net_iface = iface;
  vm->net_started = true;
  vm->net_max_packet_size = max_packet_size;
  for (unsigned i = 0; i < sizeof(vm->net_mac); i++) {
    vm->net_mac[i] = (uint8_t)(mac_bits >> (i * 8u));
  }

  vm->net_event_queue = dispatch_queue_create("ant.sandbox.vmnet.events", DISPATCH_QUEUE_SERIAL);
  if (!vm->net_event_queue) return -ENOMEM;
  vmnet_return_t event_rc = vmnet_interface_set_event_callback(
    iface,
    VMNET_INTERFACE_PACKETS_AVAILABLE,
    vm->net_event_queue,
    ^(interface_event_t event_mask, xpc_object_t event) {
      (void)event_mask;
      (void)event;
      ant_hvf_net_read_available(vm);
    });
  if (event_rc != VMNET_SUCCESS) {
    fprintf(stderr, "sandbox vm: vmnet event callback failed (%u)\n", event_rc);
    return -EIO;
  }

  if (vm->trace) {
    fprintf(stderr,
            "sandbox vm: vmnet shared network mac=%02x:%02x:%02x:%02x:%02x:%02x max_packet=%u\n",
            vm->net_mac[0],
            vm->net_mac[1],
            vm->net_mac[2],
            vm->net_mac[3],
            vm->net_mac[4],
            vm->net_mac[5],
            vm->net_max_packet_size);
  }
  return 0;
}

void ant_hvf_net_stop(ant_hvf_vm_t *vm) {
  if (!vm->net_started || !vm->net_iface) return;

  vmnet_interface_set_event_callback(vm->net_iface, 0, NULL, NULL);

  dispatch_queue_t stop_queue = dispatch_queue_create("ant.sandbox.vmnet.stop", DISPATCH_QUEUE_SERIAL);
  dispatch_semaphore_t stopped = dispatch_semaphore_create(0);
  if (stop_queue && stopped) {
    vmnet_return_t stop_rc = vmnet_stop_interface(vm->net_iface, stop_queue, ^(vmnet_return_t status) {
      (void)status;
      dispatch_semaphore_signal(stopped);
    });
    if (stop_rc == VMNET_SUCCESS) dispatch_semaphore_wait(stopped, DISPATCH_TIME_FOREVER);
  }

  if (stopped) dispatch_release(stopped);
  if (stop_queue) dispatch_release(stop_queue);
  if (vm->net_event_queue) dispatch_release(vm->net_event_queue);
  vm->net_iface = NULL;
  vm->net_event_queue = NULL;
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
    static const unsigned char pad[60];
    uint32_t payload_len = total - ANT_VIRTIO_NET_HDR_LEN;
    struct iovec iov[2] = {
      {
        .iov_base = packet + ANT_VIRTIO_NET_HDR_LEN,
        .iov_len = payload_len,
      },
      {
        .iov_base = (void *)pad,
        .iov_len = payload_len < 60u ? 60u - payload_len : 0u,
      },
    };
    struct vmpktdesc vmnet_packet = {
      .vm_pkt_size = payload_len + iov[1].iov_len,
      .vm_pkt_iov = iov,
      .vm_pkt_iovcnt = iov[1].iov_len ? 2u : 1u,
      .vm_flags = 0,
    };
    int pktcnt = 1;
    vmnet_return_t net_rc = vmnet_write(vm->net_iface, &vmnet_packet, &pktcnt);
    if (net_rc != VMNET_SUCCESS || pktcnt != 1) return -EIO;
  }

  if (vm->trace) {
    fprintf(stderr, "sandbox vm: net tx complete head=%u used_len=%u\n", head, total);
  }

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
    rc = ant_hvf_guest_read(vm, avail_base + 4u + (uint64_t)ring_slot * 2u,
                            head_raw, sizeof(head_raw));
    if (rc != 0) return rc;
    uint16_t head = ant_hvf_load16(head_raw);

    uint32_t used_len = 0;
    rc = ant_hvf_vring_write_chain(vm,
                                   desc_base,
                                   head,
                                   q->size,
                                   guest_packet,
                                   guest_len,
                                   &used_len);
    if (rc != 0) return rc;

    rc = ant_hvf_vring_add_used(vm, used_base, q->size, head, used_len);
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

  if (delivered) {
    return ant_hvf_virtio_interrupt(vm, dev, ANT_VIRTIO_NET_RX_QUEUE);
  }
  return 0;
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

  if (vm->trace) {
    fprintf(stderr,
            "sandbox vm: net notify queue=%u avail=%u last=%u desc=0x%llx used=0x%llx\n",
            queue,
            avail_idx,
            q->last_avail,
            (unsigned long long)desc_base,
            (unsigned long long)used_base);
  }

  if (queue == ANT_VIRTIO_NET_RX_QUEUE) {
    return ant_hvf_virtio_net_drain_rx(vm);
  }

  while (q->last_avail != avail_idx) {
    uint16_t ring_slot = q->last_avail % q->size;
    unsigned char head_raw[2];
    rc = ant_hvf_guest_read(vm, avail_base + 4u + (uint64_t)ring_slot * 2u,
                            head_raw, sizeof(head_raw));
    if (rc != 0) return rc;
    uint16_t head = ant_hvf_load16(head_raw);

    uint32_t used_len = 0;
    rc = ant_hvf_virtio_net_tx(vm, desc_base, head, &used_len);
    if (rc != 0) return rc;

    rc = ant_hvf_vring_add_used(vm, used_base, q->size, head, used_len);
    if (rc != 0) return rc;

    q->last_avail++;
  }

  return ant_hvf_virtio_interrupt(vm, dev, queue);
}

#endif
