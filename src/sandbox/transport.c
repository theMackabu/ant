#include <compat.h> // IWYU pragma: keep

#include "sandbox/transport.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
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

static char *transport_read_vsock(size_t *len_out) {
  int fd = socket(AF_VSOCK, SOCK_STREAM, 0);
  if (fd < 0) return NULL;

  struct ant_sockaddr_vm addr;
  memset(&addr, 0, sizeof(addr));
  addr.svm_family = AF_VSOCK;
  addr.svm_cid = VMADDR_CID_HOST;
  addr.svm_port = ANT_SANDBOX_TRANSPORT_VSOCK_PORT;

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return NULL;
  }

  size_t cap = 4096;
  size_t len = 0;
  char *buf = malloc(cap + 1);
  if (!buf) {
    close(fd);
    errno = ENOMEM;
    return NULL;
  }

  for (;;) {
    if (len == cap) {
      cap *= 2;
      char *next = realloc(buf, cap + 1);
      if (!next) {
        free(buf);
        close(fd);
        errno = ENOMEM;
        return NULL;
      }
      buf = next;
    }

    ssize_t n = recv(fd, buf + len, cap - len, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      int saved = errno;
      free(buf);
      close(fd);
      errno = saved;
      return NULL;
    }
    if (n == 0) break;

    char *newline = memchr(buf + len, '\n', (size_t)n);
    len += (size_t)n;
    if (newline) {
      len = (size_t)(newline - buf);
      break;
    }
  }

  close(fd);
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) len--;
  buf[len] = '\0';
  *len_out = len;
  return buf;
}

bool ant_sandbox_read_request_transport(ant_sandbox_request_t *out) {
  size_t json_len = 0;
  char *json = transport_read_vsock(&json_len);

  if (!json) {
    fprintf(stderr, "sandbox daemon: failed to read request from vsock: %s\n", strerror(errno));
    return false;
  }

  if (json_len == 0) {
    fprintf(stderr, "sandbox daemon: empty request from vsock\n");
    free(json);
    return false;
  }

  bool ok = ant_sandbox_parse_request_json(json, json_len, out);
  free(json);
  return ok;
}
