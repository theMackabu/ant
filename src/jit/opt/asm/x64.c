#include "x64.h"

typedef enum {
  X64_RM_REG,
  X64_RM_MEM,
  X64_RM_RIP,
} x64_rm_kind_t;

typedef struct {
  x64_rm_kind_t kind;
  uint8_t reg;
  asm_address_t mem;
  asm_label_t label;
} x64_rm_t;

typedef enum {
  X64_BYTE_REGS_NO,
  X64_BYTE_REGS_REG,
  X64_BYTE_REGS_RM,
} x64_byte_regs_t;

typedef enum {
  X64_PREFIX_NONE,
  X64_PREFIX_OPERAND_SIZE,
  X64_PREFIX_REP,
  X64_PREFIX_REPNE,
} x64_prefix_t;

static inline x64_rm_t rm_reg(uint8_t reg) {
  return (x64_rm_t){ .kind = X64_RM_REG, .reg = reg };
}

static inline x64_rm_t rm_mem(asm_address_t address) {
  return (x64_rm_t){ .kind = X64_RM_MEM, .mem = address };
}

static inline x64_rm_t rm_rip(asm_label_t label) {
  return (x64_rm_t){ .kind = X64_RM_RIP, .label = label };
}

static inline bool is_w64(asm_width_t width) {
  return width == ASM_W64;
}

static inline bool fits_i8(int64_t value) {
  return value >= INT8_MIN && value <= INT8_MAX;
}

void x64_init(x64_asm_t *a) {
  asm_buffer_init(&a->buf);
}

void x64_free(x64_asm_t *a) {
  asm_buffer_free(&a->buf);
}

asm_error_t x64_finish(x64_asm_t *a, asm_code_t *out) {
  return asm_buffer_finish(&a->buf, out);
}

void x64_data_u8(x64_asm_t *a, uint8_t value) {
  asm_emit_u8(&a->buf, value);
}

void x64_data_u32(x64_asm_t *a, uint32_t value) {
  asm_emit_u32(&a->buf, value);
}

void x64_data_u64(x64_asm_t *a, uint64_t value) {
  asm_emit_u64(&a->buf, value);
}

void x64_data_bytes(x64_asm_t *a, const void *bytes, size_t n) {
  asm_emit_bytes(&a->buf, bytes, n);
}

void x64_align_code(x64_asm_t *a, size_t alignment) {
  ASM_CHECK(alignment && (alignment & (alignment - 1)) == 0, "alignment must be a power of two");
  size_t offset = x64_offset(a);
  x64_nops(a, (alignment - offset % alignment) % alignment);
}

void x64_align_data(x64_asm_t *a, size_t alignment) {
  asm_align_with(&a->buf, alignment, 0);
}

static void emit_rex(x64_asm_t *a, bool w, uint8_t reg, const x64_rm_t *rm, x64_byte_regs_t byte_regs) {
  uint8_t rex = 0x40;
  if (w) rex |= 0x08;
  if (reg & 8) rex |= 0x04;
  switch (rm->kind) {
    case X64_RM_REG:
      if (rm->reg & 8) rex |= 0x01;
      break;
    case X64_RM_MEM:
      if (rm->mem.has_index && (rm->mem.index & 8)) rex |= 0x02;
      if (rm->mem.base & 8) rex |= 0x01;
      break;
    case X64_RM_RIP:
      break;
  }
  bool needs_byte_rex =
    (byte_regs == X64_BYTE_REGS_REG && reg >= 4 && reg < 8) ||
    (byte_regs == X64_BYTE_REGS_RM && rm->kind == X64_RM_REG && rm->reg >= 4 && rm->reg < 8);
  if (rex != 0x40 || needs_byte_rex) asm_emit_u8(&a->buf, rex);
}

