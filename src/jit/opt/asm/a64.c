#include "a64.h"
#include <stdlib.h>

static constexpr size_t VENEER_SLACK = 64;
static constexpr size_t VENEER_HORIZON = 4096;

static inline bool is_mask(uint64_t value) {
  return value != 0 && ((value + 1) & value) == 0;
}

static inline bool is_shifted_mask(uint64_t value) {
  return value != 0 && is_mask((value - 1) | value);
}

static inline uint32_t trailing_ones(uint64_t value) {
  return ~value ? (uint32_t)__builtin_ctzll(~value) : 64;
}

static inline uint32_t leading_ones(uint64_t value) {
  return ~value ? (uint32_t)__builtin_clzll(~value) : 64;
}

bool a64_encode_logical_immediate(uint64_t imm, asm_width_t width, uint32_t *out) {
  uint32_t register_size = asm_width_bits(width);
  if (imm == 0 || imm == UINT64_MAX) return false;
  if (register_size == 32 && ((imm >> 32) != 0 || imm == UINT32_MAX)) return false;

  uint32_t size = register_size;
  for (;;) {
    size /= 2;
    uint64_t mask = (UINT64_C(1) << size) - 1;
    if ((imm & mask) != ((imm >> size) & mask)) {
      size *= 2;
      break;
    }
    if (size <= 2) break;
  }

  uint64_t mask = UINT64_MAX >> (64 - size);
  uint64_t element = imm & mask;
  uint32_t rotation, ones;
  if (is_shifted_mask(element)) {
    rotation = (uint32_t)__builtin_ctzll(element);
    ones = trailing_ones(element >> rotation);
  } else {
    element |= ~mask;
    if (!is_shifted_mask(~element)) return false;
    uint32_t leading = leading_ones(element);
    rotation = 64 - leading;
    ones = leading + trailing_ones(element) - (64 - size);
  }

  uint32_t immr = (size - rotation) & (size - 1);
  uint32_t nimms = (~(size - 1) << 1) | (ones - 1);
  uint32_t n = ((nimms >> 6) & 1) ^ 1;
  *out = (n << 12) | (immr << 6) | (nimms & 0x3f);
  return true;
}

bool a64_encode_fp_immediate(double value, uint8_t *out) {
  uint64_t bits = asm_f64_bits(value);
  if (bits & ((UINT64_C(1) << 48) - 1)) return false;
  uint64_t exponent = (bits >> 52) & 0x7ff;
  uint64_t replicated = (exponent >> 2) & 0xff;
  uint64_t b = replicated & 1;
  if ((replicated != 0 && replicated != 0xff) || (exponent >> 10) == b) return false;
  uint64_t sign = bits >> 63;
  *out = (uint8_t)((sign << 7) | (b << 6) | ((exponent & 3) << 4) | ((bits >> 48) & 0xf));
  return true;
}

static inline uint32_t sf(asm_width_t width) {
  return width == ASM_W64 ? UINT32_C(1) << 31 : 0;
}

static inline uint32_t rd(asm_gpr_t r) { return r & 31u; }
static inline uint32_t rn(asm_gpr_t r) { return (r & 31u) << 5; }
static inline uint32_t rm(asm_gpr_t r) { return (r & 31u) << 16; }
static inline uint32_t vd(asm_fpr_t r) { return r & 31u; }
static inline uint32_t vn(asm_fpr_t r) { return (r & 31u) << 5; }
static inline uint32_t vm(asm_fpr_t r) { return (r & 31u) << 16; }

void a64_init(a64_asm_t *a) {
  asm_buffer_init(&a->buf);
  a->veneer_candidates = NULL;
  a->veneer_count = a->veneer_cap = 0;
  a->next_veneer_check = SIZE_MAX;
}

void a64_free(a64_asm_t *a) {
  asm_buffer_free(&a->buf);
  free(a->veneer_candidates);
  a->veneer_candidates = NULL;
  a->veneer_count = a->veneer_cap = 0;
}

asm_error_t a64_finish(a64_asm_t *a, asm_code_t *out) {
  free(a->veneer_candidates);
  a->veneer_candidates = NULL;
  a->veneer_count = a->veneer_cap = 0;
  return asm_buffer_finish(&a->buf, out);
}

static inline size_t sat_sub(size_t x, size_t y) {
  return x > y ? x - y : 0;
}

static bool deadline(const a64_asm_t *a, uint32_t id, size_t *out) {
  asm_pending_fixup_t fixup;
  if (!asm_pending_fixup(&a->buf, id, &fixup)) return false;
  *out = fixup.at + (size_t)asm_fixup_max_forward_distance(fixup.kind);
  return true;
}

static void push_candidate(a64_asm_t *a, uint32_t id) {
  if (a->veneer_count == a->veneer_cap) {
    size_t cap = a->veneer_cap ? a->veneer_cap * 2 : 16;
    uint32_t *grown = realloc(a->veneer_candidates, cap * sizeof(*grown));
    if (!grown) asm_panic("out of memory");
    a->veneer_candidates = grown;
    a->veneer_cap = cap;
  }
  a->veneer_candidates[a->veneer_count++] = id;
}

