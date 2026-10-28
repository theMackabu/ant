#include "masm.h"
#include "executable.h"

#include <inttypes.h>
#include <math.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ASM_HAVE_MASM)

int main(void) {
  puts("skipped: no macro assembler for this host");
  return 0;
}

#else

#if defined(__aarch64__)
static constexpr asm_gpr_t ARG0 = A64_X0;
static constexpr asm_gpr_t ARG1 = A64_X1;
static constexpr asm_gpr_t ARG2 = A64_X2;
static constexpr asm_gpr_t ARG3 = A64_X3;
static constexpr asm_gpr_t RET0 = A64_X0;
static constexpr asm_gpr_t RET1 = A64_X1;
static constexpr asm_gpr_t TMP0 = A64_X9;
static constexpr asm_gpr_t TMP1 = A64_X10;
static constexpr asm_gpr_t FREE = A64_X4;
static constexpr asm_gpr_t SAVED0 = A64_X19;
static constexpr asm_gpr_t SAVED1 = A64_X20;
static constexpr asm_gpr_t CALL_TARGET = A64_X8;
static constexpr asm_gpr_t OPERAND_REGISTERS[3] = { A64_X0, A64_X1, A64_X2 };
static constexpr asm_fpr_t F0 = A64_D0;
static constexpr asm_fpr_t F1 = A64_D1;
static constexpr asm_fpr_t F2 = A64_D2;
static constexpr asm_fpr_t F3 = A64_D3;
static constexpr asm_fpr_t F4 = A64_D4;
static constexpr asm_fpr_t F5 = A64_D5;
static constexpr asm_fpr_t F7 = A64_D7;
static constexpr asm_fpr_t FHIGH = A64_D30;
static constexpr asm_fpr_t FTMP0 = A64_D16;
static constexpr asm_fpr_t FTMP1 = A64_D17;
static constexpr asm_fpr_t FSAVED0 = A64_D8;
static constexpr asm_fpr_t FSAVED1 = A64_D9;
static constexpr asm_fpr_t FSAVED2 = A64_D10;
#else
static constexpr asm_gpr_t ARG0 = X64_RDI;
static constexpr asm_gpr_t ARG1 = X64_RSI;
static constexpr asm_gpr_t ARG2 = X64_RDX;
static constexpr asm_gpr_t ARG3 = X64_RCX;
static constexpr asm_gpr_t RET0 = X64_RAX;
static constexpr asm_gpr_t RET1 = X64_RDX;
static constexpr asm_gpr_t TMP0 = X64_R8;
static constexpr asm_gpr_t TMP1 = X64_R9;
static constexpr asm_gpr_t FREE = X64_RAX;
static constexpr asm_gpr_t SAVED0 = X64_RBX;
static constexpr asm_gpr_t SAVED1 = X64_R12;
static constexpr asm_gpr_t CALL_TARGET = X64_R8;
static constexpr asm_gpr_t OPERAND_REGISTERS[3] = { X64_RAX, X64_RCX, X64_RDX };
static constexpr asm_fpr_t F0 = X64_XMM0;
static constexpr asm_fpr_t F1 = X64_XMM1;
static constexpr asm_fpr_t F2 = X64_XMM2;
static constexpr asm_fpr_t F3 = X64_XMM3;
static constexpr asm_fpr_t F4 = X64_XMM4;
static constexpr asm_fpr_t F5 = X64_XMM5;
static constexpr asm_fpr_t F7 = X64_XMM7;
static constexpr asm_fpr_t FHIGH = X64_XMM15;
static constexpr asm_fpr_t FTMP0 = X64_XMM8;
static constexpr asm_fpr_t FTMP1 = X64_XMM9;
static constexpr asm_fpr_t FSAVED0 = X64_XMM8;
static constexpr asm_fpr_t FSAVED1 = X64_XMM9;
static constexpr asm_fpr_t FSAVED2 = X64_XMM10;
#endif

static constexpr asm_gpr_t SP = MASM_STACK_POINTER;

typedef struct {
  uint64_t value;
  uint64_t status;
} pair_t;

typedef uint64_t (*binary_fn)(uint64_t, uint64_t);
typedef pair_t (*binary_pair_fn)(uint64_t, uint64_t);

static int failures;

#define CHECK(cond, ...) do {                         \
  if (!(cond)) {                                      \
    if (failures < 40) {                              \
      fprintf(stderr, "  %s:%d: ", __FILE__, __LINE__); \
      fprintf(stderr, __VA_ARGS__);                   \
      fputc('\n', stderr);                            \
    }                                                 \
    failures++;                                       \
  }                                                   \
} while (0)

static void *jit_finish(masm_t *m, asm_executable_t *exe) {
  asm_code_t code;
  asm_error_t error = masm_finish(m, &code);
  if (error != ASM_OK) {
    fprintf(stderr, "assembling failed: %d\n", (int)error);
    exit(1);
  }
  bool mapped = asm_executable_init(exe, code.bytes, code.len);
  asm_code_free(&code);
  if (!mapped) {
    fprintf(stderr, "mapping executable code failed\n");
    exit(1);
  }
  return exe->address;
}

static constexpr uint64_t VALUES[16] = {
  0, 1, 2, 31, 32, 63,
  0x7fffffff, 0x80000000, 0xffffffff, 0x100000000,
  0x123456789abcdef0, 0x7fffffffffffffff, 0x8000000000000000,
  0xfffffffffffffffe, UINT64_MAX, 0xfedcba9876543210,
};

static constexpr int64_t IMMEDIATES[12] = {
  0, 1, -1, 127, -128, 0x7fffffff, -0x80000000LL, 0xffffffff, 0x100000000,
  0x123456789abc, INT64_MAX, INT64_MIN,
};

static inline uint32_t low32(uint64_t value) {
  return (uint32_t)value;
}

static inline int64_t sar64(int64_t value, unsigned amount) {
  return value < 0 ? ~(~value >> amount) : value >> amount;
}

static inline int32_t sar32(int32_t value, unsigned amount) {
  return value < 0 ? ~(~value >> amount) : value >> amount;
}

typedef void (*emit_fn)(masm_t *m, asm_gpr_t dst, asm_gpr_t lhs, asm_gpr_t rhs, int op, int64_t imm);
typedef uint64_t (*reference_fn)(uint64_t a, uint64_t b, int op, int64_t imm);

// Runs emit for every assignment of dst/lhs/rhs (aliasing included) with lhs/rhs from the first two arguments.
static void check_binary(const char *name, emit_fn emit, reference_fn reference, int op, int64_t imm) {
  for (int d = 0; d < 3; d++) for (int l = 0; l < 3; l++) for (int r = 0; r < 3; r++) {
    asm_gpr_t dst = OPERAND_REGISTERS[d], lhs = OPERAND_REGISTERS[l], rhs = OPERAND_REGISTERS[r];
    masm_t m;
    masm_init(&m);
    masm_move64(&m, TMP0, ARG0);
    masm_move64(&m, TMP1, ARG1);
    masm_move64(&m, lhs, TMP0);
    masm_move64(&m, rhs, TMP1);
    emit(&m, dst, lhs, rhs, op, imm);
    masm_move64(&m, RET0, dst);
    masm_ret(&m);
    asm_executable_t exe;
    binary_fn function = (binary_fn)jit_finish(&m, &exe);
    for (size_t i = 0; i < 16; i++) for (size_t j = 0; j < 16; j++) {
      uint64_t a = VALUES[i], b = lhs == rhs ? a : VALUES[j];
      uint64_t actual = function(a, b), expected = reference(a, b, op, imm);
      CHECK(
        actual == expected, "%s(%#" PRIx64 ", %#" PRIx64 ") imm=%" PRId64 " dst=%d lhs=%d rhs=%d: %#" PRIx64 " != %#" PRIx64,
        name, a, b, imm, dst, lhs, rhs, actual, expected
      );
    }
    asm_executable_free(&exe);
  }
}

enum {
  OP_ADD32, OP_ADD64, OP_SUB32, OP_SUB64, OP_AND32, OP_AND64, OP_OR32, OP_OR64, OP_XOR32, OP_XOR64,
  OP_MUL32, OP_MUL64, OP_SHL32, OP_SHL64, OP_SHR32, OP_SHR64, OP_SAR32, OP_SAR64,
  OP_NEG32, OP_NEG64, OP_NOT32, OP_NOT64, OP_MOVE32, OP_SIGN_EXTEND32_TO_64, OP_BINARY_COUNT,
};

static const char *const binary_names[OP_BINARY_COUNT] = {
  "add32", "add64", "sub32", "sub64", "and32", "and64", "or32", "or64", "xor32", "xor64",
  "mul32", "mul64", "shl32", "shl64", "shr32", "shr64", "sar32", "sar64",
  "neg32", "neg64", "not32", "not64", "move32", "sign_extend32_to_64",
};

