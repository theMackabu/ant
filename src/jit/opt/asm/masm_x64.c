#include "x64.h"
#include <stdlib.h>

static constexpr asm_gpr_t SCRATCH = MASM_X64_SCRATCH;
static constexpr asm_gpr_t SCRATCH2 = MASM_X64_SCRATCH2;
static constexpr asm_fpr_t FP_SCRATCH = MASM_X64_FP_SCRATCH;

static x64_cond_t condition_code(asm_cond_t cond) {
  switch (cond) {
    case ASM_COND_EQUAL:
    case ASM_COND_ZERO: return X64_COND_EQUAL;
    case ASM_COND_NOT_EQUAL:
    case ASM_COND_NON_ZERO: return X64_COND_NOT_EQUAL;
    case ASM_COND_LESS_THAN: return X64_COND_LESS;
    case ASM_COND_LESS_THAN_OR_EQUAL: return X64_COND_LESS_OR_EQUAL;
    case ASM_COND_GREATER_THAN: return X64_COND_GREATER;
    case ASM_COND_GREATER_THAN_OR_EQUAL: return X64_COND_GREATER_OR_EQUAL;
    case ASM_COND_BELOW: return X64_COND_BELOW;
    case ASM_COND_BELOW_OR_EQUAL: return X64_COND_BELOW_OR_EQUAL;
    case ASM_COND_ABOVE: return X64_COND_ABOVE;
    case ASM_COND_ABOVE_OR_EQUAL: return X64_COND_ABOVE_OR_EQUAL;
    case ASM_COND_OVERFLOW: return X64_COND_OVERFLOW;
    case ASM_COND_NO_OVERFLOW: return X64_COND_NO_OVERFLOW;
    case ASM_COND_NEGATIVE: return X64_COND_SIGN;
    case ASM_COND_POSITIVE_OR_ZERO: return X64_COND_NOT_SIGN;
  }
  return X64_COND_EQUAL;
}

typedef enum {
  DOUBLE_FLAGS_SINGLE,
  DOUBLE_FLAGS_BOTH,
  DOUBLE_FLAGS_EITHER,
} double_flags_kind_t;

typedef struct {
  double_flags_kind_t kind;
  bool swap;
  x64_cond_t first;
  x64_cond_t second;
} double_flags_t;

static double_flags_t double_flags(asm_dcond_t cond) {
  switch (cond) {
    case ASM_DCOND_EQUAL: return (double_flags_t){ DOUBLE_FLAGS_BOTH, false, X64_COND_EQUAL, X64_COND_NOT_PARITY };
    case ASM_DCOND_NOT_EQUAL: return (double_flags_t){ DOUBLE_FLAGS_BOTH, false, X64_COND_NOT_EQUAL, X64_COND_NOT_PARITY };
    case ASM_DCOND_LESS_THAN: return (double_flags_t){ DOUBLE_FLAGS_SINGLE, true, X64_COND_ABOVE, 0 };
    case ASM_DCOND_LESS_THAN_OR_EQUAL: return (double_flags_t){ DOUBLE_FLAGS_SINGLE, true, X64_COND_ABOVE_OR_EQUAL, 0 };
    case ASM_DCOND_GREATER_THAN: return (double_flags_t){ DOUBLE_FLAGS_SINGLE, false, X64_COND_ABOVE, 0 };
    case ASM_DCOND_GREATER_THAN_OR_EQUAL: return (double_flags_t){ DOUBLE_FLAGS_SINGLE, false, X64_COND_ABOVE_OR_EQUAL, 0 };
    case ASM_DCOND_EQUAL_OR_UNORDERED: return (double_flags_t){ DOUBLE_FLAGS_SINGLE, false, X64_COND_EQUAL, 0 };
    case ASM_DCOND_NOT_EQUAL_OR_UNORDERED: return (double_flags_t){ DOUBLE_FLAGS_EITHER, false, X64_COND_NOT_EQUAL, X64_COND_PARITY };
    case ASM_DCOND_LESS_THAN_OR_UNORDERED: return (double_flags_t){ DOUBLE_FLAGS_SINGLE, false, X64_COND_BELOW, 0 };
    case ASM_DCOND_LESS_THAN_OR_EQUAL_OR_UNORDERED: return (double_flags_t){ DOUBLE_FLAGS_SINGLE, false, X64_COND_BELOW_OR_EQUAL, 0 };
    case ASM_DCOND_GREATER_THAN_OR_UNORDERED: return (double_flags_t){ DOUBLE_FLAGS_SINGLE, true, X64_COND_BELOW, 0 };
    case ASM_DCOND_GREATER_THAN_OR_EQUAL_OR_UNORDERED: return (double_flags_t){ DOUBLE_FLAGS_SINGLE, true, X64_COND_BELOW_OR_EQUAL, 0 };
    case ASM_DCOND_ORDERED: return (double_flags_t){ DOUBLE_FLAGS_SINGLE, false, X64_COND_NOT_PARITY, 0 };
    case ASM_DCOND_UNORDERED: return (double_flags_t){ DOUBLE_FLAGS_SINGLE, false, X64_COND_PARITY, 0 };
  }
  return (double_flags_t){ DOUBLE_FLAGS_SINGLE, false, X64_COND_PARITY, 0 };
}

static inline bool fits_i32(int64_t value) {
  return value >= INT32_MIN && value <= INT32_MAX;
}

static void mov_if_needed(masm_x64_t *m, asm_width_t width, asm_gpr_t dst, asm_gpr_t src) {
  if (dst != src) x64_mov_rr(&m->a, width, dst, src);
}