static void emit_modrm_rm(x64_asm_t *a, uint8_t reg, const x64_rm_t *rm, size_t trailing_immediate_size) {
  uint8_t r = (uint8_t)((reg & 7) << 3);
  switch (rm->kind) {
    case X64_RM_REG:
      asm_emit_u8(&a->buf, (uint8_t)(0xc0 | r | (rm->reg & 7)));
      return;
    case X64_RM_RIP: {
      asm_emit_u8(&a->buf, (uint8_t)(r | 0x5));
      size_t at = x64_offset(a);
      asm_emit_u32(&a->buf, 0);
      ASM_CHECK(trailing_immediate_size == 0, "RIP-relative operand with trailing immediate");
      asm_add_fixup(&a->buf, rm->label, at, ASM_FIXUP_X86_REL32);
      return;
    }
    case X64_RM_MEM: {
      uint8_t base = rm->mem.base & 7;
      int32_t disp = rm->mem.disp;
      uint8_t mode = disp == 0 && base != 5 ? 0 : fits_i8(disp) ? 1 : 2;
      if (!rm->mem.has_index && base != 4) {
        asm_emit_u8(&a->buf, (uint8_t)(mode << 6 | r | base));
      } else if (!rm->mem.has_index) {
        asm_emit_u8(&a->buf, (uint8_t)(mode << 6 | r | 0x4));
        asm_emit_u8(&a->buf, 0x24);
      } else {
        ASM_CHECK(rm->mem.index != X64_RSP, "rsp cannot be an index register");
        asm_emit_u8(&a->buf, (uint8_t)(mode << 6 | r | 0x4));
        asm_emit_u8(&a->buf, (uint8_t)((unsigned)rm->mem.scale << 6 | (rm->mem.index & 7u) << 3 | base));
      }
      if (mode == 1) asm_emit_u8(&a->buf, (uint8_t)(int8_t)disp);
      else if (mode == 2) asm_emit_u32(&a->buf, (uint32_t)disp);
      return;
    }
  }
}

static void emit_instruction(
  x64_asm_t *a, x64_prefix_t prefix, bool w, const uint8_t *opcode, size_t opcode_len,
  uint8_t reg, x64_rm_t rm, x64_byte_regs_t byte_regs, size_t trailing_immediate_size
) {
  switch (prefix) {
    case X64_PREFIX_NONE: break;
    case X64_PREFIX_OPERAND_SIZE: asm_emit_u8(&a->buf, 0x66); break;
    case X64_PREFIX_REP: asm_emit_u8(&a->buf, 0xf3); break;
    case X64_PREFIX_REPNE: asm_emit_u8(&a->buf, 0xf2); break;
  }
  emit_rex(a, w, reg, &rm, byte_regs);
  asm_emit_bytes(&a->buf, opcode, opcode_len);
  emit_modrm_rm(a, reg, &rm, trailing_immediate_size);
}

#define OP(...) (const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ })

static void op_rr(x64_asm_t *a, asm_width_t width, const uint8_t *opcode, size_t opcode_len, asm_gpr_t reg, asm_gpr_t rm) {
  emit_instruction(a, X64_PREFIX_NONE, is_w64(width), opcode, opcode_len, reg, rm_reg(rm), X64_BYTE_REGS_NO, 0);
}

static void op_rm(x64_asm_t *a, asm_width_t width, const uint8_t *opcode, size_t opcode_len, uint8_t reg, asm_address_t address) {
  emit_instruction(a, X64_PREFIX_NONE, is_w64(width), opcode, opcode_len, reg, rm_mem(address), X64_BYTE_REGS_NO, 0);
}

static void sse_rr(x64_asm_t *a, x64_prefix_t prefix, bool w, const uint8_t *opcode, size_t opcode_len, uint8_t reg, uint8_t rm) {
  emit_instruction(a, prefix, w, opcode, opcode_len, reg, rm_reg(rm), X64_BYTE_REGS_NO, 0);
}

void x64_mov_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_gpr_t src) {
  op_rr(a, width, OP(0x89), src, dst);
}

void x64_mov_rm(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_address_t address) {
  op_rm(a, width, OP(0x8b), dst, address);
}

void x64_mov_mr(x64_asm_t *a, asm_width_t width, asm_address_t address, asm_gpr_t src) {
  op_rm(a, width, OP(0x89), src, address);
}

void x64_mov_ri32(x64_asm_t *a, asm_gpr_t dst, uint32_t imm) {
  x64_rm_t rm = rm_reg(dst);
  emit_rex(a, false, 0, &rm, X64_BYTE_REGS_NO);
  asm_emit_u8(&a->buf, (uint8_t)(0xb8 | (dst & 7)));
  asm_emit_u32(&a->buf, imm);
}

