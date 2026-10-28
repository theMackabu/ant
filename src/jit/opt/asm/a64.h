#ifndef ANT_JIT_OPT_ASM_A64_H
#define ANT_JIT_OPT_ASM_A64_H

#include "buffer.h"

static constexpr asm_gpr_t A64_X0 = 0;
static constexpr asm_gpr_t A64_X1 = 1;
static constexpr asm_gpr_t A64_X2 = 2;
static constexpr asm_gpr_t A64_X3 = 3;
static constexpr asm_gpr_t A64_X4 = 4;
static constexpr asm_gpr_t A64_X5 = 5;
static constexpr asm_gpr_t A64_X6 = 6;
static constexpr asm_gpr_t A64_X7 = 7;
static constexpr asm_gpr_t A64_X8 = 8;
static constexpr asm_gpr_t A64_X9 = 9;
static constexpr asm_gpr_t A64_X10 = 10;
static constexpr asm_gpr_t A64_X11 = 11;
static constexpr asm_gpr_t A64_X12 = 12;
static constexpr asm_gpr_t A64_X13 = 13;
static constexpr asm_gpr_t A64_X14 = 14;
static constexpr asm_gpr_t A64_X15 = 15;
static constexpr asm_gpr_t A64_X16 = 16;
static constexpr asm_gpr_t A64_X17 = 17;
static constexpr asm_gpr_t A64_X18 = 18;
static constexpr asm_gpr_t A64_X19 = 19;
static constexpr asm_gpr_t A64_X20 = 20;
static constexpr asm_gpr_t A64_X21 = 21;
static constexpr asm_gpr_t A64_X22 = 22;
static constexpr asm_gpr_t A64_X23 = 23;
static constexpr asm_gpr_t A64_X24 = 24;
static constexpr asm_gpr_t A64_X25 = 25;
static constexpr asm_gpr_t A64_X26 = 26;
static constexpr asm_gpr_t A64_X27 = 27;
static constexpr asm_gpr_t A64_X28 = 28;
static constexpr asm_gpr_t A64_X29 = 29;
static constexpr asm_gpr_t A64_X30 = 30;
static constexpr asm_gpr_t A64_SP = 31;
static constexpr asm_gpr_t A64_XZR = 31;
static constexpr asm_gpr_t A64_FP = A64_X29;
static constexpr asm_gpr_t A64_LR = A64_X30;

static constexpr asm_fpr_t A64_D0 = 0;
static constexpr asm_fpr_t A64_D1 = 1;
static constexpr asm_fpr_t A64_D2 = 2;
static constexpr asm_fpr_t A64_D3 = 3;
static constexpr asm_fpr_t A64_D4 = 4;
static constexpr asm_fpr_t A64_D5 = 5;
static constexpr asm_fpr_t A64_D6 = 6;
static constexpr asm_fpr_t A64_D7 = 7;
static constexpr asm_fpr_t A64_D8 = 8;
static constexpr asm_fpr_t A64_D9 = 9;
static constexpr asm_fpr_t A64_D10 = 10;
static constexpr asm_fpr_t A64_D11 = 11;
static constexpr asm_fpr_t A64_D12 = 12;
static constexpr asm_fpr_t A64_D13 = 13;
static constexpr asm_fpr_t A64_D14 = 14;
static constexpr asm_fpr_t A64_D15 = 15;
static constexpr asm_fpr_t A64_D16 = 16;
static constexpr asm_fpr_t A64_D17 = 17;
static constexpr asm_fpr_t A64_D29 = 29;
static constexpr asm_fpr_t A64_D30 = 30;
static constexpr asm_fpr_t A64_D31 = 31;

typedef enum : uint8_t {
  A64_COND_EQ = 0x0,
  A64_COND_NE = 0x1,
  A64_COND_HS = 0x2,
  A64_COND_LO = 0x3,
  A64_COND_MI = 0x4,
  A64_COND_PL = 0x5,
  A64_COND_VS = 0x6,
  A64_COND_VC = 0x7,
  A64_COND_HI = 0x8,
  A64_COND_LS = 0x9,
  A64_COND_GE = 0xa,
  A64_COND_LT = 0xb,
  A64_COND_GT = 0xc,
  A64_COND_LE = 0xd,
  A64_COND_AL = 0xe,
} a64_cond_t;

