#include "../src/sandbox/backends/darwin/backend.h"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__aarch64__) && defined(__APPLE__)

#define TEST_MEM_SIZE 0x10000u
#define TEST_DESC_BASE (ANT_HVF_GUEST_BASE + 0x1000u)
#define TEST_REQ_BASE (ANT_HVF_GUEST_BASE + 0x2000u)
#define TEST_DATA_BASE (ANT_HVF_GUEST_BASE + 0x3000u)
#define TEST_STATUS_BASE (ANT_HVF_GUEST_BASE + 0x5000u)

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

static void test_store_req(unsigned char *mem, uint32_t type, uint64_t sector) {
  unsigned char *req = mem + 0x2000u;
  ant_hvf_store32(req, type);
  ant_hvf_store32(req + 4, 0);
  ant_hvf_store64(req + 8, sector);
}

static void test_vm_init(ant_hvf_vm_t *vm, unsigned char *mem, int image_fd) {
  memset(vm, 0, sizeof(*vm));
  vm->host_mem = mem;
  vm->mem_size = TEST_MEM_SIZE;
  vm->image_fd = image_fd;
  vm->image_sectors = 8;
}

static void test_reset_mem(unsigned char *mem) {
  memset(mem, 0, TEST_MEM_SIZE);
}

static void test_config(void) {
  ant_hvf_vm_t vm;
  memset(&vm, 0, sizeof(vm));
  vm.image_sectors = 123;

  unsigned char cfg[ANT_VIRTIO_BLK_CONFIG_LEN];
  memset(cfg, 0, sizeof(cfg));
  ant_hvf_virtio_blk_config(&vm, cfg, sizeof(cfg));
  assert(ant_hvf_load64(cfg) == 123);
  assert(ant_hvf_load32(cfg + 12) == ANT_VIRTIO_BLK_SEG_MAX);
  assert(ant_hvf_load32(cfg + 20) == ANT_VIRTIO_BLK_SECTOR_SIZE);
}

static void test_read_request(void) {
  unsigned char mem[TEST_MEM_SIZE];
  test_reset_mem(mem);
  char path[] = "/tmp/ant-blk-read.XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  unlink(path);

  unsigned char image[4096];
  for (size_t i = 0; i < sizeof(image); i++) image[i] = (unsigned char)(i & 0xffu);
  assert(write(fd, image, sizeof(image)) == (ssize_t)sizeof(image));

  ant_hvf_vm_t vm;
  test_vm_init(&vm, mem, fd);
  test_store_req(mem, ANT_VIRTIO_BLK_T_IN, 2);
  test_store_desc(mem, 0, TEST_REQ_BASE, 16, ANT_VRING_DESC_F_NEXT, 1);
  test_store_desc(mem, 1, TEST_DATA_BASE, 512, ANT_VRING_DESC_F_NEXT | ANT_VRING_DESC_F_WRITE, 2);
  test_store_desc(mem, 2, TEST_STATUS_BASE, 1, ANT_VRING_DESC_F_WRITE, 0);

  uint32_t used_len = 0;
  assert(ant_hvf_virtio_blk_request(&vm, TEST_DESC_BASE, 8, 0, &used_len) == 0);
  assert(used_len == 513);
  assert(mem[0x5000] == ANT_VIRTIO_BLK_S_OK);
  assert(memcmp(mem + 0x3000, image + 1024, 512) == 0);
  close(fd);
}

static void test_write_request(void) {
  unsigned char mem[TEST_MEM_SIZE];
  test_reset_mem(mem);
  char path[] = "/tmp/ant-blk-write.XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  unlink(path);
  assert(ftruncate(fd, 4096) == 0);

  for (unsigned i = 0; i < 512; i++) mem[0x3000u + i] = (unsigned char)(0xa0u + (i & 0x1fu));

  ant_hvf_vm_t vm;
  test_vm_init(&vm, mem, fd);
  test_store_req(mem, ANT_VIRTIO_BLK_T_OUT, 3);
  test_store_desc(mem, 0, TEST_REQ_BASE, 16, ANT_VRING_DESC_F_NEXT, 1);
  test_store_desc(mem, 1, TEST_DATA_BASE, 512, ANT_VRING_DESC_F_NEXT, 2);
  test_store_desc(mem, 2, TEST_STATUS_BASE, 1, ANT_VRING_DESC_F_WRITE, 0);

  uint32_t used_len = 0;
  assert(ant_hvf_virtio_blk_request(&vm, TEST_DESC_BASE, 8, 0, &used_len) == 0);
  assert(used_len == 1);
  assert(mem[0x5000] == ANT_VIRTIO_BLK_S_OK);

  unsigned char out[512];
  assert(pread(fd, out, sizeof(out), 1536) == (ssize_t)sizeof(out));
  assert(memcmp(out, mem + 0x3000, sizeof(out)) == 0);
  close(fd);
}

