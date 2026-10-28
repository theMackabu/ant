#ifndef ANT_JIT_OPT_ASM_EXECUTABLE_H
#define ANT_JIT_OPT_ASM_EXECUTABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct asm_code_heap asm_code_heap_t;

typedef struct {
  void *address;
  size_t size;
  asm_code_heap_t *heap;
} asm_executable_t;

typedef struct {
  size_t mapped;
  size_t live;
  uint32_t chunks;
  uint32_t blocks;
} asm_code_heap_stats_t;

bool asm_executable_init(asm_executable_t *code, const uint8_t *bytes, size_t len);
void asm_executable_free(asm_executable_t *code);
bool asm_executable_patch(asm_executable_t *code, size_t offset, const uint8_t *bytes, size_t len);

asm_code_heap_t *asm_code_heap_new(void);
void asm_code_heap_destroy(asm_code_heap_t *heap);
bool asm_code_heap_place(asm_code_heap_t *heap, asm_executable_t *code, const uint8_t *bytes, size_t len);
asm_code_heap_stats_t asm_code_heap_stats(const asm_code_heap_t *heap);

#endif
