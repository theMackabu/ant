#include "backend.h"

#if defined(__aarch64__)

void ant_hvf_vsock_store_hdr(unsigned char *out, const ant_virtio_vsock_hdr_t *hdr) {
  ant_hvf_store64(out, hdr->src_cid);
  ant_hvf_store64(out + 8, hdr->dst_cid);
  ant_hvf_store32(out + 16, hdr->src_port);
  ant_hvf_store32(out + 20, hdr->dst_port);
  ant_hvf_store32(out + 24, hdr->len);
  ant_hvf_store16(out + 28, hdr->type);
  ant_hvf_store16(out + 30, hdr->op);
  ant_hvf_store32(out + 32, hdr->flags);
  ant_hvf_store32(out + 36, hdr->buf_alloc);
  ant_hvf_store32(out + 40, hdr->fwd_cnt);
}

ant_virtio_vsock_hdr_t ant_hvf_vsock_load_hdr(const unsigned char *raw) {
  ant_virtio_vsock_hdr_t hdr = {
    .src_cid = ant_hvf_load64(raw),
    .dst_cid = ant_hvf_load64(raw + 8),
    .src_port = ant_hvf_load32(raw + 16),
    .dst_port = ant_hvf_load32(raw + 20),
    .len = ant_hvf_load32(raw + 24),
    .type = ant_hvf_load16(raw + 28),
    .op = ant_hvf_load16(raw + 30),
    .flags = ant_hvf_load32(raw + 32),
    .buf_alloc = ant_hvf_load32(raw + 36),
    .fwd_cnt = ant_hvf_load32(raw + 40),
  };
  return hdr;
}
int ant_hvf_vsock_write_iov(ant_hvf_vm_t *vm,
                                   uint64_t desc_base,
                                   uint16_t head,
                                   const unsigned char *data,
                                   uint32_t len,
                                   uint32_t *used_len) {
  uint16_t index = head;
  uint32_t done = 0;

  for (unsigned chain = 0; chain < ANT_VIRTIO_VSOCK_QUEUE_SIZE && done < len; chain++) {
    ant_vring_desc_t desc;
    int rc = ant_hvf_vring_read_desc(vm, desc_base, index, &desc);
    if (rc != 0) return rc;

    if (desc.flags & ANT_VRING_DESC_F_WRITE) {
      uint32_t n = desc.len;
      if (n > len - done) n = len - done;
      rc = ant_hvf_guest_write(vm, desc.addr, data + done, n);
      if (rc != 0) return rc;
      done += n;
    }

    if (!(desc.flags & ANT_VRING_DESC_F_NEXT)) break;
    index = desc.next;
  }

  if (done != len) return -ENOSPC;
  *used_len = done;
  return 0;
}

int ant_hvf_vsock_send_packet(ant_hvf_vm_t *vm,
                                     uint16_t op,
                                     const char *payload,
                                     uint32_t payload_len) {
  ant_hvf_vsock_device_t *dev = &vm->vsock;
  ant_hvf_virtio_queue_t *q = &dev->virtio.queues[0];
  if (!q->enabled || !q->desc || !q->avail || !q->used) return -EAGAIN;

  uint64_t desc_base = q->desc;
  uint64_t avail_base = q->avail;
  uint64_t used_base = q->used;

  unsigned char idx_raw[2];
  int rc = ant_hvf_guest_read(vm, avail_base + 2, idx_raw, sizeof(idx_raw));
  if (rc != 0) return rc;
  uint16_t avail_idx = ant_hvf_load16(idx_raw);
  if (q->last_avail == avail_idx) return -EAGAIN;

  uint16_t ring_slot = q->last_avail % q->size;
  unsigned char head_raw[2];
  rc = ant_hvf_guest_read(vm, avail_base + 4u + (uint64_t)ring_slot * 2u,
                          head_raw, sizeof(head_raw));
  if (rc != 0) return rc;
  uint16_t head = ant_hvf_load16(head_raw);

  unsigned char packet[4096];
  size_t hdr_len = sizeof(ant_virtio_vsock_hdr_t);
  if (hdr_len + payload_len > sizeof(packet)) return -E2BIG;

  ant_virtio_vsock_hdr_t hdr = {
    .src_cid = ANT_HVF_VSOCK_HOST_CID,
    .dst_cid = ANT_HVF_VSOCK_GUEST_CID,
    .src_port = ANT_HVF_VSOCK_HOST_PORT,
    .dst_port = dev->peer_port,
    .len = payload_len,
    .type = ANT_VIRTIO_VSOCK_TYPE_STREAM,
    .op = op,
    .flags = 0,
    .buf_alloc = ANT_HVF_VSOCK_BUF_ALLOC,
    .fwd_cnt = dev->fwd_cnt,
  };
  ant_hvf_vsock_store_hdr(packet, &hdr);
  if (payload_len > 0) memcpy(packet + hdr_len, payload, payload_len);

  uint32_t used_len = 0;
  rc = ant_hvf_vsock_write_iov(vm, desc_base, head, packet, (uint32_t)(hdr_len + payload_len), &used_len);
  if (rc != 0) return rc;
  rc = ant_hvf_vring_add_used(vm, used_base, q->size, head, used_len);
  if (rc != 0) return rc;

  q->last_avail++;
  if (vm->trace) {
    fprintf(stderr,
            "sandbox vm: vsock rx packet op=%u len=%u peer_port=%u\n",
            op,
            payload_len,
            dev->peer_port);
  }
  return ant_hvf_virtio_interrupt(vm, &dev->virtio, 0);
}

