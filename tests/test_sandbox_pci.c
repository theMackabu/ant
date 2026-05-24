#include "../src/sandbox/backends/darwin/backend.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#if defined(__aarch64__) && defined(__APPLE__)

static void test_vm_init(ant_hvf_vm_t *vm) {
  memset(vm, 0, sizeof(*vm));
  ant_hvf_virtio_init(&vm->blk,
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
                      ANT_VIRTIO_BLK_CONFIG_LEN);
  ant_hvf_virtio_init(&vm->vsock.virtio,
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
                      8);
}

static void test_ecam_decode(void) {
  unsigned bus = 99;
  unsigned slot = 99;
  unsigned fn = 99;
  unsigned reg = 99;
  uint64_t addr = ANT_HVF_PCIE_ECAM_BASE + (2ull << 20) + (3ull << 15) + (4ull << 12) + 0xabcull;
  assert(ant_hvf_pci_addr(addr, &bus, &slot, &fn, &reg));
  assert(bus == 2);
  assert(slot == 3);
  assert(fn == 4);
  assert(reg == 0xabc);
  assert(!ant_hvf_pci_addr(ANT_HVF_PCIE_ECAM_BASE - 1, &bus, &slot, &fn, &reg));
}

static void test_present_and_absent_reads(void) {
  ant_hvf_vm_t vm;
  test_vm_init(&vm);

  uint32_t id = ant_hvf_pci_config_read32(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, 0);
  assert((id & 0xffffu) == ANT_VIRTIO_PCI_VENDOR);
  assert(((id >> 16) & 0xffffu) == ANT_VIRTIO_PCI_DEVICE_MODERN_BASE + ANT_VIRTIO_PCI_SUBDEVICE_BLOCK);
  assert(ant_hvf_pci_config_read32(&vm, 0, 31, 0, 0) == UINT32_MAX);
  assert(ant_hvf_pci_config_read32(&vm, 0, ANT_HVF_VIRTIO_NET_SLOT, 0, 0) == UINT32_MAX);
  assert(ant_hvf_pci_config_read32(&vm, 0, 0, 0, 0x0c) == 0);
}

static void test_command_mask(void) {
  ant_hvf_vm_t vm;
  test_vm_init(&vm);

  ant_hvf_pci_config_write(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_PCI_CONFIG_COMMAND, 2, UINT64_MAX);
  assert(vm.blk.pci_command == (ANT_PCI_COMMAND_IO | ANT_PCI_COMMAND_MEMORY | ANT_PCI_COMMAND_BUS_MASTER));
}

static void test_bar_sizing_and_assignment(void) {
  ant_hvf_vm_t vm;
  test_vm_init(&vm);

  ant_hvf_pci_config_write(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_PCI_CONFIG_BAR0, 4, UINT32_MAX);
  assert(vm.blk.bar0 == UINT32_MAX);
  assert(ant_hvf_pci_config_read32(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_PCI_CONFIG_BAR0) ==
         ~(ANT_HVF_VIRTIO_BAR_SIZE - 1u));

  ant_hvf_pci_config_write(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_PCI_CONFIG_BAR0, 4, 0x10041fffu);
  assert(vm.blk.bar0 == 0x10041000u);

  vm.blk.bar0 = 0;
  ant_hvf_pci_config_write(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_PCI_CONFIG_BAR0 + 0, 1, 0xff);
  ant_hvf_pci_config_write(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_PCI_CONFIG_BAR0 + 1, 1, 0xff);
  ant_hvf_pci_config_write(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_PCI_CONFIG_BAR0 + 2, 1, 0xff);
  ant_hvf_pci_config_write(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_PCI_CONFIG_BAR0 + 3, 1, 0xff);
  assert(vm.blk.bar0 == UINT32_MAX);
}

static void test_msix_control_mask(void) {
  ant_hvf_vm_t vm;
  test_vm_init(&vm);

  ant_hvf_pci_config_write(&vm, 0, ANT_HVF_VIRTIO_BLK_SLOT, 0, ANT_VIRTIO_PCI_CAP_MSIX_POS + 2, 2, UINT64_MAX);
  assert(vm.blk.msix_control == (ANT_PCI_MSIX_ENABLE | ANT_PCI_MSIX_MASK_ALL));
}

int main(void) {
  test_ecam_decode();
  test_present_and_absent_reads();
  test_command_mask();
  test_bar_sizing_and_assignment();
  test_msix_control_mask();
  return 0;
}

#else

int main(void) {
  return 0;
}

#endif
