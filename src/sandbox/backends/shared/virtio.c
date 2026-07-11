#include "sandbox_backend/backend.h"  // IWYU pragma: keep

void ant_hvf_virtio_init(
  ant_hvf_virtio_device_t *dev,
  ant_hvf_virtio_kind_t kind,
  const char *name,
  uint16_t virtio_id,
  uint16_t subsystem_id,
  uint8_t slot,
  uint8_t class_code,
  uint8_t subclass,
  uint32_t bar0,
  uint64_t features,
  uint16_t queue_count,
  uint16_t queue_size,
  uint16_t device_config_len
) {
  memset(dev, 0, sizeof(*dev));
  dev->kind = kind;
  dev->name = name;
  dev->virtio_id = virtio_id;
  dev->subsystem_id = subsystem_id;
  dev->slot = slot;
  dev->class_code = class_code;
  dev->subclass = subclass;
  dev->bar0 = bar0;
  dev->device_features = features | ANT_VIRTIO_F_VERSION_1;
  dev->config_msix_vector = ANT_VIRTIO_MSI_NO_VECTOR;
  dev->queue_count = queue_count;
  dev->device_config_len = device_config_len;
  for (uint16_t i = 0; i < queue_count && i < ANT_VIRTIO_VSOCK_QUEUE_COUNT; i++) {
    dev->queues[i].size = queue_size;
    dev->queues[i].msix_vector = ANT_VIRTIO_MSI_NO_VECTOR;
    dev->queues[i].notify_off = i;
  }
  for (unsigned i = 0; i < ANT_HVF_VIRTIO_MSIX_VECTOR_COUNT; i++) {
    dev->msix[i].vector_control = ANT_PCI_MSIX_ENTRY_MASKED;
  }
}

ant_hvf_virtio_device_t *ant_hvf_virtio_for_bar(ant_hvf_vm_t *vm, uint64_t addr) {
  ant_hvf_virtio_device_t *devices[] = {
    &vm->blk,
    vm->net_enabled ? &vm->net : NULL,
    &vm->vsock.virtio,
    &vm->rng,
  };
  for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
    ant_hvf_virtio_device_t *dev = devices[i];
    if (!dev || dev->bar0 == UINT32_MAX) continue;
    if (addr >= dev->bar0 && addr < (uint64_t)dev->bar0 + ANT_HVF_VIRTIO_BAR_SIZE) return dev;
  }
  for (size_t i = 0; i < vm->p9_count; i++) {
    ant_hvf_virtio_device_t *dev = &vm->p9[i].virtio;
    if (dev->bar0 == UINT32_MAX) continue;
    if (addr >= dev->bar0 && addr < (uint64_t)dev->bar0 + ANT_HVF_VIRTIO_BAR_SIZE) return dev;
  }
  return NULL;
}

void ant_hvf_virtio_reset(ant_hvf_virtio_device_t *dev) {
  dev->driver_features = 0;
  dev->device_feature_select = 0;
  dev->driver_feature_select = 0;
  dev->config_msix_vector = ANT_VIRTIO_MSI_NO_VECTOR;
  dev->queue_sel = 0;
  dev->status = 0;
  dev->isr = 0;
  for (uint16_t i = 0; i < dev->queue_count && i < ANT_VIRTIO_VSOCK_QUEUE_COUNT; i++) {
    dev->queues[i].desc = 0;
    dev->queues[i].avail = 0;
    dev->queues[i].used = 0;
    dev->queues[i].last_avail = 0;
    dev->queues[i].enabled = false;
    dev->queues[i].msix_vector = ANT_VIRTIO_MSI_NO_VECTOR;
  }
}

ant_hvf_virtio_queue_t *ant_hvf_virtio_selected_queue(ant_hvf_virtio_device_t *dev) {
  if (dev->queue_sel >= dev->queue_count || dev->queue_sel >= ANT_VIRTIO_VSOCK_QUEUE_COUNT) return NULL;
  return &dev->queues[dev->queue_sel];
}

