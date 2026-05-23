#include "net_internal.h"

#if defined(__aarch64__)

typedef enum {
  ANT_NAT_TCP_UNUSED = 0,
  ANT_NAT_TCP_CONNECTING_HOST,
  ANT_NAT_TCP_CONNECTING_GUEST,
  ANT_NAT_TCP_ESTABLISHED,
  ANT_NAT_TCP_HALF_CLOSED,
} ant_hvf_nat_tcp_state_t;

typedef struct ant_hvf_nat_tcp ant_hvf_nat_tcp_t;

typedef struct {
  int fd;
  uint16_t host_port;
  uint16_t guest_port;
} ant_hvf_nat_forward_t;

struct ant_hvf_nat_tcp {
  ant_hvf_nat_tcp_state_t state;
  bool inbound;
  int fd;
  uint32_t guest_ip;
  uint16_t guest_port;
  uint32_t peer_ip;
  uint16_t peer_port;
  uint32_t guest_next;
  uint32_t nat_next;
  uint32_t nat_iss;
  uint32_t guest_ack;
  uint32_t guest_window;
  bool guest_fin_seen;
  bool host_eof;
  bool host_fin_sent;
  uint8_t guest_mac[6];
  unsigned char *pending;
  size_t pending_off;
  size_t pending_len;
  size_t pending_cap;
  unsigned char *host_buf;
  size_t host_cap;
  uint32_t host_base;
  size_t host_len;
  size_t host_sent;
};

struct ant_hvf_nat {
  ant_hvf_vm_t *vm;
  pthread_mutex_t lock;
  bool lock_init;
  pthread_t thread;
  bool thread_started;
  bool stop;
  int wake_pipe[2];
  ant_hvf_nat_tcp_t **tcp;
  size_t tcp_count;
  size_t tcp_cap;
  ant_hvf_nat_forward_t *forwards;
  size_t forward_count;
  size_t forward_cap;
  uint16_t next_inbound_port;
  uint64_t guest_packets;
  uint64_t guest_drops;
  uint64_t host_packets;
  uint64_t tcp_opened;
};

static bool ant_nat_tcp_active(const ant_hvf_nat_tcp_t *c) {
  return c && c->state != ANT_NAT_TCP_UNUSED;
}

static bool ant_nat_grow_tcp_table(ant_hvf_nat_t *nat, size_t needed) {
  if (needed <= nat->tcp_cap) return true;
  size_t next = nat->tcp_cap ? nat->tcp_cap : ANT_NAT_TCP_INITIAL_CAP;
  while (next < needed) {
    if (next > SIZE_MAX / 2u) return false;
    next *= 2u;
  }
  if (next > SIZE_MAX / sizeof(*nat->tcp)) return false;
  ant_hvf_nat_tcp_t **grown = realloc(nat->tcp, next * sizeof(*grown));
  if (!grown) return false;
  memset(grown + nat->tcp_cap, 0, (next - nat->tcp_cap) * sizeof(*grown));
  nat->tcp = grown;
  nat->tcp_cap = next;
  return true;
}

static bool ant_nat_grow_buffer(unsigned char **buf, size_t *cap, size_t needed) {
  if (needed <= *cap) return true;
  if (needed > ANT_NAT_STREAM_BUFFER_LIMIT) return false;

  size_t next = *cap ? *cap : ANT_NAT_STREAM_INITIAL_CAP;
  while (next < needed) {
    if (next > ANT_NAT_STREAM_BUFFER_LIMIT / 2u) {
      next = ANT_NAT_STREAM_BUFFER_LIMIT;
    } else {
      next *= 2u;
    }
  }

  unsigned char *grown = realloc(*buf, next);
  if (!grown) return false;
  *buf = grown;
  *cap = next;
  return true;
}

static bool ant_nat_append_pending(ant_hvf_nat_tcp_t *c, const void *data, size_t len) {
  if (len == 0) return true;
  if (len > ANT_NAT_STREAM_BUFFER_LIMIT - c->pending_len) return false;

  if (c->pending_off > 0 &&
      c->pending_off + c->pending_len + len > c->pending_cap) {
    memmove(c->pending, c->pending + c->pending_off, c->pending_len);
    c->pending_off = 0;
  }

  size_t needed = c->pending_off + c->pending_len + len;
  if (!ant_nat_grow_buffer(&c->pending, &c->pending_cap, needed)) return false;
  memcpy(c->pending + c->pending_off + c->pending_len, data, len);
  c->pending_len += len;
  return true;
}

