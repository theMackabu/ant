#ifndef ANT_JIT_OPT_ASM_DISASM_H
#define ANT_JIT_OPT_ASM_DISASM_H

#include "asm.h"
#include <stdio.h>

typedef struct {
  size_t offset;
  size_t length;
  char text[96];
  bool has_constant;
  uint64_t constant;
  bool has_pc_relative_target;
  size_t pc_relative_target;
} asm_insn_t;

typedef struct {
  asm_insn_t *items;
  size_t count, cap;
} asm_listing_t;

void asm_listing_with_data(asm_listing_t *out, asm_arch_t arch, const uint8_t *code, size_t len, size_t data_offset);

static inline void asm_listing(asm_listing_t *out, asm_arch_t arch, const uint8_t *code, size_t len) {
  asm_listing_with_data(out, arch, code, len, len);
}

void asm_listing_free(asm_listing_t *listing);
void asm_listing_print(FILE *out, const asm_listing_t *listing);

size_t asm_disasm_x64(const uint8_t *code, size_t len, size_t offset, asm_insn_t *out);
size_t asm_disasm_a64(const uint8_t *code, size_t len, size_t offset, asm_insn_t *out);

#endif