static void commutative(masm_x64_t *m, x64_alu_t op, asm_width_t width, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) {
  if (dst == lhs) {
    x64_alu_rr(&m->a, op, width, dst, rhs);
  } else if (dst == rhs) {
    x64_alu_rr(&m->a, op, width, dst, lhs);
  } else {
    x64_mov_rr(&m->a, width, dst, lhs);
    x64_alu_rr(&m->a, op, width, dst, rhs);
  }
}

static void subtract(masm_x64_t *m, asm_width_t width, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) {
  if (dst == lhs) {
    x64_sub_rr(&m->a, width, dst, rhs);
  } else if (dst == rhs) {
    x64_mov_rr(&m->a, width, SCRATCH, lhs);
    x64_sub_rr(&m->a, width, SCRATCH, rhs);
    x64_mov_rr(&m->a, width, dst, SCRATCH);
  } else {
    x64_mov_rr(&m->a, width, dst, lhs);
    x64_sub_rr(&m->a, width, dst, rhs);
  }
}

static void with_imm32(masm_x64_t *m, x64_alu_t op, asm_width_t width, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm) {
  mov_if_needed(m, width, dst, lhs);
  x64_alu_ri(&m->a, op, width, dst, imm);
}

static asm_label_t constant(masm_x64_t *m, uint64_t value) {
  for (uint32_t i = 0; i < m->constant_count; i++)
    if (m->constants[i].value == value) return m->constants[i].label;
  if (m->constant_count == m->constant_cap) {
    uint32_t cap = m->constant_cap ? m->constant_cap * 2 : 8;
    masm_constant_t *grown = realloc(m->constants, cap * sizeof(*grown));
    if (!grown) asm_panic("out of memory");
    m->constants = grown;
    m->constant_cap = cap;
  }
  asm_label_t label = x64_new_label(&m->a);
  m->constants[m->constant_count++] = (masm_constant_t){ value, label };
  return label;
}

static asm_label_t constant128(masm_x64_t *m, asm_u128_t value) {
  for (uint32_t i = 0; i < m->constant128_count; i++)
    if (m->constants128[i].value.lo == value.lo && m->constants128[i].value.hi == value.hi) return m->constants128[i].label;
  if (m->constant128_count == m->constant128_cap) {
    uint32_t cap = m->constant128_cap ? m->constant128_cap * 2 : 4;
    masm_constant128_t *grown = realloc(m->constants128, cap * sizeof(*grown));
    if (!grown) asm_panic("out of memory");
    m->constants128 = grown;
    m->constant128_cap = cap;
  }
  asm_label_t label = x64_new_label(&m->a);
  m->constants128[m->constant128_count++] = (masm_constant128_t){ value, label };
  return label;
}

static void with_imm64(masm_x64_t *m, x64_alu_t op, asm_gpr_t dst, asm_gpr_t lhs, int64_t imm) {
  if (fits_i32(imm)) {
    with_imm32(m, op, ASM_W64, dst, lhs, (int32_t)imm);
    return;
  }
  mov_if_needed(m, ASM_W64, dst, lhs);
  x64_alu_r_label(&m->a, op, dst, constant(m, (uint64_t)imm));
}

static void shift_imm(masm_x64_t *m, x64_shift_t op, asm_width_t width, asm_gpr_t dst, asm_gpr_t src, uint8_t amount) {
  amount &= (uint8_t)(asm_width_bits(width) - 1);
  if (amount == 0) {
    if (dst != src || width == ASM_W32) x64_mov_rr(&m->a, width, dst, src);
    return;
  }
  mov_if_needed(m, width, dst, src);
  x64_shift_ri(&m->a, op, width, dst, amount);
}

static void shift_register(masm_x64_t *m, x64_shift_t op, asm_width_t width, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount) {
  if (amount == X64_RCX && dst != X64_RCX) {
    mov_if_needed(m, width, dst, src);
    x64_shift_rcl(&m->a, op, width, dst);
    return;
  }
  x64_mov_rr(&m->a, ASM_W64, SCRATCH, src);
  if (amount != X64_RCX) {
    x64_mov_rr(&m->a, ASM_W64, SCRATCH2, X64_RCX);
    x64_mov_rr(&m->a, ASM_W64, X64_RCX, amount);
  }
  x64_shift_rcl(&m->a, op, width, SCRATCH);
  if (amount != X64_RCX) x64_mov_rr(&m->a, ASM_W64, X64_RCX, SCRATCH2);
  x64_mov_rr(&m->a, width, dst, SCRATCH);
}

static void compare_imm(masm_x64_t *m, asm_width_t width, asm_gpr_t lhs, int64_t imm) {
  if (imm == 0) x64_test_rr(&m->a, width, lhs, lhs);
  else if (fits_i32(imm)) x64_cmp_ri(&m->a, width, lhs, (int32_t)imm);
  else x64_cmp_r_label(&m->a, lhs, constant(m, (uint64_t)imm));
}

static void test_mask(masm_x64_t *m, asm_width_t width, asm_gpr_t value, uint64_t mask) {
  uint64_t all_ones = width == ASM_W32 ? UINT32_MAX : UINT64_MAX;
  if (mask == all_ones) {
    x64_test_rr(&m->a, width, value, value);
  } else if (width == ASM_W32) {
    x64_test_ri(&m->a, ASM_W32, value, (int32_t)(uint32_t)mask);
  } else if (fits_i32((int64_t)mask)) {
    x64_test_ri(&m->a, ASM_W64, value, (int32_t)(int64_t)mask);
  } else {
    masm_x64_move_imm64(m, SCRATCH, mask);
    x64_test_rr(&m->a, ASM_W64, value, SCRATCH);
  }
}

static void set_condition(masm_x64_t *m, x64_cond_t cond, asm_gpr_t dst) {
  x64_setcc(&m->a, cond, dst);
  x64_movzx8_rr(&m->a, dst, dst);
}

