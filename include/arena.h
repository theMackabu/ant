#ifndef ARENA_H
#define ARENA_H

#include "cage.h"
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
  size_t committed;
  size_t reserved;
  size_t watermark;
  size_t live_count;
  size_t elem_size;
  size_t epoch_offset;
  void *free_list;
} ant_fixed_arena_t;

#ifdef ANT_WASM_EMBED
static constexpr size_t ANT_ARENA_MAX         = 8ULL * 1024 * 1024;
static constexpr size_t ANT_CLOSURE_ARENA_MAX = 4ULL * 1024 * 1024;
static constexpr size_t ANT_UPVALUE_ARENA_MAX = 2ULL * 1024 * 1024;
static constexpr size_t ARENA_GROW_INCREMENT  = 1ULL * 1024 * 1024;
#else
static constexpr size_t ANT_ARENA_MAX         = 4ULL * 1024 * 1024 * 1024;
static constexpr size_t ANT_CLOSURE_ARENA_MAX = 512ULL * 1024 * 1024;
static constexpr size_t ANT_UPVALUE_ARENA_MAX = 512ULL * 1024 * 1024;
static constexpr size_t ARENA_GROW_INCREMENT  = 8ULL * 1024 * 1024;
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

static inline bool fixed_arena_init(ant_fixed_arena_t *a, size_t elem_size, size_t epoch_offset, size_t max_size) {
  a->base = (uint8_t *)ant_cage_reserve(max_size, ant_arena_page_size());
  if (!a->base) return false;
  
  a->reserved = max_size;
  a->watermark = 0;
  a->live_count = 0;
  a->elem_size = elem_size;
  a->epoch_offset = epoch_offset;
  a->free_list = NULL;
  
  size_t initial = 2ULL * 1024 * 1024;
  if (initial > max_size) initial = max_size;
  
  if (ant_arena_commit(a->base, 0, initial) != 0) {
    ant_cage_free(a->base, max_size);
    a->base = NULL;
    return false;
  }
  
  a->committed = initial;
  return true;
}

static inline void fixed_arena_destroy(ant_fixed_arena_t *a) {
  if (a->base) ant_cage_free(a->base, a->reserved);
  a->base = NULL;
  a->committed = 0;
  a->reserved = 0;
  a->watermark = 0;
  a->live_count = 0;
  a->free_list = NULL;
}

static inline void *fixed_arena_alloc(ant_fixed_arena_t *a) {
  if (a->free_list) {
    void *p = a->free_list;
    a->free_list = *(void **)p;
    memset(p, 0, a->elem_size);
    a->live_count++;
    return p;
  }

  size_t needed = a->watermark + a->elem_size;
  if (needed > a->committed) {
    size_t grow = a->committed / 4;
    if (grow < (64ULL * 1024)) grow = 64ULL * 1024;
    if (grow > ARENA_GROW_INCREMENT) grow = ARENA_GROW_INCREMENT;
    size_t new_committed = a->committed + grow;
    if (new_committed > a->reserved) return NULL;
    if (ant_arena_commit(a->base, a->committed, new_committed) != 0) return NULL;
    a->committed = new_committed;
  }

  void *p = a->base + a->watermark;
  a->watermark = needed;
  memset(p, 0, a->elem_size);
  a->live_count++;
  return p;
}

static inline void *fixed_arena_alloc_uninit(ant_fixed_arena_t *a) {
  if (a->free_list) {
    void *p = a->free_list;
    a->free_list = *(void **)p;
    a->live_count++;
    return p;
  }

  size_t needed = a->watermark + a->elem_size;
  if (needed > a->committed) {
    size_t grow = a->committed / 4;
    if (grow < (64ULL * 1024)) grow = 64ULL * 1024;
    if (grow > ARENA_GROW_INCREMENT) grow = ARENA_GROW_INCREMENT;
    size_t new_committed = a->committed + grow;
    if (new_committed > a->reserved) return NULL;
    if (ant_arena_commit(a->base, a->committed, new_committed) != 0) return NULL;
    a->committed = new_committed;
  }

  void *p = a->base + a->watermark;
  a->watermark = needed;
  a->live_count++;
  
  return p;
}

static inline void fixed_arena_free_elem(ant_fixed_arena_t *a, void *p) {
  if (!p) return;
  *(void **)p = a->free_list;
  a->free_list = p;
  if (a->live_count > 0) a->live_count--;
}

static inline bool fixed_arena_contains(const ant_fixed_arena_t *a, const void *ptr) {
  uintptr_t p = (uintptr_t)ptr;
  uintptr_t lo = (uintptr_t)a->base;
  if (p < lo || p >= lo + a->watermark) return false;
  return ((p - lo) % a->elem_size) == 0;
}

#endif
