#ifndef ANT_JIT_OPT_ASM_BUFFER_H
#define ANT_JIT_OPT_ASM_BUFFER_H

#include "asm.h"

typedef enum {
  ASM_OK = 0,
  ASM_ERR_UNBOUND_LABEL,
  ASM_ERR_BRANCH_OUT_OF_RANGE,
} asm_error_t;

typedef uint32_t asm_label_t;

static constexpr size_t ASM_UNBOUND = SIZE_MAX;
static constexpr uint32_t ASM_NO_FIXUP = UINT32_MAX;

typedef enum : uint8_t {
  ASM_FIXUP_X86_REL8,
  ASM_FIXUP_X86_REL32,
  ASM_FIXUP_A64_BRANCH26,
  ASM_FIXUP_A64_BRANCH19,
  ASM_FIXUP_A64_BRANCH14,
  ASM_FIXUP_A64_ADR21,
} asm_fixup_kind_t;

int64_t asm_fixup_max_forward_distance(asm_fixup_kind_t kind);
int64_t asm_fixup_max_backward_distance(asm_fixup_kind_t kind);
bool asm_fixup_reaches(asm_fixup_kind_t kind, size_t from, size_t to);

typedef struct {
  uint32_t value;
  bool bound;
} asm_label_state_t;

typedef struct {
  uint32_t at;
  asm_label_t label;
  uint32_t next;
  asm_fixup_kind_t kind;
  bool resolved;
} asm_fixup_t;

typedef struct {
  uint32_t id;
  size_t at;
  asm_fixup_kind_t kind;
  asm_label_t label;
} asm_pending_fixup_t;

typedef struct {
  uint8_t *bytes;
  size_t len, cap;
  asm_label_state_t *labels;
  uint32_t label_count;
  size_t label_cap;
  asm_fixup_t *fixups;
  uint32_t fixup_count;
  size_t fixup_cap;
  asm_error_t error;
} asm_buffer_t;

typedef struct {
  uint8_t *bytes;
  size_t len;
  size_t data_offset;
} asm_code_t;

typedef struct {
  uint64_t value;
  asm_label_t label;
} masm_constant_t;

typedef struct {
  asm_u128_t value;
  asm_label_t label;
} masm_constant128_t;

void asm_buffer_init(asm_buffer_t *b);
void asm_buffer_free(asm_buffer_t *b);

static inline size_t asm_buffer_offset(const asm_buffer_t *b) {
  return b->len;
}

void asm_buffer_reserve(asm_buffer_t *b, size_t extra);

static inline void asm_emit_u8(asm_buffer_t *b, uint8_t value) {
  if (b->len == b->cap) asm_buffer_reserve(b, 1);
  b->bytes[b->len++] = value;
}

static inline void asm_emit_bytes(asm_buffer_t *b, const void *bytes, size_t n) {
  if (b->cap - b->len < n) asm_buffer_reserve(b, n);
  memcpy(b->bytes + b->len, bytes, n);
  b->len += n;
}

static inline void asm_emit_u16(asm_buffer_t *b, uint16_t value) {
  uint8_t le[2] = { (uint8_t)value, (uint8_t)(value >> 8) };
  asm_emit_bytes(b, le, sizeof(le));
}

static inline void asm_emit_u32(asm_buffer_t *b, uint32_t value) {
  uint8_t le[4] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24) };
  asm_emit_bytes(b, le, sizeof(le));
}

static inline void asm_emit_u64(asm_buffer_t *b, uint64_t value) {
  asm_emit_u32(b, (uint32_t)value);
  asm_emit_u32(b, (uint32_t)(value >> 32));
}

static inline uint32_t asm_read_u32(const asm_buffer_t *b, size_t at) {
  const uint8_t *p = b->bytes + at;
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void asm_write_u32(asm_buffer_t *b, size_t at, uint32_t value) {
  uint8_t *p = b->bytes + at;
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)(value >> 16);
  p[3] = (uint8_t)(value >> 24);
}

static inline void asm_buffer_set_error(asm_buffer_t *b, asm_error_t error) {
  if (b->error == ASM_OK) b->error = error;
}

asm_label_t asm_new_label(asm_buffer_t *b);
size_t asm_label_offset(const asm_buffer_t *b, asm_label_t label);
void asm_bind(asm_buffer_t *b, asm_label_t label);
uint32_t asm_add_fixup(asm_buffer_t *b, asm_label_t label, size_t at, asm_fixup_kind_t kind);
bool asm_pending_fixup(const asm_buffer_t *b, uint32_t id, asm_pending_fixup_t *out);
void asm_redirect_fixup(asm_buffer_t *b, uint32_t id, size_t target);
void asm_align_with(asm_buffer_t *b, size_t alignment, uint8_t filler);

asm_error_t asm_buffer_finish(asm_buffer_t *b, asm_code_t *out);
void asm_code_free(asm_code_t *code);

#endif