int ant_hvf_vsock_maybe_send_request(ant_hvf_vm_t *vm) {
  ant_hvf_vsock_device_t *dev = &vm->vsock;
  if (!dev->connected || dev->request_sent || !dev->request_json) return 0;

  size_t len = strlen(dev->request_json);
  if (len > 3900) return -E2BIG;
  char line[4096];
  memcpy(line, dev->request_json, len);
  line[len++] = '\n';

  int rc = ant_hvf_vsock_send_packet(vm, ANT_VIRTIO_VSOCK_OP_RW, line, (uint32_t)len);
  if (rc == 0) dev->request_sent = true;
  if (rc == -EAGAIN) return 0;
  return rc;
}

int ant_hvf_vsock_read_tx_packet(ant_hvf_vm_t *vm,
                                        uint64_t desc_base,
                                        uint16_t head,
                                        ant_virtio_vsock_hdr_t *hdr,
                                        uint32_t *used_len) {
  unsigned char raw[sizeof(ant_virtio_vsock_hdr_t)];
  uint32_t done = 0;
  uint32_t total = 0;
  uint16_t index = head;

  for (unsigned chain = 0; chain < ANT_VIRTIO_VSOCK_QUEUE_SIZE; chain++) {
    ant_vring_desc_t desc;
    int rc = ant_hvf_vring_read_desc(vm, desc_base, index, &desc);
    if (rc != 0) return rc;

    if (!(desc.flags & ANT_VRING_DESC_F_WRITE)) {
      uint32_t n = desc.len;
      if (done < sizeof(raw)) {
        uint32_t copy = n;
        if (copy > sizeof(raw) - done) copy = (uint32_t)(sizeof(raw) - done);
        rc = ant_hvf_guest_read(vm, desc.addr, raw + done, copy);
        if (rc != 0) return rc;
        done += copy;
      }
      total += n;
    }

    if (!(desc.flags & ANT_VRING_DESC_F_NEXT)) break;
    index = desc.next;
  }

  if (done < sizeof(raw)) return -EINVAL;
  *hdr = ant_hvf_vsock_load_hdr(raw);
  *used_len = total;
  return 0;
}

int ant_hvf_virtio_vsock_notify(ant_hvf_vm_t *vm, unsigned queue) {
  ant_hvf_vsock_device_t *dev = &vm->vsock;
  if (queue >= ANT_VIRTIO_VSOCK_QUEUE_COUNT) return 0;
  ant_hvf_virtio_queue_t *q = &dev->virtio.queues[queue];
  if (!q->enabled || !q->desc || !q->avail || !q->used) return 0;

  if (queue == 0) return ant_hvf_vsock_maybe_send_request(vm);
  if (queue == 2) return 0;

  uint64_t desc_base = q->desc;
  uint64_t avail_base = q->avail;
  uint64_t used_base = q->used;

  unsigned char idx_raw[2];
  int rc = ant_hvf_guest_read(vm, avail_base + 2, idx_raw, sizeof(idx_raw));
  if (rc != 0) return rc;
  uint16_t avail_idx = ant_hvf_load16(idx_raw);

  while (q->last_avail != avail_idx) {
    uint16_t ring_slot = q->last_avail % q->size;
    unsigned char head_raw[2];
    rc = ant_hvf_guest_read(vm, avail_base + 4u + (uint64_t)ring_slot * 2u,
                            head_raw, sizeof(head_raw));
    if (rc != 0) return rc;
    uint16_t head = ant_hvf_load16(head_raw);

    ant_virtio_vsock_hdr_t hdr;
    uint32_t used_len = 0;
    rc = ant_hvf_vsock_read_tx_packet(vm, desc_base, head, &hdr, &used_len);
    if (rc != 0) return rc;

    if (vm->trace) {
      fprintf(stderr,
              "sandbox vm: vsock tx op=%u len=%u src=%llu:%u dst=%llu:%u\n",
              hdr.op,
              hdr.len,
              (unsigned long long)hdr.src_cid,
              hdr.src_port,
              (unsigned long long)hdr.dst_cid,
              hdr.dst_port);
    }

    if (hdr.op == ANT_VIRTIO_VSOCK_OP_REQUEST && hdr.dst_port == ANT_HVF_VSOCK_HOST_PORT) {
      dev->connected = true;
      dev->peer_port = hdr.src_port;
      ant_hvf_vsock_send_packet(vm, ANT_VIRTIO_VSOCK_OP_RESPONSE, NULL, 0);
      ant_hvf_vsock_maybe_send_request(vm);
    } else if (hdr.op == ANT_VIRTIO_VSOCK_OP_RW) {
      dev->fwd_cnt += hdr.len;
    }

    rc = ant_hvf_vring_add_used(vm, used_base, q->size, head, used_len);
    if (rc != 0) return rc;
    q->last_avail++;
  }

  return ant_hvf_virtio_interrupt(vm, &dev->virtio, queue);
}

#endif
