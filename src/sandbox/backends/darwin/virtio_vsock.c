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
                                     const void *payload,
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

  size_t hdr_len = sizeof(ant_virtio_vsock_hdr_t);
  size_t packet_len = hdr_len + payload_len;
  if (packet_len > UINT32_MAX) return -E2BIG;

  unsigned char *packet = malloc(packet_len ? packet_len : 1);
  if (!packet) return -ENOMEM;

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
  rc = ant_hvf_vsock_write_iov(vm, desc_base, head, packet, (uint32_t)packet_len, &used_len);
  free(packet);
  if (rc != 0) return rc;
  rc = ant_hvf_vring_add_used(vm, used_base, q->size, head, used_len);
  if (rc != 0) return rc;

  q->last_avail++;
  return ant_hvf_virtio_interrupt(vm, &dev->virtio, 0);
}

int ant_hvf_vsock_maybe_send_request(ant_hvf_vm_t *vm) {
  ant_hvf_vsock_device_t *dev = &vm->vsock;
  if (!dev->connected || dev->request_sent || !dev->request_data || dev->request_len == 0) return 0;
  if (dev->request_len > UINT32_MAX) return -E2BIG;

  int rc = ant_hvf_vsock_send_packet(vm, ANT_VIRTIO_VSOCK_OP_RW,
                                     dev->request_data,
                                     (uint32_t)dev->request_len);
  if (rc == 0) {
    dev->request_sent = true;
    ant_hvf_verbosef(vm, "sent daemon request (%zu bytes)", dev->request_len);
  }
  if (rc == -EAGAIN) return 0;
  return rc;
}

int ant_hvf_vsock_read_tx_packet(ant_hvf_vm_t *vm,
                                        uint64_t desc_base,
                                        uint16_t head,
                                        ant_virtio_vsock_hdr_t *hdr,
                                        unsigned char **payload,
                                        uint32_t *used_len) {
  unsigned char *raw = NULL;
  uint32_t total = 0;
  uint16_t index = head;
  *payload = NULL;

  for (unsigned chain = 0; chain < ANT_VIRTIO_VSOCK_QUEUE_SIZE; chain++) {
    ant_vring_desc_t desc;
    int rc = ant_hvf_vring_read_desc(vm, desc_base, index, &desc);
    if (rc != 0) return rc;

    if (!(desc.flags & ANT_VRING_DESC_F_WRITE)) {
      if (desc.len > UINT32_MAX - total) return -E2BIG;
      total += desc.len;
    }

    if (!(desc.flags & ANT_VRING_DESC_F_NEXT)) break;
    index = desc.next;
  }

  if (total < sizeof(ant_virtio_vsock_hdr_t)) return -EINVAL;
  raw = malloc(total);
  if (!raw) return -ENOMEM;

  uint32_t done = 0;
  index = head;
  for (unsigned chain = 0; chain < ANT_VIRTIO_VSOCK_QUEUE_SIZE; chain++) {
    ant_vring_desc_t desc;
    int rc = ant_hvf_vring_read_desc(vm, desc_base, index, &desc);
    if (rc != 0) {
      free(raw);
      return rc;
    }

    if (!(desc.flags & ANT_VRING_DESC_F_WRITE)) {
      rc = ant_hvf_guest_read(vm, desc.addr, raw + done, desc.len);
      if (rc != 0) {
        free(raw);
        return rc;
      }
      done += desc.len;
    }

    if (!(desc.flags & ANT_VRING_DESC_F_NEXT)) break;
    index = desc.next;
  }

  *hdr = ant_hvf_vsock_load_hdr(raw);
  if (hdr->len > total - sizeof(ant_virtio_vsock_hdr_t)) {
    free(raw);
    return -EINVAL;
  }
  if (hdr->len > 0) {
    *payload = malloc(hdr->len);
    if (!*payload) {
      free(raw);
      return -ENOMEM;
    }
    memcpy(*payload, raw + sizeof(ant_virtio_vsock_hdr_t), hdr->len);
  }
  free(raw);
  *used_len = total;
  return 0;
}

static void ant_hvf_vsock_write_output(FILE *stream, const unsigned char *payload, uint32_t len, bool strip_ansi) {
  if (!strip_ansi) {
    if (len > 0) fwrite(payload, 1, len, stream);
    fflush(stream);
    return;
  }

  for (uint32_t i = 0; i < len; i++) {
    if (payload[i] == '\x1b' && i + 1 < len && payload[i + 1] == '[') {
      i += 2;
      while (i < len && ((payload[i] >= '0' && payload[i] <= '9') || payload[i] == ';')) i++;
      if (i < len && payload[i] == 'm') continue;
      fputc('\x1b', stream);
      fputc('[', stream);
      if (i < len) fputc(payload[i], stream);
      continue;
    }
    fputc(payload[i], stream);
  }
  fflush(stream);
}