void x64_mov_ri_sign_extended(x64_asm_t *a, asm_gpr_t dst, int32_t imm) {
  emit_instruction(a, X64_PREFIX_NONE, true, OP(0xc7), 0, rm_reg(dst), X64_BYTE_REGS_NO, 4);
  asm_emit_u32(&a->buf, (uint32_t)imm);
}

void x64_movabs(x64_asm_t *a, asm_gpr_t dst, uint64_t imm) {
  x64_rm_t rm = rm_reg(dst);
  emit_rex(a, true, 0, &rm, X64_BYTE_REGS_NO);
  asm_emit_u8(&a->buf, (uint8_t)(0xb8 | (dst & 7)));
  asm_emit_u64(&a->buf, imm);
}

void x64_mov_ri(x64_asm_t *a, asm_gpr_t dst, uint64_t imm) {
  int64_t signed_imm = (int64_t)imm;
  if (imm <= UINT32_MAX) x64_mov_ri32(a, dst, (uint32_t)imm);
  else if (signed_imm >= INT32_MIN && signed_imm <= INT32_MAX) x64_mov_ri_sign_extended(a, dst, (int32_t)signed_imm);
  else x64_movabs(a, dst, imm);
}

void x64_mov_mi(x64_asm_t *a, asm_width_t width, asm_address_t address, int32_t imm) {
  emit_instruction(a, X64_PREFIX_NONE, is_w64(width), OP(0xc7), 0, rm_mem(address), X64_BYTE_REGS_NO, 4);
  asm_emit_u32(&a->buf, (uint32_t)imm);
}

void x64_mov8_mr(x64_asm_t *a, asm_address_t address, asm_gpr_t src) {
  emit_instruction(a, X64_PREFIX_NONE, false, OP(0x88), src, rm_mem(address), X64_BYTE_REGS_REG, 0);
}

void x64_mov16_mr(x64_asm_t *a, asm_address_t address, asm_gpr_t src) {
  emit_instruction(a, X64_PREFIX_OPERAND_SIZE, false, OP(0x89), src, rm_mem(address), X64_BYTE_REGS_NO, 0);
}

void x64_mov8_mi(x64_asm_t *a, asm_address_t address, uint8_t imm) {
  emit_instruction(a, X64_PREFIX_NONE, false, OP(0xc6), 0, rm_mem(address), X64_BYTE_REGS_NO, 1);
  asm_emit_u8(&a->buf, imm);
}

void x64_mov16_mi(x64_asm_t *a, asm_address_t address, uint16_t imm) {
  emit_instruction(a, X64_PREFIX_OPERAND_SIZE, false, OP(0xc7), 0, rm_mem(address), X64_BYTE_REGS_NO, 2);
  asm_emit_u16(&a->buf, imm);
}

void x64_movzx8_rr(x64_asm_t *a, asm_gpr_t dst, asm_gpr_t src) {
  emit_instruction(a, X64_PREFIX_NONE, false, OP(0x0f, 0xb6), dst, rm_reg(src), X64_BYTE_REGS_RM, 0);
}

void x64_movzx8_rm(x64_asm_t *a, asm_gpr_t dst, asm_address_t address) {
  op_rm(a, ASM_W32, OP(0x0f, 0xb6), dst, address);
}

void x64_movzx16_rr(x64_asm_t *a, asm_gpr_t dst, asm_gpr_t src) {
  op_rr(a, ASM_W32, OP(0x0f, 0xb7), dst, src);
}

void x64_movzx16_rm(x64_asm_t *a, asm_gpr_t dst, asm_address_t address) {
  op_rm(a, ASM_W32, OP(0x0f, 0xb7), dst, address);
}

void x64_movsx8_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_gpr_t src) {
  emit_instruction(a, X64_PREFIX_NONE, is_w64(width), OP(0x0f, 0xbe), dst, rm_reg(src), X64_BYTE_REGS_RM, 0);
}

void x64_movsx8_rm(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_address_t address) {
  op_rm(a, width, OP(0x0f, 0xbe), dst, address);
}

void x64_movsx16_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_gpr_t src) {
  op_rr(a, width, OP(0x0f, 0xbf), dst, src);
}

void x64_movsx16_rm(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_address_t address) {
  op_rm(a, width, OP(0x0f, 0xbf), dst, address);
}

void x64_movsxd_rr(x64_asm_t *a, asm_gpr_t dst, asm_gpr_t src) {
  op_rr(a, ASM_W64, OP(0x63), dst, src);
}

