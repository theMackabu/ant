#ifndef ANT_GC_STRINGS_H
#define ANT_GC_STRINGS_H

#include "pool.h"
#include "value.h"
#include <stdbool.h>

static constexpr int STR_SHORT_CONS_THRESHOLD = 13;

enum: uint64_t {
  STR_META_ASCII_SHIFT = 56,
  STR_META_VALID_SHIFT = 58,
  STR_BUILDER_TAIL_CAP = 256,
};

enum {
  STR_UTF_UNKNOWN = 0,
  STR_UTF_VALID = 1,
  STR_UTF_INVALID = 2,
  STR_UTF_INVALID_SAME_LENGTH = 3,
};

enum: uint64_t {
  STR_HEAP_TAG_MASK    = 0x3,
  STR_HEAP_TAG_FLAT    = 0x0,
  STR_HEAP_TAG_ROPE    = 0x1,
  STR_HEAP_TAG_BUILDER = 0x2,
};

static constexpr uint64_t STR_META_FIELD_MASK   = 0x3;
static constexpr uint64_t STR_META_UTF16_MASK   = (UINT64_C(1) << STR_META_ASCII_SHIFT) - 1;
static constexpr uint64_t STR_META_ASCII_MASK   = STR_META_FIELD_MASK << STR_META_ASCII_SHIFT;
static constexpr uint64_t STR_META_VALID_MASK   = STR_META_FIELD_MASK << STR_META_VALID_SHIFT;
static constexpr uint64_t STR_META_STATE_MASK   = ~STR_META_UTF16_MASK;
static constexpr uint64_t STR_UTF16_LEN_UNKNOWN = STR_META_UTF16_MASK;

enum {
  STR_ASCII_UNKNOWN = 0,
  STR_ASCII_YES = 1,
  STR_ASCII_NO = 2,
};

typedef struct {
  ant_offset_t len;
  uint64_t meta;
  char bytes[];
} ant_flat_string_t;

static_assert(
  sizeof(ant_flat_string_t) == 16,
  "flat string header must remain 16 bytes"
);

static_assert(
  offsetof(ant_flat_string_t, bytes) == sizeof(ant_flat_string_t),
  "flat string bytes must follow packed metadata"
);

static_assert(
  offsetof(ant_large_string_alloc_t, meta) - offsetof(ant_large_string_alloc_t, len) ==
    offsetof(ant_flat_string_t, meta),
  "large and pooled string metadata layouts must match"
);

static_assert(
  offsetof(ant_large_string_alloc_t, bytes) - offsetof(ant_large_string_alloc_t, len) ==
    offsetof(ant_flat_string_t, bytes),
  "large and pooled string byte layouts must match"
);

typedef struct ant_builder_chunk {
  struct ant_builder_chunk *next;
  ant_value_t value;
} ant_builder_chunk_t;

struct ant_string_builder {
  ant_offset_t len;
  ant_value_t snapshot;
  ant_builder_chunk_t *head;
  ant_builder_chunk_t *chunk_tail;
  ant_value_t cached;
  uint16_t tail_len;
  uint8_t ascii_state;
  uint8_t in_remember_set;
  char tail[STR_BUILDER_TAIL_CAP];
};

typedef struct {
  const char *ptr;
  size_t len;
  bool needs_free;
} js_cstr_t;

typedef struct {
  size_t count;
  size_t bytes;
} js_intern_stats_t;

static inline bool str_is_heap_rope(ant_value_t value) {
  return vtype(value) == kTypeString && ((vdata(value) & STR_HEAP_TAG_MASK) == STR_HEAP_TAG_ROPE);
}

static inline bool str_is_heap_builder(ant_value_t value) {
  return vtype(value) == kTypeString && ((vdata(value) & STR_HEAP_TAG_MASK) == STR_HEAP_TAG_BUILDER);
}

static inline ant_rope_heap_t *ant_str_rope_ptr(ant_value_t value) {
  return (ant_rope_heap_t *)vptr_masked(value, STR_HEAP_TAG_MASK);
}

static inline ant_string_builder_t *ant_str_builder_ptr(ant_value_t value) {
  return (ant_string_builder_t *)vptr_masked(value, STR_HEAP_TAG_MASK);
}

static inline ant_value_t ant_mkrope_value(ant_rope_heap_t *rope) {
  return mkref_tagged(kTypeString, rope, STR_HEAP_TAG_ROPE);
}

static inline ant_value_t ant_mkbuilder_value(ant_string_builder_t *builder) {
  return mkref_tagged(kTypeString, builder, STR_HEAP_TAG_BUILDER);
}

static inline ant_flat_string_t *str_flat_from_bytes(const char *str) {
  return (ant_flat_string_t *)((char *)str - offsetof(ant_flat_string_t, bytes));
}

static inline ant_flat_string_t *ant_str_flat_ptr(ant_value_t value) {
  if (vtype(value) != kTypeString) return NULL;
  if ((vdata(value) & STR_HEAP_TAG_MASK) != STR_HEAP_TAG_FLAT) return NULL;
  return (ant_flat_string_t *)vptr(value);
}

static inline ant_flat_string_t *large_string_flat_ptr(ant_large_string_alloc_t *alloc) {
  return alloc ? (ant_flat_string_t *)&alloc->len : NULL;
}