static int ant_hvf_vsock_handle_frame(ant_hvf_vm_t *vm, uint8_t type, const unsigned char *payload, uint32_t len) {
  bool strip_ansi = (vm->vsock.capabilities & ANT_SANDBOX_CAP_COLOR_STRIP) != 0;
  if (vm->frame_handler && vm->frame_handler(type, payload, len, vm->frame_handler_user)) return 0;

  switch (type) {
    case ANT_SANDBOX_FRAME_STDOUT:
      ant_hvf_vsock_write_output(stdout, payload, len, strip_ansi);
      break;
    case ANT_SANDBOX_FRAME_STDERR:
      ant_hvf_vsock_write_output(stderr, payload, len, strip_ansi);
      break;
    case ANT_SANDBOX_FRAME_RESULT:
    {
      const char *display = NULL;
      size_t display_len = 0;
      if (ant_sandbox_result_payload_display(payload, len, &display, &display_len)) {
        if (display_len > 0) {
          ant_hvf_vsock_write_output(stdout, (const unsigned char *)display, (uint32_t)display_len, strip_ansi);
          if (display[display_len - 1] != '\n') fputc('\n', stdout);
          fflush(stdout);
        }
      } else {
        vm->vsock.protocol_error = true;
        return -EINVAL;
      }
      break;
    }
    case ANT_SANDBOX_FRAME_ERROR:
    {
      const char *display = NULL;
      size_t display_len = 0;
      if (ant_sandbox_error_payload_display(payload, len, &display, &display_len)) {
        if (display_len > 0) {
          ant_hvf_vsock_write_output(stderr, (const unsigned char *)display, (uint32_t)display_len, strip_ansi);
          if (display[display_len - 1] != '\n') fputc('\n', stderr);
          fflush(stderr);
        }
      } else {
        vm->vsock.protocol_error = true;
        return -EINVAL;
      }
      break;
    }
    case ANT_SANDBOX_FRAME_EXIT:
      if (len >= 4) {
        vm->vsock.exit_code = (int)ant_hvf_load32(payload);
        vm->vsock.exit_received = true;
        ant_hvf_verbosef(vm, "daemon exit code=%d", vm->vsock.exit_code);
      } else {
        vm->vsock.protocol_error = true;
        return -EINVAL;
      }
      break;
    default:
      vm->vsock.protocol_error = true;
      return -EINVAL;
  }
  return 0;
}

static int ant_hvf_vsock_consume_frames(ant_hvf_vm_t *vm, const unsigned char *payload, uint32_t len) {
  ant_hvf_vsock_device_t *dev = &vm->vsock;
  if (len == 0) return 0;
  if (len > ANT_SANDBOX_FRAME_MAX_SIZE) {
    dev->protocol_error = true;
    return -E2BIG;
  }
  if (dev->rx_stream_len > ANT_SANDBOX_FRAME_MAX_SIZE - len) {
    dev->protocol_error = true;
    return -E2BIG;
  }

  size_t needed = dev->rx_stream_len + len;
  if (needed > dev->rx_stream_cap) {
    size_t next = dev->rx_stream_cap ? dev->rx_stream_cap * 2 : 4096;
    while (next < needed) next *= 2;
    unsigned char *buf = realloc(dev->rx_stream, next);
    if (!buf) return -ENOMEM;
    dev->rx_stream = buf;
    dev->rx_stream_cap = next;
  }

  memcpy(dev->rx_stream + dev->rx_stream_len, payload, len);
  dev->rx_stream_len += len;

  size_t off = 0;
  while (dev->rx_stream_len - off >= ANT_SANDBOX_FRAME_HEADER_SIZE) {
    unsigned char *frame = dev->rx_stream + off;
    if (memcmp(frame, ANT_SANDBOX_FRAME_MAGIC, 4) != 0) {
      dev->protocol_error = true;
      return -EINVAL;
    }
    if (frame[4] != ANT_SANDBOX_FRAME_VERSION) {
      dev->protocol_error = true;
      return -EINVAL;
    }
    if (ant_hvf_load16(frame + 6) != 0) {
      dev->protocol_error = true;
      return -EINVAL;
    }

    uint32_t payload_len = ant_hvf_load32(frame + 8);
    if (payload_len > ANT_SANDBOX_FRAME_MAX_SIZE - ANT_SANDBOX_FRAME_HEADER_SIZE) {
      dev->protocol_error = true;
      return -E2BIG;
    }
    size_t frame_len = ANT_SANDBOX_FRAME_HEADER_SIZE + (size_t)payload_len;
    if (dev->rx_stream_len - off < frame_len) break;

    int rc = ant_hvf_vsock_handle_frame(vm, frame[5], frame + ANT_SANDBOX_FRAME_HEADER_SIZE, payload_len);
    if (rc != 0) return rc;
    off += frame_len;
  }

  if (off > 0) {
    memmove(dev->rx_stream, dev->rx_stream + off, dev->rx_stream_len - off);
    dev->rx_stream_len -= off;
  }

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
    unsigned char *payload = NULL;
    uint32_t used_len = 0;
    rc = ant_hvf_vsock_read_tx_packet(vm, desc_base, head, &hdr, &payload, &used_len);
    if (rc != 0) return rc;

    if (hdr.op == ANT_VIRTIO_VSOCK_OP_REQUEST && hdr.dst_port == ANT_HVF_VSOCK_HOST_PORT) {
      dev->connected = true;
      dev->peer_port = hdr.src_port;
      ant_hvf_verbose(vm, "daemon connected");
      ant_hvf_vsock_send_packet(vm, ANT_VIRTIO_VSOCK_OP_RESPONSE, NULL, 0);
      ant_hvf_vsock_maybe_send_request(vm);
    } else if (hdr.op == ANT_VIRTIO_VSOCK_OP_RW) {
      dev->fwd_cnt += hdr.len;
      rc = ant_hvf_vsock_consume_frames(vm, payload, hdr.len);
      if (rc != 0) {
        free(payload);
        return rc;
      }
    }
    free(payload);

    rc = ant_hvf_vring_add_used(vm, used_base, q->size, head, used_len);
    if (rc != 0) return rc;
    q->last_avail++;
  }

  return ant_hvf_virtio_interrupt(vm, &dev->virtio, queue);
}

#endif
