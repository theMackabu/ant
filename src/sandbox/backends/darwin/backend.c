#include "backend.h"
#include "sandbox/vm.h"

#if defined(__aarch64__)

static void ant_hvf_verbose_prefix(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    fputs("[0.000000] ant: ", stderr);
    return;
  }
  fprintf(stderr, "[%lld.%06ld] ant: ", (long long)ts.tv_sec, ts.tv_nsec / 1000);
}

void ant_hvf_verbose(ant_hvf_vm_t *vm, const char *message) {
  if (!vm || !vm->verbose) return;
  ant_hvf_verbose_prefix();
  fputs(message, stderr);
  fputc('\n', stderr);
}

void ant_hvf_verbosef(ant_hvf_vm_t *vm, const char *fmt, ...) {
  if (!vm || !vm->verbose) return;
  ant_hvf_verbose_prefix();
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

void *ant_hvf_timeout_thread(void *arg) {
  ant_hvf_timeout_t *timeout = arg;
  usleep(timeout->timeout_ms * 1000u);
  if (timeout->until_request_sent && timeout->vm->vsock.request_sent) return NULL;
  timeout->vm->timed_out = true;
  hv_vcpus_exit(&timeout->vm->vcpu, 1);
  return NULL;
}
int ant_hvf_run(ant_hvf_vm_t *vm, unsigned int timeout_ms, bool timeout_until_request_sent) {
  pthread_t timeout_thread;
  ant_hvf_timeout_t timeout;
  bool timeout_thread_started = false;
  int rc = 0;
  if (timeout_ms > 0) {
    timeout.vm = vm;
    timeout.timeout_ms = timeout_ms;
    timeout.until_request_sent = timeout_until_request_sent;
    int prc = pthread_create(&timeout_thread, NULL, ant_hvf_timeout_thread, &timeout);
    if (prc == 0) {
      timeout_thread_started = true;
    }
  }

  for (;;) {
    rc = ant_hvf_check(hv_vcpu_run(vm->vcpu), "hv_vcpu_run");
    if (rc != 0) goto done;

    vm->last_exit_reason = vm->vcpu_exit->reason;
    hv_vcpu_get_reg(vm->vcpu, HV_REG_PC, &vm->last_exit_pc);
    if (vm->vcpu_exit->reason == HV_EXIT_REASON_EXCEPTION) {
      vm->last_exit_esr = vm->vcpu_exit->exception.syndrome;
      vm->last_exit_ipa = vm->vcpu_exit->exception.physical_address;
      vm->last_exit_va = vm->vcpu_exit->exception.virtual_address;
    }

    if (vm->vcpu_exit->reason == HV_EXIT_REASON_EXCEPTION) {
      uint32_t ec = (uint32_t)(vm->vcpu_exit->exception.syndrome >> ESR_EC_SHIFT);
      if (ec == ESR_EC_WFX_TRAP) {
        rc = ant_hvf_handle_wfx(vm);
      } else {
        rc = ant_hvf_handle_mmio(vm, &vm->vcpu_exit->exception);
      }
      if (rc == ANT_HVF_GUEST_SHUTDOWN) {
        rc = 0;
        goto done;
      }
      if (rc != 0) {
        uint64_t pc = 0;
        hv_vcpu_get_reg(vm->vcpu, HV_REG_PC, &pc);
        fprintf(stderr,
                "sandbox vm: unhandled guest exception at pc=0x%llx esr=0x%llx ipa=0x%llx va=0x%llx\n",
                (unsigned long long)pc,
                (unsigned long long)vm->vcpu_exit->exception.syndrome,
                (unsigned long long)vm->vcpu_exit->exception.physical_address,
                (unsigned long long)vm->vcpu_exit->exception.virtual_address);
        goto done;
      }
      if (vm->vsock.exit_received) {
        rc = vm->vsock.exit_code;
        goto done;
      }
    } else if (vm->vcpu_exit->reason == HV_EXIT_REASON_VTIMER_ACTIVATED) {
      rc = ant_hvf_raise_vtimer(vm, "vtimer activated");
      if (rc != 0) goto done;
    } else if (vm->vcpu_exit->reason == HV_EXIT_REASON_CANCELED) {
      if (vm->timed_out) {
        uint64_t pc = 0;
        uint64_t cntv_ctl = 0;
        uint64_t cntv_cval = 0;
        uint64_t cntvct = 0;
        uint64_t cntfrq = 0;
        uint64_t kas_offset = 0;
        hv_vcpu_get_reg(vm->vcpu, HV_REG_PC, &pc);
        hv_vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_CNTV_CTL_EL0, &cntv_ctl);
        hv_vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_CNTV_CVAL_EL0, &cntv_cval);
        hv_vcpu_get_sys_reg(vm->vcpu, ANT_HVF_SYS_REG_CNTVCT_EL0, &cntvct);
        hv_vcpu_get_sys_reg(vm->vcpu, ANT_HVF_SYS_REG_CNTFRQ_EL0, &cntfrq);
        ant_hvf_guest_read(vm, ANT_HVF_NANOS_KAS_OFFSET_SYMBOL, &kas_offset, sizeof(kas_offset));
        fprintf(stderr,
                "sandbox vm: guest timed out at pc=0x%llx low_pc=0x%llx kas_offset=0x%llx last_exit=%u last_pc=0x%llx last_esr=0x%llx last_ipa=0x%llx last_va=0x%llx cntv_ctl=0x%llx cntv_cval=0x%llx cntvct=0x%llx cntfrq=%llu\n",
                (unsigned long long)pc,
                (unsigned long long)(kas_offset ? pc - kas_offset : 0),
                (unsigned long long)kas_offset,
                vm->last_exit_reason,
                (unsigned long long)vm->last_exit_pc,
                (unsigned long long)vm->last_exit_esr,
                (unsigned long long)vm->last_exit_ipa,
                (unsigned long long)vm->last_exit_va,
                (unsigned long long)cntv_ctl,
                (unsigned long long)cntv_cval,
                (unsigned long long)cntvct,
                (unsigned long long)cntfrq);
        rc = -ETIMEDOUT;
        goto done;
      } else {
        rc = ant_hvf_virtio_net_drain_rx(vm);
        if (rc != 0) goto done;
        rc = ant_hvf_sync_vtimer(vm);
        if (rc != 0) goto done;
        continue;
      }
    } else {
      fprintf(stderr, "sandbox vm: unknown vCPU exit reason %u\n", vm->vcpu_exit->reason);
      rc = -EIO;
      goto done;
    }
  }