static inline ant_large_string_alloc_t *large_string_alloc_from_flat(ant_flat_string_t *flat) {
  return flat ? (ant_large_string_alloc_t *)((char *)flat - offsetof(ant_large_string_alloc_t, len)) : NULL;
}

static inline uint8_t str_detect_ascii_bytes(const char *str, size_t len) {
  const unsigned char *s = (const unsigned char *)str;
  for (size_t i = 0; i < len; i++) {
    if (s[i] >= 0x80) return STR_ASCII_NO;
  }
  return STR_ASCII_YES;
}

static inline uint8_t str_flat_ascii_state(const ant_flat_string_t *flat) {
  if (!flat) return STR_ASCII_UNKNOWN;
  return (uint8_t)((flat->meta & STR_META_ASCII_MASK) >> STR_META_ASCII_SHIFT);
}

static inline uint8_t str_flat_utf_valid_state(const ant_flat_string_t *flat) {
  if (!flat) return STR_UTF_UNKNOWN;
  return (uint8_t)((flat->meta & STR_META_VALID_MASK) >> STR_META_VALID_SHIFT);
}

static inline void str_flat_set_utf_valid_state(ant_flat_string_t *flat, uint8_t state) {
  if (!flat) return;
  flat->meta = (flat->meta & ~STR_META_VALID_MASK) | (
    ((uint64_t)state << STR_META_VALID_SHIFT) & STR_META_VALID_MASK);
}

static inline ant_offset_t str_flat_cached_utf16_len(const ant_flat_string_t *flat) {
  return flat ? (ant_offset_t)(flat->meta & STR_META_UTF16_MASK) : STR_UTF16_LEN_UNKNOWN;
}

static inline void str_flat_init_meta(ant_flat_string_t *flat, uint8_t ascii_state) {
  if (!flat) return;
  flat->meta = ((uint64_t)ascii_state << STR_META_ASCII_SHIFT) | STR_UTF16_LEN_UNKNOWN;
}

static inline void str_flat_set_utf16_len(ant_flat_string_t *flat, ant_offset_t len) {
  if (!flat) return;
  flat->meta = (flat->meta & STR_META_STATE_MASK) | (len & STR_META_UTF16_MASK);
}

static inline void str_set_ascii_state(const char *str, uint8_t state) {
  ant_flat_string_t *flat = str_flat_from_bytes(str);
  str_flat_init_meta(flat, state);
}

static inline bool str_is_ascii(const char *str) {
  ant_flat_string_t *flat = str_flat_from_bytes(str);
  uint8_t state = str_flat_ascii_state(flat);
  if (state == STR_ASCII_UNKNOWN) {
    state = str_detect_ascii_bytes(flat->bytes, (size_t)flat->len);
    str_flat_init_meta(flat, state);
  }
  return state == STR_ASCII_YES;
}

bool utf8_validate_bytes(const char *str, size_t byte_len);

static inline bool str_is_valid_utf8(const char *str) {
  if (str_is_ascii(str)) return true;

  ant_flat_string_t *flat = str_flat_from_bytes(str);
  uint8_t state = str_flat_utf_valid_state(flat);
  
  if (state == STR_UTF_UNKNOWN) {
    state = utf8_validate_bytes(flat->bytes, (size_t)flat->len) 
      ? STR_UTF_VALID : STR_UTF_INVALID;
    str_flat_set_utf_valid_state(flat, state);
  }
  
  return state == STR_UTF_VALID;
}

size_t utf8_export_length_slow(const char *str, size_t str_len);

static inline size_t utf8_export_length(const char *str, size_t str_len) {
  if (str_len == 0) return 0;
  
  uint8_t state = str_flat_utf_valid_state(str_flat_from_bytes(str));
  if (state == STR_UTF_VALID || state == STR_UTF_INVALID_SAME_LENGTH) return str_len;  
  if (state == STR_UTF_UNKNOWN && str_is_ascii(str)) return str_len;
  
  return utf8_export_length_slow(str, str_len);
}

const char *intern_string(const char *str, size_t len);
const char *intern_find(const char *str, size_t len);
size_t intern_length(const char *interned);

js_cstr_t js_to_cstr(ant_t *js, ant_value_t value, char *stack_buf, size_t stack_size);
js_cstr_t js_inspect_cstr(ant_t *js, ant_value_t value, char *stack_buf, size_t stack_size);

ant_offset_t vstr(ant_t *js, ant_value_t value, ant_offset_t *len);
ant_offset_t vstrlen(ant_t *js, ant_value_t value);
ant_offset_t str_len_fast(ant_t *js, ant_value_t str);
ant_offset_t str_utf16_len(ant_t *js, ant_value_t str);

ant_value_t rope_flatten(ant_t *js, ant_value_t rope);
ant_value_t str_materialize(ant_t *js, ant_value_t value);

size_t utf8_export_into(
  const char *str, size_t str_len, uint8_t *dst, 
  size_t dst_len, size_t *out_read_units
);

uint64_t gc_strings_sweep_epoch(void);
js_intern_stats_t js_intern_stats(void);

void gc_strings_begin(ant_t *js);
void gc_strings_sweep(ant_t *js);
void gc_strings_mark(ant_t *js, const void *ptr);

#endif