void x64_movsxd_rm(x64_asm_t *a, asm_gpr_t dst, asm_address_t address) {
  op_rm(a, ASM_W64, OP(0x63), dst, address);
}

void x64_lea(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_address_t address) {
  op_rm(a, width, OP(0x8d), dst, address);
}

void x64_mov_r_label(x64_asm_t *a, asm_gpr_t dst, asm_label_t label) {
  emit_instruction(a, X64_PREFIX_NONE, true, OP(0x8b), dst, rm_rip(label), X64_BYTE_REGS_NO, 0);
}

void x64_cmp_r_label(x64_asm_t *a, asm_gpr_t lhs, asm_label_t label) {
  emit_instruction(a, X64_PREFIX_NONE, true, OP(0x3b), lhs, rm_rip(label), X64_BYTE_REGS_NO, 0);
}

void x64_alu_r_label(x64_asm_t *a, x64_alu_t op, asm_gpr_t lhs, asm_label_t label) {
  emit_instruction(a, X64_PREFIX_NONE, true, OP((uint8_t)(op << 3 | 0x03)), lhs, rm_rip(label), X64_BYTE_REGS_NO, 0);
}

void x64_lea_label(x64_asm_t *a, asm_gpr_t dst, asm_label_t label) {
  emit_instruction(a, X64_PREFIX_NONE, true, OP(0x8d), dst, rm_rip(label), X64_BYTE_REGS_NO, 0);
}

void x64_xchg_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t x, asm_gpr_t y) {
  op_rr(a, width, OP(0x87), y, x);
}

void x64_alu_rr(x64_asm_t *a, x64_alu_t op, asm_width_t width, asm_gpr_t dst, asm_gpr_t src) {
  op_rr(a, width, OP((uint8_t)(op << 3 | 0x01)), src, dst);
}

static void alu_imm(x64_asm_t *a, x64_alu_t op, asm_width_t width, x64_rm_t rm, int32_t imm) {
  if (fits_i8(imm)) {
    emit_instruction(a, X64_PREFIX_NONE, is_w64(width), OP(0x83), op, rm, X64_BYTE_REGS_NO, 1);
    asm_emit_u8(&a->buf, (uint8_t)(int8_t)imm);
  } else {
    emit_instruction(a, X64_PREFIX_NONE, is_w64(width), OP(0x81), op, rm, X64_BYTE_REGS_NO, 4);
    asm_emit_u32(&a->buf, (uint32_t)imm);
  }
}

void x64_alu_ri(x64_asm_t *a, x64_alu_t op, asm_width_t width, asm_gpr_t dst, int32_t imm) {
  alu_imm(a, op, width, rm_reg(dst), imm);
}

void x64_alu_rm(x64_asm_t *a, x64_alu_t op, asm_width_t width, asm_gpr_t dst, asm_address_t address) {
  op_rm(a, width, OP((uint8_t)(op << 3 | 0x03)), dst, address);
}

void x64_alu_mr(x64_asm_t *a, x64_alu_t op, asm_width_t width, asm_address_t address, asm_gpr_t src) {
  op_rm(a, width, OP((uint8_t)(op << 3 | 0x01)), src, address);
}

void x64_alu_mi(x64_asm_t *a, x64_alu_t op, asm_width_t width, asm_address_t address, int32_t imm) {
  alu_imm(a, op, width, rm_mem(address), imm);
}

void x64_add_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_gpr_t src) { x64_alu_rr(a, X64_ALU_ADD, width, dst, src); }
void x64_add_ri(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, int32_t imm) { x64_alu_ri(a, X64_ALU_ADD, width, dst, imm); }
void x64_sub_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_gpr_t src) { x64_alu_rr(a, X64_ALU_SUB, width, dst, src); }
void x64_sub_ri(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, int32_t imm) { x64_alu_ri(a, X64_ALU_SUB, width, dst, imm); }
void x64_and_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_gpr_t src) { x64_alu_rr(a, X64_ALU_AND, width, dst, src); }
void x64_and_ri(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, int32_t imm) { x64_alu_ri(a, X64_ALU_AND, width, dst, imm); }
void x64_or_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_gpr_t src) { x64_alu_rr(a, X64_ALU_OR, width, dst, src); }
void x64_or_ri(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, int32_t imm) { x64_alu_ri(a, X64_ALU_OR, width, dst, imm); }
void x64_xor_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_gpr_t src) { x64_alu_rr(a, X64_ALU_XOR, width, dst, src); }
void x64_xor_ri(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, int32_t imm) { x64_alu_ri(a, X64_ALU_XOR, width, dst, imm); }
void x64_cmp_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t lhs, asm_gpr_t rhs) { x64_alu_rr(a, X64_ALU_CMP, width, lhs, rhs); }
void x64_cmp_ri(x64_asm_t *a, asm_width_t width, asm_gpr_t lhs, int32_t imm) { x64_alu_ri(a, X64_ALU_CMP, width, lhs, imm); }

