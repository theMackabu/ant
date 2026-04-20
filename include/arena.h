#ifndef ARENA_H
#define ARENA_H

#include "common.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#endif

#define ANT_ARENA_MIN (32 * 1024)
#define ANT_ARENA_MAX (64ULL * 1024 * 1024 * 1024)

#define ANT_ARENA_THRESHOLD   (256ULL * 1024 * 1024)
#define ARENA_GROW_INCREMENT  (8ULL * 1024 * 1024)
#define ANT_CLOSURE_ARENA_MAX (2ULL * 1024 * 1024 * 1024)

// preferred base for mmap on platforms where the kernel may hand out
// addresses above the 47-bit NaN-boxing ceiling. We pick 0x100000000
// inside the 47-bit range and above the typical text/data segments.
#define ANT_MMAP_HINT ((void *)0x100000000ULL)

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

#ifdef _WIN32

static inline void *ant_arena_reserve(size_t max_size) {
  void *p = VirtualAlloc(NULL, max_size, MEM_RESERVE, PAGE_NOACCESS);
  return mantissa_chk(p, "VirtualAlloc");
}

static inline int ant_arena_commit(void *base, size_t old_size, size_t new_size) {
  if (new_size <= old_size) return 0;
  void *p = VirtualAlloc((char *)base + old_size, new_size - old_size, MEM_COMMIT, PAGE_READWRITE);
  return p ? 0 : -1;
}

static inline void ant_arena_free(void *base, size_t reserved_size) {
  if (base) VirtualFree(base, 0, MEM_RELEASE);
}

static inline void *ant_os_alloc(size_t size) {
  return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static inline int ant_arena_decommit(void *base, size_t old_size, size_t new_size) {
  if (new_size >= old_size) return 0;
  void *decommit_start = (char *)base + new_size;
  size_t decommit_size = old_size - new_size;
  return VirtualFree(decommit_start, decommit_size, MEM_DECOMMIT) ? 0 : -1;
}

#else

static inline void *ant_arena_reserve(size_t max_size) {
  void *p = mmap(ANT_MMAP_HINT, max_size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) return NULL;
  if ((uintptr_t)p >> 47) {
    munmap(p, max_size);
    p = mmap(NULL, max_size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return NULL;
  }
  return mantissa_chk(p, "mmap");
}

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

static inline void ant_arena_free(void *base, size_t reserved_size) {
  if (base) munmap(base, reserved_size);
}

static inline void *ant_os_alloc(size_t size) {
  void *p = mmap(ANT_MMAP_HINT, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) return NULL;
  if ((uintptr_t)p >> 47) {
    munmap(p, size);
    p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return NULL;
  }
  return mantissa_chk(p, "mmap");
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
  a->base = (uint8_t *)ant_arena_reserve(max_size);
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
    ant_arena_free(a->base, max_size);
    a->base = NULL;
    return false;
  }
  
  a->committed = initial;
  return true;
}

static inline void fixed_arena_destroy(ant_fixed_arena_t *a) {
  if (a->base) ant_arena_free(a->base, a->reserved);
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
