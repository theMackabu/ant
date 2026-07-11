#include "kvm_internal.h" // IWYU pragma: keep
#if defined(__linux__)

static void ant_kvm_noop_signal(int sig) {
  (void)sig;
}

void ant_kvm_install_wakeup_signal(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = ant_kvm_noop_signal;
  sigemptyset(&sa.sa_mask);
  (void)sigaction(SIGUSR1, &sa, NULL);
}

uint64_t ant_kvm_elapsed_ms(const struct timespec *start, const struct timespec *now) {
  uint64_t elapsed = (uint64_t)(now->tv_sec - start->tv_sec) * 1000ull;
  if (now->tv_nsec >= start->tv_nsec) {
    elapsed += (uint64_t)(now->tv_nsec - start->tv_nsec) / 1000000ull;
  } else elapsed -= (uint64_t)(start->tv_nsec - now->tv_nsec) / 1000000ull;
  return elapsed;
}

uint64_t ant_kvm_cpu_time_ns(ant_hvf_vm_t *vm) {
  if (!vm) return 0;
  if (!atomic_load_explicit(&vm->cpu_run_active, memory_order_acquire))
    return atomic_load_explicit(&vm->cpu_time_ns, memory_order_acquire);
  struct timespec now;
  if (clock_gettime(vm->vcpu_clock_id, &now) != 0)
    return atomic_load_explicit(&vm->cpu_time_ns, memory_order_acquire);
  uint64_t current = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
  uint64_t start = atomic_load_explicit(&vm->cpu_run_start_ns, memory_order_acquire);
  uint64_t base = atomic_load_explicit(&vm->cpu_run_base_ns, memory_order_acquire);
  return base + (current >= start ? current - start : 0);
}

int ant_kvm_session_send(void *opaque, const void *data, size_t len) {
  if (!opaque) return -EINVAL;
  ant_kvm_session_t *session = opaque;
  return ant_hvf_vsock_queue_frame(&session->vm, data, len);
}

void *ant_kvm_deadline_thread(void *opaque) {
  ant_kvm_deadline_t *deadline = opaque;
  const struct timespec tick = { .tv_sec = 0, .tv_nsec = 1000000 };
  for (;;) {
    if (atomic_load_explicit(&deadline->stop, memory_order_acquire) || !deadline->vm ||
        atomic_load_explicit(&deadline->vm->canceled, memory_order_acquire)) {
      return NULL;
    }
    bool wall_disarmed = deadline->timeout_until_request_sent &&
      atomic_load_explicit(&deadline->vm->timeout_disarmed, memory_order_acquire);
    if (wall_disarmed) {
      if (deadline->vm->cpu_time_ms == 0) return NULL;
    }
    if (deadline->vm->cpu_time_ms > 0 &&
        ant_kvm_cpu_time_ns(deadline->vm) >=
          (uint64_t)deadline->vm->cpu_time_ms * 1000000ull) {
      atomic_store_explicit(&deadline->vm->cpu_timed_out, true, memory_order_release);
      ant_hvf_wake_vcpu(deadline->vm);
      return NULL;
    }
    struct timespec now;
    if (!wall_disarmed && deadline->timeout_ms > 0 &&
        clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
        ant_kvm_elapsed_ms(&deadline->start, &now) >= deadline->timeout_ms) {
      atomic_store_explicit(&deadline->vm->timed_out, true, memory_order_release);
      ant_hvf_wake_vcpu(deadline->vm);
      return NULL;
    }
    nanosleep(&tick, NULL);
  }
}

void ant_kvm_set_result(
  ant_sandbox_vm_result_t *result,
  ant_sandbox_vm_result_kind_t kind,
  int code
) {
  if (!result) return;
  result->kind = kind;
  result->code = code;
}

void ant_kvm_classify_result(ant_hvf_vm_t *vm, ant_sandbox_vm_result_t *result, int rc) {
  if (!result) return;
  if (vm && vm->vsock.exit_received)
    ant_kvm_set_result(result, ANT_SANDBOX_VM_RESULT_GUEST_EXIT, vm->vsock.exit_code);
  else if (vm && ant_hvf_uart_has_panic(vm))
    ant_kvm_set_result(result, ANT_SANDBOX_VM_RESULT_KERNEL_PANIC, rc ? rc : -EFAULT);
  else if (rc == -ENOSYS)
    ant_kvm_set_result(result, ANT_SANDBOX_VM_RESULT_BACKEND_UNAVAILABLE, rc);
  else if (rc == ANT_SANDBOX_CPU_TIME_LIMIT_CODE ||
           (vm && atomic_load_explicit(&vm->cpu_timed_out, memory_order_acquire)))
    ant_kvm_set_result(result, ANT_SANDBOX_VM_RESULT_CPU_TIME_LIMIT,
                       rc ? rc : ANT_SANDBOX_CPU_TIME_LIMIT_CODE);
  else if (rc == -ETIMEDOUT)
    ant_kvm_set_result(result, ANT_SANDBOX_VM_RESULT_TIMEOUT, rc);
  else if (rc == -ECANCELED || (vm && atomic_load_explicit(&vm->canceled, memory_order_acquire)))
    ant_kvm_set_result(result, ANT_SANDBOX_VM_RESULT_CANCELED, rc ? rc : -ECANCELED);
  else if (vm && vm->vsock.protocol_error)
    ant_kvm_set_result(result, ANT_SANDBOX_VM_RESULT_PROTOCOL_ERROR, rc);
  else if (vm && vm->vsock.transport_error)
    ant_kvm_set_result(result, ANT_SANDBOX_VM_RESULT_TRANSPORT_ERROR, rc);
  else if (rc == -EINVAL)
    ant_kvm_set_result(result, ANT_SANDBOX_VM_RESULT_CONFIG_ERROR, rc);
  else if (rc != 0)
    ant_kvm_set_result(result, ANT_SANDBOX_VM_RESULT_VM_ERROR, rc);
}