void ant_hvf_virtio_common_bytes(ant_hvf_virtio_device_t *dev, unsigned char out[ANT_HVF_VIRTIO_COMMON_CFG_SIZE]) {
  memset(out, 0, ANT_HVF_VIRTIO_COMMON_CFG_SIZE);
  ant_hvf_virtio_queue_t *q = ant_hvf_virtio_selected_queue(dev);
  uint32_t selected_features = dev->device_feature_select == 0 ?
                               (uint32_t)dev->device_features :
                               (uint32_t)(dev->device_features >> 32);
  uint32_t selected_driver_features = dev->driver_feature_select == 0 ?
                                      (uint32_t)dev->driver_features :
                                      (uint32_t)(dev->driver_features >> 32);
  ant_hvf_store32(out + 0, dev->device_feature_select);
  ant_hvf_store32(out + 4, selected_features);
  ant_hvf_store32(out + 8, dev->driver_feature_select);
  ant_hvf_store32(out + 12, selected_driver_features);
  ant_hvf_store16(out + 16, dev->config_msix_vector);
  ant_hvf_store16(out + 18, dev->queue_count);
  out[20] = dev->status;
  out[21] = dev->config_generation;
  ant_hvf_store16(out + 22, dev->queue_sel);
  ant_hvf_store16(out + 24, q ? q->size : 0);
  ant_hvf_store16(out + 26, q ? q->msix_vector : ANT_VIRTIO_MSI_NO_VECTOR);
  ant_hvf_store16(out + 28, q && q->enabled ? 1 : 0);
  ant_hvf_store16(out + 30, q ? q->notify_off : 0);
  ant_hvf_store64(out + 32, q ? q->desc : 0);
  ant_hvf_store64(out + 40, q ? q->avail : 0);
  ant_hvf_store64(out + 48, q ? q->used : 0);
}

bool ant_hvf_virtio_device_config_read(ant_hvf_vm_t *vm,
                                              ant_hvf_virtio_device_t *dev,
                                              uint64_t off,
                                              unsigned size,
                                              uint64_t *value) {
  unsigned char cfg[ANT_HVF_VIRTIO_DEVICE_CONFIG_BYTES];
  memset(cfg, 0, sizeof(cfg));

  switch (dev->kind) {
    case ANT_HVF_VIRTIO_KIND_BLOCK:
      ant_hvf_virtio_blk_config(vm, cfg, sizeof(cfg));
      break;
    case ANT_HVF_VIRTIO_KIND_NET:
      memcpy(cfg, vm->net_mac, sizeof(vm->net_mac));
      break;
    case ANT_HVF_VIRTIO_KIND_9P: {
      ant_hvf_9p_device_t *p9 = ant_hvf_p9_for_virtio(vm, dev);
      if (!p9) return false;
      uint16_t tag_len = (uint16_t)strlen(p9->tag);
      ant_hvf_store16(cfg, tag_len);
      memcpy(cfg + 2, p9->tag, tag_len < sizeof(cfg) - 2 ? tag_len : sizeof(cfg) - 2);
      break;
    }
    case ANT_HVF_VIRTIO_KIND_VSOCK:
      ant_hvf_store64(cfg, ANT_HVF_VSOCK_GUEST_CID);
      break;
    case ANT_HVF_VIRTIO_KIND_RNG:
      break;
  }

  if (off >= sizeof(cfg)) {
    *value = 0;
    return true;
  }
  uint64_t word = 0;
  for (unsigned i = 0; i < size && off + i < sizeof(cfg); i++) {
    word |= (uint64_t)cfg[off + i] << (i * 8u);
  }
  *value = word;
  return true;
}

bool ant_hvf_virtio_msix_enabled(ant_hvf_virtio_device_t *dev) {
  return (dev->msix_control & ANT_PCI_MSIX_ENABLE) != 0 &&
         (dev->msix_control & ANT_PCI_MSIX_MASK_ALL) == 0;
}

bool ant_hvf_virtio_msix_masked(ant_hvf_virtio_device_t *dev, unsigned vector) {
  if (vector >= ANT_HVF_VIRTIO_MSIX_VECTOR_COUNT) return true;
  return !ant_hvf_virtio_msix_enabled(dev) ||
         (dev->msix[vector].vector_control & ANT_PCI_MSIX_ENTRY_MASKED) != 0;
}

int ant_hvf_virtio_msix_notify(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev, uint16_t vector) {
  if (vector == ANT_VIRTIO_MSI_NO_VECTOR || vector >= ANT_HVF_VIRTIO_MSIX_VECTOR_COUNT) return 0;
  if (ant_hvf_virtio_msix_masked(dev, vector)) {
    dev->msix_pba |= 1u << vector;
    return 0;
  }
  uint64_t addr = (uint64_t)dev->msix[vector].msg_addr_lo |
                  ((uint64_t)dev->msix[vector].msg_addr_hi << 32);
  return ant_hvf_send_msi(vm, addr, dev->msix[vector].msg_data);
}