static void emit_binary(masm_t *m, asm_gpr_t d, asm_gpr_t l, asm_gpr_t r, int op, int64_t imm) {
  switch (op) {
    case OP_ADD32: masm_add32(m, d, l, r); break;
    case OP_ADD64: masm_add64(m, d, l, r); break;
    case OP_SUB32: masm_sub32(m, d, l, r); break;
    case OP_SUB64: masm_sub64(m, d, l, r); break;
    case OP_AND32: masm_and32(m, d, l, r); break;
    case OP_AND64: masm_and64(m, d, l, r); break;
    case OP_OR32: masm_or32(m, d, l, r); break;
    case OP_OR64: masm_or64(m, d, l, r); break;
    case OP_XOR32: masm_xor32(m, d, l, r); break;
    case OP_XOR64: masm_xor64(m, d, l, r); break;
    case OP_MUL32: masm_mul32(m, d, l, r); break;
    case OP_MUL64: masm_mul64(m, d, l, r); break;
    case OP_SHL32: masm_shl32(m, d, l, r); break;
    case OP_SHL64: masm_shl64(m, d, l, r); break;
    case OP_SHR32: masm_shr32(m, d, l, r); break;
    case OP_SHR64: masm_shr64(m, d, l, r); break;
    case OP_SAR32: masm_sar32(m, d, l, r); break;
    case OP_SAR64: masm_sar64(m, d, l, r); break;
    case OP_NEG32: masm_neg32(m, d, l); break;
    case OP_NEG64: masm_neg64(m, d, l); break;
    case OP_NOT32: masm_not32(m, d, l); break;
    case OP_NOT64: masm_not64(m, d, l); break;
    case OP_MOVE32: masm_move32(m, d, l); break;
    case OP_SIGN_EXTEND32_TO_64: masm_sign_extend32_to_64(m, d, l); break;
  }
}

static uint64_t reference_binary(uint64_t a, uint64_t b, int op, int64_t imm) {
  uint32_t a32 = low32(a), b32 = low32(b);
  switch (op) {
    case OP_ADD32: return a32 + b32;
    case OP_ADD64: return a + b;
    case OP_SUB32: return (uint32_t)(a32 - b32);
    case OP_SUB64: return a - b;
    case OP_AND32: return a32 & b32;
    case OP_AND64: return a & b;
    case OP_OR32: return a32 | b32;
    case OP_OR64: return a | b;
    case OP_XOR32: return a32 ^ b32;
    case OP_XOR64: return a ^ b;
    case OP_MUL32: return (uint32_t)(a32 * b32);
    case OP_MUL64: return a * b;
    case OP_SHL32: return (uint32_t)(a32 << (b & 31));
    case OP_SHL64: return a << (b & 63);
    case OP_SHR32: return a32 >> (b & 31);
    case OP_SHR64: return a >> (b & 63);
    case OP_SAR32: return (uint32_t)sar32((int32_t)a32, (unsigned)(b & 31));
    case OP_SAR64: return (uint64_t)sar64((int64_t)a, (unsigned)(b & 63));
    case OP_NEG32: return (uint32_t)(0u - a32);
    case OP_NEG64: return 0 - a;
    case OP_NOT32: return (uint32_t)~a32;
    case OP_NOT64: return ~a;
    case OP_MOVE32: return a32;
    case OP_SIGN_EXTEND32_TO_64: return (uint64_t)(int64_t)(int32_t)a32;
  }
  return 0;
}

static void integer_arithmetic_and_logic(void) {
  for (int op = 0; op < OP_BINARY_COUNT; op++)
    check_binary(binary_names[op], emit_binary, reference_binary, op, 0);
}

enum {
  OPI_ADD32, OPI_ADD64, OPI_SUB32, OPI_SUB64, OPI_AND32, OPI_AND64, OPI_OR32, OPI_OR64, OPI_MUL32,
  OPI_XOR32, OPI_XOR64, OPI_SHL32, OPI_SHL64, OPI_SHR32, OPI_SHR64, OPI_SAR32, OPI_SAR64,
  OPI_MOVE_IMM32, OPI_MOVE_IMM64, OPI_COMPARE32_SET, OPI_COMPARE64_SET, OPI_LEA, OPI_LEA_INDEXED, OPI_COUNT,
};

static const char *const immediate_names[OPI_COUNT] = {
  "add32_imm", "add64_imm", "sub32_imm", "sub64_imm", "and32_imm", "and64_imm", "or32_imm", "or64_imm",
  "mul32_imm", "xor32_imm", "xor64_imm", "shl32_imm", "shl64_imm", "shr32_imm", "shr64_imm", "sar32_imm",
  "sar64_imm", "move_imm32", "move_imm64", "compare32_imm_set", "compare64_imm_set",
  "load_effective_address", "load_effective_address_indexed",
};

static void emit_immediate(masm_t *m, asm_gpr_t d, asm_gpr_t l, asm_gpr_t r, int op, int64_t i) {
  switch (op) {
    case OPI_ADD32: masm_add32_imm(m, d, l, (int32_t)i); break;
    case OPI_ADD64: masm_add64_imm(m, d, l, i); break;
    case OPI_SUB32: masm_sub32_imm(m, d, l, (int32_t)i); break;
    case OPI_SUB64: masm_sub64_imm(m, d, l, i); break;
    case OPI_AND32: masm_and32_imm(m, d, l, (uint32_t)i); break;
    case OPI_AND64: masm_and64_imm(m, d, l, (uint64_t)i); break;
    case OPI_OR32: masm_or32_imm(m, d, l, (uint32_t)i); break;
    case OPI_OR64: masm_or64_imm(m, d, l, (uint64_t)i); break;
    case OPI_MUL32: masm_mul32_imm(m, d, l, (uint32_t)i); break;
    case OPI_XOR32: masm_xor32_imm(m, d, l, (uint32_t)i); break;
    case OPI_XOR64: masm_xor64_imm(m, d, l, (uint64_t)i); break;
    case OPI_SHL32: masm_shl32_imm(m, d, l, (uint8_t)i); break;
    case OPI_SHL64: masm_shl64_imm(m, d, l, (uint8_t)i); break;
    case OPI_SHR32: masm_shr32_imm(m, d, l, (uint8_t)i); break;
    case OPI_SHR64: masm_shr64_imm(m, d, l, (uint8_t)i); break;
    case OPI_SAR32: masm_sar32_imm(m, d, l, (uint8_t)i); break;
    case OPI_SAR64: masm_sar64_imm(m, d, l, (uint8_t)i); break;
    case OPI_MOVE_IMM32: masm_move_imm32(m, d, (uint32_t)i); break;
    case OPI_MOVE_IMM64: masm_move_imm64(m, d, (uint64_t)i); break;
    case OPI_COMPARE32_SET: masm_compare32_imm_set(m, ASM_COND_LESS_THAN, d, l, (int32_t)i); break;
    case OPI_COMPARE64_SET: masm_compare64_imm_set(m, ASM_COND_BELOW, d, l, i); break;
    case OPI_LEA: masm_load_effective_address(m, d, asm_addr(l, (int32_t)i)); break;
    case OPI_LEA_INDEXED: masm_load_effective_address(m, d, asm_addr_indexed(l, l, ASM_SCALE_4, (int32_t)i)); break;
  }
}

static uint64_t reference_immediate(uint64_t a, uint64_t b, int op, int64_t i) {
  uint32_t a32 = low32(a), i32 = (uint32_t)i;
  uint64_t u = (uint64_t)i;
  switch (op) {
    case OPI_ADD32: return (uint32_t)(a32 + i32);
    case OPI_ADD64: return a + u;
    case OPI_SUB32: return (uint32_t)(a32 - i32);
    case OPI_SUB64: return a - u;
    case OPI_AND32: return a32 & i32;
    case OPI_AND64: return a & u;
    case OPI_OR32: return a32 | i32;
    case OPI_OR64: return a | u;
    case OPI_MUL32: return (uint32_t)(a32 * i32);
    case OPI_XOR32: return a32 ^ i32;
    case OPI_XOR64: return a ^ u;
    case OPI_SHL32: return (uint32_t)(a32 << (u & 31));
    case OPI_SHL64: return a << (u & 63);
    case OPI_SHR32: return a32 >> (u & 31);
    case OPI_SHR64: return a >> (u & 63);
    case OPI_SAR32: return (uint32_t)sar32((int32_t)a32, (unsigned)(u & 31));
    case OPI_SAR64: return (uint64_t)sar64((int64_t)a, (unsigned)(u & 63));
    case OPI_MOVE_IMM32: return i32;
    case OPI_MOVE_IMM64: return u;
    case OPI_COMPARE32_SET: return (int32_t)a32 < (int32_t)i32;
    case OPI_COMPARE64_SET: return a < u;
    case OPI_LEA: return a + (uint64_t)(int64_t)(int32_t)i32;
    case OPI_LEA_INDEXED: return a * 5 + (uint64_t)(int64_t)(int32_t)i32;
  }
  return 0;
}

static void integer_operations_with_immediates(void) {
  for (int op = 0; op < OPI_COUNT; op++)
    for (size_t i = 0; i < 12; i++)
      check_binary(immediate_names[op], emit_immediate, reference_immediate, op, IMMEDIATES[i]);
}

enum {
  OPO_ADD32, OPO_SUB32, OPO_MUL32, OPO_NEG32, OPO_ADD64, OPO_SUB64, OPO_ADD32_IMM, OPO_SUB32_IMM, OPO_COUNT,
};

static const char *const overflow_names[OPO_COUNT] = {
  "branch_add32_overflow", "branch_sub32_overflow", "branch_mul32_overflow", "branch_neg32_overflow",
  "branch_add64_overflow", "branch_sub64_overflow", "branch_add32_imm_overflow(-7)",
  "branch_sub32_imm_overflow(0x10000)",
};

