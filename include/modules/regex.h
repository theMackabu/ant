#ifndef REGEX_H
#define REGEX_H

#include "types.h"
#include "gc/strings.h"
#include "modules/regex_syntax.h"
#include <string.h>

static constexpr int ANT_REGEXP_STATIC_OVECTOR_COUNT = 10;

typedef struct {
  size_t static_ovector[ANT_REGEXP_STATIC_OVECTOR_COUNT * 2];
  size_t static_paren[2];
  
  uint32_t static_last_group;
  uint16_t static_materialized_mask;

  bool static_pending;
  bool write_guard_armed;
  bool replace_written;
  bool protector_invalid;

  ant_object_t *proto_ptr;
  ant_object_t *ctor_ptr;
} ant_regexp_realm_t;

void init_regex_module(ant_t *js);
void cleanup_regex_module(ant_t *js);
void gc_age_regex_cache(ant_t *js, bool minor);
void regexp_note_replace_property_write(ant_t *js);

ant_value_t is_regexp_like(ant_t *js, ant_value_t value);
ant_value_t regexp_source_value(ant_t *js, ant_value_t rx);
ant_value_t regexp_flags_value(ant_t *js, ant_value_t rx);
ant_value_t reject_regexp_arg(ant_t *js, ant_value_t value, const char *method_name);
ant_value_t regexp_create_literal(ant_t *js, ant_value_t pattern, ant_value_t flags);
ant_value_t regexp_split_limit(ant_t *js, ant_value_t limit, uint32_t *out);

size_t js_to_pcre2_pattern(const char *src, size_t src_len, char *dst, size_t dst_size, uint8_t flags);
js_cstr_t js_to_pcre2_pattern_cstr(const char *src, size_t src_len, char *stack_buf, size_t stack_size, uint8_t flags);

bool regexp_exec_truthy_try_fast(
  ant_t *js,
  ant_value_t call_func,
  ant_value_t regexp,
  ant_value_t arg,
  ant_value_t *out_result
);

bool regexp_split_try_fast(
  ant_t *js,
  ant_value_t rx,
  ant_value_t str,
  ant_value_t limit,
  ant_value_t *out
);

ant_value_t regexp_literal_exec_call(
  ant_t *js,
  ant_value_t pattern,
  ant_value_t flags,
  ant_value_t arg
);

ant_value_t regexp_literal_replace_call(
  ant_t *js,
  ant_value_t str,
  ant_value_t pattern,
  ant_value_t flags,
  ant_value_t replacement
);

static inline bool regexp_flags_valid(const char *flags, size_t len) {
  uint32_t seen = 0;
  for (size_t i = 0; i < len; i++) {
    const char *at = memchr("dgimsuvy", flags[i], 8);
    if (!flags[i] || !at) return false;
    
    uint32_t bit = 1u << (at - "dgimsuvy");
    if (seen & bit) return false;
    
    seen |= bit;
  }
  
  return (seen & 0x60) != 0x60;
}

static inline const char *regexp_literal_syntax_error(
  const char *pattern, size_t pattern_len, const char *flags, size_t flags_len
) {
  bool unicode_sets = memchr(flags, 'v', flags_len) != NULL;
  return regexp_syntax_error(pattern, pattern_len, unicode_sets || memchr(flags, 'u', flags_len), unicode_sets);
}

static inline void regexp_note_property_write(ant_t *js, const char *name, size_t len) {
  if (!name) return;
  if (len == 7 && name[0] == 'r' && memcmp(name, "replace", 7) == 0) regexp_note_replace_property_write(js);
}

#endif