void x64_cmp_mr(x64_asm_t *a, asm_width_t width, asm_address_t address, asm_gpr_t rhs) {
  x64_alu_mr(a, X64_ALU_CMP, width, address, rhs);
}

void x64_cmp_mi(x64_asm_t *a, asm_width_t width, asm_address_t address, int32_t imm) {
  x64_alu_mi(a, X64_ALU_CMP, width, address, imm);
}

void x64_cmp16_mi8(x64_asm_t *a, asm_address_t address, int8_t imm) {
  emit_instruction(a, X64_PREFIX_OPERAND_SIZE, false, OP(0x83), X64_ALU_CMP, rm_mem(address), X64_BYTE_REGS_NO, 1);
  asm_emit_u8(&a->buf, (uint8_t)imm);
}

void x64_test_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t x, asm_gpr_t y) {
  op_rr(a, width, OP(0x85), y, x);
}

void x64_test_ri(x64_asm_t *a, asm_width_t width, asm_gpr_t reg, int32_t imm) {
  emit_instruction(a, X64_PREFIX_NONE, is_w64(width), OP(0xf7), 0, rm_reg(reg), X64_BYTE_REGS_NO, 4);
  asm_emit_u32(&a->buf, (uint32_t)imm);
}

void x64_test_mi(x64_asm_t *a, asm_width_t width, asm_address_t address, int32_t imm) {
  emit_instruction(a, X64_PREFIX_NONE, is_w64(width), OP(0xf7), 0, rm_mem(address), X64_BYTE_REGS_NO, 4);
  asm_emit_u32(&a->buf, (uint32_t)imm);
}

void x64_test8_mi(x64_asm_t *a, asm_address_t address, uint8_t imm) {
  emit_instruction(a, X64_PREFIX_NONE, false, OP(0xf6), 0, rm_mem(address), X64_BYTE_REGS_NO, 1);
  asm_emit_u8(&a->buf, imm);
}

void x64_neg(x64_asm_t *a, asm_width_t width, asm_gpr_t reg) {
  emit_instruction(a, X64_PREFIX_NONE, is_w64(width), OP(0xf7), 3, rm_reg(reg), X64_BYTE_REGS_NO, 0);
}

void x64_not(x64_asm_t *a, asm_width_t width, asm_gpr_t reg) {
  emit_instruction(a, X64_PREFIX_NONE, is_w64(width), OP(0xf7), 2, rm_reg(reg), X64_BYTE_REGS_NO, 0);
}

void x64_shift_ri(x64_asm_t *a, x64_shift_t op, asm_width_t width, asm_gpr_t reg, uint8_t amount) {
  if (amount == 1) {
    emit_instruction(a, X64_PREFIX_NONE, is_w64(width), OP(0xd1), op, rm_reg(reg), X64_BYTE_REGS_NO, 0);
  } else {
    emit_instruction(a, X64_PREFIX_NONE, is_w64(width), OP(0xc1), op, rm_reg(reg), X64_BYTE_REGS_NO, 1);
    asm_emit_u8(&a->buf, amount);
  }
}

void x64_shift_rcl(x64_asm_t *a, x64_shift_t op, asm_width_t width, asm_gpr_t reg) {
  emit_instruction(a, X64_PREFIX_NONE, is_w64(width), OP(0xd3), op, rm_reg(reg), X64_BYTE_REGS_NO, 0);
}

