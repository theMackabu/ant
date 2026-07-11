#include "kvm_internal.h" // IWYU pragma: keep
#if defined(__linux__) && defined(__x86_64__)

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

static void ant_kvm_segment(
  struct kvm_segment *seg,
  uint16_t selector,
  uint8_t type, bool code
) {
  memset(seg, 0, sizeof(*seg));
  seg->base = 0;
  seg->limit = 0xffffffffu;
  seg->selector = selector;
  seg->type = type;
  seg->present = 1;
  seg->dpl = 0;
  seg->db = 1;
  seg->s = 1;
  seg->l = 0;
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
    if (vm->verbose) ant_hvf_verbosef(vm,
      "unsupported MMIO %s addr=0x%llx len=%u",
      run->mmio.is_write ? "write" : "read",
      (unsigned long long)run->mmio.phys_addr,
      run->mmio.len
    );
    if (!run->mmio.is_write) memset(run->mmio.data, 0xff, run->mmio.len);
    return 0;
  }
  uint64_t off = run->mmio.phys_addr - dev->bar0;
  if (run->mmio.is_write) {
    uint64_t value = 0;
    memcpy(&value, run->mmio.data, run->mmio.len);
    return ant_hvf_virtio_common_write(vm, dev, off, run->mmio.len, value) ? 0 : -EIO;
  }
  uint64_t value = 0;
  if (!ant_hvf_virtio_common_read(vm, dev, off, run->mmio.len, &value)) return -EIO;
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
  if (!have_deadline) deadline.timeout_ms = 0;
  vm->vcpu_thread = pthread_self();
  atomic_store_explicit(&vm->vcpu_thread_valid, true, memory_order_release);
  atomic_store_explicit(&vm->timeout_disarmed, false, memory_order_release);

  struct timespec cpu_started;
  int cpu_clock_rc = pthread_getcpuclockid(vm->vcpu_thread, &vm->vcpu_clock_id);
  if (cpu_clock_rc != 0 || clock_gettime(vm->vcpu_clock_id, &cpu_started) != 0) {
    atomic_store_explicit(&vm->vcpu_thread_valid, false, memory_order_release);
    return cpu_clock_rc != 0 ? -cpu_clock_rc : -errno;
  }
  uint64_t cpu_start_ns = (uint64_t)cpu_started.tv_sec * 1000000000ull + (uint64_t)cpu_started.tv_nsec;
  atomic_store_explicit(&vm->cpu_run_base_ns,
                        atomic_load_explicit(&vm->cpu_time_ns, memory_order_acquire),
                        memory_order_release);
  atomic_store_explicit(&vm->cpu_run_start_ns, cpu_start_ns, memory_order_release);
  atomic_store_explicit(&vm->cpu_run_active, true, memory_order_release);

  if ((have_deadline || vm->cpu_time_ms > 0) &&
      pthread_create(&deadline_thread, NULL, ant_kvm_deadline_thread, &deadline) == 0) {
    deadline_thread_started = true;
  } else if (vm->cpu_time_ms > 0) {
    rc = -EAGAIN;
    goto done;
  }

  for (;;) {
    rc = ant_hvf_vsock_maybe_send_request(vm);
    if (rc != 0 && rc != -EAGAIN) break;
    if (atomic_load_explicit(&vm->cpu_timed_out, memory_order_acquire)) {
      rc = ANT_SANDBOX_CPU_TIME_LIMIT_CODE;
      break;
    }
    if (atomic_load_explicit(&vm->canceled, memory_order_acquire)) {
      rc = -ECANCELED;
      break;
    }
    if (atomic_load_explicit(&vm->timed_out, memory_order_acquire)) {
      rc = -ETIMEDOUT;
      break;
    }
    if (timeout_until_request_sent &&
        atomic_load_explicit(&vm->vsock.request_sent, memory_order_acquire)) {
      atomic_store_explicit(&vm->timeout_disarmed, true, memory_order_release);
    }
    if (!deadline_thread_started && have_deadline &&
        (!timeout_until_request_sent ||
         !atomic_load_explicit(&vm->timeout_disarmed, memory_order_acquire))) {
      struct timespec now;
      if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
          ant_kvm_elapsed_ms(&deadline.start, &now) >= timeout_ms) {
        atomic_store_explicit(&vm->timed_out, true, memory_order_release);
        rc = -ETIMEDOUT;
        break;
      }
    }
    rc = ant_hvf_virtio_net_drain_rx_if_wake(vm);
    if (rc != 0) break;

    __atomic_store_n(&vm->run->immediate_exit, 0, __ATOMIC_RELEASE);
    atomic_store_explicit(&vm->vcpu_running, true, memory_order_release);
    if (atomic_exchange_explicit(&vm->vsock_wake_pending, false, memory_order_acq_rel)) {
      atomic_store_explicit(&vm->vcpu_running, false, memory_order_release);
      continue;
    }
    rc = ioctl(vm->vcpu_fd, KVM_RUN, 0);
    atomic_store_explicit(&vm->vcpu_running, false, memory_order_release);
    if (rc < 0) {
      if (errno == EINTR) {
        if (atomic_load_explicit(&vm->cpu_timed_out, memory_order_acquire)) {
          rc = ANT_SANDBOX_CPU_TIME_LIMIT_CODE;
          break;
        }
        if (atomic_load_explicit(&vm->canceled, memory_order_acquire)) {
          rc = -ECANCELED;
          break;
        }
        if (atomic_load_explicit(&vm->timed_out, memory_order_acquire)) {
          rc = -ETIMEDOUT;
          break;
        }
        rc = ant_hvf_virtio_net_drain_rx_if_wake(vm);
        if (rc != 0) break;
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
        fprintf(
          stderr,
          "sandbox vm: KVM fail entry hardware_entry_failure_reason=0x%llx\n",
          (unsigned long long)vm->run->fail_entry.hardware_entry_failure_reason
        );
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
  atomic_store_explicit(&deadline.stop, true, memory_order_release);
  if (deadline_thread_started) pthread_join(deadline_thread, NULL);
  struct timespec cpu_finished;
  if (clock_gettime(vm->vcpu_clock_id, &cpu_finished) == 0) {
    uint64_t end_ns = (uint64_t)cpu_finished.tv_sec * 1000000000ull + (uint64_t)cpu_finished.tv_nsec;
    uint64_t base = atomic_load_explicit(&vm->cpu_run_base_ns, memory_order_acquire);
    uint64_t start = atomic_load_explicit(&vm->cpu_run_start_ns, memory_order_acquire);
    atomic_store_explicit(&vm->cpu_time_ns, base + (end_ns >= start ? end_ns - start : 0), memory_order_release);
  }
  atomic_store_explicit(&vm->cpu_run_active, false, memory_order_release);
  atomic_store_explicit(&vm->vcpu_thread_valid, false, memory_order_release);
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
  if (vm->virtio_lock_init) pthread_mutex_destroy(&vm->virtio_lock);
  ant_hvf_vsock_clear_frames(vm);
  if (vm->vsock_lock_init) pthread_mutex_destroy(&vm->vsock_lock);
  if (rc_inout) *rc_inout = rc;
}

static int ant_kvm_init_devices(ant_hvf_vm_t *vm, const ant_sandbox_vm_config_t *config) {
  ant_hvf_virtio_init(
    &vm->blk,
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
    ANT_VIRTIO_BLK_CONFIG_LEN
  );
  
  ant_hvf_virtio_init(
    &vm->net,
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
    8
  );
  
  memcpy(vm->net_mac, (uint8_t[]){ 0x02, 0x41, 0x4e, 0x54, 0x00, 0x01 }, sizeof(vm->net_mac));
  vm->net_max_packet_size = 1518u;
  
  ant_hvf_virtio_init(
    &vm->vsock.virtio,
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
    8
  );
  
  ant_hvf_virtio_init(
    &vm->rng,
    ANT_HVF_VIRTIO_KIND_RNG,
    "virtio-rng",
    ANT_VIRTIO_PCI_SUBDEVICE_ENTROPY,
    ANT_VIRTIO_PCI_SUBDEVICE_ENTROPY,
    ANT_HVF_VIRTIO_RNG_SLOT,
    0xff,
    0x00,
    (uint32_t)ANT_HVF_VIRTIO_RNG_BAR,
    0,
    1,
    ANT_VIRTIO_RNG_QUEUE_SIZE,
    0
  );
  
  vm->vsock.capabilities = config->capabilities;
  for (size_t i = 0; i < vm->p9_count; i++) {
    ant_hvf_virtio_init(
      &vm->p9[i].virtio,
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
      (uint16_t)(2u + strlen(config->mounts[i].tag))
    );
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
  if ((config->mount_count > 0 && !config->mounts) ||
      (config->forward_count > 0 && !config->forwards)) {
    ant_kvm_set_result(config->result, ANT_SANDBOX_VM_RESULT_CONFIG_ERROR, -EINVAL);
    return -EINVAL;
  }

  unsigned long long requested_memory =
    config->memory_size ? config->memory_size : ANT_SANDBOX_DEFAULT_MEMORY_SIZE;
  if (requested_memory > SIZE_MAX ||
      (size_t)requested_memory > SIZE_MAX - ((size_t)ANT_HVF_PAGE_SIZE - 1u)) {
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
  vm->mem_size = (size_t)requested_memory;
  vm->mem_size = ant_align_page(vm->mem_size);
  vm->image_sectors = (uint64_t)image_size / 512ull;
  vm->net_enabled = config->network_enabled;
  vm->net_forwards = config->forwards;
  vm->net_forward_count = config->forward_count;
  vm->p9_count = config->mount_count;
  vm->verbose = config->verbose;
  vm->timeout_ms = config->timeout_ms;
  vm->boot_timeout_ms = config->boot_timeout_ms;
  vm->cpu_time_ms = config->cpu_time_ms;
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
    vm->net_forward_count
  );
  
  for (size_t i = 0; i < config->mount_count; i++) ant_hvf_verbosef(vm,
    "mount[%zu] host=%s guest=%s tag=%s %s", i,
    config->mounts[i].host_path,
    config->mounts[i].guest_path,
    config->mounts[i].tag,
    config->mounts[i].readonly ? "ro" : "rw"
  );
  
  for (size_t i = 0; i < config->forward_count; i++) ant_hvf_verbosef(vm,
    "forward[%zu] host=%u guest=%u", i,
    config->forwards[i].host_port,
    config->forwards[i].guest_port
  );

  if (pthread_mutex_init(&vm->net_lock, NULL) == 0) vm->net_lock_init = true;
  else if (vm->net_enabled) {
    rc = -errno;
    goto fail;
  }
  int virtio_lock_rc = pthread_mutex_init(&vm->virtio_lock, NULL);
  if (virtio_lock_rc == 0) vm->virtio_lock_init = true;
  else if (vm->net_enabled) {
    rc = -virtio_lock_rc;
    goto fail;
  }
  int vsock_lock_rc = pthread_mutex_init(&vm->vsock_lock, NULL);
  if (vsock_lock_rc == 0) vm->vsock_lock_init = true;
  else { rc = -vsock_lock_rc; goto fail; }

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
    fprintf(stderr, "sandbox vm: failed to find pvh_start32 in sandbox kernel\n");
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
    "loaded sandbox kernel (%lld bytes) pvh_start32=0x%llx",
    (long long)kernel_size,
    (unsigned long long)vm->pvh_entry32
  );
  
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

  atomic_store_explicit(&vm->vsock.request_sent, false, memory_order_release);
  vm->vsock.exit_received = false;
  vm->vsock.exit_code = 0;
  vm->vsock.protocol_error = false;
  vm->vsock.transport_error = false;
  
  atomic_store_explicit(&vm->timed_out, false, memory_order_release);
  atomic_store_explicit(&vm->cpu_timed_out, false, memory_order_release);
  atomic_store_explicit(&vm->canceled, false, memory_order_release);
  atomic_store_explicit(&vm->timeout_disarmed, false, memory_order_release);
  
  vm->frame_handler = request->frame_handler;
  vm->frame_handler_user = request->frame_handler_user;
  int queue_rc = ant_hvf_vsock_queue_frame(vm, request->request_data, request->request_len);
  if (queue_rc != 0) {
    ant_kvm_classify_result(vm, request->result, queue_rc);
    return queue_rc;
  }

  const uint8_t *frame = request->request_data;
  bool close_request = 
    request->request_len >= ANT_SANDBOX_FRAME_HEADER_SIZE &&
    memcmp(frame, ANT_SANDBOX_FRAME_MAGIC, 4) == 0 &&
    frame[4] == ANT_SANDBOX_FRAME_VERSION &&
    frame[5] == ANT_SANDBOX_FRAME_CLOSE;
    
  unsigned int timeout_ms = vm->timeout_ms ? vm->timeout_ms : vm->boot_timeout_ms;
  bool timeout_until_request_sent = !close_request && vm->timeout_ms == 0 && vm->boot_timeout_ms > 0;
  
  if (timeout_ms > 0 && timeout_until_request_sent)
    ant_hvf_verbosef(vm, "running guest request-timeout=%u ms", timeout_ms);
  else if (timeout_ms > 0)
    ant_hvf_verbosef(vm, "running guest timeout=%u ms", timeout_ms);
  else ant_hvf_verbose(vm, "running guest timeout=disabled");

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

  vm->frame_handler = NULL;
  vm->frame_handler_user = NULL;
  
  return rc;
}

static int ant_kvm_session_stats(void *opaque, ant_sandbox_vm_stats_t *stats) {
  if (!opaque || !stats) return -EINVAL;
  ant_kvm_session_t *session = opaque;
  stats->cpu_time_ns = ant_kvm_cpu_time_ns(&session->vm);
  return 0;
}

static int ant_kvm_session_cancel(void *opaque) {
  if (!opaque) return -EINVAL;
  ant_kvm_session_t *session = opaque;
  atomic_store_explicit(&session->vm.canceled, true, memory_order_release);
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
  .send_session = ant_kvm_session_send,
  .get_stats_session = ant_kvm_session_stats,
  .cancel_session = ant_kvm_session_cancel,
  .destroy_session = ant_kvm_session_destroy,
};

#endif
