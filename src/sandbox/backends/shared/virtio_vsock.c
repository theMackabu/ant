#include "sandbox_backend/backend.h" // IWYU pragma: keep

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
int ant_hvf_vsock_write_iov(
  ant_hvf_vm_t *vm,
  uint64_t desc_base,
  uint16_t head,
  const unsigned char *data,
  uint32_t len,
  uint32_t *used_len
) {
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

static uint32_t ant_hvf_vsock_peer_space(const ant_hvf_vsock_device_t *dev) {
  uint32_t in_flight = dev->tx_cnt - dev->peer_fwd_cnt;
  if (in_flight >= dev->peer_buf_alloc) return 0;
  return dev->peer_buf_alloc - in_flight;
}

static void ant_hvf_vsock_update_peer_credit(ant_hvf_vsock_device_t *dev,
                                             const ant_virtio_vsock_hdr_t *hdr) {
  dev->peer_buf_alloc = hdr->buf_alloc;
  dev->peer_fwd_cnt = hdr->fwd_cnt;
}

static int ant_hvf_vsock_send_packet_len(ant_hvf_vm_t *vm,
                                         uint16_t op,
                                         const void *payload,
                                         uint32_t payload_len) {
  ant_hvf_vsock_device_t *dev = &vm->vsock;
  ant_hvf_virtio_queue_t *q = &dev->virtio.queues[0];
  if (!q->enabled || !q->desc || !q->avail || !q->used) return -EAGAIN;

  uint64_t desc_base = q->desc;
  uint64_t avail_base = q->avail;
  uint64_t used_base = q->used;

  unsigned char idx_raw[ANT_HVF_BYTES_U16];
  int rc = ant_hvf_guest_read(vm, avail_base + 2, idx_raw, sizeof(idx_raw));
  if (rc != 0) return rc;
  uint16_t avail_idx = ant_hvf_load16(idx_raw);
  if (q->last_avail == avail_idx) return -EAGAIN;

  uint16_t ring_slot = q->last_avail % q->size;
  unsigned char head_raw[ANT_HVF_BYTES_U16];
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
  if (op == ANT_VIRTIO_VSOCK_OP_RW) dev->tx_cnt += payload_len;
  return ant_hvf_virtio_interrupt(vm, &dev->virtio, 0);
}

static int ant_hvf_vsock_maybe_send_response(ant_hvf_vm_t *vm) {
  ant_hvf_vsock_device_t *dev = &vm->vsock;
  if (!dev->connected || dev->response_sent) return 0;
  int rc = ant_hvf_vsock_send_packet_len(vm, ANT_VIRTIO_VSOCK_OP_RESPONSE, NULL, 0);
  if (rc == 0) dev->response_sent = true;
  return rc == -EAGAIN ? 0 : rc;
}

int ant_hvf_vsock_send_packet(ant_hvf_vm_t *vm,
                                     uint16_t op,
                                     const void *payload,
                                     uint32_t payload_len) {
  uint32_t chunk = payload_len;
  if (op == ANT_VIRTIO_VSOCK_OP_RW) {
    uint32_t peer_space = ant_hvf_vsock_peer_space(&vm->vsock);
    if (peer_space == 0) return -EAGAIN;
    if (chunk > peer_space) chunk = peer_space;
    if (chunk > ANT_HVF_VSOCK_MAX_PAYLOAD) chunk = ANT_HVF_VSOCK_MAX_PAYLOAD;
  }
  return ant_hvf_vsock_send_packet_len(vm, op, payload, chunk);
}

int ant_hvf_vsock_queue_frame(ant_hvf_vm_t *vm, const void *data, size_t len) {
  if (!vm || !data || len == 0 || len > ANT_SANDBOX_FRAME_MAX_SIZE) return -EINVAL;
  ant_vsock_outgoing_frame_t *frame = calloc(1, sizeof(*frame));
  if (!frame) return -ENOMEM;
  frame->data = malloc(len);
  if (!frame->data) { free(frame); return -ENOMEM; }
  memcpy(frame->data, data, len);
  frame->len = len;

  if (vm->vsock_lock_init) pthread_mutex_lock(&vm->vsock_lock);
  if (vm->vsock.outgoing_tail) vm->vsock.outgoing_tail->next = frame;
  else vm->vsock.outgoing_head = frame;
  vm->vsock.outgoing_tail = frame;
  atomic_store_explicit(&vm->vsock_wake_pending, true, memory_order_release);
  if (vm->vsock_lock_init) pthread_mutex_unlock(&vm->vsock_lock);
  if (atomic_load_explicit(&vm->vcpu_running, memory_order_acquire))
    ant_hvf_wake_vcpu(vm);
  return 0;
}

void ant_hvf_vsock_clear_frames(ant_hvf_vm_t *vm) {
  if (!vm) return;
  if (vm->vsock_lock_init) pthread_mutex_lock(&vm->vsock_lock);
  ant_vsock_outgoing_frame_t *frame = vm->vsock.outgoing_head;
  while (frame) {
    ant_vsock_outgoing_frame_t *next = frame->next;
    free(frame->data);
    free(frame);
    frame = next;
  }
  vm->vsock.outgoing_head = NULL;
  vm->vsock.outgoing_tail = NULL;
  atomic_store_explicit(&vm->vsock_wake_pending, false, memory_order_release);
  if (vm->vsock_lock_init) pthread_mutex_unlock(&vm->vsock_lock);
}

int ant_hvf_vsock_maybe_send_request(ant_hvf_vm_t *vm) {
  ant_hvf_vsock_device_t *dev = &vm->vsock;
  if (vm->vsock_lock_init) pthread_mutex_lock(&vm->vsock_lock);
  ant_vsock_outgoing_frame_t *frame = dev->outgoing_head;
  if (!dev->connected || !frame) {
    if (!frame)
      atomic_store_explicit(&vm->vsock_wake_pending, false, memory_order_release);
    if (vm->vsock_lock_init) pthread_mutex_unlock(&vm->vsock_lock);
    return 0;
  }
  int response_rc = ant_hvf_vsock_maybe_send_response(vm);
  if (response_rc != 0 || !dev->response_sent) {
    if (vm->vsock_lock_init) pthread_mutex_unlock(&vm->vsock_lock);
    return response_rc;
  }
  if (frame->len > UINT32_MAX) {
    if (vm->vsock_lock_init) pthread_mutex_unlock(&vm->vsock_lock);
    return -E2BIG;
  }

  while (frame->off < frame->len) {
    uint32_t remaining = (uint32_t)(frame->len - frame->off);
    uint32_t chunk = remaining;
    uint32_t peer_space = ant_hvf_vsock_peer_space(dev);
    if (peer_space == 0) {
      if (vm->vsock_lock_init) pthread_mutex_unlock(&vm->vsock_lock);
      return 0;
    }
    if (chunk > peer_space) chunk = peer_space;
    if (chunk > ANT_HVF_VSOCK_MAX_PAYLOAD) chunk = ANT_HVF_VSOCK_MAX_PAYLOAD;

    int rc = ant_hvf_vsock_send_packet_len(vm, ANT_VIRTIO_VSOCK_OP_RW,
                                           frame->data + frame->off,
                                           chunk);
    if (rc == -EAGAIN) {
      if (vm->vsock_lock_init) pthread_mutex_unlock(&vm->vsock_lock);
      return 0;
    }
    if (rc != 0) {
      if (vm->vsock_lock_init) pthread_mutex_unlock(&vm->vsock_lock);
      return rc;
    }
    frame->off += chunk;
  }

  bool first = !atomic_load_explicit(&dev->request_sent, memory_order_acquire);
  atomic_store_explicit(&dev->request_sent, true, memory_order_release);
  dev->outgoing_head = frame->next;
  if (!dev->outgoing_head) dev->outgoing_tail = NULL;
  atomic_store_explicit(
    &vm->vsock_wake_pending,
    dev->outgoing_head != NULL,
    memory_order_release
  );
  size_t sent_len = frame->len;
  free(frame->data);
  free(frame);
  if (vm->vsock_lock_init) pthread_mutex_unlock(&vm->vsock_lock);
  ant_hvf_verbosef(vm, "%s daemon frame (%zu bytes)", first ? "sent" : "sent queued", sent_len);
  return 0;
}

static int ant_hvf_vsock_send_credit_update(ant_hvf_vm_t *vm) {
  int rc = ant_hvf_vsock_send_packet_len(vm, ANT_VIRTIO_VSOCK_OP_CREDIT_UPDATE, NULL, 0);
  return rc == -EAGAIN ? 0 : rc;
}

static int ant_hvf_vsock_send_event(ant_hvf_vm_t *vm, uint32_t id) {
  ant_hvf_vsock_device_t *dev = &vm->vsock;
  ant_hvf_virtio_queue_t *q = &dev->virtio.queues[2];
  if (!q->enabled || !q->desc || !q->avail || !q->used) return -EAGAIN;

  uint64_t desc_base = q->desc;
  uint64_t avail_base = q->avail;
  uint64_t used_base = q->used;

  unsigned char idx_raw[ANT_HVF_BYTES_U16];
  int rc = ant_hvf_guest_read(vm, avail_base + 2, idx_raw, sizeof(idx_raw));
  if (rc != 0) return rc;
  uint16_t avail_idx = ant_hvf_load16(idx_raw);
  if (q->last_avail == avail_idx) return -EAGAIN;

  uint16_t ring_slot = q->last_avail % q->size;
  unsigned char head_raw[ANT_HVF_BYTES_U16];
  rc = ant_hvf_guest_read(vm, avail_base + 4u + (uint64_t)ring_slot * 2u,
                          head_raw, sizeof(head_raw));
  if (rc != 0) return rc;
  uint16_t head = ant_hvf_load16(head_raw);

  unsigned char raw[ANT_HVF_BYTES_U32];
  ant_hvf_store32(raw, id);
  uint32_t used_len = 0;
  rc = ant_hvf_vsock_write_iov(vm, desc_base, head, raw, sizeof(raw), &used_len);
  if (rc != 0) return rc;
  rc = ant_hvf_vring_add_used(vm, used_base, q->size, head, used_len);
  if (rc != 0) return rc;

  q->last_avail++;
  return ant_hvf_virtio_interrupt(vm, &dev->virtio, 2);
}

static int ant_hvf_vsock_maybe_send_event(ant_hvf_vm_t *vm) {
  ant_hvf_vsock_device_t *dev = &vm->vsock;
  if (!dev->event_transport_reset_pending) return 0;
  int rc = ant_hvf_vsock_send_event(vm, ANT_VIRTIO_VSOCK_EVENT_TRANSPORT_RESET);
  if (rc == 0) dev->event_transport_reset_pending = false;
  return rc == -EAGAIN ? 0 : rc;
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

  if (queue == 0) {
    int rc = ant_hvf_vsock_maybe_send_response(vm);
    if (rc != 0) return rc;
    return ant_hvf_vsock_maybe_send_request(vm);
  }
  if (queue == 2) return ant_hvf_vsock_maybe_send_event(vm);

  uint64_t desc_base = q->desc;
  uint64_t avail_base = q->avail;
  uint64_t used_base = q->used;

  int rc = 0;
  for (;;) {
    unsigned char idx_raw[ANT_HVF_BYTES_U16];
    rc = ant_hvf_guest_read(vm, avail_base + 2, idx_raw, sizeof(idx_raw));
    if (rc != 0) return rc;
    uint16_t avail_idx = ant_hvf_load16(idx_raw);
    if (q->last_avail == avail_idx) break;

    uint16_t ring_slot = q->last_avail % q->size;
    unsigned char head_raw[ANT_HVF_BYTES_U16];
    rc = ant_hvf_guest_read(vm, avail_base + 4u + (uint64_t)ring_slot * 2u,
                            head_raw, sizeof(head_raw));
    if (rc != 0) return rc;
    uint16_t head = ant_hvf_load16(head_raw);

    ant_virtio_vsock_hdr_t hdr;
    unsigned char *payload = NULL;
    uint32_t used_len = 0;
    rc = ant_hvf_vsock_read_tx_packet(vm, desc_base, head, &hdr, &payload, &used_len);
    if (rc != 0) return rc;

    if (hdr.src_cid == ANT_HVF_VSOCK_GUEST_CID &&
        hdr.dst_cid == ANT_HVF_VSOCK_HOST_CID &&
        hdr.src_port == dev->peer_port &&
        hdr.dst_port == ANT_HVF_VSOCK_HOST_PORT) {
      ant_hvf_vsock_update_peer_credit(dev, &hdr);
    }

    if (hdr.op == ANT_VIRTIO_VSOCK_OP_REQUEST && hdr.dst_port == ANT_HVF_VSOCK_HOST_PORT) {
      dev->connected = true;
      dev->peer_port = hdr.src_port;
      dev->response_sent = false;
      ant_hvf_vsock_update_peer_credit(dev, &hdr);
      ant_hvf_verbose(vm, "daemon connected");
      rc = ant_hvf_vsock_maybe_send_response(vm);
      if (rc != 0) {
        free(payload);
        return rc;
      }
      rc = ant_hvf_vsock_maybe_send_request(vm);
      if (rc != 0) {
        free(payload);
        return rc;
      }
    } else if (hdr.op == ANT_VIRTIO_VSOCK_OP_RW) {
      dev->fwd_cnt += hdr.len;
      rc = ant_hvf_vsock_consume_frames(vm, payload, hdr.len);
      if (rc != 0) {
        free(payload);
        return rc;
      }
      rc = ant_hvf_vsock_send_credit_update(vm);
      if (rc != 0) {
        free(payload);
        return rc;
      }
      rc = ant_hvf_vsock_maybe_send_request(vm);
      if (rc != 0) {
        free(payload);
        return rc;
      }
    } else if (hdr.op == ANT_VIRTIO_VSOCK_OP_CREDIT_UPDATE) {
      rc = ant_hvf_vsock_maybe_send_request(vm);
      if (rc != 0) {
        free(payload);
        return rc;
      }
    } else if (hdr.op == ANT_VIRTIO_VSOCK_OP_CREDIT_REQUEST) {
      rc = ant_hvf_vsock_send_credit_update(vm);
      if (rc != 0) {
        free(payload);
        return rc;
      }
    } else if (hdr.op == ANT_VIRTIO_VSOCK_OP_RST) {
      dev->connected = false;
      dev->response_sent = false;
      atomic_store_explicit(&dev->request_sent, false, memory_order_release);
      ant_hvf_vsock_clear_frames(vm);
      dev->transport_error = !dev->exit_received;
    } else if (hdr.op == ANT_VIRTIO_VSOCK_OP_SHUTDOWN) {
      rc = ant_hvf_vsock_send_packet(vm, ANT_VIRTIO_VSOCK_OP_SHUTDOWN, NULL, 0);
      if (rc != 0 && rc != -EAGAIN) {
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
