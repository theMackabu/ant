#ifndef ANT_CAGE_H
#define ANT_CAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static constexpr size_t ANT_CAGE_MAX_SIZE = 1ULL << 35;
static constexpr size_t ANT_CAGE_MIN_SIZE = 1ULL << 33;

extern uintptr_t ant_cage_base_address;
extern size_t ant_cage_reserved_size;

void *ant_cage_alloc(size_t size, size_t align);
void *ant_cage_reserve(size_t size, size_t align);
void ant_cage_free(void *ptr, size_t size);

_Noreturn void ant_cage_reject_pointer(const void *ptr);

static inline uintptr_t ant_cage_base(void) {
  return ant_cage_base_address;
}

static inline bool ant_cage_contains(const void *ptr) {
  uintptr_t value = (uintptr_t)ptr;
  uintptr_t base = ant_cage_base_address;
  return base && value >= base && value - base < ant_cage_reserved_size;
}

static inline uint64_t ant_cage_encode(const void *ptr) {
  if (!ptr) return 0;
  if (!ant_cage_contains(ptr)) ant_cage_reject_pointer(ptr);
  return (uint64_t)((uintptr_t)ptr - ant_cage_base_address);
}

static inline void *ant_cage_decode(uint64_t offset) {
  return offset ? (void *)(ant_cage_base_address + offset) : NULL;
}

#endif
