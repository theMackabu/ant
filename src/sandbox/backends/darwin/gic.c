#include "backend.h"

#include <mach/mach_time.h>

#if defined(__aarch64__)

static constexpr uint64_t ANT_HVF_WFI_MAX_WAIT_NS = 100000000ull;

ant_hvf_gic_api_t ant_hvf_gic;

int ant_hvf_load_gic_api(void) {
  ant_hvf_gic.create = (ant_hvf_gic_create_fn)ant_hvf_sym("hv_gic_create");
  ant_hvf_gic.config_create = (ant_hvf_gic_config_create_fn)ant_hvf_sym("hv_gic_config_create");
  ant_hvf_gic.set_distributor_base =
    (ant_hvf_gic_config_set_base_fn)ant_hvf_sym("hv_gic_config_set_distributor_base");
  ant_hvf_gic.set_redistributor_base =
    (ant_hvf_gic_config_set_base_fn)ant_hvf_sym("hv_gic_config_set_redistributor_base");
  ant_hvf_gic.set_msi_region_base =
    (ant_hvf_gic_config_set_base_fn)ant_hvf_sym("hv_gic_config_set_msi_region_base");
  ant_hvf_gic.set_msi_interrupt_range =
    (ant_hvf_gic_config_set_range_fn)ant_hvf_sym("hv_gic_config_set_msi_interrupt_range");
  ant_hvf_gic.set_spi = (ant_hvf_gic_set_spi_fn)ant_hvf_sym("hv_gic_set_spi");
  ant_hvf_gic.set_redistributor_reg =
    (ant_hvf_gic_set_redistributor_reg_fn)ant_hvf_sym("hv_gic_set_redistributor_reg");
  ant_hvf_gic.send_msi = (ant_hvf_gic_send_msi_fn)ant_hvf_sym("hv_gic_send_msi");

  if (!ant_hvf_gic.create ||
      !ant_hvf_gic.config_create ||
      !ant_hvf_gic.set_distributor_base ||
      !ant_hvf_gic.set_redistributor_base ||
      !ant_hvf_gic.set_spi ||
      !ant_hvf_gic.set_redistributor_reg) {
    fprintf(stderr, "sandbox vm: Hypervisor.framework GIC symbols are unavailable on this SDK/runtime\n");
    return -ENOSYS;
  }

  return 0;
}

int ant_hvf_create_gic(ant_hvf_vm_t *vm) {
  int rc = ant_hvf_load_gic_api();
  if (rc != 0) return rc;

  ant_hvf_gic_config_t gic_config = ant_hvf_gic.config_create();
  if (!gic_config) return -EIO;

  rc = ant_hvf_check(ant_hvf_gic.set_distributor_base(gic_config, ANT_HVF_GIC_DIST_BASE),
                     "hv_gic_config_set_distributor_base");
  if (rc == 0) rc = ant_hvf_check(
    ant_hvf_gic.set_redistributor_base(gic_config, ANT_HVF_GIC_REDIST_BASE),
    "hv_gic_config_set_redistributor_base");
  if (rc == 0) {
    if (!ant_hvf_gic.set_msi_region_base ||
        !ant_hvf_gic.set_msi_interrupt_range ||
        !ant_hvf_gic.send_msi) {
      fprintf(stderr, "sandbox vm: Hypervisor.framework GIC MSI symbols are unavailable\n");
      rc = -ENOSYS;
      goto done;
    }
    const uint32_t msi_base = ANT_HVF_GIC_MSI_VECTOR_BASE;
    const uint32_t msi_count = ANT_HVF_GIC_MSI_VECTOR_COUNT;
    hv_return_t msi_rc = ant_hvf_gic.set_msi_region_base(gic_config, ANT_HVF_GIC_MSI_BASE);
    if (msi_rc == 0) {
      msi_rc = ant_hvf_gic.set_msi_interrupt_range(gic_config, msi_base, msi_count);
    }
    if (msi_rc == 0) {
      vm->gic_msi_enabled = true;
      vm->gic_msi_base = msi_base;
      vm->gic_msi_count = msi_count;
    } else {
      fprintf(stderr,
              "sandbox vm: failed to configure GIC MSI support msi_rc=%d\n",
              msi_rc);
      rc = -EIO;
      goto done;
    }
  }
  if (rc == 0) rc = ant_hvf_check(ant_hvf_gic.create(gic_config), "hv_gic_create");

done:
  os_release(gic_config);
  return rc;
}

int ant_hvf_set_reg(hv_vcpu_t vcpu, hv_reg_t reg, uint64_t value, const char *name) {
  int rc = ant_hvf_check(ant_hvf_api.vcpu_set_reg(vcpu, reg, value), name);
  return rc;
}

uint64_t ant_hvf_host_cntfrq(void) {
  uint64_t cntfrq = 0;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
  return cntfrq;
}

int ant_hvf_guest_cntvct(ant_hvf_vm_t *vm, uint64_t *out) {
  uint64_t offset = 0;
  int rc = ant_hvf_check(
    ant_hvf_api.vcpu_get_vtimer_offset(vm->vcpu, &offset),
    "hv_vcpu_get_vtimer_offset"
  );
  if (rc != 0) return rc;
  *out = mach_absolute_time() - offset;
  return 0;
}

