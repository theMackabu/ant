#include "../src/sandbox/backends/darwin/backend.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#if defined(__aarch64__) && defined(__APPLE__)

#define TEST_MEM_SIZE 0x10000u
#define TEST_DESC_BASE (ANT_HVF_GUEST_BASE + 0x1000u)
#define TEST_AVAIL_BASE (ANT_HVF_GUEST_BASE + 0x2000u)
#define TEST_USED_BASE (ANT_HVF_GUEST_BASE + 0x3000u)
#define TEST_DATA_BASE (ANT_HVF_GUEST_BASE + 0x4000u)
#define TEST_DATA2_BASE (ANT_HVF_GUEST_BASE + 0x5000u)

static void test_store_desc(unsigned char *mem,
                            unsigned index,
                            uint64_t addr,
                            uint32_t len,
                            uint16_t flags,
                            uint16_t next) {
  unsigned char *desc = mem + 0x1000u + index * 16u;
  ant_hvf_store64(desc, addr);
  ant_hvf_store32(desc + 8, len);
  ant_hvf_store16(desc + 12, flags);
  ant_hvf_store16(desc + 14, next);
}

static void test_store_avail(unsigned char *mem, uint16_t idx, uint16_t head) {
  ant_hvf_store16(mem + 0x2000u + 2, idx);
  ant_hvf_store16(mem + 0x2000u + 4, head);
}

static void test_vm_init(ant_hvf_vm_t *vm, unsigned char *mem) {
  memset(vm, 0, sizeof(*vm));
  vm->host_mem = mem;
  vm->mem_size = TEST_MEM_SIZE;
  ant_hvf_virtio_init(&vm->rng,
                      ANT_HVF_VIRTIO_KIND_RNG,
                      "virtio-rng",
                      ANT_VIRTIO_PCI_SUBDEVICE_ENTROPY,
                      ANT_VIRTIO_PCI_SUBDEVICE_ENTROPY,
                      ANT_HVF_VIRTIO_RNG_SLOT,
                      0xff,
                      0x00,
                      (uint32_t)ANT_HVF_VIRTIO_RNG_BAR,
                      0,
                      1,
                      ANT_VIRTIO_RNG_QUEUE_SIZE,
                      0);
  vm->rng.queues[0].enabled = true;
  vm->rng.queues[0].desc = TEST_DESC_BASE;
  vm->rng.queues[0].avail = TEST_AVAIL_BASE;
  vm->rng.queues[0].used = TEST_USED_BASE;
}

static int all_zero(const unsigned char *p, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (p[i] != 0) return 0;
  }
  return 1;
}

static void test_single_descriptor_fill(void) {
  unsigned char mem[TEST_MEM_SIZE];
  memset(mem, 0, sizeof(mem));
  ant_hvf_vm_t vm;
  test_vm_init(&vm, mem);
  test_store_desc(mem, 0, TEST_DATA_BASE, 64, ANT_VRING_DESC_F_WRITE, 0);
  test_store_avail(mem, 1, 0);

  assert(ant_hvf_virtio_rng_notify(&vm, &vm.rng) == 0);
  assert(!all_zero(mem + 0x4000u, 64));
  assert(ant_hvf_load16(mem + 0x3000u + 2) == 1);
  assert(ant_hvf_load32(mem + 0x3000u + 4) == 0);
  assert(ant_hvf_load32(mem + 0x3000u + 8) == 64);
}

static void test_chained_descriptor_fill(void) {
  unsigned char mem[TEST_MEM_SIZE];
  memset(mem, 0, sizeof(mem));
  ant_hvf_vm_t vm;
  test_vm_init(&vm, mem);
  test_store_desc(mem, 0, TEST_DATA_BASE, 32, ANT_VRING_DESC_F_WRITE | ANT_VRING_DESC_F_NEXT, 1);
  test_store_desc(mem, 1, TEST_DATA2_BASE, 48, ANT_VRING_DESC_F_WRITE, 0);
  test_store_avail(mem, 1, 0);

  assert(ant_hvf_virtio_rng_notify(&vm, &vm.rng) == 0);
  assert(!all_zero(mem + 0x4000u, 32));
  assert(!all_zero(mem + 0x5000u, 48));
  assert(ant_hvf_load16(mem + 0x3000u + 2) == 1);
  assert(ant_hvf_load32(mem + 0x3000u + 8) == 80);
}

static void test_rejects_readonly_descriptor(void) {
  unsigned char mem[TEST_MEM_SIZE];
  memset(mem, 0, sizeof(mem));
  ant_hvf_vm_t vm;
  test_vm_init(&vm, mem);
  test_store_desc(mem, 0, TEST_DATA_BASE, 64, 0, 0);
  test_store_avail(mem, 1, 0);

  assert(ant_hvf_virtio_rng_notify(&vm, &vm.rng) == -EINVAL);
  assert(ant_hvf_load16(mem + 0x3000u + 2) == 0);
}

int main(void) {
  test_single_descriptor_fill();
  test_chained_descriptor_fill();
  test_rejects_readonly_descriptor();
  return 0;
}

#else

int main(void) {
  return 0;
}

#endif
