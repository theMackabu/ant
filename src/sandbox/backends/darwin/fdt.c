#include "backend.h"

#if defined(__aarch64__)

int ant_fdt_reserve(ant_fdt_t *fdt, size_t len, unsigned char **out) {
  size_t aligned = ant_align4(len);
  if (aligned > sizeof(fdt->structure) - fdt->structure_len) return -ENOSPC;
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
  if (len > sizeof(fdt->strings) - fdt->strings_len) return -ENOSPC;
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

int ant_hvf_build_dtb(ant_hvf_vm_t *vm) {
  ant_fdt_t fdt;
  memset(&fdt, 0, sizeof(fdt));

  int rc = ant_fdt_begin(&fdt, "");
  if (rc != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "compatible", "linux,dummy-virt")) != 0) return rc;
  if ((rc = ant_fdt_prop_u32(&fdt, "#address-cells", 2)) != 0) return rc;
  if ((rc = ant_fdt_prop_u32(&fdt, "#size-cells", 2)) != 0) return rc;
  if ((rc = ant_fdt_prop_u32(&fdt, "interrupt-parent", 1)) != 0) return rc;

  if ((rc = ant_fdt_begin(&fdt, "cpus")) != 0) return rc;
  if ((rc = ant_fdt_prop_u32(&fdt, "#address-cells", 2)) != 0) return rc;
  if ((rc = ant_fdt_prop_u32(&fdt, "#size-cells", 0)) != 0) return rc;
  if ((rc = ant_fdt_begin(&fdt, "cpu@0")) != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "device_type", "cpu")) != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "compatible", "arm,arm-v8")) != 0) return rc;
  uint64_t cpu_reg[] = { 0 };
  if ((rc = ant_fdt_prop_reg64(&fdt, "reg", cpu_reg, 1)) != 0) return rc;
  if ((rc = ant_fdt_end(&fdt)) != 0) return rc;
  if ((rc = ant_fdt_end(&fdt)) != 0) return rc;

  if ((rc = ant_fdt_begin(&fdt, "timer")) != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "compatible", "arm,armv8-timer")) != 0) return rc;
  if ((rc = ant_fdt_prop_null(&fdt, "always-on")) != 0) return rc;
  if (vm->cntfrq == 0 || vm->cntfrq > UINT32_MAX) return -EINVAL;
  if ((rc = ant_fdt_prop_u32(&fdt, "clock-frequency", (uint32_t)vm->cntfrq)) != 0) return rc;
  if ((rc = ant_fdt_prop_u32(&fdt, "timebase-frequency", (uint32_t)vm->cntfrq)) != 0) return rc;
  uint32_t timer_irq[] = {
    1, 13, 4,
    1, 14, 4,
    1, 11, 4,
    1, 10, 4,
  };
  if ((rc = ant_fdt_prop_cells(&fdt, "interrupts", timer_irq, sizeof(timer_irq) / sizeof(timer_irq[0]))) != 0) return rc;
  if ((rc = ant_fdt_end(&fdt)) != 0) return rc;

  if ((rc = ant_fdt_begin(&fdt, "memory@40000000")) != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "device_type", "memory")) != 0) return rc;
  uint64_t mem_reg[] = { ANT_HVF_GUEST_BASE, vm->mem_size };
  if ((rc = ant_fdt_prop_reg64(&fdt, "reg", mem_reg, 2)) != 0) return rc;
  if ((rc = ant_fdt_end(&fdt)) != 0) return rc;

  if ((rc = ant_fdt_begin(&fdt, "chosen")) != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "bootargs", "")) != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "stdout-path", "/uart@9000000")) != 0) return rc;
  if ((rc = ant_fdt_end(&fdt)) != 0) return rc;

  if ((rc = ant_fdt_begin(&fdt, "intc")) != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "compatible", "arm,gic-v3")) != 0) return rc;
  if ((rc = ant_fdt_prop_null(&fdt, "interrupt-controller")) != 0) return rc;
  if ((rc = ant_fdt_prop_u32(&fdt, "#interrupt-cells", 3)) != 0) return rc;
  if ((rc = ant_fdt_prop_u32(&fdt, "phandle", 1)) != 0) return rc;
  uint64_t gic_reg[] = { ANT_HVF_GIC_DIST_BASE, 0x10000, ANT_HVF_GIC_REDIST_BASE, 0x200000 };
  if ((rc = ant_fdt_prop_reg64(&fdt, "reg", gic_reg, 4)) != 0) return rc;
  if ((rc = ant_fdt_end(&fdt)) != 0) return rc;

  if ((rc = ant_fdt_begin(&fdt, "uart@9000000")) != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "compatible", "arm,pl011")) != 0) return rc;
  uint64_t uart_reg[] = { ANT_HVF_UART_BASE, 0x1000 };
  if ((rc = ant_fdt_prop_reg64(&fdt, "reg", uart_reg, 2)) != 0) return rc;
  uint32_t uart_irq[] = { 0, 1, 4 };
  if ((rc = ant_fdt_prop_cells(&fdt, "interrupts", uart_irq, 3)) != 0) return rc;
  if ((rc = ant_fdt_end(&fdt)) != 0) return rc;

  if ((rc = ant_fdt_begin(&fdt, "pcie@3f000000")) != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "compatible", "pci-host-ecam-generic")) != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "device_type", "pci")) != 0) return rc;
  if ((rc = ant_fdt_prop_u32(&fdt, "#address-cells", 3)) != 0) return rc;
  if ((rc = ant_fdt_prop_u32(&fdt, "#size-cells", 2)) != 0) return rc;
  uint64_t pcie_reg[] = { ANT_HVF_PCIE_ECAM_BASE, ANT_HVF_PCIE_ECAM_SIZE };
  if ((rc = ant_fdt_prop_reg64(&fdt, "reg", pcie_reg, 2)) != 0) return rc;
  uint32_t bus_range[] = { 0, 0 };
  if ((rc = ant_fdt_prop_cells(&fdt, "bus-range", bus_range, 2)) != 0) return rc;
  if ((rc = ant_fdt_end(&fdt)) != 0) return rc;

  if ((rc = ant_fdt_end(&fdt)) != 0) return rc;
  if ((rc = ant_fdt_u32(&fdt, FDT_END)) != 0) return rc;

  size_t off_mem = sizeof(ant_fdt_header_t);
  size_t off_struct = ant_align4(off_mem + 16);
  size_t off_strings = off_struct + fdt.structure_len;
  size_t total = off_strings + fdt.strings_len;
  if (total > ANT_HVF_DTB_MAX) return -ENOSPC;

  unsigned char *dtb = ant_hvf_guest_ptr(vm, ANT_HVF_DTB_BASE, total);
  if (!dtb) return -EINVAL;
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
  return 0;
}

#endif
