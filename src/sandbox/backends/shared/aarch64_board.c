#include "shared/backend.h" // IWYU pragma: keep

#if defined(__aarch64__)

uint64_t ant_hvf_host_cntfrq(void) {
  uint64_t cntfrq = 0;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
  return cntfrq;
}

int ant_fdt_init(ant_fdt_t *fdt) {
  if (!fdt) return -EINVAL;
  memset(fdt, 0, sizeof(*fdt));
  fdt->structure_cap = ANT_FDT_STRUCTURE_BYTES;
  fdt->strings_cap = ANT_FDT_STRINGS_BYTES;
  fdt->structure = calloc(1, fdt->structure_cap);
  fdt->strings = calloc(1, fdt->strings_cap);
  if (!fdt->structure || !fdt->strings) {
    ant_fdt_free(fdt);
    return -ENOMEM;
  }
  return 0;
}

void ant_fdt_free(ant_fdt_t *fdt) {
  if (!fdt) return;
  free(fdt->structure);
  free(fdt->strings);
  memset(fdt, 0, sizeof(*fdt));
}

static int ant_fdt_grow(unsigned char **buf, size_t *cap, size_t used, size_t need) {
  if (need <= *cap - used) return 0;
  size_t new_cap = *cap ? *cap : 256u;
  while (need > new_cap - used) {
    if (new_cap >= ANT_HVF_DTB_MAX) return -ENOSPC;
    size_t next = new_cap * 2u;
    new_cap = next > ANT_HVF_DTB_MAX ? ANT_HVF_DTB_MAX : next;
  }
  unsigned char *new_buf = realloc(*buf, new_cap);
  if (!new_buf) return -ENOMEM;
  memset(new_buf + *cap, 0, new_cap - *cap);
  *buf = new_buf;
  *cap = new_cap;
  return 0;
}

int ant_fdt_reserve(ant_fdt_t *fdt, size_t len, unsigned char **out) {
  size_t aligned = ant_align4(len);
  int rc = ant_fdt_grow(&fdt->structure, &fdt->structure_cap, fdt->structure_len, aligned);
  if (rc != 0) return rc;
  *out = fdt->structure + fdt->structure_len;
  memset(*out, 0, aligned);
  fdt->structure_len += aligned;
  return 0;
}

int ant_fdt_u32(ant_fdt_t *fdt, uint32_t val) {
  unsigned char *out;
  int rc = ant_fdt_reserve(fdt, sizeof(uint32_t), &out);
  if (rc != 0) return rc;
  *(uint32_t *)out = ant_bswap32(val);
  return 0;
}

int ant_fdt_string_offset(ant_fdt_t *fdt, const char *name) {
  size_t len = strlen(name) + 1;
  int rc = ant_fdt_grow(&fdt->strings, &fdt->strings_cap, fdt->strings_len, len);
  if (rc != 0) return rc;
  int off = (int)fdt->strings_len;
  memcpy(fdt->strings + fdt->strings_len, name, len);
  fdt->strings_len += len;
  return off;
}

int ant_fdt_begin(ant_fdt_t *fdt, const char *name) {
  int rc = ant_fdt_u32(fdt, FDT_BEGIN_NODE);
  if (rc != 0) return rc;
  unsigned char *out;
  size_t len = strlen(name) + 1;
  rc = ant_fdt_reserve(fdt, len, &out);
  if (rc != 0) return rc;
  memcpy(out, name, len);
  return 0;
}

int ant_fdt_end(ant_fdt_t *fdt) {
  return ant_fdt_u32(fdt, FDT_END_NODE);
}