static void emit_overflow(masm_t *m, int op, asm_gpr_t d, asm_gpr_t l, asm_gpr_t r, asm_label_t o) {
  switch (op) {
    case OPO_ADD32: masm_branch_add32_overflow(m, d, l, r, o); break;
    case OPO_SUB32: masm_branch_sub32_overflow(m, d, l, r, o); break;
    case OPO_MUL32: masm_branch_mul32_overflow(m, d, l, r, o); break;
    case OPO_NEG32: masm_branch_neg32_overflow(m, d, l, o); break;
    case OPO_ADD64: masm_branch_add64_overflow(m, d, l, r, o); break;
    case OPO_SUB64: masm_branch_sub64_overflow(m, d, l, r, o); break;
    case OPO_ADD32_IMM: masm_branch_add32_imm_overflow(m, d, l, -7, o); break;
    case OPO_SUB32_IMM: masm_branch_sub32_imm_overflow(m, d, l, 0x10000, o); break;
  }
}

static pair_t reference_overflow(int op, uint64_t a, uint64_t b) {
  int32_t x = (int32_t)low32(a), y = (int32_t)low32(b), r32 = 0;
  int64_t r64 = 0;
  bool overflow = false, wide = false;
  switch (op) {
    case OPO_ADD32: overflow = __builtin_add_overflow(x, y, &r32); break;
    case OPO_SUB32: overflow = __builtin_sub_overflow(x, y, &r32); break;
    case OPO_MUL32: overflow = __builtin_mul_overflow(x, y, &r32); break;
    case OPO_NEG32: overflow = __builtin_sub_overflow(0, x, &r32); break;
    case OPO_ADD64: wide = true; overflow = __builtin_add_overflow((int64_t)a, (int64_t)b, &r64); break;
    case OPO_SUB64: wide = true; overflow = __builtin_sub_overflow((int64_t)a, (int64_t)b, &r64); break;
    case OPO_ADD32_IMM: overflow = __builtin_add_overflow(x, -7, &r32); break;
    case OPO_SUB32_IMM: overflow = __builtin_sub_overflow(x, 0x10000, &r32); break;
  }
  if (overflow) return (pair_t){ 0, 1 };
  return (pair_t){ wide ? (uint64_t)r64 : (uint64_t)(uint32_t)r32, 0 };
}

static void overflow_checking_arithmetic(void) {
  static constexpr uint64_t values[12] = {
    0, 1, 2, 0x7fffffff, 0x80000000, 0xffffffff, 0x40000000, 0xc0000000, 0x10000,
    0x7fffffffffffffff, 0x8000000000000000, UINT64_MAX,
  };
  for (int op = 0; op < OPO_COUNT; op++)
  for (int d = 0; d < 3; d++) for (int l = 0; l < 3; l++) for (int r = 0; r < 3; r++) {
    asm_gpr_t dst = OPERAND_REGISTERS[d], lhs = OPERAND_REGISTERS[l], rhs = OPERAND_REGISTERS[r];
    masm_t m;
    masm_init(&m);
    asm_label_t overflow = masm_new_label(&m);
    masm_move64(&m, TMP0, ARG0);
    masm_move64(&m, TMP1, ARG1);
    masm_move64(&m, lhs, TMP0);
    masm_move64(&m, rhs, TMP1);
    emit_overflow(&m, op, dst, lhs, rhs, overflow);
    masm_move64(&m, RET0, dst);
    masm_move_imm64(&m, RET1, 0);
    masm_ret(&m);
    masm_bind(&m, overflow);
    masm_move_imm64(&m, RET0, 0);
    masm_move_imm64(&m, RET1, 1);
    masm_ret(&m);
    asm_executable_t exe;
    binary_pair_fn function = (binary_pair_fn)jit_finish(&m, &exe);
    for (size_t i = 0; i < 12; i++) for (size_t j = 0; j < 12; j++) {
      uint64_t a = values[i], b = lhs == rhs ? a : values[j];
      pair_t actual = function(a, b), expected = reference_overflow(op, a, b);
      CHECK(
        actual.value == expected.value && actual.status == expected.status,
        "%s(%#" PRIx64 ", %#" PRIx64 ") dst=%d lhs=%d rhs=%d: {%#" PRIx64 ", %" PRIu64 "} != {%#" PRIx64 ", %" PRIu64 "}",
        overflow_names[op], a, b, dst, lhs, rhs, actual.value, actual.status, expected.value, expected.status
      );
    }
    asm_executable_free(&exe);
  }
}

static constexpr asm_cond_t ALL_CONDITIONS[16] = {
  ASM_COND_EQUAL, ASM_COND_NOT_EQUAL, ASM_COND_LESS_THAN, ASM_COND_LESS_THAN_OR_EQUAL,
  ASM_COND_GREATER_THAN, ASM_COND_GREATER_THAN_OR_EQUAL, ASM_COND_BELOW, ASM_COND_BELOW_OR_EQUAL,
  ASM_COND_ABOVE, ASM_COND_ABOVE_OR_EQUAL, ASM_COND_OVERFLOW, ASM_COND_NO_OVERFLOW,
  ASM_COND_ZERO, ASM_COND_NON_ZERO, ASM_COND_NEGATIVE, ASM_COND_POSITIVE_OR_ZERO,
};

static bool evaluate_comparison(asm_cond_t cond, asm_width_t width, uint64_t lhs, uint64_t rhs) {
  uint64_t l = lhs, r = rhs;
  int64_t sl = (int64_t)lhs, sr = (int64_t)rhs, difference;
  bool negative, overflow;
  if (width == ASM_W32) {
    uint32_t a = low32(lhs), b = low32(rhs);
    int32_t d32;
    l = a;
    r = b;
    sl = (int32_t)a;
    sr = (int32_t)b;
    negative = (int32_t)(a - b) < 0;
    overflow = __builtin_sub_overflow((int32_t)a, (int32_t)b, &d32);
  } else {
    negative = (int64_t)(lhs - rhs) < 0;
    overflow = __builtin_sub_overflow((int64_t)lhs, (int64_t)rhs, &difference);
  }
  switch (cond) {
    case ASM_COND_EQUAL: case ASM_COND_ZERO: return l == r;
    case ASM_COND_NOT_EQUAL: case ASM_COND_NON_ZERO: return l != r;
    case ASM_COND_LESS_THAN: return sl < sr;
    case ASM_COND_LESS_THAN_OR_EQUAL: return sl <= sr;
    case ASM_COND_GREATER_THAN: return sl > sr;
    case ASM_COND_GREATER_THAN_OR_EQUAL: return sl >= sr;
    case ASM_COND_BELOW: return l < r;
    case ASM_COND_BELOW_OR_EQUAL: return l <= r;
    case ASM_COND_ABOVE: return l > r;
    case ASM_COND_ABOVE_OR_EQUAL: return l >= r;
    case ASM_COND_OVERFLOW: return overflow;
    case ASM_COND_NO_OVERFLOW: return !overflow;
    case ASM_COND_NEGATIVE: return negative;
    case ASM_COND_POSITIVE_OR_ZERO: return !negative;
  }
  return false;
}

static void emit_return_taken(masm_t *m, asm_label_t taken) {
  masm_move_imm32(m, RET0, 0);
  masm_ret(m);
  masm_bind(m, taken);
  masm_move_imm32(m, RET0, 1);
  masm_ret(m);
}

static void compares_and_branches(void) {
  for (size_t c = 0; c < 16; c++) {
    asm_cond_t cond = ALL_CONDITIONS[c];
    CHECK(asm_cond_invert(asm_cond_invert(cond)) == cond, "invert(invert(%d))", cond);
    for (int w = 0; w < 2; w++) {
      asm_width_t width = w ? ASM_W64 : ASM_W32;
      masm_t m;
      asm_executable_t branch_exe, set_exe, inverted_exe;

      masm_init(&m);
      asm_label_t taken = masm_new_label(&m);
      if (width == ASM_W32) masm_branch32(&m, cond, ARG0, ARG1, taken);
      else masm_branch64(&m, cond, ARG0, ARG1, taken);
      emit_return_taken(&m, taken);
      binary_fn branch = (binary_fn)jit_finish(&m, &branch_exe);

      masm_init(&m);
      if (width == ASM_W32) masm_compare32_set(&m, cond, RET0, ARG0, ARG1);
      else masm_compare64_set(&m, cond, RET0, ARG0, ARG1);
      masm_ret(&m);
      binary_fn set = (binary_fn)jit_finish(&m, &set_exe);

      masm_init(&m);
      if (width == ASM_W32) masm_compare32_set(&m, asm_cond_invert(cond), ARG0, ARG0, ARG1);
      else masm_compare64_set(&m, asm_cond_invert(cond), ARG0, ARG0, ARG1);
      masm_move64(&m, RET0, ARG0);
      masm_ret(&m);
      binary_fn inverted = (binary_fn)jit_finish(&m, &inverted_exe);

      for (size_t i = 0; i < 16; i++) for (size_t j = 0; j < 16; j++) {
        uint64_t a = VALUES[i], b = VALUES[j];
        uint64_t expected = evaluate_comparison(cond, width, a, b);
        CHECK(branch(a, b) == expected, "branch cond=%d w=%d %#" PRIx64 " %#" PRIx64, cond, w, a, b);
        CHECK(set(a, b) == expected, "set cond=%d w=%d %#" PRIx64 " %#" PRIx64, cond, w, a, b);
        CHECK(inverted(a, b) == 1 - expected, "inverted cond=%d w=%d %#" PRIx64 " %#" PRIx64, cond, w, a, b);
      }
      asm_executable_free(&branch_exe);
      asm_executable_free(&set_exe);
      asm_executable_free(&inverted_exe);
    }
    for (size_t k = 0; k < 12; k++) {
      int64_t imm = IMMEDIATES[k];
      masm_t m;
      asm_executable_t branch_exe, branch32_exe;

      masm_init(&m);
      asm_label_t taken = masm_new_label(&m);
      masm_branch64_imm(&m, cond, ARG0, imm, taken);
      emit_return_taken(&m, taken);
      binary_fn branch = (binary_fn)jit_finish(&m, &branch_exe);

      masm_init(&m);
      taken = masm_new_label(&m);
      masm_branch32_imm(&m, cond, ARG0, (int32_t)imm, taken);
      emit_return_taken(&m, taken);
      binary_fn branch32 = (binary_fn)jit_finish(&m, &branch32_exe);

      for (size_t i = 0; i < 16; i++) {
        uint64_t a = VALUES[i];
        uint64_t expected = evaluate_comparison(cond, ASM_W64, a, (uint64_t)imm);
        CHECK(branch(a, 0) == expected, "branch64_imm cond=%d %#" PRIx64 " %#" PRIx64, cond, a, (uint64_t)imm);
        expected = evaluate_comparison(cond, ASM_W32, a, (uint64_t)imm);
        CHECK(branch32(a, 0) == expected, "branch32_imm cond=%d %#" PRIx64 " %#" PRIx64, cond, a, (uint64_t)imm);
      }
      asm_executable_free(&branch_exe);
      asm_executable_free(&branch32_exe);
    }
  }
}

