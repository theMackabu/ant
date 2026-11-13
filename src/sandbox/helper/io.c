#include <compat.h> // IWYU pragma: keep
#include "protocol.h"

#include <time.h>

int sandbox_vm_read_full(int fd, void *buf, size_t len) {
  unsigned char *p = buf;
  while (len > 0) {
    ssize_t n = read(fd, p, len);
    if (n == 0) return -EPIPE;
    if (n < 0) {
      if (errno == EINTR) continue;
      return -errno;
    }
    p += (size_t)n;
    len -= (size_t)n;
  }
  return 0;
}

int sandbox_vm_write_full(int fd, const void *buf, size_t len) {
  const unsigned char *p = buf;
  while (len > 0) {
    ssize_t n = write(fd, p, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -errno;
    }
    p += (size_t)n;
    len -= (size_t)n;
  }
  return 0;
}

int sandbox_vm_send_header(int fd, uint8_t type, uint8_t frame_type, uint32_t length) {
  ant_sandbox_vm_helper_header_t header = {
    .magic = ANT_SANDBOX_VM_HELPER_MAGIC,
    .type = type,
    .frame_type = frame_type,
    .reserved = 0,
    .length = length,
  };
  return sandbox_vm_write_full(fd, &header, sizeof(header));
}

uint64_t sandbox_vm_cpu_time_ns(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0) return 0;
  return (uint64_t)value.tv_sec * 1000000000ull + (uint64_t)value.tv_nsec;
}

int sandbox_vm_send_result(
  int fd, uint8_t type, int rc,
  const ant_sandbox_vm_result_t *result
) {
  ant_sandbox_vm_helper_result_t payload = {
    .rc = rc,
    .kind = result ? (int32_t)result->kind : ANT_SANDBOX_VM_RESULT_NONE,
    .code = result ? result->code : 0,
  };
  int send_rc = sandbox_vm_send_header(fd, type, 0, sizeof(payload));
  if (send_rc != 0) return send_rc;
  return sandbox_vm_write_full(fd, &payload, sizeof(payload));
}
