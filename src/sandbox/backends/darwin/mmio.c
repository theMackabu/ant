#include "backend.h"

#if defined(__aarch64__)

bool ant_hvf_pci_mmio_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value) {
  unsigned bus, slot, fn, reg;
  if (!ant_hvf_pci_addr(addr, &bus, &slot, &fn, &reg)) return false;
  uint32_t word = ant_hvf_pci_config_read32(vm, bus, slot, fn, reg);
  *value = ant_hvf_select_width(word, reg & 3u, size);
  return true;
}

bool ant_hvf_pci_mmio_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value) {
  unsigned bus, slot, fn, reg;
  if (!ant_hvf_pci_addr(addr, &bus, &slot, &fn, &reg)) return false;
  ant_hvf_pci_config_write(vm, bus, slot, fn, reg, size, value);
  return true;
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
  if (!vm->gic_msi_enabled || !ant_hvf_gic.send_msi) return true;

  uint32_t intid = (uint32_t)value;
  if (intid < vm->gic_msi_base || intid >= vm->gic_msi_base + vm->gic_msi_count) return true;

  hv_return_t rc = ant_hvf_gic.send_msi(ANT_HVF_GIC_MSI_BASE + ANT_HVF_GICM_SET_SPI_NSR, intid);
  if (rc != HV_SUCCESS && vm->trace) {
    fprintf(stderr,
            "sandbox vm: hv_gic_send_msi failed intid=%u rc=%d\n",
            intid,
            rc);
  }
  return true;
}

bool ant_hvf_mmio_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value) {
  if (addr == ANT_HVF_UART_BASE + 0x18) {
    *value = 0;
    return true;
  }
  if (addr >= ANT_HVF_GIC_MSI_BASE && addr < ANT_HVF_GIC_MSI_BASE + ANT_HVF_GIC_MSI_SIZE) {
    return ant_hvf_gic_msi_read(vm, addr, size, value);
  }
  if (addr >= ANT_HVF_RTC_BASE && addr < ANT_HVF_RTC_BASE + 0x1000) {
    uint64_t off = addr - ANT_HVF_RTC_BASE;
    switch (off) {
      case 0x000: *value = (uint32_t)time(NULL); return true;
      case 0xfe0: *value = 0x31; return true;
      case 0xfe4: *value = 0x10; return true;
      case 0xfe8: *value = 0x04; return true;
      case 0xfec: *value = 0x00; return true;
      case 0xff0: *value = 0x0d; return true;
      case 0xff4: *value = 0xf0; return true;
      case 0xff8: *value = 0x05; return true;
      case 0xffc: *value = 0xb1; return true;
      default: *value = 0; return true;
    }
  }
  if (addr >= ANT_HVF_PCIE_ECAM_BASE && addr < ANT_HVF_PCIE_ECAM_BASE + ANT_HVF_PCIE_ECAM_SIZE) {
    return ant_hvf_pci_mmio_read(vm, addr, size, value);
  }
  ant_hvf_virtio_device_t *virtio = ant_hvf_virtio_for_bar(vm, addr);
  if (virtio) {
    return ant_hvf_virtio_common_read(vm, virtio, addr - virtio->bar0, size, value);
  }
  if (addr >= ANT_HVF_PCIE_PIO_BASE && addr < ANT_HVF_PCIE_PIO_BASE + 0x10000) {
    *value = 0;
    return true;
  }
  return false;
}

bool ant_hvf_mmio_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value) {
  (void)size;
  if (addr == ANT_HVF_UART_BASE) {
    ant_hvf_uart_put(vm, (uint8_t)(value & 0xff));
    return true;
  }
  if (addr >= ANT_HVF_GIC_MSI_BASE && addr < ANT_HVF_GIC_MSI_BASE + ANT_HVF_GIC_MSI_SIZE) {
    return ant_hvf_gic_msi_write(vm, addr, size, value);
  }
  if (addr >= ANT_HVF_RTC_BASE && addr < ANT_HVF_RTC_BASE + 0x1000) return true;
  if (addr >= ANT_HVF_PCIE_ECAM_BASE && addr < ANT_HVF_PCIE_ECAM_BASE + ANT_HVF_PCIE_ECAM_SIZE) {
    return ant_hvf_pci_mmio_write(vm, addr, size, value);
  }
  ant_hvf_virtio_device_t *virtio = ant_hvf_virtio_for_bar(vm, addr);
  if (virtio) {
    return ant_hvf_virtio_common_write(vm, virtio, addr - virtio->bar0, size, value);
  }
  if (addr >= ANT_HVF_PCIE_PIO_BASE && addr < ANT_HVF_PCIE_PIO_BASE + 0x10000) return true;
  return false;
}