void x64_shl_ri(x64_asm_t *a, asm_width_t width, asm_gpr_t reg, uint8_t amount) { x64_shift_ri(a, X64_SHIFT_SHL, width, reg, amount); }
void x64_shr_ri(x64_asm_t *a, asm_width_t width, asm_gpr_t reg, uint8_t amount) { x64_shift_ri(a, X64_SHIFT_SHR, width, reg, amount); }
void x64_sar_ri(x64_asm_t *a, asm_width_t width, asm_gpr_t reg, uint8_t amount) { x64_shift_ri(a, X64_SHIFT_SAR, width, reg, amount); }

void x64_imul_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_gpr_t src) {
  op_rr(a, width, OP(0x0f, 0xaf), dst, src);
}

void x64_imul_rri(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_gpr_t src, int32_t imm) {
  if (fits_i8(imm)) {
    emit_instruction(a, X64_PREFIX_NONE, is_w64(width), OP(0x6b), dst, rm_reg(src), X64_BYTE_REGS_NO, 1);
    asm_emit_u8(&a->buf, (uint8_t)(int8_t)imm);
  } else {
    emit_instruction(a, X64_PREFIX_NONE, is_w64(width), OP(0x69), dst, rm_reg(src), X64_BYTE_REGS_NO, 4);
    asm_emit_u32(&a->buf, (uint32_t)imm);
  }
}

void x64_sign_extend_rax_into_rdx(x64_asm_t *a, asm_width_t width) {
  if (is_w64(width)) asm_emit_u8(&a->buf, 0x48);
  asm_emit_u8(&a->buf, 0x99);
}

void x64_idiv(x64_asm_t *a, asm_width_t width, asm_gpr_t divisor) {
  emit_instruction(a, X64_PREFIX_NONE, is_w64(width), OP(0xf7), 7, rm_reg(divisor), X64_BYTE_REGS_NO, 0);
}

void x64_setcc(x64_asm_t *a, x64_cond_t cond, asm_gpr_t dst) {
  emit_instruction(a, X64_PREFIX_NONE, false, OP(0x0f, (uint8_t)(0x90 | cond)), 0, rm_reg(dst), X64_BYTE_REGS_RM, 0);
}

void x64_cmovcc(x64_asm_t *a, x64_cond_t cond, asm_width_t width, asm_gpr_t dst, asm_gpr_t src) {
  op_rr(a, width, OP(0x0f, (uint8_t)(0x40 | cond)), dst, src);
}

static bool backward_rel8_reaches(const x64_asm_t *a, asm_label_t label, size_t instruction_size) {
  size_t target = x64_label_offset(a, label);
  return target != ASM_UNBOUND && asm_fixup_reaches(ASM_FIXUP_X86_REL8, x64_offset(a) + instruction_size - 1, target);
}

static void emit_rel8(x64_asm_t *a, asm_label_t label) {
  size_t at = x64_offset(a);
  asm_emit_u8(&a->buf, 0);
  asm_add_fixup(&a->buf, label, at, ASM_FIXUP_X86_REL8);
}

static void emit_rel32(x64_asm_t *a, asm_label_t label) {
  size_t at = x64_offset(a);
  asm_emit_u32(&a->buf, 0);
  asm_add_fixup(&a->buf, label, at, ASM_FIXUP_X86_REL32);
}

void x64_jcc(x64_asm_t *a, x64_cond_t cond, asm_label_t label) {
  if (backward_rel8_reaches(a, label, 2)) {
    x64_jcc_short(a, cond, label);
    return;
  }
  asm_emit_u8(&a->buf, 0x0f);
  asm_emit_u8(&a->buf, (uint8_t)(0x80 | cond));
  emit_rel32(a, label);
}

void x64_jcc_short(x64_asm_t *a, x64_cond_t cond, asm_label_t label) {
  asm_emit_u8(&a->buf, (uint8_t)(0x70 | cond));
  emit_rel8(a, label);
}

void x64_jmp(x64_asm_t *a, asm_label_t label) {
  if (backward_rel8_reaches(a, label, 2)) {
    x64_jmp_short(a, label);
    return;
  }
  asm_emit_u8(&a->buf, 0xe9);
  emit_rel32(a, label);
}

void x64_jmp_short(x64_asm_t *a, asm_label_t label) {
  asm_emit_u8(&a->buf, 0xeb);
  emit_rel8(a, label);
}

void x64_call(x64_asm_t *a, asm_label_t label) {
  asm_emit_u8(&a->buf, 0xe8);
  emit_rel32(a, label);
}

