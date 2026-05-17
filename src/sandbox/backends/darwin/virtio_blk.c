#include "backend.h"

#if defined(__aarch64__)

int ant_hvf_vring_read_desc(ant_hvf_vm_t *vm, uint64_t desc_base, uint16_t index, ant_vring_desc_t *out) {
  unsigned char raw[16];
  int rc = ant_hvf_guest_read(vm, desc_base + (uint64_t)index * sizeof(raw), raw, sizeof(raw));
  if (rc != 0) return rc;
  out->addr = ant_hvf_load64(raw);
  out->len = ant_hvf_load32(raw + 8);
  out->flags = ant_hvf_load16(raw + 12);
  out->next = ant_hvf_load16(raw + 14);
  return 0;
}

int ant_hvf_disk_read(ant_hvf_vm_t *vm, uint64_t sector, uint64_t guest_addr, uint32_t len) {
  void *dest = ant_hvf_guest_ptr(vm, guest_addr, len);
  if (!dest) return -EFAULT;
  off_t off = (off_t)(sector * 512ull);
  size_t done = 0;
  while (done < len) {
    ssize_t n = pread(vm->image_fd, (unsigned char *)dest + done, len - done, off + (off_t)done);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -errno;
    }
    if (n == 0) {
      memset((unsigned char *)dest + done, 0, len - done);
      return 0;
    }
    done += (size_t)n;
  }
  return 0;
}

int ant_hvf_disk_write(ant_hvf_vm_t *vm, uint64_t sector, uint64_t guest_addr, uint32_t len) {
  void *src = ant_hvf_guest_ptr(vm, guest_addr, len);
  if (!src) return -EFAULT;
  off_t off = (off_t)(sector * 512ull);
  size_t done = 0;
  while (done < len) {
    ssize_t n = pwrite(vm->image_fd, (unsigned char *)src + done, len - done, off + (off_t)done);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -errno;
    }
    done += (size_t)n;
  }
  return 0;
}

int ant_hvf_virtio_blk_request(ant_hvf_vm_t *vm,
                                      uint64_t desc_base,
                                      uint16_t head,
                                      uint32_t *used_len) {
  ant_vring_desc_t desc;
  int rc = ant_hvf_vring_read_desc(vm, desc_base, head, &desc);
  if (rc != 0) return rc;
  if (!(desc.flags & ANT_VRING_DESC_F_NEXT)) return -EINVAL;

  unsigned char req_raw[16];
  if (desc.len < sizeof(req_raw)) return -EINVAL;
  rc = ant_hvf_guest_read(vm, desc.addr, req_raw, sizeof(req_raw));
  if (rc != 0) return rc;

  ant_virtio_blk_req_t req = {
    .type = ant_hvf_load32(req_raw),
    .reserved = ant_hvf_load32(req_raw + 4),
    .sector = ant_hvf_load64(req_raw + 8),
  };
  (void)req.reserved;

  if (vm->trace) {
    fprintf(stderr, "sandbox vm: blk req head=%u type=%u sector=%llu\n",
            head, req.type, (unsigned long long)req.sector);
  }

  uint8_t status = ANT_VIRTIO_BLK_S_OK;
  uint32_t total = 0;
  uint16_t next = desc.next;
  ant_vring_desc_t status_desc;
  memset(&status_desc, 0, sizeof(status_desc));

  for (unsigned chain = 0; chain < ANT_VIRTIO_BLK_QUEUE_SIZE; chain++) {
    rc = ant_hvf_vring_read_desc(vm, desc_base, next, &desc);
    if (rc != 0) return rc;

    if (!(desc.flags & ANT_VRING_DESC_F_NEXT)) {
      status_desc = desc;
      break;
    }

    if (req.type == ANT_VIRTIO_BLK_T_IN) {
      rc = ant_hvf_disk_read(vm, req.sector + total / 512u, desc.addr, desc.len);
    } else if (req.type == ANT_VIRTIO_BLK_T_OUT) {
      rc = ant_hvf_disk_write(vm, req.sector + total / 512u, desc.addr, desc.len);
    } else if (req.type == ANT_VIRTIO_BLK_T_FLUSH) {
      rc = fsync(vm->image_fd) == 0 ? 0 : -errno;
    } else {
      rc = -ENOTSUP;
    }

    if (rc != 0) {
      status = rc == -ENOTSUP ? 2u : ANT_VIRTIO_BLK_S_IOERR;
    }
    total += desc.len;
    next = desc.next;
  }

  if (!status_desc.addr || status_desc.len < 1) return -EINVAL;
  rc = ant_hvf_guest_write(vm, status_desc.addr, &status, 1);
  if (rc != 0) return rc;

  *used_len = (req.type == ANT_VIRTIO_BLK_T_IN ? total : 0) + 1u;
  if (vm->trace) {
    fprintf(stderr, "sandbox vm: blk complete head=%u status=%u used_len=%u\n",
            head, status, *used_len);
  }
  return 0;
}

int ant_hvf_virtio_blk_notify(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev) {
  ant_hvf_virtio_queue_t *q = &dev->queues[0];
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
            "sandbox vm: blk notify avail=%u last=%u desc=0x%llx used=0x%llx\n",
            avail_idx,
            q->last_avail,
            (unsigned long long)desc_base,
            (unsigned long long)used_base);
  }

  while (q->last_avail != avail_idx) {
    uint16_t ring_slot = q->last_avail % q->size;
    unsigned char head_raw[2];
    rc = ant_hvf_guest_read(vm, avail_base + 4u + (uint64_t)ring_slot * 2u,
                            head_raw, sizeof(head_raw));
    if (rc != 0) return rc;
    uint16_t head = ant_hvf_load16(head_raw);

    uint32_t used_len = 0;
    rc = ant_hvf_virtio_blk_request(vm, desc_base, head, &used_len);
    if (rc != 0) return rc;

    rc = ant_hvf_vring_add_used(vm, used_base, q->size, head, used_len);
    if (rc != 0) return rc;

    q->last_avail++;
  }

  return ant_hvf_virtio_interrupt(vm, dev, 0);
}

#endif