int ant_hvf_advance_pc(hv_vcpu_t vcpu) {
  uint64_t pc = 0;
  int rc = ant_hvf_check(hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc), "hv_vcpu_get_reg(PC)");
  if (rc != 0) return rc;
  return ant_hvf_check(hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4), "hv_vcpu_set_reg(PC)");
}

int ant_hvf_handle_mmio(ant_hvf_vm_t *vm, hv_vcpu_exit_exception_t *ex) {
  uint32_t esr = (uint32_t)ex->syndrome;
  unsigned ec = (esr >> ESR_EC_SHIFT) & 0x3f;
  if (ec == ESR_EC_HVC64) {
    uint64_t fn = 0;
    int rc = ant_hvf_check(hv_vcpu_get_reg(vm->vcpu, HV_REG_X0, &fn), "hv_vcpu_get_reg(HVC X0)");
    if (rc != 0) return rc;
    if (fn == 0x84000008u || fn == 0x84000009u) return ANT_HVF_GUEST_SHUTDOWN;
    rc = ant_hvf_check(hv_vcpu_set_reg(vm->vcpu, HV_REG_X0, UINT64_MAX), "hv_vcpu_set_reg(HVC X0)");
    if (rc != 0) return rc;
    return ant_hvf_advance_pc(vm->vcpu);
  }
  if (ec == ESR_EC_HLT && (esr & ESR_HLT_IMM16_MASK) == 0xf000u) {
    if (vm->trace) {
      uint64_t op = 0;
      uint64_t arg = 0;
      uint64_t status = 0;
      hv_vcpu_get_reg(vm->vcpu, HV_REG_X0, &op);
      hv_vcpu_get_reg(vm->vcpu, HV_REG_X1, &arg);
      if (op == 0x18u) ant_hvf_guest_read(vm, arg + 8, &status, sizeof(status));
      fprintf(stderr,
              "sandbox vm: guest semihosting exit op=0x%llx status=%llu\n",
              (unsigned long long)op,
              (unsigned long long)status);
    }
    return ANT_HVF_GUEST_SHUTDOWN;
  }

  if (ec != ESR_EC_DATA_ABORT || !(esr & ESR_DA_ISV)) {
    return -ENOSYS;
  }

  unsigned size = 1u << ((esr >> ESR_DA_SAS_SHIFT) & 0x3);
  unsigned reg = (esr >> ESR_DA_SRT_SHIFT) & 0x1f;
  bool write = (esr & ESR_DA_WNR) != 0;

  if (write) {
    uint64_t value = 0;
    int rc = ant_hvf_check(hv_vcpu_get_reg(vm->vcpu, (hv_reg_t)(HV_REG_X0 + reg), &value),
                           "hv_vcpu_get_reg(mmio write)");
    if (rc != 0) return rc;
    if (vm->trace) {
      fprintf(stderr,
              "sandbox vm: mmio write ipa=0x%llx size=%u x%u=0x%llx\n",
              (unsigned long long)ex->physical_address,
              size,
              reg,
              (unsigned long long)value);
    }
    if (!ant_hvf_mmio_write(vm, ex->physical_address, size, value)) return -ENOSYS;
  } else {
    uint64_t value = 0;
    if (!ant_hvf_mmio_read(vm, ex->physical_address, size, &value)) return -ENOSYS;
    if (vm->trace) {
      fprintf(stderr,
              "sandbox vm: mmio read ipa=0x%llx size=%u x%u=0x%llx\n",
              (unsigned long long)ex->physical_address,
              size,
              reg,
              (unsigned long long)value);
    }
    int rc = ant_hvf_check(hv_vcpu_set_reg(vm->vcpu, (hv_reg_t)(HV_REG_X0 + reg), value),
                           "hv_vcpu_set_reg(mmio read)");
    if (rc != 0) return rc;
  }

  return ant_hvf_advance_pc(vm->vcpu);
}

#endif
