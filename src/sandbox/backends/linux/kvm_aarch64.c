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

enum {
  ANT_KVM_AARCH64_DTB_MAX = 0x20000u,
  ANT_KVM_AARCH64_FDT_STRUCTURE_BYTES = 8192u,
  ANT_KVM_AARCH64_FDT_STRINGS_BYTES = 2048u,
  ANT_KVM_AARCH64_FDT_MAX_PROP_CELLS_32 = 32u,
  ANT_KVM_AARCH64_FDT_MAX_PROP_CELLS_64 = 16u,
  ANT_KVM_AARCH64_FDT_MAGIC = 0xd00dfeedu,
  ANT_KVM_AARCH64_FDT_BEGIN_NODE = 1u,
  ANT_KVM_AARCH64_FDT_END_NODE = 2u,
  ANT_KVM_AARCH64_FDT_PROP = 3u,
  ANT_KVM_AARCH64_FDT_END = 9u,
  ANT_KVM_AARCH64_GICM_TYPER = 0x0008u,
  ANT_KVM_AARCH64_GICM_SET_SPI_NSR = 0x0040u,
  ANT_KVM_AARCH64_GICM_IIDR = 0x0fccu,
};

typedef struct {
  uint32_t magic;
  uint32_t totalsize;
  uint32_t off_dt_struct;
  uint32_t off_dt_strings;
  uint32_t off_mem_rsvmap;
  uint32_t version;
  uint32_t last_comp_version;
  uint32_t boot_cpuid_phys;
  uint32_t size_dt_strings;
  uint32_t size_dt_struct;
} ant_kvm_fdt_header_t;

typedef struct {
  unsigned char *structure;
  unsigned char *strings;
  size_t structure_len;
  size_t structure_cap;
  size_t strings_len;
  size_t strings_cap;
} ant_kvm_fdt_t;

static uint64_t ant_kvm_host_cntfrq(void) {
  uint64_t cntfrq = 0;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
  return cntfrq;
}

static int ant_kvm_fdt_init(ant_kvm_fdt_t *fdt) {
  if (!fdt) return -EINVAL;
  memset(fdt, 0, sizeof(*fdt));
  fdt->structure_cap = ANT_KVM_AARCH64_FDT_STRUCTURE_BYTES;
  fdt->strings_cap = ANT_KVM_AARCH64_FDT_STRINGS_BYTES;
  fdt->structure = calloc(1, fdt->structure_cap);
  fdt->strings = calloc(1, fdt->strings_cap);
  if (!fdt->structure || !fdt->strings) {
    free(fdt->structure);
    free(fdt->strings);
    memset(fdt, 0, sizeof(*fdt));
    return -ENOMEM;
  }
  return 0;
}

static void ant_kvm_fdt_free(ant_kvm_fdt_t *fdt) {
  if (!fdt) return;
  free(fdt->structure);
  free(fdt->strings);
  memset(fdt, 0, sizeof(*fdt));
}

static int ant_kvm_fdt_grow(unsigned char **buf, size_t *cap, size_t used, size_t need) {
  if (need <= *cap - used) return 0;
  size_t new_cap = *cap ? *cap : 256u;
  while (need > new_cap - used) {
    if (new_cap >= ANT_KVM_AARCH64_DTB_MAX) return -ENOSPC;
    size_t next = new_cap * 2u;
    new_cap = next > ANT_KVM_AARCH64_DTB_MAX ? ANT_KVM_AARCH64_DTB_MAX : next;
  }
  unsigned char *new_buf = realloc(*buf, new_cap);
  if (!new_buf) return -ENOMEM;
  memset(new_buf + *cap, 0, new_cap - *cap);
  *buf = new_buf;
  *cap = new_cap;
  return 0;
}

static int ant_kvm_fdt_reserve(ant_kvm_fdt_t *fdt, size_t len, unsigned char **out) {
  size_t aligned = ant_align4(len);
  int rc = ant_kvm_fdt_grow(&fdt->structure, &fdt->structure_cap, fdt->structure_len, aligned);
  if (rc != 0) return rc;
  *out = fdt->structure + fdt->structure_len;
  memset(*out, 0, aligned);
  fdt->structure_len += aligned;
  return 0;
}

static int ant_kvm_fdt_u32(ant_kvm_fdt_t *fdt, uint32_t val) {
  unsigned char *out;
  int rc = ant_kvm_fdt_reserve(fdt, sizeof(uint32_t), &out);
  if (rc != 0) return rc;
  *(uint32_t *)out = ant_bswap32(val);
  return 0;
}