static size_t ant_nat_host_room(ant_hvf_nat_tcp_t *c) {
  if (c->host_len >= ANT_NAT_STREAM_BUFFER_LIMIT) return 0;
  size_t wanted = c->host_len + ANT_NAT_STREAM_GROW_CHUNK;
  if (wanted > ANT_NAT_STREAM_BUFFER_LIMIT) wanted = ANT_NAT_STREAM_BUFFER_LIMIT;
  if (!ant_nat_grow_buffer(&c->host_buf, &c->host_cap, wanted)) return 0;
  return c->host_cap - c->host_len;
}

static void ant_nat_free_tcp_buffers(ant_hvf_nat_tcp_t *c) {
  if (!c) return;
  free(c->pending);
  free(c->host_buf);
  c->pending = NULL;
  c->host_buf = NULL;
  c->pending_cap = 0;
  c->host_cap = 0;
}

void ant_nat_wake(ant_hvf_nat_t *nat) {
  if (!nat || nat->wake_pipe[1] < 0) return;
  unsigned char byte = 0;
  ssize_t ignored = write(nat->wake_pipe[1], &byte, sizeof(byte));
  (void)ignored;
}

static void ant_nat_drain_wake(ant_hvf_nat_t *nat) {
  unsigned char buf[64];
  while (nat->wake_pipe[0] >= 0) {
    ssize_t n = read(nat->wake_pipe[0], buf, sizeof(buf));
    if (n > 0) continue;
    if (n < 0 && errno == EINTR) continue;
    break;
  }
}

void ant_nat_note_guest_packet(ant_hvf_nat_t *nat) {
  if (nat) nat->guest_packets++;
}

void ant_hvf_net_note_rx(ant_hvf_vm_t *vm) {
  if (!vm->net_lock_init) return;
  pthread_mutex_lock(&vm->net_lock);
  vm->net_rx_wake = true;
  pthread_mutex_unlock(&vm->net_lock);
  if (vm->vcpu) hv_vcpus_exit(&vm->vcpu, 1);
}

void ant_net_enqueue(ant_hvf_vm_t *vm, const void *data, uint32_t len) {
  if (len == 0 || len > ANT_HVF_NET_MAX_PACKET) return;
  ant_hvf_net_packet_t packet;
  packet.len = len;
  memcpy(packet.data, data, len);

  bool queued = false;
  pthread_mutex_lock(&vm->net_lock);
  if (vm->net_rx_count < ANT_HVF_NET_RX_BACKLOG) {
    uint32_t slot = (vm->net_rx_head + vm->net_rx_count) % ANT_HVF_NET_RX_BACKLOG;
    vm->net_rx_packets[slot] = packet;
    vm->net_rx_count++;
    queued = true;
  }
  pthread_mutex_unlock(&vm->net_lock);
  if (queued) {
    if (vm->net_nat) vm->net_nat->host_packets++;
    ant_hvf_virtio_net_drain_rx(vm);
    ant_hvf_net_note_rx(vm);
  } else if (vm->net_nat) {
    vm->net_nat->guest_drops++;
  }
}

static int ant_socket_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -errno;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -errno;
}

static ant_hvf_nat_tcp_t *ant_nat_find_tcp(ant_hvf_nat_t *nat,
                                           uint32_t guest_ip,
                                           uint16_t guest_port,
                                           uint32_t peer_ip,
                                           uint16_t peer_port) {
  for (size_t i = 0; i < nat->tcp_count; i++) {
    ant_hvf_nat_tcp_t *c = nat->tcp[i];
    if (!ant_nat_tcp_active(c)) continue;
    if (c->guest_ip == guest_ip && c->guest_port == guest_port &&
        c->peer_ip == peer_ip && c->peer_port == peer_port) {
      return c;
    }
  }
  return NULL;
}