static inline a64_cond_t a64_cond_invert(a64_cond_t cond) {
  ASM_CHECK(cond != A64_COND_AL, "AL has no inverse");
  return (a64_cond_t)(cond ^ 1);
}

typedef enum : uint8_t {
  A64_SHIFT_LSL = 0,
  A64_SHIFT_LSR = 1,
  A64_SHIFT_ASR = 2,
  A64_SHIFT_ROR = 3,
} a64_shift_t;

typedef enum : uint8_t {
  A64_EXTEND_UXTB = 0,
  A64_EXTEND_UXTH = 1,
  A64_EXTEND_UXTW = 2,
  A64_EXTEND_UXTX = 3,
  A64_EXTEND_SXTB = 4,
  A64_EXTEND_SXTH = 5,
  A64_EXTEND_SXTW = 6,
  A64_EXTEND_SXTX = 7,
} a64_extend_t;

typedef enum : uint32_t {
  A64_MEM_STRB = 0x38000000,
  A64_MEM_LDRB = 0x38400000,
  A64_MEM_LDRSB_X = 0x38800000,
  A64_MEM_LDRSB_W = 0x38c00000,
  A64_MEM_STRH = 0x78000000,
  A64_MEM_LDRH = 0x78400000,
  A64_MEM_LDRSH_X = 0x78800000,
  A64_MEM_LDRSH_W = 0x78c00000,
  A64_MEM_STR_W = 0xb8000000,
  A64_MEM_LDR_W = 0xb8400000,
  A64_MEM_LDRSW = 0xb8800000,
  A64_MEM_STR_X = 0xf8000000,
  A64_MEM_LDR_X = 0xf8400000,
  A64_MEM_STR_S = 0xbc000000,
  A64_MEM_LDR_S = 0xbc400000,
  A64_MEM_STR_D = 0xfc000000,
  A64_MEM_LDR_D = 0xfc400000,
  A64_MEM_STR_Q = 0x3c800000,
  A64_MEM_LDR_Q = 0x3cc00000,
} a64_memop_t;

static inline uint32_t a64_memop_size_log2(a64_memop_t op) {
  if (op == A64_MEM_STR_Q || op == A64_MEM_LDR_Q) return 4;
  return (uint32_t)op >> 30;
}

typedef enum : uint8_t {
  A64_ADDR_OFFSET,
  A64_ADDR_REGISTER,
  A64_ADDR_PRE_INDEX,
  A64_ADDR_POST_INDEX,
} a64_addr_mode_t;

typedef struct {
  a64_addr_mode_t mode;
  asm_gpr_t base;
  asm_gpr_t index;
  a64_extend_t extend;
  bool shifted;
  int32_t offset;
} a64_mem_t;

static inline a64_mem_t a64_mem_offset(asm_gpr_t base, int32_t offset) {
  return (a64_mem_t){ .mode = A64_ADDR_OFFSET, .base = base, .offset = offset };
}

static inline a64_mem_t a64_mem_register(asm_gpr_t base, asm_gpr_t index, a64_extend_t extend, bool shifted) {
  return (a64_mem_t){ .mode = A64_ADDR_REGISTER, .base = base, .index = index, .extend = extend, .shifted = shifted };
}

static inline a64_mem_t a64_mem_pre_index(asm_gpr_t base, int32_t offset) {
  return (a64_mem_t){ .mode = A64_ADDR_PRE_INDEX, .base = base, .offset = offset };
}

static inline a64_mem_t a64_mem_post_index(asm_gpr_t base, int32_t offset) {
  return (a64_mem_t){ .mode = A64_ADDR_POST_INDEX, .base = base, .offset = offset };
}

typedef struct {
  a64_addr_mode_t mode;
  asm_gpr_t base;
  int32_t offset;
} a64_pair_t;

static inline a64_pair_t a64_pair_offset(asm_gpr_t base, int32_t offset) {
  return (a64_pair_t){ .mode = A64_ADDR_OFFSET, .base = base, .offset = offset };
}

