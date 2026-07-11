#include <compat.h> // IWYU pragma: keep

#include "sandbox/transport.h"

#include "output.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32

bool ant_sandbox_transport_send_frame(ant_sandbox_frame_type_t type, const void *payload, size_t payload_len) {
  (void)type;
  (void)payload;
  (void)payload_len;
  errno = ENOSYS;
  return false;
}

bool ant_sandbox_transport_send_exit(int code) {
  (void)code;
  errno = ENOSYS;
  return false;
}

bool ant_sandbox_transport_install_output_frames(void) {
  errno = ENOSYS;
  return false;
}

int ant_sandbox_transport_fd(void) {
  return -1;
}

bool ant_sandbox_read_request_transport(ant_sandbox_request_t *out) {
  (void)out;
  errno = ENOSYS;
  fprintf(stderr, "sandbox daemon: native Windows sandbox transport is not implemented; use WSL for sandbox support\n");
  return false;
}

#else

#include <sys/socket.h>
#include <unistd.h>

#ifndef AF_VSOCK
#define AF_VSOCK 40
#endif

#ifndef VMADDR_CID_HOST
#define VMADDR_CID_HOST ANT_SANDBOX_TRANSPORT_VSOCK_HOST_CID
#endif

struct ant_sockaddr_vm {
  unsigned short svm_family;
  unsigned short svm_rsvd;
  unsigned int svm_port;
  unsigned int svm_cid;
  unsigned char svm_zero[4];
};

static int g_transport_fd = -1;

static uint32_t transport_load32(const uint8_t *in) {
  return
    (uint32_t)in[0]         |
    ((uint32_t)in[1] << 8)  |
    ((uint32_t)in[2] << 16) |
    ((uint32_t)in[3] << 24);
}

static bool transport_read_exact(int fd, uint8_t *buf, size_t len) {
  size_t done = 0;
  while (done < len) {
    ssize_t n = recv(fd, buf + done, len - done, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) {
      errno = ECONNRESET;
      return false;
    }
    done += (size_t)n;
  }
  return true;
}

static int transport_connect_vsock(void) {
  if (g_transport_fd >= 0) return 0;

  int fd = socket(AF_VSOCK, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct ant_sockaddr_vm addr;
  memset(&addr, 0, sizeof(addr));
  addr.svm_family = AF_VSOCK;
  addr.svm_cid = VMADDR_CID_HOST;
  addr.svm_port = ANT_SANDBOX_TRANSPORT_VSOCK_PORT;

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
  }

  g_transport_fd = fd;
  return 0;
}

static uint8_t *transport_read_vsock(size_t *len_out) {
  if (transport_connect_vsock() != 0) return NULL;

  uint8_t header[12];
  if (!transport_read_exact(g_transport_fd, header, sizeof(header))) {
    int saved = errno;
    errno = saved;
    return NULL;
  }

  uint32_t payload_len = transport_load32(header + 8);
  size_t frame_len = sizeof(header) + (size_t)payload_len;
  if (payload_len > 16u * 1024u * 1024u || frame_len < sizeof(header)) {
    errno = E2BIG;
    return NULL;
  }

  uint8_t *buf = malloc(frame_len);
  if (!buf) {
    errno = ENOMEM;
    return NULL;
  }

  memcpy(buf, header, sizeof(header));
  if (!transport_read_exact(g_transport_fd, buf + sizeof(header), payload_len)) {
    int saved = errno;
    free(buf);
    errno = saved;
    return NULL;
  }
  *len_out = frame_len;
  return buf;
}

bool ant_sandbox_transport_send_frame(ant_sandbox_frame_type_t type, const void *payload, size_t payload_len) {
  if (transport_connect_vsock() != 0) return false;

  size_t frame_len = 0;
  uint8_t *frame = ant_sandbox_build_frame(type, payload, payload_len, &frame_len);
  if (!frame) {
    errno = EINVAL;
    return false;
  }

  size_t done = 0;
  while (done < frame_len) {
    ssize_t n = send(g_transport_fd, frame + done, frame_len - done, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      free(frame);
      return false;
    }
    if (n == 0) {
      free(frame);
      errno = ECONNRESET;
      return false;
    }
    done += (size_t)n;
  }

  free(frame);
  return true;
}

bool ant_sandbox_transport_send_exit(int code) {
  uint8_t payload[4];
  payload[0] = (uint8_t)code;
  payload[1] = (uint8_t)(code >> 8);
  payload[2] = (uint8_t)(code >> 16);
  payload[3] = (uint8_t)(code >> 24);
  return ant_sandbox_transport_send_frame(ANT_SANDBOX_FRAME_EXIT, payload, sizeof(payload));
}

static bool transport_output_writer(FILE *stream, const void *data, size_t len, void *user) {
  return ant_sandbox_transport_send_frame(stream == stderr ? ANT_SANDBOX_FRAME_STDERR : ANT_SANDBOX_FRAME_STDOUT, data, len);
}

bool ant_sandbox_transport_install_output_frames(void) {
  if (transport_connect_vsock() != 0) return false;
  ant_output_set_writer(transport_output_writer, NULL);
  return true;
}

int ant_sandbox_transport_fd(void) {
  return g_transport_fd;
}

bool ant_sandbox_read_request_transport(ant_sandbox_request_t *out) {
  size_t frame_len = 0;
  uint8_t *frame = transport_read_vsock(&frame_len);

  if (!frame) {
    fprintf(stderr, "sandbox daemon: failed to read request from vsock: %s\n", strerror(errno));
    return false;
  }

  if (frame_len == 0) {
    fprintf(stderr, "sandbox daemon: empty request frame from vsock\n");
    free(frame);
    return false;
  }

  bool ok = ant_sandbox_parse_request_frame(frame, frame_len, out);
  free(frame);
  return ok;
}

#endif