static void compare_doubles(masm_x64_t *m, bool swap, asm_fpr_t lhs, asm_fpr_t rhs) {
  if (swap) x64_ucomisd(&m->a, rhs, lhs);
  else x64_ucomisd(&m->a, lhs, rhs);
}

typedef void (*sse_op_t)(x64_asm_t *a, asm_fpr_t dst, asm_fpr_t src);

static void double_operation(masm_x64_t *m, sse_op_t operation, bool is_commutative, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs) {
  if (dst == lhs) {
    operation(&m->a, dst, rhs);
  } else if (dst == rhs) {
    if (is_commutative) {
      operation(&m->a, dst, lhs);
    } else {
      x64_movapd_rr(&m->a, FP_SCRATCH, lhs);
      operation(&m->a, FP_SCRATCH, rhs);
      x64_movapd_rr(&m->a, dst, FP_SCRATCH);
    }
  } else {
    x64_movapd_rr(&m->a, dst, lhs);
    operation(&m->a, dst, rhs);
  }
}

void masm_x64_init(masm_x64_t *m) {
  *m = (masm_x64_t){ 0 };
  x64_init(&m->a);
}

void masm_x64_free(masm_x64_t *m) {
  x64_free(&m->a);
  free(m->constants);
  free(m->constants128);
  *m = (masm_x64_t){ 0 };
}

size_t masm_x64_offset(const masm_x64_t *m) { return x64_offset(&m->a); }
asm_label_t masm_x64_new_label(masm_x64_t *m) { return x64_new_label(&m->a); }
void masm_x64_bind(masm_x64_t *m, asm_label_t label) { x64_bind(&m->a, label); }
size_t masm_x64_label_offset(const masm_x64_t *m, asm_label_t label) { return x64_label_offset(&m->a, label); }

size_t masm_x64_patchable_nop(masm_x64_t *m) {
  size_t offset = x64_offset(&m->a);
  x64_nops(&m->a, 5);
  return offset;
}

size_t masm_x64_jump_patch(size_t from, size_t to, uint8_t *out) {
  int64_t displacement = (int64_t)to - ((int64_t)from + 5);
  ASM_CHECK(fits_i32(displacement), "code is larger than 2 GiB");
  uint32_t rel = (uint32_t)(int32_t)displacement;
  out[0] = 0xe9;
  out[1] = (uint8_t)rel;
  out[2] = (uint8_t)(rel >> 8);
  out[3] = (uint8_t)(rel >> 16);
  out[4] = (uint8_t)(rel >> 24);
  return 5;
}

asm_error_t masm_x64_finish(masm_x64_t *m, asm_code_t *out) {
  size_t data_offset = x64_offset(&m->a);
  if (m->constant128_count) {
    x64_align_data(&m->a, 16);
    for (uint32_t i = 0; i < m->constant128_count; i++) {
      x64_bind(&m->a, m->constants128[i].label);
      x64_data_u64(&m->a, m->constants128[i].value.lo);
      x64_data_u64(&m->a, m->constants128[i].value.hi);
    }
  }
  if (m->constant_count) {
    x64_align_data(&m->a, 8);
    for (uint32_t i = 0; i < m->constant_count; i++) {
      x64_bind(&m->a, m->constants[i].label);
      x64_data_u64(&m->a, m->constants[i].value);
    }
  }
  asm_error_t error = x64_finish(&m->a, out);
  if (error == ASM_OK) out->data_offset = data_offset;
  masm_x64_free(m);
  return error;
}

void masm_x64_unreachable(masm_x64_t *m) {
  x64_ud2(&m->a);
}

void masm_x64_move64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src) {
  mov_if_needed(m, ASM_W64, dst, src);
}

void masm_x64_move32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src) {
  x64_mov_rr(&m->a, ASM_W32, dst, src);
}

void masm_x64_move_imm64(masm_x64_t *m, asm_gpr_t dst, uint64_t imm) {
  if (imm <= UINT32_MAX || fits_i32((int64_t)imm)) x64_mov_ri(&m->a, dst, imm);
  else x64_mov_r_label(&m->a, dst, constant(m, imm));
}

void masm_x64_move_imm32(masm_x64_t *m, asm_gpr_t dst, uint32_t imm) {
  x64_mov_ri32(&m->a, dst, imm);
}

void masm_x64_sign_extend32_to_64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src) {
  x64_movsxd_rr(&m->a, dst, src);
}

void masm_x64_load8(masm_x64_t *m, asm_gpr_t dst, asm_address_t address) { x64_movzx8_rm(&m->a, dst, address); }
void masm_x64_load8_sign_extend(masm_x64_t *m, asm_gpr_t dst, asm_address_t address) { x64_movsx8_rm(&m->a, ASM_W64, dst, address); }
void masm_x64_load16(masm_x64_t *m, asm_gpr_t dst, asm_address_t address) { x64_movzx16_rm(&m->a, dst, address); }
void masm_x64_load16_sign_extend(masm_x64_t *m, asm_gpr_t dst, asm_address_t address) { x64_movsx16_rm(&m->a, ASM_W64, dst, address); }
void masm_x64_load32(masm_x64_t *m, asm_gpr_t dst, asm_address_t address) { x64_mov_rm(&m->a, ASM_W32, dst, address); }
void masm_x64_load32_sign_extend(masm_x64_t *m, asm_gpr_t dst, asm_address_t address) { x64_movsxd_rm(&m->a, dst, address); }
void masm_x64_load64(masm_x64_t *m, asm_gpr_t dst, asm_address_t address) { x64_mov_rm(&m->a, ASM_W64, dst, address); }
void masm_x64_store8(masm_x64_t *m, asm_address_t address, asm_gpr_t src) { x64_mov8_mr(&m->a, address, src); }
void masm_x64_store16(masm_x64_t *m, asm_address_t address, asm_gpr_t src) { x64_mov16_mr(&m->a, address, src); }
void masm_x64_store32(masm_x64_t *m, asm_address_t address, asm_gpr_t src) { x64_mov_mr(&m->a, ASM_W32, address, src); }
void masm_x64_store64(masm_x64_t *m, asm_address_t address, asm_gpr_t src) { x64_mov_mr(&m->a, ASM_W64, address, src); }