static void test_flush_request(void) {
  unsigned char mem[TEST_MEM_SIZE];
  test_reset_mem(mem);
  char path[] = "/tmp/ant-blk-flush.XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  unlink(path);
  assert(ftruncate(fd, 4096) == 0);

  ant_hvf_vm_t vm;
  test_vm_init(&vm, mem, fd);
  test_store_req(mem, ANT_VIRTIO_BLK_T_FLUSH, 0);
  test_store_desc(mem, 0, TEST_REQ_BASE, 16, ANT_VRING_DESC_F_NEXT, 1);
  test_store_desc(mem, 1, TEST_STATUS_BASE, 1, ANT_VRING_DESC_F_WRITE, 0);

  uint32_t used_len = 0;
  assert(ant_hvf_virtio_blk_request(&vm, TEST_DESC_BASE, 8, 0, &used_len) == 0);
  assert(used_len == 1);
  assert(mem[0x5000] == ANT_VIRTIO_BLK_S_OK);
  close(fd);
}

static void test_unsupported_request(void) {
  unsigned char mem[TEST_MEM_SIZE];
  test_reset_mem(mem);
  char path[] = "/tmp/ant-blk-unsupp.XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  unlink(path);
  assert(ftruncate(fd, 4096) == 0);

  ant_hvf_vm_t vm;
  test_vm_init(&vm, mem, fd);
  test_store_req(mem, 99, 0);
  test_store_desc(mem, 0, TEST_REQ_BASE, 16, ANT_VRING_DESC_F_NEXT, 1);
  test_store_desc(mem, 1, TEST_STATUS_BASE, 1, ANT_VRING_DESC_F_WRITE, 0);

  uint32_t used_len = 0;
  assert(ant_hvf_virtio_blk_request(&vm, TEST_DESC_BASE, 8, 0, &used_len) == 0);
  assert(used_len == 1);
  assert(mem[0x5000] == ANT_VIRTIO_BLK_S_UNSUPP);
  close(fd);
}

static void test_bad_read_direction(void) {
  unsigned char mem[TEST_MEM_SIZE];
  test_reset_mem(mem);
  char path[] = "/tmp/ant-blk-baddir.XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  unlink(path);
  assert(ftruncate(fd, 4096) == 0);

  ant_hvf_vm_t vm;
  test_vm_init(&vm, mem, fd);
  test_store_req(mem, ANT_VIRTIO_BLK_T_IN, 0);
  test_store_desc(mem, 0, TEST_REQ_BASE, 16, ANT_VRING_DESC_F_NEXT, 1);
  test_store_desc(mem, 1, TEST_DATA_BASE, 512, ANT_VRING_DESC_F_NEXT, 2);
  test_store_desc(mem, 2, TEST_STATUS_BASE, 1, ANT_VRING_DESC_F_WRITE, 0);

  uint32_t used_len = 0;
  assert(ant_hvf_virtio_blk_request(&vm, TEST_DESC_BASE, 8, 0, &used_len) == 0);
  assert(mem[0x5000] == ANT_VIRTIO_BLK_S_IOERR);
  close(fd);
}

static void test_out_of_bounds_read(void) {
  unsigned char mem[TEST_MEM_SIZE];
  test_reset_mem(mem);
  char path[] = "/tmp/ant-blk-oob.XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  unlink(path);
  assert(ftruncate(fd, 4096) == 0);

  ant_hvf_vm_t vm;
  test_vm_init(&vm, mem, fd);
  test_store_req(mem, ANT_VIRTIO_BLK_T_IN, 8);
  test_store_desc(mem, 0, TEST_REQ_BASE, 16, ANT_VRING_DESC_F_NEXT, 1);
  test_store_desc(mem, 1, TEST_DATA_BASE, 512, ANT_VRING_DESC_F_NEXT | ANT_VRING_DESC_F_WRITE, 2);
  test_store_desc(mem, 2, TEST_STATUS_BASE, 1, ANT_VRING_DESC_F_WRITE, 0);

  uint32_t used_len = 0;
  assert(ant_hvf_virtio_blk_request(&vm, TEST_DESC_BASE, 8, 0, &used_len) == 0);
  assert(mem[0x5000] == ANT_VIRTIO_BLK_S_IOERR);
  close(fd);
}

int main(void) {
  test_config();
  test_read_request();
  test_write_request();
  test_flush_request();
  test_unsupported_request();
  test_bad_read_direction();
  test_out_of_bounds_read();
  return 0;
}

#else

int main(void) {
  return 0;
}

#endif