typedef uint64_t (*memory_fn)(const uint64_t *, uint64_t);

static void memory_compares(void) {
  static constexpr asm_cond_t conditions[4] = {
    ASM_COND_EQUAL, ASM_COND_NOT_EQUAL, ASM_COND_LESS_THAN, ASM_COND_ABOVE,
  };
  for (size_t c = 0; c < 4; c++) for (size_t k = 0; k < 12; k++) {
    asm_cond_t cond = conditions[c];
    int64_t imm = IMMEDIATES[k];
    masm_t m;
    asm_executable_t exe, exe32, exe_register;

    masm_init(&m);
    asm_label_t taken = masm_new_label(&m), not_taken = masm_new_label(&m);
    masm_branch64_memory_imm(&m, cond, asm_addr(ARG0, 8), imm, taken);
    masm_jump(&m, not_taken);
    masm_bind(&m, taken);
    masm_move_imm32(&m, RET0, 1);
    masm_ret(&m);
    masm_bind(&m, not_taken);
    masm_move_imm32(&m, RET0, 0);
    masm_ret(&m);
    memory_fn function = (memory_fn)jit_finish(&m, &exe);

    masm_init(&m);
    taken = masm_new_label(&m);
    masm_branch32_memory_imm(&m, cond, asm_addr_indexed(ARG0, ARG1, ASM_SCALE_8, 0), (int32_t)imm, taken);
    emit_return_taken(&m, taken);
    memory_fn function32 = (memory_fn)jit_finish(&m, &exe32);

    masm_init(&m);
    taken = masm_new_label(&m);
    masm_move_imm64(&m, ARG3, (uint64_t)imm);
    masm_branch64_memory(&m, cond, asm_addr(ARG0, 8), ARG3, taken);
    emit_return_taken(&m, taken);
    memory_fn function_register = (memory_fn)jit_finish(&m, &exe_register);

    for (size_t i = 0; i < 16; i++) {
      uint64_t value = VALUES[i];
      uint64_t memory[2] = { 0, value };
      uint64_t expected = evaluate_comparison(cond, ASM_W64, value, (uint64_t)imm);
      CHECK(function(memory, 0) == expected, "cond=%d %#" PRIx64 " %#" PRIx64, cond, value, (uint64_t)imm);
      CHECK(function_register(memory, 0) == expected, "register cond=%d %#" PRIx64 " %#" PRIx64, cond, value, (uint64_t)imm);
      expected = evaluate_comparison(cond, ASM_W32, value, (uint64_t)imm);
      CHECK(function32(memory, 1) == expected, "32 cond=%d %#" PRIx64 " %#" PRIx64, cond, value, (uint64_t)imm);
    }
    asm_executable_free(&exe);
    asm_executable_free(&exe32);
    asm_executable_free(&exe_register);
  }
}

typedef void (*mutate_fn)(uint64_t *);

static void additions_to_memory(void) {
  for (size_t k = 0; k < 12; k++) {
    int32_t imm = (int32_t)IMMEDIATES[k];
    masm_t m;
    masm_init(&m);
    masm_add32_to_memory_imm(&m, asm_addr(ARG0, 8), imm);
    masm_add64_to_memory_imm(&m, asm_addr(ARG0, 16), imm);
    masm_ret(&m);
    asm_executable_t exe;
    mutate_fn function = (mutate_fn)jit_finish(&m, &exe);
    for (size_t i = 0; i < 16; i++) {
      uint64_t value = VALUES[i];
      uint64_t memory[4] = { value, value, value, value };
      function(memory);
      uint64_t low = (uint32_t)(low32(value) + (uint32_t)imm);
      CHECK(memory[1] == ((value & ~(uint64_t)0xffffffff) | low), "32 %#" PRIx64 " %d", value, imm);
      CHECK(memory[2] == value + (uint64_t)(int64_t)imm, "64 %#" PRIx64 " %d", value, imm);
      CHECK(memory[0] == value && memory[3] == value, "neighbors %#" PRIx64 " %d", value, imm);
    }
    asm_executable_free(&exe);
  }
}

static void loads_and_stores_of_128_bit_constants(void) {
  asm_u128_t first = { .lo = 0xfedcba9876543210, .hi = 0x0123456789abcdef };
  asm_u128_t second = { .lo = UINT64_MAX - 7, .hi = UINT64_MAX };
  masm_t m;
  masm_init(&m);
  masm_load_imm128(&m, F0, first);
  masm_store128(&m, asm_addr(ARG0, 8), F0);
  masm_load_imm128(&m, FHIGH, second);
  masm_store128(&m, asm_addr(ARG0, 24), FHIGH);
  masm_load_imm128(&m, F1, first);
  masm_store128(&m, asm_addr(ARG0, 40), F1);
  masm_ret(&m);
  asm_executable_t exe;
  mutate_fn function = (mutate_fn)jit_finish(&m, &exe);
  uint64_t memory[8];
  for (size_t i = 0; i < 8; i++) memory[i] = 7;
  function(memory);
  CHECK(memory[0] == 7 && memory[7] == 7, "guards %" PRIu64 " %" PRIu64, memory[0], memory[7]);
  CHECK(memory[1] == first.lo && memory[2] == first.hi, "first %#" PRIx64 " %#" PRIx64, memory[1], memory[2]);
  CHECK(memory[3] == second.lo && memory[4] == second.hi, "second %#" PRIx64 " %#" PRIx64, memory[3], memory[4]);
  CHECK(memory[5] == first.lo && memory[6] == first.hi, "first again %#" PRIx64 " %#" PRIx64, memory[5], memory[6]);
  asm_executable_free(&exe);
}

typedef uint64_t (*compare_fn)(const uint64_t *);

static void stack_pointer_comparisons(void) {
  masm_t m;
  masm_init(&m);
  asm_label_t below = masm_new_label(&m);
  masm_branch_if_stack_pointer_below(&m, asm_addr(ARG0, 8), below);
  emit_return_taken(&m, below);
  asm_executable_t exe;
  compare_fn function = (compare_fn)jit_finish(&m, &exe);
  volatile uint64_t local = 0;
  uint64_t near_stack_pointer = (uint64_t)(uintptr_t)&local;
  uint64_t limits[4][2] = {
    { 0, UINT64_MAX }, { 0, near_stack_pointer + 0x10000 }, { 0, near_stack_pointer - 0x10000 }, { 0, 0 },
  };
  CHECK(function(limits[0]) == 1, "limit max");
  CHECK(function(limits[1]) == 1, "limit above");
  CHECK(function(limits[2]) == 0, "limit below");
  CHECK(function(limits[3]) == 0, "limit zero");
  asm_executable_free(&exe);
}

typedef uint64_t (*select_fn)(uint64_t, uint64_t, uint64_t, uint64_t);

