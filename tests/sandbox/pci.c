#include "darwin/backend.h"
#include "wire.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#if defined(__aarch64__) && defined(__APPLE__)

static void test_vm_init(ant_hvf_vm_t *vm) {
  memset(vm, 0, sizeof(*vm));
  ant_devices_add_block(ant_hvf_devices(vm), ANT_HVF_VIRTIO_BLK_SLOT, ANT_HVF_VIRTIO_BLK_BAR);
  ant_devices_add_vsock(ant_hvf_devices(vm), ANT_HVF_VIRTIO_VSOCK_SLOT, ANT_HVF_VIRTIO_VSOCK_BAR);
  ant_devices_add_rng(ant_hvf_devices(vm), ANT_HVF_VIRTIO_RNG_SLOT, ANT_HVF_VIRTIO_RNG_BAR);
}

static void test_ecam_decode(void) {
  ant_hvf_vm_t vm;
  test_vm_init(&vm);
  unsigned bus = 99;
  unsigned slot = 99;
  unsigned fn = 99;
  unsigned reg = 99;
  uint64_t addr = ANT_HVF_PCIE_ECAM_BASE + (2ull << 20) + (3ull << 15) + (4ull << 12) + 0xabcull;
  assert(ant_hvf_pci_addr(&vm, addr, &bus, &slot, &fn, &reg));
  assert(bus == 2);
  assert(slot == 3);
  assert(fn == 4);
  assert(reg == 0xabc);
  assert(!ant_hvf_pci_addr(&vm, ANT_HVF_PCIE_ECAM_BASE - 1, &bus, &slot, &fn, &reg));
}

static void test_present_and_absent_reads(void) {
  ant_hvf_vm_t vm;
  test_vm_init(&vm);

  uint32_t id = ant_hvf_pci_config_read32(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, 0);
  assert((id & 0xffffu) == ANT_VIRTIO_PCI_VENDOR);
  assert(((id >> 16) & 0xffffu) == ANT_VIRTIO_PCI_DEVICE_MODERN_BASE + ANT_VIRTIO_PCI_SUBDEVICE_BLOCK);
  id = ant_hvf_pci_config_read32(&vm, 0, ANT_HVF_VIRTIO_RNG_SLOT, 0, 0);
  assert((id & 0xffffu) == ANT_VIRTIO_PCI_VENDOR);
  assert(((id >> 16) & 0xffffu) == ANT_VIRTIO_PCI_DEVICE_MODERN_BASE + ANT_VIRTIO_PCI_SUBDEVICE_ENTROPY);
  assert(ant_hvf_pci_config_read32(&vm, 0, 31, 0, 0) == UINT32_MAX);
  assert(ant_hvf_pci_config_read32(&vm, 0, ANT_HVF_VIRTIO_NET_SLOT, 0, 0) == UINT32_MAX);
  assert(ant_hvf_pci_config_read32(&vm, 0, 0, 0, 0x0c) == 0);
}

static void test_zero_config_device_skips_device_config_cap(void) {
  ant_hvf_vm_t vm;
  test_vm_init(&vm);

  uint32_t isr_cap = ant_hvf_pci_config_read32(&vm, 0, ANT_HVF_VIRTIO_RNG_SLOT, 0, ANT_VIRTIO_PCI_CAP_ISR_POS);
  assert((isr_cap & 0xffu) == ANT_PCI_CAP_VENDOR);
  assert(((isr_cap >> 8) & 0xffu) == ANT_VIRTIO_PCI_CAP_MSIX_POS);
}

static void test_command_mask(void) {
  ant_hvf_vm_t vm;
  test_vm_init(&vm);

  ant_hvf_pci_config_write(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_PCI_CONFIG_COMMAND, 2, UINT64_MAX);
  uint32_t command = ant_hvf_pci_config_read32(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_PCI_CONFIG_COMMAND);
  assert((command & 0xffffu) == (ANT_PCI_COMMAND_IO | ANT_PCI_COMMAND_MEMORY | ANT_PCI_COMMAND_BUS_MASTER));
}