static void track_short_branch(a64_asm_t *a, uint32_t id) {
  if (id == ASM_NO_FIXUP) return;
  size_t due;
  deadline(a, id, &due);
  push_candidate(a, id);
  size_t pool_size = 4 * (a->veneer_count + 1);
  size_t check = sat_sub(a->next_veneer_check, 4);
  size_t limit = sat_sub(due, pool_size + VENEER_SLACK);
  a->next_veneer_check = check < limit ? check : limit;
}

static size_t earliest_deadline(const a64_asm_t *a) {
  size_t earliest = SIZE_MAX;
  for (size_t i = 0; i < a->veneer_count; i++) {
    size_t due;
    if (deadline(a, a->veneer_candidates[i], &due) && due < earliest) earliest = due;
  }
  return earliest;
}

static void emit_veneer_pool(a64_asm_t *a, size_t horizon) {
  size_t due_count = 0, remaining = 0;
  uint32_t *due_ids = malloc((a->veneer_count ? a->veneer_count : 1) * sizeof(*due_ids));
  if (!due_ids) asm_panic("out of memory");
  for (size_t i = 0; i < a->veneer_count; i++) {
    uint32_t id = a->veneer_candidates[i];
    size_t due;
    if (deadline(a, id, &due) && due <= horizon) due_ids[due_count++] = id;
    else a->veneer_candidates[remaining++] = id;
  }
  a->veneer_count = remaining;
  asm_emit_u32(&a->buf, 0x14000000 | (uint32_t)(due_count + 1));
  for (size_t i = 0; i < due_count; i++) {
    asm_pending_fixup_t fixup;
    asm_pending_fixup(&a->buf, due_ids[i], &fixup);
    size_t veneer = a64_offset(a);
    asm_emit_u32(&a->buf, 0x14000000);
    asm_add_fixup(&a->buf, fixup.label, veneer, ASM_FIXUP_A64_BRANCH26);
    asm_redirect_fixup(&a->buf, due_ids[i], veneer);
  }
  free(due_ids);
}

static void check_veneers(a64_asm_t *a, size_t size) {
  size_t kept = 0;
  for (size_t i = 0; i < a->veneer_count; i++) {
    asm_pending_fixup_t fixup;
    if (asm_pending_fixup(&a->buf, a->veneer_candidates[i], &fixup)) a->veneer_candidates[kept++] = a->veneer_candidates[i];
  }
  a->veneer_count = kept;
  if (kept == 0) {
    a->next_veneer_check = SIZE_MAX;
    return;
  }
  size_t pool_size = 4 * (a->veneer_count + 1);
  size_t limit = a64_offset(a) + size + pool_size + VENEER_SLACK;
  if (earliest_deadline(a) <= limit) emit_veneer_pool(a, limit + VENEER_HORIZON);
  pool_size = 4 * (a->veneer_count + 1);
  size_t earliest = earliest_deadline(a);
  a->next_veneer_check = earliest == SIZE_MAX ? SIZE_MAX : sat_sub(earliest, pool_size + VENEER_SLACK);
}

static inline void reserve(a64_asm_t *a, size_t size) {
  if (a64_offset(a) + size >= a->next_veneer_check) check_veneers(a, size);
}

void a64_emit(a64_asm_t *a, uint32_t word) {
  reserve(a, 4);
  asm_emit_u32(&a->buf, word);
}

void a64_data_u32(a64_asm_t *a, uint32_t value) {
  reserve(a, 4);
  asm_emit_u32(&a->buf, value);
}

void a64_data_u64(a64_asm_t *a, uint64_t value) {
  reserve(a, 8);
  asm_emit_u64(&a->buf, value);
}

void a64_data_bytes(a64_asm_t *a, const void *bytes, size_t n) {
  reserve(a, n);
  asm_emit_bytes(&a->buf, bytes, n);
}

void a64_align_code(a64_asm_t *a, size_t alignment) {
  ASM_CHECK(alignment >= 4 && (alignment & (alignment - 1)) == 0, "bad code alignment");
  ASM_CHECK(a64_offset(a) % 4 == 0, "unaligned code offset");
  while (a64_offset(a) % alignment != 0) a64_nop(a);
}

void a64_align_data(a64_asm_t *a, size_t alignment) {
  ASM_CHECK(alignment && (alignment & (alignment - 1)) == 0, "alignment must be a power of two");
  reserve(a, (alignment - a64_offset(a) % alignment) % alignment);
  asm_align_with(&a->buf, alignment, 0);
}

static void move_wide(a64_asm_t *a, uint32_t opcode, asm_width_t width, asm_gpr_t r, uint16_t imm16, uint8_t shift) {
  ASM_CHECK(shift % 16 == 0 && shift < asm_width_bits(width), "bad move wide shift");
  a64_emit(a, opcode | sf(width) | ((uint32_t)(shift / 16) << 21) | ((uint32_t)imm16 << 5) | rd(r));
}

void a64_movz(a64_asm_t *a, asm_width_t width, asm_gpr_t r, uint16_t imm16, uint8_t shift) {
  move_wide(a, 0x52800000, width, r, imm16, shift);
}

void a64_movn(a64_asm_t *a, asm_width_t width, asm_gpr_t r, uint16_t imm16, uint8_t shift) {
  move_wide(a, 0x12800000, width, r, imm16, shift);
}