static void bit_tests_and_select(void) {
  static constexpr uint64_t masks[8] = {
    1, 0x80, 0x80000000, 0xffffffff, 0x100000000, 0x8000000000000000, 0xffff000000000000, UINT64_MAX,
  };
  static constexpr asm_cond_t conditions[4] = {
    ASM_COND_ZERO, ASM_COND_NON_ZERO, ASM_COND_NEGATIVE, ASM_COND_POSITIVE_OR_ZERO,
  };
  for (size_t c = 0; c < 4; c++) for (size_t k = 0; k < 8; k++) {
    asm_cond_t cond = conditions[c];
    uint64_t mask = masks[k];
    masm_t m;
    asm_executable_t exe32, exe64;

    masm_init(&m);
    asm_label_t taken = masm_new_label(&m);
    masm_branch_test32(&m, cond, ARG0, (uint32_t)mask, taken);
    emit_return_taken(&m, taken);
    binary_fn function32 = (binary_fn)jit_finish(&m, &exe32);

    masm_init(&m);
    taken = masm_new_label(&m);
    masm_branch_test64(&m, cond, ARG0, mask, taken);
    emit_return_taken(&m, taken);
    binary_fn function64 = (binary_fn)jit_finish(&m, &exe64);

    for (size_t i = 0; i < 16; i++) {
      uint64_t value = VALUES[i];
      uint64_t masked32 = value & mask & 0xffffffff, masked64 = value & mask;
      bool expected32, expected64;
      switch (cond) {
        case ASM_COND_ZERO: expected32 = masked32 == 0; expected64 = masked64 == 0; break;
        case ASM_COND_NON_ZERO: expected32 = masked32 != 0; expected64 = masked64 != 0; break;
        case ASM_COND_NEGATIVE:
          expected32 = (masked32 & (1ull << 31)) != 0;
          expected64 = (masked64 & (1ull << 63)) != 0;
          break;
        default:
          expected32 = (masked32 & (1ull << 31)) == 0;
          expected64 = (masked64 & (1ull << 63)) == 0;
          break;
      }
      if ((uint32_t)mask != 0)
        CHECK(function32(value, 0) == expected32, "test32 cond=%d %#" PRIx64 " %#" PRIx64, cond, value, mask);
      CHECK(function64(value, 0) == expected64, "test64 cond=%d %#" PRIx64 " %#" PRIx64, cond, value, mask);
    }
    asm_executable_free(&exe32);
    asm_executable_free(&exe64);
  }

  static constexpr asm_cond_t select_conditions[3] = { ASM_COND_LESS_THAN, ASM_COND_EQUAL, ASM_COND_ABOVE };
  const asm_gpr_t destinations[5] = { FREE, ARG0, ARG1, ARG2, ARG3 };
  for (size_t c = 0; c < 3; c++) for (size_t d = 0; d < 5; d++) {
    asm_cond_t cond = select_conditions[c];
    asm_gpr_t dst = destinations[d];
    masm_t m;
    masm_init(&m);
    masm_select64(&m, cond, ARG0, ARG1, dst, ARG2, ARG3);
    masm_move64(&m, RET0, dst);
    masm_ret(&m);
    asm_executable_t exe;
    select_fn function = (select_fn)jit_finish(&m, &exe);
    for (size_t i = 0; i < 16; i++) for (size_t j = 0; j < 16; j++) {
      uint64_t a = VALUES[i], b = VALUES[j];
      uint64_t expected = evaluate_comparison(cond, ASM_W64, a, b) ? 111 : 222;
      CHECK(function(a, b, 111, 222) == expected, "select cond=%d %#" PRIx64 " %#" PRIx64 " dst=%d", cond, a, b, dst);
    }
    asm_executable_free(&exe);
  }
}

typedef uint64_t (*access_fn)(void *, uint64_t);

static uint64_t read_le(const uint8_t *memory, size_t offset, size_t size) {
  uint64_t value = 0;
  for (size_t i = 0; i < size; i++) value |= (uint64_t)memory[offset + i] << (8 * i);
  return value;
}

static void emit_load(masm_t *m, int kind, asm_gpr_t dst, asm_address_t address) {
  switch (kind) {
    case 0: masm_load8(m, dst, address); break;
    case 1: masm_load8_sign_extend(m, dst, address); break;
    case 2: masm_load16(m, dst, address); break;
    case 3: masm_load16_sign_extend(m, dst, address); break;
    case 4: masm_load32(m, dst, address); break;
    case 5: masm_load32_sign_extend(m, dst, address); break;
    case 6: masm_load64(m, dst, address); break;
  }
}

static void emit_store(masm_t *m, int kind, asm_address_t address, asm_gpr_t src) {
  switch (kind) {
    case 0: masm_store8(m, address, src); break;
    case 1: masm_store16(m, address, src); break;
    case 2: masm_store32(m, address, src); break;
    case 3: masm_store64(m, address, src); break;
  }
}

static void loads_and_stores(void) {
  uint8_t memory[64];
  for (size_t i = 0; i < 64; i++) memory[i] = (uint8_t)((uint8_t)i * 37 + 0x81);
  static constexpr size_t load_sizes[7] = { 1, 1, 2, 2, 4, 4, 8 };
  static constexpr bool load_signed[7] = { false, true, false, true, false, true, false };
  static constexpr int32_t displacements[3] = { 0, 3, -8 };
  for (int kind = 0; kind < 7; kind++)
  for (int s = 0; s < 4; s++)
  for (size_t d = 0; d < 3; d++) {
    asm_scale_t scale = (asm_scale_t)s;
    int64_t scale_bytes = 1 << s;
    masm_t m;
    masm_init(&m);
    emit_load(&m, kind, ARG0, asm_addr_indexed(ARG0, ARG1, scale, displacements[d]));
    masm_move64(&m, RET0, ARG0);
    masm_ret(&m);
    asm_executable_t exe;
    access_fn function = (access_fn)jit_finish(&m, &exe);
    for (uint64_t index = 0; index < 3; index++) {
      size_t size = load_sizes[kind];
      size_t offset = (size_t)(16 + (int64_t)index * scale_bytes + displacements[d]);
      uint64_t expected = read_le(memory, offset, size);
      if (load_signed[kind]) {
        unsigned shift = (unsigned)(64 - 8 * size);
        expected = (uint64_t)sar64((int64_t)(expected << shift), shift);
      }
      CHECK(
        function(memory + 16, index) == expected, "load kind %d size %zu scale %d disp %d index %" PRIu64,
        kind, size, s, displacements[d], index
      );
    }
    asm_executable_free(&exe);
  }

  static constexpr uint64_t stored = 0x1122334455667788;
  for (int kind = 0; kind < 4; kind++) {
    size_t size = (size_t)1 << kind;
    masm_t m;
    masm_init(&m);
    masm_move64(&m, ARG1, ARG1);
    emit_store(&m, kind, asm_addr(ARG0, 5), ARG1);
    masm_ret(&m);
    asm_executable_t exe;
    access_fn function = (access_fn)jit_finish(&m, &exe);
    uint8_t target[16], expected[16];
    memset(target, 0xaa, sizeof(target));
    memset(expected, 0xaa, sizeof(expected));
    for (size_t i = 0; i < size; i++) expected[5 + i] = (uint8_t)(stored >> (8 * i));
    function(target, stored);
    CHECK(memcmp(target, expected, sizeof(target)) == 0, "store size %zu", size);
    asm_executable_free(&exe);
  }

  static constexpr uint64_t immediates[4] = { 0, 7, 0xfffffffffffffff0, 0x123456789abcdef0 };
  for (size_t k = 0; k < 4; k++) {
    uint64_t imm = immediates[k];
    masm_t m;
    masm_init(&m);
    masm_store_imm64(&m, asm_addr(ARG0, 0), imm);
    masm_store_imm32(&m, asm_addr(ARG0, 8), (uint32_t)imm);
    masm_ret(&m);
    asm_executable_t exe;
    access_fn function = (access_fn)jit_finish(&m, &exe);
    uint8_t target[16];
    memset(target, 0xaa, sizeof(target));
    function(target, 0);
    CHECK(read_le(target, 0, 8) == imm, "store_imm64 %#" PRIx64, imm);
    CHECK(read_le(target, 8, 4) == (uint32_t)imm, "store_imm32 %#" PRIx64, imm);
    CHECK(read_le(target, 12, 4) == 0xaaaaaaaa, "store_imm guard %#" PRIx64, imm);
    asm_executable_free(&exe);
  }

  masm_t m;
  asm_executable_t exe;
  masm_init(&m);
  masm_load_double(&m, F3, asm_addr_indexed(ARG0, ARG1, ASM_SCALE_8, 0));
  masm_add_double(&m, F3, F3, F3);
  masm_store_double(&m, asm_addr(ARG0, 0), F3);
  masm_ret(&m);
  access_fn function = (access_fn)jit_finish(&m, &exe);
  double doubles[2] = { 0.0, 1.25 };
  function(doubles, 1);
  CHECK(doubles[0] == 2.5 && doubles[1] == 1.25, "doubles %g %g", doubles[0], doubles[1]);
  asm_executable_free(&exe);

  masm_init(&m);
  masm_load_float_as_double(&m, F3, asm_addr_indexed(ARG0, ARG1, ASM_SCALE_4, 0));
  masm_store_double(&m, asm_addr(ARG0, 8), F3);
  masm_move_double_imm(&m, F4, 0.1);
  masm_store_double_as_float(&m, asm_addr(ARG0, 0), F4);
  masm_ret(&m);
  function = (access_fn)jit_finish(&m, &exe);
  float one_and_a_half = 1.5f;
  uint32_t words[4] = { 0, 0, 0, 0 };
  memcpy(&words[1], &one_and_a_half, sizeof(float));
  function(words, 1);
  float narrowed;
  memcpy(&narrowed, &words[0], sizeof(float));
  CHECK(narrowed == 0.1f, "float %a", (double)narrowed);
  CHECK(asm_f64_from_bits((uint64_t)words[2] | (uint64_t)words[3] << 32) == 1.5, "widened float");
  asm_executable_free(&exe);
}

static const double DOUBLES[16] = {
  0.0, -0.0, 1.0, -1.5, 0.1, -0.3, 2.5, 2147483647.0, -2147483648.0, 2147483648.0, 4294967296.5,
  1e300, __builtin_inf(), -__builtin_inf(), __builtin_nan(""), 2.2250738585072014e-308,
};

static bool same_double(double a, double b) {
  return asm_f64_bits(a) == asm_f64_bits(b) || (isnan(a) && isnan(b));
}

typedef double (*double_binary_fn)(double, double);

enum { OPD_ADD, OPD_SUB, OPD_MUL, OPD_DIV, OPD_SQRT, OPD_FLOOR, OPD_CEIL, OPD_NEG, OPD_ABS, OPD_MOVE, OPD_COUNT };

