#if !defined(_WIN32)

#include <sys/mman.h>
#include <unistd.h>
#include "heap.h"

size_t jit_heap_page_round(size_t size) {
  static size_t page = 0;
  if (!page) {
    long v = sysconf(_SC_PAGESIZE);
    page = v > 0 ? (size_t)v : 4096u;
  }
  return (size + page - 1) / page * page;
}

void *jit_heap_os_reserve(size_t size) {
  void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  return p == MAP_FAILED ? NULL : p;
}

bool jit_heap_os_commit(void *p, size_t size) {
  (void)p;
  (void)size;
  return true;
}

void jit_heap_os_release(void *p, size_t size) {
  munmap(p, size);
}

jit_heap_chunk_t *jit_heap_chunk_map(void) {
  char *p = jit_heap_os_reserve(2 * JIT_HEAP_CHUNK_SIZE);
  if (!p) return NULL;
  
  char *base = 
    (char *)(((uintptr_t)p + JIT_HEAP_CHUNK_SIZE - 1) & 
    ~(uintptr_t)(JIT_HEAP_CHUNK_SIZE - 1));
  
  if (base > p) munmap(p, (size_t)(base - p));
  char *end = p + 2 * JIT_HEAP_CHUNK_SIZE;
  
  if (end > base + JIT_HEAP_CHUNK_SIZE) 
    munmap(base + JIT_HEAP_CHUNK_SIZE, (size_t)(end - base - JIT_HEAP_CHUNK_SIZE));
  
  jit_heap_chunk_t *c = (jit_heap_chunk_t *)base;
  c->reservation = base;
  c->reservation_size = JIT_HEAP_CHUNK_SIZE;
  
  return c;
}

#endif