static inline a64_pair_t a64_pair_pre_index(asm_gpr_t base, int32_t offset) {
  return (a64_pair_t){ .mode = A64_ADDR_PRE_INDEX, .base = base, .offset = offset };
}

static inline a64_pair_t a64_pair_post_index(asm_gpr_t base, int32_t offset) {
  return (a64_pair_t){ .mode = A64_ADDR_POST_INDEX, .base = base, .offset = offset };
}

bool a64_encode_logical_immediate(uint64_t imm, asm_width_t width, uint32_t *out);
bool a64_encode_fp_immediate(double value, uint8_t *out);

typedef struct {
  asm_buffer_t buf;
  uint32_t *veneer_candidates;
  size_t veneer_count, veneer_cap;
  size_t next_veneer_check;
} a64_asm_t;

void a64_init(a64_asm_t *a);
void a64_free(a64_asm_t *a);

static inline size_t a64_offset(const a64_asm_t *a) { return asm_buffer_offset(&a->buf); }
static inline asm_label_t a64_new_label(a64_asm_t *a) { return asm_new_label(&a->buf); }
static inline void a64_bind(a64_asm_t *a, asm_label_t label) { asm_bind(&a->buf, label); }
static inline size_t a64_label_offset(const a64_asm_t *a, asm_label_t label) { return asm_label_offset(&a->buf, label); }

asm_error_t a64_finish(a64_asm_t *a, asm_code_t *out);

void a64_emit(a64_asm_t *a, uint32_t word);
void a64_data_u32(a64_asm_t *a, uint32_t value);
void a64_data_u64(a64_asm_t *a, uint64_t value);
void a64_data_bytes(a64_asm_t *a, const void *bytes, size_t n);
void a64_align_code(a64_asm_t *a, size_t alignment);
void a64_align_data(a64_asm_t *a, size_t alignment);

void a64_movz(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, uint16_t imm16, uint8_t shift);
void a64_movn(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, uint16_t imm16, uint8_t shift);
void a64_movk(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, uint16_t imm16, uint8_t shift);
void a64_mov_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, uint64_t imm);
size_t a64_mov_imm_instruction_count(uint64_t imm);
void a64_mov(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rm);
void a64_mov_sp(a64_asm_t *a, asm_gpr_t rd, asm_gpr_t rn);

void a64_add_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, uint32_t imm12, bool shift12);
void a64_adds_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, uint32_t imm12, bool shift12);
void a64_sub_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, uint32_t imm12, bool shift12);
void a64_subs_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, uint32_t imm12, bool shift12);
void a64_cmp_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t rn, uint32_t imm12, bool shift12);
void a64_cmn_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t rn, uint32_t imm12, bool shift12);
void a64_add(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm, a64_shift_t shift, uint8_t amount);
void a64_adds(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm, a64_shift_t shift, uint8_t amount);
void a64_sub(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm, a64_shift_t shift, uint8_t amount);
void a64_subs(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm, a64_shift_t shift, uint8_t amount);
void a64_cmp(a64_asm_t *a, asm_width_t width, asm_gpr_t rn, asm_gpr_t rm);
void a64_cmn(a64_asm_t *a, asm_width_t width, asm_gpr_t rn, asm_gpr_t rm);
void a64_neg(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rm);
void a64_negs(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rm);
void a64_add_extended(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm, a64_extend_t extend, uint8_t amount);
void a64_sub_extended(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm, a64_extend_t extend, uint8_t amount);
void a64_subs_extended(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm, a64_extend_t extend, uint8_t amount);

void a64_and_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, uint64_t imm);
void a64_orr_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, uint64_t imm);
void a64_eor_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, uint64_t imm);
void a64_ands_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, uint64_t imm);
void a64_tst_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t rn, uint64_t imm);
void a64_and(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm, a64_shift_t shift, uint8_t amount);
void a64_bic(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm, a64_shift_t shift, uint8_t amount);
void a64_orr(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm, a64_shift_t shift, uint8_t amount);
void a64_orn(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm, a64_shift_t shift, uint8_t amount);
void a64_eor(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm, a64_shift_t shift, uint8_t amount);
void a64_ands(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm, a64_shift_t shift, uint8_t amount);
void a64_tst(a64_asm_t *a, asm_width_t width, asm_gpr_t rn, asm_gpr_t rm);
void a64_mvn(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rm);

