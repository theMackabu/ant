#if defined(__linux__)

#include "backend.h"
#include "sandbox/sandbox.h"

#include <limits.h>

enum {
  ANT_KVM_HVM_START_MAGIC = 0x336ec578u,
  ANT_KVM_HVM_MEMMAP_TYPE_RAM = 1u,
  ANT_KVM_PIO_IN = 0,
  ANT_KVM_PIO_OUT = 1,
};

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t flags;
  uint32_t nr_modules;
  uint64_t modlist_paddr;
  uint64_t cmdline_paddr;
  uint64_t rsdp_paddr;
  uint64_t memmap_paddr;
  uint32_t memmap_entries;
  uint32_t reserved;
} __attribute__((packed)) ant_kvm_hvm_start_info_t;

typedef struct {
  uint64_t addr;
  uint64_t size;
  uint32_t type;
  uint32_t reserved;
} __attribute__((packed)) ant_kvm_hvm_memmap_entry_t;

typedef struct {
  ant_hvf_vm_t vm;
  bool irqchip_created;
  bool memory_registered;
} ant_kvm_session_t;

typedef struct {
  ant_hvf_vm_t *vm;
  struct timespec start;
  unsigned int timeout_ms;
  bool timeout_until_request_sent;
  volatile sig_atomic_t stop;
} ant_kvm_deadline_t;

static void ant_kvm_noop_signal(int sig) {
  (void)sig;
}

static void ant_kvm_install_wakeup_signal(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = ant_kvm_noop_signal;
  sigemptyset(&sa.sa_mask);
  (void)sigaction(SIGUSR1, &sa, NULL);
}

static uint64_t ant_kvm_elapsed_ms(const struct timespec *start, const struct timespec *now) {
  uint64_t elapsed = (uint64_t)(now->tv_sec - start->tv_sec) * 1000ull;
  if (now->tv_nsec >= start->tv_nsec) {
    elapsed += (uint64_t)(now->tv_nsec - start->tv_nsec) / 1000000ull;
  } else {
    elapsed -= (uint64_t)(start->tv_nsec - now->tv_nsec) / 1000000ull;
  }
  return elapsed;
}

static void *ant_kvm_deadline_thread(void *opaque) {
  ant_kvm_deadline_t *deadline = opaque;
  const struct timespec tick = { .tv_sec = 0, .tv_nsec = 1000000 };
  for (;;) {
    if (deadline->stop || !deadline->vm || deadline->vm->canceled) return NULL;
    if (deadline->timeout_until_request_sent && deadline->vm->timeout_disarmed) return NULL;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
        ant_kvm_elapsed_ms(&deadline->start, &now) >= deadline->timeout_ms) {
      deadline->vm->timed_out = true;
      ant_hvf_wake_vcpu(deadline->vm);
      return NULL;
    }
    nanosleep(&tick, NULL);
  }
}

static void ant_kvm_set_result(ant_sandbox_vm_result_t *result,
                               ant_sandbox_vm_result_kind_t kind,
                               int code) {
  if (!result) return;
  result->kind = kind;
  result->code = code;
}

