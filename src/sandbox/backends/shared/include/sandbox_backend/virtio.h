#pragma once

#include <compat.h> // IWYU pragma: keep

#include "forward.h"

#include <stdbool.h>
#include <stdint.h>


#define ANT_VIRTIO_PCI_VENDOR 0x1af4u
#define ANT_VIRTIO_PCI_DEVICE_MODERN_BASE 0x1040u
#define ANT_VIRTIO_PCI_SUBDEVICE_NET 1u
#define ANT_VIRTIO_PCI_SUBDEVICE_BLOCK 2u
#define ANT_VIRTIO_PCI_SUBDEVICE_ENTROPY 4u
#define ANT_VIRTIO_PCI_SUBDEVICE_9P 9u
#define ANT_VIRTIO_PCI_SUBDEVICE_VSOCK 19u
#define ANT_VIRTIO_PCI_SUBVENDOR 0x1af4u
#define ANT_VIRTIO_F_VERSION_1 (1ull << 32)

#define ANT_HVF_VIRTIO_BAR_SIZE 0x1000u
#define ANT_HVF_VIRTIO_COMMON_CFG 0x000u
#define ANT_HVF_VIRTIO_COMMON_CFG_SIZE 0x038u
#define ANT_HVF_VIRTIO_NOTIFY_CFG 0x100u
#define ANT_HVF_VIRTIO_ISR_CFG 0x200u
#define ANT_HVF_VIRTIO_DEVICE_CFG 0x300u
#define ANT_HVF_VIRTIO_MSIX_TABLE 0x800u
#define ANT_HVF_VIRTIO_MSIX_PBA 0x900u
#define ANT_HVF_VIRTIO_MSIX_ENTRY_SIZE 16u
#define ANT_HVF_VIRTIO_MSIX_VECTOR_COUNT 4u
#define ANT_HVF_VIRTIO_MAX_QUEUES 3u
#define ANT_HVF_VIRTIO_DEVICE_CONFIG_BYTES 1200u
#define ANT_HVF_VRING_USED_ELEM_BYTES 8u
#define ANT_VIRTIO_MSI_NO_VECTOR 0xffffu

#define ANT_VIRTIO_PCI_CAP_COMMON_CFG 1u
#define ANT_VIRTIO_PCI_CAP_NOTIFY_CFG 2u
#define ANT_VIRTIO_PCI_CAP_ISR_CFG 3u
#define ANT_VIRTIO_PCI_CAP_DEVICE_CFG 4u
#define ANT_VIRTIO_PCI_CAP_COMMON_POS 0x40u
#define ANT_VIRTIO_PCI_CAP_NOTIFY_POS 0x50u
#define ANT_VIRTIO_PCI_CAP_ISR_POS 0x70u
#define ANT_VIRTIO_PCI_CAP_DEVICE_POS 0x80u
#define ANT_VIRTIO_PCI_CAP_MSIX_POS 0x90u

#define ANT_VRING_DESC_F_NEXT 1u
#define ANT_VRING_DESC_F_WRITE 2u

typedef enum {
  ANT_HVF_VIRTIO_KIND_BLOCK,
  ANT_HVF_VIRTIO_KIND_NET,
  ANT_HVF_VIRTIO_KIND_9P,
  ANT_HVF_VIRTIO_KIND_VSOCK,
  ANT_HVF_VIRTIO_KIND_RNG,
} ant_hvf_virtio_kind_t;

typedef struct {
  uint64_t desc;
  uint64_t avail;
  uint64_t used;
  uint16_t size;
  uint16_t last_avail;
  uint16_t msix_vector;
  uint16_t notify_off;
  bool enabled;
} ant_hvf_virtio_queue_t;

typedef struct {
  uint32_t msg_addr_lo;
  uint32_t msg_addr_hi;
  uint32_t msg_data;
  uint32_t vector_control;
} ant_hvf_msix_entry_t;

