#include "buffer.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void asm_panic(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  fputs("ANT FATAL: asm: ", stderr);
  vfprintf(stderr, fmt, args);
  fputc('\n', stderr);
  va_end(args);
  abort();
}

static void *asm_grow(void *ptr, size_t elem, size_t needed, size_t *cap) {
  if (needed <= *cap) return ptr;
  size_t next = *cap ? *cap * 2 : 64;
  while (next < needed) next *= 2;
  void *grown = realloc(ptr, next * elem);
  if (!grown) asm_panic("out of memory");
  *cap = next;
  return grown;
}

int64_t asm_fixup_max_forward_distance(asm_fixup_kind_t kind) {
  switch (kind) {
    case ASM_FIXUP_X86_REL8: return INT8_MAX;
    case ASM_FIXUP_X86_REL32: return INT32_MAX;
    case ASM_FIXUP_A64_BRANCH26: return ((INT64_C(1) << 25) - 1) * 4;
    case ASM_FIXUP_A64_BRANCH19: return ((INT64_C(1) << 18) - 1) * 4;
    case ASM_FIXUP_A64_BRANCH14: return ((INT64_C(1) << 13) - 1) * 4;
    case ASM_FIXUP_A64_ADR21: return (INT64_C(1) << 20) - 1;
  }
  return 0;
}

int64_t asm_fixup_max_backward_distance(asm_fixup_kind_t kind) {
  switch (kind) {
    case ASM_FIXUP_X86_REL8: return 128;
    case ASM_FIXUP_X86_REL32: return INT64_C(1) << 31;
    case ASM_FIXUP_A64_BRANCH26: return (INT64_C(1) << 25) * 4;
    case ASM_FIXUP_A64_BRANCH19: return (INT64_C(1) << 18) * 4;
    case ASM_FIXUP_A64_BRANCH14: return (INT64_C(1) << 13) * 4;
    case ASM_FIXUP_A64_ADR21: return INT64_C(1) << 20;
  }
  return 0;
}

static inline size_t asm_fixup_origin(asm_fixup_kind_t kind, size_t at) {
  switch (kind) {
    case ASM_FIXUP_X86_REL8: return at + 1;
    case ASM_FIXUP_X86_REL32: return at + 4;
    default: return at;
  }
}

bool asm_fixup_reaches(asm_fixup_kind_t kind, size_t from, size_t to) {
  int64_t distance = (int64_t)to - (int64_t)asm_fixup_origin(kind, from);
  return distance <= asm_fixup_max_forward_distance(kind) && -distance <= asm_fixup_max_backward_distance(kind);
}

void asm_buffer_init(asm_buffer_t *b) {
  *b = (asm_buffer_t){ 0 };
}

void asm_buffer_free(asm_buffer_t *b) {
  free(b->bytes);
  free(b->labels);
  free(b->fixups);
  *b = (asm_buffer_t){ 0 };
}

void asm_buffer_reserve(asm_buffer_t *b, size_t extra) {
  b->bytes = asm_grow(b->bytes, 1, b->len + extra, &b->cap);
}

static inline uint32_t asm_offset_u32(size_t offset) {
  ASM_CHECK(offset <= UINT32_MAX, "code buffer exceeds 4 GiB");
  return (uint32_t)offset;
}

static void asm_patch(asm_buffer_t *b, size_t at, asm_fixup_kind_t kind, size_t target) {
  if (!asm_fixup_reaches(kind, at, target)) {
    asm_buffer_set_error(b, ASM_ERR_BRANCH_OUT_OF_RANGE);
    return;
  }
  int64_t distance = (int64_t)target - (int64_t)asm_fixup_origin(kind, at);
  uint32_t word;
  switch (kind) {
    case ASM_FIXUP_X86_REL8:
      b->bytes[at] = (uint8_t)(int8_t)distance;
      break;
    case ASM_FIXUP_X86_REL32:
      asm_write_u32(b, at, (uint32_t)(int32_t)distance);
      break;
    case ASM_FIXUP_A64_BRANCH26:
      word = asm_read_u32(b, at) & ~UINT32_C(0x03ffffff);
      asm_write_u32(b, at, word | ((uint32_t)(distance >> 2) & 0x03ffffff));
      break;
    case ASM_FIXUP_A64_BRANCH19:
      word = asm_read_u32(b, at) & ~(UINT32_C(0x7ffff) << 5);
      asm_write_u32(b, at, word | (((uint32_t)(distance >> 2) & 0x7ffff) << 5));
      break;
    case ASM_FIXUP_A64_BRANCH14:
      word = asm_read_u32(b, at) & ~(UINT32_C(0x3fff) << 5);
      asm_write_u32(b, at, word | (((uint32_t)(distance >> 2) & 0x3fff) << 5));
      break;
    case ASM_FIXUP_A64_ADR21: {
      word = asm_read_u32(b, at) & ~((UINT32_C(0x3) << 29) | (UINT32_C(0x7ffff) << 5));
      uint32_t immlo = (uint32_t)distance & 0x3;
      uint32_t immhi = (uint32_t)(distance >> 2) & 0x7ffff;
      asm_write_u32(b, at, word | (immlo << 29) | (immhi << 5));
      break;
    }
  }
}

