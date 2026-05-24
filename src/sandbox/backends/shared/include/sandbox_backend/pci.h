#pragma once

#include <compat.h> // IWYU pragma: keep

#include "virtio.h"
#include "virtio_9p.h"

#include <stdbool.h>
#include <stdint.h>


#define ANT_PCI_STATUS_CAP_LIST 0x0010u
#define ANT_PCI_COMMAND_IO 0x0001u
#define ANT_PCI_COMMAND_MEMORY 0x0002u
#define ANT_PCI_COMMAND_BUS_MASTER 0x0004u
#define ANT_PCI_CAP_VENDOR 0x09u
#define ANT_PCI_CAP_MSIX 0x11u
#define ANT_PCI_MSIX_ENABLE 0x8000u
#define ANT_PCI_MSIX_MASK_ALL 0x4000u
#define ANT_PCI_MSIX_ENTRY_MASKED 0x00000001u
#define ANT_PCI_CONFIG_SIZE 256u
#define ANT_PCI_CONFIG_VENDOR_ID 0x00u
#define ANT_PCI_CONFIG_DEVICE_ID 0x02u
#define ANT_PCI_CONFIG_COMMAND 0x04u
#define ANT_PCI_CONFIG_STATUS 0x06u
#define ANT_PCI_CONFIG_REVISION_ID 0x08u
#define ANT_PCI_CONFIG_PROG_IF 0x09u
#define ANT_PCI_CONFIG_SUBCLASS 0x0au
#define ANT_PCI_CONFIG_CLASS_CODE 0x0bu
#define ANT_PCI_CONFIG_HEADER_TYPE 0x0eu
#define ANT_PCI_CONFIG_BAR0 0x10u
#define ANT_PCI_CONFIG_SUBVENDOR_ID 0x2cu
#define ANT_PCI_CONFIG_SUBSYSTEM_ID 0x2eu
#define ANT_PCI_CONFIG_CAP_PTR 0x34u
#define ANT_PCI_CONFIG_INTERRUPT_PIN 0x3du

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
void ant_hvf_virtio_build_config(ant_hvf_virtio_device_t *dev, unsigned char cfg[ANT_PCI_CONFIG_SIZE]);
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

