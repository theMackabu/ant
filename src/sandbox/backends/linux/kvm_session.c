#include "kvm_internal.h" // IWYU pragma: keep
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))

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
  atomic_store_explicit(
    &vm->cpu_run_base_ns,
    atomic_load_explicit(&vm->cpu_time_ns, memory_order_acquire), memory_order_release);
  atomic_store_explicit(&vm->cpu_run_start_ns, cpu_start_ns, memory_order_release);
  atomic_store_explicit(&vm->cpu_run_active, true, memory_order_release);

  rc = ant_kvm_deadline_init(&deadline);
  if (rc != 0) {
    atomic_store_explicit(&vm->cpu_run_active, false, memory_order_release);
    atomic_store_explicit(&vm->vcpu_thread_valid, false, memory_order_release);
    return rc;
  }
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
      ant_kvm_arch_report_timeout(vm);
      rc = -ETIMEDOUT;
      break;
    }
    
    if (timeout_until_request_sent && ant_hvf_vsock_request_sent(vm))
      atomic_store_explicit(&vm->timeout_disarmed, true, memory_order_release);
    
    if (
      !deadline_thread_started && have_deadline && 
      (!timeout_until_request_sent || !atomic_load_explicit(&vm->timeout_disarmed, memory_order_acquire))
    ) {
      struct timespec now;
      if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 && ant_kvm_elapsed_ms(&deadline.start, &now) >= timeout_ms) {
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
          ant_kvm_arch_report_timeout(vm);
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
      case KVM_EXIT_MMIO:
        rc = ant_kvm_arch_handle_mmio(vm);
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
      default: {
        bool stop = false;
        rc = ant_kvm_arch_handle_exit(vm, &stop);
        if (rc == -ENOTSUP) {
          fprintf(stderr, "sandbox vm: unhandled KVM exit reason %u\n", vm->run->exit_reason);
          rc = -EIO;
          stop = true;
        }
        if (stop) goto done;
        break;
      }
    }

    if (rc != 0) break;
    if (ant_hvf_vsock_exit_received(vm)) {
      rc = ant_hvf_vsock_exit_code(vm);
      break;
    }
  }

done:
  ant_kvm_deadline_finish(&deadline, deadline_thread, deadline_thread_started);
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

  if (!ant_hvf_vsock_exit_received(vm) && ant_hvf_uart_has_panic(vm)) ant_hvf_uart_report_panic(vm);
  else ant_hvf_uart_discard(vm);
  
  ant_hvf_9p_report(vm);
  ant_hvf_net_stop(vm);
  
  if (vm->run && vm->run != MAP_FAILED) munmap(vm->run, vm->run_mmap_size);
  if (vm->vcpu_fd >= 0) close(vm->vcpu_fd);
  if (vm->gic_fd >= 0) close(vm->gic_fd);
  if (vm->vm_fd >= 0) close(vm->vm_fd);
  if (vm->kvm_fd >= 0) close(vm->kvm_fd);
  if (vm->image_fd >= 0) close(vm->image_fd);
  if (vm->host_mem && vm->host_mem != MAP_FAILED) munmap(vm->host_mem, vm->mem_size);
  
  ant_devices_free(vm->devices);
  vm->devices = NULL;
  
  ant_hvf_vsock_clear_frames(vm);
  if (rc_inout) *rc_inout = rc;
}

static int ant_kvm_init_devices(ant_hvf_vm_t *vm, const ant_sandbox_vm_config_t *config) {
  ant_devices_add_block(ant_hvf_devices(vm), ANT_HVF_VIRTIO_BLK_SLOT, ANT_HVF_VIRTIO_BLK_BAR);
  ant_devices_add_net(ant_hvf_devices(vm), ANT_HVF_VIRTIO_NET_SLOT, ANT_HVF_VIRTIO_NET_BAR);
  memcpy(vm->net_mac, (uint8_t[]){ 0x02, 0x41, 0x4e, 0x54, 0x00, 0x01 }, sizeof(vm->net_mac));
  ant_devices_add_vsock(ant_hvf_devices(vm), ANT_HVF_VIRTIO_VSOCK_SLOT, ANT_HVF_VIRTIO_VSOCK_BAR);
  ant_devices_add_rng(ant_hvf_devices(vm), ANT_HVF_VIRTIO_RNG_SLOT, ANT_HVF_VIRTIO_RNG_BAR);
  ant_devices_set_capabilities(ant_hvf_devices(vm), config->capabilities);
  
  for (size_t i = 0; i < vm->p9_count; i++) {
    int rc = ant_devices_add_share(
      ant_hvf_devices(vm),
      (uint8_t)(ANT_HVF_VIRTIO_9P_SLOT_BASE + i),
      (uint32_t)(ANT_HVF_VIRTIO_9P_BAR_BASE + i * ANT_HVF_VIRTIO_BAR_SIZE),
      config->mounts[i].host_path,
      config->mounts[i].tag,
      config->mounts[i].readonly
    ) ? 0 : -EINVAL;
    
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
      (config->forward_count > 0 && !config->forwards) ||
      (config->allow_host_port_count > 0 && !config->allow_host_ports)) {
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
  vm->gic_fd = -1;
  vm->image_fd = -1;
  vm->mem_size = (size_t)requested_memory;
  vm->mem_size = ant_align_page(vm->mem_size);
  vm->image_sectors = (uint64_t)image_size / 512ull;
  vm->net_enabled = config->network_enabled;
  vm->net_forwards = config->forwards;
  vm->net_forward_count = config->forward_count;
  vm->net_allow_host_ports = config->allow_host_ports;
  vm->net_allow_host_port_count = config->allow_host_port_count;
  vm->net_allow_host_all = config->allow_host_all;
  vm->net_allow_lan = config->allow_lan;
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
  
  rc = ant_kvm_arch_setup_vm(session);
  if (rc != 0) goto fail;

  struct kvm_userspace_memory_region region = {
    .slot = 0,
    .flags = 0,
    .guest_phys_addr = ANT_HVF_GUEST_BASE,
    .memory_size = vm->mem_size,
    .userspace_addr = (uint64_t)(uintptr_t)vm->host_mem,
  };
  
  rc = ant_kvm_ioctl(vm->vm_fd, KVM_SET_USER_MEMORY_REGION, &region, "KVM_SET_USER_MEMORY_REGION");
  if (rc != 0) goto fail;
  session->memory_registered = true;

  rc = ant_hvf_load_kernel(vm, config->kernel_path);
  if (rc != 0) goto fail;
  rc = ant_kvm_arch_prepare_boot(vm, config->kernel_path);
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
  rc = ant_kvm_arch_setup_vcpu(session);
  if (rc != 0) goto fail;

  ant_kvm_install_wakeup_signal();
  ant_hvf_verbosef(vm, "loaded sandbox kernel (%lld bytes)", (long long)kernel_size);
  
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
  ant_hvf_vsock_begin_request(vm);
  
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
    ant_hvf_vsock_fail_transport(vm);
    ant_kvm_classify_result(vm, request->result, maybe_sent);
    return maybe_sent;
  }

  int rc = ant_kvm_run_guest(vm, timeout_ms, timeout_until_request_sent);
  if (rc == -ETIMEDOUT) fprintf(stderr, "sandbox vm: guest timed out\n");
  if (!ant_hvf_vsock_exit_received(vm) && ant_hvf_uart_has_panic(vm) && rc == 0) rc = -EFAULT;
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
