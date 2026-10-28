#include "a64.h"

#include <stdlib.h>
#if defined(__aarch64__) && defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#endif

static constexpr asm_gpr_t ADDRESS_SCRATCH = MASM_A64_ADDRESS_SCRATCH;
static constexpr asm_gpr_t VALUE_SCRATCH = MASM_A64_VALUE_SCRATCH;
static constexpr asm_fpr_t FP_SCRATCH = MASM_A64_FP_SCRATCH;

static a64_cond_t condition_code(asm_cond_t cond) {
  switch (cond) {
    case ASM_COND_EQUAL:
    case ASM_COND_ZERO: return A64_COND_EQ;
    case ASM_COND_NOT_EQUAL:
    case ASM_COND_NON_ZERO: return A64_COND_NE;
    case ASM_COND_LESS_THAN: return A64_COND_LT;
    case ASM_COND_LESS_THAN_OR_EQUAL: return A64_COND_LE;
    case ASM_COND_GREATER_THAN: return A64_COND_GT;
    case ASM_COND_GREATER_THAN_OR_EQUAL: return A64_COND_GE;
    case ASM_COND_BELOW: return A64_COND_LO;
    case ASM_COND_BELOW_OR_EQUAL: return A64_COND_LS;
    case ASM_COND_ABOVE: return A64_COND_HI;
    case ASM_COND_ABOVE_OR_EQUAL: return A64_COND_HS;
    case ASM_COND_OVERFLOW: return A64_COND_VS;
    case ASM_COND_NO_OVERFLOW: return A64_COND_VC;
    case ASM_COND_NEGATIVE: return A64_COND_MI;
    case ASM_COND_POSITIVE_OR_ZERO: return A64_COND_PL;
  }
  return A64_COND_EQ;
}

typedef struct {
  bool either;
  a64_cond_t first;
  a64_cond_t second;
} double_flags_t;

static double_flags_t double_flags(asm_dcond_t cond) {
  switch (cond) {
    case ASM_DCOND_EQUAL: return (double_flags_t){ false, A64_COND_EQ, 0 };
    case ASM_DCOND_NOT_EQUAL: return (double_flags_t){ true, A64_COND_MI, A64_COND_GT };
    case ASM_DCOND_LESS_THAN: return (double_flags_t){ false, A64_COND_MI, 0 };
    case ASM_DCOND_LESS_THAN_OR_EQUAL: return (double_flags_t){ false, A64_COND_LS, 0 };
    case ASM_DCOND_GREATER_THAN: return (double_flags_t){ false, A64_COND_GT, 0 };
    case ASM_DCOND_GREATER_THAN_OR_EQUAL: return (double_flags_t){ false, A64_COND_GE, 0 };
    case ASM_DCOND_EQUAL_OR_UNORDERED: return (double_flags_t){ true, A64_COND_EQ, A64_COND_VS };
    case ASM_DCOND_NOT_EQUAL_OR_UNORDERED: return (double_flags_t){ false, A64_COND_NE, 0 };
    case ASM_DCOND_LESS_THAN_OR_UNORDERED: return (double_flags_t){ false, A64_COND_LT, 0 };
    case ASM_DCOND_LESS_THAN_OR_EQUAL_OR_UNORDERED: return (double_flags_t){ false, A64_COND_LE, 0 };
    case ASM_DCOND_GREATER_THAN_OR_UNORDERED: return (double_flags_t){ false, A64_COND_HI, 0 };
    case ASM_DCOND_GREATER_THAN_OR_EQUAL_OR_UNORDERED: return (double_flags_t){ false, A64_COND_PL, 0 };
    case ASM_DCOND_ORDERED: return (double_flags_t){ false, A64_COND_VC, 0 };
    case ASM_DCOND_UNORDERED: return (double_flags_t){ false, A64_COND_VS, 0 };
  }
  return (double_flags_t){ false, A64_COND_VS, 0 };
}

static bool arithmetic_immediate(uint64_t imm, uint32_t *imm12, bool *shift12) {
  if (imm < 4096) {
    *imm12 = (uint32_t)imm;
    *shift12 = false;
    return true;
  }
  if ((imm & 0xfff) == 0 && (imm >> 12) < 4096) {
    *imm12 = (uint32_t)(imm >> 12);
    *shift12 = true;
    return true;
  }
  return false;
}

typedef enum {
  ARITH_ADD,
  ARITH_SUB,
} arith_op_t;

typedef enum {
  LOGICAL_AND,
  LOGICAL_OR,
  LOGICAL_XOR,
} logical_op_t;

static inline arith_op_t opposite(arith_op_t op) {
  return op == ARITH_ADD ? ARITH_SUB : ARITH_ADD;
}

static inline uint64_t unsigned_abs(int64_t value) {
  return value < 0 ? 0 - (uint64_t)value : (uint64_t)value;
}

static asm_label_t constant(masm_a64_t *m, uint64_t value) {
  for (uint32_t i = 0; i < m->constant_count; i++)
    if (m->constants[i].value == value) return m->constants[i].label;
  if (m->constant_count == m->constant_cap) {
    uint32_t cap = m->constant_cap ? m->constant_cap * 2 : 8;
    masm_constant_t *grown = realloc(m->constants, cap * sizeof(*grown));
    if (!grown) asm_panic("out of memory");
    m->constants = grown;
    m->constant_cap = cap;
  }
  asm_label_t label = a64_new_label(&m->a);
  m->constants[m->constant_count++] = (masm_constant_t){ value, label };
  return label;
}

static asm_label_t constant128(masm_a64_t *m, asm_u128_t value) {
  for (uint32_t i = 0; i < m->constant128_count; i++)
    if (m->constants128[i].value.lo == value.lo && m->constants128[i].value.hi == value.hi) return m->constants128[i].label;
  if (m->constant128_count == m->constant128_cap) {
    uint32_t cap = m->constant128_cap ? m->constant128_cap * 2 : 4;
    masm_constant128_t *grown = realloc(m->constants128, cap * sizeof(*grown));
    if (!grown) asm_panic("out of memory");
    m->constants128 = grown;
    m->constant128_cap = cap;
  }
  asm_label_t label = a64_new_label(&m->a);
  m->constants128[m->constant128_count++] = (masm_constant128_t){ value, label };
  return label;
}

