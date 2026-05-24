#include "backend.h"

#if defined(__aarch64__)

bool ant_hvf_pci_addr(uint64_t addr, unsigned *bus, unsigned *slot, unsigned *fn, unsigned *reg) {
  if (addr < ANT_HVF_PCIE_ECAM_BASE || addr >= ANT_HVF_PCIE_ECAM_BASE + ANT_HVF_PCIE_ECAM_SIZE) {
    return false;
  }

  uint64_t off = addr - ANT_HVF_PCIE_ECAM_BASE;
  *bus = (unsigned)((off >> 20) & 0xff);
  *slot = (unsigned)((off >> 15) & 0x1f);
  *fn = (unsigned)((off >> 12) & 0x7);
  *reg = (unsigned)(off & 0xfff);
  return true;
}

bool ant_hvf_is_virtio_slot(unsigned bus, unsigned slot, unsigned fn) {
  bool p9_slot = slot >= ANT_HVF_VIRTIO_9P_SLOT_BASE &&
                 slot < ANT_HVF_VIRTIO_9P_SLOT_BASE + ANT_HVF_VIRTIO_9P_MAX;
  return bus == 0 && fn == 0 &&
         (slot == ANT_HVF_VIRTIO_BLK_SLOT || slot == ANT_HVF_VIRTIO_NET_SLOT ||
          p9_slot || slot == ANT_HVF_VIRTIO_VSOCK_SLOT);
}

ant_hvf_virtio_device_t *ant_hvf_virtio_for_slot(ant_hvf_vm_t *vm, unsigned slot) {
  if (slot == ANT_HVF_VIRTIO_BLK_SLOT) return &vm->blk;
  if (slot == ANT_HVF_VIRTIO_NET_SLOT) return vm->net_enabled ? &vm->net : NULL;
  ant_hvf_9p_device_t *p9 = ant_hvf_p9_for_slot(vm, slot);
  if (p9) return &p9->virtio;
  if (slot == ANT_HVF_VIRTIO_VSOCK_SLOT) return &vm->vsock.virtio;
  return NULL;
}

static ant_hvf_virtio_device_t *ant_hvf_pci_device_at(ant_hvf_vm_t *vm,
                                                      unsigned bus,
                                                      unsigned slot,
                                                      unsigned fn) {
  if (!ant_hvf_is_virtio_slot(bus, slot, fn)) return NULL;
  return ant_hvf_virtio_for_slot(vm, slot);
}

ant_hvf_9p_device_t *ant_hvf_p9_for_slot(ant_hvf_vm_t *vm, unsigned slot) {
  if (slot < ANT_HVF_VIRTIO_9P_SLOT_BASE) return NULL;
  unsigned index = slot - ANT_HVF_VIRTIO_9P_SLOT_BASE;
  if (index >= vm->p9_count || index >= ANT_HVF_VIRTIO_9P_MAX) return NULL;
  return &vm->p9[index];
}

ant_hvf_9p_device_t *ant_hvf_p9_for_virtio(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev) {
  for (size_t i = 0; i < vm->p9_count; i++) {
    if (&vm->p9[i].virtio == dev) return &vm->p9[i];
  }
  return NULL;
}

void ant_hvf_cfg_store16(unsigned char *cfg, unsigned off, uint16_t value) {
  ant_hvf_store16(cfg + off, value);
}

void ant_hvf_cfg_store32(unsigned char *cfg, unsigned off, uint32_t value) {
  ant_hvf_store32(cfg + off, value);
}

void ant_hvf_virtio_cfg_cap(unsigned char *cfg,
                                   unsigned pos,
                                   unsigned next,
                                   uint8_t type,
                                   uint32_t offset,
                                   uint32_t length,
                                   bool notify) {
  cfg[pos + 0] = ANT_PCI_CAP_VENDOR;
  cfg[pos + 1] = (uint8_t)next;
  cfg[pos + 2] = notify ? 20u : 16u;
  cfg[pos + 3] = type;
  cfg[pos + 4] = 0;
  cfg[pos + 5] = 0;
  cfg[pos + 6] = 0;
  cfg[pos + 7] = 0;
  ant_hvf_cfg_store32(cfg, pos + 8, offset);
  ant_hvf_cfg_store32(cfg, pos + 12, length);
  if (notify) ant_hvf_cfg_store32(cfg, pos + 16, 2u);
}