int ant_hvf_virtio_interrupt(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev, unsigned queue) {
  uint16_t vector = ANT_VIRTIO_MSI_NO_VECTOR;
  if (queue < dev->queue_count && queue < ANT_VIRTIO_VSOCK_QUEUE_COUNT) {
    vector = dev->queues[queue].msix_vector;
  }
  dev->isr |= 1u;
  return ant_hvf_virtio_msix_notify(vm, dev, vector);
}

bool ant_hvf_virtio_msix_read(ant_hvf_virtio_device_t *dev,
                                     uint64_t off,
                                     unsigned size,
                                     uint64_t *value) {
  if (off >= ANT_HVF_VIRTIO_MSIX_TABLE &&
      off < ANT_HVF_VIRTIO_MSIX_TABLE + ANT_HVF_VIRTIO_MSIX_VECTOR_COUNT * ANT_HVF_VIRTIO_MSIX_ENTRY_SIZE) {
    unsigned vector = (unsigned)((off - ANT_HVF_VIRTIO_MSIX_TABLE) / ANT_HVF_VIRTIO_MSIX_ENTRY_SIZE);
    unsigned entry_off = (unsigned)((off - ANT_HVF_VIRTIO_MSIX_TABLE) % ANT_HVF_VIRTIO_MSIX_ENTRY_SIZE);
    uint32_t word = 0;
    switch (entry_off & ~3u) {
      case 0: word = dev->msix[vector].msg_addr_lo; break;
      case 4: word = dev->msix[vector].msg_addr_hi; break;
      case 8: word = dev->msix[vector].msg_data; break;
      case 12: word = dev->msix[vector].vector_control; break;
      default: word = 0; break;
    }
    *value = ant_hvf_select_width(word, entry_off & 3u, size);
    return true;
  }
  if (off >= ANT_HVF_VIRTIO_MSIX_PBA && off < ANT_HVF_VIRTIO_MSIX_PBA + 8u) {
    *value = ant_hvf_select_width(dev->msix_pba, (unsigned)(off - ANT_HVF_VIRTIO_MSIX_PBA), size);
    return true;
  }
  return false;
}

bool ant_hvf_virtio_msix_write(ant_hvf_vm_t *vm,
                                      ant_hvf_virtio_device_t *dev,
                                      uint64_t off,
                                      unsigned size,
                                      uint64_t value) {
  if (off < ANT_HVF_VIRTIO_MSIX_TABLE ||
      off >= ANT_HVF_VIRTIO_MSIX_TABLE + ANT_HVF_VIRTIO_MSIX_VECTOR_COUNT * ANT_HVF_VIRTIO_MSIX_ENTRY_SIZE) {
    return false;
  }

  unsigned vector = (unsigned)((off - ANT_HVF_VIRTIO_MSIX_TABLE) / ANT_HVF_VIRTIO_MSIX_ENTRY_SIZE);
  unsigned entry_off = (unsigned)((off - ANT_HVF_VIRTIO_MSIX_TABLE) % ANT_HVF_VIRTIO_MSIX_ENTRY_SIZE);
  bool was_masked = ant_hvf_virtio_msix_masked(dev, vector);
  switch (entry_off & ~3u) {
    case 0: ant_hvf_assign_width(&dev->msix[vector].msg_addr_lo, entry_off & 3u, size, value); break;
    case 4: ant_hvf_assign_width(&dev->msix[vector].msg_addr_hi, entry_off & 3u, size, value); break;
    case 8: ant_hvf_assign_width(&dev->msix[vector].msg_data, entry_off & 3u, size, value); break;
    case 12: ant_hvf_assign_width(&dev->msix[vector].vector_control, entry_off & 3u, size, value); break;
    default: break;
  }
  bool is_masked = ant_hvf_virtio_msix_masked(dev, vector);
  if (was_masked && !is_masked && (dev->msix_pba & (1u << vector))) {
    dev->msix_pba &= ~(1u << vector);
    ant_hvf_virtio_msix_notify(vm, dev, (uint16_t)vector);
  }
  return true;
}

