#ifndef ARENA_H
#define ARENA_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#endif

#define ANT_ARENA_MIN (32 * 1024)
#define ANT_ARENA_MAX (256ULL * 1024 * 1024 * 1024)

#define ANT_ARENA_THRESHOLD (256ULL * 1024 * 1024)
#define ARENA_GROW_INCREMENT (8ULL * 1024 * 1024)

static inline void *mantissa_chk(void *p, const char *func) {
  if (p && ((uintptr_t)p >> 48) != 0) {
    fprintf(stderr, 
      "FATAL: %s returned pointer %p outside 48-bit NaN-boxing range\n"
      "Please report this issue with your OS/architecture details.\n", func, p
    ); abort();
  }
  
  return p;
}

#define ant_calloc(size) mantissa_chk(calloc(1, size), "calloc")
#define ant_realloc(ptr, size) mantissa_chk(realloc(ptr, size), "realloc")

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
  (void)reserved_size;
  if (base) VirtualFree(base, 0, MEM_RELEASE);
}

static inline int ant_arena_decommit(void *base, size_t old_size, size_t new_size) {
  if (new_size >= old_size) return 0;
  void *decommit_start = (char *)base + new_size;
  size_t decommit_size = old_size - new_size;
  return VirtualFree(decommit_start, decommit_size, MEM_DECOMMIT) ? 0 : -1;
}

#else

static inline void *ant_arena_reserve(size_t max_size) {
  void *p = mmap(NULL, max_size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) return NULL;
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
  return mprotect((char *)base + old_pages, new_pages - old_pages, PROT_READ | PROT_WRITE);
}

static inline void ant_arena_free(void *base, size_t reserved_size) {
  if (base) munmap(base, reserved_size);
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
  
  if (mprotect(decommit_start, decommit_size, PROT_NONE) == 0) return 0;
  madvise(decommit_start, decommit_size, MADV_DONTNEED);
  return 0;
}

#endif
#endif