static int ant_kvm_fdt_string_offset(ant_kvm_fdt_t *fdt, const char *name) {
  size_t len = strlen(name) + 1;
  int rc = ant_kvm_fdt_grow(&fdt->strings, &fdt->strings_cap, fdt->strings_len, len);
  if (rc != 0) return rc;
  int off = (int)fdt->strings_len;
  memcpy(fdt->strings + fdt->strings_len, name, len);
  fdt->strings_len += len;
  return off;
}

static int ant_kvm_fdt_begin(ant_kvm_fdt_t *fdt, const char *name) {
  int rc = ant_kvm_fdt_u32(fdt, ANT_KVM_AARCH64_FDT_BEGIN_NODE);
  if (rc != 0) return rc;
  unsigned char *out;
  size_t len = strlen(name) + 1;
  rc = ant_kvm_fdt_reserve(fdt, len, &out);
  if (rc != 0) return rc;
  memcpy(out, name, len);
  return 0;
}

static int ant_kvm_fdt_end(ant_kvm_fdt_t *fdt) {
  return ant_kvm_fdt_u32(fdt, ANT_KVM_AARCH64_FDT_END_NODE);
}

static int ant_kvm_fdt_prop(ant_kvm_fdt_t *fdt, const char *name, const void *data, size_t len) {
  int nameoff = ant_kvm_fdt_string_offset(fdt, name);
  if (nameoff < 0) return nameoff;
  int rc = ant_kvm_fdt_u32(fdt, ANT_KVM_AARCH64_FDT_PROP);
  if (rc != 0) return rc;
  if ((rc = ant_kvm_fdt_u32(fdt, (uint32_t)len)) != 0) return rc;
  if ((rc = ant_kvm_fdt_u32(fdt, (uint32_t)nameoff)) != 0) return rc;
  unsigned char *out;
  rc = ant_kvm_fdt_reserve(fdt, len, &out);
  if (rc != 0) return rc;
  memcpy(out, data, len);
  return 0;
}

static int ant_kvm_fdt_prop_null(ant_kvm_fdt_t *fdt, const char *name) {
  return ant_kvm_fdt_prop(fdt, name, "", 0);
}

static int ant_kvm_fdt_prop_string(ant_kvm_fdt_t *fdt, const char *name, const char *value) {
  return ant_kvm_fdt_prop(fdt, name, value, strlen(value) + 1);
}

static int ant_kvm_fdt_prop_u32(ant_kvm_fdt_t *fdt, const char *name, uint32_t value) {
  uint32_t be = ant_bswap32(value);
  return ant_kvm_fdt_prop(fdt, name, &be, sizeof(be));
}

static int ant_kvm_fdt_prop_cells(ant_kvm_fdt_t *fdt, const char *name, const uint32_t *cells, size_t count) {
  uint32_t be[ANT_KVM_AARCH64_FDT_MAX_PROP_CELLS_32];
  if (count > sizeof(be) / sizeof(be[0])) return -EINVAL;
  for (size_t i = 0; i < count; i++) be[i] = ant_bswap32(cells[i]);
  return ant_kvm_fdt_prop(fdt, name, be, count * sizeof(be[0]));
}

static int ant_kvm_fdt_prop_reg64(ant_kvm_fdt_t *fdt, const char *name, const uint64_t *cells, size_t count) {
  uint64_t be[ANT_KVM_AARCH64_FDT_MAX_PROP_CELLS_64];
  if (count > sizeof(be) / sizeof(be[0])) return -EINVAL;
  for (size_t i = 0; i < count; i++) be[i] = ant_bswap64(cells[i]);
  return ant_kvm_fdt_prop(fdt, name, be, count * sizeof(be[0]));
}