void a64_ubfm(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, uint8_t immr, uint8_t imms);
void a64_sbfm(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, uint8_t immr, uint8_t imms);
void a64_lsl_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, uint8_t amount);
void a64_lsr_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, uint8_t amount);
void a64_asr_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, uint8_t amount);
void a64_lsl(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm);
void a64_lsr(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm);
void a64_asr(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm);
void a64_ror(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm);
void a64_sdiv(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm);
void a64_udiv(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm);
void a64_sxtw(a64_asm_t *a, asm_gpr_t rd, asm_gpr_t rn);
void a64_sxtb(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn);
void a64_sxth(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn);
void a64_uxtb(a64_asm_t *a, asm_gpr_t rd, asm_gpr_t rn);
void a64_uxth(a64_asm_t *a, asm_gpr_t rd, asm_gpr_t rn);

void a64_madd(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm, asm_gpr_t ra);
void a64_msub(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm, asm_gpr_t ra);
void a64_mul(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm);
void a64_smull(a64_asm_t *a, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm);
void a64_umull(a64_asm_t *a, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm);
void a64_smulh(a64_asm_t *a, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm);
void a64_umulh(a64_asm_t *a, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm);
void a64_check_smull_overflow(a64_asm_t *a, asm_gpr_t product);
void a64_check_smulh_overflow(a64_asm_t *a, asm_gpr_t high, asm_gpr_t low);

void a64_csel(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm, a64_cond_t cond);
void a64_csinc(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm, a64_cond_t cond);
void a64_csinv(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm, a64_cond_t cond);
void a64_csneg(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_gpr_t rn, asm_gpr_t rm, a64_cond_t cond);
void a64_cset(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, a64_cond_t cond);
void a64_csetm(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, a64_cond_t cond);

void a64_load_store(a64_asm_t *a, a64_memop_t op, uint8_t rt, a64_mem_t operand);
void a64_ldr(a64_asm_t *a, asm_width_t width, asm_gpr_t rt, a64_mem_t operand);
void a64_str(a64_asm_t *a, asm_width_t width, asm_gpr_t rt, a64_mem_t operand);
void a64_ldrb(a64_asm_t *a, asm_gpr_t rt, a64_mem_t operand);
void a64_ldrh(a64_asm_t *a, asm_gpr_t rt, a64_mem_t operand);
void a64_ldrsb(a64_asm_t *a, asm_width_t width, asm_gpr_t rt, a64_mem_t operand);
void a64_ldrsh(a64_asm_t *a, asm_width_t width, asm_gpr_t rt, a64_mem_t operand);
void a64_ldrsw(a64_asm_t *a, asm_gpr_t rt, a64_mem_t operand);
void a64_strb(a64_asm_t *a, asm_gpr_t rt, a64_mem_t operand);
void a64_strh(a64_asm_t *a, asm_gpr_t rt, a64_mem_t operand);
void a64_ldr_d(a64_asm_t *a, asm_fpr_t rt, a64_mem_t operand);
void a64_str_d(a64_asm_t *a, asm_fpr_t rt, a64_mem_t operand);
void a64_stp(a64_asm_t *a, asm_gpr_t rt, asm_gpr_t rt2, a64_pair_t operand);
void a64_ldp(a64_asm_t *a, asm_gpr_t rt, asm_gpr_t rt2, a64_pair_t operand);
void a64_stp_d(a64_asm_t *a, asm_fpr_t rt, asm_fpr_t rt2, a64_pair_t operand);
void a64_ldp_d(a64_asm_t *a, asm_fpr_t rt, asm_fpr_t rt2, a64_pair_t operand);

