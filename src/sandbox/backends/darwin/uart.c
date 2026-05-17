#include "backend.h"

#if defined(__aarch64__)

void ant_hvf_uart_flush(ant_hvf_vm_t *vm) {
  size_t off = 0;
  while (off < vm->uart_len) {
    ssize_t n = write(STDOUT_FILENO, vm->uart_buf + off, vm->uart_len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;
    off += (size_t)n;
  }
  vm->uart_len = 0;
}

void ant_hvf_uart_put(ant_hvf_vm_t *vm, uint8_t byte) {
  vm->uart_buf[vm->uart_len++] = byte;
  if (vm->uart_len == sizeof(vm->uart_buf)) ant_hvf_uart_flush(vm);
}

#endif