int ant_fdt_prop(ant_fdt_t *fdt, const char *name, const void *data, size_t len) {
  int nameoff = ant_fdt_string_offset(fdt, name);
  if (nameoff < 0) return nameoff;
  int rc = ant_fdt_u32(fdt, FDT_PROP);
  if (rc != 0) return rc;
  rc = ant_fdt_u32(fdt, (uint32_t)len);
  if (rc != 0) return rc;
  rc = ant_fdt_u32(fdt, (uint32_t)nameoff);
  if (rc != 0) return rc;
  unsigned char *out;
  rc = ant_fdt_reserve(fdt, len, &out);
  if (rc != 0) return rc;
  memcpy(out, data, len);
  return 0;
}

int ant_fdt_prop_null(ant_fdt_t *fdt, const char *name) {
  return ant_fdt_prop(fdt, name, "", 0);
}

int ant_fdt_prop_string(ant_fdt_t *fdt, const char *name, const char *value) {
  return ant_fdt_prop(fdt, name, value, strlen(value) + 1);
}

int ant_fdt_prop_u32(ant_fdt_t *fdt, const char *name, uint32_t value) {
  uint32_t be = ant_bswap32(value);
  return ant_fdt_prop(fdt, name, &be, sizeof(be));
}

int ant_fdt_prop_cells(ant_fdt_t *fdt, const char *name, const uint32_t *cells, size_t count) {
  uint32_t be[ANT_FDT_MAX_PROP_CELLS_32];
  if (count > sizeof(be) / sizeof(be[0])) return -EINVAL;
  for (size_t i = 0; i < count; i++) be[i] = ant_bswap32(cells[i]);
  return ant_fdt_prop(fdt, name, be, count * sizeof(be[0]));
}

int ant_fdt_prop_reg64(ant_fdt_t *fdt, const char *name, const uint64_t *cells, size_t count) {
  uint64_t be[ANT_FDT_MAX_PROP_CELLS_64];
  if (count > sizeof(be) / sizeof(be[0])) return -EINVAL;
  for (size_t i = 0; i < count; i++) be[i] = ant_bswap64(cells[i]);
  return ant_fdt_prop(fdt, name, be, count * sizeof(be[0]));
}