void a64_b(a64_asm_t *a, asm_label_t label);
void a64_bl(a64_asm_t *a, asm_label_t label);
void a64_b_cond(a64_asm_t *a, a64_cond_t cond, asm_label_t label);
void a64_cbz(a64_asm_t *a, asm_width_t width, asm_gpr_t rt, asm_label_t label);
void a64_cbnz(a64_asm_t *a, asm_width_t width, asm_gpr_t rt, asm_label_t label);
void a64_tbz(a64_asm_t *a, asm_gpr_t rt, uint8_t bit, asm_label_t label);
void a64_tbnz(a64_asm_t *a, asm_gpr_t rt, uint8_t bit, asm_label_t label);
void a64_br(a64_asm_t *a, asm_gpr_t rn);
void a64_blr(a64_asm_t *a, asm_gpr_t rn);
void a64_ret(a64_asm_t *a);
void a64_ldr_literal(a64_asm_t *a, asm_gpr_t rt, asm_label_t label);
void a64_ldr_literal_double(a64_asm_t *a, asm_fpr_t rt, asm_label_t label);
void a64_ldr_literal_quad(a64_asm_t *a, asm_fpr_t rt, asm_label_t label);
void a64_adr(a64_asm_t *a, asm_gpr_t rd, asm_label_t label);
void a64_nop(a64_asm_t *a);
void a64_brk(a64_asm_t *a, uint16_t imm16);
void a64_udf(a64_asm_t *a, uint16_t imm16);

void a64_fmov(a64_asm_t *a, asm_fpr_t rd, asm_fpr_t rn);
void a64_fmov_from_gpr(a64_asm_t *a, asm_fpr_t rd, asm_gpr_t rn);
void a64_fmov_to_gpr(a64_asm_t *a, asm_gpr_t rd, asm_fpr_t rn);
void a64_fmov_imm(a64_asm_t *a, asm_fpr_t rd, double value);
void a64_fadd(a64_asm_t *a, asm_fpr_t rd, asm_fpr_t rn, asm_fpr_t rm);
void a64_fsub(a64_asm_t *a, asm_fpr_t rd, asm_fpr_t rn, asm_fpr_t rm);
void a64_fmul(a64_asm_t *a, asm_fpr_t rd, asm_fpr_t rn, asm_fpr_t rm);
void a64_fdiv(a64_asm_t *a, asm_fpr_t rd, asm_fpr_t rn, asm_fpr_t rm);
void a64_fmin(a64_asm_t *a, asm_fpr_t rd, asm_fpr_t rn, asm_fpr_t rm);
void a64_fmax(a64_asm_t *a, asm_fpr_t rd, asm_fpr_t rn, asm_fpr_t rm);
void a64_fsqrt(a64_asm_t *a, asm_fpr_t rd, asm_fpr_t rn);
void a64_frintm(a64_asm_t *a, asm_fpr_t rd, asm_fpr_t rn);
void a64_frintp(a64_asm_t *a, asm_fpr_t rd, asm_fpr_t rn);
void a64_frintz(a64_asm_t *a, asm_fpr_t rd, asm_fpr_t rn);
void a64_fneg(a64_asm_t *a, asm_fpr_t rd, asm_fpr_t rn);
void a64_fabs(a64_asm_t *a, asm_fpr_t rd, asm_fpr_t rn);
void a64_fcmp(a64_asm_t *a, asm_fpr_t rn, asm_fpr_t rm);
void a64_fcmp_zero(a64_asm_t *a, asm_fpr_t rn);
void a64_scvtf(a64_asm_t *a, asm_width_t width, asm_fpr_t rd, asm_gpr_t rn);
void a64_fcvt_double_from_single(a64_asm_t *a, asm_fpr_t rd, asm_fpr_t rn);
void a64_fcvt_single_from_double(a64_asm_t *a, asm_fpr_t rd, asm_fpr_t rn);
void a64_fcvtzs(a64_asm_t *a, asm_width_t width, asm_gpr_t rd, asm_fpr_t rn);
void a64_fjcvtzs(a64_asm_t *a, asm_gpr_t rd, asm_fpr_t rn);

static constexpr asm_gpr_t MASM_A64_ADDRESS_SCRATCH = A64_X16;
static constexpr asm_gpr_t MASM_A64_VALUE_SCRATCH = A64_X17;
static constexpr asm_fpr_t MASM_A64_FP_SCRATCH = A64_D31;