void a64_movk(a64_asm_t *a, asm_width_t width, asm_gpr_t r, uint16_t imm16, uint8_t shift) {
  move_wide(a, 0x72800000, width, r, imm16, shift);
}

static void logical_immediate(a64_asm_t *a, uint32_t opcode, asm_width_t width, asm_gpr_t d, asm_gpr_t n, uint32_t encoding) {
  a64_emit(a, opcode | sf(width) | (encoding << 10) | rn(n) | rd(d));
}

void a64_mov_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t r, uint64_t imm) {
  if (width == ASM_W32) imm &= UINT32_MAX;
  size_t halfwords = asm_width_bits(width) / 16;
  size_t zero_count = 0, ones_count = 0;
  for (size_t i = 0; i < halfwords; i++) {
    uint16_t halfword = (uint16_t)(imm >> (16 * i));
    zero_count += halfword == 0;
    ones_count += halfword == 0xffff;
  }
  size_t instruction_count = halfwords - (zero_count > ones_count ? zero_count : ones_count);
  uint32_t encoding;
  if (instruction_count > 1 && a64_encode_logical_immediate(imm, width, &encoding)) {
    logical_immediate(a, 0x32000000, width, r, A64_XZR, encoding);
    return;
  }
  bool inverted = ones_count > zero_count;
  uint16_t skip = inverted ? 0xffff : 0;
  bool first = true;
  for (size_t i = 0; i < halfwords; i++) {
    uint16_t value = (uint16_t)(imm >> (16 * i));
    if (value == skip) continue;
    uint8_t shift = (uint8_t)(16 * i);
    if (!first) a64_movk(a, width, r, value, shift);
    else if (inverted) a64_movn(a, width, r, (uint16_t)~value, shift);
    else a64_movz(a, width, r, value, shift);
    first = false;
  }
  if (first) {
    if (inverted) a64_movn(a, width, r, 0, 0);
    else a64_movz(a, width, r, 0, 0);
  }
}

size_t a64_mov_imm_instruction_count(uint64_t imm) {
  size_t zero_count = 0, ones_count = 0;
  for (size_t i = 0; i < 4; i++) {
    uint16_t halfword = (uint16_t)(imm >> (16 * i));
    zero_count += halfword == 0;
    ones_count += halfword == 0xffff;
  }
  size_t count = 4 - (zero_count > ones_count ? zero_count : ones_count);
  if (count < 1) count = 1;
  uint32_t encoding;
  if (count > 1 && a64_encode_logical_immediate(imm, ASM_W64, &encoding)) return 1;
  return count;
}

void a64_mov(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t m) {
  a64_orr(a, width, d, A64_XZR, m, A64_SHIFT_LSL, 0);
}

void a64_mov_sp(a64_asm_t *a, asm_gpr_t d, asm_gpr_t n) {
  a64_add_imm(a, ASM_W64, d, n, 0, false);
}

static void add_sub_imm(a64_asm_t *a, uint32_t opcode, asm_width_t width, asm_gpr_t d, asm_gpr_t n, uint32_t imm12, bool shift12) {
  ASM_CHECK(imm12 < 4096, "add/sub immediate out of range");
  a64_emit(a, opcode | sf(width) | ((uint32_t)shift12 << 22) | (imm12 << 10) | rn(n) | rd(d));
}

void a64_add_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, uint32_t imm12, bool shift12) {
  add_sub_imm(a, 0x11000000, width, d, n, imm12, shift12);
}

void a64_adds_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, uint32_t imm12, bool shift12) {
  add_sub_imm(a, 0x31000000, width, d, n, imm12, shift12);
}

void a64_sub_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, uint32_t imm12, bool shift12) {
  add_sub_imm(a, 0x51000000, width, d, n, imm12, shift12);
}

void a64_subs_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, uint32_t imm12, bool shift12) {
  add_sub_imm(a, 0x71000000, width, d, n, imm12, shift12);
}

void a64_cmp_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t n, uint32_t imm12, bool shift12) {
  a64_subs_imm(a, width, A64_XZR, n, imm12, shift12);
}

void a64_cmn_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t n, uint32_t imm12, bool shift12) {
  a64_adds_imm(a, width, A64_XZR, n, imm12, shift12);
}

static void shifted_register(
  a64_asm_t *a, uint32_t opcode, asm_width_t width, asm_gpr_t d, asm_gpr_t n,
  asm_gpr_t m, a64_shift_t shift, uint8_t amount
) {
  ASM_CHECK(amount < asm_width_bits(width), "shift amount out of range");
  a64_emit(a, opcode | sf(width) | ((uint32_t)shift << 22) | rm(m) | ((uint32_t)amount << 10) | rn(n) | rd(d));
}

void a64_add(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, a64_shift_t shift, uint8_t amount) {
  ASM_CHECK(shift != A64_SHIFT_ROR, "add cannot rotate");
  shifted_register(a, 0x0b000000, width, d, n, m, shift, amount);
}

void a64_adds(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, a64_shift_t shift, uint8_t amount) {
  ASM_CHECK(shift != A64_SHIFT_ROR, "adds cannot rotate");
  shifted_register(a, 0x2b000000, width, d, n, m, shift, amount);
}

