#ifndef REGEX_H
#define REGEX_H

#include "types.h"

typedef struct {
  const char *pattern_ptr;
  ant_offset_t pattern_len;
  
  const char *str_ptr;
  ant_offset_t str_len;
  
  bool global;
  bool ignore_case;
  bool multiline;
} regex_match_args_t;

void init_regex_module(void);
void cleanup_regex_module(void);
void gc_sweep_regex_cache(void);
void regexp_note_exec_property_write(void);
void regexp_note_replace_property_write(void);

size_t js_to_pcre2_pattern(
  const char *src, size_t src_len,
  char *dst, size_t dst_size, bool v_flag
);

ant_value_t is_regexp_like(ant_t *js, ant_value_t value);
ant_value_t do_regex_match_pcre2(ant_t *js, regex_match_args_t args);
ant_value_t reject_regexp_arg(ant_t *js, ant_value_t value, const char *method_name);

bool regexp_exec_truthy_try_fast(
  ant_t *js,
  ant_value_t call_func,
  ant_value_t regexp,
  ant_value_t arg,
  ant_value_t *out_result
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

#endif
