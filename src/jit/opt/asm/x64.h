#ifndef ANT_JIT_OPT_ASM_X64_H
#define ANT_JIT_OPT_ASM_X64_H

#include "buffer.h"

static constexpr asm_gpr_t X64_RAX = 0;
static constexpr asm_gpr_t X64_RCX = 1;
static constexpr asm_gpr_t X64_RDX = 2;
static constexpr asm_gpr_t X64_RBX = 3;
static constexpr asm_gpr_t X64_RSP = 4;
static constexpr asm_gpr_t X64_RBP = 5;
static constexpr asm_gpr_t X64_RSI = 6;
static constexpr asm_gpr_t X64_RDI = 7;
static constexpr asm_gpr_t X64_R8 = 8;
static constexpr asm_gpr_t X64_R9 = 9;
static constexpr asm_gpr_t X64_R10 = 10;
static constexpr asm_gpr_t X64_R11 = 11;
static constexpr asm_gpr_t X64_R12 = 12;
static constexpr asm_gpr_t X64_R13 = 13;
static constexpr asm_gpr_t X64_R14 = 14;
static constexpr asm_gpr_t X64_R15 = 15;

static constexpr asm_fpr_t X64_XMM0 = 0;
static constexpr asm_fpr_t X64_XMM1 = 1;
static constexpr asm_fpr_t X64_XMM2 = 2;
static constexpr asm_fpr_t X64_XMM3 = 3;
static constexpr asm_fpr_t X64_XMM4 = 4;
static constexpr asm_fpr_t X64_XMM5 = 5;
static constexpr asm_fpr_t X64_XMM6 = 6;
static constexpr asm_fpr_t X64_XMM7 = 7;
static constexpr asm_fpr_t X64_XMM8 = 8;
static constexpr asm_fpr_t X64_XMM9 = 9;
static constexpr asm_fpr_t X64_XMM10 = 10;
static constexpr asm_fpr_t X64_XMM11 = 11;
static constexpr asm_fpr_t X64_XMM12 = 12;
static constexpr asm_fpr_t X64_XMM13 = 13;
static constexpr asm_fpr_t X64_XMM14 = 14;
static constexpr asm_fpr_t X64_XMM15 = 15;

typedef enum : uint8_t {
  X64_COND_OVERFLOW = 0x0,
  X64_COND_NO_OVERFLOW = 0x1,
  X64_COND_BELOW = 0x2,
  X64_COND_ABOVE_OR_EQUAL = 0x3,
  X64_COND_EQUAL = 0x4,
  X64_COND_NOT_EQUAL = 0x5,
  X64_COND_BELOW_OR_EQUAL = 0x6,
  X64_COND_ABOVE = 0x7,
  X64_COND_SIGN = 0x8,
  X64_COND_NOT_SIGN = 0x9,
  X64_COND_PARITY = 0xa,
  X64_COND_NOT_PARITY = 0xb,
  X64_COND_LESS = 0xc,
  X64_COND_GREATER_OR_EQUAL = 0xd,
  X64_COND_LESS_OR_EQUAL = 0xe,
  X64_COND_GREATER = 0xf,
} x64_cond_t;

static inline x64_cond_t x64_cond_invert(x64_cond_t cond) {
  return (x64_cond_t)(cond ^ 1);
}

typedef enum : uint8_t {
  X64_ALU_ADD = 0,
  X64_ALU_OR = 1,
  X64_ALU_ADC = 2,
  X64_ALU_SBB = 3,
  X64_ALU_AND = 4,
  X64_ALU_SUB = 5,
  X64_ALU_XOR = 6,
  X64_ALU_CMP = 7,
} x64_alu_t;

typedef enum : uint8_t {
  X64_SHIFT_ROL = 0,
  X64_SHIFT_ROR = 1,
  X64_SHIFT_SHL = 4,
  X64_SHIFT_SHR = 5,
  X64_SHIFT_SAR = 7,
} x64_shift_t;