void a64_sub(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, a64_shift_t shift, uint8_t amount) {
  ASM_CHECK(shift != A64_SHIFT_ROR, "sub cannot rotate");
  shifted_register(a, 0x4b000000, width, d, n, m, shift, amount);
}

void a64_subs(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, a64_shift_t shift, uint8_t amount) {
  ASM_CHECK(shift != A64_SHIFT_ROR, "subs cannot rotate");
  shifted_register(a, 0x6b000000, width, d, n, m, shift, amount);
}

void a64_cmp(a64_asm_t *a, asm_width_t width, asm_gpr_t n, asm_gpr_t m) {
  a64_subs(a, width, A64_XZR, n, m, A64_SHIFT_LSL, 0);
}

void a64_cmn(a64_asm_t *a, asm_width_t width, asm_gpr_t n, asm_gpr_t m) {
  a64_adds(a, width, A64_XZR, n, m, A64_SHIFT_LSL, 0);
}

void a64_neg(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t m) {
  a64_sub(a, width, d, A64_XZR, m, A64_SHIFT_LSL, 0);
}

void a64_negs(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t m) {
  a64_subs(a, width, d, A64_XZR, m, A64_SHIFT_LSL, 0);
}

static void extended_register(
  a64_asm_t *a, uint32_t opcode, asm_width_t width, asm_gpr_t d, asm_gpr_t n,
  asm_gpr_t m, a64_extend_t extend, uint8_t amount
) {
  ASM_CHECK(amount <= 4, "extended register shift out of range");
  a64_emit(a, opcode | sf(width) | rm(m) | ((uint32_t)extend << 13) | ((uint32_t)amount << 10) | rn(n) | rd(d));
}

void a64_add_extended(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, a64_extend_t extend, uint8_t amount) {
  extended_register(a, 0x0b200000, width, d, n, m, extend, amount);
}

void a64_sub_extended(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, a64_extend_t extend, uint8_t amount) {
  extended_register(a, 0x4b200000, width, d, n, m, extend, amount);
}

void a64_subs_extended(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, a64_extend_t extend, uint8_t amount) {
  extended_register(a, 0x6b200000, width, d, n, m, extend, amount);
}

static void logical_imm(a64_asm_t *a, uint32_t opcode, asm_width_t width, asm_gpr_t d, asm_gpr_t n, uint64_t imm) {
  uint32_t encoding;
  if (!a64_encode_logical_immediate(imm, width, &encoding))
    asm_panic("%#llx is not a valid %u-bit logical immediate", (unsigned long long)imm, asm_width_bits(width));
  logical_immediate(a, opcode, width, d, n, encoding);
}

void a64_and_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, uint64_t imm) { logical_imm(a, 0x12000000, width, d, n, imm); }
void a64_orr_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, uint64_t imm) { logical_imm(a, 0x32000000, width, d, n, imm); }
void a64_eor_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, uint64_t imm) { logical_imm(a, 0x52000000, width, d, n, imm); }
void a64_ands_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, uint64_t imm) { logical_imm(a, 0x72000000, width, d, n, imm); }

void a64_tst_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t n, uint64_t imm) {
  a64_ands_imm(a, width, A64_XZR, n, imm);
}

void a64_and(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, a64_shift_t shift, uint8_t amount) {
  shifted_register(a, 0x0a000000, width, d, n, m, shift, amount);
}

void a64_bic(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, a64_shift_t shift, uint8_t amount) {
  shifted_register(a, 0x0a200000, width, d, n, m, shift, amount);
}

void a64_orr(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, a64_shift_t shift, uint8_t amount) {
  shifted_register(a, 0x2a000000, width, d, n, m, shift, amount);
}

void a64_orn(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, a64_shift_t shift, uint8_t amount) {
  shifted_register(a, 0x2a200000, width, d, n, m, shift, amount);
}

void a64_eor(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, a64_shift_t shift, uint8_t amount) {
  shifted_register(a, 0x4a000000, width, d, n, m, shift, amount);
}

void a64_ands(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, a64_shift_t shift, uint8_t amount) {
  shifted_register(a, 0x6a000000, width, d, n, m, shift, amount);
}

void a64_tst(a64_asm_t *a, asm_width_t width, asm_gpr_t n, asm_gpr_t m) {
  a64_ands(a, width, A64_XZR, n, m, A64_SHIFT_LSL, 0);
}

void a64_mvn(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t m) {
  a64_orn(a, width, d, A64_XZR, m, A64_SHIFT_LSL, 0);
}

static void bitfield(a64_asm_t *a, uint32_t opcode, asm_width_t width, asm_gpr_t d, asm_gpr_t n, uint8_t immr, uint8_t imms) {
  ASM_CHECK(immr < asm_width_bits(width) && imms < asm_width_bits(width), "bitfield position out of range");
  uint32_t nbit = width == ASM_W64 ? UINT32_C(1) << 22 : 0;
  a64_emit(a, opcode | sf(width) | nbit | ((uint32_t)immr << 16) | ((uint32_t)imms << 10) | rn(n) | rd(d));
}

void a64_ubfm(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, uint8_t immr, uint8_t imms) {
  bitfield(a, 0x53000000, width, d, n, immr, imms);
}

void a64_sbfm(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, uint8_t immr, uint8_t imms) {
  bitfield(a, 0x13000000, width, d, n, immr, imms);
}