void masm_x64_store_imm32(masm_x64_t *m, asm_address_t address, uint32_t imm) {
  x64_mov_mi(&m->a, ASM_W32, address, (int32_t)imm);
}

void masm_x64_store_imm64(masm_x64_t *m, asm_address_t address, uint64_t imm) {
  if (fits_i32((int64_t)imm)) {
    x64_mov_mi(&m->a, ASM_W64, address, (int32_t)(int64_t)imm);
  } else {
    masm_x64_move_imm64(m, SCRATCH, imm);
    x64_mov_mr(&m->a, ASM_W64, address, SCRATCH);
  }
}

void masm_x64_add32_to_memory_imm(masm_x64_t *m, asm_address_t address, int32_t imm) {
  x64_alu_mi(&m->a, X64_ALU_ADD, ASM_W32, address, imm);
}

void masm_x64_add64_to_memory_imm(masm_x64_t *m, asm_address_t address, int32_t imm) {
  x64_alu_mi(&m->a, X64_ALU_ADD, ASM_W64, address, imm);
}

void masm_x64_load_effective_address(masm_x64_t *m, asm_gpr_t dst, asm_address_t address) {
  x64_lea(&m->a, ASM_W64, dst, address);
}

void masm_x64_load_double(masm_x64_t *m, asm_fpr_t dst, asm_address_t address) { x64_movsd_rm(&m->a, dst, address); }
void masm_x64_store_double(masm_x64_t *m, asm_address_t address, asm_fpr_t src) { x64_movsd_mr(&m->a, address, src); }

void masm_x64_load_imm128(masm_x64_t *m, asm_fpr_t dst, asm_u128_t imm) {
  x64_movups_r_label(&m->a, dst, constant128(m, imm));
}

void masm_x64_store128(masm_x64_t *m, asm_address_t address, asm_fpr_t src) {
  x64_movups_mr(&m->a, address, src);
}

void masm_x64_load_float_as_double(masm_x64_t *m, asm_fpr_t dst, asm_address_t address) {
  x64_movss_rm(&m->a, dst, address);
  x64_cvtss2sd(&m->a, dst, dst);
}

void masm_x64_store_double_as_float(masm_x64_t *m, asm_address_t address, asm_fpr_t src) {
  x64_cvtsd2ss(&m->a, FP_SCRATCH, src);
  x64_movss_mr(&m->a, address, FP_SCRATCH);
}

void masm_x64_add32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) {
  commutative(m, X64_ALU_ADD, ASM_W32, dst, lhs, rhs);
}

void masm_x64_add32_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm) {
  if (dst == lhs) x64_add_ri(&m->a, ASM_W32, dst, imm);
  else x64_lea(&m->a, ASM_W32, dst, asm_addr(lhs, imm));
}

void masm_x64_add64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) {
  commutative(m, X64_ALU_ADD, ASM_W64, dst, lhs, rhs);
}

void masm_x64_add64_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int64_t imm) {
  if (fits_i32(imm) && dst != lhs) x64_lea(&m->a, ASM_W64, dst, asm_addr(lhs, (int32_t)imm));
  else with_imm64(m, X64_ALU_ADD, dst, lhs, imm);
}

void masm_x64_sub32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) {
  subtract(m, ASM_W32, dst, lhs, rhs);
}

void masm_x64_sub32_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm) {
  with_imm32(m, X64_ALU_SUB, ASM_W32, dst, lhs, imm);
}

void masm_x64_sub64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) {
  subtract(m, ASM_W64, dst, lhs, rhs);
}

void masm_x64_sub64_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int64_t imm) {
  with_imm64(m, X64_ALU_SUB, dst, lhs, imm);
}

void masm_x64_and32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) {
  commutative(m, X64_ALU_AND, ASM_W32, dst, lhs, rhs);
}

void masm_x64_and32_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint32_t imm) {
  with_imm32(m, X64_ALU_AND, ASM_W32, dst, lhs, (int32_t)imm);
}

void masm_x64_and64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) {
  commutative(m, X64_ALU_AND, ASM_W64, dst, lhs, rhs);
}

void masm_x64_and64_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint64_t imm) {
  if (imm == UINT32_MAX) x64_mov_rr(&m->a, ASM_W32, dst, lhs);
  else with_imm64(m, X64_ALU_AND, dst, lhs, (int64_t)imm);
}

void masm_x64_or32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) {
  commutative(m, X64_ALU_OR, ASM_W32, dst, lhs, rhs);
}

void masm_x64_or32_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint32_t imm) {
  with_imm32(m, X64_ALU_OR, ASM_W32, dst, lhs, (int32_t)imm);
}

void masm_x64_or64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) {
  commutative(m, X64_ALU_OR, ASM_W64, dst, lhs, rhs);
}

void masm_x64_or64_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint64_t imm) {
  with_imm64(m, X64_ALU_OR, dst, lhs, (int64_t)imm);
}

void masm_x64_xor32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) {
  commutative(m, X64_ALU_XOR, ASM_W32, dst, lhs, rhs);
}