static int ant_kvm_build_dtb(ant_hvf_vm_t *vm) {
  ant_kvm_fdt_t fdt;
  int rc = ant_kvm_fdt_init(&fdt);
  if (rc != 0) goto out;

  if ((rc = ant_kvm_fdt_begin(&fdt, "")) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_string(&fdt, "compatible", "linux,dummy-virt")) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_u32(&fdt, "#address-cells", 2)) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_u32(&fdt, "#size-cells", 2)) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_u32(&fdt, "interrupt-parent", 1)) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_null(&fdt, "ant,hvf-prohibit-dc-zva")) != 0) goto out;

  if ((rc = ant_kvm_fdt_begin(&fdt, "cpus")) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_u32(&fdt, "#address-cells", 2)) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_u32(&fdt, "#size-cells", 0)) != 0) goto out;
  if ((rc = ant_kvm_fdt_begin(&fdt, "cpu@0")) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_string(&fdt, "device_type", "cpu")) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_string(&fdt, "compatible", "arm,arm-v8")) != 0) goto out;
  uint64_t cpu_reg[] = { 0 };
  if ((rc = ant_kvm_fdt_prop_reg64(&fdt, "reg", cpu_reg, 1)) != 0) goto out;
  if ((rc = ant_kvm_fdt_end(&fdt)) != 0) goto out;
  if ((rc = ant_kvm_fdt_end(&fdt)) != 0) goto out;

  if ((rc = ant_kvm_fdt_begin(&fdt, "timer")) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_string(&fdt, "compatible", "arm,armv8-timer")) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_null(&fdt, "always-on")) != 0) goto out;
  if (vm->cntfrq == 0 || vm->cntfrq > UINT32_MAX) { rc = -EINVAL; goto out; }
  if ((rc = ant_kvm_fdt_prop_u32(&fdt, "clock-frequency", (uint32_t)vm->cntfrq)) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_u32(&fdt, "timebase-frequency", (uint32_t)vm->cntfrq)) != 0) goto out;
  uint32_t timer_irq[] = { 1, 13, 4, 1, 14, 4, 1, 11, 4, 1, 10, 4 };
  if ((rc = ant_kvm_fdt_prop_cells(&fdt, "interrupts", timer_irq, sizeof(timer_irq) / sizeof(timer_irq[0]))) != 0) goto out;
  if ((rc = ant_kvm_fdt_end(&fdt)) != 0) goto out;

  if ((rc = ant_kvm_fdt_begin(&fdt, "memory@40000000")) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_string(&fdt, "device_type", "memory")) != 0) goto out;
  uint64_t mem_reg[] = { ANT_HVF_GUEST_BASE, vm->mem_size };
  if ((rc = ant_kvm_fdt_prop_reg64(&fdt, "reg", mem_reg, 2)) != 0) goto out;
  if ((rc = ant_kvm_fdt_end(&fdt)) != 0) goto out;

  if ((rc = ant_kvm_fdt_begin(&fdt, "chosen")) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_string(&fdt, "bootargs", "")) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_string(&fdt, "stdout-path", "/uart@9000000")) != 0) goto out;
  if ((rc = ant_kvm_fdt_end(&fdt)) != 0) goto out;

  if ((rc = ant_kvm_fdt_begin(&fdt, "intc")) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_string(
    &fdt, "compatible",
    vm->gic_version == 2 ? "arm,cortex-a15-gic" : "arm,gic-v3")) != 0
  ) goto out;
  if ((rc = ant_kvm_fdt_prop_null(&fdt, "interrupt-controller")) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_u32(&fdt, "#interrupt-cells", 3)) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_u32(&fdt, "phandle", 1)) != 0) goto out;
  if (vm->gic_version == 2) {
    uint64_t gic_reg[] = { ANT_HVF_GIC_DIST_BASE, KVM_VGIC_V2_DIST_SIZE, ANT_HVF_GIC_CPU_BASE, KVM_VGIC_V2_CPU_SIZE };
    if ((rc = ant_kvm_fdt_prop_reg64(&fdt, "reg", gic_reg, 4)) != 0) goto out;
  } else {
    uint64_t gic_reg[] = { ANT_HVF_GIC_DIST_BASE, 0x10000, ANT_HVF_GIC_REDIST_BASE, 0x200000 };
    if ((rc = ant_kvm_fdt_prop_reg64(&fdt, "reg", gic_reg, 4)) != 0) goto out;
  }
  if ((rc = ant_kvm_fdt_end(&fdt)) != 0) goto out;

  if ((rc = ant_kvm_fdt_begin(&fdt, "uart@9000000")) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_string(&fdt, "compatible", "arm,pl011")) != 0) goto out;
  uint64_t uart_reg[] = { ANT_HVF_UART_BASE, 0x1000 };
  
  if ((rc = ant_kvm_fdt_prop_reg64(&fdt, "reg", uart_reg, 2)) != 0) goto out;
  uint32_t uart_irq[] = { 0, 1, 4 };
  
  if ((rc = ant_kvm_fdt_prop_cells(&fdt, "interrupts", uart_irq, 3)) != 0) goto out;
  if ((rc = ant_kvm_fdt_end(&fdt)) != 0) goto out;

  if ((rc = ant_kvm_fdt_begin(&fdt, "pcie@3f000000")) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_string(&fdt, "compatible", "pci-host-ecam-generic")) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_string(&fdt, "device_type", "pci")) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_u32(&fdt, "#address-cells", 3)) != 0) goto out;
  if ((rc = ant_kvm_fdt_prop_u32(&fdt, "#size-cells", 2)) != 0) goto out;
  
  uint64_t pcie_reg[] = { ANT_HVF_PCIE_ECAM_BASE, ANT_HVF_PCIE_ECAM_SIZE };
  if ((rc = ant_kvm_fdt_prop_reg64(&fdt, "reg", pcie_reg, 2)) != 0) goto out;
  
  uint32_t bus_range[] = { 0, 0 };
  if ((rc = ant_kvm_fdt_prop_cells(&fdt, "bus-range", bus_range, 2)) != 0) goto out;
  if ((rc = ant_kvm_fdt_end(&fdt)) != 0) goto out;

  if ((rc = ant_kvm_fdt_end(&fdt)) != 0) goto out;
  if ((rc = ant_kvm_fdt_u32(&fdt, ANT_KVM_AARCH64_FDT_END)) != 0) goto out;

  size_t off_mem = sizeof(ant_kvm_fdt_header_t);
  size_t off_struct = ant_align4(off_mem + 16);
  size_t off_strings = off_struct + fdt.structure_len;
  size_t total = off_strings + fdt.strings_len;
  if (total > ANT_KVM_AARCH64_DTB_MAX) { rc = -ENOSPC; goto out; }

  unsigned char *dtb = ant_hvf_guest_ptr(vm, ANT_HVF_DTB_BASE, total);
  if (!dtb) { rc = -EINVAL; goto out; }
  memset(dtb, 0, total);

  ant_kvm_fdt_header_t hdr = {
    .magic = ant_bswap32(ANT_KVM_AARCH64_FDT_MAGIC),
    .totalsize = ant_bswap32((uint32_t)total),
    .off_dt_struct = ant_bswap32((uint32_t)off_struct),
    .off_dt_strings = ant_bswap32((uint32_t)off_strings),
    .off_mem_rsvmap = ant_bswap32((uint32_t)off_mem),
    .version = ant_bswap32(17),
    .last_comp_version = ant_bswap32(16),
    .boot_cpuid_phys = 0,
    .size_dt_strings = ant_bswap32((uint32_t)fdt.strings_len),
    .size_dt_struct = ant_bswap32((uint32_t)fdt.structure_len),
  };
  
  memcpy(dtb, &hdr, sizeof(hdr));
  memcpy(dtb + off_struct, fdt.structure, fdt.structure_len);
  memcpy(dtb + off_strings, fdt.strings, fdt.strings_len);
  rc = 0;