static uint16_t ant_nat_alloc_inbound_port(ant_hvf_nat_t *nat) {
  if (nat->next_inbound_port < ANT_NAT_INBOUND_PORT_FIRST) {
    nat->next_inbound_port = ANT_NAT_INBOUND_PORT_FIRST;
  }

  for (uint32_t tries = 0; tries <= ANT_NAT_INBOUND_PORT_LAST - ANT_NAT_INBOUND_PORT_FIRST; tries++) {
    uint16_t port = nat->next_inbound_port++;
    if (nat->next_inbound_port < ANT_NAT_INBOUND_PORT_FIRST) nat->next_inbound_port = ANT_NAT_INBOUND_PORT_FIRST;

    bool used = false;
    for (size_t i = 0; i < nat->tcp_count; i++) {
      ant_hvf_nat_tcp_t *c = nat->tcp[i];
      if (ant_nat_tcp_active(c) && c->inbound && c->peer_port == port) {
        used = true;
        break;
      }
    }
    if (!used) return port;
  }

  return 0;
}

static ant_hvf_nat_tcp_t *ant_nat_alloc_tcp(ant_hvf_nat_t *nat) {
  for (size_t i = 0; i < nat->tcp_count; i++) {
    if (!ant_nat_tcp_active(nat->tcp[i])) {
      ant_hvf_nat_tcp_t *c = nat->tcp[i];
      memset(c, 0, sizeof(*c));
      c->state = ANT_NAT_TCP_ESTABLISHED;
      c->fd = -1;
      nat->tcp_opened++;
      return c;
    }
  }

  if (!ant_nat_grow_tcp_table(nat, nat->tcp_count + 1u)) {
    return NULL;
  }

  ant_hvf_nat_tcp_t *c = calloc(1, sizeof(*c));
  if (!c) return NULL;
  c->fd = -1;
  nat->tcp[nat->tcp_count++] = c;
  c->state = ANT_NAT_TCP_ESTABLISHED;
  nat->tcp_opened++;
  return c;
}

static void ant_nat_close_tcp(ant_hvf_nat_tcp_t *c) {
  if (c->fd >= 0) close(c->fd);
  ant_nat_free_tcp_buffers(c);
  memset(c, 0, sizeof(*c));
  c->fd = -1;
}

static void ant_nat_close_fd(ant_hvf_nat_tcp_t *c) {
  if (c->fd >= 0) close(c->fd);
  c->fd = -1;
  c->state = ANT_NAT_TCP_HALF_CLOSED;
  free(c->pending);
  c->pending = NULL;
  c->pending_off = 0;
  c->pending_len = 0;
  c->pending_cap = 0;
}

static uint32_t ant_nat_host_sent_seq(const ant_hvf_nat_tcp_t *c) {
  return c->host_base + (uint32_t)c->host_sent;
}

static void ant_nat_drop_guest_acked(ant_hvf_nat_tcp_t *c) {
  if (c->host_len == 0) {
    if ((int32_t)(c->guest_ack - c->host_base) > 0) c->host_base = c->guest_ack;
    c->host_sent = 0;
    return;
  }

  if ((int32_t)(c->guest_ack - c->host_base) <= 0) return;

  uint32_t acked32 = c->guest_ack - c->host_base;
  size_t acked = acked32 > c->host_len ? c->host_len : (size_t)acked32;
  if (acked > 0) {
    if (acked < c->host_len) memmove(c->host_buf, c->host_buf + acked, c->host_len - acked);
    c->host_len -= acked;
    c->host_base += (uint32_t)acked;
    c->host_sent = c->host_sent > acked ? c->host_sent - acked : 0;
  }

  if (c->host_len == 0) {
    c->host_base = c->guest_ack;
    c->host_sent = 0;
  }
}

static void ant_nat_send_host_buffer(ant_hvf_nat_t *nat, ant_hvf_nat_tcp_t *c) {
  ant_hvf_vm_t *vm = nat->vm;
  ant_nat_drop_guest_acked(c);

  while (c->host_sent < c->host_len) {
    uint32_t sent_seq = ant_nat_host_sent_seq(c);
    uint32_t in_flight = sent_seq - c->guest_ack;
    uint32_t guest_window = c->guest_window ? c->guest_window : ANT_TCP_GUEST_WINDOW;
    if (in_flight >= guest_window) break;

    size_t chunk = c->host_len - c->host_sent;
    if (chunk > ANT_TCP_MSS) chunk = ANT_TCP_MSS;
    uint32_t allowed = guest_window - in_flight;
    if (chunk > allowed) chunk = allowed;
    if (chunk == 0) break;

    ant_net_send_tcp(vm, c->guest_mac, c->peer_ip, c->guest_ip, c->peer_port, c->guest_port,
                     sent_seq, c->guest_next, ANT_TCP_ACK | ANT_TCP_PSH,
                     c->host_buf + c->host_sent, (uint16_t)chunk);
    c->host_sent += chunk;
    c->nat_next = sent_seq + (uint32_t)chunk;
  }

  if (c->host_eof && !c->host_fin_sent && c->host_sent == c->host_len) {
    uint32_t fin_seq = c->host_base + (uint32_t)c->host_len;
    ant_net_send_tcp(vm, c->guest_mac, c->peer_ip, c->guest_ip, c->peer_port, c->guest_port,
                     fin_seq, c->guest_next, ANT_TCP_ACK | ANT_TCP_FIN, NULL, 0);
    c->host_fin_sent = true;
    c->nat_next = fin_seq + 1u;
  }
}