static void ant_kvm_verbose_prefix(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    fputs("[0.000000] ant: ", stderr);
    return;
  }
  fprintf(stderr, "[%lld.%06ld] ant: ", (long long)ts.tv_sec, ts.tv_nsec / 1000);
}

void ant_hvf_verbose(ant_hvf_vm_t *vm, const char *message) {
  if (!vm || !vm->verbose) return;
  ant_kvm_verbose_prefix();
  fputs(message, stderr);
  fputc('\n', stderr);
}

void ant_hvf_verbosef(ant_hvf_vm_t *vm, const char *fmt, ...) {
  if (!vm || !vm->verbose) return;
  ant_kvm_verbose_prefix();
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

#if defined(__x86_64__)
int ant_hvf_send_msi(ant_hvf_vm_t *vm, uint64_t addr, uint32_t data) {
  struct kvm_msi msi = {
    .address_lo = (uint32_t)addr,
    .address_hi = (uint32_t)(addr >> 32),
    .data = data,
  };
  if (ioctl(vm->vm_fd, KVM_SIGNAL_MSI, &msi) < 0) return -errno;
  return 0;
}
#endif

void ant_hvf_wake_vcpu(ant_hvf_vm_t *vm) {
  if (vm && atomic_load_explicit(&vm->vcpu_thread_valid, memory_order_acquire)) {
    if (vm->run) __atomic_store_n(&vm->run->immediate_exit, 1, __ATOMIC_RELEASE);
    pthread_kill(vm->vcpu_thread, SIGUSR1);
  }
}

static size_t ant_hvf_uart_limit(ant_hvf_vm_t *vm) {
  if (vm->uart.limit > 0) return vm->uart.limit;
  size_t limit = 16u * 1024u;
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
  return 
    ant_hvf_uart_contains(vm, "Illegal instruction in kernel mode") ||
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

int ant_kvm_ioctl(int fd, unsigned long req, void *arg, const char *op) {
  if (ioctl(fd, req, arg) == 0) return 0;
  int rc = -errno;
  fprintf(stderr, "sandbox vm: %s failed: %s\n", op, strerror(errno));
  return rc;
}

int ant_kvm_find_symbol(const char *path, const char *name, uint64_t *value_out) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -errno;
  ant_elf64_ehdr_t eh;
  int rc = ant_read_all(fd, &eh, sizeof(eh), 0);
  if (rc != 0) goto done;
  if (eh.shoff == 0 || eh.shentsize < sizeof(ant_elf64_shdr_t)) {
    rc = -ENOENT;
    goto done;
  }
  for (uint16_t i = 0; i < eh.shnum; i++) {
    ant_elf64_shdr_t sh;
    rc = ant_read_all(fd, &sh, sizeof(sh), (off_t)(eh.shoff + (uint64_t)i * eh.shentsize));
    if (rc != 0) goto done;
    if (sh.type != 2u || sh.entsize < sizeof(ant_elf64_sym_t) || sh.link >= eh.shnum) continue;

    ant_elf64_shdr_t strtab;
    rc = ant_read_all(fd, &strtab, sizeof(strtab), (off_t)(eh.shoff + (uint64_t)sh.link * eh.shentsize));
    if (rc != 0) goto done;
    if (strtab.size == 0 || strtab.size > 16ull * 1024ull * 1024ull) continue;
    char *strings = malloc((size_t)strtab.size);
    if (!strings) {
      rc = -ENOMEM;
      goto done;
    }
    rc = ant_read_all(fd, strings, (size_t)strtab.size, (off_t)strtab.offset);
    if (rc != 0) {
      free(strings);
      goto done;
    }
    uint64_t count = sh.size / sh.entsize;
    for (uint64_t j = 0; j < count; j++) {
      ant_elf64_sym_t sym;
      rc = ant_read_all(fd, &sym, sizeof(sym), (off_t)(sh.offset + j * sh.entsize));
      if (rc != 0) {
        free(strings);
        goto done;
      }
      if (sym.name >= strtab.size) continue;
      if (strcmp(strings + sym.name, name) == 0) {
        *value_out = sym.value;
        free(strings);
        rc = 0;
        goto done;
      }
    }
    free(strings);
  }
  rc = -ENOENT;

done:
  close(fd);
  return rc;
}

#endif