static void mov_imm(masm_a64_t *m, asm_width_t width, asm_gpr_t dst, uint64_t imm) {
  if (width == ASM_W64 && a64_mov_imm_instruction_count(imm) > 2) a64_ldr_literal(&m->a, dst, constant(m, imm));
  else a64_mov_imm(&m->a, width, dst, imm);
}

static a64_mem_t legalize(masm_a64_t *m, asm_address_t address, uint32_t size_log2) {
  asm_gpr_t base = address.base;
  int32_t disp = address.disp;
  int32_t size = 1 << size_log2;
  if (!address.has_index) {
    bool scaled = disp >= 0 && disp % size == 0 && disp / size < 4096;
    if (scaled || (disp >= -256 && disp < 256)) return a64_mem_offset(base, disp);
    a64_mov_imm(&m->a, ASM_W64, ADDRESS_SCRATCH, (uint64_t)(int64_t)disp);
    return a64_mem_register(base, ADDRESS_SCRATCH, A64_EXTEND_UXTX, false);
  }
  uint32_t scale = (uint32_t)address.scale;
  bool index_fits = scale == 0 || scale == size_log2;
  if (disp == 0 && index_fits) return a64_mem_register(base, address.index, A64_EXTEND_UXTX, scale != 0);
  if (disp == 0) {
    a64_add_extended(&m->a, ASM_W64, ADDRESS_SCRATCH, base, address.index, A64_EXTEND_UXTX, (uint8_t)scale);
    return a64_mem_offset(ADDRESS_SCRATCH, 0);
  }
  a64_mov_imm(&m->a, ASM_W64, ADDRESS_SCRATCH, (uint64_t)(int64_t)disp);
  a64_add_extended(&m->a, ASM_W64, ADDRESS_SCRATCH, base, ADDRESS_SCRATCH, A64_EXTEND_UXTX, 0);
  if (index_fits) return a64_mem_register(ADDRESS_SCRATCH, address.index, A64_EXTEND_UXTX, scale != 0);
  a64_add(&m->a, ASM_W64, ADDRESS_SCRATCH, ADDRESS_SCRATCH, address.index, A64_SHIFT_LSL, (uint8_t)scale);
  return a64_mem_offset(ADDRESS_SCRATCH, 0);
}

static void load_store(masm_a64_t *m, a64_memop_t op, uint8_t rt, asm_address_t address) {
  a64_mem_t operand = legalize(m, address, a64_memop_size_log2(op));
  a64_load_store(&m->a, op, rt, operand);
}

static void arithmetic_imm(masm_a64_t *m, arith_op_t op, asm_width_t width, asm_gpr_t dst, asm_gpr_t lhs, int64_t imm) {
  if (imm < 0) op = opposite(op);
  uint64_t magnitude = unsigned_abs(imm);
  if (width == ASM_W32) magnitude &= UINT32_MAX;
  uint32_t imm12;
  bool shift12;
  if (arithmetic_immediate(magnitude, &imm12, &shift12)) {
    if (op == ARITH_ADD) a64_add_imm(&m->a, width, dst, lhs, imm12, shift12);
    else a64_sub_imm(&m->a, width, dst, lhs, imm12, shift12);
    return;
  }
  mov_imm(m, width, VALUE_SCRATCH, magnitude);
  if (dst == A64_SP || lhs == A64_SP) {
    if (op == ARITH_ADD) a64_add_extended(&m->a, ASM_W64, dst, lhs, VALUE_SCRATCH, A64_EXTEND_UXTX, 0);
    else a64_sub_extended(&m->a, ASM_W64, dst, lhs, VALUE_SCRATCH, A64_EXTEND_UXTX, 0);
  } else {
    if (op == ARITH_ADD) a64_add(&m->a, width, dst, lhs, VALUE_SCRATCH, A64_SHIFT_LSL, 0);
    else a64_sub(&m->a, width, dst, lhs, VALUE_SCRATCH, A64_SHIFT_LSL, 0);
  }
}

static void add_to_memory_imm(masm_a64_t *m, asm_width_t width, asm_address_t address, int32_t imm) {
  a64_memop_t load = width == ASM_W32 ? A64_MEM_LDR_W : A64_MEM_LDR_X;
  a64_memop_t store = width == ASM_W32 ? A64_MEM_STR_W : A64_MEM_STR_X;
  load_store(m, load, VALUE_SCRATCH, address);
  uint32_t imm12;
  bool shift12;
  if (arithmetic_immediate(unsigned_abs(imm), &imm12, &shift12)) {
    arithmetic_imm(m, ARITH_ADD, width, VALUE_SCRATCH, VALUE_SCRATCH, imm);
  } else {
    mov_imm(m, width, ADDRESS_SCRATCH, (uint64_t)(int64_t)imm);
    a64_add(&m->a, width, VALUE_SCRATCH, VALUE_SCRATCH, ADDRESS_SCRATCH, A64_SHIFT_LSL, 0);
  }
  load_store(m, store, VALUE_SCRATCH, address);
}

static void move_width(masm_a64_t *m, asm_width_t width, asm_gpr_t dst, asm_gpr_t src) {
  if (dst != src || width == ASM_W32) a64_mov(&m->a, width, dst, src);
}

static void arithmetic_imm_set_flags(masm_a64_t *m, arith_op_t op, asm_width_t width, asm_gpr_t dst, asm_gpr_t lhs, int64_t imm) {
  uint32_t imm12;
  bool shift12;
  if (arithmetic_immediate(unsigned_abs(imm), &imm12, &shift12)) {
    if (imm < 0) op = opposite(op);
    if (op == ARITH_ADD) a64_adds_imm(&m->a, width, dst, lhs, imm12, shift12);
    else a64_subs_imm(&m->a, width, dst, lhs, imm12, shift12);
    return;
  }
  mov_imm(m, width, VALUE_SCRATCH, (uint64_t)imm);
  if (op == ARITH_ADD) a64_adds(&m->a, width, dst, lhs, VALUE_SCRATCH, A64_SHIFT_LSL, 0);
  else a64_subs(&m->a, width, dst, lhs, VALUE_SCRATCH, A64_SHIFT_LSL, 0);
}