void masm_x64_xor32_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint32_t imm) {
  with_imm32(m, X64_ALU_XOR, ASM_W32, dst, lhs, (int32_t)imm);
}

void masm_x64_xor64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) {
  commutative(m, X64_ALU_XOR, ASM_W64, dst, lhs, rhs);
}

void masm_x64_xor64_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint64_t imm) {
  with_imm64(m, X64_ALU_XOR, dst, lhs, (int64_t)imm);
}

void masm_x64_mul32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) {
  if (dst == rhs) {
    x64_imul_rr(&m->a, ASM_W32, dst, lhs);
  } else {
    mov_if_needed(m, ASM_W32, dst, lhs);
    x64_imul_rr(&m->a, ASM_W32, dst, rhs);
  }
}

void masm_x64_mul32_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, uint32_t imm) {
  x64_imul_rri(&m->a, ASM_W32, dst, lhs, (int32_t)imm);
}

void masm_x64_mul64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) {
  if (dst == rhs) {
    x64_imul_rr(&m->a, ASM_W64, dst, lhs);
  } else {
    mov_if_needed(m, ASM_W64, dst, lhs);
    x64_imul_rr(&m->a, ASM_W64, dst, rhs);
  }
}

void masm_x64_neg32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src) {
  mov_if_needed(m, ASM_W32, dst, src);
  x64_neg(&m->a, ASM_W32, dst);
}

void masm_x64_neg64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src) {
  mov_if_needed(m, ASM_W64, dst, src);
  x64_neg(&m->a, ASM_W64, dst);
}

void masm_x64_not32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src) {
  mov_if_needed(m, ASM_W32, dst, src);
  x64_not(&m->a, ASM_W32, dst);
}

void masm_x64_not64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src) {
  mov_if_needed(m, ASM_W64, dst, src);
  x64_not(&m->a, ASM_W64, dst);
}

void masm_x64_shl32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount) { shift_register(m, X64_SHIFT_SHL, ASM_W32, dst, src, amount); }
void masm_x64_shl32_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount) { shift_imm(m, X64_SHIFT_SHL, ASM_W32, dst, src, amount); }
void masm_x64_shl64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount) { shift_register(m, X64_SHIFT_SHL, ASM_W64, dst, src, amount); }
void masm_x64_shl64_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount) { shift_imm(m, X64_SHIFT_SHL, ASM_W64, dst, src, amount); }
void masm_x64_shr32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount) { shift_register(m, X64_SHIFT_SHR, ASM_W32, dst, src, amount); }
void masm_x64_shr32_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount) { shift_imm(m, X64_SHIFT_SHR, ASM_W32, dst, src, amount); }
void masm_x64_shr64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount) { shift_register(m, X64_SHIFT_SHR, ASM_W64, dst, src, amount); }
void masm_x64_shr64_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount) { shift_imm(m, X64_SHIFT_SHR, ASM_W64, dst, src, amount); }
void masm_x64_sar32(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount) { shift_register(m, X64_SHIFT_SAR, ASM_W32, dst, src, amount); }
void masm_x64_sar32_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount) { shift_imm(m, X64_SHIFT_SAR, ASM_W32, dst, src, amount); }
void masm_x64_sar64(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t amount) { shift_register(m, X64_SHIFT_SAR, ASM_W64, dst, src, amount); }
void masm_x64_sar64_imm(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, uint8_t amount) { shift_imm(m, X64_SHIFT_SAR, ASM_W64, dst, src, amount); }

void masm_x64_branch_add32_overflow(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t overflow) {
  commutative(m, X64_ALU_ADD, ASM_W32, dst, lhs, rhs);
  x64_jcc(&m->a, X64_COND_OVERFLOW, overflow);
}

void masm_x64_branch_add32_imm_overflow(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm, asm_label_t overflow) {
  with_imm32(m, X64_ALU_ADD, ASM_W32, dst, lhs, imm);
  x64_jcc(&m->a, X64_COND_OVERFLOW, overflow);
}

void masm_x64_branch_sub32_overflow(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t overflow) {
  subtract(m, ASM_W32, dst, lhs, rhs);
  x64_jcc(&m->a, X64_COND_OVERFLOW, overflow);
}

void masm_x64_branch_sub32_imm_overflow(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm, asm_label_t overflow) {
  with_imm32(m, X64_ALU_SUB, ASM_W32, dst, lhs, imm);
  x64_jcc(&m->a, X64_COND_OVERFLOW, overflow);
}

void masm_x64_branch_mul32_overflow(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t overflow) {
  masm_x64_mul32(m, dst, lhs, rhs);
  x64_jcc(&m->a, X64_COND_OVERFLOW, overflow);
}

void masm_x64_branch_neg32_overflow(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t src, asm_label_t overflow) {
  masm_x64_neg32(m, dst, src);
  x64_jcc(&m->a, X64_COND_OVERFLOW, overflow);
}

void masm_x64_branch_add64_overflow(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t overflow) {
  commutative(m, X64_ALU_ADD, ASM_W64, dst, lhs, rhs);
  x64_jcc(&m->a, X64_COND_OVERFLOW, overflow);
}

void masm_x64_branch_sub64_overflow(masm_x64_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t overflow) {
  subtract(m, ASM_W64, dst, lhs, rhs);
  x64_jcc(&m->a, X64_COND_OVERFLOW, overflow);
}

void masm_x64_branch32(masm_x64_t *m, asm_cond_t cond, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t target) {
  x64_cmp_rr(&m->a, ASM_W32, lhs, rhs);
  x64_jcc(&m->a, condition_code(cond), target);
}