out:
  ant_kvm_fdt_free(&fdt);
  return rc;
}

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

static void ant_kvm_report_aarch64_regs(ant_hvf_vm_t *vm, const char *reason) {
  uint64_t pc = 0;
  uint64_t pstate = 0;
  uint64_t x0 = 0;
  uint64_t x1 = 0;
  uint64_t sp_el1 = 0;
  uint64_t spsr_el1 = 0;
  uint64_t elr_el1 = 0;
  uint64_t esr_el1 = 0;
  uint64_t far_el1 = 0;
  uint64_t vbar_el1 = 0;
  uint64_t kas_offset = 0;
  uint64_t kas_kern_offset = 0;
  uint64_t pcie_ecam_base = 0;
  uint64_t cntv_ctl = 0;
  uint64_t cntv_cval = 0;
  uint64_t cntvct = 0;
  uint64_t id_aa64pfr0 = 0;
  (void)ant_kvm_get_one_reg64(
    vm,
    KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
      KVM_REG_ARM_CORE_REG(regs.pc),
    &pc,
    "pc"
  );
  (void)ant_kvm_get_one_reg64(
    vm,
    KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
      KVM_REG_ARM_CORE_REG(regs.pstate),
    &pstate,
    "pstate"
  );
  (void)ant_kvm_get_one_reg64(
    vm,
    KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
      KVM_REG_ARM_CORE_REG(regs.regs[0]),
    &x0,
    "x0"
  );
  (void)ant_kvm_get_one_reg64(
    vm,
    KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
      KVM_REG_ARM_CORE_REG(regs.regs[1]),
    &x1,
    "x1"
  );
  (void)ant_kvm_get_one_reg64(
    vm,
    KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
      KVM_REG_ARM_CORE_REG(sp_el1),
    &sp_el1,
    "sp_el1"
  );
  (void)ant_kvm_get_one_reg64(vm, ANT_KVM_REG_SPSR_EL1, &spsr_el1, "spsr_el1");
  (void)ant_kvm_get_one_reg64(vm, ANT_KVM_REG_ELR_EL1, &elr_el1, "elr_el1");
  (void)ant_kvm_get_one_reg64(vm, ANT_KVM_REG_ESR_EL1, &esr_el1, "esr_el1");
  (void)ant_kvm_get_one_reg64(vm, ANT_KVM_REG_FAR_EL1, &far_el1, "far_el1");
  (void)ant_kvm_get_one_reg64(vm, ANT_KVM_REG_VBAR_EL1, &vbar_el1, "vbar_el1");
  (void)ant_kvm_get_one_reg64(vm, KVM_REG_ARM_TIMER_CTL, &cntv_ctl, "cntv_ctl");
  (void)ant_kvm_get_one_reg64(vm, KVM_REG_ARM_TIMER_CVAL, &cntv_cval, "cntv_cval");
  (void)ant_kvm_get_one_reg64(vm, KVM_REG_ARM_TIMER_CNT, &cntvct, "cntvct");
  (void)ant_kvm_get_one_reg64(vm, ANT_KVM_REG_ID_AA64PFR0_EL1, &id_aa64pfr0, "id_aa64pfr0_el1");
  if (vm->kas_offset_symbol != 0) {
    (void)ant_hvf_guest_read(vm, vm->kas_offset_symbol, &kas_offset, sizeof(kas_offset));
  }
  if (vm->kas_kern_offset_symbol != 0) {
    (void)ant_hvf_guest_read(vm, vm->kas_kern_offset_symbol, &kas_kern_offset, sizeof(kas_kern_offset));
  }
  if (vm->pcie_ecam_base_symbol != 0) {
    (void)ant_hvf_guest_read(vm, vm->pcie_ecam_base_symbol, &pcie_ecam_base, sizeof(pcie_ecam_base));
  }
  fprintf(
    stderr,
    "sandbox vm: %s pc=0x%llx low_pc=0x%llx kas_offset=0x%llx kas_kern_offset=0x%llx pcie_ecam_base=0x%llx pstate=0x%llx x0=0x%llx x1=0x%llx sp_el1=0x%llx spsr_el1=0x%llx elr_el1=0x%llx esr_el1=0x%llx far_el1=0x%llx vbar_el1=0x%llx cntv_ctl=0x%llx cntv_cval=0x%llx cntvct=0x%llx cntfrq=%llu id_aa64pfr0=0x%llx\n",
    reason,
    (unsigned long long)pc,
    (unsigned long long)(kas_kern_offset ? pc - kas_kern_offset : (kas_offset ? pc - kas_offset : 0)),
    (unsigned long long)kas_offset,
    (unsigned long long)kas_kern_offset,
    (unsigned long long)pcie_ecam_base,
    (unsigned long long)pstate,
    (unsigned long long)x0,
    (unsigned long long)x1,
    (unsigned long long)sp_el1,
    (unsigned long long)spsr_el1,
    (unsigned long long)elr_el1,
    (unsigned long long)esr_el1,
    (unsigned long long)far_el1,
    (unsigned long long)vbar_el1,
    (unsigned long long)cntv_ctl,
    (unsigned long long)cntv_cval,
    (unsigned long long)cntvct,
    (unsigned long long)vm->cntfrq,
    (unsigned long long)id_aa64pfr0
  );
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

static bool ant_kvm_pci_mmio_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value) {
  unsigned bus, slot, fn, reg;
  if (!ant_hvf_pci_addr(addr, &bus, &slot, &fn, &reg)) return false;
  uint32_t word = ant_hvf_pci_config_read32(vm, bus, slot, fn, reg);
  *value = ant_hvf_select_width(word, reg & 3u, size);
  return true;
}