done:
  if (timeout_thread_started) {
    if (!vm->timed_out) pthread_cancel(timeout_thread);
    pthread_join(timeout_thread, NULL);
  }
  return rc;
}

typedef struct {
  ant_hvf_vm_t vm;
  bool vm_created;
  bool mem_mapped;
  bool vcpu_created;
} ant_hvf_session_t;

static void ant_hvf_set_result(ant_sandbox_vm_result_t *result,
                               ant_sandbox_vm_result_kind_t kind,
                               int code) {
  if (!result) return;
  result->kind = kind;
  result->code = code;
}

static void ant_hvf_classify_result(ant_hvf_vm_t *vm, ant_sandbox_vm_result_t *result, int rc) {
  if (!result) return;
  if (vm && vm->vsock.exit_received) {
    ant_hvf_set_result(result, ANT_SANDBOX_VM_RESULT_GUEST_EXIT, vm->vsock.exit_code);
  } else if (vm && ant_hvf_uart_has_panic(vm)) {
    ant_hvf_set_result(result, ANT_SANDBOX_VM_RESULT_KERNEL_PANIC, rc ? rc : -EFAULT);
  } else if (rc == -ENOSYS) {
    ant_hvf_set_result(result, ANT_SANDBOX_VM_RESULT_BACKEND_UNAVAILABLE, rc);
  } else if (rc == -ETIMEDOUT) {
    ant_hvf_set_result(result, ANT_SANDBOX_VM_RESULT_TIMEOUT, rc);
  } else if (vm && vm->vsock.protocol_error) {
    ant_hvf_set_result(result, ANT_SANDBOX_VM_RESULT_PROTOCOL_ERROR, rc);
  } else if (vm && vm->vsock.transport_error) {
    ant_hvf_set_result(result, ANT_SANDBOX_VM_RESULT_TRANSPORT_ERROR, rc);
  } else if (rc == -EINVAL) {
    ant_hvf_set_result(result, ANT_SANDBOX_VM_RESULT_CONFIG_ERROR, rc);
  } else if (rc != 0) {
    ant_hvf_set_result(result, ANT_SANDBOX_VM_RESULT_VM_ERROR, rc);
  }
}

