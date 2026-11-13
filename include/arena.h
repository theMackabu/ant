#pragma once

#include "cage.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif !defined(ANT_WASM_EMBED)
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#endif

typedef struct {
  uint8_t *base;
  size_t used;
  size_t committed;
} ant_arena_chunk_t;

typedef struct ant_fixed_arena {
  uint8_t *cursor;
  uint8_t *end;
  uint8_t *last_end;
  void *free_list;
  size_t live_count;
  size_t elem_size;
  size_t epoch_offset;
  size_t chunk_bytes;
  size_t committed;
  size_t limit;
  ant_arena_chunk_t *chunks;
  uint32_t chunk_count;
  uint32_t chunk_cap;
  _Atomic(uint32_t) *owners;
  _Atomic(uint16_t) *ordinals;
  uintptr_t table_base;
  size_t table_slots;
  uint32_t id;
} ant_fixed_arena_t;

#ifdef ANT_WASM_EMBED
static constexpr unsigned ANT_ARENA_CHUNK_SHIFT = 20;
static constexpr size_t ANT_ARENA_MAX         = 8ULL * 1024 * 1024;
static constexpr size_t ANT_CLOSURE_ARENA_MAX = 4ULL * 1024 * 1024;
static constexpr size_t ANT_UPVALUE_ARENA_MAX = 2ULL * 1024 * 1024;
static constexpr size_t ANT_PROMISE_ARENA_MAX = 2ULL * 1024 * 1024;
static constexpr size_t ANT_ARENA_MIN         = 8ULL * 1024 * 1024;
#else
static constexpr unsigned ANT_ARENA_CHUNK_SHIFT = 23;
static constexpr size_t ANT_ARENA_MAX         = 4ULL * 1024 * 1024 * 1024;
static constexpr size_t ANT_CLOSURE_ARENA_MAX = 512ULL * 1024 * 1024;
static constexpr size_t ANT_UPVALUE_ARENA_MAX = 512ULL * 1024 * 1024;
static constexpr size_t ANT_PROMISE_ARENA_MAX = 512ULL * 1024 * 1024;
static constexpr size_t ANT_ARENA_MIN         = 16ULL * 1024 * 1024;
#endif

static constexpr size_t ANT_ARENA_CHUNK_SIZE = (size_t)1 << ANT_ARENA_CHUNK_SHIFT;
static constexpr size_t ARENA_GROW_INCREMENT = ANT_ARENA_CHUNK_SIZE;

static constexpr uint32_t ANT_ARENA_MAX_CHUNKS = (uint32_t)UINT16_MAX + 1;
static constexpr uint32_t ANT_ARENA_PARTIAL_CHUNK = (uint32_t)1 << 31;

#ifndef ANT_WASM_EMBED
static_assert(
  ANT_ARENA_CHUNK_SIZE <= ANT_CAGE_ALIGNMENT, 
  "arena chunks must not straddle cage-aligned table slots"
);
#endif

static inline size_t ant_arena_page_size(void) {
#ifdef ANT_WASM_EMBED
  return 65536u;
#elif defined(_WIN32)
  static size_t cached = 0;
  if (cached) return cached;
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  cached = info.dwPageSize ? (size_t)info.dwPageSize : 4096u;
  return cached;
#else
  long page = sysconf(_SC_PAGESIZE);
  return page > 0 ? (size_t)page : 4096u;
#endif
}

static inline size_t ant_arena_round_up_page(size_t size) {
  size_t page_size = ant_arena_page_size();
  if (size == 0) return 0;
  return ((size + page_size - 1) / page_size) * page_size;
}

#ifdef ANT_WASM_EMBED

static inline int ant_arena_commit(void *base, size_t old_size, size_t new_size) {
  (void)base;
  (void)old_size;
  (void)new_size;
  return 0;
}

static inline int ant_arena_decommit(void *base, size_t old_size, size_t new_size) {
  (void)base;
  (void)old_size;
  (void)new_size;
  return 0;
}

#elif defined(_WIN32)