static constexpr asm_gpr_t MASM_A64_ARGUMENT_GPRS[] = {
  A64_X0, A64_X1, A64_X2, A64_X3, A64_X4, A64_X5, A64_X6, A64_X7,
};
static constexpr asm_fpr_t MASM_A64_ARGUMENT_FPRS[] = {
  A64_D0, A64_D1, A64_D2, A64_D3, A64_D4, A64_D5, A64_D6, A64_D7,
};
static constexpr asm_gpr_t MASM_A64_RETURN_GPRS[2] = { A64_X0, A64_X1 };

static constexpr asm_regset_t MASM_A64_CALLEE_SAVED_GPRS = 0x7ff80000;
static constexpr asm_regset_t MASM_A64_CALLEE_SAVED_FPRS = 0x0000ff00;
static constexpr asm_regset_t MASM_A64_CALLER_SAVED_GPRS = 0x0003ffff;
static constexpr asm_regset_t MASM_A64_CALLER_SAVED_FPRS = 0xffff00ff;
static constexpr asm_regset_t MASM_A64_SCRATCH_GPRS = ASM_REG(A64_X16) | ASM_REG(A64_X17);
static constexpr asm_regset_t MASM_A64_SCRATCH_FPRS = ASM_REG(A64_D31);
static constexpr asm_regset_t MASM_A64_ALLOCATABLE_GPRS = 0x1ff8ffff;
static constexpr asm_regset_t MASM_A64_ALLOCATABLE_FPRS = 0x7fffffff;
static constexpr asm_gpr_t MASM_A64_STACK_POINTER = A64_SP;
static constexpr asm_gpr_t MASM_A64_FRAME_POINTER = A64_FP;
static constexpr size_t MASM_A64_JUMP_PATCH_SIZE = 4;

typedef struct {
  a64_asm_t a;
  masm_constant_t *constants;
  uint32_t constant_count, constant_cap;
  masm_constant128_t *constants128;
  uint32_t constant128_count, constant128_cap;
} masm_a64_t;

void masm_a64_init(masm_a64_t *m);
void masm_a64_free(masm_a64_t *m);
size_t masm_a64_offset(const masm_a64_t *m);
asm_label_t masm_a64_new_label(masm_a64_t *m);
void masm_a64_bind(masm_a64_t *m, asm_label_t label);
size_t masm_a64_label_offset(const masm_a64_t *m, asm_label_t label);
size_t masm_a64_patchable_nop(masm_a64_t *m);
size_t masm_a64_jump_patch(size_t from, size_t to, uint8_t *out);
asm_error_t masm_a64_finish(masm_a64_t *m, asm_code_t *out);
void masm_a64_unreachable(masm_a64_t *m);

void masm_a64_move64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src);
void masm_a64_move32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src);
void masm_a64_move_imm64(masm_a64_t *m, asm_gpr_t dst, uint64_t imm);
void masm_a64_move_imm32(masm_a64_t *m, asm_gpr_t dst, uint32_t imm);
void masm_a64_sign_extend32_to_64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src);

void masm_a64_load8(masm_a64_t *m, asm_gpr_t dst, asm_address_t address);
void masm_a64_load8_sign_extend(masm_a64_t *m, asm_gpr_t dst, asm_address_t address);
void masm_a64_load16(masm_a64_t *m, asm_gpr_t dst, asm_address_t address);
void masm_a64_load16_sign_extend(masm_a64_t *m, asm_gpr_t dst, asm_address_t address);
void masm_a64_load32(masm_a64_t *m, asm_gpr_t dst, asm_address_t address);
void masm_a64_load32_sign_extend(masm_a64_t *m, asm_gpr_t dst, asm_address_t address);
void masm_a64_load64(masm_a64_t *m, asm_gpr_t dst, asm_address_t address);
void masm_a64_store8(masm_a64_t *m, asm_address_t address, asm_gpr_t src);
void masm_a64_store16(masm_a64_t *m, asm_address_t address, asm_gpr_t src);
void masm_a64_store32(masm_a64_t *m, asm_address_t address, asm_gpr_t src);
void masm_a64_store64(masm_a64_t *m, asm_address_t address, asm_gpr_t src);
void masm_a64_store_imm32(masm_a64_t *m, asm_address_t address, uint32_t imm);
void masm_a64_store_imm64(masm_a64_t *m, asm_address_t address, uint64_t imm);
void masm_a64_load_imm128(masm_a64_t *m, asm_fpr_t dst, asm_u128_t imm);
void masm_a64_store128(masm_a64_t *m, asm_address_t address, asm_fpr_t src);
void masm_a64_add32_to_memory_imm(masm_a64_t *m, asm_address_t address, int32_t imm);
void masm_a64_add64_to_memory_imm(masm_a64_t *m, asm_address_t address, int32_t imm);
void masm_a64_load_effective_address(masm_a64_t *m, asm_gpr_t dst, asm_address_t address);
void masm_a64_load_double(masm_a64_t *m, asm_fpr_t dst, asm_address_t address);
void masm_a64_store_double(masm_a64_t *m, asm_address_t address, asm_fpr_t src);
void masm_a64_load_float_as_double(masm_a64_t *m, asm_fpr_t dst, asm_address_t address);
void masm_a64_store_double_as_float(masm_a64_t *m, asm_address_t address, asm_fpr_t src);

