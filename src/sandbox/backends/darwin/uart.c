#include "backend.h"

#if defined(__aarch64__)

static size_t ant_hvf_uart_limit(ant_hvf_vm_t *vm) {
  if (vm->uart.limit > 0) return vm->uart.limit;

  size_t limit = ANT_HVF_UART_DIAGNOSTIC_DEFAULT_BYTES;
  const char *env = getenv("ANT_SANDBOX_VM_CONSOLE_CAPTURE_BYTES");
  if (env && env[0]) {
    unsigned long long parsed = strtoull(env, NULL, 10);
    if (parsed > 0 && parsed <= SIZE_MAX) limit = (size_t)parsed;
  }

  vm->uart.limit = limit;
  return limit;
}

void ant_hvf_uart_discard(ant_hvf_vm_t *vm) {
  if (!vm) return;
  free(vm->uart.data);
  memset(&vm->uart, 0, sizeof(vm->uart));
}

static bool ant_hvf_uart_contains(ant_hvf_vm_t *vm, const char *needle) {
  if (!vm || !needle || !needle[0] || vm->uart.len == 0) return false;
  size_t needle_len = strlen(needle);
  if (needle_len > vm->uart.len) return false;

  for (size_t i = 0; i <= vm->uart.len - needle_len; i++) {
    if (memcmp(vm->uart.data + i, needle, needle_len) == 0) return true;
  }
  return false;
}

bool ant_hvf_uart_has_panic(ant_hvf_vm_t *vm) {
  return ant_hvf_uart_contains(vm, "Illegal instruction in kernel mode") ||
         ant_hvf_uart_contains(vm, "no fault handler for frame") ||
         ant_hvf_uart_contains(vm, "assertion ") ||
         ant_hvf_uart_contains(vm, "kernel panic") ||
         ant_hvf_uart_contains(vm, "Kernel panic") ||
         ant_hvf_uart_contains(vm, "PANIC");
}

void ant_hvf_uart_report_panic(ant_hvf_vm_t *vm) {
  if (!vm || vm->uart.len == 0) return;
  fprintf(stderr, "SandboxKernelPanic: guest kernel panic\n");
  if (vm->uart.truncated) fprintf(stderr, "[guest console truncated]\n");

  size_t off = 0;
  while (off < vm->uart.len) {
    ssize_t n = write(STDERR_FILENO, vm->uart.data + off, vm->uart.len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;
    off += (size_t)n;
  }
  if (vm->uart.data[vm->uart.len - 1] != '\n') fputc('\n', stderr);
  ant_hvf_uart_discard(vm);
}

void ant_hvf_uart_put(ant_hvf_vm_t *vm, uint8_t byte) {
  if (!vm) return;

  if (vm->verbose) {
    while (write(STDERR_FILENO, &byte, 1) < 0 && errno == EINTR) {}
  }

  size_t limit = ant_hvf_uart_limit(vm);
  if (limit == 0) return;

  if (vm->uart.len == vm->uart.cap && vm->uart.cap < limit) {
    size_t next = vm->uart.cap ? vm->uart.cap * 2 : 1024;
    if (next < vm->uart.cap || next > limit) next = limit;
    uint8_t *data = realloc(vm->uart.data, next);
    if (!data) {
      vm->uart.truncated = true;
      return;
    }
    vm->uart.data = data;
    vm->uart.cap = next;
  }

  if (vm->uart.len < vm->uart.cap) {
    vm->uart.data[vm->uart.len++] = byte;
    return;
  }

  if (vm->uart.cap > 1) memmove(vm->uart.data, vm->uart.data + 1, vm->uart.cap - 1);
  if (vm->uart.cap > 0) vm->uart.data[vm->uart.cap - 1] = byte;
  vm->uart.len = vm->uart.cap;
  vm->uart.truncated = true;
}

#endif