static bool ant_kvm_pci_mmio_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value) {
  unsigned bus, slot, fn, reg;
  if (!ant_hvf_pci_addr(addr, &bus, &slot, &fn, &reg)) return false;
  ant_hvf_pci_config_write(vm, bus, slot, fn, reg, size, value);
  return true;
}

static bool ant_kvm_gic_msi_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value) {
  uint64_t off = addr - ANT_HVF_GIC_MSI_BASE;
  uint32_t word = 0;
  switch (off & ~3ull) {
    case ANT_KVM_AARCH64_GICM_TYPER:
      if (vm->gic_msi_enabled) {
        word = ((vm->gic_msi_base & 0x7ffu) << 16) | (vm->gic_msi_count & 0x7ffu);
      }
      break;
    case ANT_KVM_AARCH64_GICM_IIDR:
      word = 0x43b;
      break;
    default:
      word = 0;
      break;
  }
  *value = ant_hvf_select_width(word, (unsigned)(off & 3u), size);
  return true;
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

static int ant_kvm_raise_ppi(ant_hvf_vm_t *vm, uint32_t ppi) {
  if (ppi > KVM_ARM_IRQ_NUM_MASK) return -EINVAL;
  uint32_t irq = (KVM_ARM_IRQ_TYPE_PPI << KVM_ARM_IRQ_TYPE_SHIFT) |
                 (0u << KVM_ARM_IRQ_VCPU_SHIFT) |
                 ((ppi & KVM_ARM_IRQ_NUM_MASK) << KVM_ARM_IRQ_NUM_SHIFT);
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
  (void)addr;
  return ant_kvm_raise_spi(vm, data);
}

static bool ant_kvm_gic_msi_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value) {
  (void)size;
  uint64_t off = addr - ANT_HVF_GIC_MSI_BASE;
  if ((off & ~3ull) != ANT_KVM_AARCH64_GICM_SET_SPI_NSR) return true;
  if (!vm->gic_msi_enabled) return true;
  uint32_t intid = (uint32_t)value;
  if (intid < vm->gic_msi_base || intid >= vm->gic_msi_base + vm->gic_msi_count) return true;
  return ant_kvm_raise_spi(vm, intid) == 0;
}