typedef struct {
  asm_buffer_t buf;
} x64_asm_t;

void x64_init(x64_asm_t *a);
void x64_free(x64_asm_t *a);

static inline size_t x64_offset(const x64_asm_t *a) { return asm_buffer_offset(&a->buf); }
static inline asm_label_t x64_new_label(x64_asm_t *a) { return asm_new_label(&a->buf); }
static inline void x64_bind(x64_asm_t *a, asm_label_t label) { asm_bind(&a->buf, label); }
static inline size_t x64_label_offset(const x64_asm_t *a, asm_label_t label) { return asm_label_offset(&a->buf, label); }

asm_error_t x64_finish(x64_asm_t *a, asm_code_t *out);

void x64_data_u8(x64_asm_t *a, uint8_t value);
void x64_data_u32(x64_asm_t *a, uint32_t value);
void x64_data_u64(x64_asm_t *a, uint64_t value);
void x64_data_bytes(x64_asm_t *a, const void *bytes, size_t n);
void x64_align_code(x64_asm_t *a, size_t alignment);
void x64_align_data(x64_asm_t *a, size_t alignment);

void x64_mov_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_gpr_t src);
void x64_mov_rm(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_address_t address);
void x64_mov_mr(x64_asm_t *a, asm_width_t width, asm_address_t address, asm_gpr_t src);
void x64_mov_ri32(x64_asm_t *a, asm_gpr_t dst, uint32_t imm);
void x64_mov_ri_sign_extended(x64_asm_t *a, asm_gpr_t dst, int32_t imm);
void x64_movabs(x64_asm_t *a, asm_gpr_t dst, uint64_t imm);
void x64_mov_ri(x64_asm_t *a, asm_gpr_t dst, uint64_t imm);
void x64_mov_mi(x64_asm_t *a, asm_width_t width, asm_address_t address, int32_t imm);
void x64_mov8_mr(x64_asm_t *a, asm_address_t address, asm_gpr_t src);
void x64_mov16_mr(x64_asm_t *a, asm_address_t address, asm_gpr_t src);
void x64_mov8_mi(x64_asm_t *a, asm_address_t address, uint8_t imm);
void x64_mov16_mi(x64_asm_t *a, asm_address_t address, uint16_t imm);
void x64_movzx8_rr(x64_asm_t *a, asm_gpr_t dst, asm_gpr_t src);
void x64_movzx8_rm(x64_asm_t *a, asm_gpr_t dst, asm_address_t address);
void x64_movzx16_rr(x64_asm_t *a, asm_gpr_t dst, asm_gpr_t src);
void x64_movzx16_rm(x64_asm_t *a, asm_gpr_t dst, asm_address_t address);
void x64_movsx8_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_gpr_t src);
void x64_movsx8_rm(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_address_t address);
void x64_movsx16_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_gpr_t src);
void x64_movsx16_rm(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_address_t address);
void x64_movsxd_rr(x64_asm_t *a, asm_gpr_t dst, asm_gpr_t src);
void x64_movsxd_rm(x64_asm_t *a, asm_gpr_t dst, asm_address_t address);
void x64_lea(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_address_t address);
void x64_mov_r_label(x64_asm_t *a, asm_gpr_t dst, asm_label_t label);
void x64_cmp_r_label(x64_asm_t *a, asm_gpr_t lhs, asm_label_t label);
void x64_alu_r_label(x64_asm_t *a, x64_alu_t op, asm_gpr_t lhs, asm_label_t label);
void x64_lea_label(x64_asm_t *a, asm_gpr_t dst, asm_label_t label);
void x64_xchg_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t x, asm_gpr_t y);