static void ant_nat_flush_pending(ant_hvf_nat_tcp_t *c) {
  if (c->fd < 0) return;
  while (c->pending_len > 0 && c->state != ANT_NAT_TCP_CONNECTING_HOST) {
    ssize_t n = write(c->fd, c->pending + c->pending_off, c->pending_len);
    if (n > 0) {
      /* guest -> host */
      c->pending_off += (size_t)n;
      c->pending_len -= (size_t)n;
      if (c->pending_len == 0) c->pending_off = 0;
      continue;
    }
    if (n < 0 && (errno == EINTR)) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    ant_nat_close_tcp(c);
    break;
  }
}

static void ant_nat_tcp_from_host(ant_hvf_nat_t *nat, ant_hvf_nat_tcp_t *c) {
  for (;;) {
    size_t room = ant_nat_host_room(c);
    if (room == 0) break;
    ssize_t n = read(c->fd, c->host_buf + c->host_len, room);
    if (n > 0) {
      if (c->host_len == 0 && c->host_sent == 0) c->host_base = c->nat_next;
      c->host_len += (size_t)n;
      ant_nat_send_host_buffer(nat, c);
      continue;
    }
    if (n == 0) {
      c->host_eof = true;
      ant_nat_close_fd(c);
      ant_nat_send_host_buffer(nat, c);
      break;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
    ant_nat_close_tcp(c);
    break;
  }
}

static void ant_nat_check_connect(ant_hvf_nat_tcp_t *c) {
  if (c->state != ANT_NAT_TCP_CONNECTING_HOST) return;
  int err = 0;
  socklen_t len = sizeof(err);
  if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
    ant_nat_close_tcp(c);
    return;
  }
  c->state = ANT_NAT_TCP_ESTABLISHED;
  ant_nat_flush_pending(c);
}