enum {
  ANT_KVM_RTC_DR = 0x000,
  ANT_KVM_RTC_MR = 0x004,
  ANT_KVM_RTC_LR = 0x008,
  ANT_KVM_RTC_CR = 0x00c,
  ANT_KVM_RTC_IMSC = 0x010,
  ANT_KVM_RTC_RIS = 0x014,
  ANT_KVM_RTC_MIS = 0x018,
  ANT_KVM_RTC_ICR = 0x01c,
  ANT_KVM_RTC_PERIPH_ID0 = 0xfe0,
  ANT_KVM_RTC_PERIPH_ID1 = 0xfe4,
  ANT_KVM_RTC_PERIPH_ID2 = 0xfe8,
  ANT_KVM_RTC_PERIPH_ID3 = 0xfec,
  ANT_KVM_RTC_PCELL_ID0 = 0xff0,
  ANT_KVM_RTC_PCELL_ID1 = 0xff4,
  ANT_KVM_RTC_PCELL_ID2 = 0xff8,
  ANT_KVM_RTC_PCELL_ID3 = 0xffc,
};

static bool ant_kvm_mmio_unsupported(
  ant_hvf_vm_t *vm,
  const char *region,
  bool write,
  uint64_t addr,
  unsigned size,
  uint64_t value
) {
  if (vm && vm->verbose) {
  if (write) ant_hvf_verbosef(vm,
    "unsupported %s MMIO write addr=0x%llx size=%u value=0x%llx",
    region, (unsigned long long)addr, size, (unsigned long long)value
  );
  else ant_hvf_verbosef(vm,
    "unsupported %s MMIO read addr=0x%llx size=%u",
    region, (unsigned long long)addr, size
  );}
  return false;
}

static uint32_t ant_kvm_rtc_counter(ant_hvf_vm_t *vm) {
  time_t now = time(NULL);
  if (vm->rtc_load_host == 0) {
    vm->rtc_load_host = now;
    vm->rtc_load_value = (uint32_t)now;
    vm->rtc_enabled = true;
  }
  if (!vm->rtc_enabled) return vm->rtc_load_value;
  time_t elapsed = now - vm->rtc_load_host;
  if (elapsed <= 0) return vm->rtc_load_value;
  return vm->rtc_load_value + (uint32_t)elapsed;
}

static uint32_t ant_kvm_rtc_raw_interrupt(ant_hvf_vm_t *vm) {
  uint32_t counter = ant_kvm_rtc_counter(vm);
  return (vm->rtc_match != 0 && counter >= vm->rtc_match) ? 1u : 0u;
}

static bool ant_kvm_rtc_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value) {
  uint64_t off = addr - ANT_HVF_RTC_BASE;
  uint32_t word = 0;
  switch (off & ~3ull) {
    case ANT_KVM_RTC_DR: word = ant_kvm_rtc_counter(vm); break;
    case ANT_KVM_RTC_MR: word = vm->rtc_match; break;
    case ANT_KVM_RTC_LR: word = vm->rtc_load_value; break;
    case ANT_KVM_RTC_CR: word = vm->rtc_enabled ? 1u : 0u; break;
    case ANT_KVM_RTC_IMSC: word = vm->rtc_imsc & 1u; break;
    case ANT_KVM_RTC_RIS: word = ant_kvm_rtc_raw_interrupt(vm); break;
    case ANT_KVM_RTC_MIS: word = ant_kvm_rtc_raw_interrupt(vm) & vm->rtc_imsc; break;
    case ANT_KVM_RTC_ICR: word = vm->rtc_icr & 1u; break;
    case ANT_KVM_RTC_PERIPH_ID0: word = 0x31; break;
    case ANT_KVM_RTC_PERIPH_ID1: word = 0x10; break;
    case ANT_KVM_RTC_PERIPH_ID2: word = 0x04; break;
    case ANT_KVM_RTC_PERIPH_ID3: word = 0x00; break;
    case ANT_KVM_RTC_PCELL_ID0: word = 0x0d; break;
    case ANT_KVM_RTC_PCELL_ID1: word = 0xf0; break;
    case ANT_KVM_RTC_PCELL_ID2: word = 0x05; break;
    case ANT_KVM_RTC_PCELL_ID3: word = 0xb1; break;
    default: return ant_kvm_mmio_unsupported(vm, "RTC", false, addr, size, 0);
  }
  *value = ant_hvf_select_width(word, (unsigned)(off & 3u), size);
  return true;
}