void x64_alu_rr(x64_asm_t *a, x64_alu_t op, asm_width_t width, asm_gpr_t dst, asm_gpr_t src);
void x64_alu_ri(x64_asm_t *a, x64_alu_t op, asm_width_t width, asm_gpr_t dst, int32_t imm);
void x64_alu_rm(x64_asm_t *a, x64_alu_t op, asm_width_t width, asm_gpr_t dst, asm_address_t address);
void x64_alu_mr(x64_asm_t *a, x64_alu_t op, asm_width_t width, asm_address_t address, asm_gpr_t src);
void x64_alu_mi(x64_asm_t *a, x64_alu_t op, asm_width_t width, asm_address_t address, int32_t imm);
void x64_add_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_gpr_t src);
void x64_add_ri(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, int32_t imm);
void x64_sub_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_gpr_t src);
void x64_sub_ri(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, int32_t imm);
void x64_and_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_gpr_t src);
void x64_and_ri(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, int32_t imm);
void x64_or_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_gpr_t src);
void x64_or_ri(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, int32_t imm);
void x64_xor_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_gpr_t src);
void x64_xor_ri(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, int32_t imm);
void x64_cmp_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t lhs, asm_gpr_t rhs);
void x64_cmp_ri(x64_asm_t *a, asm_width_t width, asm_gpr_t lhs, int32_t imm);
void x64_cmp_mr(x64_asm_t *a, asm_width_t width, asm_address_t address, asm_gpr_t rhs);
void x64_cmp_mi(x64_asm_t *a, asm_width_t width, asm_address_t address, int32_t imm);
void x64_cmp16_mi8(x64_asm_t *a, asm_address_t address, int8_t imm);
void x64_test_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t x, asm_gpr_t y);
void x64_test_ri(x64_asm_t *a, asm_width_t width, asm_gpr_t reg, int32_t imm);
void x64_test_mi(x64_asm_t *a, asm_width_t width, asm_address_t address, int32_t imm);
void x64_test8_mi(x64_asm_t *a, asm_address_t address, uint8_t imm);
void x64_neg(x64_asm_t *a, asm_width_t width, asm_gpr_t reg);
void x64_not(x64_asm_t *a, asm_width_t width, asm_gpr_t reg);
void x64_shift_ri(x64_asm_t *a, x64_shift_t op, asm_width_t width, asm_gpr_t reg, uint8_t amount);
void x64_shift_rcl(x64_asm_t *a, x64_shift_t op, asm_width_t width, asm_gpr_t reg);
void x64_shl_ri(x64_asm_t *a, asm_width_t width, asm_gpr_t reg, uint8_t amount);
void x64_shr_ri(x64_asm_t *a, asm_width_t width, asm_gpr_t reg, uint8_t amount);
void x64_sar_ri(x64_asm_t *a, asm_width_t width, asm_gpr_t reg, uint8_t amount);
void x64_imul_rr(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_gpr_t src);
void x64_imul_rri(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_gpr_t src, int32_t imm);
void x64_sign_extend_rax_into_rdx(x64_asm_t *a, asm_width_t width);
void x64_idiv(x64_asm_t *a, asm_width_t width, asm_gpr_t divisor);
void x64_setcc(x64_asm_t *a, x64_cond_t cond, asm_gpr_t dst);
void x64_cmovcc(x64_asm_t *a, x64_cond_t cond, asm_width_t width, asm_gpr_t dst, asm_gpr_t src);

void x64_jcc(x64_asm_t *a, x64_cond_t cond, asm_label_t label);
void x64_jcc_short(x64_asm_t *a, x64_cond_t cond, asm_label_t label);
void x64_jmp(x64_asm_t *a, asm_label_t label);
void x64_jmp_short(x64_asm_t *a, asm_label_t label);
void x64_call(x64_asm_t *a, asm_label_t label);
void x64_call_r(x64_asm_t *a, asm_gpr_t target);
void x64_call_m(x64_asm_t *a, asm_address_t address);
void x64_jmp_r(x64_asm_t *a, asm_gpr_t target);
void x64_call_label_indirect(x64_asm_t *a, asm_label_t label);
void x64_jmp_m(x64_asm_t *a, asm_address_t address);
void x64_push(x64_asm_t *a, asm_gpr_t reg);
void x64_pop(x64_asm_t *a, asm_gpr_t reg);
void x64_ret(x64_asm_t *a);
void x64_nop(x64_asm_t *a);
void x64_nops(x64_asm_t *a, size_t size);
void x64_int3(x64_asm_t *a);
void x64_ud2(x64_asm_t *a);

