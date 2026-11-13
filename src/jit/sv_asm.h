#pragma once

#include <mir.h>
#include <stdint.h>

typedef struct {
  MIR_context_t ctx;
  MIR_item_t fn;
} sv_asm_t;

typedef MIR_label_t sv_asm_label_t;

static inline sv_asm_t sv_asm(MIR_context_t ctx, MIR_item_t fn) {
  return (sv_asm_t){ ctx, fn };
}

MIR_reg_t sv_asm_reg(const sv_asm_t *a, MIR_type_t type, const char *prefix, int site);
sv_asm_label_t sv_asm_new_label(const sv_asm_t *a);

void sv_asm_append(const sv_asm_t *a, MIR_insn_t insn);
void sv_asm_bind(const sv_asm_t *a, sv_asm_label_t label);
void sv_asm_jump(const sv_asm_t *a, sv_asm_label_t label);
void sv_asm_bt(const sv_asm_t *a, sv_asm_label_t label, MIR_reg_t cond);
void sv_asm_bf(const sv_asm_t *a, sv_asm_label_t label, MIR_reg_t cond);

void sv_asm_mov_imm(const sv_asm_t *a, MIR_reg_t dst, uint64_t imm);
void sv_asm_f64_mov_imm(const sv_asm_t *a, MIR_reg_t dst, double imm);

void sv_asm_load(const sv_asm_t *a, MIR_type_t type, MIR_reg_t dst, MIR_reg_t base, MIR_disp_t disp);
void sv_asm_store(const sv_asm_t *a, MIR_type_t type, MIR_reg_t base, MIR_disp_t disp, MIR_reg_t src);
void sv_asm_store_imm(const sv_asm_t *a, MIR_type_t type, MIR_reg_t base, MIR_disp_t disp, uint64_t imm);

void sv_asm_load_index(
  const sv_asm_t *a, MIR_type_t type, MIR_reg_t dst,
  MIR_reg_t base, MIR_disp_t disp, MIR_reg_t index, MIR_scale_t scale
);

void sv_asm_store_index(
  const sv_asm_t *a, MIR_type_t type, MIR_reg_t base,
  MIR_disp_t disp, MIR_reg_t index, MIR_scale_t scale, MIR_reg_t src
);

#define SV_ASM_BINARY(name, code, imm_t, imm_op)                                  \
  void sv_asm_##name(const sv_asm_t *a, MIR_reg_t dst, MIR_reg_t x, MIR_reg_t y); \
  void sv_asm_##name##_imm(const sv_asm_t *a, MIR_reg_t dst, MIR_reg_t x, imm_t imm);
#define SV_ASM_BRANCH(name, code, imm_t, imm_op)                                         \
  void sv_asm_##name(const sv_asm_t *a, sv_asm_label_t label, MIR_reg_t x, MIR_reg_t y); \
  void sv_asm_##name##_imm(const sv_asm_t *a, sv_asm_label_t label, MIR_reg_t x, imm_t imm);
#define SV_ASM_UNARY(name, code) \
  void sv_asm_##name(const sv_asm_t *a, MIR_reg_t dst, MIR_reg_t src);
#include "sv_asm_ops.h"