static inline int ant_arena_commit(void *base, size_t old_size, size_t new_size) {
  if (new_size <= old_size) return 0;
  size_t old_pages = ant_arena_round_up_page(old_size);
  size_t new_pages = ant_arena_round_up_page(new_size);

  if (new_pages <= old_pages) return 0;
  void *p = VirtualAlloc((char *)base + old_pages, new_pages - old_pages, MEM_COMMIT, PAGE_READWRITE);

  return p ? 0 : -1;
}

static inline int ant_arena_decommit(void *base, size_t old_size, size_t new_size) {
  if (new_size >= old_size) return 0;
  size_t new_pages = ant_arena_round_up_page(new_size);
  size_t old_pages = ant_arena_round_up_page(old_size);

  if (new_pages >= old_pages) return 0;
  void *decommit_start = (char *)base + new_pages;
  size_t decommit_size = old_pages - new_pages;

  return VirtualFree(decommit_start, decommit_size, MEM_DECOMMIT) ? 0 : -1;
}

#else

static inline int ant_arena_commit(void *base, size_t old_size, size_t new_size) {
  if (new_size <= old_size) return 0;

  long page_size_long = sysconf(_SC_PAGESIZE);
  if (page_size_long <= 0) {
    errno = (page_size_long == -1 && errno == 0) ? EINVAL : errno;
    return -1;
  }

  size_t page_size = (size_t)page_size_long;
  size_t old_pages = ((old_size + page_size - 1) / page_size) * page_size;
  size_t new_pages = ((new_size + page_size - 1) / page_size) * page_size;

  if (new_pages <= old_pages) return 0;
#ifdef __APPLE__
  madvise((char *)base + old_pages, new_pages - old_pages, MADV_FREE_REUSE);
#endif
  return mprotect((char *)base + old_pages, new_pages - old_pages, PROT_READ | PROT_WRITE);
}

static inline int ant_arena_decommit(void *base, size_t old_size, size_t new_size) {
  if (new_size >= old_size) return 0;

  long page_size_long = sysconf(_SC_PAGESIZE);
  if (page_size_long <= 0) return -1;

  size_t page_size = (size_t)page_size_long;
  size_t new_pages = ((new_size + page_size - 1) / page_size) * page_size;
  size_t old_pages = ((old_size + page_size - 1) / page_size) * page_size;

  if (new_pages >= old_pages) return 0;

  void *decommit_start = (char *)base + new_pages;
  size_t decommit_size = old_pages - new_pages;

#ifdef __APPLE__
  madvise(decommit_start, decommit_size, MADV_FREE_REUSABLE);
  if (mprotect(decommit_start, decommit_size, PROT_NONE) != 0) return -1;
#else
  if (mprotect(decommit_start, decommit_size, PROT_NONE) != 0) return -1;
  if (madvise(decommit_start, decommit_size, MADV_DONTNEED) != 0) return -1;
#endif
  return 0;
}

#endif

bool fixed_arena_refill(ant_fixed_arena_t *a);
bool fixed_arena_init(ant_fixed_arena_t *a, size_t elem_size, size_t epoch_offset, size_t max_size);

void fixed_arena_destroy(ant_fixed_arena_t *a);
void fixed_arena_trim(ant_fixed_arena_t *a, uint32_t chunk, size_t used);

static inline bool fixed_arena_table_slot(const ant_fixed_arena_t *a, const void *ptr, size_t *slot) {
  uintptr_t index = ((uintptr_t)ptr - a->table_base) >> ANT_ARENA_CHUNK_SHIFT;
  if (index >= a->table_slots) return false;
  *slot = index;
  return (atomic_load_explicit(&a->owners[index], memory_order_relaxed) & ~ANT_ARENA_PARTIAL_CHUNK) == a->id;
}

static inline size_t fixed_arena_chunk_used(const ant_fixed_arena_t *a, uint32_t chunk) {
  return chunk + 1 == a->chunk_count ? (size_t)(a->cursor - a->chunks[chunk].base) : a->chunks[chunk].used;
}