void masm_x64_branch32_imm(masm_x64_t *m, asm_cond_t cond, asm_gpr_t lhs, int32_t imm, asm_label_t target) {
  compare_imm(m, ASM_W32, lhs, imm);
  x64_jcc(&m->a, condition_code(cond), target);
}

void masm_x64_branch64(masm_x64_t *m, asm_cond_t cond, asm_gpr_t lhs, asm_gpr_t rhs, asm_label_t target) {
  x64_cmp_rr(&m->a, ASM_W64, lhs, rhs);
  x64_jcc(&m->a, condition_code(cond), target);
}

void masm_x64_branch_if_stack_pointer_below(masm_x64_t *m, asm_address_t limit, asm_label_t target) {
  masm_x64_branch64_memory(m, ASM_COND_ABOVE, limit, X64_RSP, target);
}

void masm_x64_branch64_imm(masm_x64_t *m, asm_cond_t cond, asm_gpr_t lhs, int64_t imm, asm_label_t target) {
  compare_imm(m, ASM_W64, lhs, imm);
  x64_jcc(&m->a, condition_code(cond), target);
}

void masm_x64_branch32_memory_imm(masm_x64_t *m, asm_cond_t cond, asm_address_t address, int32_t imm, asm_label_t target) {
  x64_cmp_mi(&m->a, ASM_W32, address, imm);
  x64_jcc(&m->a, condition_code(cond), target);
}

void masm_x64_branch16_memory_imm(masm_x64_t *m, asm_cond_t cond, asm_address_t address, uint16_t imm, asm_label_t target) {
  int16_t imm16 = (int16_t)imm;
  if (imm16 >= INT8_MIN && imm16 <= INT8_MAX && (cond == ASM_COND_EQUAL || cond == ASM_COND_NOT_EQUAL)) {
    x64_cmp16_mi8(&m->a, address, (int8_t)imm16);
  } else {
    x64_movzx16_rm(&m->a, SCRATCH, address);
    x64_cmp_ri(&m->a, ASM_W32, SCRATCH, imm);
  }
  x64_jcc(&m->a, condition_code(cond), target);
}

void masm_x64_branch64_memory(masm_x64_t *m, asm_cond_t cond, asm_address_t address, asm_gpr_t rhs, asm_label_t target) {
  x64_cmp_mr(&m->a, ASM_W64, address, rhs);
  x64_jcc(&m->a, condition_code(cond), target);
}

void masm_x64_branch64_memory_imm(masm_x64_t *m, asm_cond_t cond, asm_address_t address, int64_t imm, asm_label_t target) {
  if (fits_i32(imm)) {
    x64_cmp_mi(&m->a, ASM_W64, address, (int32_t)imm);
  } else {
    masm_x64_move_imm64(m, SCRATCH, (uint64_t)imm);
    x64_cmp_mr(&m->a, ASM_W64, address, SCRATCH);
  }
  x64_jcc(&m->a, condition_code(cond), target);
}

void masm_x64_branch_test32(masm_x64_t *m, asm_cond_t cond, asm_gpr_t value, uint32_t mask, asm_label_t target) {
  test_mask(m, ASM_W32, value, mask);
  x64_jcc(&m->a, condition_code(cond), target);
}

void masm_x64_branch_test64(masm_x64_t *m, asm_cond_t cond, asm_gpr_t value, uint64_t mask, asm_label_t target) {
  test_mask(m, ASM_W64, value, mask);
  x64_jcc(&m->a, condition_code(cond), target);
}

void masm_x64_compare32_set(masm_x64_t *m, asm_cond_t cond, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) {
  x64_cmp_rr(&m->a, ASM_W32, lhs, rhs);
  set_condition(m, condition_code(cond), dst);
}

void masm_x64_compare32_imm_set(masm_x64_t *m, asm_cond_t cond, asm_gpr_t dst, asm_gpr_t lhs, int32_t imm) {
  compare_imm(m, ASM_W32, lhs, imm);
  set_condition(m, condition_code(cond), dst);
}

void masm_x64_compare64_set(masm_x64_t *m, asm_cond_t cond, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs) {
  x64_cmp_rr(&m->a, ASM_W64, lhs, rhs);
  set_condition(m, condition_code(cond), dst);
}

void masm_x64_compare64_imm_set(masm_x64_t *m, asm_cond_t cond, asm_gpr_t dst, asm_gpr_t lhs, int64_t imm) {
  compare_imm(m, ASM_W64, lhs, imm);
  set_condition(m, condition_code(cond), dst);
}

void masm_x64_select32(
  masm_x64_t *m, asm_cond_t cond, asm_gpr_t lhs, asm_gpr_t rhs,
  asm_gpr_t dst, asm_gpr_t if_true, asm_gpr_t if_false
) {
  x64_cond_t cc = condition_code(cond);
  x64_cmp_rr(&m->a, ASM_W32, lhs, rhs);
  if (dst == if_true) {
    x64_cmovcc(&m->a, x64_cond_invert(cc), ASM_W32, dst, if_false);
  } else {
    mov_if_needed(m, ASM_W32, dst, if_false);
    x64_cmovcc(&m->a, cc, ASM_W32, dst, if_true);
  }
}

void masm_x64_select64(
  masm_x64_t *m, asm_cond_t cond, asm_gpr_t lhs, asm_gpr_t rhs,
  asm_gpr_t dst, asm_gpr_t if_true, asm_gpr_t if_false
) {
  x64_cond_t cc = condition_code(cond);
  x64_cmp_rr(&m->a, ASM_W64, lhs, rhs);
  if (dst == if_true) {
    x64_cmovcc(&m->a, x64_cond_invert(cc), ASM_W64, dst, if_false);
  } else {
    mov_if_needed(m, ASM_W64, dst, if_false);
    x64_cmovcc(&m->a, cc, ASM_W64, dst, if_true);
  }
}

