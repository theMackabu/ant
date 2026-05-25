#include "sandbox_backend/backend.h" // IWYU pragma: keep

#if defined(__linux__)
#  include <sys/random.h>
#elif defined(__APPLE__)
#  include <stdlib.h>
#endif

static int ant_hvf_entropy_fill(void *buf, size_t len) {
  unsigned char *p = buf;
#if defined(__APPLE__)
  arc4random_buf(buf, len);
  return 0;
#elif defined(__linux__)
  while (len > 0) {
    ssize_t n = getrandom(p, len, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == ENOSYS) break;
      return -errno;
    }
    if (n == 0) break;
    p += (size_t)n;
    len -= (size_t)n;
  }
  if (len == 0) return 0;
#endif

  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -errno;
  while (len > 0) {
    ssize_t n = read(fd, p, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      int rc = -errno;
      close(fd);
      return rc;
    }
    if (n == 0) {
      close(fd);
      return -EIO;
    }
    p += (size_t)n;
    len -= (size_t)n;
  }
  close(fd);
  return 0;
}

static int ant_hvf_virtio_rng_request(ant_hvf_vm_t *vm,
                                      uint64_t desc_base,
                                      uint16_t queue_size,
                                      uint16_t head,
                                      uint32_t *used_len) {
  if (queue_size == 0 || head >= queue_size) return -EINVAL;

  bool visited[ANT_VIRTIO_RNG_QUEUE_SIZE] = {0};
  if (queue_size > ANT_VIRTIO_RNG_QUEUE_SIZE) return -EINVAL;

  uint16_t index = head;
  uint32_t total = 0;
  for (unsigned chain = 0; chain < queue_size; chain++) {
    if (index >= queue_size || visited[index]) return -EINVAL;
    visited[index] = true;

    ant_vring_desc_t desc;
    int rc = ant_hvf_vring_read_desc(vm, desc_base, index, &desc);
    if (rc != 0) return rc;
    if (!(desc.flags & ANT_VRING_DESC_F_WRITE)) return -EINVAL;
    if (desc.len > UINT32_MAX - total) return -EOVERFLOW;

    void *guest = ant_hvf_guest_ptr(vm, desc.addr, desc.len);
    if (!guest) return -EFAULT;
    rc = ant_hvf_entropy_fill(guest, desc.len);
    if (rc != 0) return rc;
    total += desc.len;

    if (!(desc.flags & ANT_VRING_DESC_F_NEXT)) {
      *used_len = total;
      return 0;
    }
    index = desc.next;
  }

  return -ELOOP;
}

int ant_hvf_virtio_rng_notify(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev) {
  ant_hvf_virtio_queue_t *q = &dev->queues[0];
  if (!q->enabled || !q->desc || !q->avail || !q->used) return 0;

  unsigned char idx_raw[ANT_HVF_BYTES_U16];
  int rc = ant_hvf_guest_read(vm, q->avail + 2, idx_raw, sizeof(idx_raw));
  if (rc != 0) return rc;
  uint16_t avail_idx = ant_hvf_load16(idx_raw);

  while (q->last_avail != avail_idx) {
    uint16_t ring_slot = q->last_avail % q->size;
    unsigned char head_raw[ANT_HVF_BYTES_U16];
    rc = ant_hvf_guest_read(vm, q->avail + 4u + (uint64_t)ring_slot * 2u,
                            head_raw, sizeof(head_raw));
    if (rc != 0) return rc;
    uint16_t head = ant_hvf_load16(head_raw);
    if (head >= q->size) return -EINVAL;

    uint32_t used_len = 0;
    rc = ant_hvf_virtio_rng_request(vm, q->desc, q->size, head, &used_len);
    if (rc != 0) return rc;
    rc = ant_hvf_vring_add_used(vm, q->used, q->size, head, used_len);
    if (rc != 0) return rc;

    q->last_avail++;
  }

  return ant_hvf_virtio_interrupt(vm, dev, 0);
}