void x64_movsd_rr(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src);
void x64_movapd_rr(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src);
void x64_movsd_rm(x64_asm_t *a, asm_fpr_t dst, asm_address_t address);
void x64_movsd_r_label(x64_asm_t *a, asm_fpr_t dst, asm_label_t label);
void x64_movsd_mr(x64_asm_t *a, asm_address_t address, asm_fpr_t src);
void x64_movups_r_label(x64_asm_t *a, asm_fpr_t dst, asm_label_t label);
void x64_movups_mr(x64_asm_t *a, asm_address_t address, asm_fpr_t src);
void x64_movq_xr(x64_asm_t *a, asm_fpr_t dst, asm_gpr_t src);
void x64_movq_rx(x64_asm_t *a, asm_gpr_t dst, asm_fpr_t src);
void x64_addsd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src);
void x64_mulsd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src);
void x64_subsd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src);
void x64_divsd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src);
void x64_sqrtsd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src);
void x64_roundsd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src, uint8_t mode);
void x64_ucomisd(x64_asm_t *a, asm_fpr_t lhs, asm_fpr_t rhs);
void x64_cvtsi2sd(x64_asm_t *a, asm_width_t width, asm_fpr_t dst, asm_gpr_t src);
void x64_cvttsd2si(x64_asm_t *a, asm_width_t width, asm_gpr_t dst, asm_fpr_t src);
void x64_xorpd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src);
void x64_andpd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src);
void x64_orpd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src);
void x64_minsd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src);
void x64_maxsd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src);
void x64_movss_rm(x64_asm_t *a, asm_fpr_t dst, asm_address_t address);
void x64_movss_mr(x64_asm_t *a, asm_address_t address, asm_fpr_t src);
void x64_cvtss2sd(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src);
void x64_cvtsd2ss(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src);
void x64_movmskpd(x64_asm_t *a, asm_gpr_t dst, asm_fpr_t src);

static constexpr asm_gpr_t MASM_X64_SCRATCH = X64_R11;
static constexpr asm_gpr_t MASM_X64_SCRATCH2 = X64_R10;
static constexpr asm_fpr_t MASM_X64_FP_SCRATCH = X64_XMM15;

static constexpr asm_gpr_t MASM_X64_ARGUMENT_GPRS[] = { X64_RDI, X64_RSI, X64_RDX, X64_RCX, X64_R8, X64_R9 };
static constexpr asm_fpr_t MASM_X64_ARGUMENT_FPRS[] = {
  X64_XMM0, X64_XMM1, X64_XMM2, X64_XMM3, X64_XMM4, X64_XMM5, X64_XMM6, X64_XMM7,
};
static constexpr asm_gpr_t MASM_X64_RETURN_GPRS[2] = { X64_RAX, X64_RDX };

static constexpr asm_regset_t MASM_X64_CALLEE_SAVED_GPRS =
  ASM_REG(X64_RBX) | ASM_REG(X64_RBP) | ASM_REG(X64_R12) | ASM_REG(X64_R13) | ASM_REG(X64_R14) | ASM_REG(X64_R15);
static constexpr asm_regset_t MASM_X64_CALLEE_SAVED_FPRS = 0;
static constexpr asm_regset_t MASM_X64_CALLER_SAVED_GPRS =
  ASM_REG(X64_RAX) | ASM_REG(X64_RCX) | ASM_REG(X64_RDX) | ASM_REG(X64_RSI) | ASM_REG(X64_RDI) |
  ASM_REG(X64_R8) | ASM_REG(X64_R9) | ASM_REG(X64_R10) | ASM_REG(X64_R11);
