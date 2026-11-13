#include "kvm_internal.h" // IWYU pragma: keep
#if defined(__linux__) && defined(__aarch64__)

#ifndef PSR_MODE_EL1h
#  define PSR_MODE_EL1h 0x00000005u
#endif
#ifndef PSR_F_BIT
#  define PSR_F_BIT 0x00000040u
#endif
#ifndef PSR_I_BIT
#  define PSR_I_BIT 0x00000080u
#endif
#ifndef PSR_A_BIT
#  define PSR_A_BIT 0x00000100u
#endif
#ifndef PSR_D_BIT
#  define PSR_D_BIT 0x00000200u
#endif

#define ANT_KVM_REG_ID_AA64PFR0_EL1 ARM64_SYS_REG(3, 0, 0, 4, 0)
#define ANT_KVM_REG_MPIDR_EL1 ARM64_SYS_REG(3, 0, 0, 0, 5)
#define ANT_KVM_REG_SPSR_EL1 ARM64_SYS_REG(3, 0, 4, 0, 0)
#define ANT_KVM_REG_ELR_EL1 ARM64_SYS_REG(3, 0, 4, 0, 1)
#define ANT_KVM_REG_ESR_EL1 ARM64_SYS_REG(3, 0, 5, 2, 0)
#define ANT_KVM_REG_FAR_EL1 ARM64_SYS_REG(3, 0, 6, 0, 0)
#define ANT_KVM_REG_VBAR_EL1 ARM64_SYS_REG(3, 0, 12, 0, 0)

static int ant_kvm_set_device_attr(int fd, uint32_t group, uint64_t attr_id, void *value, const char *op) {
  struct kvm_device_attr attr = {
    .group = group,
    .attr = attr_id,
    .addr = (uint64_t)(uintptr_t)value,
  };
  return ant_kvm_ioctl(fd, KVM_SET_DEVICE_ATTR, &attr, op);
}

static int ant_kvm_create_gic(ant_hvf_vm_t *vm) {
  struct kvm_create_device dev = {
    .type = KVM_DEV_TYPE_ARM_VGIC_V3,
    .fd = -1,
  };
  int rc = 0;
  if (ioctl(vm->vm_fd, KVM_CREATE_DEVICE, &dev) == 0) {
    vm->gic_version = 3;
  } else {
    int v3_err = errno;
    dev.type = KVM_DEV_TYPE_ARM_VGIC_V2;
    dev.fd = -1;
    if (ioctl(vm->vm_fd, KVM_CREATE_DEVICE, &dev) == 0) {
      vm->gic_version = 2;
      ant_hvf_verbosef(vm, "VGICv3 unavailable (%s); using VGICv2", strerror(v3_err));
    } else {
      rc = -errno;
      fprintf(
        stderr,
        "sandbox vm: KVM_CREATE_DEVICE(VGICv3/VGICv2) failed: v3=%s v2=%s\n",
        strerror(v3_err),
        strerror(errno)
      );
      return rc;
    }
  }
  vm->gic_fd = dev.fd;

  uint64_t dist = ANT_HVF_GIC_DIST_BASE;
  rc = ant_kvm_set_device_attr(
    vm->gic_fd,
    KVM_DEV_ARM_VGIC_GRP_ADDR,
    vm->gic_version == 2 ? KVM_VGIC_V2_ADDR_TYPE_DIST : KVM_VGIC_V3_ADDR_TYPE_DIST,
    &dist, "KVM_SET_DEVICE_ATTR(VGIC dist)"
  );
  if (rc != 0) return rc;

  uint64_t redist = vm->gic_version == 2 ? ANT_HVF_GIC_CPU_BASE : ANT_HVF_GIC_REDIST_BASE;
  rc = ant_kvm_set_device_attr(
    vm->gic_fd,
     KVM_DEV_ARM_VGIC_GRP_ADDR,
     vm->gic_version == 2 ? KVM_VGIC_V2_ADDR_TYPE_CPU : KVM_VGIC_V3_ADDR_TYPE_REDIST,
     &redist, vm->gic_version == 2 ?
       "KVM_SET_DEVICE_ATTR(VGIC cpu)" :
       "KVM_SET_DEVICE_ATTR(VGIC redist)"
  );
  
  if (rc != 0) return rc;
  uint32_t nr_irqs = 128;
  rc = ant_kvm_set_device_attr(
    vm->gic_fd,
    KVM_DEV_ARM_VGIC_GRP_NR_IRQS, 0,
    &nr_irqs,
    "KVM_SET_DEVICE_ATTR(VGIC nr-irqs)"
  );
  
  if (rc != 0) return rc;
  vm->gic_msi_enabled = true;
  vm->gic_msi_base = ANT_HVF_GIC_MSI_VECTOR_BASE;
  vm->gic_msi_count = ANT_HVF_GIC_MSI_VECTOR_COUNT;
  
  return 0;
}