bool ant_hvf_virtio_common_read(ant_hvf_vm_t *vm,
                                       ant_hvf_virtio_device_t *dev,
                                       uint64_t off,
                                       unsigned size,
                                       uint64_t *value) {
  if (off < ANT_HVF_VIRTIO_COMMON_CFG_SIZE) {
    unsigned char common[ANT_HVF_VIRTIO_COMMON_CFG_SIZE];
    ant_hvf_virtio_common_bytes(dev, common);
    uint64_t word = 0;
    for (unsigned i = 0; i < size && off + i < sizeof(common); i++) {
      word |= (uint64_t)common[off + i] << (i * 8u);
    }
    *value = word;
    return true;
  }
  if (off == ANT_HVF_VIRTIO_ISR_CFG) {
    *value = dev->isr;
    dev->isr = 0;
    return true;
  }
  if (off >= ANT_HVF_VIRTIO_DEVICE_CFG && off < ANT_HVF_VIRTIO_DEVICE_CFG + dev->device_config_len) {
    return ant_hvf_virtio_device_config_read(vm, dev, off - ANT_HVF_VIRTIO_DEVICE_CFG, size, value);
  }
  if (ant_hvf_virtio_msix_read(dev, off, size, value)) return true;
  if (off >= ANT_HVF_VIRTIO_NOTIFY_CFG && off < ANT_HVF_VIRTIO_NOTIFY_CFG + (uint64_t)dev->queue_count * 2u) {
    *value = 0;
    return true;
  }
  *value = 0;
  return true;
}

bool ant_hvf_virtio_common_write(ant_hvf_vm_t *vm,
                                        ant_hvf_virtio_device_t *dev,
                                        uint64_t off,
                                        unsigned size,
                                        uint64_t value) {
  if (ant_hvf_virtio_msix_write(vm, dev, off, size, value)) return true;

  if (off >= ANT_HVF_VIRTIO_NOTIFY_CFG && off < ANT_HVF_VIRTIO_NOTIFY_CFG + (uint64_t)dev->queue_count * 2u) {
    unsigned queue = (unsigned)((off - ANT_HVF_VIRTIO_NOTIFY_CFG) / 2u);
    switch (dev->kind) {
      case ANT_HVF_VIRTIO_KIND_BLOCK:
        ant_hvf_virtio_blk_notify(vm, dev);
        break;
      case ANT_HVF_VIRTIO_KIND_NET:
        ant_hvf_virtio_net_notify(vm, dev, queue);
        break;
      case ANT_HVF_VIRTIO_KIND_9P:
        {
          ant_hvf_9p_device_t *p9 = ant_hvf_p9_for_virtio(vm, dev);
          if (p9) ant_hvf_virtio_9p_notify(vm, p9);
        }
        break;
      case ANT_HVF_VIRTIO_KIND_VSOCK:
        ant_hvf_virtio_vsock_notify(vm, queue);
        break;
      case ANT_HVF_VIRTIO_KIND_RNG:
        ant_hvf_virtio_rng_notify(vm, dev);
        break;
    }
    return true;
  }

  if (off >= ANT_HVF_VIRTIO_DEVICE_CFG && off < ANT_HVF_VIRTIO_DEVICE_CFG + dev->device_config_len) {
    return true;
  }

  if (off >= ANT_HVF_VIRTIO_COMMON_CFG_SIZE) return true;

  ant_hvf_virtio_queue_t *q = ant_hvf_virtio_selected_queue(dev);
  switch (off) {
    case 0:
      dev->device_feature_select = (uint32_t)value;
      return true;
    case 8:
      dev->driver_feature_select = (uint32_t)value;
      return true;
    case 12:
      if (dev->driver_feature_select == 0) {
        ant_hvf_assign_width64(&dev->driver_features, 0, 4, value);
      } else if (dev->driver_feature_select == 1) {
        uint64_t high = value;
        dev->driver_features = (dev->driver_features & UINT32_MAX) | (high << 32);
      }
      return true;
    case 16:
      dev->config_msix_vector = (uint16_t)value;
      return true;
    case 20:
      if ((uint8_t)value == 0) {
        ant_hvf_virtio_reset(dev);
        if (dev->kind == ANT_HVF_VIRTIO_KIND_VSOCK) {
          vm->vsock.connected = false;
          atomic_store_explicit(&vm->vsock.request_sent, false, memory_order_release);
          vm->vsock.peer_port = 0;
          vm->vsock.fwd_cnt = 0;
          vm->vsock.exit_received = false;
          vm->vsock.exit_code = 0;
          vm->vsock.rx_stream_len = 0;
        } else if (dev->kind == ANT_HVF_VIRTIO_KIND_9P) {
          ant_hvf_9p_device_t *p9 = ant_hvf_p9_for_virtio(vm, dev);
          if (p9 && p9->fids) memset(p9->fids, 0, p9->fid_count * sizeof(*p9->fids));
        }
      } else {
        dev->status = (uint8_t)value;
      }
      return true;
    case 22:
      dev->queue_sel = (uint16_t)value;
      return true;
    case 24:
      if (q) q->size = (uint16_t)value;
      return true;
    case 26:
      if (q) q->msix_vector = (uint16_t)value;
      return true;
    case 28:
      if (q) q->enabled = ((uint16_t)value) != 0;
      return true;
    default:
      if (q && off >= 32 && off < 40) {
        ant_hvf_assign_width64(&q->desc, (unsigned)(off - 32), size, value);
      } else if (q && off >= 40 && off < 48) {
        ant_hvf_assign_width64(&q->avail, (unsigned)(off - 40), size, value);
      } else if (q && off >= 48 && off < 56) {
        ant_hvf_assign_width64(&q->used, (unsigned)(off - 48), size, value);
      }
      return true;
  }
}
int ant_hvf_vring_add_used(ant_hvf_vm_t *vm,
                                  uint64_t used_base,
                                  unsigned queue_size,
                                  uint16_t head,
                                  uint32_t used_len) {
  unsigned char used_idx_raw[ANT_HVF_BYTES_U16];
  int rc = ant_hvf_guest_read(vm, used_base + 2, used_idx_raw, sizeof(used_idx_raw));
  if (rc != 0) return rc;
  uint16_t used_idx = ant_hvf_load16(used_idx_raw);
  uint64_t elem = used_base + 4u + (uint64_t)(used_idx % queue_size) * 8u;
  unsigned char used_elem[ANT_HVF_VRING_USED_ELEM_BYTES];
  ant_hvf_store32(used_elem, head);
  ant_hvf_store32(used_elem + 4, used_len);
  rc = ant_hvf_guest_write(vm, elem, used_elem, sizeof(used_elem));
  if (rc != 0) return rc;
  ant_hvf_store16(used_idx_raw, (uint16_t)(used_idx + 1));
  return ant_hvf_guest_write(vm, used_base + 2, used_idx_raw, sizeof(used_idx_raw));
}

