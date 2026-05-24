#include "sandbox_backend/backend.h"


void ant_hvf_virtio_blk_config(const ant_hvf_vm_t *vm, unsigned char *cfg, size_t cfg_len) {
  if (!cfg || cfg_len < ANT_VIRTIO_BLK_CONFIG_LEN) return;
  ant_hvf_store64(cfg, vm->image_sectors);
  ant_hvf_store32(cfg + 12, ANT_VIRTIO_BLK_SEG_MAX);
  ant_hvf_store32(cfg + 20, ANT_VIRTIO_BLK_SECTOR_SIZE);
}

int ant_hvf_vring_read_desc(ant_hvf_vm_t *vm, uint64_t desc_base, uint16_t index, ant_vring_desc_t *out) {
  unsigned char raw[ANT_VIRTIO_BLK_DESC_BYTES];
  int rc = ant_hvf_guest_read(vm, desc_base + (uint64_t)index * sizeof(raw), raw, sizeof(raw));
  if (rc != 0) return rc;
  out->addr = ant_hvf_load64(raw);
  out->len = ant_hvf_load32(raw + 8);
  out->flags = ant_hvf_load16(raw + 12);
  out->next = ant_hvf_load16(raw + 14);
  return 0;
}

static int ant_hvf_disk_check_bounds(ant_hvf_vm_t *vm, uint64_t sector, uint32_t len, off_t *off_out) {
  if (len % ANT_VIRTIO_BLK_SECTOR_SIZE != 0) return -EINVAL;
  if (sector > UINT64_MAX / ANT_VIRTIO_BLK_SECTOR_SIZE) return -EOVERFLOW;
  uint64_t byte_off = sector * ANT_VIRTIO_BLK_SECTOR_SIZE;
  if ((uint64_t)len > UINT64_MAX - byte_off) return -EOVERFLOW;
  uint64_t end = byte_off + (uint64_t)len;
  if (vm->image_sectors > UINT64_MAX / ANT_VIRTIO_BLK_SECTOR_SIZE) return -EOVERFLOW;
  uint64_t image_bytes = vm->image_sectors * ANT_VIRTIO_BLK_SECTOR_SIZE;
  if (end > image_bytes) return -EIO;
  if (byte_off > (uint64_t)INT64_MAX) return -EOVERFLOW;
  *off_out = (off_t)byte_off;
  return 0;
}