void masm_a64_add32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_a64_add32_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm);
void masm_a64_add64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_a64_add64_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int64_t imm);
void masm_a64_sub32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_a64_sub32_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm);
void masm_a64_sub64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_a64_sub64_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int64_t imm);
void masm_a64_and32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_a64_and32_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint32_t imm);
void masm_a64_and64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_a64_and64_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint64_t imm);
void masm_a64_or32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_a64_or32_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint32_t imm);
void masm_a64_or64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_a64_or64_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint64_t imm);
void masm_a64_xor32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_a64_xor32_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint32_t imm);
void masm_a64_xor64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_a64_xor64_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint64_t imm);
void masm_a64_mul32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_a64_remainder32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_gpr_t quotient);
void masm_a64_mul32_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint32_t imm);
void masm_a64_mul64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_a64_neg32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src);
void masm_a64_neg64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src);
void masm_a64_not32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src);
void masm_a64_not64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src);
void masm_a64_shl32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount);
void masm_a64_shl32_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount);
void masm_a64_shl64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount);
void masm_a64_shl64_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount);
void masm_a64_shr32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount);
void masm_a64_shr32_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount);
void masm_a64_shr64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount);
void masm_a64_shr64_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount);
void masm_a64_sar32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount);
void masm_a64_sar32_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount);
void masm_a64_sar64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount);
void masm_a64_sar64_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount);

void masm_a64_branch_add32_overflow(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t overflow);
void masm_a64_branch_add32_imm_overflow(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm, asm_label_t overflow);
void masm_a64_branch_sub32_overflow(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t overflow);
void masm_a64_branch_sub32_imm_overflow(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm, asm_label_t overflow);
void masm_a64_branch_mul32_overflow(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t overflow);
void masm_a64_branch_neg32_overflow(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_label_t overflow);
void masm_a64_branch_add64_overflow(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t overflow);
void masm_a64_branch_sub64_overflow(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t overflow);

