#include "backend.h"

#if defined(__aarch64__)

ant_hvf_gic_api_t ant_hvf_gic;

void *ant_hvf_sym(const char *name) {
  return dlsym(RTLD_DEFAULT, name);
}

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
      if (vm->trace) {
        fprintf(stderr,
                "sandbox vm: GIC MSI range base=%u count=%u region=0x%llx\n",
                msi_base,
                msi_count,
                (unsigned long long)ANT_HVF_GIC_MSI_BASE);
      }
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
  int rc = ant_hvf_check(hv_vcpu_set_reg(vcpu, reg, value), name);
  return rc;
}

uint64_t ant_hvf_host_cntfrq(void) {
  uint64_t cntfrq = 0;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
  return cntfrq;
}

uint64_t ant_hvf_host_cntvct(void) {
  uint64_t cntvct = 0;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cntvct));
  return cntvct;
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
  rc = ant_hvf_check(hv_vcpu_set_sys_reg(vm->vcpu, HV_SYS_REG_MPIDR_EL1, 0),
                     "hv_vcpu_set_sys_reg(MPIDR_EL1)");
  if (rc != 0) return rc;
  hv_return_t cntfrq_rc = hv_vcpu_set_sys_reg(vm->vcpu, ANT_HVF_SYS_REG_CNTFRQ_EL0, vm->cntfrq);
  if (cntfrq_rc != HV_SUCCESS && vm->trace) {
    fprintf(stderr, "sandbox vm: HVF does not allow setting CNTFRQ_EL0 (%d)\n", cntfrq_rc);
  }
  return ant_hvf_check(hv_vcpu_set_vtimer_mask(vm->vcpu, false),
                       "hv_vcpu_set_vtimer_mask(init)");
}

int ant_hvf_handle_wfx(ant_hvf_vm_t *vm) {
  uint64_t ctl = 0;
  uint64_t cval = 0;
  int rc = ant_hvf_check(hv_vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_CNTV_CTL_EL0, &ctl),
                         "hv_vcpu_get_sys_reg(CNTV_CTL_EL0)");
  if (rc != 0) return rc;

  rc = ant_hvf_advance_pc(vm->vcpu);
  if (rc != 0) return rc;

  if ((ctl & 1u) == 0 || (ctl & 2u) != 0) {
    if (vm->trace) fprintf(stderr, "sandbox vm: wfx wait without active vtimer\n");
    return 0;
  }

  rc = ant_hvf_check(hv_vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_CNTV_CVAL_EL0, &cval),
                     "hv_vcpu_get_sys_reg(CNTV_CVAL_EL0)");
  if (rc != 0) return rc;

  uint64_t now = ant_hvf_host_cntvct();
  if (cval > now && vm->cntfrq > 0) {
    uint64_t ticks = cval - now;
    uint64_t ns = (ticks * 1000000000ull) / vm->cntfrq;
    if (ns > 100000000ull) ns = 100000000ull;
    if (vm->trace) {
      fprintf(stderr,
              "sandbox vm: wfx timer wait ticks=%llu ns=%llu\n",
              (unsigned long long)ticks,
              (unsigned long long)ns);
    }
    if (ns > 0) {
      struct timespec ts = {
        .tv_sec = (time_t)(ns / 1000000000ull),
        .tv_nsec = (long)(ns % 1000000000ull),
      };
      while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
    }
  } else if (vm->trace) {
    fprintf(stderr, "sandbox vm: wfx timer already expired\n");
  }

  return ant_hvf_sync_vtimer(vm);
}

int ant_hvf_sync_vtimer(ant_hvf_vm_t *vm) {
  uint64_t ctl = 0;
  uint64_t cval = 0;
  if (hv_vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_CNTV_CTL_EL0, &ctl) != HV_SUCCESS ||
      hv_vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_CNTV_CVAL_EL0, &cval) != HV_SUCCESS) {
    return 0;
  }
  if ((ctl & 1u) == 0 || (ctl & 2u) != 0 || ant_hvf_host_cntvct() < cval) return 0;
  return ant_hvf_raise_vtimer(vm, "vtimer sync");
}

int ant_hvf_raise_vtimer(ant_hvf_vm_t *vm, const char *where) {
  hv_return_t gic_rc = ant_hvf_gic.set_redistributor_reg(
    vm->vcpu,
    (ant_hvf_gic_redistributor_reg_t)ANT_HVF_GICR_ISPENDR0,
    1ull << ANT_HVF_GIC_EL1_VIRTUAL_TIMER);
  if (gic_rc != HV_SUCCESS && vm->trace) {
    fprintf(stderr, "sandbox vm: HVF denied direct vtimer PPI pending at %s (%d)\n",
            where,
            gic_rc);
  }
  hv_return_t irq_rc = hv_vcpu_set_pending_interrupt(vm->vcpu, HV_INTERRUPT_TYPE_IRQ, true);
  if (irq_rc != HV_SUCCESS && vm->trace) {
    fprintf(stderr, "sandbox vm: HVF denied vtimer IRQ-line assertion at %s (%d)\n",
            where,
            irq_rc);
  }
  return ant_hvf_check(hv_vcpu_set_vtimer_mask(vm->vcpu, false),
                       "hv_vcpu_set_vtimer_mask(vtimer)");
}

#endif