static void ant_hvf_session_cleanup(ant_hvf_session_t *session, int *rc_inout) {
  if (!session) return;
  ant_hvf_vm_t *vm = &session->vm;
  int rc = rc_inout ? *rc_inout : 0;

  if (!vm->vsock.exit_received && ant_hvf_uart_has_panic(vm)) ant_hvf_uart_report_panic(vm);
  else ant_hvf_uart_discard(vm);
  for (size_t i = 0; i < vm->p9_count; i++) ant_hvf_9p_report_stats(vm, &vm->p9[i], i);
  ant_hvf_net_stop(vm);
  if (session->vcpu_created) {
    int destroy_rc = ant_hvf_check(hv_vcpu_destroy(vm->vcpu), "hv_vcpu_destroy");
    if (rc == 0) rc = destroy_rc;
  }
  if (session->mem_mapped) {
    int unmap_rc = ant_hvf_check(hv_vm_unmap(ANT_HVF_GUEST_BASE, vm->mem_size), "hv_vm_unmap");
    if (rc == 0) rc = unmap_rc;
  }
  if (session->vm_created) {
    int destroy_rc = ant_hvf_check(hv_vm_destroy(), "hv_vm_destroy");
    if (rc == 0) rc = destroy_rc;
  }
  if (vm->image_fd >= 0) close(vm->image_fd);
  if (vm->host_mem && vm->host_mem != MAP_FAILED) munmap(vm->host_mem, vm->mem_size);
  free(vm->vsock.rx_stream);
  for (size_t i = 0; i < vm->p9_count; i++) {
    free(vm->p9[i].fids);
    ant_hvf_9p_stat_cache_clear(&vm->p9[i]);
    ant_hvf_9p_file_cache_clear(&vm->p9[i]);
  }
  if (vm->net_lock_init) pthread_mutex_destroy(&vm->net_lock);
  if (rc_inout) *rc_inout = rc;
}

static int ant_hvf_session_create(const ant_sandbox_vm_config_t *config, void **session_out) {
  if (!config || !session_out) return -EINVAL;
  *session_out = NULL;

  off_t image_size = 0;
  off_t kernel_size = 0;
  int rc = ant_hvf_check_file("image", config->image_path, &image_size);
  if (rc != 0) return rc;
  rc = ant_hvf_check_file("kernel", config->kernel_path, &kernel_size);
  if (rc != 0) return rc;

  ant_hvf_session_t *session = calloc(1, sizeof(*session));
  if (!session) return -ENOMEM;
  ant_hvf_vm_t *vm = &session->vm;
  vm->mem_size = config->memory_size ? (size_t)config->memory_size : (1024ull * 1024ull * 1024ull);
  vm->mem_size = ant_align_page(vm->mem_size);
  vm->image_fd = -1;
  vm->image_sectors = (uint64_t)image_size / 512ull;
  vm->net_enabled = config->network_enabled;
  vm->net_forwards = config->forwards;
  vm->net_forward_count = config->forward_count;
  if (config->mount_count == 0 || config->mount_count > ANT_HVF_VIRTIO_9P_MAX) {
    free(session);
    ant_hvf_set_result(config->result, ANT_SANDBOX_VM_RESULT_CONFIG_ERROR, -EINVAL);
    return -EINVAL;
  }
  vm->p9_count = config->mount_count;
  ant_hvf_virtio_init(&vm->blk,
                      ANT_HVF_VIRTIO_KIND_BLOCK,
                      "virtio-blk",
                      ANT_VIRTIO_PCI_SUBDEVICE_BLOCK,
                      ANT_VIRTIO_PCI_SUBDEVICE_BLOCK,
                      ANT_HVF_VIRTIO_BLK_SLOT,
                      0x01,
                      0x00,
                      (uint32_t)ANT_HVF_VIRTIO_BLK_BAR,
                      0,
                      1,
                      ANT_VIRTIO_BLK_QUEUE_SIZE,
                      24);
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
  }
  vm->cntfrq = ant_hvf_host_cntfrq();
  vm->verbose = config->verbose;
  vm->timeout_ms = config->timeout_ms;
  vm->boot_timeout_ms = config->boot_timeout_ms;
  vm->frame_handler = config->frame_handler;
  vm->frame_handler_user = config->frame_handler_user;
  ant_hvf_verbosef(vm,
                   "Hypervisor.framework backend image=%s kernel=%s memory=%zu MiB mounts=%zu forwards=%zu",
                   config->image_path,
                   config->kernel_path,
                   vm->mem_size / ((size_t)1024 * 1024),
                   vm->p9_count,
                   vm->net_forward_count);
  for (size_t i = 0; i < config->mount_count; i++) {
    ant_hvf_verbosef(vm,
                     "mount[%zu] host=%s guest=%s tag=%s %s",
                     i,
                     config->mounts[i].host_path,
                     config->mounts[i].guest_path,
                     config->mounts[i].tag,
                     config->mounts[i].readonly ? "ro" : "rw");
  }
  for (size_t i = 0; i < config->forward_count; i++) {
    ant_hvf_verbosef(vm,
                     "forward[%zu] host=%u guest=%u",
                     i,
                     config->forwards[i].host_port,
                     config->forwards[i].guest_port);
  }

  if (vm->mem_size < 64ull * 1024ull * 1024ull) {
    rc = -EINVAL;
    goto fail;
  }

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
  ant_hvf_verbosef(vm, "opened disk image (%lld bytes)", (long long)image_size);

  vm->host_mem = mmap(NULL, vm->mem_size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  if (vm->host_mem == MAP_FAILED) {
    rc = -errno;
    goto fail;
  }
  ant_hvf_verbosef(vm, "allocated guest RAM at %p", vm->host_mem);

  rc = ant_hvf_check(hv_vm_create(NULL), "hv_vm_create");
  if (rc != 0) goto fail;
  session->vm_created = true;
  ant_hvf_verbose(vm, "created VM");

  rc = ant_hvf_create_gic(vm);
  if (rc != 0) goto fail;
  ant_hvf_verbose(vm, "created GIC");

  rc = ant_hvf_check(
    hv_vm_map(vm->host_mem, ANT_HVF_GUEST_BASE, vm->mem_size,
              HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC),
    "hv_vm_map");
  if (rc != 0) goto fail;
  session->mem_mapped = true;
  ant_hvf_verbosef(vm,
                   "mapped guest RAM base=0x%llx size=%zu MiB",
                   (unsigned long long)ANT_HVF_GUEST_BASE,
                   vm->mem_size / ((size_t)1024 * 1024));

  rc = ant_hvf_load_kernel(vm, config->kernel_path);
  if (rc != 0) goto fail;
  ant_hvf_verbosef(vm, "loaded Nanos kernel (%lld bytes)", (long long)kernel_size);

  rc = ant_hvf_build_dtb(vm);
  if (rc != 0) goto fail;
  ant_hvf_verbose(vm, "built boot device tree");

  rc = ant_hvf_check(hv_vcpu_create(&vm->vcpu, &vm->vcpu_exit, NULL), "hv_vcpu_create");
  if (rc != 0) goto fail;
  session->vcpu_created = true;
  ant_hvf_verbose(vm, "created vCPU");

  rc = ant_hvf_init_vcpu(vm);
  if (rc != 0) goto fail;

  *session_out = session;
  return 0;

fail:
  ant_hvf_classify_result(vm, config->result, rc);
  ant_hvf_session_cleanup(session, &rc);
  free(session);
  return rc;
}

