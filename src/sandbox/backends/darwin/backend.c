#include "backend.h"
#include "sandbox/vm.h"
#include "sandbox/sandbox.h"

#include <errno.h>
#include <stdio.h>

#if defined(__aarch64__)

int ant_hvf_check(hv_return_t ret, const char *op) {
  if (ret == HV_SUCCESS) return 0;

  if (ret == HV_DENIED) {
    fprintf(
      stderr,
      "sandbox vm: %s denied; sign the binary with com.apple.security.hypervisor\n", op
    );
    return -EACCES;
  }

  fprintf(stderr, "sandbox vm: %s failed with Hypervisor.framework error %d\n", op, ret);
  return -EIO;
}

int ant_hvf_send_msi(ant_hvf_vm_t *vm, uint64_t addr, uint32_t data) {
  if (!ant_hvf_gic.send_msi) return -ENOSYS;
  hv_return_t rc = ant_hvf_gic.send_msi((hv_ipa_t)addr, data);
  if (rc != HV_SUCCESS) return -EIO;
  return 0;
}

void ant_hvf_wake_vcpu(ant_hvf_vm_t *vm) {
  if (vm && vm->vcpu) hv_vcpus_exit(&vm->vcpu, 1);
}

static int ant_hvf_create_vcpu(ant_hvf_vm_t *vm) {
  hv_vcpu_config_t config = hv_vcpu_config_create();
  if (!config) return -EIO;

  uint64_t dczid = 0;
  hv_return_t dczid_rc =
    hv_vcpu_config_get_feature_reg(config, HV_FEATURE_REG_DCZID_EL0, &dczid);
  if (dczid_rc == HV_SUCCESS) ant_hvf_verbosef(vm, "vCPU feature DCZID_EL0=0x%llx", (u64)dczid);
  else ant_hvf_verbosef(vm, "vCPU feature DCZID_EL0 unavailable rc=%d", dczid_rc);

  hv_return_t create_rc = hv_vcpu_create(&vm->vcpu, &vm->vcpu_exit, config);
  os_release(config);
  return ant_hvf_check(create_rc, "hv_vcpu_create");
}

typedef hv_return_t (
  *ant_hvf_vm_config_set_ipa_granule_fn
)(hv_vm_config_t, uint32_t);

static int ant_hvf_create_vm(ant_hvf_vm_t *vm) {
  hv_vm_config_t config = hv_vm_config_create();
  if (!config) return ant_hvf_check(hv_vm_create(NULL), "hv_vm_create");

  ant_hvf_vm_config_set_ipa_granule_fn set_ipa_granule = 
    (ant_hvf_vm_config_set_ipa_granule_fn)
    dlsym(RTLD_DEFAULT, "hv_vm_config_set_ipa_granule");
  
  if (set_ipa_granule) {
    hv_return_t granule_rc = set_ipa_granule(config, 0);
    if (granule_rc == HV_SUCCESS) ant_hvf_verbose(vm, "configured VM IPA granule=4KB");
    else ant_hvf_verbosef(vm, "VM IPA granule=4KB unavailable rc=%d", granule_rc);
  }

  hv_return_t create_rc = hv_vm_create(config);
  os_release(config);
  return ant_hvf_check(create_rc, "hv_vm_create");
}

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

static uint64_t ant_hvf_process_cpu_time_ns(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0) return 0;
  return (uint64_t)value.tv_sec * 1000000000ull + (uint64_t)value.tv_nsec;
}

static uint64_t ant_hvf_elapsed_ns(const struct timespec *start, const struct timespec *end) {
  uint64_t seconds = (uint64_t)(end->tv_sec - start->tv_sec);
  int64_t nanos = end->tv_nsec - start->tv_nsec;
  if (nanos < 0) { seconds--; nanos += 1000000000ll; }
  return seconds * 1000000000ull + (uint64_t)nanos;
}