static constexpr asm_regset_t MASM_X64_CALLER_SAVED_FPRS = 0xffff;
static constexpr asm_regset_t MASM_X64_SCRATCH_GPRS = ASM_REG(X64_R11) | ASM_REG(X64_R10);
static constexpr asm_regset_t MASM_X64_SCRATCH_FPRS = ASM_REG(X64_XMM15);
static constexpr asm_regset_t MASM_X64_ALLOCATABLE_GPRS =
  ASM_REG(X64_RAX) | ASM_REG(X64_RCX) | ASM_REG(X64_RDX) | ASM_REG(X64_RBX) | ASM_REG(X64_RSI) | ASM_REG(X64_RDI) |
  ASM_REG(X64_R8) | ASM_REG(X64_R9) | ASM_REG(X64_R12) | ASM_REG(X64_R13) | ASM_REG(X64_R14) | ASM_REG(X64_R15);
static constexpr asm_regset_t MASM_X64_ALLOCATABLE_FPRS = 0x7fff;
static constexpr asm_gpr_t MASM_X64_STACK_POINTER = X64_RSP;
static constexpr asm_gpr_t MASM_X64_FRAME_POINTER = X64_RBP;
static constexpr size_t MASM_X64_JUMP_PATCH_SIZE = 5;

typedef struct {
  x64_asm_t a;
  masm_constant_t *constants;
  uint32_t constant_count, constant_cap;
  masm_constant128_t *constants128;
  uint32_t constant128_count, constant128_cap;
} masm_x64_t;

void masm_x64_init(masm_x64_t *m);
void masm_x64_free(masm_x64_t *m);
size_t masm_x64_offset(const masm_x64_t *m);
asm_label_t masm_x64_new_label(masm_x64_t *m);
void masm_x64_bind(masm_x64_t *m, asm_label_t label);
size_t masm_x64_label_offset(const masm_x64_t *m, asm_label_t label);
size_t masm_x64_patchable_nop(masm_x64_t *m);
size_t masm_x64_jump_patch(size_t from, size_t to, uint8_t *out);
asm_error_t masm_x64_finish(masm_x64_t *m, asm_code_t *out);
void masm_x64_unreachable(masm_x64_t *m);

void masm_x64_move64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src);
void masm_x64_move32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src);
void masm_x64_move_imm64(masm_x64_t *m, asm_gpr_t dst, uint64_t imm);
void masm_x64_move_imm32(masm_x64_t *m, asm_gpr_t dst, uint32_t imm);
void masm_x64_sign_extend32_to_64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src);

void masm_x64_load8(masm_x64_t *m, asm_gpr_t dst, asm_address_t address);
void masm_x64_load8_sign_extend(masm_x64_t *m, asm_gpr_t dst, asm_address_t address);
void masm_x64_load16(masm_x64_t *m, asm_gpr_t dst, asm_address_t address);
void masm_x64_load16_sign_extend(masm_x64_t *m, asm_gpr_t dst, asm_address_t address);
void masm_x64_load32(masm_x64_t *m, asm_gpr_t dst, asm_address_t address);
void masm_x64_load32_sign_extend(masm_x64_t *m, asm_gpr_t dst, asm_address_t address);
void masm_x64_load64(masm_x64_t *m, asm_gpr_t dst, asm_address_t address);
void masm_x64_store8(masm_x64_t *m, asm_address_t address, asm_gpr_t src);
void masm_x64_store16(masm_x64_t *m, asm_address_t address, asm_gpr_t src);
void masm_x64_store32(masm_x64_t *m, asm_address_t address, asm_gpr_t src);
void masm_x64_store64(masm_x64_t *m, asm_address_t address, asm_gpr_t src);
void masm_x64_store_imm32(masm_x64_t *m, asm_address_t address, uint32_t imm);
void masm_x64_store_imm64(masm_x64_t *m, asm_address_t address, uint64_t imm);
void masm_x64_load_imm128(masm_x64_t *m, asm_fpr_t dst, asm_u128_t imm);
void masm_x64_store128(masm_x64_t *m, asm_address_t address, asm_fpr_t src);
void masm_x64_add32_to_memory_imm(masm_x64_t *m, asm_address_t address, int32_t imm);
void masm_x64_add64_to_memory_imm(masm_x64_t *m, asm_address_t address, int32_t imm);
void masm_x64_load_effective_address(masm_x64_t *m, asm_gpr_t dst, asm_address_t address);
void masm_x64_load_double(masm_x64_t *m, asm_fpr_t dst, asm_address_t address);
void masm_x64_store_double(masm_x64_t *m, asm_address_t address, asm_fpr_t src);
void masm_x64_load_float_as_double(masm_x64_t *m, asm_fpr_t dst, asm_address_t address);
void masm_x64_store_double_as_float(masm_x64_t *m, asm_address_t address, asm_fpr_t src);