static void logical_imm(masm_a64_t *m, logical_op_t op, asm_width_t width, asm_gpr_t dst, asm_gpr_t lhs, uint64_t imm) {
  uint64_t all_ones = width == ASM_W32 ? UINT32_MAX : UINT64_MAX;
  imm &= all_ones;
  if (imm == 0) {
    if (op == LOGICAL_AND) a64_movz(&m->a, width, dst, 0, 0);
    else move_width(m, width, dst, lhs);
    return;
  }
  if (imm == all_ones) {
    switch (op) {
      case LOGICAL_AND: move_width(m, width, dst, lhs); break;
      case LOGICAL_OR: a64_movn(&m->a, width, dst, 0, 0); break;
      case LOGICAL_XOR: a64_mvn(&m->a, width, dst, lhs); break;
    }
    return;
  }
  uint32_t encoding;
  if (a64_encode_logical_immediate(imm, width, &encoding)) {
    switch (op) {
      case LOGICAL_AND: a64_and_imm(&m->a, width, dst, lhs, imm); break;
      case LOGICAL_OR: a64_orr_imm(&m->a, width, dst, lhs, imm); break;
      case LOGICAL_XOR: a64_eor_imm(&m->a, width, dst, lhs, imm); break;
    }
    return;
  }
  mov_imm(m, width, VALUE_SCRATCH, imm);
  switch (op) {
    case LOGICAL_AND: a64_and(&m->a, width, dst, lhs, VALUE_SCRATCH, A64_SHIFT_LSL, 0); break;
    case LOGICAL_OR: a64_orr(&m->a, width, dst, lhs, VALUE_SCRATCH, A64_SHIFT_LSL, 0); break;
    case LOGICAL_XOR: a64_eor(&m->a, width, dst, lhs, VALUE_SCRATCH, A64_SHIFT_LSL, 0); break;
  }
}

static void compare_imm(masm_a64_t *m, asm_width_t width, asm_gpr_t lhs, int64_t imm) {
  arithmetic_imm_set_flags(m, ARITH_SUB, width, A64_XZR, lhs, imm);
}

static void shift_imm(masm_a64_t *m, a64_shift_t op, asm_width_t width, asm_gpr_t dst, asm_gpr_t src, uint8_t amount) {
  amount &= (uint8_t)(asm_width_bits(width) - 1);
  if (amount == 0) {
    move_width(m, width, dst, src);
    return;
  }
  switch (op) {
    case A64_SHIFT_LSL: a64_lsl_imm(&m->a, width, dst, src, amount); break;
    case A64_SHIFT_LSR: a64_lsr_imm(&m->a, width, dst, src, amount); break;
    case A64_SHIFT_ASR: a64_asr_imm(&m->a, width, dst, src, amount); break;
    case A64_SHIFT_ROR: asm_panic("unreachable");
  }
}

static void test_mask(masm_a64_t *m, asm_width_t width, asm_gpr_t value, uint64_t mask) {
  uint64_t all_ones = width == ASM_W32 ? UINT32_MAX : UINT64_MAX;
  uint32_t encoding;
  if (mask == all_ones) {
    a64_tst(&m->a, width, value, value);
  } else if (a64_encode_logical_immediate(mask, width, &encoding)) {
    a64_tst_imm(&m->a, width, value, mask);
  } else {
    mov_imm(m, width, VALUE_SCRATCH, mask);
    a64_tst(&m->a, width, value, VALUE_SCRATCH);
  }
}

static void branch_test(masm_a64_t *m, asm_width_t width, asm_cond_t cond, asm_gpr_t value, uint64_t mask, asm_label_t target) {
  bool zero_test = cond == ASM_COND_ZERO || cond == ASM_COND_EQUAL;
  bool non_zero_test = cond == ASM_COND_NON_ZERO || cond == ASM_COND_NOT_EQUAL;
  if (mask && (mask & (mask - 1)) == 0 && (zero_test || non_zero_test)) {
    uint8_t bit = (uint8_t)__builtin_ctzll(mask);
    if (zero_test) a64_tbz(&m->a, value, bit, target);
    else a64_tbnz(&m->a, value, bit, target);
    return;
  }
  test_mask(m, width, value, mask);
  a64_b_cond(&m->a, condition_code(cond), target);
}

static void branch_compare_imm(masm_a64_t *m, asm_width_t width, asm_cond_t cond, asm_gpr_t lhs, int64_t imm, asm_label_t target) {
  if (imm == 0) {
    if (cond == ASM_COND_EQUAL || cond == ASM_COND_ZERO) {
      a64_cbz(&m->a, width, lhs, target);
      return;
    }
    if (cond == ASM_COND_NOT_EQUAL || cond == ASM_COND_NON_ZERO) {
      a64_cbnz(&m->a, width, lhs, target);
      return;
    }
  }
  compare_imm(m, width, lhs, imm);
  a64_b_cond(&m->a, condition_code(cond), target);
}

static void set_double_condition(masm_a64_t *m, asm_dcond_t cond, asm_gpr_t dst) {
  double_flags_t flags = double_flags(cond);
  a64_cset(&m->a, ASM_W32, dst, flags.first);
  if (flags.either) a64_csinc(&m->a, ASM_W32, dst, dst, A64_XZR, a64_cond_invert(flags.second));
}

static void push_registers(masm_a64_t *m, asm_regset_t registers, bool is_fpr) {
  while (registers) {
    uint8_t x = asm_regset_first(registers);
    registers = asm_regset_without(registers, x);
    if (registers) {
      uint8_t y = asm_regset_first(registers);
      registers = asm_regset_without(registers, y);
      if (is_fpr) a64_stp_d(&m->a, x, y, a64_pair_pre_index(A64_SP, -16));
      else a64_stp(&m->a, x, y, a64_pair_pre_index(A64_SP, -16));
    } else if (is_fpr) {
      a64_str_d(&m->a, x, a64_mem_pre_index(A64_SP, -16));
    } else {
      a64_str(&m->a, ASM_W64, x, a64_mem_pre_index(A64_SP, -16));
    }
  }
}