void masm_x64_jump(masm_x64_t *m, asm_label_t target) {
  x64_jmp(&m->a, target);
}

void masm_x64_jump_absolute(masm_x64_t *m, uint64_t address) {
  masm_x64_move_imm64(m, SCRATCH, address);
  x64_jmp_r(&m->a, SCRATCH);
}

void masm_x64_call(masm_x64_t *m, asm_label_t target) {
  x64_call(&m->a, target);
}

void masm_x64_call_register(masm_x64_t *m, asm_gpr_t target) {
  x64_call_r(&m->a, target);
}

void masm_x64_call_absolute(masm_x64_t *m, uint64_t address) {
  x64_call_label_indirect(&m->a, constant(m, address));
}

void masm_x64_ret(masm_x64_t *m) {
  x64_ret(&m->a);
}

void masm_x64_move_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t src) {
  if (dst != src) x64_movapd_rr(&m->a, dst, src);
}

void masm_x64_move_double_imm(masm_x64_t *m, asm_fpr_t dst, double value) {
  uint64_t bits = asm_f64_bits(value);
  if (bits == 0) x64_xorpd(&m->a, dst, dst);
  else x64_movsd_r_label(&m->a, dst, constant(m, bits));
}

void masm_x64_move_gpr_to_double(masm_x64_t *m, asm_fpr_t dst, asm_gpr_t src) { x64_movq_xr(&m->a, dst, src); }
void masm_x64_move_double_to_gpr(masm_x64_t *m, asm_gpr_t dst, asm_fpr_t src) { x64_movq_rx(&m->a, dst, src); }

void masm_x64_add_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs) { double_operation(m, x64_addsd, true, dst, lhs, rhs); }
void masm_x64_sub_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs) { double_operation(m, x64_subsd, false, dst, lhs, rhs); }

static void min_or_max_double(masm_x64_t *m, bool max, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs) {
  asm_label_t unordered = x64_new_label(&m->a), differ = x64_new_label(&m->a), done = x64_new_label(&m->a);
  x64_movapd_rr(&m->a, FP_SCRATCH, lhs);
  x64_ucomisd(&m->a, lhs, rhs);
  x64_jcc(&m->a, X64_COND_PARITY, unordered);
  x64_jcc(&m->a, X64_COND_NOT_EQUAL, differ);
  if (max) x64_andpd(&m->a, FP_SCRATCH, rhs);
  else x64_orpd(&m->a, FP_SCRATCH, rhs);
  x64_jmp(&m->a, done);
  x64_bind(&m->a, unordered);
  x64_addsd(&m->a, FP_SCRATCH, rhs);
  x64_jmp(&m->a, done);
  x64_bind(&m->a, differ);
  if (max) x64_maxsd(&m->a, FP_SCRATCH, rhs);
  else x64_minsd(&m->a, FP_SCRATCH, rhs);
  x64_bind(&m->a, done);
  x64_movapd_rr(&m->a, dst, FP_SCRATCH);
}

void masm_x64_min_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs) { min_or_max_double(m, false, dst, lhs, rhs); }
void masm_x64_max_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs) { min_or_max_double(m, true, dst, lhs, rhs); }
void masm_x64_mul_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs) { double_operation(m, x64_mulsd, true, dst, lhs, rhs); }
void masm_x64_div_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs) { double_operation(m, x64_divsd, false, dst, lhs, rhs); }

void masm_x64_sqrt_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t src) { x64_sqrtsd(&m->a, dst, src); }
void masm_x64_floor_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t src) { x64_roundsd(&m->a, dst, src, 0x9); }
void masm_x64_ceil_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t src) { x64_roundsd(&m->a, dst, src, 0xa); }
void masm_x64_trunc_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t src) { x64_roundsd(&m->a, dst, src, 0xb); }

void masm_x64_neg_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t src) {
  x64_movsd_r_label(&m->a, FP_SCRATCH, constant(m, UINT64_C(1) << 63));
  masm_x64_move_double(m, dst, src);
  x64_xorpd(&m->a, dst, FP_SCRATCH);
}

void masm_x64_abs_double(masm_x64_t *m, asm_fpr_t dst, asm_fpr_t src) {
  x64_movsd_r_label(&m->a, FP_SCRATCH, constant(m, ~(UINT64_C(1) << 63)));
  masm_x64_move_double(m, dst, src);
  x64_andpd(&m->a, dst, FP_SCRATCH);
}

void masm_x64_convert_int32_to_double(masm_x64_t *m, asm_fpr_t dst, asm_gpr_t src) {
  x64_xorpd(&m->a, dst, dst);
  x64_cvtsi2sd(&m->a, ASM_W32, dst, src);
}

void masm_x64_convert_int64_to_double(masm_x64_t *m, asm_fpr_t dst, asm_gpr_t src) {
  x64_xorpd(&m->a, dst, dst);
  x64_cvtsi2sd(&m->a, ASM_W64, dst, src);
}

void masm_x64_branch_convert_double_to_int32(
  masm_x64_t *m, asm_gpr_t dst, asm_fpr_t src,
  asm_label_t fail, asm_negative_zero_t negative_zero
) {
  x64_cvttsd2si(&m->a, ASM_W32, dst, src);
  x64_xorpd(&m->a, FP_SCRATCH, FP_SCRATCH);
  x64_cvtsi2sd(&m->a, ASM_W32, FP_SCRATCH, dst);
  x64_ucomisd(&m->a, FP_SCRATCH, src);
  x64_jcc(&m->a, X64_COND_NOT_EQUAL, fail);
  x64_jcc(&m->a, X64_COND_PARITY, fail);
  if (negative_zero == ASM_NEGATIVE_ZERO_FAIL) {
    asm_label_t done = x64_new_label(&m->a);
    x64_test_rr(&m->a, ASM_W32, dst, dst);
    x64_jcc(&m->a, X64_COND_NOT_EQUAL, done);
    x64_movmskpd(&m->a, SCRATCH, src);
    x64_test_ri(&m->a, ASM_W32, SCRATCH, 1);
    x64_jcc(&m->a, X64_COND_NOT_EQUAL, fail);
    x64_bind(&m->a, done);
  }
}