static bool ant_kvm_rtc_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value) {
  uint64_t off = addr - ANT_HVF_RTC_BASE;
  if (size != 4 || (off & 3u) != 0) return ant_kvm_mmio_unsupported(vm, "RTC", true, addr, size, value);
  switch (off) {
    case ANT_KVM_RTC_MR:
      vm->rtc_match = (uint32_t)value;
      return true;
    case ANT_KVM_RTC_LR:
      vm->rtc_load_value = (uint32_t)value;
      vm->rtc_load_host = time(NULL);
      return true;
    case ANT_KVM_RTC_CR: {
      bool enable = (value & 1u) != 0;
      if (enable != vm->rtc_enabled) {
        vm->rtc_load_value = ant_kvm_rtc_counter(vm);
        vm->rtc_load_host = time(NULL);
        vm->rtc_enabled = enable;
      }
      return true;
    }
    case ANT_KVM_RTC_IMSC:
      vm->rtc_imsc = (uint32_t)value & 1u;
      return true;
    case ANT_KVM_RTC_ICR:
      vm->rtc_icr = (uint32_t)value & 1u;
      return true;
    default:
      return ant_kvm_mmio_unsupported(vm, "RTC", true, addr, size, value);
  }
}

static bool ant_kvm_mmio_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value) {
  if (addr == ANT_HVF_UART_BASE + 0x18) {
    *value = 0;
    return true;
  }
  if (addr >= ANT_HVF_GIC_MSI_BASE && addr < ANT_HVF_GIC_MSI_BASE + ANT_HVF_GIC_MSI_SIZE) {
    return ant_kvm_gic_msi_read(vm, addr, size, value);
  }
  if (addr >= ANT_HVF_RTC_BASE && addr < ANT_HVF_RTC_BASE + 0x1000) {
    return ant_kvm_rtc_read(vm, addr, size, value);
  }
  if (addr >= ANT_HVF_PCIE_ECAM_BASE && addr < ANT_HVF_PCIE_ECAM_BASE + ANT_HVF_PCIE_ECAM_SIZE) {
    return ant_kvm_pci_mmio_read(vm, addr, size, value);
  }
  ant_hvf_virtio_device_t *virtio = ant_hvf_virtio_for_bar(vm, addr);
  if (virtio) return ant_hvf_virtio_common_read(vm, virtio, addr - virtio->bar0, size, value);
  if (addr >= ANT_HVF_PCIE_PIO_BASE && addr < ANT_HVF_PCIE_PIO_BASE + 0x10000) {
    return ant_kvm_mmio_unsupported(vm, "PCI PIO", false, addr, size, 0);
  }
  return ant_kvm_mmio_unsupported(vm, "unknown", false, addr, size, 0);
}

static bool ant_kvm_mmio_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value) {
  if (addr == ANT_HVF_UART_BASE) {
    ant_hvf_uart_put(vm, (uint8_t)(value & 0xff));
    return true;
  }
  if (addr >= ANT_HVF_GIC_MSI_BASE && addr < ANT_HVF_GIC_MSI_BASE + ANT_HVF_GIC_MSI_SIZE) {
    return ant_kvm_gic_msi_write(vm, addr, size, value);
  }
  if (addr >= ANT_HVF_RTC_BASE && addr < ANT_HVF_RTC_BASE + 0x1000) {
    return ant_kvm_rtc_write(vm, addr, size, value);
  }
  if (addr >= ANT_HVF_PCIE_ECAM_BASE && addr < ANT_HVF_PCIE_ECAM_BASE + ANT_HVF_PCIE_ECAM_SIZE) {
    return ant_kvm_pci_mmio_write(vm, addr, size, value);
  }
  ant_hvf_virtio_device_t *virtio = ant_hvf_virtio_for_bar(vm, addr);
  if (virtio) return ant_hvf_virtio_common_write(vm, virtio, addr - virtio->bar0, size, value);
  if (addr >= ANT_HVF_PCIE_PIO_BASE && addr < ANT_HVF_PCIE_PIO_BASE + 0x10000) {
    return ant_kvm_mmio_unsupported(vm, "PCI PIO", true, addr, size, value);
  }
  return ant_kvm_mmio_unsupported(vm, "unknown", true, addr, size, value);
}