asm_label_t asm_new_label(asm_buffer_t *b) {
  ASM_CHECK(b->label_count < UINT32_MAX, "too many labels");
  b->labels = asm_grow(b->labels, sizeof(*b->labels), (size_t)b->label_count + 1, &b->label_cap);
  b->labels[b->label_count] = (asm_label_state_t){ .value = ASM_NO_FIXUP, .bound = false };
  return b->label_count++;
}

size_t asm_label_offset(const asm_buffer_t *b, asm_label_t label) {
  asm_label_state_t state = b->labels[label];
  return state.bound ? state.value : ASM_UNBOUND;
}

void asm_bind(asm_buffer_t *b, asm_label_t label) {
  size_t offset = asm_buffer_offset(b);
  asm_label_state_t *state = &b->labels[label];
  ASM_CHECK(!state->bound, "label bound twice");
  uint32_t fixup_id = state->value;
  *state = (asm_label_state_t){ .value = asm_offset_u32(offset), .bound = true };
  while (fixup_id != ASM_NO_FIXUP) {
    asm_fixup_t fixup = b->fixups[fixup_id];
    if (!fixup.resolved) {
      asm_patch(b, fixup.at, fixup.kind, offset);
      b->fixups[fixup_id].resolved = true;
    }
    fixup_id = fixup.next;
  }
}

uint32_t asm_add_fixup(asm_buffer_t *b, asm_label_t label, size_t at, asm_fixup_kind_t kind) {
  asm_label_state_t state = b->labels[label];
  if (state.bound) {
    asm_patch(b, at, kind, state.value);
    return ASM_NO_FIXUP;
  }
  ASM_CHECK(b->fixup_count < UINT32_MAX - 1, "too many fixups");
  b->fixups = asm_grow(b->fixups, sizeof(*b->fixups), (size_t)b->fixup_count + 1, &b->fixup_cap);
  uint32_t id = b->fixup_count++;
  b->fixups[id] = (asm_fixup_t){
    .at = asm_offset_u32(at),
    .label = label,
    .next = state.value,
    .kind = kind,
    .resolved = false,
  };
  b->labels[label].value = id;
  return id;
}

bool asm_pending_fixup(const asm_buffer_t *b, uint32_t id, asm_pending_fixup_t *out) {
  const asm_fixup_t *fixup = &b->fixups[id];
  if (fixup->resolved) return false;
  *out = (asm_pending_fixup_t){ .id = id, .at = fixup->at, .kind = fixup->kind, .label = fixup->label };
  return true;
}

void asm_redirect_fixup(asm_buffer_t *b, uint32_t id, size_t target) {
  asm_fixup_t fixup = b->fixups[id];
  ASM_CHECK(!fixup.resolved, "redirecting a resolved fixup");
  asm_patch(b, fixup.at, fixup.kind, target);
  b->fixups[id].resolved = true;
}

void asm_align_with(asm_buffer_t *b, size_t alignment, uint8_t filler) {
  ASM_CHECK(alignment && (alignment & (alignment - 1)) == 0, "alignment must be a power of two");
  while (asm_buffer_offset(b) % alignment != 0) asm_emit_u8(b, filler);
}

asm_error_t asm_buffer_finish(asm_buffer_t *b, asm_code_t *out) {
  asm_error_t error = b->error;
  for (uint32_t i = 0; error == ASM_OK && i < b->fixup_count; i++)
    if (!b->fixups[i].resolved) error = ASM_ERR_UNBOUND_LABEL;
  if (error != ASM_OK) {
    asm_buffer_free(b);
    *out = (asm_code_t){ 0 };
    return error;
  }
  *out = (asm_code_t){ .bytes = b->bytes, .len = b->len, .data_offset = b->len };
  b->bytes = NULL;
  asm_buffer_free(b);
  return ASM_OK;
}

void asm_code_free(asm_code_t *code) {
  free(code->bytes);
  *code = (asm_code_t){ 0 };
}