static inline size_t fixed_arena_slots_per_chunk(const ant_fixed_arena_t *a) {
  return a->chunk_bytes / a->elem_size;
}

static inline size_t fixed_arena_extent(const ant_fixed_arena_t *a) {
  if (!a->chunk_count) return 0;
  return (size_t)(a->chunk_count - 1) * a->chunk_bytes + fixed_arena_chunk_used(a, a->chunk_count - 1);
}

static inline void *fixed_arena_alloc_uninit(ant_fixed_arena_t *a) {
  if (a->free_list) {
    void *p = a->free_list;

    a->free_list = *(void **)p;
    a->live_count++;

    return p;
  }

  uint8_t *p = a->cursor;
  if (__builtin_expect((size_t)(a->end - p) < a->elem_size, 0)) {
    if (!fixed_arena_refill(a)) return NULL;
    p = a->cursor;
  }

  a->cursor = p + a->elem_size;
  a->live_count++;

  return p;
}

static inline void *fixed_arena_alloc(ant_fixed_arena_t *a) {
  void *p = fixed_arena_alloc_uninit(a);
  if (p) memset(p, 0, a->elem_size);
  return p;
}

static inline void fixed_arena_free_elem(ant_fixed_arena_t *a, void *p) {
  if (!p) return;
  *(void **)p = a->free_list;
  a->free_list = p;
  if (a->live_count > 0) a->live_count--;
}

static inline bool fixed_arena_owns(const ant_fixed_arena_t *a, const void *ptr) {
  size_t slot;
  return fixed_arena_table_slot(a, ptr, &slot);
}

static inline bool fixed_arena_past_cursor(const ant_fixed_arena_t *a, const void *ptr) {
  return (const uint8_t *)ptr >= a->cursor && (const uint8_t *)ptr < a->last_end;
}

static inline bool fixed_arena_holds_offset(const ant_fixed_arena_t *a, uint64_t offset, const void *ptr) {
  uint32_t owner = atomic_load_explicit(&a->owners[offset >> ANT_ARENA_CHUNK_SHIFT], memory_order_relaxed);
  if (__builtin_expect(owner == a->id, 1)) return true;
  return owner == (a->id | ANT_ARENA_PARTIAL_CHUNK) && !fixed_arena_past_cursor(a, ptr);
}

static inline bool fixed_arena_locate(const ant_fixed_arena_t *a, const void *ptr, uint32_t *chunk, size_t *offset) {
  size_t slot;
  if (!fixed_arena_table_slot(a, ptr, &slot)) return false;

  uint32_t index = atomic_load_explicit(&a->ordinals[slot], memory_order_relaxed);
  if (index >= a->chunk_count) return false;

  size_t off = (size_t)((const uint8_t *)ptr - a->chunks[index].base);
  if (off >= fixed_arena_chunk_used(a, index) || off % a->elem_size != 0) return false;

  *chunk = index;
  *offset = off;

  return true;
}

static inline bool fixed_arena_contains(const ant_fixed_arena_t *a, const void *ptr) {
  uint32_t chunk;
  size_t offset;
  return fixed_arena_locate(a, ptr, &chunk, &offset);
}

static inline uint32_t fixed_arena_slot_index(const ant_fixed_arena_t *a, const void *ptr) {
  uint32_t chunk;
  size_t offset;
  if (!fixed_arena_locate(a, ptr, &chunk, &offset)) return UINT32_MAX;
  return (uint32_t)((size_t)chunk * fixed_arena_slots_per_chunk(a) + offset / a->elem_size);
}

static inline void *fixed_arena_slot_at(const ant_fixed_arena_t *a, uint32_t slot) {
  size_t per_chunk = fixed_arena_slots_per_chunk(a);
  size_t chunk = slot / per_chunk;
  if (chunk >= a->chunk_count) return NULL;

  size_t offset = (slot % per_chunk) * a->elem_size;
  if (offset >= fixed_arena_chunk_used(a, (uint32_t)chunk)) return NULL;

  return a->chunks[chunk].base + offset;
}