void ant_hvf_virtio_build_config(ant_hvf_virtio_device_t *dev, unsigned char cfg[ANT_PCI_CONFIG_SIZE]) {
  memset(cfg, 0, ANT_PCI_CONFIG_SIZE);

  ant_hvf_cfg_store16(cfg, ANT_PCI_CONFIG_VENDOR_ID, ANT_VIRTIO_PCI_VENDOR);
  ant_hvf_cfg_store16(cfg, ANT_PCI_CONFIG_DEVICE_ID, (uint16_t)(ANT_VIRTIO_PCI_DEVICE_MODERN_BASE + dev->virtio_id));
  ant_hvf_cfg_store16(cfg, ANT_PCI_CONFIG_COMMAND, (uint16_t)dev->pci_command);
  ant_hvf_cfg_store16(cfg, ANT_PCI_CONFIG_STATUS, ANT_PCI_STATUS_CAP_LIST);
  cfg[ANT_PCI_CONFIG_REVISION_ID] = 1;
  cfg[ANT_PCI_CONFIG_SUBCLASS] = dev->subclass;
  cfg[ANT_PCI_CONFIG_CLASS_CODE] = dev->class_code;
  cfg[ANT_PCI_CONFIG_HEADER_TYPE] = 0;
  ant_hvf_cfg_store32(cfg, ANT_PCI_CONFIG_BAR0, dev->bar0);
  ant_hvf_cfg_store16(cfg, ANT_PCI_CONFIG_SUBVENDOR_ID, ANT_VIRTIO_PCI_SUBVENDOR);
  ant_hvf_cfg_store16(cfg, ANT_PCI_CONFIG_SUBSYSTEM_ID, dev->subsystem_id);
  cfg[ANT_PCI_CONFIG_CAP_PTR] = ANT_VIRTIO_PCI_CAP_COMMON_POS;
  cfg[ANT_PCI_CONFIG_INTERRUPT_PIN] = 1;

  ant_hvf_virtio_cfg_cap(cfg,
                         ANT_VIRTIO_PCI_CAP_COMMON_POS,
                         ANT_VIRTIO_PCI_CAP_NOTIFY_POS,
                         ANT_VIRTIO_PCI_CAP_COMMON_CFG,
                         ANT_HVF_VIRTIO_COMMON_CFG,
                         ANT_HVF_VIRTIO_COMMON_CFG_SIZE,
                         false);
  ant_hvf_virtio_cfg_cap(cfg,
                         ANT_VIRTIO_PCI_CAP_NOTIFY_POS,
                         ANT_VIRTIO_PCI_CAP_ISR_POS,
                         ANT_VIRTIO_PCI_CAP_NOTIFY_CFG,
                         ANT_HVF_VIRTIO_NOTIFY_CFG,
                         (uint32_t)dev->queue_count * 2u,
                         true);
  ant_hvf_virtio_cfg_cap(cfg,
                         ANT_VIRTIO_PCI_CAP_ISR_POS,
                         ANT_VIRTIO_PCI_CAP_DEVICE_POS,
                         ANT_VIRTIO_PCI_CAP_ISR_CFG,
                         ANT_HVF_VIRTIO_ISR_CFG,
                         1u,
                         false);
  ant_hvf_virtio_cfg_cap(cfg,
                         ANT_VIRTIO_PCI_CAP_DEVICE_POS,
                         ANT_VIRTIO_PCI_CAP_MSIX_POS,
                         ANT_VIRTIO_PCI_CAP_DEVICE_CFG,
                         ANT_HVF_VIRTIO_DEVICE_CFG,
                         dev->device_config_len,
                         false);

  cfg[ANT_VIRTIO_PCI_CAP_MSIX_POS + 0] = ANT_PCI_CAP_MSIX;
  cfg[ANT_VIRTIO_PCI_CAP_MSIX_POS + 1] = 0;
  ant_hvf_cfg_store16(cfg,
                      ANT_VIRTIO_PCI_CAP_MSIX_POS + 2,
                      (uint16_t)((dev->msix_control & (ANT_PCI_MSIX_ENABLE | ANT_PCI_MSIX_MASK_ALL)) |
                                 (ANT_HVF_VIRTIO_MSIX_VECTOR_COUNT - 1u)));
  ant_hvf_cfg_store32(cfg, ANT_VIRTIO_PCI_CAP_MSIX_POS + 4, ANT_HVF_VIRTIO_MSIX_TABLE);
  ant_hvf_cfg_store32(cfg, ANT_VIRTIO_PCI_CAP_MSIX_POS + 8, ANT_HVF_VIRTIO_MSIX_PBA);
}