void a64_lsl_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, uint8_t amount) {
  uint8_t size = (uint8_t)asm_width_bits(width);
  ASM_CHECK(amount < size, "shift amount out of range");
  a64_ubfm(a, width, d, n, (uint8_t)((size - amount) % size), (uint8_t)(size - 1 - amount));
}

void a64_lsr_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, uint8_t amount) {
  a64_ubfm(a, width, d, n, amount, (uint8_t)(asm_width_bits(width) - 1));
}

void a64_asr_imm(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, uint8_t amount) {
  a64_sbfm(a, width, d, n, amount, (uint8_t)(asm_width_bits(width) - 1));
}

static void data_processing_2(a64_asm_t *a, uint32_t opcode, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m) {
  a64_emit(a, opcode | sf(width) | rm(m) | rn(n) | rd(d));
}

void a64_lsl(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m) { data_processing_2(a, 0x1ac02000, width, d, n, m); }
void a64_lsr(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m) { data_processing_2(a, 0x1ac02400, width, d, n, m); }
void a64_asr(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m) { data_processing_2(a, 0x1ac02800, width, d, n, m); }
void a64_ror(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m) { data_processing_2(a, 0x1ac02c00, width, d, n, m); }
void a64_sdiv(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m) { data_processing_2(a, 0x1ac00c00, width, d, n, m); }
void a64_udiv(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m) { data_processing_2(a, 0x1ac00800, width, d, n, m); }

void a64_sxtw(a64_asm_t *a, asm_gpr_t d, asm_gpr_t n) { a64_sbfm(a, ASM_W64, d, n, 0, 31); }
void a64_sxtb(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n) { a64_sbfm(a, width, d, n, 0, 7); }
void a64_sxth(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n) { a64_sbfm(a, width, d, n, 0, 15); }
void a64_uxtb(a64_asm_t *a, asm_gpr_t d, asm_gpr_t n) { a64_ubfm(a, ASM_W32, d, n, 0, 7); }
void a64_uxth(a64_asm_t *a, asm_gpr_t d, asm_gpr_t n) { a64_ubfm(a, ASM_W32, d, n, 0, 15); }

static void data_processing_3(a64_asm_t *a, uint32_t opcode, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, asm_gpr_t ra) {
  a64_emit(a, opcode | rm(m) | ((ra & 31u) << 10) | rn(n) | rd(d));
}

void a64_madd(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, asm_gpr_t ra) {
  data_processing_3(a, 0x1b000000 | sf(width), d, n, m, ra);
}

void a64_msub(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, asm_gpr_t ra) {
  data_processing_3(a, 0x1b008000 | sf(width), d, n, m, ra);
}

void a64_mul(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m) {
  a64_madd(a, width, d, n, m, A64_XZR);
}

void a64_smull(a64_asm_t *a, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m) { data_processing_3(a, 0x9b200000, d, n, m, A64_XZR); }
void a64_umull(a64_asm_t *a, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m) { data_processing_3(a, 0x9ba00000, d, n, m, A64_XZR); }
void a64_smulh(a64_asm_t *a, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m) { data_processing_3(a, 0x9b400000, d, n, m, A64_XZR); }
void a64_umulh(a64_asm_t *a, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m) { data_processing_3(a, 0x9bc00000, d, n, m, A64_XZR); }

void a64_check_smull_overflow(a64_asm_t *a, asm_gpr_t product) {
  a64_subs_extended(a, ASM_W64, A64_XZR, product, product, A64_EXTEND_SXTW, 0);
}

void a64_check_smulh_overflow(a64_asm_t *a, asm_gpr_t high, asm_gpr_t low) {
  a64_subs(a, ASM_W64, A64_XZR, high, low, A64_SHIFT_ASR, 63);
}

static void conditional_select(a64_asm_t *a, uint32_t opcode, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, a64_cond_t cond) {
  a64_emit(a, opcode | sf(width) | rm(m) | ((uint32_t)cond << 12) | rn(n) | rd(d));
}

void a64_csel(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, a64_cond_t cond) {
  conditional_select(a, 0x1a800000, width, d, n, m, cond);
}

void a64_csinc(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, a64_cond_t cond) {
  conditional_select(a, 0x1a800400, width, d, n, m, cond);
}

void a64_csinv(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, a64_cond_t cond) {
  conditional_select(a, 0x5a800000, width, d, n, m, cond);
}

void a64_csneg(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_gpr_t n, asm_gpr_t m, a64_cond_t cond) {
  conditional_select(a, 0x5a800400, width, d, n, m, cond);
}

void a64_cset(a64_asm_t *a, asm_width_t width, asm_gpr_t d, a64_cond_t cond) {
  a64_csinc(a, width, d, A64_XZR, A64_XZR, a64_cond_invert(cond));
}

void a64_csetm(a64_asm_t *a, asm_width_t width, asm_gpr_t d, a64_cond_t cond) {
  a64_csinv(a, width, d, A64_XZR, A64_XZR, a64_cond_invert(cond));
}