int ant_hvf_vring_read_chain(ant_hvf_vm_t *vm,
                                    uint64_t desc_base,
                                    uint16_t head,
                                    unsigned queue_size,
                                    unsigned char *out,
                                    uint32_t out_cap,
                                    uint32_t *out_len) {
  uint16_t index = head;
  uint32_t total = 0;

  for (unsigned chain = 0; chain < queue_size; chain++) {
    ant_vring_desc_t desc;
    int rc = ant_hvf_vring_read_desc(vm, desc_base, index, &desc);
    if (rc != 0) return rc;
    if (desc.flags & ANT_VRING_DESC_F_WRITE) return -EINVAL;
    if (desc.len > out_cap - total) return -ENOSPC;
    rc = ant_hvf_guest_read(vm, desc.addr, out + total, desc.len);
    if (rc != 0) return rc;
    total += desc.len;
    if (!(desc.flags & ANT_VRING_DESC_F_NEXT)) {
      *out_len = total;
      return 0;
    }
    index = desc.next;
  }

  return -ELOOP;
}

int ant_hvf_vring_write_chain(
  ant_hvf_vm_t *vm,
  uint64_t desc_base,
  uint16_t head,
  unsigned queue_size,
  const unsigned char *data,
  uint32_t len,
  uint32_t *used_len
) {
  uint16_t index = head;
  uint32_t done = 0;

  for (unsigned chain = 0; chain < queue_size && done < len; chain++) {
    ant_vring_desc_t desc;
    int rc = ant_hvf_vring_read_desc(vm, desc_base, index, &desc);
    if (rc != 0) return rc;
    if (!(desc.flags & ANT_VRING_DESC_F_WRITE)) return -EINVAL;

    uint32_t chunk = len - done;
    if (chunk > desc.len) chunk = desc.len;
    rc = ant_hvf_guest_write(vm, desc.addr, data + done, chunk);
    if (rc != 0) return rc;
    done += chunk;

    if (done == len) {
      *used_len = done;
      return 0;
    }
    if (!(desc.flags & ANT_VRING_DESC_F_NEXT)) break;
    index = desc.next;
  }

  return -ENOSPC;
}