int ant_hvf_build_dtb(ant_hvf_vm_t *vm, int gic_version) {
  ant_fdt_t fdt;
  int rc = ant_fdt_init(&fdt);
  if (rc != 0) goto out;

  rc = ant_fdt_begin(&fdt, "");
  if (rc != 0) goto out;
  if ((rc = ant_fdt_prop_string(&fdt, "compatible", "linux,dummy-virt")) != 0) goto out;
  if ((rc = ant_fdt_prop_u32(&fdt, "#address-cells", 2)) != 0) goto out;
  if ((rc = ant_fdt_prop_u32(&fdt, "#size-cells", 2)) != 0) goto out;
  if ((rc = ant_fdt_prop_u32(&fdt, "interrupt-parent", 1)) != 0) goto out;

  if ((rc = ant_fdt_begin(&fdt, "cpus")) != 0) goto out;
  if ((rc = ant_fdt_prop_u32(&fdt, "#address-cells", 2)) != 0) goto out;
  if ((rc = ant_fdt_prop_u32(&fdt, "#size-cells", 0)) != 0) goto out;
  if ((rc = ant_fdt_begin(&fdt, "cpu@0")) != 0) goto out;
  if ((rc = ant_fdt_prop_string(&fdt, "device_type", "cpu")) != 0) goto out;
  if ((rc = ant_fdt_prop_string(&fdt, "compatible", "arm,arm-v8")) != 0) goto out;
  uint64_t cpu_reg[] = { 0 };
  if ((rc = ant_fdt_prop_reg64(&fdt, "reg", cpu_reg, 1)) != 0) goto out;
  if ((rc = ant_fdt_end(&fdt)) != 0) goto out;
  if ((rc = ant_fdt_end(&fdt)) != 0) goto out;

  if ((rc = ant_fdt_begin(&fdt, "timer")) != 0) goto out;
  if ((rc = ant_fdt_prop_string(&fdt, "compatible", "arm,armv8-timer")) != 0) goto out;
  if ((rc = ant_fdt_prop_null(&fdt, "always-on")) != 0) goto out;
  if (vm->cntfrq == 0 || vm->cntfrq > UINT32_MAX) { rc = -EINVAL; goto out; }
  if ((rc = ant_fdt_prop_u32(&fdt, "clock-frequency", (uint32_t)vm->cntfrq)) != 0) goto out;
  if ((rc = ant_fdt_prop_u32(&fdt, "timebase-frequency", (uint32_t)vm->cntfrq)) != 0) goto out;
  uint32_t timer_irq[] = {
    1, 13, 4,
    1, 14, 4,
    1, 11, 4,
    1, 10, 4,
  };
  if ((rc = ant_fdt_prop_cells(&fdt, "interrupts", timer_irq, sizeof(timer_irq) / sizeof(timer_irq[0]))) != 0) goto out;
  if ((rc = ant_fdt_end(&fdt)) != 0) goto out;

  if ((rc = ant_fdt_begin(&fdt, "memory@40000000")) != 0) goto out;
  if ((rc = ant_fdt_prop_string(&fdt, "device_type", "memory")) != 0) goto out;
  uint64_t mem_reg[] = { ANT_HVF_GUEST_BASE, vm->mem_size };
  if ((rc = ant_fdt_prop_reg64(&fdt, "reg", mem_reg, 2)) != 0) goto out;
  if ((rc = ant_fdt_end(&fdt)) != 0) goto out;

  if ((rc = ant_fdt_begin(&fdt, "chosen")) != 0) goto out;
  if ((rc = ant_fdt_prop_string(&fdt, "bootargs", "")) != 0) goto out;
  if ((rc = ant_fdt_prop_string(&fdt, "stdout-path", "/uart@9000000")) != 0) goto out;
  if ((rc = ant_fdt_end(&fdt)) != 0) goto out;

  if ((rc = ant_fdt_begin(&fdt, "intc")) != 0) goto out;
  if ((rc = ant_fdt_prop_string(&fdt, "compatible", gic_version == 2 ? "arm,cortex-a15-gic" : "arm,gic-v3")) != 0) goto out;
  if ((rc = ant_fdt_prop_null(&fdt, "interrupt-controller")) != 0) goto out;
  if ((rc = ant_fdt_prop_u32(&fdt, "#interrupt-cells", 3)) != 0) goto out;
  if ((rc = ant_fdt_prop_u32(&fdt, "phandle", 1)) != 0) goto out;
  uint64_t gic_reg[] = {
    ANT_HVF_GIC_DIST_BASE, gic_version == 2 ? ANT_HVF_GIC_V2_DIST_SIZE : ANT_HVF_GIC_DIST_SIZE,
    gic_version == 2 ? ANT_HVF_GIC_CPU_BASE : ANT_HVF_GIC_REDIST_BASE,
    gic_version == 2 ? ANT_HVF_GIC_CPU_SIZE : ANT_HVF_GIC_REDIST_SIZE,
  };
  if ((rc = ant_fdt_prop_reg64(&fdt, "reg", gic_reg, 4)) != 0) goto out;
  if ((rc = ant_fdt_end(&fdt)) != 0) goto out;

  if ((rc = ant_fdt_begin(&fdt, "uart@9000000")) != 0) goto out;
  if ((rc = ant_fdt_prop_string(&fdt, "compatible", "arm,pl011")) != 0) goto out;
  uint64_t uart_reg[] = { ANT_HVF_UART_BASE, 0x1000 };
  if ((rc = ant_fdt_prop_reg64(&fdt, "reg", uart_reg, 2)) != 0) goto out;
  uint32_t uart_irq[] = { 0, 1, 4 };
  if ((rc = ant_fdt_prop_cells(&fdt, "interrupts", uart_irq, 3)) != 0) goto out;
  if ((rc = ant_fdt_end(&fdt)) != 0) goto out;

  if ((rc = ant_fdt_begin(&fdt, "pcie@3f000000")) != 0) goto out;
  if ((rc = ant_fdt_prop_string(&fdt, "compatible", "pci-host-ecam-generic")) != 0) goto out;
  if ((rc = ant_fdt_prop_string(&fdt, "device_type", "pci")) != 0) goto out;
  if ((rc = ant_fdt_prop_u32(&fdt, "#address-cells", 3)) != 0) goto out;
  if ((rc = ant_fdt_prop_u32(&fdt, "#size-cells", 2)) != 0) goto out;
  uint64_t pcie_reg[] = { ANT_HVF_PCIE_ECAM_BASE, ANT_HVF_PCIE_ECAM_SIZE };
  if ((rc = ant_fdt_prop_reg64(&fdt, "reg", pcie_reg, 2)) != 0) goto out;
  uint32_t bus_range[] = { 0, 0 };
  if ((rc = ant_fdt_prop_cells(&fdt, "bus-range", bus_range, 2)) != 0) goto out;
  if ((rc = ant_fdt_end(&fdt)) != 0) goto out;

  if ((rc = ant_fdt_end(&fdt)) != 0) goto out;
  if ((rc = ant_fdt_u32(&fdt, FDT_END)) != 0) goto out;

  size_t off_mem = sizeof(ant_fdt_header_t);
  size_t off_struct = ant_align4(off_mem + 16);
  size_t off_strings = off_struct + fdt.structure_len;
  size_t total = off_strings + fdt.strings_len;
  if (total > ANT_HVF_DTB_MAX) { rc = -ENOSPC; goto out; }

  unsigned char *dtb = ant_hvf_guest_ptr(vm, ANT_HVF_DTB_BASE, ANT_HVF_DTB_MAX);
  if (!dtb) { rc = -EINVAL; goto out; }
  memset(dtb, 0, ANT_HVF_DTB_MAX);

  ant_fdt_header_t hdr = {
    .magic = ant_bswap32(FDT_MAGIC),
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
  ant_fdt_free(&fdt);
  return rc;
}

bool ant_hvf_gic_msi_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value) {
  uint64_t off = addr - ANT_HVF_GIC_MSI_BASE;
  uint32_t word = 0;

  switch (off & ~3ull) {
    case ANT_HVF_GICM_TYPER:
      if (vm->gic_msi_enabled) {
        word = ((vm->gic_msi_base & 0x7ffu) << 16) | (vm->gic_msi_count & 0x7ffu);
      }
      break;
    case ANT_HVF_GICM_IIDR:
      word = 0x43b;
      break;
    default:
      word = 0;
      break;
  }

  *value = ant_hvf_select_width(word, (unsigned)(off & 3u), size);
  return true;
}