void masm_a64_branch32(masm_a64_t *m, asm_cond_t cond, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t target);
void masm_a64_branch32_imm(masm_a64_t *m, asm_cond_t cond, asm_gpr_t lhs, int32_t imm, asm_label_t target);
void masm_a64_branch64(masm_a64_t *m, asm_cond_t cond, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t target);
void masm_a64_branch_if_stack_pointer_below(masm_a64_t *m, asm_address_t limit, asm_label_t target);
void masm_a64_branch64_imm(masm_a64_t *m, asm_cond_t cond, asm_gpr_t lhs, int64_t imm, asm_label_t target);
void masm_a64_branch32_memory_imm(masm_a64_t *m, asm_cond_t cond, asm_address_t address, int32_t imm, asm_label_t target);
void masm_a64_branch16_memory_imm(masm_a64_t *m, asm_cond_t cond, asm_address_t address, uint16_t imm, asm_label_t target);
void masm_a64_branch64_memory(masm_a64_t *m, asm_cond_t cond, asm_address_t address, asm_gpr_t rhs, asm_label_t target);
void masm_a64_branch64_memory_imm(masm_a64_t *m, asm_cond_t cond, asm_address_t address, int64_t imm, asm_label_t target);
void masm_a64_branch_test32(masm_a64_t *m, asm_cond_t cond, asm_gpr_t value, uint32_t mask, asm_label_t target);
void masm_a64_branch_test64(masm_a64_t *m, asm_cond_t cond, asm_gpr_t value, uint64_t mask, asm_label_t target);
void masm_a64_compare32_set(masm_a64_t *m, asm_cond_t cond, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_a64_compare32_imm_set(masm_a64_t *m, asm_cond_t cond, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm);
void masm_a64_compare64_set(masm_a64_t *m, asm_cond_t cond, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs);
void masm_a64_compare64_imm_set(masm_a64_t *m, asm_cond_t cond, asm_gpr_t dst, asm_gpr_t lhs, int64_t imm);
void masm_a64_select32(
  masm_a64_t *m, asm_cond_t cond, asm_gpr_t lhs, asm_gpr_t rhs,
  asm_gpr_t dst, asm_gpr_t if_true, asm_gpr_t if_false
);
void masm_a64_select64(
  masm_a64_t *m, asm_cond_t cond, asm_gpr_t lhs, asm_gpr_t rhs,
  asm_gpr_t dst, asm_gpr_t if_true, asm_gpr_t if_false
);

void masm_a64_jump(masm_a64_t *m, asm_label_t target);
void masm_a64_jump_absolute(masm_a64_t *m, uint64_t address);
void masm_a64_call(masm_a64_t *m, asm_label_t target);
void masm_a64_call_register(masm_a64_t *m, asm_gpr_t target);
void masm_a64_call_absolute(masm_a64_t *m, uint64_t address);
void masm_a64_ret(masm_a64_t *m);

void masm_a64_move_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t src);
void masm_a64_move_double_imm(masm_a64_t *m, asm_fpr_t dst, double value);
void masm_a64_move_gpr_to_double(masm_a64_t *m, asm_fpr_t dst, asm_gpr_t src);
void masm_a64_move_double_to_gpr(masm_a64_t *m, asm_gpr_t dst, asm_fpr_t src);
void masm_a64_add_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs);
void masm_a64_sub_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs);
void masm_a64_min_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs);
void masm_a64_max_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs);
void masm_a64_mul_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs);
void masm_a64_div_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs);
void masm_a64_sqrt_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t src);
void masm_a64_floor_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t src);
void masm_a64_ceil_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t src);
void masm_a64_trunc_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t src);
void masm_a64_neg_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t src);
void masm_a64_abs_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t src);
void masm_a64_convert_int32_to_double(masm_a64_t *m, asm_fpr_t dst, asm_gpr_t src);
void masm_a64_convert_int64_to_double(masm_a64_t *m, asm_fpr_t dst, asm_gpr_t src);
void masm_a64_branch_convert_double_to_int32(
  masm_a64_t *m, asm_gpr_t dst, asm_fpr_t src,
  asm_label_t fail, asm_negative_zero_t negative_zero
);
void masm_a64_truncate_double_to_int64(masm_a64_t *m, asm_gpr_t dst, asm_fpr_t src);
bool masm_a64_has_js_convert(void);
void masm_a64_js_convert_double_to_int32(masm_a64_t *m, asm_gpr_t dst, asm_fpr_t src);
void masm_a64_branch_js_convert_double_to_int32(masm_a64_t *m, asm_gpr_t dst, asm_fpr_t src, asm_label_t fail);
void masm_a64_branch_double(masm_a64_t *m, asm_dcond_t cond, asm_fpr_t lhs, asm_fpr_t rhs, asm_label_t target);
void masm_a64_compare_double_set(masm_a64_t *m, asm_dcond_t cond, asm_gpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs);

asm_frame_t masm_a64_frame(asm_regset_t saved_gprs, asm_regset_t saved_fprs, uint32_t locals_size);
void masm_a64_emit_prologue(masm_a64_t *m, const asm_frame_t *frame);
void masm_a64_emit_epilogue(masm_a64_t *m, const asm_frame_t *frame);

#endif