static uint32_t ant_hvf_pci_bar0_size_mask(void) {
  return ~(ANT_HVF_VIRTIO_BAR_SIZE - 1u);
}

static void ant_hvf_pci_write_bar0(ant_hvf_virtio_device_t *dev, unsigned reg, unsigned size, uint64_t value) {
  if (reg == ANT_PCI_CONFIG_BAR0 && size == 4) {
    uint32_t next = (uint32_t)value;
    dev->bar0 = next == UINT32_MAX ? UINT32_MAX : (next & ant_hvf_pci_bar0_size_mask());
    return;
  }
  ant_hvf_assign_width(&dev->bar0, reg & 3u, size, value);
}

static void ant_hvf_pci_write_command(ant_hvf_virtio_device_t *dev, unsigned reg, unsigned size, uint64_t value) {
  ant_hvf_assign_width(&dev->pci_command, reg & 3u, size, value);
  dev->pci_command &= ANT_PCI_COMMAND_IO | ANT_PCI_COMMAND_MEMORY | ANT_PCI_COMMAND_BUS_MASTER;
}

static void ant_hvf_pci_write_msix_control(ant_hvf_virtio_device_t *dev,
                                           unsigned reg,
                                           unsigned size,
                                           uint64_t value) {
  ant_hvf_assign_width16(&dev->msix_control, (reg - (ANT_VIRTIO_PCI_CAP_MSIX_POS + 2)) & 1u, size, value);
  dev->msix_control &= ANT_PCI_MSIX_ENABLE | ANT_PCI_MSIX_MASK_ALL;
}

uint32_t ant_hvf_pci_config_read32(ant_hvf_vm_t *vm, unsigned bus, unsigned slot, unsigned fn, unsigned reg) {
  if (bus == 0 && slot == 0 && fn == 0 && (reg & ~3u) == 0x0c) {
    return 0;
  }

  ant_hvf_virtio_device_t *dev = ant_hvf_pci_device_at(vm, bus, slot, fn);
  if (!dev) return UINT32_MAX;

  if ((reg & ~3u) == ANT_PCI_CONFIG_BAR0 && dev->bar0 == UINT32_MAX) {
    return ant_hvf_pci_bar0_size_mask();
  }

  unsigned char cfg[ANT_PCI_CONFIG_SIZE];
  ant_hvf_virtio_build_config(dev, cfg);
  unsigned aligned = reg & ~3u;
  if (aligned >= sizeof(cfg)) return 0;
  return ant_hvf_load32(cfg + aligned);
}

void ant_hvf_pci_config_write(ant_hvf_vm_t *vm,
                                     unsigned bus,
                                     unsigned slot,
                                     unsigned fn,
                                     unsigned reg,
                                     unsigned size,
                                     uint64_t value) {
  ant_hvf_virtio_device_t *dev = ant_hvf_pci_device_at(vm, bus, slot, fn);
  if (!dev) return;
  if ((reg & ~3u) == ANT_PCI_CONFIG_BAR0) {
    ant_hvf_pci_write_bar0(dev, reg, size, value);
    return;
  }
  if ((reg & ~3u) == ANT_PCI_CONFIG_COMMAND && (reg & 3u) < 2) {
    ant_hvf_pci_write_command(dev, reg, size, value);
    return;
  }
  if (reg >= ANT_VIRTIO_PCI_CAP_MSIX_POS + 2 && reg < ANT_VIRTIO_PCI_CAP_MSIX_POS + 4) {
    ant_hvf_pci_write_msix_control(dev, reg, size, value);
  }
}


#endif
