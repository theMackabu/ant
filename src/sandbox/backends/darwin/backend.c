#include "backend.h"
#include "sandbox/vm.h"
#include "sandbox/sandbox.h"

#include <errno.h>
#include <stdio.h>

#if defined(__aarch64__)

static constexpr uint64_t ANT_HVF_TIMEOUT_MIN_WAIT_NS = 1000000ull;

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

void ant_hvf_wake_vcpu(ant_hvf_vm_t *vm) {
  if (!vm) return;
  if (vm->vcpu_exit) ant_hvf_api.vcpus_exit(&vm->vcpu, 1);
  if (!vm->idle_sync_init) return;
  pthread_mutex_lock(&vm->idle_lock);
  vm->idle_kicked = true;
  pthread_cond_signal(&vm->idle_cond);
  pthread_mutex_unlock(&vm->idle_lock);
}

static int ant_hvf_create_vcpu(ant_hvf_vm_t *vm) {
  hv_vcpu_config_t config = ant_hvf_api.vcpu_config_create();
  if (!config) return -EIO;

  uint64_t dczid = 0;
  hv_return_t dczid_rc =
    ant_hvf_api.vcpu_config_get_feature_reg(config, HV_FEATURE_REG_DCZID_EL0, &dczid);
  if (dczid_rc == HV_SUCCESS) ant_hvf_verbosef(vm, "vCPU feature DCZID_EL0=0x%llx", (u64)dczid);
  else ant_hvf_verbosef(vm, "vCPU feature DCZID_EL0 unavailable rc=%d", dczid_rc);

  hv_return_t create_rc = ant_hvf_api.vcpu_create(&vm->vcpu, &vm->vcpu_exit, config);
  os_release(config);
  return ant_hvf_check(create_rc, "hv_vcpu_create");
}