void a64_load_store(a64_asm_t *a, a64_memop_t op, uint8_t rt, a64_mem_t operand) {
  uint32_t size_log2 = a64_memop_size_log2(op);
  uint32_t t = rt & 31u;
  uint32_t base_opcode = (uint32_t)op;
  int32_t offset = operand.offset;
  switch (operand.mode) {
    case A64_ADDR_OFFSET: {
      int32_t size = 1 << size_log2;
      if (offset >= 0 && offset % size == 0 && offset / size < 4096) {
        uint32_t imm12 = (uint32_t)(offset / size);
        a64_emit(a, base_opcode | (UINT32_C(1) << 24) | (imm12 << 10) | rn(operand.base) | t);
      } else {
        ASM_CHECK(offset >= -256 && offset < 256, "load/store offset %d not encodable", offset);
        a64_emit(a, base_opcode | (((uint32_t)offset & 0x1ff) << 12) | rn(operand.base) | t);
      }
      return;
    }
    case A64_ADDR_REGISTER:
      ASM_CHECK(
        operand.extend == A64_EXTEND_UXTW || operand.extend == A64_EXTEND_UXTX ||
        operand.extend == A64_EXTEND_SXTW || operand.extend == A64_EXTEND_SXTX,
        "bad load/store register extend"
      );
      a64_emit(a,
        base_opcode | (UINT32_C(1) << 21) | rm(operand.index) | ((uint32_t)operand.extend << 13) |
        ((uint32_t)operand.shifted << 12) | (UINT32_C(2) << 10) | rn(operand.base) | t
      );
      return;
    case A64_ADDR_PRE_INDEX:
      ASM_CHECK(offset >= -256 && offset < 256, "pre-index offset %d not encodable", offset);
      a64_emit(a, base_opcode | (((uint32_t)offset & 0x1ff) << 12) | (UINT32_C(3) << 10) | rn(operand.base) | t);
      return;
    case A64_ADDR_POST_INDEX:
      ASM_CHECK(offset >= -256 && offset < 256, "post-index offset %d not encodable", offset);
      a64_emit(a, base_opcode | (((uint32_t)offset & 0x1ff) << 12) | (UINT32_C(1) << 10) | rn(operand.base) | t);
      return;
  }
}

void a64_ldr(a64_asm_t *a, asm_width_t width, asm_gpr_t rt, a64_mem_t operand) {
  a64_load_store(a, width == ASM_W64 ? A64_MEM_LDR_X : A64_MEM_LDR_W, rt, operand);
}

void a64_str(a64_asm_t *a, asm_width_t width, asm_gpr_t rt, a64_mem_t operand) {
  a64_load_store(a, width == ASM_W64 ? A64_MEM_STR_X : A64_MEM_STR_W, rt, operand);
}

void a64_ldrb(a64_asm_t *a, asm_gpr_t rt, a64_mem_t operand) { a64_load_store(a, A64_MEM_LDRB, rt, operand); }
void a64_ldrh(a64_asm_t *a, asm_gpr_t rt, a64_mem_t operand) { a64_load_store(a, A64_MEM_LDRH, rt, operand); }

void a64_ldrsb(a64_asm_t *a, asm_width_t width, asm_gpr_t rt, a64_mem_t operand) {
  a64_load_store(a, width == ASM_W64 ? A64_MEM_LDRSB_X : A64_MEM_LDRSB_W, rt, operand);
}

void a64_ldrsh(a64_asm_t *a, asm_width_t width, asm_gpr_t rt, a64_mem_t operand) {
  a64_load_store(a, width == ASM_W64 ? A64_MEM_LDRSH_X : A64_MEM_LDRSH_W, rt, operand);
}

void a64_ldrsw(a64_asm_t *a, asm_gpr_t rt, a64_mem_t operand) { a64_load_store(a, A64_MEM_LDRSW, rt, operand); }
void a64_strb(a64_asm_t *a, asm_gpr_t rt, a64_mem_t operand) { a64_load_store(a, A64_MEM_STRB, rt, operand); }
void a64_strh(a64_asm_t *a, asm_gpr_t rt, a64_mem_t operand) { a64_load_store(a, A64_MEM_STRH, rt, operand); }
void a64_ldr_d(a64_asm_t *a, asm_fpr_t rt, a64_mem_t operand) { a64_load_store(a, A64_MEM_LDR_D, rt, operand); }
void a64_str_d(a64_asm_t *a, asm_fpr_t rt, a64_mem_t operand) { a64_load_store(a, A64_MEM_STR_D, rt, operand); }

static void load_store_pair(a64_asm_t *a, uint32_t opcode, uint8_t rt, uint8_t rt2, a64_pair_t operand) {
  uint32_t mode = operand.mode == A64_ADDR_POST_INDEX ? 1 : operand.mode == A64_ADDR_OFFSET ? 2 : 3;
  ASM_CHECK(operand.mode != A64_ADDR_REGISTER, "pair operands have no register form");
  ASM_CHECK(
    operand.offset % 8 == 0 && operand.offset >= -512 && operand.offset < 512,
    "pair offset %d not encodable", operand.offset
  );
  uint32_t imm7 = (uint32_t)(operand.offset / 8) & 0x7f;
  a64_emit(a, opcode | (mode << 23) | (imm7 << 15) | ((rt2 & 31u) << 10) | rn(operand.base) | (rt & 31u));
}