int ant_hvf_init_vcpu(ant_hvf_vm_t *vm) {
  int rc = ant_hvf_set_reg(vm->vcpu, HV_REG_PC, vm->kernel_entry, "hv_vcpu_set_reg(PC)");
  if (rc != 0) return rc;
  rc = ant_hvf_set_reg(vm->vcpu, HV_REG_X0, ANT_HVF_DTB_BASE, "hv_vcpu_set_reg(X0)");
  if (rc != 0) return rc;
  rc = ant_hvf_set_reg(vm->vcpu, HV_REG_X1, 0, "hv_vcpu_set_reg(X1)");
  if (rc != 0) return rc;
  rc = ant_hvf_set_reg(vm->vcpu, HV_REG_CPSR, 0x3c5, "hv_vcpu_set_reg(CPSR)");
  if (rc != 0) return rc;
  rc = ant_hvf_check(ant_hvf_api.vcpu_set_sys_reg(vm->vcpu, HV_SYS_REG_MPIDR_EL1, 0),
                     "hv_vcpu_set_sys_reg(MPIDR_EL1)");
  if (rc != 0) return rc;
  hv_return_t cntfrq_rc =
    ant_hvf_api.vcpu_set_sys_reg(vm->vcpu, ANT_HVF_SYS_REG_CNTFRQ_EL0, vm->cntfrq);
  if (cntfrq_rc == HV_SUCCESS) {
    ant_hvf_verbosef(vm, "seeded CNTFRQ_EL0=%llu", (unsigned long long)vm->cntfrq);
  } else {
    ant_hvf_verbosef(vm,
                     "CNTFRQ_EL0 sysreg seed unavailable rc=%d; using timer frequency from device tree",
                     cntfrq_rc);
  }
  return ant_hvf_check(ant_hvf_api.vcpu_set_vtimer_mask(vm->vcpu, false),
                       "hv_vcpu_set_vtimer_mask(init)");
}

static void ant_hvf_idle_wait(ant_hvf_vm_t *vm, uint64_t ns) {
  struct timespec ts = {
    .tv_sec = (time_t)(ns / 1000000000ull),
    .tv_nsec = (long)(ns % 1000000000ull),
  };
  atomic_store_explicit(&vm->vcpu_running, true, memory_order_release);
  pthread_mutex_lock(&vm->idle_lock);
  if (!vm->idle_kicked && !atomic_load_explicit(&vm->vsock_wake_pending, memory_order_acquire))
    pthread_cond_timedwait_relative_np(&vm->idle_cond, &vm->idle_lock, &ts);
  vm->idle_kicked = false;
  pthread_mutex_unlock(&vm->idle_lock);
  atomic_store_explicit(&vm->vcpu_running, false, memory_order_release);
}

int ant_hvf_handle_wfx(ant_hvf_vm_t *vm) {
  bool wfe = (vm->vcpu_exit->exception.syndrome & ESR_WFX_TI_WFE) != 0;
  int rc = ant_hvf_advance_pc(vm->vcpu);
  if (rc != 0) return rc;
  if (wfe) return ant_hvf_sync_vtimer(vm);

  uint64_t ctl = 0;
  rc = ant_hvf_check(
    ant_hvf_api.vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_CNTV_CTL_EL0, &ctl),
    "hv_vcpu_get_sys_reg(CNTV_CTL_EL0)"
  );
  if (rc != 0) return rc;

  uint64_t ns = ANT_HVF_WFI_MAX_WAIT_NS;
  if ((ctl & 1u) != 0 && (ctl & 2u) == 0) {
    uint64_t cval = 0;
    rc = ant_hvf_check(
      ant_hvf_api.vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_CNTV_CVAL_EL0, &cval),
      "hv_vcpu_get_sys_reg(CNTV_CVAL_EL0)"
    );
    if (rc != 0) return rc;

    uint64_t now = 0;
    rc = ant_hvf_guest_cntvct(vm, &now);
    if (rc != 0) return rc;
    if (cval <= now) ns = 0;
    else if ((cval - now) < vm->cntfrq / 10) ns = ((cval - now) * 1000000000ull) / vm->cntfrq;
  }

  if (ns > 0) ant_hvf_idle_wait(vm, ns);
  return ant_hvf_sync_vtimer(vm);
}

int ant_hvf_sync_vtimer(ant_hvf_vm_t *vm) {
  uint64_t ctl = 0;
  uint64_t cval = 0;
  if (ant_hvf_api.vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_CNTV_CTL_EL0, &ctl) != HV_SUCCESS ||
    ant_hvf_api.vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_CNTV_CVAL_EL0, &cval) != HV_SUCCESS
  ) return 0;
  if ((ctl & 1u) == 0 || (ctl & 2u) != 0) return 0;
  uint64_t now = 0;
  int rc = ant_hvf_guest_cntvct(vm, &now);
  if (rc != 0) return rc;
  if (now < cval) return 0;
  return ant_hvf_raise_vtimer(vm, "vtimer sync");
}

int ant_hvf_raise_vtimer(ant_hvf_vm_t *vm, const char *where) {
  ant_hvf_gic.set_redistributor_reg(
    vm->vcpu,
    (ant_hvf_gic_redistributor_reg_t)ANT_HVF_GICR_ISPENDR0,
    1ull << ANT_HVF_GIC_EL1_VIRTUAL_TIMER);
  ant_hvf_api.vcpu_set_pending_interrupt(vm->vcpu, HV_INTERRUPT_TYPE_IRQ, true);
  return ant_hvf_check(
    ant_hvf_api.vcpu_set_vtimer_mask(vm->vcpu, false),
    "hv_vcpu_set_vtimer_mask(vtimer)"
  );
}

#endif