static void *ant_nat_thread(void *arg) {
  ant_hvf_nat_t *nat = arg;
  nfds_t poll_cap = 0;
  struct pollfd *fds = NULL;
  ant_hvf_nat_tcp_t **tcp_map = NULL;
  ant_hvf_nat_forward_t **fwd_map = NULL;

  while (!nat->stop) {
    nfds_t n = 0;
    pthread_mutex_lock(&nat->lock);
    nfds_t needed = (nfds_t)(1u + nat->tcp_count + nat->forward_count);
    if (needed > poll_cap) {
      struct pollfd *new_fds = calloc(needed, sizeof(*new_fds));
      ant_hvf_nat_tcp_t **new_tcp_map = calloc(needed, sizeof(*new_tcp_map));
      ant_hvf_nat_forward_t **new_fwd_map = calloc(needed, sizeof(*new_fwd_map));
      if (!new_fds || !new_tcp_map || !new_fwd_map) {
        free(new_fds);
        free(new_tcp_map);
        free(new_fwd_map);
        pthread_mutex_unlock(&nat->lock);
        break;
      }
      free(fds);
      free(tcp_map);
      free(fwd_map);
      fds = new_fds;
      tcp_map = new_tcp_map;
      fwd_map = new_fwd_map;
      poll_cap = needed;
    }
    memset(tcp_map, 0, poll_cap * sizeof(*tcp_map));
    memset(fwd_map, 0, poll_cap * sizeof(*fwd_map));
    fds[n++] = (struct pollfd){ .fd = nat->wake_pipe[0], .events = POLLIN };

    for (size_t i = 0; i < nat->forward_count && n < poll_cap; i++) {
      fds[n] = (struct pollfd){ .fd = nat->forwards[i].fd, .events = POLLIN };
      fwd_map[n] = &nat->forwards[i];
      n++;
    }
    for (size_t i = 0; i < nat->tcp_count && n < poll_cap; i++) {
      ant_hvf_nat_tcp_t *c = nat->tcp[i];
      if (!ant_nat_tcp_active(c) || c->fd < 0) continue;
      uint32_t in_flight = ant_nat_host_sent_seq(c) - c->guest_ack;
      uint32_t guest_window = c->guest_window ? c->guest_window : ANT_TCP_GUEST_WINDOW;
      short ev = in_flight < guest_window &&
                 c->host_len < ANT_NAT_STREAM_BUFFER_LIMIT &&
                 !c->host_eof ? POLLIN : 0;
      if (c->state == ANT_NAT_TCP_CONNECTING_HOST || c->pending_len) ev |= POLLOUT;
      if (!ev) continue;
      fds[n] = (struct pollfd){ .fd = c->fd, .events = ev };
      tcp_map[n] = c;
      n++;
    }
    pthread_mutex_unlock(&nat->lock);

    int prc = poll(fds, n, 25);
    if (prc < 0 && errno == EINTR) continue;
    if (prc <= 0) continue;
    if (fds[0].revents & POLLIN) ant_nat_drain_wake(nat);

    pthread_mutex_lock(&nat->lock);
    for (nfds_t i = 1; i < n; i++) {
      if (!fds[i].revents) continue;
      if (fwd_map[i]) {
        int fd = accept(fwd_map[i]->fd, NULL, NULL);
        if (fd >= 0) {
          ant_socket_nonblock(fd);
          ant_hvf_nat_tcp_t *c = ant_nat_alloc_tcp(nat);
          uint16_t peer_port = ant_nat_alloc_inbound_port(nat);
          if (c && peer_port > 0) {
            c->inbound = true;
            c->fd = fd;
            c->guest_ip = ANT_NET_GUEST_IP;
            c->guest_port = fwd_map[i]->guest_port;
            c->peer_ip = ANT_NET_HOST_IP;
            c->peer_port = peer_port;
            c->nat_iss = 0x41000000u + ((uint32_t)peer_port << 4u);
            c->nat_next = c->nat_iss + 1u;
            c->guest_ack = c->nat_iss;
            c->guest_window = 65535u;
            c->host_base = c->nat_next;
            c->state = ANT_NAT_TCP_CONNECTING_GUEST;
            memcpy(c->guest_mac, nat->vm->net_guest_mac_seen ? nat->vm->net_guest_mac :
                                  (uint8_t[]){ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, 6);
            ant_net_send_tcp(nat->vm, c->guest_mac, c->peer_ip, c->guest_ip, c->peer_port, c->guest_port,
                             c->nat_iss, 0, ANT_TCP_SYN, NULL, 0);
          } else {
            if (c) ant_nat_close_tcp(c);
            close(fd);
          }
        }
        continue;
      }
      ant_hvf_nat_tcp_t *c = tcp_map[i];
      if (!c || !ant_nat_tcp_active(c)) continue;
      if (fds[i].revents & POLLOUT) {
        ant_nat_check_connect(c);
        if (ant_nat_tcp_active(c)) ant_nat_flush_pending(c);
      }
      if (ant_nat_tcp_active(c) && (fds[i].revents & (POLLIN | POLLHUP))) ant_nat_tcp_from_host(nat, c);
      if (ant_nat_tcp_active(c) && (fds[i].revents & POLLERR)) ant_nat_close_tcp(c);
    }
    pthread_mutex_unlock(&nat->lock);
  }
  free(fds);
  free(tcp_map);
  free(fwd_map);
  return NULL;
}

void ant_nat_handle_tcp(ant_hvf_nat_t *nat,
                        const ant_eth_t *eth,
                        const ant_ipv4_t *ip,
                        const ant_tcp_t *tcp,
                        const unsigned char *payload,
                        size_t payload_len) {
  ant_hvf_vm_t *vm = nat->vm;
  uint32_t src_ip = ntohl(ip->src);
  uint32_t dst_ip = ntohl(ip->dst);
  uint16_t src_port = ntohs(tcp->src);
  uint16_t dst_port = ntohs(tcp->dst);
  uint32_t seq = ntohl(tcp->seq);
  uint32_t ack = ntohl(tcp->ack);

  pthread_mutex_lock(&nat->lock);
  ant_hvf_nat_tcp_t *c = ant_nat_find_tcp(nat, src_ip, src_port, dst_ip, dst_port);
  if (!c) c = ant_nat_find_tcp(nat, dst_ip, dst_port, src_ip, src_port);

  if (!c && (tcp->flags & ANT_TCP_SYN) && !(tcp->flags & ANT_TCP_ACK)) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) goto done;
    ant_socket_nonblock(fd);
    c = ant_nat_alloc_tcp(nat);
    if (!c) {
      close(fd);
      goto done;
    }
    c->fd = fd;
    c->guest_ip = src_ip;
    c->guest_port = src_port;
    c->peer_ip = dst_ip;
    c->peer_port = dst_port;
    c->guest_next = seq + 1u;
    c->nat_iss = 0x51000000u + (uint32_t)(src_port << 4u);
    c->nat_next = c->nat_iss + 1u;
    c->guest_ack = c->nat_iss;
    c->guest_window = ntohs(tcp->win);
    c->host_base = c->nat_next;
    memcpy(c->guest_mac, eth->src, 6);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(dst_ip == ANT_NET_HOST_IP ? 0x7f000001u : dst_ip);
    addr.sin_port = htons(dst_port);
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS) {
      ant_net_send_rst(vm, eth->src, dst_ip, src_ip, dst_port, src_port, 0, seq + 1u);
      ant_nat_close_tcp(c);
      goto done;
    }
    c->state = rc != 0 ? ANT_NAT_TCP_CONNECTING_HOST : ANT_NAT_TCP_ESTABLISHED;
    ant_net_send_tcp(vm, eth->src, dst_ip, src_ip, dst_port, src_port, c->nat_iss, c->guest_next,
                     ANT_TCP_SYN | ANT_TCP_ACK, NULL, 0);
    ant_nat_wake(nat);
    goto done;
  }

  if (!c) {
    if (!(tcp->flags & ANT_TCP_RST)) {
      ant_net_send_rst(vm, eth->src, dst_ip, src_ip, dst_port, src_port, ack, seq + (uint32_t)payload_len);
    }
    goto done;
  }

  if (tcp->flags & ANT_TCP_RST) {
    ant_nat_close_tcp(c);
    goto done;
  }

  uint32_t prev_window = c->guest_window;
  c->guest_window = ntohs(tcp->win);

  if (tcp->flags & ANT_TCP_ACK) {
    uint32_t prev = c->guest_ack;
    if ((int32_t)(ack - c->guest_ack) > 0) c->guest_ack = ack;
    if (c->guest_ack != prev || c->guest_window != prev_window) {
      ant_nat_drop_guest_acked(c);
      ant_nat_send_host_buffer(nat, c);
      ant_nat_wake(nat);
    }
  }

  if (c->state == ANT_NAT_TCP_CONNECTING_GUEST && (tcp->flags & ANT_TCP_SYN) && (tcp->flags & ANT_TCP_ACK)) {
    c->guest_next = seq + 1u;
    c->state = ANT_NAT_TCP_ESTABLISHED;
    c->guest_ack = ack;
    c->guest_window = ntohs(tcp->win);
    ant_nat_drop_guest_acked(c);
    ant_net_send_tcp(vm, c->guest_mac, c->peer_ip, c->guest_ip, c->peer_port, c->guest_port,
                     c->nat_next, c->guest_next, ANT_TCP_ACK, NULL, 0);
    ant_nat_wake(nat);
    goto done;
  }

  if (payload_len > 0) {
    c->guest_next = seq + (uint32_t)payload_len;
    if (c->fd >= 0 && !c->guest_fin_seen) {
      if (!ant_nat_append_pending(c, payload, payload_len)) goto done;
    }
    ant_net_send_tcp(vm, c->guest_mac, c->peer_ip, c->guest_ip, c->peer_port, c->guest_port,
                     c->nat_next, c->guest_next, ANT_TCP_ACK, NULL, 0);
    ant_nat_flush_pending(c);
    ant_nat_wake(nat);
  }

  if (tcp->flags & ANT_TCP_FIN) {
    uint32_t fin_next = seq + (uint32_t)payload_len + 1u;
    if (fin_next > c->guest_next) c->guest_next = fin_next;
    c->guest_fin_seen = true;
    c->state = ANT_NAT_TCP_HALF_CLOSED;
    if (c->fd >= 0) shutdown(c->fd, SHUT_WR);
    ant_net_send_tcp(vm, c->guest_mac, c->peer_ip, c->guest_ip, c->peer_port, c->guest_port,
                     c->nat_next, c->guest_next, ANT_TCP_ACK, NULL, 0);
    if (c->host_fin_sent && c->guest_ack >= c->nat_next) ant_nat_close_tcp(c);
  } else if (c->host_fin_sent && c->guest_fin_seen && c->guest_ack >= c->nat_next) {
    ant_nat_close_tcp(c);
  }