void *ant_hvf_timeout_thread(void *arg) {
  ant_hvf_timeout_t *timeout = arg;
  const struct timespec tick = { .tv_sec = 0, .tv_nsec = 1000000 };
  while (!atomic_load_explicit(&timeout->stop, memory_order_acquire)) {
    if (timeout->vm->cpu_time_ms > 0 &&
        ant_hvf_process_cpu_time_ns() >= (uint64_t)timeout->vm->cpu_time_ms * 1000000ull) {
      atomic_store_explicit(&timeout->vm->cpu_timed_out, true, memory_order_release);
      hv_vcpus_exit(&timeout->vm->vcpu, 1);
      return NULL;
    }
    bool wall_disarmed = timeout->until_request_sent &&
      atomic_load_explicit(&timeout->vm->vsock.request_sent, memory_order_acquire);
    if (timeout->timeout_ms > 0 && !wall_disarmed) {
      struct timespec now;
      if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
          ant_hvf_elapsed_ns(&timeout->started_at, &now) >=
            (uint64_t)timeout->timeout_ms * 1000000ull) {
        atomic_store_explicit(&timeout->vm->timed_out, true, memory_order_release);
        hv_vcpus_exit(&timeout->vm->vcpu, 1);
        return NULL;
      }
    }
    nanosleep(&tick, NULL);
  }
  return NULL;
}
int ant_hvf_run(ant_hvf_vm_t *vm, unsigned int timeout_ms, bool timeout_until_request_sent) {
  pthread_t timeout_thread;
  ant_hvf_timeout_t timeout;
  
  bool timeout_thread_started = false;
  int rc = 0;
  
  if (timeout_ms > 0 || vm->cpu_time_ms > 0) {
    timeout = (ant_hvf_timeout_t){
      .vm = vm,
      .timeout_ms = timeout_ms,
      .until_request_sent = timeout_until_request_sent,
    };
    clock_gettime(CLOCK_MONOTONIC, &timeout.started_at);
    int prc = pthread_create(&timeout_thread, NULL, ant_hvf_timeout_thread, &timeout);
    if (prc == 0) timeout_thread_started = true;
  }

  for (;;) {
    rc = ant_hvf_vsock_maybe_send_request(vm);
    if (rc != 0 && rc != -EAGAIN) goto done;
    rc = ant_hvf_virtio_net_drain_rx_if_wake(vm);
    if (rc != 0) goto done;

    atomic_store_explicit(&vm->vcpu_running, true, memory_order_release);
    if (atomic_exchange_explicit(&vm->vsock_wake_pending, false, memory_order_acq_rel)) {
      atomic_store_explicit(&vm->vcpu_running, false, memory_order_release);
      continue;
    }
    rc = ant_hvf_check(hv_vcpu_run(vm->vcpu), "hv_vcpu_run");
    atomic_store_explicit(&vm->vcpu_running, false, memory_order_release);
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
      if (ec == ESR_EC_WFX_TRAP) rc = ant_hvf_handle_wfx(vm);
      else rc = ant_hvf_handle_mmio(vm, &vm->vcpu_exit->exception);
      if (rc == ANT_HVF_GUEST_SHUTDOWN) { rc = 0; goto done; }
      if (rc != 0) {
        uint64_t pc = 0; uint64_t elr = 0; uint64_t esr_el1 = 0; uint64_t far = 0; uint64_t vbar = 0;
        hv_vcpu_get_reg(vm->vcpu, HV_REG_PC, &pc);
        hv_vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_ELR_EL1, &elr);
        hv_vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_ESR_EL1, &esr_el1);
        hv_vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_FAR_EL1, &far);
        hv_vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_VBAR_EL1, &vbar);
        fprintf(stderr, "sandbox vm: unhandled guest exception at pc=0x%llx esr=0x%llx ipa=0x%llx va=0x%llx elr=0x%llx guest_esr=0x%llx far=0x%llx vbar=0x%llx\n",
          (u64)pc,
          (u64)vm->vcpu_exit->exception.syndrome,
          (u64)vm->vcpu_exit->exception.physical_address,
          (u64)vm->vcpu_exit->exception.virtual_address,
          (u64)elr, (u64)esr_el1, (u64)far, (u64)vbar
        );
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
      if (atomic_load_explicit(&vm->cpu_timed_out, memory_order_acquire)) {
        rc = ANT_SANDBOX_CPU_TIME_LIMIT_CODE;
        goto done;
      } else if (atomic_load_explicit(&vm->canceled, memory_order_acquire)) {
        rc = -ECANCELED;
        goto done;
      } else if (atomic_load_explicit(&vm->timed_out, memory_order_acquire)) {
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
        fprintf(stderr, "sandbox vm: guest timed out at pc=0x%llx low_pc=0x%llx kas_offset=0x%llx last_exit=%u last_pc=0x%llx last_esr=0x%llx last_ipa=0x%llx last_va=0x%llx cntv_ctl=0x%llx cntv_cval=0x%llx cntvct=0x%llx cntfrq=%llu\n",
          (u64)pc,
          (u64)(kas_offset ? pc - kas_offset : 0), (u64)kas_offset,
          vm->last_exit_reason, (u64)vm->last_exit_pc, (u64)vm->last_exit_esr, (u64)vm->last_exit_ipa,
          (u64)vm->last_exit_va, (u64)cntv_ctl, (u64)cntv_cval, (u64)cntvct, (u64)cntfrq
        );
        rc = -ETIMEDOUT;
        goto done;
      } else {
        rc = ant_hvf_virtio_net_drain_rx_if_wake(vm);
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
    atomic_store_explicit(&timeout.stop, true, memory_order_release);
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

static void ant_hvf_set_result(
  ant_sandbox_vm_result_t *result,
  ant_sandbox_vm_result_kind_t kind,
  int code
) {
  if (!result) return;
  result->kind = kind;
  result->code = code;
}

static void ant_hvf_classify_result(ant_hvf_vm_t *vm, ant_sandbox_vm_result_t *result, int rc) {
  if (!result) return;
  
  if (vm && vm->vsock.exit_received)
    ant_hvf_set_result(result, ANT_SANDBOX_VM_RESULT_GUEST_EXIT, vm->vsock.exit_code);
  else if (vm && ant_hvf_uart_has_panic(vm))
    ant_hvf_set_result(result, ANT_SANDBOX_VM_RESULT_KERNEL_PANIC, rc ? rc : -EFAULT);
  else if (rc == -ENOSYS)
    ant_hvf_set_result(result, ANT_SANDBOX_VM_RESULT_BACKEND_UNAVAILABLE, rc);
  else if (rc == ANT_SANDBOX_CPU_TIME_LIMIT_CODE ||
           (vm && atomic_load_explicit(&vm->cpu_timed_out, memory_order_acquire)))
    ant_hvf_set_result(result, ANT_SANDBOX_VM_RESULT_CPU_TIME_LIMIT,
                       rc ? rc : ANT_SANDBOX_CPU_TIME_LIMIT_CODE);
  else if (rc == -ETIMEDOUT)
    ant_hvf_set_result(result, ANT_SANDBOX_VM_RESULT_TIMEOUT, rc);
  else if (vm && vm->vsock.protocol_error)
    ant_hvf_set_result(result, ANT_SANDBOX_VM_RESULT_PROTOCOL_ERROR, rc);
  else if (vm && vm->vsock.transport_error)
    ant_hvf_set_result(result, ANT_SANDBOX_VM_RESULT_TRANSPORT_ERROR, rc);
  else if (rc == -EINVAL)
    ant_hvf_set_result(result, ANT_SANDBOX_VM_RESULT_CONFIG_ERROR, rc);
  else if (rc != 0)
    ant_hvf_set_result(result, ANT_SANDBOX_VM_RESULT_VM_ERROR, rc);
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
  
  vm->mem_size = config->memory_size ? (size_t)config->memory_size : ANT_SANDBOX_DEFAULT_MEMORY_SIZE;
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
    rc = ant_hvf_9p_buffers_init(&vm->p9[i], ANT_HVF_9P_MAX_MSIZE);
    if (rc != 0) goto fail;
  }
  
  vm->cntfrq = ant_hvf_host_cntfrq();
  if (vm->cntfrq == 0 || vm->cntfrq > UINT32_MAX) {
    fprintf(stderr, "sandbox vm: unsupported host generic timer frequency %llu\n", (u64)vm->cntfrq);
    rc = -EIO;
    goto fail;
  }
  
  vm->verbose = config->verbose;
  vm->timeout_ms = config->timeout_ms;
  vm->boot_timeout_ms = config->boot_timeout_ms;
  vm->cpu_time_ms = config->cpu_time_ms;
  vm->frame_handler = config->frame_handler;
  vm->frame_handler_user = config->frame_handler_user;
  ant_hvf_verbosef(vm,
    "Hypervisor.framework backend image=%s kernel=%s memory=%zu MiB mounts=%zu forwards=%zu",
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

  if (vm->mem_size < ANT_SANDBOX_MIN_MEMORY_SIZE) {
    rc = -EINVAL;
    goto fail;
  }

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
  
  ant_hvf_verbosef(vm, "opened disk image (%lld bytes)", (long long)image_size);
  vm->host_mem = mmap(NULL, vm->mem_size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  
  if (vm->host_mem == MAP_FAILED) {
    rc = -errno;
    goto fail;
  }
  
  ant_hvf_verbosef(vm, "allocated guest RAM at %p", vm->host_mem);
  rc = ant_hvf_create_vm(vm);
  
  if (rc != 0) goto fail;
  session->vm_created = true;
  ant_hvf_verbose(vm, "created VM");

  rc = ant_hvf_create_gic(vm);
  if (rc != 0) goto fail;
  ant_hvf_verbose(vm, "created GIC");

  rc = ant_hvf_check(
    hv_vm_map(
      vm->host_mem, ANT_HVF_GUEST_BASE, vm->mem_size,
      HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC
    ),
    "hv_vm_map"
  );
  
  if (rc != 0) goto fail;
  session->mem_mapped = true;
  
  ant_hvf_verbosef(vm,
    "mapped guest RAM base=0x%llx size=%zu MiB",
    (u64)ANT_HVF_GUEST_BASE,
    vm->mem_size / ((size_t)1024 * 1024)
  );

  rc = ant_hvf_load_kernel(vm, config->kernel_path);
  if (rc != 0) goto fail;
  ant_hvf_verbosef(vm, "loaded sandbox kernel (%lld bytes)", (long long)kernel_size);

  rc = ant_hvf_build_dtb(vm);
  if (rc != 0) goto fail;
  ant_hvf_verbose(vm, "built boot device tree");

  rc = ant_hvf_create_vcpu(vm);
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

  atomic_store_explicit(&vm->vsock.request_sent, false, memory_order_release);
  vm->vsock.exit_received = false;
  vm->vsock.exit_code = 0;
  vm->vsock.protocol_error = false;
  vm->vsock.transport_error = false;
  atomic_store_explicit(&vm->timed_out, false, memory_order_release);
  atomic_store_explicit(&vm->cpu_timed_out, false, memory_order_release);
  atomic_store_explicit(&vm->canceled, false, memory_order_release);
  
  vm->frame_handler = request->frame_handler;
  vm->frame_handler_user = request->frame_handler_user;

  int queue_rc = ant_hvf_vsock_queue_frame(vm, request->request_data, request->request_len);
  if (queue_rc != 0) return queue_rc;

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
  else if (timeout_ms > 0) ant_hvf_verbosef(vm, "running guest timeout=%u ms", timeout_ms);
  else ant_hvf_verbose(vm, "running guest timeout=disabled");

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

  vm->frame_handler = NULL;
  vm->frame_handler_user = NULL;
  
  return rc;
}

static int ant_hvf_session_send(void *opaque, const void *data, size_t len) {
  if (!opaque) return -EINVAL;
  ant_hvf_session_t *session = opaque;
  return ant_hvf_vsock_queue_frame(&session->vm, data, len);
}

static int ant_hvf_session_stats(void *opaque, ant_sandbox_vm_stats_t *stats) {
  if (!opaque || !stats) return -EINVAL;
  stats->cpu_time_ns = ant_hvf_process_cpu_time_ns();
  return 0;
}

static int ant_hvf_session_cancel(void *opaque) {
  if (!opaque) return -EINVAL;
  ant_hvf_session_t *session = opaque;
  atomic_store_explicit(&session->vm.canceled, true, memory_order_release);
  ant_hvf_wake_vcpu(&session->vm);
  return 0;
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
  fprintf(stderr, "sandbox vm: Hypervisor.framework backend requires Apple Silicon\n");
  return -ENOSYS;
}

static int ant_hvf_session_create(const ant_sandbox_vm_config_t *config, void **session_out) { return -ENOSYS; }
static int ant_hvf_session_execute(void *session, const ant_sandbox_vm_request_t *request) { return -ENOSYS; }
static int ant_hvf_session_send(void *session, const void *data, size_t len) { return -ENOSYS; }
static void ant_hvf_session_destroy(void *session) { (void)session; }

#endif

const ant_sandbox_vm_backend_t ant_sandbox_vm_darwin_backend = {
  .name = "hypervisor.framework",
  .start = ant_hvf_start,
  .create_session = ant_hvf_session_create,
  .execute_session = ant_hvf_session_execute,
  .send_session = ant_hvf_session_send,
  .get_stats_session = ant_hvf_session_stats,
  .cancel_session = ant_hvf_session_cancel,
  .destroy_session = ant_hvf_session_destroy,
};
