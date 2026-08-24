#include "cage.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void) {
  const size_t block_size = 64 * 1024;
  void *first = ant_cage_alloc(block_size, block_size);
  void *second = ant_cage_alloc(block_size, block_size);

  assert(first != NULL);
  assert(((uintptr_t)first & (block_size - 1)) == 0);
  assert(second == (void *)((uintptr_t)first + block_size));
  assert(ant_cage_decode(ant_cage_encode(first)) == first);
  memset(first, 0xa5, block_size);
  memset(second, 0x5a, block_size);

  ant_cage_free(first, block_size);
  ant_cage_free(second, block_size);

  void *merged = ant_cage_reserve(block_size * 2, block_size);
  assert(merged == first);
  ant_cage_free(merged, block_size * 2);

  void *reused = ant_cage_alloc(block_size * 2, block_size);
  assert(reused == first);
  memset(reused, 0, block_size * 2);
  ant_cage_free(reused, block_size * 2);
  return 0;
}