static void pop_registers(masm_a64_t *m, asm_regset_t registers, bool is_fpr) {
  uint8_t list[32];
  uint32_t count = 0;
  ASM_REGSET_EACH(registers, reg) list[count++] = reg;
  uint32_t chunks = (count + 1) / 2;
  for (uint32_t c = chunks; c-- > 0;) {
    uint32_t i = 2 * c;
    if (i + 1 < count) {
      if (is_fpr) a64_ldp_d(&m->a, list[i], list[i + 1], a64_pair_post_index(A64_SP, 16));
      else a64_ldp(&m->a, list[i], list[i + 1], a64_pair_post_index(A64_SP, 16));
    } else if (is_fpr) {
      a64_ldr_d(&m->a, list[i], a64_mem_post_index(A64_SP, 16));
    } else {
      a64_ldr(&m->a, ASM_W64, list[i], a64_mem_post_index(A64_SP, 16));
    }
  }
}

void masm_a64_init(masm_a64_t *m) {
  *m = (masm_a64_t){ 0 };
  a64_init(&m->a);
}

void masm_a64_free(masm_a64_t *m) {
  a64_free(&m->a);
  free(m->constants);
  free(m->constants128);
  *m = (masm_a64_t){ 0 };
}

size_t masm_a64_offset(const masm_a64_t *m) { return a64_offset(&m->a); }
asm_label_t masm_a64_new_label(masm_a64_t *m) { return a64_new_label(&m->a); }
void masm_a64_bind(masm_a64_t *m, asm_label_t label) { a64_bind(&m->a, label); }
size_t masm_a64_label_offset(const masm_a64_t *m, asm_label_t label) { return a64_label_offset(&m->a, label); }

size_t masm_a64_patchable_nop(masm_a64_t *m) {
  size_t offset = a64_offset(&m->a);
  a64_nop(&m->a);
  return offset;
}

size_t masm_a64_jump_patch(size_t from, size_t to, uint8_t *out) {
  int64_t words = ((int64_t)to - (int64_t)from) / 4;
  ASM_CHECK(words >= -(INT64_C(1) << 25) && words < (INT64_C(1) << 25), "code is larger than 128 MiB");
  uint32_t word = 0x14000000 | ((uint32_t)words & 0x03ffffff);
  out[0] = (uint8_t)word;
  out[1] = (uint8_t)(word >> 8);
  out[2] = (uint8_t)(word >> 16);
  out[3] = (uint8_t)(word >> 24);
  return 4;
}

asm_error_t masm_a64_finish(masm_a64_t *m, asm_code_t *out) {
  size_t data_offset = a64_offset(&m->a);
  if (m->constant128_count) {
    a64_align_data(&m->a, 16);
    for (uint32_t i = 0; i < m->constant128_count; i++) {
      a64_bind(&m->a, m->constants128[i].label);
      a64_data_u64(&m->a, m->constants128[i].value.lo);
      a64_data_u64(&m->a, m->constants128[i].value.hi);
    }
  }
  if (m->constant_count) {
    a64_align_data(&m->a, 8);
    for (uint32_t i = 0; i < m->constant_count; i++) {
      a64_bind(&m->a, m->constants[i].label);
      a64_data_u64(&m->a, m->constants[i].value);
    }
  }
  asm_error_t error = a64_finish(&m->a, out);
  if (error == ASM_OK) out->data_offset = data_offset;
  masm_a64_free(m);
  return error;
}

void masm_a64_unreachable(masm_a64_t *m) {
  a64_udf(&m->a, 0);
}

void masm_a64_move64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src) {
  if (dst == src) return;
  if (dst == A64_SP || src == A64_SP) a64_mov_sp(&m->a, dst, src);
  else a64_mov(&m->a, ASM_W64, dst, src);
}

void masm_a64_move32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src) {
  a64_mov(&m->a, ASM_W32, dst, src);
}

void masm_a64_move_imm64(masm_a64_t *m, asm_gpr_t dst, uint64_t imm) {
  mov_imm(m, ASM_W64, dst, imm);
}

void masm_a64_move_imm32(masm_a64_t *m, asm_gpr_t dst, uint32_t imm) {
  a64_mov_imm(&m->a, ASM_W32, dst, imm);
}

void masm_a64_sign_extend32_to_64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src) {
  a64_sxtw(&m->a, dst, src);
}

void masm_a64_load8(masm_a64_t *m, asm_gpr_t dst, asm_address_t address) { load_store(m, A64_MEM_LDRB, dst, address); }
void masm_a64_load8_sign_extend(masm_a64_t *m, asm_gpr_t dst, asm_address_t address) { load_store(m, A64_MEM_LDRSB_X, dst, address); }
void masm_a64_load16(masm_a64_t *m, asm_gpr_t dst, asm_address_t address) { load_store(m, A64_MEM_LDRH, dst, address); }
void masm_a64_load16_sign_extend(masm_a64_t *m, asm_gpr_t dst, asm_address_t address) { load_store(m, A64_MEM_LDRSH_X, dst, address); }
void masm_a64_load32(masm_a64_t *m, asm_gpr_t dst, asm_address_t address) { load_store(m, A64_MEM_LDR_W, dst, address); }
void masm_a64_load32_sign_extend(masm_a64_t *m, asm_gpr_t dst, asm_address_t address) { load_store(m, A64_MEM_LDRSW, dst, address); }
void masm_a64_load64(masm_a64_t *m, asm_gpr_t dst, asm_address_t address) { load_store(m, A64_MEM_LDR_X, dst, address); }
void masm_a64_store8(masm_a64_t *m, asm_address_t address, asm_gpr_t src) { load_store(m, A64_MEM_STRB, src, address); }
void masm_a64_store16(masm_a64_t *m, asm_address_t address, asm_gpr_t src) { load_store(m, A64_MEM_STRH, src, address); }
void masm_a64_store32(masm_a64_t *m, asm_address_t address, asm_gpr_t src) { load_store(m, A64_MEM_STR_W, src, address); }
void masm_a64_store64(masm_a64_t *m, asm_address_t address, asm_gpr_t src) { load_store(m, A64_MEM_STR_X, src, address); }