done:
  pthread_mutex_unlock(&nat->lock);
}

int ant_hvf_net_start(ant_hvf_vm_t *vm) {
  ant_hvf_nat_t *nat = calloc(1, sizeof(*nat));
  if (!nat) return -ENOMEM;
  nat->vm = vm;
  nat->wake_pipe[0] = -1;
  nat->wake_pipe[1] = -1;
  vm->net_nat = nat;

  if (pthread_mutex_init(&nat->lock, NULL) != 0) {
    vm->net_nat = NULL;
    free(nat);
    return -errno;
  }
  nat->lock_init = true;

  if (pipe(nat->wake_pipe) != 0) {
    int rc = -errno;
    ant_hvf_net_stop(vm);
    return rc;
  }
  ant_socket_nonblock(nat->wake_pipe[0]);
  ant_socket_nonblock(nat->wake_pipe[1]);

  nat->forward_cap = vm->net_forward_count;
  if (nat->forward_cap > 0) {
    nat->forwards = calloc(nat->forward_cap, sizeof(*nat->forwards));
    if (!nat->forwards) {
      ant_hvf_net_stop(vm);
      return -ENOMEM;
    }
  }

  for (size_t i = 0; i < vm->net_forward_count; i++) {
    if (nat->forward_count >= nat->forward_cap) {
      ant_hvf_net_stop(vm);
      return -E2BIG;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      int rc = -errno;
      ant_hvf_net_stop(vm);
      return rc;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ant_socket_nonblock(fd);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001u);
    addr.sin_port = htons(vm->net_forwards[i].host_port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 64) != 0) {
      int rc = -errno;
      close(fd);
      ant_hvf_net_stop(vm);
      return rc;
    }
    nat->forwards[nat->forward_count++] = (ant_hvf_nat_forward_t){
      .fd = fd,
      .host_port = vm->net_forwards[i].host_port,
      .guest_port = vm->net_forwards[i].guest_port,
    };
  }

  int prc = pthread_create(&nat->thread, NULL, ant_nat_thread, nat);
  if (prc != 0) {
    ant_hvf_net_stop(vm);
    return -prc;
  }
  nat->thread_started = true;
  vm->net_started = true;
  vm->net_max_packet_size = 1518u;
  ant_hvf_verbosef(vm, "network backend ready (native NAT, forwards=%zu)", nat->forward_count);
  return 0;
}