static const char *const double_names[OPD_COUNT] = {
  "add", "sub", "mul", "div", "sqrt", "floor", "ceil", "neg", "abs", "move",
};

static void emit_double(masm_t *m, int op, asm_fpr_t d, asm_fpr_t l, asm_fpr_t r) {
  switch (op) {
    case OPD_ADD: masm_add_double(m, d, l, r); break;
    case OPD_SUB: masm_sub_double(m, d, l, r); break;
    case OPD_MUL: masm_mul_double(m, d, l, r); break;
    case OPD_DIV: masm_div_double(m, d, l, r); break;
    case OPD_SQRT: masm_sqrt_double(m, d, l); break;
    case OPD_FLOOR: masm_floor_double(m, d, l); break;
    case OPD_CEIL: masm_ceil_double(m, d, l); break;
    case OPD_NEG: masm_neg_double(m, d, l); break;
    case OPD_ABS: masm_abs_double(m, d, l); break;
    case OPD_MOVE: masm_move_double(m, d, l); break;
  }
}

static double reference_double(int op, double a, double b) {
  switch (op) {
    case OPD_ADD: return a + b;
    case OPD_SUB: return a - b;
    case OPD_MUL: return a * b;
    case OPD_DIV: return a / b;
    case OPD_SQRT: return sqrt(a);
    case OPD_FLOOR: return floor(a);
    case OPD_CEIL: return ceil(a);
    case OPD_NEG: return -a;
    case OPD_ABS: return fabs(a);
    case OPD_MOVE: return a;
  }
  return 0.0;
}

static void double_arithmetic(void) {
  const asm_fpr_t registers[3] = { F0, F1, F2 };
  for (int op = 0; op < OPD_COUNT; op++)
  for (int d = 0; d < 3; d++) for (int l = 0; l < 3; l++) for (int r = 0; r < 3; r++) {
    asm_fpr_t dst = registers[d], lhs = registers[l], rhs = registers[r];
    masm_t m;
    masm_init(&m);
    masm_move_double(&m, FTMP0, F0);
    masm_move_double(&m, FTMP1, F1);
    masm_move_double(&m, lhs, FTMP0);
    masm_move_double(&m, rhs, FTMP1);
    emit_double(&m, op, dst, lhs, rhs);
    masm_move_double(&m, F0, dst);
    masm_ret(&m);
    asm_executable_t exe;
    double_binary_fn function = (double_binary_fn)jit_finish(&m, &exe);
    for (size_t i = 0; i < 16; i++) for (size_t j = 0; j < 16; j++) {
      double a = DOUBLES[i], b = lhs == rhs ? a : DOUBLES[j];
      double actual = function(a, b), expected = reference_double(op, a, b);
      CHECK(
        same_double(actual, expected), "%s(%g, %g) = %g, expected %g dst=%d lhs=%d rhs=%d",
        double_names[op], a, b, actual, expected, dst, lhs, rhs
      );
    }
    asm_executable_free(&exe);
  }

  for (size_t i = 0; i < 16; i++) {
    double value = DOUBLES[i];
    masm_t m;
    masm_init(&m);
    masm_move_double_imm(&m, F5, value);
    masm_move_double(&m, F0, F5);
    masm_ret(&m);
    asm_executable_t exe;
    double_binary_fn function = (double_binary_fn)jit_finish(&m, &exe);
    double actual = function(1.0, 2.0);
    CHECK(same_double(actual, value), "move_double_imm %g = %g", value, actual);
    asm_executable_free(&exe);
  }
}

typedef double (*to_double_fn)(uint64_t);
typedef uint64_t (*from_double_fn)(double);
typedef pair_t (*from_double_pair_fn)(double);

static void double_conversions(void) {
  masm_t m;
  asm_executable_t int32_exe, int64_exe, bits_exe, to_bits_exe, truncate_exe;

  masm_init(&m);
  masm_convert_int32_to_double(&m, F0, ARG0);
  masm_ret(&m);
  to_double_fn int32 = (to_double_fn)jit_finish(&m, &int32_exe);

  masm_init(&m);
  masm_convert_int64_to_double(&m, F0, ARG0);
  masm_ret(&m);
  to_double_fn int64 = (to_double_fn)jit_finish(&m, &int64_exe);

  masm_init(&m);
  masm_move_gpr_to_double(&m, F7, ARG0);
  masm_move_double(&m, F0, F7);
  masm_ret(&m);
  to_double_fn bits = (to_double_fn)jit_finish(&m, &bits_exe);

  for (size_t i = 0; i < 16; i++) {
    uint64_t value = VALUES[i];
    CHECK(int32(value) == (double)(int32_t)low32(value), "int32 %#" PRIx64, value);
    CHECK(int64(value) == (double)(int64_t)value, "int64 %#" PRIx64, value);
    CHECK(asm_f64_bits(bits(value)) == value, "bits %#" PRIx64, value);
  }

  masm_init(&m);
  masm_move_double_to_gpr(&m, RET0, F0);
  masm_ret(&m);
  from_double_fn to_bits = (from_double_fn)jit_finish(&m, &to_bits_exe);

  masm_init(&m);
  masm_truncate_double_to_int64(&m, RET0, F0);
  masm_ret(&m);
  from_double_fn truncate = (from_double_fn)jit_finish(&m, &truncate_exe);

  for (size_t i = 0; i < 16; i++) {
    double value = DOUBLES[i];
    CHECK(to_bits(value) == asm_f64_bits(value), "to_bits %g", value);
    if (isfinite(value) && fabs(value) < 9e18)
      CHECK(truncate(value) == (uint64_t)(int64_t)trunc(value), "truncate %g", value);
  }

  asm_executable_free(&int32_exe);
  asm_executable_free(&int64_exe);
  asm_executable_free(&bits_exe);
  asm_executable_free(&to_bits_exe);
  asm_executable_free(&truncate_exe);

  static const double extra[5] = { 5.0, -7.0, 0.5, -0.5, 2147483647.5 };
  for (int nz = 0; nz < 2; nz++) {
    asm_negative_zero_t negative_zero = nz ? ASM_NEGATIVE_ZERO_FAIL : ASM_NEGATIVE_ZERO_ALLOW;
    masm_init(&m);
    asm_label_t fail = masm_new_label(&m);
    masm_branch_convert_double_to_int32(&m, RET0, F0, fail, negative_zero);
    masm_move_imm64(&m, RET1, 0);
    masm_ret(&m);
    masm_bind(&m, fail);
    masm_move_imm64(&m, RET0, 0);
    masm_move_imm64(&m, RET1, 1);
    masm_ret(&m);
    asm_executable_t exe;
    from_double_pair_fn function = (from_double_pair_fn)jit_finish(&m, &exe);
    for (size_t i = 0; i < 21; i++) {
      double value = i < 16 ? DOUBLES[i] : extra[i - 16];
      bool exact = isfinite(value) && value == trunc(value) && value >= -2147483648.0 && value <= 2147483647.0;
      bool is_negative_zero = value == 0.0 && signbit(value);
      pair_t expected = { 0, 1 };
      if (exact && !(is_negative_zero && negative_zero == ASM_NEGATIVE_ZERO_FAIL))
        expected = (pair_t){ (uint32_t)(int32_t)value, 0 };
      pair_t actual = function(value);
      CHECK(
        actual.value == expected.value && actual.status == expected.status,
        "convert %g negative_zero=%d: {%#" PRIx64 ", %" PRIu64 "}", value, nz, actual.value, actual.status
      );
    }
    asm_executable_free(&exe);
  }
}

// Ant: JavaScript ToInt32 (modulo 2^32, NaN and infinities 0), and the branch on it being inexact, which fails for
// -0 and for every NaN, also the NaN-boxed values that are no numbers.
static void js_conversions_wrap_and_check_exactness(void) {
  if (!masm_has_js_convert()) return;
  masm_t m;
  masm_init(&m);
  masm_js_convert_double_to_int32(&m, RET0, F0);
  masm_ret(&m);
  asm_executable_t wrap_exe;
  from_double_fn wrap = (from_double_fn)jit_finish(&m, &wrap_exe);
  masm_init(&m);
  asm_label_t fail = masm_new_label(&m);
  masm_branch_js_convert_double_to_int32(&m, RET0, F0, fail);
  masm_move_imm64(&m, RET1, 0);
  masm_ret(&m);
  masm_bind(&m, fail);
  masm_move_imm64(&m, RET0, 0);
  masm_move_imm64(&m, RET1, 1);
  masm_ret(&m);
  asm_executable_t exe;
  from_double_pair_fn check = (from_double_pair_fn)jit_finish(&m, &exe);
  static const double extra[8] = { 5.0, -7.0, 0.5, -0.5, 2147483647.5, 4294967301.0, -2147483649.0, 1e300 };
  for (size_t i = 0; i < 24; i++) {
    double value = i < 16 ? DOUBLES[i] : extra[i - 16];
    double truncated = isfinite(value) ? trunc(value) : 0.0;
    double modulo = fmod(truncated, 4294967296.0);
    uint32_t expected_bits = isfinite(modulo) ? (uint32_t)(int64_t)modulo : 0;
    CHECK((uint32_t)wrap(value) == expected_bits, "wrap %g: %#" PRIx64, value, wrap(value));
    bool exact = isfinite(value) && value == trunc(value) && value >= -2147483648.0 && value <= 2147483647.0 && !(value == 0.0 && signbit(value));
    pair_t actual = check(value);
    CHECK(actual.status == (exact ? 0u : 1u) && (!exact || (uint32_t)actual.value == (uint32_t)(int32_t)value), "check %g", value);
  }
  for (uint64_t tagged = UINT64_C(0xfff0000000000000); tagged != UINT64_C(0xfff0000000000010); tagged++)
    CHECK(check(asm_f64_from_bits(tagged)).status == 1, "tagged %#" PRIx64, tagged);
  asm_executable_free(&wrap_exe);
  asm_executable_free(&exe);
}