void masm_a64_store_imm32(masm_a64_t *m, asm_address_t address, uint32_t imm) {
  asm_gpr_t source = A64_XZR;
  if (imm != 0) {
    a64_mov_imm(&m->a, ASM_W32, VALUE_SCRATCH, imm);
    source = VALUE_SCRATCH;
  }
  load_store(m, A64_MEM_STR_W, source, address);
}

void masm_a64_store_imm64(masm_a64_t *m, asm_address_t address, uint64_t imm) {
  asm_gpr_t source = A64_XZR;
  if (imm != 0) {
    mov_imm(m, ASM_W64, VALUE_SCRATCH, imm);
    source = VALUE_SCRATCH;
  }
  load_store(m, A64_MEM_STR_X, source, address);
}

void masm_a64_add32_to_memory_imm(masm_a64_t *m, asm_address_t address, int32_t imm) {
  add_to_memory_imm(m, ASM_W32, address, imm);
}

void masm_a64_add64_to_memory_imm(masm_a64_t *m, asm_address_t address, int32_t imm) {
  add_to_memory_imm(m, ASM_W64, address, imm);
}

void masm_a64_load_effective_address(masm_a64_t *m, asm_gpr_t dst, asm_address_t address) {
  asm_gpr_t base = address.base;
  if (address.has_index) {
    a64_add_extended(&m->a, ASM_W64, dst, address.base, address.index, A64_EXTEND_UXTX, (uint8_t)address.scale);
    base = dst;
  }
  if (address.disp != 0) arithmetic_imm(m, ARITH_ADD, ASM_W64, dst, base, address.disp);
  else masm_a64_move64(m, dst, base);
}

void masm_a64_load_double(masm_a64_t *m, asm_fpr_t dst, asm_address_t address) { load_store(m, A64_MEM_LDR_D, dst, address); }
void masm_a64_store_double(masm_a64_t *m, asm_address_t address, asm_fpr_t src) { load_store(m, A64_MEM_STR_D, src, address); }

void masm_a64_load_imm128(masm_a64_t *m, asm_fpr_t dst, asm_u128_t imm) {
  a64_ldr_literal_quad(&m->a, dst, constant128(m, imm));
}

void masm_a64_store128(masm_a64_t *m, asm_address_t address, asm_fpr_t src) {
  load_store(m, A64_MEM_STR_Q, src, address);
}

void masm_a64_load_float_as_double(masm_a64_t *m, asm_fpr_t dst, asm_address_t address) {
  load_store(m, A64_MEM_LDR_S, dst, address);
  a64_fcvt_double_from_single(&m->a, dst, dst);
}

void masm_a64_store_double_as_float(masm_a64_t *m, asm_address_t address, asm_fpr_t src) {
  a64_fcvt_single_from_double(&m->a, FP_SCRATCH, src);
  load_store(m, A64_MEM_STR_S, FP_SCRATCH, address);
}

void masm_a64_add32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) { a64_add(&m->a, ASM_W32, dst, lhs, rhs, A64_SHIFT_LSL, 0); }
void masm_a64_add32_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm) { arithmetic_imm(m, ARITH_ADD, ASM_W32, dst, lhs, imm); }
void masm_a64_add64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) { a64_add(&m->a, ASM_W64, dst, lhs, rhs, A64_SHIFT_LSL, 0); }
void masm_a64_add64_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int64_t imm) { arithmetic_imm(m, ARITH_ADD, ASM_W64, dst, lhs, imm); }
void masm_a64_sub32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) { a64_sub(&m->a, ASM_W32, dst, lhs, rhs, A64_SHIFT_LSL, 0); }
void masm_a64_sub32_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm) { arithmetic_imm(m, ARITH_SUB, ASM_W32, dst, lhs, imm); }
void masm_a64_sub64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) { a64_sub(&m->a, ASM_W64, dst, lhs, rhs, A64_SHIFT_LSL, 0); }
void masm_a64_sub64_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int64_t imm) { arithmetic_imm(m, ARITH_SUB, ASM_W64, dst, lhs, imm); }
void masm_a64_and32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) { a64_and(&m->a, ASM_W32, dst, lhs, rhs, A64_SHIFT_LSL, 0); }
void masm_a64_and32_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint32_t imm) { logical_imm(m, LOGICAL_AND, ASM_W32, dst, lhs, imm); }
void masm_a64_and64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) { a64_and(&m->a, ASM_W64, dst, lhs, rhs, A64_SHIFT_LSL, 0); }
void masm_a64_and64_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint64_t imm) { logical_imm(m, LOGICAL_AND, ASM_W64, dst, lhs, imm); }
void masm_a64_or32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) { a64_orr(&m->a, ASM_W32, dst, lhs, rhs, A64_SHIFT_LSL, 0); }
void masm_a64_or32_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint32_t imm) { logical_imm(m, LOGICAL_OR, ASM_W32, dst, lhs, imm); }
void masm_a64_or64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) { a64_orr(&m->a, ASM_W64, dst, lhs, rhs, A64_SHIFT_LSL, 0); }
void masm_a64_or64_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint64_t imm) { logical_imm(m, LOGICAL_OR, ASM_W64, dst, lhs, imm); }
void masm_a64_xor32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) { a64_eor(&m->a, ASM_W32, dst, lhs, rhs, A64_SHIFT_LSL, 0); }
void masm_a64_xor32_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint32_t imm) { logical_imm(m, LOGICAL_XOR, ASM_W32, dst, lhs, imm); }
void masm_a64_xor64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) { a64_eor(&m->a, ASM_W64, dst, lhs, rhs, A64_SHIFT_LSL, 0); }
void masm_a64_xor64_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint64_t imm) { logical_imm(m, LOGICAL_XOR, ASM_W64, dst, lhs, imm); }
void masm_a64_mul32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) { a64_mul(&m->a, ASM_W32, dst, lhs, rhs); }