void ant_hvf_net_stop(ant_hvf_vm_t *vm) {
  ant_hvf_nat_t *nat = vm->net_nat;
  if (!nat) return;
  nat->stop = true;
  ant_nat_wake(nat);
  if (nat->thread_started) pthread_join(nat->thread, NULL);
  for (size_t i = 0; i < nat->tcp_count; i++) {
    if (ant_nat_tcp_active(nat->tcp[i])) ant_nat_close_tcp(nat->tcp[i]);
    ant_nat_free_tcp_buffers(nat->tcp[i]);
    free(nat->tcp[i]);
  }
  for (size_t i = 0; i < nat->forward_count; i++) {
    if (nat->forwards[i].fd >= 0) close(nat->forwards[i].fd);
  }
  if (vm->verbose) {
    ant_hvf_verbosef(vm,
                     "network backend stopped (guest_packets=%llu host_packets=%llu drops=%llu tcp_opened=%llu tcp_slots=%zu)",
                     (unsigned long long)nat->guest_packets,
                     (unsigned long long)nat->host_packets,
                     (unsigned long long)nat->guest_drops,
                     (unsigned long long)nat->tcp_opened,
                     nat->tcp_count);
  }
  free(nat->tcp);
  free(nat->forwards);
  if (nat->wake_pipe[0] >= 0) close(nat->wake_pipe[0]);
  if (nat->wake_pipe[1] >= 0) close(nat->wake_pipe[1]);
  if (nat->lock_init) pthread_mutex_destroy(&nat->lock);
  free(nat);
  vm->net_nat = NULL;
  vm->net_started = false;
}


#endif