typedef uint64_t (*double_compare_fn)(double, double);

// Math.min and Math.max of doubles: NaN if either is, -0 below +0; and the int32 select both build on.
static void min_max_and_select32(void) {
  masm_t m;
  asm_executable_t min_exe, max_exe, select_exe;
  masm_init(&m);
  masm_min_double(&m, F0, F0, F1);
  masm_ret(&m);
  double_binary_fn min = (double_binary_fn)jit_finish(&m, &min_exe);
  masm_init(&m);
  masm_max_double(&m, F0, F1, F0);
  masm_ret(&m);
  double_binary_fn max = (double_binary_fn)jit_finish(&m, &max_exe);
  for (size_t i = 0; i < 16; i++) for (size_t j = 0; j < 16; j++) {
    double a = DOUBLES[i], b = DOUBLES[j];
    double low = isnan(a) || isnan(b) ? (double)NAN : a == b ? (signbit(a) ? a : b) : a < b ? a : b;
    double high = isnan(a) || isnan(b) ? (double)NAN : a == b ? (signbit(a) ? b : a) : a > b ? a : b;
    double got_min = min(a, b), got_max = max(b, a);
    CHECK(isnan(low) ? isnan(got_min) : asm_f64_bits(got_min) == asm_f64_bits(low), "min %g %g = %g", a, b, got_min);
    CHECK(isnan(high) ? isnan(got_max) : asm_f64_bits(got_max) == asm_f64_bits(high), "max %g %g = %g", a, b, got_max);
  }
  masm_init(&m);
  masm_select32(&m, ASM_COND_LESS_THAN, ARG0, ARG1, RET0, ARG0, ARG1);
  masm_ret(&m);
  uint64_t (*select)(uint64_t, uint64_t) = (uint64_t (*)(uint64_t, uint64_t))jit_finish(&m, &select_exe);
  static const int32_t values[5] = {INT32_MIN, -1, 0, 7, INT32_MAX};
  for (int i = 0; i < 5; i++) for (int j = 0; j < 5; j++) {
    int32_t expected = values[i] < values[j] ? values[i] : values[j];
    CHECK((int32_t)(uint32_t)select((uint32_t)values[i], (uint32_t)values[j]) == expected, "select32 %d %d", values[i], values[j]);
  }
  asm_executable_free(&min_exe);
  asm_executable_free(&max_exe);
  asm_executable_free(&select_exe);
}

static void double_compares(void) {
  for (int c = ASM_DCOND_EQUAL; c <= ASM_DCOND_UNORDERED; c++) {
    asm_dcond_t cond = (asm_dcond_t)c;
    CHECK(asm_dcond_invert(asm_dcond_invert(cond)) == cond, "invert(invert(%d))", c);
    masm_t m;
    asm_executable_t branch_exe, set_exe;

    masm_init(&m);
    asm_label_t taken = masm_new_label(&m);
    masm_branch_double(&m, cond, F0, F1, taken);
    emit_return_taken(&m, taken);
    double_compare_fn branch = (double_compare_fn)jit_finish(&m, &branch_exe);

    masm_init(&m);
    masm_move_imm64(&m, RET0, UINT64_MAX);
    masm_compare_double_set(&m, cond, RET0, F0, F1);
    masm_ret(&m);
    double_compare_fn set = (double_compare_fn)jit_finish(&m, &set_exe);

    for (size_t i = 0; i < 16; i++) for (size_t j = 0; j < 16; j++) {
      double a = DOUBLES[i], b = DOUBLES[j];
      uint64_t expected = asm_dcond_evaluate(cond, a, b);
      CHECK(branch(a, b) == expected, "branch dcond=%d %g %g", c, a, b);
      CHECK(set(a, b) == expected, "set dcond=%d %g %g", c, a, b);
      CHECK(asm_dcond_evaluate(asm_dcond_invert(cond), a, b) == !asm_dcond_evaluate(cond, a, b), "evaluate invert %d", c);
    }
    asm_executable_free(&branch_exe);
    asm_executable_free(&set_exe);
  }
}

[[gnu::noinline]] static uint64_t helper(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
  alignas(16) volatile uint8_t local = 0;
  // The ABI requires a 16-byte aligned stack at calls; an aligned local's address shows whether the caller kept it.
  uint64_t misaligned = (uint64_t)(uintptr_t)&local % 16;
  (void)local;
  return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 1000 * misaligned;
}