static int ant_kvm_finalize_gic(ant_hvf_vm_t *vm) {
  return ant_kvm_set_device_attr(
    vm->gic_fd, KVM_DEV_ARM_VGIC_GRP_CTRL,  KVM_DEV_ARM_VGIC_CTRL_INIT,
    NULL, "KVM_SET_DEVICE_ATTR(VGIC init)"
  );
}

static int ant_kvm_set_one_reg64(ant_hvf_vm_t *vm, uint64_t id, uint64_t value, const char *name) {
  struct kvm_one_reg reg = {
    .id = id,
    .addr = (uint64_t)(uintptr_t)&value,
  };
  
  if (ioctl(vm->vcpu_fd, KVM_SET_ONE_REG, &reg) == 0) return 0;
  int rc = -errno;
  
  fprintf(stderr, "sandbox vm: KVM_SET_ONE_REG(%s) failed: %s\n", name, strerror(errno));
  return rc;
}

static int ant_kvm_get_one_reg64(ant_hvf_vm_t *vm, uint64_t id, uint64_t *value, const char *name) {
  struct kvm_one_reg reg = {
    .id = id,
    .addr = (uint64_t)(uintptr_t)value,
  };
  
  if (ioctl(vm->vcpu_fd, KVM_GET_ONE_REG, &reg) == 0) return 0;
  int rc = -errno;
  if (vm->verbose) ant_hvf_verbosef(vm, "KVM_GET_ONE_REG(%s) failed: %s", name, strerror(errno));
    
  return rc;
}

#define ANT_KVM_CORE_REG(field) \
  (KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE | KVM_REG_ARM_CORE_REG(field))

static const struct {
  const char *name;
  uint64_t id;
} ant_kvm_report_regs[] = {
  { "pstate", ANT_KVM_CORE_REG(regs.pstate) },
  { "x0", ANT_KVM_CORE_REG(regs.regs[0]) },
  { "x1", ANT_KVM_CORE_REG(regs.regs[1]) },
  { "sp_el1", ANT_KVM_CORE_REG(sp_el1) },
  { "spsr_el1", ANT_KVM_REG_SPSR_EL1 },
  { "elr_el1", ANT_KVM_REG_ELR_EL1 },
  { "esr_el1", ANT_KVM_REG_ESR_EL1 },
  { "far_el1", ANT_KVM_REG_FAR_EL1 },
  { "vbar_el1", ANT_KVM_REG_VBAR_EL1 },
  { "cntv_ctl", KVM_REG_ARM_TIMER_CTL },
  { "cntv_cval", KVM_REG_ARM_TIMER_CVAL },
  { "cntvct", KVM_REG_ARM_TIMER_CNT },
  { "id_aa64pfr0_el1", ANT_KVM_REG_ID_AA64PFR0_EL1 },
};

static uint64_t ant_kvm_guest_symbol(ant_hvf_vm_t *vm, uint64_t symbol) {
  uint64_t value = 0;
  if (symbol != 0) (void)ant_hvf_guest_read(vm, symbol, &value, sizeof(value));
  return value;
}