void masm_a64_remainder32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_gpr_t quotient) {
  a64_sdiv(&m->a, ASM_W32, quotient, lhs, rhs);
  a64_msub(&m->a, ASM_W32, dst, quotient, rhs, lhs);
}

void masm_a64_mul32_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint32_t imm) {
  mov_imm(m, ASM_W32, VALUE_SCRATCH, imm);
  a64_mul(&m->a, ASM_W32, dst, lhs, VALUE_SCRATCH);
}

void masm_a64_mul64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) { a64_mul(&m->a, ASM_W64, dst, lhs, rhs); }
void masm_a64_neg32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src) { a64_neg(&m->a, ASM_W32, dst, src); }
void masm_a64_neg64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src) { a64_neg(&m->a, ASM_W64, dst, src); }
void masm_a64_not32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src) { a64_mvn(&m->a, ASM_W32, dst, src); }
void masm_a64_not64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src) { a64_mvn(&m->a, ASM_W64, dst, src); }

void masm_a64_shl32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount) { a64_lsl(&m->a, ASM_W32, dst, src, amount); }
void masm_a64_shl32_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount) { shift_imm(m, A64_SHIFT_LSL, ASM_W32, dst, src, amount); }
void masm_a64_shl64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount) { a64_lsl(&m->a, ASM_W64, dst, src, amount); }
void masm_a64_shl64_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount) { shift_imm(m, A64_SHIFT_LSL, ASM_W64, dst, src, amount); }
void masm_a64_shr32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount) { a64_lsr(&m->a, ASM_W32, dst, src, amount); }
void masm_a64_shr32_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount) { shift_imm(m, A64_SHIFT_LSR, ASM_W32, dst, src, amount); }
void masm_a64_shr64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount) { a64_lsr(&m->a, ASM_W64, dst, src, amount); }
void masm_a64_shr64_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount) { shift_imm(m, A64_SHIFT_LSR, ASM_W64, dst, src, amount); }
void masm_a64_sar32(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount) { a64_asr(&m->a, ASM_W32, dst, src, amount); }
void masm_a64_sar32_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount) { shift_imm(m, A64_SHIFT_ASR, ASM_W32, dst, src, amount); }
void masm_a64_sar64(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount) { a64_asr(&m->a, ASM_W64, dst, src, amount); }
void masm_a64_sar64_imm(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount) { shift_imm(m, A64_SHIFT_ASR, ASM_W64, dst, src, amount); }

void masm_a64_branch_add32_overflow(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t overflow) {
  a64_adds(&m->a, ASM_W32, dst, lhs, rhs, A64_SHIFT_LSL, 0);
  a64_b_cond(&m->a, A64_COND_VS, overflow);
}

void masm_a64_branch_add32_imm_overflow(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm, asm_label_t overflow) {
  arithmetic_imm_set_flags(m, ARITH_ADD, ASM_W32, dst, lhs, imm);
  a64_b_cond(&m->a, A64_COND_VS, overflow);
}

void masm_a64_branch_sub32_overflow(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t overflow) {
  a64_subs(&m->a, ASM_W32, dst, lhs, rhs, A64_SHIFT_LSL, 0);
  a64_b_cond(&m->a, A64_COND_VS, overflow);
}

void masm_a64_branch_sub32_imm_overflow(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm, asm_label_t overflow) {
  arithmetic_imm_set_flags(m, ARITH_SUB, ASM_W32, dst, lhs, imm);
  a64_b_cond(&m->a, A64_COND_VS, overflow);
}

void masm_a64_branch_mul32_overflow(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t overflow) {
  a64_smull(&m->a, VALUE_SCRATCH, lhs, rhs);
  a64_check_smull_overflow(&m->a, VALUE_SCRATCH);
  a64_b_cond(&m->a, A64_COND_NE, overflow);
  a64_mov(&m->a, ASM_W32, dst, VALUE_SCRATCH);
}

void masm_a64_branch_neg32_overflow(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_label_t overflow) {
  a64_negs(&m->a, ASM_W32, dst, src);
  a64_b_cond(&m->a, A64_COND_VS, overflow);
}

void masm_a64_branch_add64_overflow(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t overflow) {
  a64_adds(&m->a, ASM_W64, dst, lhs, rhs, A64_SHIFT_LSL, 0);
  a64_b_cond(&m->a, A64_COND_VS, overflow);
}

void masm_a64_branch_sub64_overflow(masm_a64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t overflow) {
  a64_subs(&m->a, ASM_W64, dst, lhs, rhs, A64_SHIFT_LSL, 0);
  a64_b_cond(&m->a, A64_COND_VS, overflow);
}

void masm_a64_branch32(masm_a64_t *m, asm_cond_t cond, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t target) {
  a64_cmp(&m->a, ASM_W32, lhs, rhs);
  a64_b_cond(&m->a, condition_code(cond), target);
}

void masm_a64_branch32_imm(masm_a64_t *m, asm_cond_t cond, asm_gpr_t lhs, int32_t imm, asm_label_t target) {
  branch_compare_imm(m, ASM_W32, cond, lhs, imm, target);
}

void masm_a64_branch64(masm_a64_t *m, asm_cond_t cond, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t target) {
  a64_cmp(&m->a, ASM_W64, lhs, rhs);
  a64_b_cond(&m->a, condition_code(cond), target);
}

void masm_a64_branch_if_stack_pointer_below(masm_a64_t *m, asm_address_t limit, asm_label_t target) {
  load_store(m, A64_MEM_LDR_X, VALUE_SCRATCH, limit);
  a64_subs_extended(&m->a, ASM_W64, A64_XZR, A64_SP, VALUE_SCRATCH, A64_EXTEND_UXTX, 0);
  a64_b_cond(&m->a, condition_code(ASM_COND_BELOW), target);
}

