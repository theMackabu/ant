#include "jit_internal.h"

static MIR_op_t sv_asm_r(const sv_asm_t *a, MIR_reg_t reg) {
  return MIR_new_reg_op(a->ctx, reg);
}

static MIR_op_t sv_asm_l(const sv_asm_t *a, sv_asm_label_t label) {
  return MIR_new_label_op(a->ctx, label);
}

static MIR_op_t sv_asm_mem(const sv_asm_t *a, MIR_type_t type, MIR_reg_t base, MIR_disp_t disp) {
  return MIR_new_mem_op(a->ctx, type, disp, base, 0, 1);
}

static MIR_op_t sv_asm_mem_index(
  const sv_asm_t *a, MIR_type_t type, MIR_reg_t base, MIR_disp_t disp, MIR_reg_t index, MIR_scale_t scale
) {
  return MIR_new_mem_op(a->ctx, type, disp, base, index, scale);
}

static MIR_insn_code_t sv_asm_move_code(MIR_type_t type) {
  if (type == MIR_T_D) return MIR_DMOV;
  if (type == MIR_T_F) return MIR_FMOV;
  return type == MIR_T_LD ? MIR_LDMOV : MIR_MOV;
}

static void sv_asm_emit(const sv_asm_t *a, MIR_insn_code_t code, MIR_op_t x, MIR_op_t y) {
  MIR_append_insn(a->ctx, a->fn, MIR_new_insn(a->ctx, code, x, y));
}

static void sv_asm_emit3(const sv_asm_t *a, MIR_insn_code_t code, MIR_op_t x, MIR_op_t y, MIR_op_t z) {
  MIR_append_insn(a->ctx, a->fn, MIR_new_insn(a->ctx, code, x, y, z));
}

MIR_reg_t sv_asm_reg(const sv_asm_t *a, MIR_type_t type, const char *prefix, int site) {
  char name[64];
  snprintf(name, sizeof(name), "%s_%d", prefix, site);
  return MIR_new_func_reg(a->ctx, a->fn->u.func, type, name);
}

sv_asm_label_t sv_asm_new_label(const sv_asm_t *a) {
  return MIR_new_label(a->ctx);
}

void sv_asm_append(const sv_asm_t *a, MIR_insn_t insn) {
  MIR_append_insn(a->ctx, a->fn, insn);
}

void sv_asm_bind(const sv_asm_t *a, sv_asm_label_t label) {
  MIR_append_insn(a->ctx, a->fn, label);
}

void sv_asm_jump(const sv_asm_t *a, sv_asm_label_t label) {
  MIR_append_insn(a->ctx, a->fn, MIR_new_insn(a->ctx, MIR_JMP, sv_asm_l(a, label)));
}

void sv_asm_mov_imm(const sv_asm_t *a, MIR_reg_t dst, uint64_t imm) {
  sv_asm_emit(a, MIR_MOV, sv_asm_r(a, dst), MIR_new_uint_op(a->ctx, imm));
}

void sv_asm_load(const sv_asm_t *a, MIR_type_t type, MIR_reg_t dst, MIR_reg_t base, MIR_disp_t disp) {
  sv_asm_emit(a, sv_asm_move_code(type), sv_asm_r(a, dst), sv_asm_mem(a, type, base, disp));
}

void sv_asm_store(const sv_asm_t *a, MIR_type_t type, MIR_reg_t base, MIR_disp_t disp, MIR_reg_t src) {
  sv_asm_emit(a, sv_asm_move_code(type), sv_asm_mem(a, type, base, disp), sv_asm_r(a, src));
}

void sv_asm_load_index(
  const sv_asm_t *a, MIR_type_t type, MIR_reg_t dst,
  MIR_reg_t base, MIR_disp_t disp, MIR_reg_t index, MIR_scale_t scale
) {
  sv_asm_emit(a, sv_asm_move_code(type), sv_asm_r(a, dst), sv_asm_mem_index(a, type, base, disp, index, scale));
}

void sv_asm_store_index(
  const sv_asm_t *a, MIR_type_t type, MIR_reg_t base,
  MIR_disp_t disp, MIR_reg_t index, MIR_scale_t scale, MIR_reg_t src
) {
  sv_asm_emit(a, sv_asm_move_code(type), sv_asm_mem_index(a, type, base, disp, index, scale), sv_asm_r(a, src));
}

#define SV_ASM_BINARY(name, code, imm_t, imm_op)                                       \
  void sv_asm_##name(const sv_asm_t *a, MIR_reg_t dst, MIR_reg_t x, MIR_reg_t y) {     \
    sv_asm_emit3(a, code, sv_asm_r(a, dst), sv_asm_r(a, x), sv_asm_r(a, y));           \
  }                                                                                    \
  void sv_asm_##name##_imm(const sv_asm_t *a, MIR_reg_t dst, MIR_reg_t x, imm_t imm) { \
    sv_asm_emit3(a, code, sv_asm_r(a, dst), sv_asm_r(a, x), imm_op(a->ctx, imm));      \
  }
#define SV_ASM_BRANCH(name, code, imm_t, imm_op)                                              \
  void sv_asm_##name(const sv_asm_t *a, sv_asm_label_t label, MIR_reg_t x, MIR_reg_t y) {     \
    sv_asm_emit3(a, code, sv_asm_l(a, label), sv_asm_r(a, x), sv_asm_r(a, y));                \
  }                                                                                           \
  void sv_asm_##name##_imm(const sv_asm_t *a, sv_asm_label_t label, MIR_reg_t x, imm_t imm) { \
    sv_asm_emit3(a, code, sv_asm_l(a, label), sv_asm_r(a, x), imm_op(a->ctx, imm));           \
  }
#define SV_ASM_UNARY(name, code)                                        \
  void sv_asm_##name(const sv_asm_t *a, MIR_reg_t dst, MIR_reg_t src) { \
    sv_asm_emit(a, code, sv_asm_r(a, dst), sv_asm_r(a, src));           \
  }
#include "sv_asm_ops.h"