void x64_call_r(x64_asm_t *a, asm_gpr_t target) {
  emit_instruction(a, X64_PREFIX_NONE, false, OP(0xff), 2, rm_reg(target), X64_BYTE_REGS_NO, 0);
}

void x64_call_m(x64_asm_t *a, asm_address_t address) {
  emit_instruction(a, X64_PREFIX_NONE, false, OP(0xff), 2, rm_mem(address), X64_BYTE_REGS_NO, 0);
}

void x64_jmp_r(x64_asm_t *a, asm_gpr_t target) {
  emit_instruction(a, X64_PREFIX_NONE, false, OP(0xff), 4, rm_reg(target), X64_BYTE_REGS_NO, 0);
}

void x64_call_label_indirect(x64_asm_t *a, asm_label_t label) {
  emit_instruction(a, X64_PREFIX_NONE, false, OP(0xff), 2, rm_rip(label), X64_BYTE_REGS_NO, 0);
}

void x64_jmp_m(x64_asm_t *a, asm_address_t address) {
  emit_instruction(a, X64_PREFIX_NONE, false, OP(0xff), 4, rm_mem(address), X64_BYTE_REGS_NO, 0);
}

void x64_push(x64_asm_t *a, asm_gpr_t reg) {
  x64_rm_t rm = rm_reg(reg);
  emit_rex(a, false, 0, &rm, X64_BYTE_REGS_NO);
  asm_emit_u8(&a->buf, (uint8_t)(0x50 | (reg & 7)));
}

void x64_pop(x64_asm_t *a, asm_gpr_t reg) {
  x64_rm_t rm = rm_reg(reg);
  emit_rex(a, false, 0, &rm, X64_BYTE_REGS_NO);
  asm_emit_u8(&a->buf, (uint8_t)(0x58 | (reg & 7)));
}

void x64_ret(x64_asm_t *a) {
  asm_emit_u8(&a->buf, 0xc3);
}

void x64_nop(x64_asm_t *a) {
  asm_emit_u8(&a->buf, 0x90);
}

void x64_nops(x64_asm_t *a, size_t size) {
  static const uint8_t nops[9][9] = {
    { 0x90 },
    { 0x66, 0x90 },
    { 0x0f, 0x1f, 0x00 },
    { 0x0f, 0x1f, 0x40, 0x00 },
    { 0x0f, 0x1f, 0x44, 0x00, 0x00 },
    { 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00 },
    { 0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00 },
    { 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
  };
  while (size > 0) {
    size_t chunk = size < 9 ? size : 9;
    asm_emit_bytes(&a->buf, nops[chunk - 1], chunk);
    size -= chunk;
  }
}

void x64_int3(x64_asm_t *a) {
  asm_emit_u8(&a->buf, 0xcc);
}

void x64_ud2(x64_asm_t *a) {
  asm_emit_u8(&a->buf, 0x0f);
  asm_emit_u8(&a->buf, 0x0b);
}

void x64_movsd_rr(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src) {
  sse_rr(a, X64_PREFIX_REPNE, false, OP(0x0f, 0x10), dst, src);
}

void x64_movapd_rr(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src) {
  sse_rr(a, X64_PREFIX_OPERAND_SIZE, false, OP(0x0f, 0x28), dst, src);
}

void x64_movsd_rm(x64_asm_t *a, asm_fpr_t dst, asm_address_t address) {
  emit_instruction(a, X64_PREFIX_REPNE, false, OP(0x0f, 0x10), dst, rm_mem(address), X64_BYTE_REGS_NO, 0);
}

void x64_movsd_r_label(x64_asm_t *a, asm_fpr_t dst, asm_label_t label) {
  emit_instruction(a, X64_PREFIX_REPNE, false, OP(0x0f, 0x10), dst, rm_rip(label), X64_BYTE_REGS_NO, 0);
}

void x64_movsd_mr(x64_asm_t *a, asm_address_t address, asm_fpr_t src) {
  emit_instruction(a, X64_PREFIX_REPNE, false, OP(0x0f, 0x11), src, rm_mem(address), X64_BYTE_REGS_NO, 0);
}

void x64_movups_r_label(x64_asm_t *a, asm_fpr_t dst, asm_label_t label) {
  emit_instruction(a, X64_PREFIX_NONE, false, OP(0x0f, 0x10), dst, rm_rip(label), X64_BYTE_REGS_NO, 0);
}

void x64_movups_mr(x64_asm_t *a, asm_address_t address, asm_fpr_t src) {
  emit_instruction(a, X64_PREFIX_NONE, false, OP(0x0f, 0x11), src, rm_mem(address), X64_BYTE_REGS_NO, 0);
}

void x64_movq_xr(x64_asm_t *a, asm_fpr_t dst, asm_gpr_t src) {
  sse_rr(a, X64_PREFIX_OPERAND_SIZE, true, OP(0x0f, 0x6e), dst, src);
}

void x64_movq_rx(x64_asm_t *a, asm_gpr_t dst, asm_fpr_t src) {
  sse_rr(a, X64_PREFIX_OPERAND_SIZE, true, OP(0x0f, 0x7e), src, dst);
}

void x64_addsd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src) { sse_rr(a, X64_PREFIX_REPNE, false, OP(0x0f, 0x58), dst, src); }
void x64_mulsd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src) { sse_rr(a, X64_PREFIX_REPNE, false, OP(0x0f, 0x59), dst, src); }
void x64_subsd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src) { sse_rr(a, X64_PREFIX_REPNE, false, OP(0x0f, 0x5c), dst, src); }
void x64_divsd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src) { sse_rr(a, X64_PREFIX_REPNE, false, OP(0x0f, 0x5e), dst, src); }
void x64_sqrtsd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src) { sse_rr(a, X64_PREFIX_REPNE, false, OP(0x0f, 0x51), dst, src); }