static void ant_kvm_report_aarch64_regs(ant_hvf_vm_t *vm, const char *reason) {
  ant_hvf_dump_t dump = {0};

  uint64_t pc = 0;
  (void)ant_kvm_get_one_reg64(vm, ANT_KVM_CORE_REG(regs.pc), &pc, "pc");
  uint64_t kas_offset = ant_kvm_guest_symbol(vm, vm->kas_offset_symbol);
  uint64_t kas_kern_offset = ant_kvm_guest_symbol(vm, vm->kas_kern_offset_symbol);
  uint64_t slide = kas_kern_offset ? kas_kern_offset : kas_offset;

  ant_hvf_dump_add(&dump, "pc", pc);
  ant_hvf_dump_add(&dump, "low_pc", slide ? pc - slide : 0);
  ant_hvf_dump_add(&dump, "kas_offset", kas_offset);
  ant_hvf_dump_add(&dump, "kas_kern_offset", kas_kern_offset);
  ant_hvf_dump_add(&dump, "pcie_ecam_base", ant_kvm_guest_symbol(vm, vm->pcie_ecam_base_symbol));

  for (size_t i = 0; i < sizeof(ant_kvm_report_regs) / sizeof(ant_kvm_report_regs[0]); i++) {
    uint64_t value = 0;
    (void)ant_kvm_get_one_reg64(vm, ant_kvm_report_regs[i].id, &value, ant_kvm_report_regs[i].name);
    ant_hvf_dump_add(&dump, ant_kvm_report_regs[i].name, value);
  }

  ant_hvf_dump_add(&dump, "cntfrq", vm->cntfrq);
  ant_hvf_dump_report(&dump, reason);
}

static int ant_kvm_init_vcpu(ant_hvf_vm_t *vm) {
  struct kvm_vcpu_init init;
  memset(&init, 0, sizeof(init));
  if (ioctl(vm->vm_fd, KVM_ARM_PREFERRED_TARGET, &init) < 0) return -errno;
  init.features[0] |= 1u << KVM_ARM_VCPU_PSCI_0_2;
  if (ioctl(vm->vcpu_fd, KVM_ARM_VCPU_INIT, &init) < 0) return -errno;

  int rc = ant_kvm_set_one_reg64(
    vm, KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
    KVM_REG_ARM_CORE_REG(regs.pc),
    vm->kernel_entry, "pc"
  );
  if (rc != 0) return rc;
  rc = ant_kvm_set_one_reg64(
    vm, KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
    KVM_REG_ARM_CORE_REG(regs.regs[0]),
    ANT_HVF_DTB_BASE, "x0"
  );
  if (rc != 0) return rc;
  rc = ant_kvm_set_one_reg64(
    vm, KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
    KVM_REG_ARM_CORE_REG(regs.regs[1]),
    0, "x1"
  );
  if (rc != 0) return rc;
  rc = ant_kvm_set_one_reg64(
    vm, KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
    KVM_REG_ARM_CORE_REG(regs.regs[2]),
    0, "x2"
  );
  if (rc != 0) return rc;
  rc = ant_kvm_set_one_reg64(
    vm, KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
    KVM_REG_ARM_CORE_REG(regs.regs[3]),
    0, "x3"
  );
  if (rc != 0) return rc;
  rc = ant_kvm_set_one_reg64(
    vm, KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
    KVM_REG_ARM_CORE_REG(regs.pstate),
    PSR_MODE_EL1h | PSR_F_BIT | PSR_I_BIT | PSR_A_BIT | PSR_D_BIT, "pstate"
  );
  if (rc != 0) return rc;
  rc = ant_kvm_set_one_reg64(vm, ANT_KVM_REG_MPIDR_EL1, 0, "mpidr_el1");
  if (rc != 0 && vm->verbose) ant_hvf_verbosef(vm, "continuing with KVM-provided MPIDR_EL1");
  rc = ant_kvm_set_one_reg64(
    vm, KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
    KVM_REG_ARM_CORE_REG(sp_el1),
    ANT_HVF_GUEST_BASE + vm->mem_size - ANT_HVF_PAGE_SIZE, "sp_el1"
  );
  if (rc != 0) return rc;
  return 0;
}

static int ant_kvm_set_vcpu_device_attr_int(
  ant_hvf_vm_t *vm,
  uint32_t group,
  uint64_t attr_id,
  int value,
  const char *op
) {
  struct kvm_device_attr attr = {
    .group = group,
    .attr = attr_id,
    .addr = (uint64_t)(uintptr_t)&value,
  };
  return ant_kvm_ioctl(vm->vcpu_fd, KVM_SET_DEVICE_ATTR, &attr, op);
}

static int ant_kvm_init_vcpu_timer(ant_hvf_vm_t *vm) {
  int rc = ant_kvm_set_vcpu_device_attr_int(vm,
    KVM_ARM_VCPU_TIMER_CTRL,
    KVM_ARM_VCPU_TIMER_IRQ_VTIMER,
    27, "KVM_SET_DEVICE_ATTR(vtimer irq)"
  );
  
  if (rc != 0) {
    ant_hvf_verbosef(vm, "KVM timer IRQ override unavailable; using kernel defaults");
    return 0;
  }
  
  rc = ant_kvm_set_vcpu_device_attr_int(vm,
    KVM_ARM_VCPU_TIMER_CTRL,
    KVM_ARM_VCPU_TIMER_IRQ_PTIMER,
    30, "KVM_SET_DEVICE_ATTR(ptimer irq)"
  );
  if (rc != 0) return rc;
  return 0;
}