static void test_bar_sizing_and_assignment(void) {
  ant_hvf_vm_t vm;
  test_vm_init(&vm);

  ant_hvf_pci_config_write(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_PCI_CONFIG_BAR0, 4, UINT32_MAX);
  assert(ant_hvf_virtio_bar0(ant_devices_at(vm.devices, ANT_HVF_VIRTIO_BLK_SLOT)) == UINT32_MAX);
  assert(ant_hvf_pci_config_read32(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_PCI_CONFIG_BAR0) ==
         ~(ANT_HVF_VIRTIO_BAR_SIZE - 1u));

  ant_hvf_pci_config_write(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_PCI_CONFIG_BAR0, 4, 0x10041fffu);
  assert(ant_hvf_virtio_bar0(ant_devices_at(vm.devices, ANT_HVF_VIRTIO_BLK_SLOT)) == 0x10041000u);

  ant_hvf_pci_config_write(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_PCI_CONFIG_BAR0, 4, 0);
  ant_hvf_pci_config_write(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_PCI_CONFIG_BAR0 + 0, 1, 0xff);
  ant_hvf_pci_config_write(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_PCI_CONFIG_BAR0 + 1, 1, 0xff);
  ant_hvf_pci_config_write(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_PCI_CONFIG_BAR0 + 2, 1, 0xff);
  ant_hvf_pci_config_write(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_PCI_CONFIG_BAR0 + 3, 1, 0xff);
  assert(ant_hvf_virtio_bar0(ant_devices_at(vm.devices, ANT_HVF_VIRTIO_BLK_SLOT)) == UINT32_MAX);
}

static void test_msix_control_mask(void) {
  ant_hvf_vm_t vm;
  test_vm_init(&vm);

  ant_hvf_pci_config_write(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_VIRTIO_PCI_CAP_MSIX_POS + 2, 2, UINT64_MAX);
  uint32_t msix = ant_hvf_pci_config_read32(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_VIRTIO_PCI_CAP_MSIX_POS);
  assert(((msix >> 16) & (ANT_PCI_MSIX_ENABLE | ANT_PCI_MSIX_MASK_ALL)) == (ANT_PCI_MSIX_ENABLE | ANT_PCI_MSIX_MASK_ALL));
}

static uint16_t test_queue_size(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev, uint16_t queue) {
  uint64_t value = 0;
  ant_hvf_virtio_common_write(vm, dev, 22, 2, queue);
  assert(ant_hvf_virtio_common_read(vm, dev, 24, 2, &value));
  return (uint16_t)value;
}

static void test_queue_size_validation(void) {
  ant_hvf_vm_t vm;
  test_vm_init(&vm);
  ant_hvf_virtio_device_t *dev = ant_devices_at(vm.devices, ANT_HVF_VIRTIO_VSOCK_SLOT);

  assert(test_queue_size(&vm, dev, 0) == ANT_VIRTIO_VSOCK_QUEUE_SIZE);
  ant_hvf_virtio_common_write(&vm, dev, 24, 2, 0);
  assert(test_queue_size(&vm, dev, 0) == ANT_VIRTIO_VSOCK_QUEUE_SIZE);
  ant_hvf_virtio_common_write(&vm, dev, 24, 2, 24);
  assert(test_queue_size(&vm, dev, 0) == ANT_VIRTIO_VSOCK_QUEUE_SIZE);
  ant_hvf_virtio_common_write(&vm, dev, 24, 2, ANT_VIRTIO_VSOCK_QUEUE_SIZE * 2u);
  assert(test_queue_size(&vm, dev, 0) == ANT_VIRTIO_VSOCK_QUEUE_SIZE);
  ant_hvf_virtio_common_write(&vm, dev, 24, 2, 16);
  assert(test_queue_size(&vm, dev, 0) == 16);

  ant_hvf_virtio_common_write(&vm, dev, 20, 1, 0);
  assert(test_queue_size(&vm, dev, 0) == ANT_VIRTIO_VSOCK_QUEUE_SIZE);
}

int main(void) {
  test_ecam_decode();
  test_present_and_absent_reads();
  test_zero_config_device_skips_device_config_cap();
  test_command_mask();
  test_bar_sizing_and_assignment();
  test_msix_control_mask();
  test_queue_size_validation();
  return 0;
}

#else

int main(void) {
  return 0;
}

#endif
