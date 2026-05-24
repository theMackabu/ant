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

  ant_hvf_gic.send_msi(ANT_HVF_GIC_MSI_BASE + ANT_HVF_GICM_SET_SPI_NSR, intid);
  return true;
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

static bool ant_hvf_mmio_unsupported(ant_hvf_vm_t *vm,
                                     const char *region,
                                     bool write,
                                     uint64_t addr,
                                     unsigned size,
                                     uint64_t value) {
  if (write) {
    fprintf(stderr,
            "sandbox vm: unsupported %s MMIO write addr=0x%llx size=%u value=0x%llx\n",
            region,
            (unsigned long long)addr,
            size,
            (unsigned long long)value);
  } else {
    fprintf(stderr,
            "sandbox vm: unsupported %s MMIO read addr=0x%llx size=%u\n",
            region,
            (unsigned long long)addr,
            size);
  }
  (void)vm;
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
  if (addr >= ANT_HVF_GIC_MSI_BASE && addr < ANT_HVF_GIC_MSI_BASE + ANT_HVF_GIC_MSI_SIZE) {
    return ant_hvf_gic_msi_read(vm, addr, size, value);
  }
  if (addr >= ANT_HVF_RTC_BASE && addr < ANT_HVF_RTC_BASE + 0x1000) {
    return ant_hvf_rtc_read(vm, addr, size, value);
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
  return ant_hvf_mmio_unsupported(vm, "unknown", false, addr, size, 0);
}

bool ant_hvf_mmio_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value) {
  if (addr == ANT_HVF_UART_BASE) {
    ant_hvf_uart_put(vm, (uint8_t)(value & 0xff));
    return true;
  }
  if (addr >= ANT_HVF_GIC_MSI_BASE && addr < ANT_HVF_GIC_MSI_BASE + ANT_HVF_GIC_MSI_SIZE) {
    return ant_hvf_gic_msi_write(vm, addr, size, value);
  }
  if (addr >= ANT_HVF_RTC_BASE && addr < ANT_HVF_RTC_BASE + 0x1000) {
    return ant_hvf_rtc_write(vm, addr, size, value);
  }
  if (addr >= ANT_HVF_PCIE_ECAM_BASE && addr < ANT_HVF_PCIE_ECAM_BASE + ANT_HVF_PCIE_ECAM_SIZE) {
    return ant_hvf_pci_mmio_write(vm, addr, size, value);
  }
  ant_hvf_virtio_device_t *virtio = ant_hvf_virtio_for_bar(vm, addr);
  if (virtio) {
    return ant_hvf_virtio_common_write(vm, virtio, addr - virtio->bar0, size, value);
  }
  if (addr >= ANT_HVF_PCIE_PIO_BASE && addr < ANT_HVF_PCIE_PIO_BASE + 0x10000) return true;
  return ant_hvf_mmio_unsupported(vm, "unknown", true, addr, size, value);
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
    if (!ant_hvf_mmio_write(vm, ex->physical_address, size, value)) return -ENOSYS;
  } else {
    uint64_t value = 0;
    if (!ant_hvf_mmio_read(vm, ex->physical_address, size, &value)) return -ENOSYS;
    int rc = ant_hvf_check(hv_vcpu_set_reg(vm->vcpu, (hv_reg_t)(HV_REG_X0 + reg), value),
                           "hv_vcpu_set_reg(mmio read)");
    if (rc != 0) return rc;
  }

  return ant_hvf_advance_pc(vm->vcpu);
}

#endif