void masm_a64_branch64_imm(masm_a64_t *m, asm_cond_t cond, asm_gpr_t lhs, int64_t imm, asm_label_t target) {
  branch_compare_imm(m, ASM_W64, cond, lhs, imm, target);
}

void masm_a64_branch32_memory_imm(masm_a64_t *m, asm_cond_t cond, asm_address_t address, int32_t imm, asm_label_t target) {
  load_store(m, A64_MEM_LDR_W, ADDRESS_SCRATCH, address);
  compare_imm(m, ASM_W32, ADDRESS_SCRATCH, imm);
  a64_b_cond(&m->a, condition_code(cond), target);
}

void masm_a64_branch16_memory_imm(masm_a64_t *m, asm_cond_t cond, asm_address_t address, uint16_t imm, asm_label_t target) {
  load_store(m, A64_MEM_LDRH, ADDRESS_SCRATCH, address);
  compare_imm(m, ASM_W32, ADDRESS_SCRATCH, imm);
  a64_b_cond(&m->a, condition_code(cond), target);
}

void masm_a64_branch64_memory(masm_a64_t *m, asm_cond_t cond, asm_address_t address, asm_gpr_t rhs, asm_label_t target) {
  load_store(m, A64_MEM_LDR_X, ADDRESS_SCRATCH, address);
  a64_cmp(&m->a, ASM_W64, ADDRESS_SCRATCH, rhs);
  a64_b_cond(&m->a, condition_code(cond), target);
}

void masm_a64_branch64_memory_imm(masm_a64_t *m, asm_cond_t cond, asm_address_t address, int64_t imm, asm_label_t target) {
  load_store(m, A64_MEM_LDR_X, ADDRESS_SCRATCH, address);
  compare_imm(m, ASM_W64, ADDRESS_SCRATCH, imm);
  a64_b_cond(&m->a, condition_code(cond), target);
}

void masm_a64_branch_test32(masm_a64_t *m, asm_cond_t cond, asm_gpr_t value, uint32_t mask, asm_label_t target) {
  branch_test(m, ASM_W32, cond, value, mask, target);
}

void masm_a64_branch_test64(masm_a64_t *m, asm_cond_t cond, asm_gpr_t value, uint64_t mask, asm_label_t target) {
  branch_test(m, ASM_W64, cond, value, mask, target);
}

void masm_a64_compare32_set(masm_a64_t *m, asm_cond_t cond, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) {
  a64_cmp(&m->a, ASM_W32, lhs, rhs);
  a64_cset(&m->a, ASM_W32, dst, condition_code(cond));
}

void masm_a64_compare32_imm_set(masm_a64_t *m, asm_cond_t cond, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm) {
  compare_imm(m, ASM_W32, lhs, imm);
  a64_cset(&m->a, ASM_W32, dst, condition_code(cond));
}

void masm_a64_compare64_set(masm_a64_t *m, asm_cond_t cond, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) {
  a64_cmp(&m->a, ASM_W64, lhs, rhs);
  a64_cset(&m->a, ASM_W32, dst, condition_code(cond));
}

void masm_a64_compare64_imm_set(masm_a64_t *m, asm_cond_t cond, asm_gpr_t dst, asm_gpr_t lhs, int64_t imm) {
  compare_imm(m, ASM_W64, lhs, imm);
  a64_cset(&m->a, ASM_W32, dst, condition_code(cond));
}

void masm_a64_select32(
  masm_a64_t *m, asm_cond_t cond, asm_gpr_t lhs, asm_gpr_t rhs,
  asm_gpr_t dst, asm_gpr_t if_true, asm_gpr_t if_false
) {
  a64_cmp(&m->a, ASM_W32, lhs, rhs);
  a64_csel(&m->a, ASM_W32, dst, if_true, if_false, condition_code(cond));
}

void masm_a64_select64(
  masm_a64_t *m, asm_cond_t cond, asm_gpr_t lhs, asm_gpr_t rhs,
  asm_gpr_t dst, asm_gpr_t if_true, asm_gpr_t if_false
) {
  a64_cmp(&m->a, ASM_W64, lhs, rhs);
  a64_csel(&m->a, ASM_W64, dst, if_true, if_false, condition_code(cond));
}

void masm_a64_jump(masm_a64_t *m, asm_label_t target) {
  a64_b(&m->a, target);
}

void masm_a64_jump_absolute(masm_a64_t *m, uint64_t address) {
  mov_imm(m, ASM_W64, ADDRESS_SCRATCH, address);
  a64_br(&m->a, ADDRESS_SCRATCH);
}

void masm_a64_call(masm_a64_t *m, asm_label_t target) {
  a64_bl(&m->a, target);
}

void masm_a64_call_register(masm_a64_t *m, asm_gpr_t target) {
  a64_blr(&m->a, target);
}

void masm_a64_call_absolute(masm_a64_t *m, uint64_t address) {
  mov_imm(m, ASM_W64, ADDRESS_SCRATCH, address);
  a64_blr(&m->a, ADDRESS_SCRATCH);
}

void masm_a64_ret(masm_a64_t *m) {
  a64_ret(&m->a);
}

void masm_a64_move_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t src) {
  if (dst != src) a64_fmov(&m->a, dst, src);
}

void masm_a64_move_double_imm(masm_a64_t *m, asm_fpr_t dst, double value) {
  uint64_t bits = asm_f64_bits(value);
  uint8_t imm8;
  if (a64_encode_fp_immediate(value, &imm8)) a64_fmov_imm(&m->a, dst, value);
  else if (bits == 0) a64_fmov_from_gpr(&m->a, dst, A64_XZR);
  else a64_ldr_literal_double(&m->a, dst, constant(m, bits));
}