void x64_roundsd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src, uint8_t mode) {
  emit_instruction(a, X64_PREFIX_OPERAND_SIZE, false, OP(0x0f, 0x3a, 0x0b), dst, rm_reg(src), X64_BYTE_REGS_NO, 1);
  asm_emit_u8(&a->buf, mode);
}

void x64_ucomisd(x64_asm_t *a, asm_fpr_t lhs, asm_fpr_t rhs) {
  sse_rr(a, X64_PREFIX_OPERAND_SIZE, false, OP(0x0f, 0x2e), lhs, rhs);
}

void x64_cvtsi2sd(x64_asm_t *a, asm_width_t width, asm_fpr_t dst, asm_gpr_t src) {
  sse_rr(a, X64_PREFIX_REPNE, is_w64(width), OP(0x0f, 0x2a), dst, src);
}

void x64_cvttsd2si(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_fpr_t src) {
  sse_rr(a, X64_PREFIX_REPNE, is_w64(width), OP(0x0f, 0x2c), dst, src);
}

void x64_xorpd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src) { sse_rr(a, X64_PREFIX_OPERAND_SIZE, false, OP(0x0f, 0x57), dst, src); }
void x64_andpd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src) { sse_rr(a, X64_PREFIX_OPERAND_SIZE, false, OP(0x0f, 0x54), dst, src); }
void x64_orpd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src) { sse_rr(a, X64_PREFIX_OPERAND_SIZE, false, OP(0x0f, 0x56), dst, src); }
void x64_minsd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src) { sse_rr(a, X64_PREFIX_REPNE, false, OP(0x0f, 0x5d), dst, src); }
void x64_maxsd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src) { sse_rr(a, X64_PREFIX_REPNE, false, OP(0x0f, 0x5f), dst, src); }

void x64_movss_rm(x64_asm_t *a, asm_fpr_t dst, asm_address_t address) {
  emit_instruction(a, X64_PREFIX_REP, false, OP(0x0f, 0x10), dst, rm_mem(address), X64_BYTE_REGS_NO, 0);
}

void x64_movss_mr(x64_asm_t *a, asm_address_t address, asm_fpr_t src) {
  emit_instruction(a, X64_PREFIX_REP, false, OP(0x0f, 0x11), src, rm_mem(address), X64_BYTE_REGS_NO, 0);
}

void x64_cvtss2sd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src) { sse_rr(a, X64_PREFIX_REP, false, OP(0x0f, 0x5a), dst, src); }
void x64_cvtsd2ss(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src) { sse_rr(a, X64_PREFIX_REPNE, false, OP(0x0f, 0x5a), dst, src); }

void x64_movmskpd(x64_asm_t *a, asm_gpr_t dst, asm_fpr_t src) {
  sse_rr(a, X64_PREFIX_OPERAND_SIZE, false, OP(0x0f, 0x50), dst, src);
}