bool ant_hvf_gic_msi_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value) {
  (void)size;
  uint64_t off = addr - ANT_HVF_GIC_MSI_BASE;
  if ((off & ~3ull) != ANT_HVF_GICM_SET_SPI_NSR) return true;
  if (!vm->gic_msi_enabled) return true;

  uint32_t intid = (uint32_t)value;
  if (intid < vm->gic_msi_base || intid >= vm->gic_msi_base + vm->gic_msi_count) return true;

  return ant_hvf_send_msi(vm, ANT_HVF_GIC_MSI_BASE + ANT_HVF_GICM_SET_SPI_NSR, intid) == 0;
}

enum {
  ANT_HVF_RTC_DR = 0x000,
  ANT_HVF_RTC_MR = 0x004,
  ANT_HVF_RTC_LR = 0x008,
  ANT_HVF_RTC_CR = 0x00c,
  ANT_HVF_RTC_IMSC = 0x010,
  ANT_HVF_RTC_RIS = 0x014,
  ANT_HVF_RTC_MIS = 0x018,
  ANT_HVF_RTC_ICR = 0x01c,
  ANT_HVF_RTC_PERIPH_ID0 = 0xfe0,
  ANT_HVF_RTC_PERIPH_ID1 = 0xfe4,
  ANT_HVF_RTC_PERIPH_ID2 = 0xfe8,
  ANT_HVF_RTC_PERIPH_ID3 = 0xfec,
  ANT_HVF_RTC_PCELL_ID0 = 0xff0,
  ANT_HVF_RTC_PCELL_ID1 = 0xff4,
  ANT_HVF_RTC_PCELL_ID2 = 0xff8,
  ANT_HVF_RTC_PCELL_ID3 = 0xffc,
};