typedef pair_t (*call_fn)(uint64_t);
typedef uint64_t (*six_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static void calls_with_frames(void) {
  asm_frame_t frame = masm_frame(ASM_REG(SAVED0) | ASM_REG(SAVED1), 0, 24);
  CHECK(frame.locals_size % 8 == 0, "locals size %u", frame.locals_size);
  masm_t m;
  asm_executable_t exe;
  masm_init(&m);
  masm_emit_prologue(&m, &frame);
  masm_move64(&m, SAVED0, ARG0);
  masm_store64(&m, asm_frame_local(&frame, SP, 8), ARG0);
  size_t argument_count = sizeof(MASM_ARGUMENT_GPRS) / sizeof(MASM_ARGUMENT_GPRS[0]);
  for (size_t i = 0; i < argument_count; i++) masm_move_imm64(&m, MASM_ARGUMENT_GPRS[i], i + 1);
  masm_call_absolute(&m, (uint64_t)(uintptr_t)&helper);
  masm_move64(&m, RET1, SAVED0);
  masm_load64(&m, ARG3, asm_frame_local(&frame, SP, 8));
  masm_add64(&m, RET1, RET1, ARG3);
  masm_emit_epilogue(&m, &frame);
  masm_ret(&m);
  call_fn call = (call_fn)jit_finish(&m, &exe);
  pair_t result = call(21);
  CHECK(result.value == 1 + 4 + 9 + 16 + 25 + 36 && result.status == 42, "call {%" PRIu64 ", %" PRIu64 "}", result.value, result.status);
  asm_executable_free(&exe);

  masm_init(&m);
  masm_add64_imm(&m, ARG0, ARG0, 100);
  masm_jump_absolute(&m, (uint64_t)(uintptr_t)&helper);
  six_fn six = (six_fn)jit_finish(&m, &exe);
  uint64_t tail = six(1, 0, 0, 0, 0, 0);
  CHECK(tail == 101, "tail jump %" PRIu64, tail);
  asm_executable_free(&exe);

  masm_init(&m);
  asm_label_t callee = masm_new_label(&m);
  asm_frame_t empty = masm_frame(0, 0, 0);
  masm_emit_prologue(&m, &empty);
  masm_call(&m, callee);
  masm_move64(&m, ARG3, RET0);
#if defined(__aarch64__)
  a64_adr(&m.a, CALL_TARGET, callee);
#else
  x64_lea_label(&m.a, CALL_TARGET, callee);
#endif
  masm_call_register(&m, CALL_TARGET);
  masm_add64(&m, RET0, RET0, ARG3);
  masm_emit_epilogue(&m, &empty);
  masm_ret(&m);
  masm_bind(&m, callee);
  masm_move_imm64(&m, RET0, 7);
  masm_ret(&m);
  binary_fn local = (binary_fn)jit_finish(&m, &exe);
  uint64_t sum = local(0, 0);
  CHECK(sum == 14, "local calls %" PRIu64, sum);
  asm_executable_free(&exe);
}

typedef uint64_t (*run_fn)(void);

static inline uint64_t magic(uint8_t reg) {
  return 0x5a5a000000000000 | ((uint64_t)reg * 0x01010101);
}

static void emit_harness_entry(masm_t *m) {
#if defined(__aarch64__)
  a64_stp(&m->a, A64_FP, A64_LR, a64_pair_pre_index(A64_SP, -16));
  for (asm_gpr_t r = A64_X19; r < A64_X29; r += 2) a64_stp(&m->a, r, (asm_gpr_t)(r + 1), a64_pair_pre_index(A64_SP, -16));
  a64_stp_d(&m->a, A64_D8, A64_D9, a64_pair_pre_index(A64_SP, -16));
  a64_str_d(&m->a, A64_D10, a64_mem_pre_index(A64_SP, -16));
#else
  ASM_REGSET_EACH(asm_regset_without(MASM_CALLEE_SAVED_GPRS, X64_RSP), reg) x64_push(&m->a, reg);
  x64_sub_ri(&m->a, ASM_W64, X64_RSP, 8);
#endif
}

static void emit_harness_exit(masm_t *m) {
#if defined(__aarch64__)
  a64_ldr_d(&m->a, A64_D10, a64_mem_post_index(A64_SP, 16));
  a64_ldp_d(&m->a, A64_D8, A64_D9, a64_pair_post_index(A64_SP, 16));
  for (int r = A64_X27; r >= A64_X19; r -= 2) a64_ldp(&m->a, (asm_gpr_t)r, (asm_gpr_t)(r + 1), a64_pair_post_index(A64_SP, 16));
  a64_ldp(&m->a, A64_FP, A64_LR, a64_pair_post_index(A64_SP, 16));
#else
  x64_add_ri(&m->a, ASM_W64, X64_RSP, 8);
  ASM_REGSET_EACH_REVERSE(asm_regset_without(MASM_CALLEE_SAVED_GPRS, X64_RSP), reg) x64_pop(&m->a, reg);
#endif
}

static void prologue_and_epilogue_preserve_callee_saved_registers(void) {
  asm_regset_t callee_saved = MASM_CALLEE_SAVED_GPRS;
#if defined(__aarch64__)
  asm_regset_t checked = asm_regset_without(asm_regset_without(callee_saved, A64_SP), A64_LR);
  CHECK(checked == 0x3ff80000, "checked registers %#x", checked);
#else
  asm_regset_t checked = asm_regset_without(callee_saved, X64_RSP);
  CHECK(
    checked == (ASM_REG(X64_RBX) | ASM_REG(X64_RBP) | ASM_REG(X64_R12) | ASM_REG(X64_R13) | ASM_REG(X64_R14) | ASM_REG(X64_R15)),
    "checked registers %#x", checked
  );
#endif
  static constexpr uint32_t locals_sizes[4] = { 0, 8, 24, 4096 };
  const asm_regset_t fpr_sets[2] = { 0, ASM_REG(FSAVED0) | ASM_REG(FSAVED1) | ASM_REG(FSAVED2) };
  for (size_t l = 0; l < 4; l++) for (size_t f = 0; f < 2; f++) {
    uint32_t locals = locals_sizes[l];
    asm_regset_t saved_fprs = fpr_sets[f];
    asm_frame_t frame = masm_frame(callee_saved, saved_fprs, locals);
    masm_t m;
    masm_init(&m);
    asm_label_t inner = masm_new_label(&m);
    emit_harness_entry(&m);
    ASM_REGSET_EACH(checked, reg) masm_move_imm64(&m, reg, magic(reg));
    masm_move_double_imm(&m, FSAVED0, 3.0);
    masm_call(&m, inner);
    ASM_REGSET_EACH(checked, reg) {
      masm_move_imm64(&m, ARG3, magic(reg));
      masm_xor64(&m, ARG3, ARG3, reg);
      masm_or64(&m, RET0, RET0, ARG3);
    }
    if (saved_fprs != 0) {
      masm_move_double_to_gpr(&m, ARG3, FSAVED0);
      masm_xor64_imm(&m, ARG3, ARG3, asm_f64_bits(3.0));
      masm_or64(&m, RET0, RET0, ARG3);
    }
    emit_harness_exit(&m);
    masm_ret(&m);

    masm_bind(&m, inner);
    masm_emit_prologue(&m, &frame);
    ASM_REGSET_EACH(checked, reg) masm_move_imm64(&m, reg, 0xbad);
    ASM_REGSET_EACH(saved_fprs, reg) masm_move_double_imm(&m, reg, 13.0);
    if (locals > 0) {
      masm_store_imm64(&m, asm_frame_local(&frame, SP, 0), 0xbad);
      masm_store_imm64(&m, asm_frame_local(&frame, SP, frame.locals_size - 8), 0xbad);
    }
    masm_move64(&m, RET0, SP);
    masm_and64_imm(&m, RET0, RET0, 15);
    masm_emit_epilogue(&m, &frame);
    masm_ret(&m);

    asm_executable_t exe;
    run_fn function = (run_fn)jit_finish(&m, &exe);
    uint64_t result = function();
    CHECK(result == 0, "locals %u fprs %#x: %#" PRIx64, locals, saved_fprs, result);
    asm_executable_free(&exe);
  }
}

static void loops_with_backward_branches(void) {
  masm_t m;
  masm_init(&m);
  asm_label_t head = masm_new_label(&m), done = masm_new_label(&m);
  masm_move_imm64(&m, FREE, 0);
  masm_bind(&m, head);
  masm_branch64_imm(&m, ASM_COND_EQUAL, ARG0, 0, done);
  masm_add64(&m, FREE, FREE, ARG0);
  masm_sub64_imm(&m, ARG0, ARG0, 1);
  masm_jump(&m, head);
  masm_bind(&m, done);
  masm_move64(&m, RET0, FREE);
  masm_ret(&m);
  asm_executable_t exe;
  binary_fn function = (binary_fn)jit_finish(&m, &exe);
  uint64_t sum = function(100, 0), none = function(0, 0);
  CHECK(sum == 5050, "loop sum %" PRIu64, sum);
  CHECK(none == 0, "empty loop %" PRIu64, none);
  asm_executable_free(&exe);
}

static void *heap_constant(asm_code_heap_t *heap, asm_executable_t *exe, uint64_t value, uint32_t padding) {
  masm_t m;
  masm_init(&m);
  masm_move_imm64(&m, RET0, value);
  masm_ret(&m);
  for (uint32_t i = 0; i < padding; i++) masm_move_imm64(&m, TMP0, i);
  asm_code_t code;
  if (masm_finish(&m, &code) != ASM_OK) exit(1);
  bool placed = asm_code_heap_place(heap, exe, code.bytes, code.len);
  asm_code_free(&code);
  if (!placed) {
    fprintf(stderr, "placing code in the heap failed\n");
    exit(1);
  }
  return exe->address;
}

static void code_heap_packs_and_reuses_freed_code(void) {
  asm_code_heap_t *heap = asm_code_heap_new();
  enum { COUNT = 64 };
  asm_executable_t exes[COUNT];
  binary_fn functions[COUNT];
  for (uint32_t i = 0; i < COUNT; i++) functions[i] = (binary_fn)heap_constant(heap, &exes[i], 1000 + i, i % 7);
  asm_code_heap_stats_t full = asm_code_heap_stats(heap);
  CHECK(full.chunks == 1 && full.blocks == COUNT, "chunks %u blocks %u", full.chunks, full.blocks);
  for (uint32_t i = 0; i < COUNT; i++) CHECK(functions[i](0, 0) == 1000 + i, "function %u", i);
  for (uint32_t i = 0; i < COUNT; i += 2) asm_executable_free(&exes[i]);
  asm_code_heap_stats_t half = asm_code_heap_stats(heap);
  CHECK(half.blocks == COUNT / 2 && half.live < full.live, "after frees: blocks %u live %zu", half.blocks, half.live);
  for (uint32_t i = 0; i < COUNT; i += 2) {
    functions[i] = (binary_fn)heap_constant(heap, &exes[i], 5000 + i, i % 7);
    CHECK(exes[i].address < (void *)((uint8_t *)exes[COUNT - 1].address + exes[COUNT - 1].size), "freed space not reused for %u", i);
  }
  for (uint32_t i = 0; i < COUNT; i++) CHECK(functions[i](0, 0) == (i % 2 ? 1000 : 5000) + i, "function %u after reuse", i);
  asm_code_heap_stats_t reused = asm_code_heap_stats(heap);
  CHECK(reused.mapped == full.mapped, "mapped %zu, was %zu", reused.mapped, full.mapped);
  asm_executable_t big;
  binary_fn large = (binary_fn)heap_constant(heap, &big, 77, 70000);
  CHECK(large(0, 0) == 77, "large code");
  CHECK(asm_code_heap_stats(heap).chunks == 2, "large code chunk");
  asm_executable_free(&big);
  CHECK(asm_code_heap_stats(heap).chunks == 1, "empty chunk kept: %u", asm_code_heap_stats(heap).chunks);
  for (uint32_t i = 0; i < COUNT; i++) asm_executable_free(&exes[i]);
  asm_code_heap_stats_t empty = asm_code_heap_stats(heap);
  CHECK(empty.blocks == 0 && empty.live == 0, "blocks %u live %zu", empty.blocks, empty.live);
  asm_code_heap_destroy(heap);
}

typedef struct {
  const char *name;
  void (*run)(void);
} test_case_t;

static const test_case_t tests[] = {
  { "integer_arithmetic_and_logic", integer_arithmetic_and_logic },
  { "integer_operations_with_immediates", integer_operations_with_immediates },
  { "overflow_checking_arithmetic", overflow_checking_arithmetic },
  { "compares_and_branches", compares_and_branches },
  { "memory_compares", memory_compares },
  { "additions_to_memory", additions_to_memory },
  { "loads_and_stores_of_128_bit_constants", loads_and_stores_of_128_bit_constants },
  { "stack_pointer_comparisons", stack_pointer_comparisons },
  { "bit_tests_and_select", bit_tests_and_select },
  { "loads_and_stores", loads_and_stores },
  { "double_arithmetic", double_arithmetic },
  { "double_conversions", double_conversions },
  { "js_conversions_wrap_and_check_exactness", js_conversions_wrap_and_check_exactness },
  { "double_compares", double_compares },
  { "min_max_and_select32", min_max_and_select32 },
  { "calls_with_frames", calls_with_frames },
  { "prologue_and_epilogue_preserve_callee_saved_registers", prologue_and_epilogue_preserve_callee_saved_registers },
  { "loops_with_backward_branches", loops_with_backward_branches },
  { "code_heap_packs_and_reuses_freed_code", code_heap_packs_and_reuses_freed_code },
};

int main(void) {
  size_t passed = 0, failed = 0;
  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    int before = failures;
    tests[i].run();
    bool ok = failures == before;
    printf("%s %s\n", ok ? "ok  " : "FAIL", tests[i].name);
    if (ok) passed++;
    else failed++;
  }
  printf("%zu passed, %zu failed\n", passed, failed);
  return failed ? 1 : 0;
}

#endif