void masm_x64_add32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_x64_add32_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm);
void masm_x64_add64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_x64_add64_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int64_t imm);
void masm_x64_sub32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_x64_sub32_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm);
void masm_x64_sub64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_x64_sub64_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int64_t imm);
void masm_x64_and32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_x64_and32_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint32_t imm);
void masm_x64_and64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_x64_and64_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint64_t imm);
void masm_x64_or32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_x64_or32_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint32_t imm);
void masm_x64_or64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_x64_or64_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint64_t imm);
void masm_x64_xor32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_x64_xor32_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint32_t imm);
void masm_x64_xor64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_x64_xor64_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint64_t imm);
void masm_x64_mul32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_x64_mul32_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint32_t imm);
void masm_x64_mul64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_x64_neg32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src);
void masm_x64_neg64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src);
void masm_x64_not32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src);
void masm_x64_not64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src);
void masm_x64_shl32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount);
void masm_x64_shl32_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount);
void masm_x64_shl64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount);
void masm_x64_shl64_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount);
void masm_x64_shr32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount);
void masm_x64_shr32_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount);
void masm_x64_shr64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount);
void masm_x64_shr64_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount);
void masm_x64_sar32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount);
void masm_x64_sar32_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount);
void masm_x64_sar64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount);
void masm_x64_sar64_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount);

void masm_x64_branch_add32_overflow(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t overflow);
void masm_x64_branch_add32_imm_overflow(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm, asm_label_t overflow);
void masm_x64_branch_sub32_overflow(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t overflow);
void masm_x64_branch_sub32_imm_overflow(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm, asm_label_t overflow);
void masm_x64_branch_mul32_overflow(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t overflow);
void masm_x64_branch_neg32_overflow(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_label_t overflow);
void masm_x64_branch_add64_overflow(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t overflow);
void masm_x64_branch_sub64_overflow(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t overflow);