static bool ant_hvf_mmio_unsupported(
  ant_hvf_vm_t *vm,
  const char *region,
  bool write,
  uint64_t addr,
  unsigned size,
  uint64_t value
) {
  if (write) fprintf(
    stderr,
    "sandbox vm: unsupported %s MMIO write addr=0x%llx size=%u value=0x%llx\n",
    region,
    (unsigned long long)addr,
    size,
    (unsigned long long)value
  );
  else fprintf(stderr,
    "sandbox vm: unsupported %s MMIO read addr=0x%llx size=%u\n",
    region,
    (unsigned long long)addr,
    size
  );
  return false;
}

static uint32_t ant_hvf_rtc_counter(ant_hvf_vm_t *vm) {
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

static uint32_t ant_hvf_rtc_raw_interrupt(ant_hvf_vm_t *vm) {
  uint32_t counter = ant_hvf_rtc_counter(vm);
  return (vm->rtc_match != 0 && counter >= vm->rtc_match) ? 1u : 0u;
}

static bool ant_hvf_rtc_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value) {
  uint64_t off = addr - ANT_HVF_RTC_BASE;
  uint32_t word = 0;

  switch (off & ~3ull) {
    case ANT_HVF_RTC_DR:
      word = ant_hvf_rtc_counter(vm);
      break;
    case ANT_HVF_RTC_MR:
      word = vm->rtc_match;
      break;
    case ANT_HVF_RTC_LR:
      word = vm->rtc_load_value;
      break;
    case ANT_HVF_RTC_CR:
      word = vm->rtc_enabled ? 1u : 0u;
      break;
    case ANT_HVF_RTC_IMSC:
      word = vm->rtc_imsc & 1u;
      break;
    case ANT_HVF_RTC_RIS:
      word = ant_hvf_rtc_raw_interrupt(vm);
      break;
    case ANT_HVF_RTC_MIS:
      word = ant_hvf_rtc_raw_interrupt(vm) & vm->rtc_imsc;
      break;
    case ANT_HVF_RTC_ICR:
      word = vm->rtc_icr & 1u;
      break;
    case ANT_HVF_RTC_PERIPH_ID0:
      word = 0x31;
      break;
    case ANT_HVF_RTC_PERIPH_ID1:
      word = 0x10;
      break;
    case ANT_HVF_RTC_PERIPH_ID2:
      word = 0x04;
      break;
    case ANT_HVF_RTC_PERIPH_ID3:
      word = 0x00;
      break;
    case ANT_HVF_RTC_PCELL_ID0:
      word = 0x0d;
      break;
    case ANT_HVF_RTC_PCELL_ID1:
      word = 0xf0;
      break;
    case ANT_HVF_RTC_PCELL_ID2:
      word = 0x05;
      break;
    case ANT_HVF_RTC_PCELL_ID3:
      word = 0xb1;
      break;
    default:
      return ant_hvf_mmio_unsupported(vm, "RTC", false, addr, size, 0);
  }

  *value = ant_hvf_select_width(word, (unsigned)(off & 3u), size);
  return true;
}