void masm_x64_truncate_double_to_int64(masm_x64_t *m, asm_gpr_t dst, asm_fpr_t src) {
  x64_cvttsd2si(&m->a, ASM_W64, dst, src);
}

bool masm_x64_has_js_convert(void) {
  return false;
}

void masm_x64_js_convert_double_to_int32(masm_x64_t *, asm_gpr_t, asm_fpr_t) {
  abort();
}

void masm_x64_branch_js_convert_double_to_int32(masm_x64_t *, asm_gpr_t, asm_fpr_t, asm_label_t) {
  abort();
}

void masm_x64_branch_double(masm_x64_t *m, asm_dcond_t cond, asm_fpr_t lhs, asm_fpr_t rhs, asm_label_t target) {
  double_flags_t flags = double_flags(cond);
  switch (flags.kind) {
    case DOUBLE_FLAGS_SINGLE:
      compare_doubles(m, flags.swap, lhs, rhs);
      x64_jcc(&m->a, flags.first, target);
      break;
    case DOUBLE_FLAGS_BOTH: {
      asm_label_t skip = x64_new_label(&m->a);
      compare_doubles(m, false, lhs, rhs);
      x64_jcc(&m->a, x64_cond_invert(flags.second), skip);
      x64_jcc(&m->a, flags.first, target);
      x64_bind(&m->a, skip);
      break;
    }
    case DOUBLE_FLAGS_EITHER:
      compare_doubles(m, false, lhs, rhs);
      x64_jcc(&m->a, flags.second, target);
      x64_jcc(&m->a, flags.first, target);
      break;
  }
}

void masm_x64_compare_double_set(masm_x64_t *m, asm_dcond_t cond, asm_gpr_t dst, asm_fpr_t lhs, asm_fpr_t rhs) {
  double_flags_t flags = double_flags(cond);
  compare_doubles(m, flags.kind == DOUBLE_FLAGS_SINGLE && flags.swap, lhs, rhs);
  switch (flags.kind) {
    case DOUBLE_FLAGS_SINGLE:
      set_condition(m, flags.first, dst);
      break;
    case DOUBLE_FLAGS_BOTH:
      x64_setcc(&m->a, flags.first, dst);
      x64_setcc(&m->a, flags.second, SCRATCH);
      x64_and_rr(&m->a, ASM_W32, dst, SCRATCH);
      x64_movzx8_rr(&m->a, dst, dst);
      break;
    case DOUBLE_FLAGS_EITHER:
      x64_setcc(&m->a, flags.first, dst);
      x64_setcc(&m->a, flags.second, SCRATCH);
      x64_or_rr(&m->a, ASM_W32, dst, SCRATCH);
      x64_movzx8_rr(&m->a, dst, dst);
      break;
  }
}

asm_frame_t masm_x64_frame(asm_regset_t saved_gprs, asm_regset_t saved_fprs, uint32_t locals_size) {
  saved_gprs = asm_regset_without(asm_regset_without(saved_gprs, X64_RSP), X64_RBP);
  uint32_t pushed = 8 * asm_regset_len(saved_gprs);
  uint32_t fpr_area = 8 * asm_regset_len(saved_fprs);
  locals_size = (locals_size + 7) & ~UINT32_C(7);
  if ((pushed + fpr_area + locals_size) % 16 != 0) locals_size += 8;
  return (asm_frame_t){
    .saved_gprs = saved_gprs,
    .saved_fprs = saved_fprs,
    .locals_size = locals_size,
    .caller_stack_offset = locals_size + fpr_area + pushed + 16,
  };
}

void masm_x64_emit_prologue(masm_x64_t *m, const asm_frame_t *frame) {
  x64_push(&m->a, X64_RBP);
  x64_mov_rr(&m->a, ASM_W64, X64_RBP, X64_RSP);
  ASM_REGSET_EACH(frame->saved_gprs, reg) x64_push(&m->a, reg);
  uint32_t reserved = frame->locals_size + 8 * asm_regset_len(frame->saved_fprs);
  if (reserved != 0) x64_sub_ri(&m->a, ASM_W64, X64_RSP, (int32_t)reserved);
  uint32_t offset = frame->locals_size;
  ASM_REGSET_EACH(frame->saved_fprs, reg) {
    x64_movsd_mr(&m->a, asm_addr(X64_RSP, (int32_t)offset), reg);
    offset += 8;
  }
}

void masm_x64_emit_epilogue(masm_x64_t *m, const asm_frame_t *frame) {
  uint32_t offset = frame->locals_size;
  ASM_REGSET_EACH(frame->saved_fprs, reg) {
    x64_movsd_rm(&m->a, reg, asm_addr(X64_RSP, (int32_t)offset));
    offset += 8;
  }
  uint32_t reserved = frame->locals_size + 8 * asm_regset_len(frame->saved_fprs);
  if (reserved != 0) x64_add_ri(&m->a, ASM_W64, X64_RSP, (int32_t)reserved);
  ASM_REGSET_EACH_REVERSE(frame->saved_gprs, reg) x64_pop(&m->a, reg);
  x64_pop(&m->a, X64_RBP);
}
