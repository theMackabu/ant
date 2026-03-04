#ifndef REGEX_H
#define REGEX_H

#include "types.h"
#include "gc.h"

void init_regex_module(void);
void cleanup_regex_module(void);

void regex_gc_update_roots(ant_offset_t (*weak_off)(void *ctx, ant_offset_t old), GC_OP_VAL_ARGS);
size_t js_to_pcre2_pattern(const char *src, size_t src_len, char *dst, size_t dst_size);

ant_value_t builtin_regexp_symbol_split(ant_t *js, ant_value_t *args, int nargs);
ant_value_t builtin_regexp_symbol_match(ant_t *js, ant_value_t *args, int nargs);
ant_value_t builtin_regexp_symbol_replace(ant_t *js, ant_value_t *args, int nargs);
ant_value_t builtin_regexp_symbol_search(ant_t *js, ant_value_t *args, int nargs);
ant_value_t builtin_regexp_flags_getter(ant_t *js, ant_value_t *args, int nargs);

ant_value_t is_regexp_like(ant_t *js, ant_value_t value);
ant_value_t reject_regexp_arg(ant_t *js, ant_value_t value, const char *method_name);

ant_value_t do_regex_match_pcre2(
  ant_t *js, const char *pattern_ptr, ant_offset_t pattern_len,
  const char *str_ptr, ant_offset_t str_len,
  bool global_flag, bool ignore_case, bool multiline
);

#endif
