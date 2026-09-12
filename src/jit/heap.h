#ifndef ANT_JIT_HEAP_H
#define ANT_JIT_HEAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <mir-alloc.h>

#define JIT_HEAP_CHUNK_SIZE (1u << 20)

typedef struct jit_heap_chunk {
  uint64_t magic;
  uint32_t cls, block, live, bump;
  void *free_list;
  struct jit_heap_chunk *prev, *next;
  char *reservation;
  size_t reservation_size;
  bool full;
} jit_heap_chunk_t;

typedef struct MIR_heap *MIR_heap_t;

MIR_heap_t jit_heap_create(void);
MIR_alloc_t jit_heap_mir_alloc(MIR_heap_t h);
void jit_heap_release(MIR_heap_t h);
void jit_heap_destroy(MIR_heap_t h);

size_t jit_heap_page_round(size_t size);
void *jit_heap_os_reserve(size_t size);
bool jit_heap_os_commit(void *p, size_t size);
void jit_heap_os_release(void *p, size_t size);
jit_heap_chunk_t *jit_heap_chunk_map(void);

#endif