static int ant_kvm_handle_mmio(ant_hvf_vm_t *vm) {
  struct kvm_run *run = vm->run;
  uint64_t value = 0;
  if (run->mmio.len > sizeof(value)) return -EINVAL;
  if (run->mmio.is_write) {
    memcpy(&value, run->mmio.data, run->mmio.len);
    return ant_kvm_mmio_write(vm, run->mmio.phys_addr, run->mmio.len, value) ? 0 : -ENOSYS;
  }
  if (!ant_kvm_mmio_read(vm, run->mmio.phys_addr, run->mmio.len, &value)) return -ENOSYS;
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
      ant_kvm_report_aarch64_regs(vm, "guest timed out");
      rc = -ETIMEDOUT;
      break;
    }
    if (timeout_until_request_sent &&
        atomic_load_explicit(&vm->vsock.request_sent, memory_order_acquire)) {
      atomic_store_explicit(&vm->timeout_disarmed, true, memory_order_release);
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
          ant_kvm_report_aarch64_regs(vm, "guest timed out");
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
        rc = ant_kvm_handle_mmio(vm);
        break;
      case KVM_EXIT_SYSTEM_EVENT:
        if (vm->run->system_event.type == KVM_SYSTEM_EVENT_SHUTDOWN) {
          rc = 0;
          goto done;
        }
        fprintf(stderr, "sandbox vm: KVM system event type=%u\n", vm->run->system_event.type);
        rc = -EIO;
        goto done;
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
  if (vm->gic_fd >= 0) close(vm->gic_fd);
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
  vm->gic_fd = -1;
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
  vm->cntfrq = ant_kvm_host_cntfrq();
  
  if (vm->cntfrq == 0 || vm->cntfrq > UINT32_MAX) {
    rc = -EINVAL;
    goto fail;
  }

  rc = ant_kvm_init_devices(vm, config);
  if (rc != 0) goto fail;

  ant_hvf_verbosef(vm,
    "KVM aarch64 backend image=%s kernel=%s memory=%zu MiB mounts=%zu forwards=%zu",
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
  if (vm->image_fd < 0) { rc = -errno; goto fail; }

  vm->host_mem = mmap(NULL, vm->mem_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (vm->host_mem == MAP_FAILED) { rc = -errno; goto fail; }

  vm->kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (vm->kvm_fd < 0) { rc = -errno; goto fail; }
  int api = ioctl(vm->kvm_fd, KVM_GET_API_VERSION, 0);
  if (api != KVM_API_VERSION) { rc = -ENOSYS; goto fail; }
  vm->vm_fd = ioctl(vm->kvm_fd, KVM_CREATE_VM, 0);
  if (vm->vm_fd < 0) { rc = -errno; goto fail; }

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
  (void)ant_kvm_find_symbol(config->kernel_path, "kernel_phys_offset", &vm->kas_offset_symbol);
  (void)ant_kvm_find_symbol(config->kernel_path, "kas_kern_offset", &vm->kas_kern_offset_symbol);
  (void)ant_kvm_find_symbol(config->kernel_path, "pcie_ecam_base", &vm->pcie_ecam_base_symbol);

  vm->vcpu_fd = ioctl(vm->vm_fd, KVM_CREATE_VCPU, 0);
  if (vm->vcpu_fd < 0) { rc = -errno; goto fail; }
  int mmap_size = ioctl(vm->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
  if (mmap_size <= 0) { rc = -EIO; goto fail; }
  vm->run_mmap_size = (size_t)mmap_size;
  vm->run = mmap(NULL, vm->run_mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vm->vcpu_fd, 0);
  if (vm->run == MAP_FAILED) { rc = -errno; goto fail; }

  rc = ant_kvm_create_gic(vm);
  if (rc != 0) goto fail;
  session->irqchip_created = true;
  rc = ant_kvm_build_dtb(vm);
  if (rc != 0) goto fail;
  rc = ant_kvm_init_vcpu(vm);
  if (rc != 0) goto fail;
  rc = ant_kvm_init_vcpu_timer(vm);
  if (rc != 0) goto fail;
  rc = ant_kvm_finalize_gic(vm);
  if (rc != 0) goto fail;

  ant_kvm_install_wakeup_signal();
  ant_hvf_verbosef(vm,
    "loaded sandbox kernel (%lld bytes) entry=0x%llx cntfrq=%llu gic=v%d",
    (long long)kernel_size,
    (unsigned long long)vm->kernel_entry,
    (unsigned long long)vm->cntfrq,
    vm->gic_version
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
  else
    ant_hvf_verbose(vm, "running guest timeout=disabled");

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