void a64_stp(a64_asm_t *a, asm_gpr_t rt, asm_gpr_t rt2, a64_pair_t operand) { load_store_pair(a, 0xa8000000, rt, rt2, operand); }
void a64_ldp(a64_asm_t *a, asm_gpr_t rt, asm_gpr_t rt2, a64_pair_t operand) { load_store_pair(a, 0xa8400000, rt, rt2, operand); }
void a64_stp_d(a64_asm_t *a, asm_fpr_t rt, asm_fpr_t rt2, a64_pair_t operand) { load_store_pair(a, 0x6c000000, rt, rt2, operand); }
void a64_ldp_d(a64_asm_t *a, asm_fpr_t rt, asm_fpr_t rt2, a64_pair_t operand) { load_store_pair(a, 0x6c400000, rt, rt2, operand); }

static inline uint32_t kind_shift(asm_fixup_kind_t kind) {
  return kind == ASM_FIXUP_A64_BRANCH19 || kind == ASM_FIXUP_A64_BRANCH14 ? 5 : 0;
}

static void branch_to_label(a64_asm_t *a, uint32_t word, asm_fixup_kind_t kind, asm_label_t label, uint32_t invert) {
  reserve(a, 8);
  size_t target = a64_label_offset(a, label);
  if (target != ASM_UNBOUND && !asm_fixup_reaches(kind, a64_offset(a), target)) {
    a64_emit(a, (word ^ invert) | (UINT32_C(2) << kind_shift(kind)));
    a64_b(a, label);
    return;
  }
  size_t at = a64_offset(a);
  a64_emit(a, word);
  track_short_branch(a, asm_add_fixup(&a->buf, label, at, kind));
}

static void emit_with_fixup(a64_asm_t *a, uint32_t word, asm_label_t label, asm_fixup_kind_t kind) {
  reserve(a, 4);
  size_t at = a64_offset(a);
  asm_emit_u32(&a->buf, word);
  asm_add_fixup(&a->buf, label, at, kind);
}

void a64_b(a64_asm_t *a, asm_label_t label) {
  emit_with_fixup(a, 0x14000000, label, ASM_FIXUP_A64_BRANCH26);
}

void a64_bl(a64_asm_t *a, asm_label_t label) {
  emit_with_fixup(a, 0x94000000, label, ASM_FIXUP_A64_BRANCH26);
}

void a64_b_cond(a64_asm_t *a, a64_cond_t cond, asm_label_t label) {
  ASM_CHECK(cond != A64_COND_AL, "b.al is not a conditional branch");
  branch_to_label(a, 0x54000000 | cond, ASM_FIXUP_A64_BRANCH19, label, 1);
}

void a64_cbz(a64_asm_t *a, asm_width_t width, asm_gpr_t rt, asm_label_t label) {
  branch_to_label(a, 0x34000000 | sf(width) | rd(rt), ASM_FIXUP_A64_BRANCH19, label, UINT32_C(1) << 24);
}

void a64_cbnz(a64_asm_t *a, asm_width_t width, asm_gpr_t rt, asm_label_t label) {
  branch_to_label(a, 0x35000000 | sf(width) | rd(rt), ASM_FIXUP_A64_BRANCH19, label, UINT32_C(1) << 24);
}

static inline uint32_t test_bit_branch(uint32_t opcode, asm_gpr_t rt, uint8_t bit) {
  ASM_CHECK(bit < 64, "test bit out of range");
  return opcode | ((uint32_t)(bit >> 5) << 31) | ((uint32_t)(bit & 31) << 19) | rd(rt);
}

void a64_tbz(a64_asm_t *a, asm_gpr_t rt, uint8_t bit, asm_label_t label) {
  branch_to_label(a, test_bit_branch(0x36000000, rt, bit), ASM_FIXUP_A64_BRANCH14, label, UINT32_C(1) << 24);
}

void a64_tbnz(a64_asm_t *a, asm_gpr_t rt, uint8_t bit, asm_label_t label) {
  branch_to_label(a, test_bit_branch(0x37000000, rt, bit), ASM_FIXUP_A64_BRANCH14, label, UINT32_C(1) << 24);
}

void a64_br(a64_asm_t *a, asm_gpr_t n) { a64_emit(a, 0xd61f0000 | rn(n)); }
void a64_blr(a64_asm_t *a, asm_gpr_t n) { a64_emit(a, 0xd63f0000 | rn(n)); }
void a64_ret(a64_asm_t *a) { a64_emit(a, 0xd65f0000 | rn(A64_LR)); }

void a64_ldr_literal(a64_asm_t *a, asm_gpr_t rt, asm_label_t label) {
  emit_with_fixup(a, 0x58000000 | rd(rt), label, ASM_FIXUP_A64_BRANCH19);
}

void a64_ldr_literal_double(a64_asm_t *a, asm_fpr_t rt, asm_label_t label) {
  emit_with_fixup(a, 0x5c000000 | vd(rt), label, ASM_FIXUP_A64_BRANCH19);
}

void a64_ldr_literal_quad(a64_asm_t *a, asm_fpr_t rt, asm_label_t label) {
  emit_with_fixup(a, 0x9c000000 | vd(rt), label, ASM_FIXUP_A64_BRANCH19);
}

