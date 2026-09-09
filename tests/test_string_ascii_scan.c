// cc -std=gnu23 -Iinclude tests/test_string_ascii_scan.c -o /tmp/test-string-ascii-scan
#include "gc/strings.h"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
  unsigned char bytes[160];
  for (size_t offset = 0; offset < 16; offset++) {
    for (size_t len = 0; len <= 128; len++) {
      memset(bytes, 'a', sizeof(bytes));
      assert(str_detect_ascii_bytes((char *)bytes + offset, len) == STR_ASCII_YES);
      for (size_t i = 0; i < len; i++) {
        bytes[offset + i] = 0x80;
        assert(str_detect_ascii_bytes((char *)bytes + offset, len) == STR_ASCII_NO);
        bytes[offset + i] = 0; // Embedded NUL is still ASCII.
      }
      assert(str_detect_ascii_bytes((char *)bytes + offset, len) == STR_ASCII_YES);
    }
  }
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  char *region = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  assert(region != MAP_FAILED);
  assert(mprotect(region + page, page, PROT_NONE) == 0);
  memset(region, 'a', page);
  for (size_t len = 0; len <= 128; len++)
    assert(str_detect_ascii_bytes(region + page - len, len) == STR_ASCII_YES);
  assert(munmap(region, page * 2) == 0);
  assert(str_detect_ascii_bytes(NULL, 0) == STR_ASCII_YES);
  puts("PASS ASCII scan alignment, high bytes, NUL and allocation boundaries");
}