static bool ant_hvf_rtc_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value) {
  uint64_t off = addr - ANT_HVF_RTC_BASE;
  if (size != 4 || (off & 3u) != 0) {
    return ant_hvf_mmio_unsupported(vm, "RTC", true, addr, size, value);
  }

  switch (off) {
    case ANT_HVF_RTC_MR:
      vm->rtc_match = (uint32_t)value;
      return true;
    case ANT_HVF_RTC_LR:
      vm->rtc_load_value = (uint32_t)value;
      vm->rtc_load_host = time(NULL);
      return true;
    case ANT_HVF_RTC_CR: {
      bool enable = (value & 1u) != 0;
      if (enable != vm->rtc_enabled) {
        vm->rtc_load_value = ant_hvf_rtc_counter(vm);
        vm->rtc_load_host = time(NULL);
        vm->rtc_enabled = enable;
      }
      return true;
    }
    case ANT_HVF_RTC_IMSC:
      vm->rtc_imsc = (uint32_t)value & 1u;
      return true;
    case ANT_HVF_RTC_ICR:
      vm->rtc_icr = (uint32_t)value & 1u;
      return true;
    default:
      return ant_hvf_mmio_unsupported(vm, "RTC", true, addr, size, value);
  }
}

bool ant_hvf_mmio_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value) {
  if (addr == ANT_HVF_UART_BASE + 0x18) {
    *value = 0;
    return true;
  }
  if (addr >= ANT_HVF_GIC_MSI_BASE && addr < ANT_HVF_GIC_MSI_BASE + ANT_HVF_GIC_MSI_SIZE)
    return ant_hvf_gic_msi_read(vm, addr, size, value);
  if (addr >= ANT_HVF_RTC_BASE && addr < ANT_HVF_RTC_BASE + 0x1000)
    return ant_hvf_rtc_read(vm, addr, size, value);
  if (addr >= ANT_HVF_PCIE_ECAM_BASE && addr < ANT_HVF_PCIE_ECAM_BASE + ANT_HVF_PCIE_ECAM_SIZE)
    return ant_hvf_pci_mmio_read(vm, addr, size, value);
  ant_hvf_virtio_device_t *virtio = ant_hvf_virtio_for_bar(vm, addr);
  if (virtio)
    return ant_hvf_virtio_common_read(vm, virtio, addr - ant_hvf_virtio_bar0(virtio), size, value);
  if (addr >= ANT_HVF_PCIE_PIO_BASE && addr < ANT_HVF_PCIE_PIO_BASE + 0x10000)
    return ant_hvf_mmio_unsupported(vm, "PCI PIO", false, addr, size, 0);
  return ant_hvf_mmio_unsupported(vm, "unknown", false, addr, size, 0);
}

bool ant_hvf_mmio_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value) {
  if (addr == ANT_HVF_UART_BASE) {
    ant_hvf_uart_put(vm, (uint8_t)(value & 0xff));
    return true;
  }
  if (addr >= ANT_HVF_GIC_MSI_BASE && addr < ANT_HVF_GIC_MSI_BASE + ANT_HVF_GIC_MSI_SIZE)
    return ant_hvf_gic_msi_write(vm, addr, size, value);
  if (addr >= ANT_HVF_RTC_BASE && addr < ANT_HVF_RTC_BASE + 0x1000)
    return ant_hvf_rtc_write(vm, addr, size, value);
  if (addr >= ANT_HVF_PCIE_ECAM_BASE && addr < ANT_HVF_PCIE_ECAM_BASE + ANT_HVF_PCIE_ECAM_SIZE)
    return ant_hvf_pci_mmio_write(vm, addr, size, value);
  ant_hvf_virtio_device_t *virtio = ant_hvf_virtio_for_bar(vm, addr);
  if (virtio)
    return ant_hvf_virtio_common_write(vm, virtio, addr - ant_hvf_virtio_bar0(virtio), size, value);
  if (addr >= ANT_HVF_PCIE_PIO_BASE && addr < ANT_HVF_PCIE_PIO_BASE + 0x10000)
    return ant_hvf_mmio_unsupported(vm, "PCI PIO", true, addr, size, value);
  return ant_hvf_mmio_unsupported(vm, "unknown", true, addr, size, value);
}

#endif