static void ant_kvm_classify_result(ant_hvf_vm_t *vm, ant_sandbox_vm_result_t *result, int rc) {
  if (!result) return;
  if (vm && vm->vsock.exit_received) {
    ant_kvm_set_result(result, ANT_SANDBOX_VM_RESULT_GUEST_EXIT, vm->vsock.exit_code);
  } else if (vm && ant_hvf_uart_has_panic(vm)) {
    ant_kvm_set_result(result, ANT_SANDBOX_VM_RESULT_KERNEL_PANIC, rc ? rc : -EFAULT);
  } else if (rc == -ENOSYS) {
    ant_kvm_set_result(result, ANT_SANDBOX_VM_RESULT_BACKEND_UNAVAILABLE, rc);
  } else if (rc == -ETIMEDOUT) {
    ant_kvm_set_result(result, ANT_SANDBOX_VM_RESULT_TIMEOUT, rc);
  } else if (rc == -ECANCELED || (vm && vm->canceled)) {
    ant_kvm_set_result(result, ANT_SANDBOX_VM_RESULT_CANCELED, rc ? rc : -ECANCELED);
  } else if (vm && vm->vsock.protocol_error) {
    ant_kvm_set_result(result, ANT_SANDBOX_VM_RESULT_PROTOCOL_ERROR, rc);
  } else if (vm && vm->vsock.transport_error) {
    ant_kvm_set_result(result, ANT_SANDBOX_VM_RESULT_TRANSPORT_ERROR, rc);
  } else if (rc == -EINVAL) {
    ant_kvm_set_result(result, ANT_SANDBOX_VM_RESULT_CONFIG_ERROR, rc);
  } else if (rc != 0) {
    ant_kvm_set_result(result, ANT_SANDBOX_VM_RESULT_VM_ERROR, rc);
  }
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

int ant_hvf_send_msi(ant_hvf_vm_t *vm, uint64_t addr, uint32_t data) {
  struct kvm_msi msi = {
    .address_lo = (uint32_t)addr,
    .address_hi = (uint32_t)(addr >> 32),
    .data = data,
  };
  if (ioctl(vm->vm_fd, KVM_SIGNAL_MSI, &msi) < 0) return -errno;
  return 0;
}

void ant_hvf_wake_vcpu(ant_hvf_vm_t *vm) {
  if (vm && vm->vcpu_thread_valid) pthread_kill(vm->vcpu_thread, SIGUSR1);
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

static int ant_kvm_ioctl(int fd, unsigned long req, void *arg, const char *op) {
  if (ioctl(fd, req, arg) == 0) return 0;
  int rc = -errno;
  fprintf(stderr, "sandbox vm: %s failed: %s\n", op, strerror(errno));
  return rc;
}

static int ant_kvm_find_symbol(const char *path, const char *name, uint64_t *value_out) {
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

static int ant_kvm_write_pvh_start(ant_hvf_vm_t *vm) {
  ant_kvm_hvm_start_info_t start = {
    .magic = ANT_KVM_HVM_START_MAGIC,
    .version = 1,
    .memmap_paddr = ANT_HVF_PVH_MEMMAP,
    .memmap_entries = 1,
  };
  ant_kvm_hvm_memmap_entry_t mem = {
    .addr = ANT_HVF_PVH_START_INFO,
    .size = vm->mem_size - ANT_HVF_PVH_START_INFO,
    .type = ANT_KVM_HVM_MEMMAP_TYPE_RAM,
  };
  int rc = ant_hvf_guest_write(vm, ANT_HVF_PVH_START_INFO, &start, sizeof(start));
  if (rc != 0) return rc;
  return ant_hvf_guest_write(vm, ANT_HVF_PVH_MEMMAP, &mem, sizeof(mem));
}

static void ant_kvm_segment(struct kvm_segment *seg,
                            uint16_t selector,
                            uint8_t type,
                            bool code) {
  memset(seg, 0, sizeof(*seg));
  seg->base = 0;
  seg->limit = 0xffffffffu;
  seg->selector = selector;
  seg->type = type;
  seg->present = 1;
  seg->dpl = 0;
  seg->db = 1;
  seg->s = 1;
  seg->l = code ? 0 : 0;
  seg->g = 1;
}

static int ant_kvm_init_vcpu(ant_hvf_vm_t *vm) {
  struct kvm_sregs sregs;
  if (ioctl(vm->vcpu_fd, KVM_GET_SREGS, &sregs) < 0) return -errno;

  ant_kvm_segment(&sregs.cs, 0x08, 0x0b, true);
  ant_kvm_segment(&sregs.ds, 0x10, 0x03, false);
  sregs.es = sregs.ds;
  sregs.fs = sregs.ds;
  sregs.gs = sregs.ds;
  sregs.ss = sregs.ds;
  sregs.cr0 |= 1u;
  if (ioctl(vm->vcpu_fd, KVM_SET_SREGS, &sregs) < 0) return -errno;

  struct kvm_regs regs = {
    .rip = vm->pvh_entry32,
    .rbx = ANT_HVF_PVH_START_INFO,
    .rsp = 0x90000,
    .rflags = 0x2,
  };
  if (ioctl(vm->vcpu_fd, KVM_SET_REGS, &regs) < 0) return -errno;
  return 0;
}

static int ant_kvm_set_cpuid(ant_hvf_vm_t *vm) {
  int nent = 128;
  for (;;) {
    size_t bytes = sizeof(struct kvm_cpuid2) + (size_t)nent * sizeof(struct kvm_cpuid_entry2);
    struct kvm_cpuid2 *cpuid = calloc(1, bytes);
    if (!cpuid) return -ENOMEM;
    cpuid->nent = (uint32_t)nent;
    if (ioctl(vm->kvm_fd, KVM_GET_SUPPORTED_CPUID, cpuid) == 0) {
      int rc = ioctl(vm->vcpu_fd, KVM_SET_CPUID2, cpuid) == 0 ? 0 : -errno;
      free(cpuid);
      return rc;
    }
    int err = errno;
    free(cpuid);
    if (err != E2BIG || nent > 4096) return -err;
    nent *= 2;
  }
}

static uint32_t ant_kvm_pci_cfg_read(ant_hvf_vm_t *vm, uint16_t port, unsigned size) {
  if ((vm->pci_addr & 0x80000000u) == 0 || port < ANT_KVM_PCI_DATA_PORT ||
      port + size > ANT_KVM_PCI_DATA_PORT + 4u) {
    return UINT32_MAX;
  }
  unsigned bus = (vm->pci_addr >> 16) & 0xffu;
  unsigned slot = (vm->pci_addr >> 11) & 0x1fu;
  unsigned fn = (vm->pci_addr >> 8) & 0x7u;
  unsigned reg = (vm->pci_addr & 0xfcu) + (unsigned)(port - ANT_KVM_PCI_DATA_PORT);
  uint32_t word = ant_hvf_pci_config_read32(vm, bus, slot, fn, reg);
  return (uint32_t)ant_hvf_select_width(word, reg & 3u, size);
}

static void ant_kvm_pci_cfg_write(ant_hvf_vm_t *vm, uint16_t port, unsigned size, uint32_t value) {
  if ((vm->pci_addr & 0x80000000u) == 0 || port < ANT_KVM_PCI_DATA_PORT ||
      port + size > ANT_KVM_PCI_DATA_PORT + 4u) {
    return;
  }
  unsigned bus = (vm->pci_addr >> 16) & 0xffu;
  unsigned slot = (vm->pci_addr >> 11) & 0x1fu;
  unsigned fn = (vm->pci_addr >> 8) & 0x7u;
  unsigned reg = (vm->pci_addr & 0xfcu) + (unsigned)(port - ANT_KVM_PCI_DATA_PORT);
  ant_hvf_pci_config_write(vm, bus, slot, fn, reg, size, value);
}

static uint32_t ant_kvm_io_read(ant_hvf_vm_t *vm, uint16_t port, unsigned size) {
  if (port == ANT_KVM_PCI_ADDR_PORT && size == 4) return vm->pci_addr;
  if (port >= ANT_KVM_PCI_DATA_PORT && port < ANT_KVM_PCI_DATA_PORT + 4u) {
    return ant_kvm_pci_cfg_read(vm, port, size);
  }
  if (port >= ANT_KVM_COM1_PORT && port < ANT_KVM_COM1_PORT + 8u) {
    unsigned off = port - ANT_KVM_COM1_PORT;
    if (off == 5) return 0x60u;
    if (off == 6) return 0xb0u;
    return 0;
  }
  if (port == 0x70 || port == 0x71 || port == 0x80) return 0;
  return UINT32_MAX;
}

static void ant_kvm_io_write(ant_hvf_vm_t *vm, uint16_t port, unsigned size, uint32_t value) {
  if (port == ANT_KVM_PCI_ADDR_PORT && size == 4) {
    vm->pci_addr = value;
    return;
  }
  if (port >= ANT_KVM_PCI_DATA_PORT && port < ANT_KVM_PCI_DATA_PORT + 4u) {
    ant_kvm_pci_cfg_write(vm, port, size, value);
    return;
  }
  if (port == ANT_KVM_COM1_PORT) {
    ant_hvf_uart_put(vm, (uint8_t)value);
    return;
  }
}

static int ant_kvm_handle_io(ant_hvf_vm_t *vm) {
  struct kvm_run *run = vm->run;
  unsigned char *data = (unsigned char *)run + run->io.data_offset;
  for (uint32_t i = 0; i < run->io.count; i++) {
    unsigned char *p = data + (size_t)i * run->io.size;
    if (run->io.direction == ANT_KVM_PIO_OUT) {
      uint32_t value = 0;
      memcpy(&value, p, run->io.size);
      ant_kvm_io_write(vm, run->io.port, run->io.size, value);
    } else {
      uint32_t value = ant_kvm_io_read(vm, run->io.port, run->io.size);
      memcpy(p, &value, run->io.size);
    }
  }
  return 0;
}

static int ant_kvm_handle_mmio(ant_hvf_vm_t *vm) {
  struct kvm_run *run = vm->run;
  ant_hvf_virtio_device_t *dev = ant_hvf_virtio_for_bar(vm, run->mmio.phys_addr);
  if (!dev) {
    if (vm->verbose) {
      ant_hvf_verbosef(vm,
                       "unsupported MMIO %s addr=0x%llx len=%u",
                       run->mmio.is_write ? "write" : "read",
                       (unsigned long long)run->mmio.phys_addr,
                       run->mmio.len);
    }
    if (!run->mmio.is_write) memset(run->mmio.data, 0xff, run->mmio.len);
    return 0;
  }
  uint64_t off = run->mmio.phys_addr - dev->bar0;
  bool trace = getenv("ANT_KVM_TRACE_VIRTIO") != NULL;
  if (run->mmio.is_write) {
    uint64_t value = 0;
    memcpy(&value, run->mmio.data, run->mmio.len);
    if (trace) {
      fprintf(stderr,
              "kvm virtio write %s addr=0x%llx off=0x%llx len=%u value=0x%llx\n",
              dev->name,
              (unsigned long long)run->mmio.phys_addr,
              (unsigned long long)off,
              run->mmio.len,
              (unsigned long long)value);
    }
    return ant_hvf_virtio_common_write(vm, dev, off, run->mmio.len, value) ? 0 : -EIO;
  }
  uint64_t value = 0;
  if (!ant_hvf_virtio_common_read(vm, dev, off, run->mmio.len, &value)) return -EIO;
  if (trace) {
    fprintf(stderr,
            "kvm virtio read %s addr=0x%llx off=0x%llx len=%u value=0x%llx\n",
            dev->name,
            (unsigned long long)run->mmio.phys_addr,
            (unsigned long long)off,
            run->mmio.len,
            (unsigned long long)value);
  }
  memcpy(run->mmio.data, &value, run->mmio.len);
  return 0;
}

static int ant_kvm_run_guest(ant_hvf_vm_t *vm, unsigned int timeout_ms, bool timeout_until_request_sent) {
  int rc = 0;
  pthread_t deadline_thread;
  bool deadline_thread_started = false;
  ant_kvm_deadline_t deadline = {
    .vm = vm,
    .timeout_ms = timeout_ms,
    .timeout_until_request_sent = timeout_until_request_sent,
  };
  bool have_deadline = timeout_ms > 0 && clock_gettime(CLOCK_MONOTONIC, &deadline.start) == 0;
  vm->vcpu_thread = pthread_self();
  vm->vcpu_thread_valid = true;
  vm->timeout_disarmed = false;

  if (have_deadline && pthread_create(&deadline_thread, NULL, ant_kvm_deadline_thread, &deadline) == 0) {
    deadline_thread_started = true;
  }

  for (;;) {
    if (vm->canceled) {
      rc = -ECANCELED;
      break;
    }
    if (vm->timed_out) {
      rc = -ETIMEDOUT;
      break;
    }
    if (timeout_until_request_sent && vm->vsock.request_sent) vm->timeout_disarmed = true;
    if (!deadline_thread_started && have_deadline &&
        (!timeout_until_request_sent || !vm->timeout_disarmed)) {
      struct timespec now;
      if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
          ant_kvm_elapsed_ms(&deadline.start, &now) >= timeout_ms) {
        vm->timed_out = true;
        rc = -ETIMEDOUT;
        break;
      }
    }

    rc = ioctl(vm->vcpu_fd, KVM_RUN, 0);
    if (rc < 0) {
      if (errno == EINTR) {
        if (vm->canceled) {
          rc = -ECANCELED;
          break;
        }
        if (vm->timed_out) {
          rc = -ETIMEDOUT;
          break;
        }
        if (vm->net_rx_wake) {
          vm->net_rx_wake = false;
          rc = ant_hvf_virtio_net_drain_rx(vm);
          if (rc != 0) break;
        }
        continue;
      }
      rc = -errno;
      break;
    }

    switch (vm->run->exit_reason) {
      case KVM_EXIT_IO:
        rc = ant_kvm_handle_io(vm);
        break;
      case KVM_EXIT_MMIO:
        rc = ant_kvm_handle_mmio(vm);
        break;
      case KVM_EXIT_HLT:
      case KVM_EXIT_SHUTDOWN:
        rc = 0;
        goto done;
      case KVM_EXIT_FAIL_ENTRY:
        fprintf(stderr,
                "sandbox vm: KVM fail entry hardware_entry_failure_reason=0x%llx\n",
                (unsigned long long)vm->run->fail_entry.hardware_entry_failure_reason);
        rc = -EIO;
        goto done;
      case KVM_EXIT_INTERNAL_ERROR:
        fprintf(stderr, "sandbox vm: KVM internal error suberror=%u\n", vm->run->internal.suberror);
        rc = -EIO;
        goto done;
      default:
        fprintf(stderr, "sandbox vm: unhandled KVM exit reason %u\n", vm->run->exit_reason);
        rc = -EIO;
        goto done;
    }

    if (rc != 0) break;
    if (vm->vsock.exit_received) {
      rc = vm->vsock.exit_code;
      break;
    }
  }

done:
  deadline.stop = true;
  if (deadline_thread_started) pthread_join(deadline_thread, NULL);
  vm->vcpu_thread_valid = false;
  return rc;
}

static void ant_kvm_session_cleanup(ant_kvm_session_t *session, int *rc_inout) {
  if (!session) return;
  ant_hvf_vm_t *vm = &session->vm;
  int rc = rc_inout ? *rc_inout : 0;

  if (!vm->vsock.exit_received && ant_hvf_uart_has_panic(vm)) ant_hvf_uart_report_panic(vm);
  else ant_hvf_uart_discard(vm);
  for (size_t i = 0; i < vm->p9_count; i++) ant_hvf_9p_report_stats(vm, &vm->p9[i], i);
  ant_hvf_net_stop(vm);
  if (vm->run && vm->run != MAP_FAILED) munmap(vm->run, vm->run_mmap_size);
  if (vm->vcpu_fd >= 0) close(vm->vcpu_fd);
  if (vm->vm_fd >= 0) close(vm->vm_fd);
  if (vm->kvm_fd >= 0) close(vm->kvm_fd);
  if (vm->image_fd >= 0) close(vm->image_fd);
  if (vm->host_mem && vm->host_mem != MAP_FAILED) munmap(vm->host_mem, vm->mem_size);
  free(vm->vsock.rx_stream);
  for (size_t i = 0; i < vm->p9_count; i++) {
    free(vm->p9[i].fids);
    ant_hvf_9p_buffers_free(&vm->p9[i]);
    ant_hvf_9p_stat_cache_clear(&vm->p9[i]);
    ant_hvf_9p_file_cache_clear(&vm->p9[i]);
  }
  if (vm->net_lock_init) pthread_mutex_destroy(&vm->net_lock);
  if (rc_inout) *rc_inout = rc;
}

static int ant_kvm_init_devices(ant_hvf_vm_t *vm, const ant_sandbox_vm_config_t *config) {
  ant_hvf_virtio_init(&vm->blk,
                      ANT_HVF_VIRTIO_KIND_BLOCK,
                      "virtio-blk",
                      ANT_VIRTIO_PCI_SUBDEVICE_BLOCK,
                      ANT_VIRTIO_PCI_SUBDEVICE_BLOCK,
                      ANT_HVF_VIRTIO_BLK_SLOT,
                      0x01,
                      0x00,
                      (uint32_t)ANT_HVF_VIRTIO_BLK_BAR,
                      ANT_VIRTIO_BLK_FEATURES,
                      1,
                      ANT_VIRTIO_BLK_QUEUE_SIZE,
                      ANT_VIRTIO_BLK_CONFIG_LEN);
  ant_hvf_virtio_init(&vm->net,
                      ANT_HVF_VIRTIO_KIND_NET,
                      "virtio-net",
                      ANT_VIRTIO_PCI_SUBDEVICE_NET,
                      ANT_VIRTIO_PCI_SUBDEVICE_NET,
                      ANT_HVF_VIRTIO_NET_SLOT,
                      0x02,
                      0x00,
                      (uint32_t)ANT_HVF_VIRTIO_NET_BAR,
                      ANT_VIRTIO_NET_F_MAC,
                      ANT_VIRTIO_NET_QUEUE_COUNT,
                      ANT_VIRTIO_NET_QUEUE_SIZE,
                      8);
  memcpy(vm->net_mac, (uint8_t[]){ 0x02, 0x41, 0x4e, 0x54, 0x00, 0x01 }, sizeof(vm->net_mac));
  vm->net_max_packet_size = 1518u;
  ant_hvf_virtio_init(&vm->vsock.virtio,
                      ANT_HVF_VIRTIO_KIND_VSOCK,
                      "virtio-vsock",
                      ANT_VIRTIO_PCI_SUBDEVICE_VSOCK,
                      ANT_VIRTIO_PCI_SUBDEVICE_VSOCK,
                      ANT_HVF_VIRTIO_VSOCK_SLOT,
                      0x08,
                      0x00,
                      (uint32_t)ANT_HVF_VIRTIO_VSOCK_BAR,
                      ANT_VIRTIO_VSOCK_F_STREAM,
                      ANT_VIRTIO_VSOCK_QUEUE_COUNT,
                      ANT_VIRTIO_VSOCK_QUEUE_SIZE,
                      8);
  vm->vsock.capabilities = config->capabilities;
  for (size_t i = 0; i < vm->p9_count; i++) {
    ant_hvf_virtio_init(&vm->p9[i].virtio,
                        ANT_HVF_VIRTIO_KIND_9P,
                        "virtio-9p",
                        ANT_VIRTIO_PCI_SUBDEVICE_9P,
                        ANT_VIRTIO_PCI_SUBDEVICE_9P,
                        (uint8_t)(ANT_HVF_VIRTIO_9P_SLOT_BASE + i),
                        0x01,
                        0x00,
                        (uint32_t)(ANT_HVF_VIRTIO_9P_BAR_BASE + i * ANT_HVF_VIRTIO_BAR_SIZE),
                        ANT_VIRTIO_9P_F_MOUNT_TAG,
                        1,
                        ANT_VIRTIO_9P_QUEUE_SIZE,
                        (uint16_t)(2u + strlen(config->mounts[i].tag)));
    vm->p9[i].root = config->mounts[i].host_path;
    vm->p9[i].tag = config->mounts[i].tag;
    vm->p9[i].readonly = config->mounts[i].readonly;
    int rc = ant_hvf_9p_buffers_init(&vm->p9[i], ANT_HVF_9P_MAX_MSIZE);
    if (rc != 0) return rc;
  }
  return 0;
}

static int ant_kvm_session_create(const ant_sandbox_vm_config_t *config, void **session_out) {
  if (!config || !session_out) return -EINVAL;
  *session_out = NULL;

  off_t image_size = 0;
  off_t kernel_size = 0;
  int rc = ant_hvf_check_file("image", config->image_path, &image_size);
  if (rc != 0) return rc;
  rc = ant_hvf_check_file("kernel", config->kernel_path, &kernel_size);
  if (rc != 0) return rc;
  if (config->mount_count == 0 || config->mount_count > ANT_HVF_VIRTIO_9P_MAX) {
    ant_kvm_set_result(config->result, ANT_SANDBOX_VM_RESULT_CONFIG_ERROR, -EINVAL);
    return -EINVAL;
  }

  ant_kvm_session_t *session = calloc(1, sizeof(*session));
  if (!session) return -ENOMEM;
  ant_hvf_vm_t *vm = &session->vm;
  vm->kvm_fd = -1;
  vm->vm_fd = -1;
  vm->vcpu_fd = -1;
  vm->image_fd = -1;
  vm->mem_size = config->memory_size ? (size_t)config->memory_size : (1024ull * 1024ull * 1024ull);
  vm->mem_size = ant_align_page(vm->mem_size);
  vm->image_sectors = (uint64_t)image_size / 512ull;
  vm->net_enabled = config->network_enabled;
  vm->net_forwards = config->forwards;
  vm->net_forward_count = config->forward_count;
  vm->p9_count = config->mount_count;
  vm->verbose = config->verbose;
  vm->timeout_ms = config->timeout_ms;
  vm->boot_timeout_ms = config->boot_timeout_ms;
  vm->frame_handler = config->frame_handler;
  vm->frame_handler_user = config->frame_handler_user;

  rc = ant_kvm_init_devices(vm, config);
  if (rc != 0) goto fail;

  ant_hvf_verbosef(vm,
                   "KVM backend image=%s kernel=%s memory=%zu MiB mounts=%zu forwards=%zu",
                   config->image_path,
                   config->kernel_path,
                   vm->mem_size / ((size_t)1024 * 1024),
                   vm->p9_count,
                   vm->net_forward_count);

  if (pthread_mutex_init(&vm->net_lock, NULL) == 0) {
    vm->net_lock_init = true;
  } else if (vm->net_enabled) {
    rc = -errno;
    goto fail;
  }

  if (vm->net_enabled) {
    ant_hvf_verbose(vm, "starting network backend");
    rc = ant_hvf_net_start(vm);
    if (rc != 0) goto fail;
  }

  vm->image_fd = open(config->image_path, O_RDWR);
  if (vm->image_fd < 0) {
    rc = -errno;
    goto fail;
  }

  vm->host_mem = mmap(NULL, vm->mem_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (vm->host_mem == MAP_FAILED) {
    rc = -errno;
    goto fail;
  }

  vm->kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (vm->kvm_fd < 0) {
    rc = -errno;
    goto fail;
  }
  int api = ioctl(vm->kvm_fd, KVM_GET_API_VERSION, 0);
  if (api != KVM_API_VERSION) {
    rc = -ENOSYS;
    goto fail;
  }
  vm->vm_fd = ioctl(vm->kvm_fd, KVM_CREATE_VM, 0);
  if (vm->vm_fd < 0) {
    rc = -errno;
    goto fail;
  }
  int tss = 0xfffbd000;
  (void)ioctl(vm->vm_fd, KVM_SET_TSS_ADDR, tss);
  if (ioctl(vm->vm_fd, KVM_CREATE_IRQCHIP, 0) < 0) {
    rc = -errno;
    goto fail;
  }
  session->irqchip_created = true;
  struct kvm_pit_config pit = { .flags = KVM_PIT_SPEAKER_DUMMY };
  (void)ioctl(vm->vm_fd, KVM_CREATE_PIT2, &pit);

  struct kvm_userspace_memory_region region = {
    .slot = 0,
    .flags = 0,
    .guest_phys_addr = 0,
    .memory_size = vm->mem_size,
    .userspace_addr = (uint64_t)(uintptr_t)vm->host_mem,
  };
  rc = ant_kvm_ioctl(vm->vm_fd, KVM_SET_USER_MEMORY_REGION, &region, "KVM_SET_USER_MEMORY_REGION");
  if (rc != 0) goto fail;
  session->memory_registered = true;

  rc = ant_hvf_load_kernel(vm, config->kernel_path);
  if (rc != 0) goto fail;
  rc = ant_kvm_find_symbol(config->kernel_path, "pvh_start32", &vm->pvh_entry32);
  if (rc != 0) {
    fprintf(stderr, "sandbox vm: failed to find pvh_start32 in Nanos kernel\n");
    goto fail;
  }
  rc = ant_kvm_write_pvh_start(vm);
  if (rc != 0) goto fail;

  vm->vcpu_fd = ioctl(vm->vm_fd, KVM_CREATE_VCPU, 0);
  if (vm->vcpu_fd < 0) {
    rc = -errno;
    goto fail;
  }
  int mmap_size = ioctl(vm->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
  if (mmap_size <= 0) {
    rc = -EIO;
    goto fail;
  }
  vm->run_mmap_size = (size_t)mmap_size;
  vm->run = mmap(NULL, vm->run_mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vm->vcpu_fd, 0);
  if (vm->run == MAP_FAILED) {
    rc = -errno;
    goto fail;
  }
  rc = ant_kvm_set_cpuid(vm);
  if (rc != 0) goto fail;
  rc = ant_kvm_init_vcpu(vm);
  if (rc != 0) goto fail;

  ant_kvm_install_wakeup_signal();

  ant_hvf_verbosef(vm,
                   "loaded Nanos kernel (%lld bytes) pvh_start32=0x%llx",
                   (long long)kernel_size,
                   (unsigned long long)vm->pvh_entry32);
  *session_out = session;
  return 0;

fail:
  ant_kvm_classify_result(vm, config->result, rc);
  ant_kvm_session_cleanup(session, &rc);
  free(session);
  return rc;
}

static int ant_kvm_session_execute(void *opaque, const ant_sandbox_vm_request_t *request) {
  if (!opaque || !request || !request->request_data || request->request_len == 0) return -EINVAL;
  ant_kvm_session_t *session = opaque;
  ant_hvf_vm_t *vm = &session->vm;

  vm->vsock.request_data = request->request_data;
  vm->vsock.request_len = request->request_len;
  vm->vsock.request_off = 0;
  vm->vsock.request_sent = false;
  vm->vsock.exit_received = false;
  vm->vsock.exit_code = 0;
  vm->vsock.protocol_error = false;
  vm->vsock.transport_error = false;
  vm->timed_out = false;
  vm->canceled = false;
  vm->timeout_disarmed = false;
  vm->frame_handler = request->frame_handler;
  vm->frame_handler_user = request->frame_handler_user;

  const uint8_t *frame = request->request_data;
  bool close_request = request->request_len >= ANT_SANDBOX_FRAME_HEADER_SIZE &&
                       memcmp(frame, ANT_SANDBOX_FRAME_MAGIC, 4) == 0 &&
                       frame[4] == ANT_SANDBOX_FRAME_VERSION &&
                       frame[5] == ANT_SANDBOX_FRAME_CLOSE;
  unsigned int timeout_ms = vm->timeout_ms ? vm->timeout_ms : vm->boot_timeout_ms;
  bool timeout_until_request_sent = !close_request && vm->timeout_ms == 0 && vm->boot_timeout_ms > 0;
  if (timeout_ms > 0 && timeout_until_request_sent) {
    ant_hvf_verbosef(vm, "running guest request-timeout=%u ms", timeout_ms);
  } else if (timeout_ms > 0) {
    ant_hvf_verbosef(vm, "running guest timeout=%u ms", timeout_ms);
  } else {
    ant_hvf_verbose(vm, "running guest timeout=disabled");
  }

  int maybe_sent = ant_hvf_vsock_maybe_send_request(vm);
  if (maybe_sent != 0 && maybe_sent != -EAGAIN) {
    vm->vsock.transport_error = true;
    ant_kvm_classify_result(vm, request->result, maybe_sent);
    return maybe_sent;
  }

  int rc = ant_kvm_run_guest(vm, timeout_ms, timeout_until_request_sent);
  if (rc == -ETIMEDOUT) fprintf(stderr, "sandbox vm: guest timed out\n");
  if (!vm->vsock.exit_received && ant_hvf_uart_has_panic(vm) && rc == 0) rc = -EFAULT;
  ant_kvm_classify_result(vm, request->result, rc);

  vm->vsock.request_data = NULL;
  vm->vsock.request_len = 0;
  vm->frame_handler = NULL;
  vm->frame_handler_user = NULL;
  return rc;
}

static int ant_kvm_session_cancel(void *opaque) {
  if (!opaque) return -EINVAL;
  ant_kvm_session_t *session = opaque;
  session->vm.canceled = true;
  ant_hvf_wake_vcpu(&session->vm);
  return 0;
}

static void ant_kvm_session_destroy(void *opaque) {
  if (!opaque) return;
  ant_kvm_session_t *session = opaque;
  (void)ant_kvm_session_cancel(session);
  int rc = 0;
  ant_kvm_session_cleanup(session, &rc);
  free(session);
}

static int ant_kvm_start(const ant_sandbox_vm_config_t *config) {
  void *session = NULL;
  int rc = ant_kvm_session_create(config, &session);
  if (rc == 0) {
    ant_sandbox_vm_request_t request = {
      .request_data = config->request_data,
      .request_len = config->request_len,
      .frame_handler = config->frame_handler,
      .frame_handler_user = config->frame_handler_user,
      .result = config->result,
    };
    rc = ant_kvm_session_execute(session, &request);
  }
  ant_kvm_session_destroy(session);
  return rc;
}

const ant_sandbox_vm_backend_t ant_sandbox_vm_linux_backend = {
  .name = "kvm",
  .start = ant_kvm_start,
  .create_session = ant_kvm_session_create,
  .execute_session = ant_kvm_session_execute,
  .cancel_session = ant_kvm_session_cancel,
  .destroy_session = ant_kvm_session_destroy,
};

#endif