typedef struct {
  ant_hvf_virtio_kind_t kind;
  const char *name;
  uint16_t virtio_id;
  uint16_t subsystem_id;
  uint8_t slot;
  uint8_t class_code;
  uint8_t subclass;
  uint32_t bar0;
  uint32_t pci_command;
  uint16_t msix_control;
  uint32_t msix_pba;
  uint64_t device_features;
  uint64_t driver_features;
  uint32_t device_feature_select;
  uint32_t driver_feature_select;
  uint16_t config_msix_vector;
  uint16_t queue_sel;
  uint16_t queue_count;
  uint8_t status;
  uint8_t isr;
  uint8_t config_generation;
  uint16_t device_config_len;
  ant_hvf_virtio_queue_t queues[ANT_HVF_VIRTIO_MAX_QUEUES];
  ant_hvf_msix_entry_t msix[ANT_HVF_VIRTIO_MSIX_VECTOR_COUNT];
} ant_hvf_virtio_device_t;

typedef struct {
  uint64_t addr;
  uint32_t len;
} ant_hvf_iov_t;

typedef struct {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
} ant_vring_desc_t;

void ant_hvf_virtio_init(ant_hvf_virtio_device_t *dev,
                         ant_hvf_virtio_kind_t kind,
                         const char *name,
                         uint16_t virtio_id,
                         uint16_t subsystem_id,
                         uint8_t slot,
                         uint8_t class_code,
                         uint8_t subclass,
                         uint32_t bar0,
                         uint64_t features,
                         uint16_t queue_count,
                         uint16_t queue_size,
                         uint16_t device_config_len);
ant_hvf_virtio_device_t *ant_hvf_virtio_for_bar(ant_hvf_vm_t *vm, uint64_t addr);
void ant_hvf_virtio_reset(ant_hvf_virtio_device_t *dev);
ant_hvf_virtio_queue_t *ant_hvf_virtio_selected_queue(ant_hvf_virtio_device_t *dev);
void ant_hvf_virtio_common_bytes(ant_hvf_virtio_device_t *dev, unsigned char out[ANT_HVF_VIRTIO_COMMON_CFG_SIZE]);
bool ant_hvf_virtio_device_config_read(ant_hvf_vm_t *vm,
                                       ant_hvf_virtio_device_t *dev,
                                       uint64_t off,
                                       unsigned size,
                                       uint64_t *value);
bool ant_hvf_virtio_msix_enabled(ant_hvf_virtio_device_t *dev);
bool ant_hvf_virtio_msix_masked(ant_hvf_virtio_device_t *dev, unsigned vector);
int ant_hvf_virtio_msix_notify(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev, uint16_t vector);
int ant_hvf_virtio_interrupt(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev, unsigned queue);
bool ant_hvf_virtio_msix_read(ant_hvf_virtio_device_t *dev, uint64_t off, unsigned size, uint64_t *value);
bool ant_hvf_virtio_msix_write(ant_hvf_vm_t *vm,
                               ant_hvf_virtio_device_t *dev,
                               uint64_t off,
                               unsigned size,
                               uint64_t value);
bool ant_hvf_virtio_common_read(ant_hvf_vm_t *vm,
                                ant_hvf_virtio_device_t *dev,
                                uint64_t off,
                                unsigned size,
                                uint64_t *value);
bool ant_hvf_virtio_common_write(ant_hvf_vm_t *vm,
                                 ant_hvf_virtio_device_t *dev,
                                 uint64_t off,
                                 unsigned size,
                                 uint64_t value);
int ant_hvf_vring_read_desc(ant_hvf_vm_t *vm, uint64_t desc_base, uint16_t index, ant_vring_desc_t *out);
int ant_hvf_vring_add_used(ant_hvf_vm_t *vm,
                           uint64_t used_base,
                           unsigned queue_size,
                           uint16_t head,
                           uint32_t used_len);
int ant_hvf_vring_read_chain(ant_hvf_vm_t *vm,
                             uint64_t desc_base,
                             uint16_t head,
                             unsigned queue_size,
                             unsigned char *out,
                             uint32_t out_cap,
                             uint32_t *out_len);
int ant_hvf_vring_write_chain(ant_hvf_vm_t *vm,
                              uint64_t desc_base,
                              uint16_t head,
                              unsigned queue_size,
                              const unsigned char *data,
                              uint32_t len,
                              uint32_t *used_len);