void masm_a64_move_gpr_to_double(masm_a64_t *m, asm_fpr_t dst, asm_gpr_t src) { a64_fmov_from_gpr(&m->a, dst, src); }
void masm_a64_move_double_to_gpr(masm_a64_t *m, asm_gpr_t dst, asm_fpr_t src) { a64_fmov_to_gpr(&m->a, dst, src); }
void masm_a64_add_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs) { a64_fadd(&m->a, dst, lhs, rhs); }
void masm_a64_sub_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs) { a64_fsub(&m->a, dst, lhs, rhs); }
void masm_a64_min_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs) { a64_fmin(&m->a, dst, lhs, rhs); }
void masm_a64_max_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs) { a64_fmax(&m->a, dst, lhs, rhs); }
void masm_a64_mul_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs) { a64_fmul(&m->a, dst, lhs, rhs); }
void masm_a64_div_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs) { a64_fdiv(&m->a, dst, lhs, rhs); }
void masm_a64_sqrt_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t src) { a64_fsqrt(&m->a, dst, src); }
void masm_a64_floor_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t src) { a64_frintm(&m->a, dst, src); }
void masm_a64_ceil_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t src) { a64_frintp(&m->a, dst, src); }
void masm_a64_trunc_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t src) { a64_frintz(&m->a, dst, src); }
void masm_a64_neg_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t src) { a64_fneg(&m->a, dst, src); }
void masm_a64_abs_double(masm_a64_t *m, asm_fpr_t dst, asm_fpr_t src) { a64_fabs(&m->a, dst, src); }
void masm_a64_convert_int32_to_double(masm_a64_t *m, asm_fpr_t dst, asm_gpr_t src) { a64_scvtf(&m->a, ASM_W32, dst, src); }
void masm_a64_convert_int64_to_double(masm_a64_t *m, asm_fpr_t dst, asm_gpr_t src) { a64_scvtf(&m->a, ASM_W64, dst, src); }

void masm_a64_branch_convert_double_to_int32(
  masm_a64_t *m, asm_gpr_t dst, asm_fpr_t src,
  asm_label_t fail, asm_negative_zero_t negative_zero
) {
  a64_fcvtzs(&m->a, ASM_W32, dst, src);
  a64_scvtf(&m->a, ASM_W32, FP_SCRATCH, dst);
  a64_fcmp(&m->a, FP_SCRATCH, src);
  a64_b_cond(&m->a, A64_COND_NE, fail);
  if (negative_zero == ASM_NEGATIVE_ZERO_FAIL) {
    asm_label_t done = a64_new_label(&m->a);
    a64_cbnz(&m->a, ASM_W32, dst, done);
    a64_fmov_to_gpr(&m->a, VALUE_SCRATCH, src);
    a64_tbnz(&m->a, VALUE_SCRATCH, 63, fail);
    a64_bind(&m->a, done);
  }
}

void masm_a64_truncate_double_to_int64(masm_a64_t *m, asm_gpr_t dst, asm_fpr_t src) {
  a64_fcvtzs(&m->a, ASM_W64, dst, src);
}

bool masm_a64_has_js_convert(void) {
  static int has = -1;
  if (has < 0) {
#if defined(__aarch64__) && defined(__APPLE__)
    int value = 0;
    size_t size = sizeof value;
    has = sysctlbyname("hw.optional.arm.FEAT_JSCVT", &value, &size, NULL, 0) == 0 && value;
#elif defined(__aarch64__) && defined(__linux__)
    has = (getauxval(AT_HWCAP) & (1UL << 13)) != 0;
#else
    has = 0;
#endif
  }
  return has;
}

void masm_a64_js_convert_double_to_int32(masm_a64_t *m, asm_gpr_t dst, asm_fpr_t src) {
  a64_fjcvtzs(&m->a, dst, src);
}

void masm_a64_branch_js_convert_double_to_int32(masm_a64_t *m, asm_gpr_t dst, asm_fpr_t src, asm_label_t fail) {
  a64_fjcvtzs(&m->a, dst, src);
  a64_b_cond(&m->a, A64_COND_NE, fail);
}

void masm_a64_branch_double(masm_a64_t *m, asm_dcond_t cond, asm_fpr_t lhs, asm_fpr_t rhs, asm_label_t target) {
  double_flags_t flags = double_flags(cond);
  a64_fcmp(&m->a, lhs, rhs);
  a64_b_cond(&m->a, flags.first, target);
  if (flags.either) a64_b_cond(&m->a, flags.second, target);
}

void masm_a64_compare_double_set(masm_a64_t *m, asm_dcond_t cond, asm_gpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs) {
  a64_fcmp(&m->a, lhs, rhs);
  set_double_condition(m, cond, dst);
}

asm_frame_t masm_a64_frame(asm_regset_t saved_gprs, asm_regset_t saved_fprs, uint32_t locals_size) {
  saved_gprs = asm_regset_without(asm_regset_without(asm_regset_without(saved_gprs, A64_SP), A64_FP), A64_LR);
  uint32_t gpr_area = 16 * ((asm_regset_len(saved_gprs) + 1) / 2);
  uint32_t fpr_area = 16 * ((asm_regset_len(saved_fprs) + 1) / 2);
  locals_size = (locals_size + 15) & ~UINT32_C(15);
  return (asm_frame_t){
    .saved_gprs = saved_gprs,
    .saved_fprs = saved_fprs,
    .locals_size = locals_size,
    .caller_stack_offset = locals_size + fpr_area + gpr_area + 16,
  };
}

void masm_a64_emit_prologue(masm_a64_t *m, const asm_frame_t *frame) {
  a64_stp(&m->a, A64_FP, A64_LR, a64_pair_pre_index(A64_SP, -16));
  a64_mov_sp(&m->a, A64_FP, A64_SP);
  push_registers(m, frame->saved_gprs, false);
  push_registers(m, frame->saved_fprs, true);
  if (frame->locals_size != 0) arithmetic_imm(m, ARITH_SUB, ASM_W64, A64_SP, A64_SP, frame->locals_size);
}

void masm_a64_emit_epilogue(masm_a64_t *m, const asm_frame_t *frame) {
  if (frame->locals_size != 0) arithmetic_imm(m, ARITH_ADD, ASM_W64, A64_SP, A64_SP, frame->locals_size);
  pop_registers(m, frame->saved_fprs, true);
  pop_registers(m, frame->saved_gprs, false);
  a64_ldp(&m->a, A64_FP, A64_LR, a64_pair_post_index(A64_SP, 16));
}