static int ant_hvf_session_execute(void *opaque, const ant_sandbox_vm_request_t *request) {
  if (!opaque || !request || !request->request_data || request->request_len == 0) return -EINVAL;
  ant_hvf_session_t *session = opaque;
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
  vm->frame_handler = request->frame_handler;
  vm->frame_handler_user = request->frame_handler_user;

  unsigned int timeout_ms = vm->timeout_ms ? vm->timeout_ms : vm->boot_timeout_ms;
  bool timeout_until_request_sent = vm->timeout_ms == 0 && vm->boot_timeout_ms > 0;
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
    ant_hvf_classify_result(vm, request->result, maybe_sent);
    return maybe_sent;
  }

  int rc = ant_hvf_run(vm, timeout_ms, timeout_until_request_sent);
  if (rc == -ETIMEDOUT) fprintf(stderr, "sandbox vm: guest timed out\n");
  if (!vm->vsock.exit_received && ant_hvf_uart_has_panic(vm) && rc == 0) rc = -EFAULT;
  ant_hvf_classify_result(vm, request->result, rc);

  vm->vsock.request_data = NULL;
  vm->vsock.request_len = 0;
  vm->frame_handler = NULL;
  vm->frame_handler_user = NULL;
  return rc;
}

static void ant_hvf_session_destroy(void *opaque) {
  if (!opaque) return;
  ant_hvf_session_t *session = opaque;
  int rc = 0;
  ant_hvf_session_cleanup(session, &rc);
  free(session);
}

int ant_hvf_start(const ant_sandbox_vm_config_t *config) {
  void *session = NULL;
  int rc = ant_hvf_session_create(config, &session);
  if (rc == 0) {
    ant_sandbox_vm_request_t request = {
      .request_data = config->request_data,
      .request_len = config->request_len,
      .frame_handler = config->frame_handler,
      .frame_handler_user = config->frame_handler_user,
    };
    rc = ant_hvf_session_execute(session, &request);
  }
  ant_hvf_session_destroy(session);
  return rc;
}

#else

int ant_hvf_start(const ant_sandbox_vm_config_t *config) {
  (void)config;
  fprintf(stderr, "sandbox vm: Hypervisor.framework backend requires Apple Silicon\n");
  return -ENOSYS;
}

static int ant_hvf_session_create(const ant_sandbox_vm_config_t *config, void **session_out) {
  (void)config;
  (void)session_out;
  return -ENOSYS;
}

static int ant_hvf_session_execute(void *session, const ant_sandbox_vm_request_t *request) {
  (void)session;
  (void)request;
  return -ENOSYS;
}

static void ant_hvf_session_destroy(void *session) {
  (void)session;
}


#endif

const ant_sandbox_vm_backend_t ant_sandbox_vm_darwin_backend = {
  .name = "hypervisor.framework",
  .start = ant_hvf_start,
  .create_session = ant_hvf_session_create,
  .execute_session = ant_hvf_session_execute,
  .destroy_session = ant_hvf_session_destroy,
};
