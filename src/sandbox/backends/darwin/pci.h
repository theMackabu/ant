#pragma once

#include <compat.h> // IWYU pragma: keep

#include "virtio.h"
#include "virtio_9p.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(__aarch64__)

#define ANT_PCI_STATUS_CAP_LIST 0x0010u
#define ANT_PCI_COMMAND_IO 0x0001u
#define ANT_PCI_COMMAND_MEMORY 0x0002u
#define ANT_PCI_COMMAND_BUS_MASTER 0x0004u
#define ANT_PCI_CAP_VENDOR 0x09u
#define ANT_PCI_CAP_MSIX 0x11u
#define ANT_PCI_MSIX_ENABLE 0x8000u
#define ANT_PCI_MSIX_MASK_ALL 0x4000u
#define ANT_PCI_MSIX_ENTRY_MASKED 0x00000001u

bool ant_hvf_pci_addr(uint64_t addr, unsigned *bus, unsigned *slot, unsigned *fn, unsigned *reg);
bool ant_hvf_is_virtio_slot(unsigned bus, unsigned slot, unsigned fn);
ant_hvf_virtio_device_t *ant_hvf_virtio_for_slot(ant_hvf_vm_t *vm, unsigned slot);
ant_hvf_9p_device_t *ant_hvf_p9_for_slot(ant_hvf_vm_t *vm, unsigned slot);
ant_hvf_9p_device_t *ant_hvf_p9_for_virtio(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev);
void ant_hvf_cfg_store16(unsigned char *cfg, unsigned off, uint16_t value);
void ant_hvf_cfg_store32(unsigned char *cfg, unsigned off, uint32_t value);
void ant_hvf_virtio_cfg_cap(unsigned char *cfg,
                            unsigned pos,
                            unsigned next,
                            uint8_t type,
                            uint32_t offset,
                            uint32_t length,
                            bool notify);
void ant_hvf_virtio_build_config(ant_hvf_virtio_device_t *dev, unsigned char cfg[256]);
uint32_t ant_hvf_pci_config_read32(ant_hvf_vm_t *vm, unsigned bus, unsigned slot, unsigned fn, unsigned reg);
void ant_hvf_pci_config_write(ant_hvf_vm_t *vm,
                              unsigned bus,
                              unsigned slot,
                              unsigned fn,
                              unsigned reg,
                              unsigned size,
                              uint64_t value);
bool ant_hvf_pci_mmio_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value);
bool ant_hvf_pci_mmio_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value);

#endif