int ant_hvf_disk_read(ant_hvf_vm_t *vm, uint64_t sector, uint64_t guest_addr, uint32_t len) {
  void *dest = ant_hvf_guest_ptr(vm, guest_addr, len);
  if (!dest) return -EFAULT;
  off_t off = 0;
  int rc = ant_hvf_disk_check_bounds(vm, sector, len, &off);
  if (rc != 0) return rc;
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
  off_t off = 0;
  int rc = ant_hvf_disk_check_bounds(vm, sector, len, &off);
  if (rc != 0) return rc;
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
                                      uint16_t queue_size,
                                      uint16_t head,
                                      uint32_t *used_len) {
  if (queue_size == 0 || head >= queue_size) return -EINVAL;
  ant_vring_desc_t desc;
  int rc = ant_hvf_vring_read_desc(vm, desc_base, head, &desc);
  if (rc != 0) return rc;
  if (desc.flags & ANT_VRING_DESC_F_WRITE) return -EINVAL;
  if (!(desc.flags & ANT_VRING_DESC_F_NEXT)) return -EINVAL;
  if (desc.next >= queue_size) return -EINVAL;

  unsigned char req_raw[ANT_VIRTIO_BLK_REQUEST_BYTES];
  if (desc.len < sizeof(req_raw)) return -EINVAL;
  rc = ant_hvf_guest_read(vm, desc.addr, req_raw, sizeof(req_raw));
  if (rc != 0) return rc;

  ant_virtio_blk_req_t req = {
    .type = ant_hvf_load32(req_raw),
    .reserved = ant_hvf_load32(req_raw + 4),
    .sector = ant_hvf_load64(req_raw + 8),
  };
  (void)req.reserved;

  uint8_t status = ANT_VIRTIO_BLK_S_OK;
  uint16_t next = desc.next;
  ant_vring_desc_t status_desc;
  memset(&status_desc, 0, sizeof(status_desc));
  bool saw_data = false;
  bool visited[ANT_VIRTIO_BLK_QUEUE_SIZE] = {0};
  uint64_t total = 0;

  if (queue_size > ANT_VIRTIO_BLK_QUEUE_SIZE) return -EINVAL;
  visited[head] = true;
  for (unsigned chain = 0; chain < queue_size; chain++) {
    if (visited[next]) return -EINVAL;
    visited[next] = true;
    rc = ant_hvf_vring_read_desc(vm, desc_base, next, &desc);
    if (rc != 0) return rc;

    if (!(desc.flags & ANT_VRING_DESC_F_NEXT)) {
      status_desc = desc;
      break;
    }
    if (desc.next >= queue_size) return -EINVAL;
    saw_data = true;

    if (req.type == ANT_VIRTIO_BLK_T_IN && !(desc.flags & ANT_VRING_DESC_F_WRITE)) {
      status = ANT_VIRTIO_BLK_S_IOERR;
    } else if (req.type == ANT_VIRTIO_BLK_T_OUT && (desc.flags & ANT_VRING_DESC_F_WRITE)) {
      status = ANT_VIRTIO_BLK_S_IOERR;
    } else if (req.type == ANT_VIRTIO_BLK_T_FLUSH) {
      status = ANT_VIRTIO_BLK_S_UNSUPP;
    }

    if (status != ANT_VIRTIO_BLK_S_OK) {
      rc = 0;
    } else if (req.type == ANT_VIRTIO_BLK_T_IN) {
      if (total > UINT64_MAX - desc.len) {
        rc = -EOVERFLOW;
      } else if (req.sector > UINT64_MAX - total / ANT_VIRTIO_BLK_SECTOR_SIZE) {
        rc = -EOVERFLOW;
      } else {
        rc = ant_hvf_disk_read(vm, req.sector + total / ANT_VIRTIO_BLK_SECTOR_SIZE, desc.addr, desc.len);
      }
    } else if (req.type == ANT_VIRTIO_BLK_T_OUT) {
      if (total > UINT64_MAX - desc.len) {
        rc = -EOVERFLOW;
      } else if (req.sector > UINT64_MAX - total / ANT_VIRTIO_BLK_SECTOR_SIZE) {
        rc = -EOVERFLOW;
      } else {
        rc = ant_hvf_disk_write(vm, req.sector + total / ANT_VIRTIO_BLK_SECTOR_SIZE, desc.addr, desc.len);
      }
    } else {
      rc = -ENOTSUP;
    }

    if (rc != 0) {
      status = rc == -ENOTSUP ? ANT_VIRTIO_BLK_S_UNSUPP : ANT_VIRTIO_BLK_S_IOERR;
    }
    total += desc.len;
    next = desc.next;
  }

  if (!status_desc.addr || status_desc.len < 1 || !(status_desc.flags & ANT_VRING_DESC_F_WRITE)) {
    return -EINVAL;
  }
  if (req.type != ANT_VIRTIO_BLK_T_IN &&
      req.type != ANT_VIRTIO_BLK_T_OUT &&
      req.type != ANT_VIRTIO_BLK_T_FLUSH) {
    status = ANT_VIRTIO_BLK_S_UNSUPP;
  } else if ((req.type == ANT_VIRTIO_BLK_T_IN || req.type == ANT_VIRTIO_BLK_T_OUT) && !saw_data) {
    status = ANT_VIRTIO_BLK_S_IOERR;
  }
  if (req.type == ANT_VIRTIO_BLK_T_FLUSH) {
    if (saw_data) {
      status = ANT_VIRTIO_BLK_S_IOERR;
    } else {
      rc = fsync(vm->image_fd) == 0 ? 0 : -errno;
      if (rc != 0) status = ANT_VIRTIO_BLK_S_IOERR;
    }
  }
  rc = ant_hvf_guest_write(vm, status_desc.addr, &status, 1);
  if (rc != 0) return rc;

  uint64_t response_len = (req.type == ANT_VIRTIO_BLK_T_IN ? total : 0) + 1u;
  if (response_len > UINT32_MAX) return -EOVERFLOW;
  *used_len = (uint32_t)response_len;
  return 0;
}

int ant_hvf_virtio_blk_notify(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev) {
  ant_hvf_virtio_queue_t *q = &dev->queues[0];
  if (!q->enabled || !q->desc || !q->avail || !q->used) return 0;

  uint64_t desc_base = q->desc;
  uint64_t avail_base = q->avail;
  uint64_t used_base = q->used;

  unsigned char idx_raw[ANT_HVF_BYTES_U16];
  int rc = ant_hvf_guest_read(vm, avail_base + 2, idx_raw, sizeof(idx_raw));
  if (rc != 0) return rc;
  uint16_t avail_idx = ant_hvf_load16(idx_raw);

  while (q->last_avail != avail_idx) {
    uint16_t ring_slot = q->last_avail % q->size;
    unsigned char head_raw[ANT_HVF_BYTES_U16];
    rc = ant_hvf_guest_read(vm, avail_base + 4u + (uint64_t)ring_slot * 2u,
                            head_raw, sizeof(head_raw));
    if (rc != 0) return rc;
    uint16_t head = ant_hvf_load16(head_raw);

    uint32_t used_len = 0;
    if (head >= q->size) return -EINVAL;
    rc = ant_hvf_virtio_blk_request(vm, desc_base, q->size, head, &used_len);
    if (rc != 0) return rc;

    rc = ant_hvf_vring_add_used(vm, used_base, q->size, head, used_len);
    if (rc != 0) return rc;

    q->last_avail++;
  }

  return ant_hvf_virtio_interrupt(vm, dev, 0);
}