void masm_x64_branch32(masm_x64_t *m, asm_cond_t cond, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t target);
void masm_x64_branch32_imm(masm_x64_t *m, asm_cond_t cond, asm_gpr_t lhs, int32_t imm, asm_label_t target);
void masm_x64_branch64(masm_x64_t *m, asm_cond_t cond, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t target);
void masm_x64_branch_if_stack_pointer_below(masm_x64_t *m, asm_address_t limit, asm_label_t target);
void masm_x64_branch64_imm(masm_x64_t *m, asm_cond_t cond, asm_gpr_t lhs, int64_t imm, asm_label_t target);
void masm_x64_branch32_memory_imm(masm_x64_t *m, asm_cond_t cond, asm_address_t address, int32_t imm, asm_label_t target);
void masm_x64_branch16_memory_imm(masm_x64_t *m, asm_cond_t cond, asm_address_t address, uint16_t imm, asm_label_t target);
void masm_x64_branch64_memory(masm_x64_t *m, asm_cond_t cond, asm_address_t address, asm_gpr_t rhs, asm_label_t target);
void masm_x64_branch64_memory_imm(masm_x64_t *m, asm_cond_t cond, asm_address_t address, int64_t imm, asm_label_t target);
void masm_x64_branch_test32(masm_x64_t *m, asm_cond_t cond, asm_gpr_t value, uint32_t mask, asm_label_t target);
void masm_x64_branch_test64(masm_x64_t *m, asm_cond_t cond, asm_gpr_t value, uint64_t mask, asm_label_t target);
void masm_x64_compare32_set(masm_x64_t *m, asm_cond_t cond, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_x64_compare32_imm_set(masm_x64_t *m, asm_cond_t cond, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm);
void masm_x64_compare64_set(masm_x64_t *m, asm_cond_t cond, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_x64_compare64_imm_set(masm_x64_t *m, asm_cond_t cond, asm_gpr_t dst, asm_gpr_t lhs, int64_t imm);
void masm_x64_select32(
  masm_x64_t *m, asm_cond_t cond, asm_gpr_t lhs, asm_gpr_t rhs,
  asm_gpr_t dst, asm_gpr_t if_true, asm_gpr_t if_false
);
void masm_x64_select64(
  masm_x64_t *m, asm_cond_t cond, asm_gpr_t lhs, asm_gpr_t rhs,
  asm_gpr_t dst, asm_gpr_t if_true, asm_gpr_t if_false
);

void masm_x64_jump(masm_x64_t *m, asm_label_t target);
void masm_x64_jump_absolute(masm_x64_t *m, uint64_t address);
void masm_x64_call(masm_x64_t *m, asm_label_t target);
void masm_x64_call_register(masm_x64_t *m, asm_gpr_t target);
void masm_x64_call_absolute(masm_x64_t *m, uint64_t address);
void masm_x64_ret(masm_x64_t *m);

void masm_x64_move_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t src);
void masm_x64_move_double_imm(masm_x64_t *m, asm_fpr_t dst, double value);
void masm_x64_move_gpr_to_double(masm_x64_t *m, asm_fpr_t dst, asm_gpr_t src);
void masm_x64_move_double_to_gpr(masm_x64_t *m, asm_gpr_t dst, asm_fpr_t src);
void masm_x64_add_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs);
void masm_x64_sub_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs);
void masm_x64_min_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs);
void masm_x64_max_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs);
void masm_x64_mul_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs);
void masm_x64_div_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs);
void masm_x64_sqrt_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t src);
void masm_x64_floor_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t src);
void masm_x64_ceil_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t src);
void masm_x64_trunc_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t src);
void masm_x64_neg_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t src);
void masm_x64_abs_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t src);
void masm_x64_convert_int32_to_double(masm_x64_t *m, asm_fpr_t dst, asm_gpr_t src);
void masm_x64_convert_int64_to_double(masm_x64_t *m, asm_fpr_t dst, asm_gpr_t src);
void masm_x64_branch_convert_double_to_int32(
  masm_x64_t *m, asm_gpr_t dst, asm_fpr_t src,
  asm_label_t fail, asm_negative_zero_t negative_zero
);
void masm_x64_truncate_double_to_int64(masm_x64_t *m, asm_gpr_t dst, asm_fpr_t src);
bool masm_x64_has_js_convert(void);
void masm_x64_js_convert_double_to_int32(masm_x64_t *m, asm_gpr_t dst, asm_fpr_t src);
void masm_x64_branch_js_convert_double_to_int32(masm_x64_t *m, asm_gpr_t dst, asm_fpr_t src, asm_label_t fail);
void masm_x64_branch_double(masm_x64_t *m, asm_dcond_t cond, asm_fpr_t lhs, asm_fpr_t rhs, asm_label_t target);
void masm_x64_compare_double_set(masm_x64_t *m, asm_dcond_t cond, asm_gpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs);

asm_frame_t masm_x64_frame(asm_regset_t saved_gprs, asm_regset_t saved_fprs, uint32_t locals_size);
void masm_x64_emit_prologue(masm_x64_t *m, const asm_frame_t *frame);
void masm_x64_emit_epilogue(masm_x64_t *m, const asm_frame_t *frame);

#endif