void a64_adr(a64_asm_t *a, asm_gpr_t d, asm_label_t label) {
  emit_with_fixup(a, 0x10000000 | rd(d), label, ASM_FIXUP_A64_ADR21);
}

void a64_nop(a64_asm_t *a) { a64_emit(a, 0xd503201f); }
void a64_brk(a64_asm_t *a, uint16_t imm16) { a64_emit(a, 0xd4200000 | ((uint32_t)imm16 << 5)); }
void a64_udf(a64_asm_t *a, uint16_t imm16) { a64_emit(a, imm16); }

void a64_fmov(a64_asm_t *a, asm_fpr_t d, asm_fpr_t n) { a64_emit(a, 0x1e604000 | vn(n) | vd(d)); }
void a64_fmov_from_gpr(a64_asm_t *a, asm_fpr_t d, asm_gpr_t n) { a64_emit(a, 0x9e670000 | rn(n) | vd(d)); }
void a64_fmov_to_gpr(a64_asm_t *a, asm_gpr_t d, asm_fpr_t n) { a64_emit(a, 0x9e660000 | vn(n) | rd(d)); }

void a64_fmov_imm(a64_asm_t *a, asm_fpr_t d, double value) {
  uint8_t imm8;
  if (!a64_encode_fp_immediate(value, &imm8)) asm_panic("%g is not an encodable fmov immediate", value);
  a64_emit(a, 0x1e601000 | ((uint32_t)imm8 << 13) | vd(d));
}

static void fp_data_processing_2(a64_asm_t *a, uint32_t opcode, asm_fpr_t d, asm_fpr_t n, asm_fpr_t m) {
  a64_emit(a, opcode | vm(m) | vn(n) | vd(d));
}

void a64_fadd(a64_asm_t *a, asm_fpr_t d, asm_fpr_t n, asm_fpr_t m) { fp_data_processing_2(a, 0x1e602800, d, n, m); }
void a64_fsub(a64_asm_t *a, asm_fpr_t d, asm_fpr_t n, asm_fpr_t m) { fp_data_processing_2(a, 0x1e603800, d, n, m); }
void a64_fmul(a64_asm_t *a, asm_fpr_t d, asm_fpr_t n, asm_fpr_t m) { fp_data_processing_2(a, 0x1e600800, d, n, m); }
void a64_fdiv(a64_asm_t *a, asm_fpr_t d, asm_fpr_t n, asm_fpr_t m) { fp_data_processing_2(a, 0x1e601800, d, n, m); }
void a64_fmin(a64_asm_t *a, asm_fpr_t d, asm_fpr_t n, asm_fpr_t m) { fp_data_processing_2(a, 0x1e605800, d, n, m); }
void a64_fmax(a64_asm_t *a, asm_fpr_t d, asm_fpr_t n, asm_fpr_t m) { fp_data_processing_2(a, 0x1e604800, d, n, m); }

void a64_fsqrt(a64_asm_t *a, asm_fpr_t d, asm_fpr_t n) { a64_emit(a, 0x1e61c000 | vn(n) | vd(d)); }
void a64_frintm(a64_asm_t *a, asm_fpr_t d, asm_fpr_t n) { a64_emit(a, 0x1e654000 | vn(n) | vd(d)); }
void a64_frintp(a64_asm_t *a, asm_fpr_t d, asm_fpr_t n) { a64_emit(a, 0x1e64c000 | vn(n) | vd(d)); }
void a64_frintz(a64_asm_t *a, asm_fpr_t d, asm_fpr_t n) { a64_emit(a, 0x1e65c000 | vn(n) | vd(d)); }
void a64_fneg(a64_asm_t *a, asm_fpr_t d, asm_fpr_t n) { a64_emit(a, 0x1e614000 | vn(n) | vd(d)); }
void a64_fabs(a64_asm_t *a, asm_fpr_t d, asm_fpr_t n) { a64_emit(a, 0x1e60c000 | vn(n) | vd(d)); }

void a64_fcmp(a64_asm_t *a, asm_fpr_t n, asm_fpr_t m) { a64_emit(a, 0x1e602000 | vm(m) | vn(n)); }
void a64_fcmp_zero(a64_asm_t *a, asm_fpr_t n) { a64_emit(a, 0x1e602008 | vn(n)); }

void a64_scvtf(a64_asm_t *a, asm_width_t width, asm_fpr_t d, asm_gpr_t n) {
  a64_emit(a, 0x1e620000 | sf(width) | rn(n) | vd(d));
}

void a64_fcvt_double_from_single(a64_asm_t *a, asm_fpr_t d, asm_fpr_t n) { a64_emit(a, 0x1e22c000 | vn(n) | vd(d)); }
void a64_fcvt_single_from_double(a64_asm_t *a, asm_fpr_t d, asm_fpr_t n) { a64_emit(a, 0x1e624000 | vn(n) | vd(d)); }

void a64_fcvtzs(a64_asm_t *a, asm_width_t width, asm_gpr_t d, asm_fpr_t n) {
  a64_emit(a, 0x1e780000 | sf(width) | vn(n) | rd(d));
}

void a64_fjcvtzs(a64_asm_t *a, asm_gpr_t d, asm_fpr_t n) {
  a64_emit(a, 0x1e7e0000 | vn(n) | rd(d));
}