static int ant_hvf_create_vm(ant_hvf_vm_t *vm) {
  hv_vm_config_t config = ant_hvf_api.vm_config_create();
  if (!config) return ant_hvf_check(ant_hvf_api.vm_create(NULL), "hv_vm_create");

  if (ant_hvf_api.vm_config_set_ipa_granule) {
    hv_return_t granule_rc = ant_hvf_api.vm_config_set_ipa_granule(config, 0);
    if (granule_rc == HV_SUCCESS) ant_hvf_verbose(vm, "configured VM IPA granule=4KB");
    else ant_hvf_verbosef(vm, "VM IPA granule=4KB unavailable rc=%d", granule_rc);
  }

  hv_return_t create_rc = ant_hvf_api.vm_create(config);
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
  ant_hvf_vm_t *vm = timeout->vm;
  long online = sysconf(_SC_NPROCESSORS_ONLN);
  uint64_t cpu_rate = online > 0 ? (uint64_t)online : 1;
  atomic_bool *fired = NULL;

  pthread_mutex_lock(&timeout->lock);
  while (!timeout->stop) {
    uint64_t wait_ns = UINT64_MAX;
    if (vm->cpu_time_ms > 0) {
      uint64_t limit = (uint64_t)vm->cpu_time_ms * 1000000ull;
      uint64_t used = ant_hvf_process_cpu_time_ns();
      if (used >= limit) { fired = &vm->cpu_timed_out; break; }
      wait_ns = (limit - used) / cpu_rate;
    }
    bool wall_disarmed = timeout->until_request_sent &&
      ant_hvf_vsock_request_sent(vm);
    if (timeout->timeout_ms > 0 && !wall_disarmed) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      uint64_t limit = (uint64_t)timeout->timeout_ms * 1000000ull;
      uint64_t elapsed = ant_hvf_elapsed_ns(&timeout->started_at, &now);
      if (elapsed >= limit) { fired = &vm->timed_out; break; }
      if (limit - elapsed < wait_ns) wait_ns = limit - elapsed;
    }
    if (wait_ns == UINT64_MAX) {
      pthread_cond_wait(&timeout->cond, &timeout->lock);
      continue;
    }
    if (wait_ns < ANT_HVF_TIMEOUT_MIN_WAIT_NS) wait_ns = ANT_HVF_TIMEOUT_MIN_WAIT_NS;
    struct timespec ts = {
      .tv_sec = (time_t)(wait_ns / 1000000000ull),
      .tv_nsec = (long)(wait_ns % 1000000000ull),
    };
    pthread_cond_timedwait_relative_np(&timeout->cond, &timeout->lock, &ts);
  }
  pthread_mutex_unlock(&timeout->lock);

  if (fired) {
    atomic_store_explicit(fired, true, memory_order_release);
    ant_hvf_wake_vcpu(vm);
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
    pthread_mutex_init(&timeout.lock, NULL);
    pthread_cond_init(&timeout.cond, NULL);
    int prc = pthread_create(&timeout_thread, NULL, ant_hvf_timeout_thread, &timeout);
    if (prc == 0) timeout_thread_started = true;
    else {
      pthread_cond_destroy(&timeout.cond);
      pthread_mutex_destroy(&timeout.lock);
    }
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
    rc = ant_hvf_check(ant_hvf_api.vcpu_run(vm->vcpu), "hv_vcpu_run");
    atomic_store_explicit(&vm->vcpu_running, false, memory_order_release);
    if (rc != 0) goto done;

    vm->last_exit_reason = vm->vcpu_exit->reason;
    ant_hvf_api.vcpu_get_reg(vm->vcpu, HV_REG_PC, &vm->last_exit_pc);
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
        ant_hvf_report_exception(vm);
        goto done;
      }
      if (ant_hvf_vsock_exit_received(vm)) {
        rc = ant_hvf_vsock_exit_code(vm);
        goto done;
      }
    } else if (vm->vcpu_exit->reason == HV_EXIT_REASON_VTIMER_ACTIVATED) {
      rc = ant_hvf_raise_vtimer(vm);
      if (rc != 0) goto done;
    } else if (vm->vcpu_exit->reason == HV_EXIT_REASON_CANCELED) {
      if (atomic_load_explicit(&vm->cpu_timed_out, memory_order_acquire)) {
        rc = ANT_SANDBOX_CPU_TIME_LIMIT_CODE;
        goto done;
      } else if (atomic_load_explicit(&vm->canceled, memory_order_acquire)) {
        rc = -ECANCELED;
        goto done;
      } else if (atomic_load_explicit(&vm->timed_out, memory_order_acquire)) {
        ant_hvf_report_timeout(vm);
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
    pthread_mutex_lock(&timeout.lock);
    timeout.stop = true;
    pthread_cond_signal(&timeout.cond);
    pthread_mutex_unlock(&timeout.lock);
    pthread_join(timeout_thread, NULL);
    pthread_cond_destroy(&timeout.cond);
    pthread_mutex_destroy(&timeout.lock);
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
  
  if (vm && ant_hvf_vsock_exit_received(vm))
    ant_hvf_set_result(result, ANT_SANDBOX_VM_RESULT_GUEST_EXIT, ant_hvf_vsock_exit_code(vm));
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
  else if (rc == -ECANCELED || (vm && atomic_load_explicit(&vm->canceled, memory_order_acquire)))
    ant_hvf_set_result(result, ANT_SANDBOX_VM_RESULT_CANCELED, rc ? rc : -ECANCELED);
  else if (vm && ant_hvf_vsock_protocol_error(vm))
    ant_hvf_set_result(result, ANT_SANDBOX_VM_RESULT_PROTOCOL_ERROR, rc);
  else if (vm && ant_hvf_vsock_transport_error(vm))
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

  if (!ant_hvf_vsock_exit_received(vm) && ant_hvf_uart_has_panic(vm)) ant_hvf_uart_report_panic(vm);
  else ant_hvf_uart_discard(vm);
  
  ant_hvf_9p_report(vm);
  ant_hvf_net_stop(vm);
  
  if (session->vcpu_created) {
    int destroy_rc = ant_hvf_check(ant_hvf_api.vcpu_destroy(vm->vcpu), "hv_vcpu_destroy");
    if (rc == 0) rc = destroy_rc;
  }
  
  if (session->mem_mapped) {
    int unmap_rc = ant_hvf_check(ant_hvf_api.vm_unmap(ANT_HVF_GUEST_BASE, vm->mem_size), "hv_vm_unmap");
    if (rc == 0) rc = unmap_rc;
  }
  
  if (session->vm_created) {
    int destroy_rc = ant_hvf_check(ant_hvf_api.vm_destroy(), "hv_vm_destroy");
    if (rc == 0) rc = destroy_rc;
  }
  
  if (vm->image_fd >= 0) close(vm->image_fd);
  if (vm->host_mem && vm->host_mem != MAP_FAILED) munmap(vm->host_mem, vm->mem_size);
  
  ant_devices_free(vm->devices);
  vm->devices = NULL;
  
  ant_hvf_vsock_clear_frames(vm);
  if (vm->idle_sync_init) {
    pthread_cond_destroy(&vm->idle_cond);
    pthread_mutex_destroy(&vm->idle_lock);
    vm->idle_sync_init = false;
  }
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

  rc = ant_hvf_load_api();
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
  vm->net_allow_host_ports = config->allow_host_ports;
  vm->net_allow_host_port_count = config->allow_host_port_count;
  vm->net_allow_host_all = config->allow_host_all;
  vm->net_allow_lan = config->allow_lan;
  
  if (config->mount_count == 0 || config->mount_count > ANT_HVF_VIRTIO_9P_MAX) {
    free(session);
    ant_hvf_set_result(config->result, ANT_SANDBOX_VM_RESULT_CONFIG_ERROR, -EINVAL);
    return -EINVAL;
  }
  
  vm->p9_count = config->mount_count;
  
  ant_devices_add_block(ant_hvf_devices(vm), ANT_HVF_VIRTIO_BLK_SLOT, ANT_HVF_VIRTIO_BLK_BAR);
  ant_devices_add_net(ant_hvf_devices(vm), ANT_HVF_VIRTIO_NET_SLOT, ANT_HVF_VIRTIO_NET_BAR);
  memcpy(vm->net_mac, (uint8_t[]){ 0x02, 0x41, 0x4e, 0x54, 0x00, 0x01 }, sizeof(vm->net_mac));
  ant_devices_add_vsock(ant_hvf_devices(vm), ANT_HVF_VIRTIO_VSOCK_SLOT, ANT_HVF_VIRTIO_VSOCK_BAR);
  ant_devices_add_rng(ant_hvf_devices(vm), ANT_HVF_VIRTIO_RNG_SLOT, ANT_HVF_VIRTIO_RNG_BAR);
  ant_devices_set_capabilities(ant_hvf_devices(vm), config->capabilities);
  
  for (size_t i = 0; i < vm->p9_count; i++) {
    rc = ant_devices_add_share(
      ant_hvf_devices(vm),
      (uint8_t)(ANT_HVF_VIRTIO_9P_SLOT_BASE + i),
      (uint32_t)(ANT_HVF_VIRTIO_9P_BAR_BASE + i * ANT_HVF_VIRTIO_BAR_SIZE),
      config->mounts[i].host_path,
      config->mounts[i].tag,
      config->mounts[i].readonly
    ) ? 0 : -EINVAL;
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

  int idle_lock_rc = pthread_mutex_init(&vm->idle_lock, NULL);
  if (idle_lock_rc != 0) { rc = -idle_lock_rc; goto fail; }
  int idle_cond_rc = pthread_cond_init(&vm->idle_cond, NULL);
  if (idle_cond_rc != 0) {
    pthread_mutex_destroy(&vm->idle_lock);
    rc = -idle_cond_rc;
    goto fail;
  }
  vm->idle_sync_init = true;

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
    ant_hvf_api.vm_map(
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

  rc = ant_hvf_build_dtb(vm, 3);
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

  ant_hvf_vsock_begin_request(vm);
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
    ant_hvf_vsock_fail_transport(vm);
    ant_hvf_classify_result(vm, request->result, maybe_sent);
    return maybe_sent;
  }

  int rc = ant_hvf_run(vm, timeout_ms, timeout_until_request_sent);
  if (rc == -ETIMEDOUT) fprintf(stderr, "sandbox vm: guest timed out\n");
  if (!ant_hvf_vsock_exit_received(vm) && ant_hvf_uart_has_panic(vm) && rc == 0) rc = -EFAULT;
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
static int ant_hvf_session_stats(void *session, ant_sandbox_vm_stats_t *stats) { return -ENOSYS; }
static int ant_hvf_session_cancel(void *session) { return -ENOSYS; }
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