static int ant_kvm_raise_spi(ant_hvf_vm_t *vm, uint32_t intid) {
  if (intid < 32) return -EINVAL;
  uint32_t irq = (KVM_ARM_IRQ_TYPE_SPI << KVM_ARM_IRQ_TYPE_SHIFT) |
                 ((intid & KVM_ARM_IRQ_NUM_MASK) << KVM_ARM_IRQ_NUM_SHIFT);
  struct kvm_irq_level level = {
    .irq = irq,
    .level = 1,
  };
  int rc = ioctl(vm->vm_fd, KVM_IRQ_LINE, &level) == 0 ? 0 : -errno;
  level.level = 0;
  int rc2 = ioctl(vm->vm_fd, KVM_IRQ_LINE, &level) == 0 ? 0 : -errno;
  return rc != 0 ? rc : rc2;
}

int ant_hvf_send_msi(ant_hvf_vm_t *vm, uint64_t addr, uint32_t data) {
  return ant_kvm_raise_spi(vm, data);
}

int ant_kvm_arch_handle_mmio(ant_hvf_vm_t *vm) {
  struct kvm_run *run = vm->run;
  uint64_t value = 0;
  if (run->mmio.len > sizeof(value)) return -EINVAL;
  if (run->mmio.is_write) {
    memcpy(&value, run->mmio.data, run->mmio.len);
    return ant_hvf_mmio_write(vm, run->mmio.phys_addr, run->mmio.len, value) ? 0 : -ENOSYS;
  }
  if (!ant_hvf_mmio_read(vm, run->mmio.phys_addr, run->mmio.len, &value)) return -ENOSYS;
  memcpy(run->mmio.data, &value, run->mmio.len);
  return 0;
}

int ant_kvm_arch_handle_exit(ant_hvf_vm_t *vm, bool *stop) {
  if (vm->run->exit_reason != KVM_EXIT_SYSTEM_EVENT) return -ENOTSUP;

  *stop = true;
  if (vm->run->system_event.type == KVM_SYSTEM_EVENT_SHUTDOWN) return 0;
  fprintf(stderr, "sandbox vm: KVM system event type=%u\n", vm->run->system_event.type);
  return -EIO;
}

void ant_kvm_arch_report_timeout(ant_hvf_vm_t *vm) {
  ant_kvm_report_aarch64_regs(vm, "guest timed out");
}

int ant_kvm_arch_setup_vm(ant_kvm_session_t *session) {
  ant_hvf_vm_t *vm = &session->vm;
  vm->cntfrq = ant_hvf_host_cntfrq();
  return vm->cntfrq == 0 || vm->cntfrq > UINT32_MAX ? -EINVAL : 0;
}

int ant_kvm_arch_prepare_boot(ant_hvf_vm_t *vm, const char *kernel_path) {
  (void)ant_kvm_find_symbol(kernel_path, "kernel_phys_offset", &vm->kas_offset_symbol);
  (void)ant_kvm_find_symbol(kernel_path, "kas_kern_offset", &vm->kas_kern_offset_symbol);
  (void)ant_kvm_find_symbol(kernel_path, "pcie_ecam_base", &vm->pcie_ecam_base_symbol);
  return 0;
}

int ant_kvm_arch_setup_vcpu(ant_kvm_session_t *session) {
  ant_hvf_vm_t *vm = &session->vm;
  int rc = ant_kvm_create_gic(vm);
  if (rc != 0) return rc;
  session->irqchip_created = true;

  rc = ant_hvf_build_dtb(vm, vm->gic_version);
  if (rc == 0) rc = ant_kvm_init_vcpu(vm);
  if (rc == 0) rc = ant_kvm_init_vcpu_timer(vm);
  if (rc == 0) rc = ant_kvm_finalize_gic(vm);
  if (rc != 0) return rc;

  ant_hvf_verbosef(
    vm, "entry=0x%llx cntfrq=%llu gic=v%d",
    (unsigned long long)vm->kernel_entry, (unsigned long long)vm->cntfrq, vm->gic_version
  );
  return 0;
}

#endif
