#include "../src/sandbox/backends/darwin/backend.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#if defined(__aarch64__) && defined(__APPLE__)

static void test_rtc_ids(void) {
  ant_hvf_vm_t vm;
  memset(&vm, 0, sizeof(vm));
  uint64_t value = 0;

  assert(ant_hvf_mmio_read(&vm, ANT_HVF_RTC_BASE + 0xfe0, 1, &value));
  assert(value == 0x31);
  assert(ant_hvf_mmio_read(&vm, ANT_HVF_RTC_BASE + 0xfe4, 1, &value));
  assert(value == 0x10);
  assert(ant_hvf_mmio_read(&vm, ANT_HVF_RTC_BASE + 0xfe8, 1, &value));
  assert(value == 0x04);
  assert(ant_hvf_mmio_read(&vm, ANT_HVF_RTC_BASE + 0xfec, 1, &value));
  assert(value == 0x00);
  assert(ant_hvf_mmio_read(&vm, ANT_HVF_RTC_BASE + 0xff0, 1, &value));
  assert(value == 0x0d);
  assert(ant_hvf_mmio_read(&vm, ANT_HVF_RTC_BASE + 0xff4, 1, &value));
  assert(value == 0xf0);
  assert(ant_hvf_mmio_read(&vm, ANT_HVF_RTC_BASE + 0xff8, 1, &value));
  assert(value == 0x05);
  assert(ant_hvf_mmio_read(&vm, ANT_HVF_RTC_BASE + 0xffc, 1, &value));
  assert(value == 0xb1);
}

static void test_rtc_registers(void) {
  ant_hvf_vm_t vm;
  memset(&vm, 0, sizeof(vm));
  uint64_t value = 0;

  assert(ant_hvf_mmio_write(&vm, ANT_HVF_RTC_BASE + 0x008, 4, 1234));
  assert(ant_hvf_mmio_read(&vm, ANT_HVF_RTC_BASE + 0x000, 4, &value));
  assert(value == 1234);
  assert(ant_hvf_mmio_read(&vm, ANT_HVF_RTC_BASE + 0x008, 4, &value));
  assert(value == 1234);

  assert(ant_hvf_mmio_write(&vm, ANT_HVF_RTC_BASE + 0x004, 4, 1234));
  assert(ant_hvf_mmio_read(&vm, ANT_HVF_RTC_BASE + 0x014, 4, &value));
  assert(value == 1);
  assert(ant_hvf_mmio_read(&vm, ANT_HVF_RTC_BASE + 0x018, 4, &value));
  assert(value == 0);

  assert(ant_hvf_mmio_write(&vm, ANT_HVF_RTC_BASE + 0x010, 4, 1));
  assert(ant_hvf_mmio_read(&vm, ANT_HVF_RTC_BASE + 0x018, 4, &value));
  assert(value == 1);

  assert(ant_hvf_mmio_write(&vm, ANT_HVF_RTC_BASE + 0x00c, 4, 1));
  assert(ant_hvf_mmio_read(&vm, ANT_HVF_RTC_BASE + 0x00c, 4, &value));
  assert(value == 1);
}

static void test_unsupported_mmio(void) {
  ant_hvf_vm_t vm;
  memset(&vm, 0, sizeof(vm));
  uint64_t value = 0;

  assert(!ant_hvf_mmio_write(&vm, ANT_HVF_RTC_BASE + 0x000, 4, 1));
  assert(!ant_hvf_mmio_write(&vm, ANT_HVF_RTC_BASE + 0x008, 2, 1));
  assert(!ant_hvf_mmio_read(&vm, ANT_HVF_RTC_BASE + 0x020, 4, &value));
  assert(!ant_hvf_mmio_read(&vm, 0x0bad0000ull, 4, &value));
  assert(!ant_hvf_mmio_write(&vm, 0x0bad0000ull, 4, 1));
}

int main(void) {
  test_rtc_ids();
  test_rtc_registers();
  test_unsupported_mmio();
  return 0;
}

#else

int main(void) {
  return 0;
}

#endif
