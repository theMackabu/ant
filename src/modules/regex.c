// TODO: cleanup module, make cleaner

#include "ant.h"
#include "utf8.h"
#include "errors.h"
#include "internal.h"
#include "utils.h"
#include "escape.h"
#include "descriptors.h"
#include "ptr.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <pcre2.h>

#include "silver/engine.h"
#include "modules/regex.h"
#include "modules/symbol.h"

typedef struct compiled_regex_cache_entry {
  char *pattern;
  size_t pattern_len;
  uint64_t key_hash;
  uint8_t flags_mask;
  pcre2_code *code;
  pcre2_match_data *scratch_match_data;
  ant_shape_t *lastindex_shape;
  uint32_t lastindex_slot;
  uint32_t namecount;
  bool jit_ready;
  bool scratch_in_use;
  size_t object_refs;
  uint8_t cache_refs;
} compiled_regex_cache_entry_t;

typedef struct {
  compiled_regex_cache_entry_t **slots;
  size_t count;
  size_t cap;
} compiled_regex_cache_table_t;

typedef struct {
  compiled_regex_cache_entry_t *compiled;
  pcre2_match_data *match_data;
  bool borrowed;
} regex_match_scope_t;

typedef struct {
  ant_shape_t *shape;
  uint32_t index_slot;
  uint32_t input_slot;
  uint32_t groups_slot;
  uint32_t indices_slot;
} regexp_result_shape_t;

enum {
  REGEXP_FLAG_HAS_INDICES = 1 << 0,
  REGEXP_FLAG_GLOBAL      = 1 << 1,
  REGEXP_FLAG_IGNORE_CASE = 1 << 2,
  REGEXP_FLAG_MULTILINE   = 1 << 3,
  REGEXP_FLAG_DOTALL      = 1 << 4,
  REGEXP_FLAG_UNICODE     = 1 << 5,
  REGEXP_FLAG_UNICODE_SET = 1 << 6,
  REGEXP_FLAG_STICKY      = 1 << 7,
};

/* ANT_REGEX_STATS=1: per-run regex path counters, dumped at exit (atexit,
   works on servers where process.exit skips isolate teardown). */
static uint64_t rxstat_exec_internal, rxstat_shared_fast, rxstat_cache_hit,
  rxstat_cache_miss, rxstat_compiles, rxstat_jit_match,
  rxstat_interp_match, rxstat_max_cache, rxstat_inserts, rxstat_lit_obj,
  rxstat_exec_generic, rxstat_compiled_gen0_hit, rxstat_compiled_gen1_hit,
  rxstat_compiled_miss, rxstat_compiled_promotions, rxstat_compiled_evictions,
  rxstat_match_dynamic, rxstat_flags_calls, rxstat_flags_cached,
  rxstat_flags_reparsed, rxstat_lastindex_reads, rxstat_lastindex_writes,
  rxstat_lastindex_resets, rxstat_flags_direct,
  rxstat_flags_internal, rxstat_lastindex_fast_reads,
  rxstat_lastindex_fast_writes, rxstat_result_arrays,
  rxstat_result_captures, rxstat_result_named_groups,
  rxstat_result_indices, rxstat_symbol_match_calls,
  rxstat_symbol_match_results, rxstat_symbol_replace_calls,
  rxstat_symbol_replace_results, rxstat_symbol_replace_func,
  rxstat_symbol_replace_substitution, rxstat_result_shape_fast,
  rxstat_result_shape_fallback, rxstat_batch_match_calls,
  rxstat_batch_match_results, rxstat_batch_replace_calls,
  rxstat_batch_replace_results, rxstat_batch_guard_rejects,
  rxstat_batch_reject_state, rxstat_batch_reject_exec_location,
  rxstat_batch_reject_exec_accessor, rxstat_batch_reject_exec_value,
  rxstat_batch_reject_flags, rxstat_batch_reject_lastindex;
static bool rxstat_enabled;
static void rxstat_dump(void) {
  fprintf(stderr,
    "[regex-stats] exec_internal=%llu shared_fast=%llu generic_exec=%llu lit_obj=%llu\n"
    "[regex-stats] object-data hit=%llu miss=%llu attaches=%llu max_live=%llu\n"
    "[regex-stats] compiled-cache gen0=%llu gen1=%llu miss=%llu promotions=%llu evictions=%llu\n"
    "[regex-stats] compiles=%llu jit_match=%llu interp_match=%llu dynamic-match-data=%llu\n"
    "[regex-stats] flags calls=%llu direct=%llu internal=%llu cached=%llu reparsed=%llu\n"
    "[regex-stats] lastIndex reads=%llu writes=%llu resets=%llu fast-read=%llu fast-write=%llu\n"
    "[regex-stats] results arrays=%llu captures=%llu named=%llu indices=%llu shape-fast=%llu shape-fallback=%llu\n"
    "[regex-stats] symbol-match calls=%llu discarded-results=%llu\n"
    "[regex-stats] symbol-replace calls=%llu materialized-results=%llu func=%llu substitution=%llu\n"
    "[regex-stats] batch match-calls=%llu match-results=%llu replace-calls=%llu replace-results=%llu guard-rejects=%llu\n"
    "[regex-stats] batch-reject state=%llu exec-location=%llu exec-accessor=%llu exec-value=%llu flags=%llu lastIndex=%llu\n",
    (unsigned long long)rxstat_exec_internal, (unsigned long long)rxstat_shared_fast,
    (unsigned long long)rxstat_exec_generic, (unsigned long long)rxstat_lit_obj,
    (unsigned long long)rxstat_cache_hit, (unsigned long long)rxstat_cache_miss,
    (unsigned long long)rxstat_inserts,
    (unsigned long long)rxstat_max_cache,
    (unsigned long long)rxstat_compiled_gen0_hit,
    (unsigned long long)rxstat_compiled_gen1_hit,
    (unsigned long long)rxstat_compiled_miss,
    (unsigned long long)rxstat_compiled_promotions,
    (unsigned long long)rxstat_compiled_evictions,
    (unsigned long long)rxstat_compiles,
    (unsigned long long)rxstat_jit_match,
    (unsigned long long)rxstat_interp_match,
    (unsigned long long)rxstat_match_dynamic,
    (unsigned long long)rxstat_flags_calls,
    (unsigned long long)rxstat_flags_direct,
    (unsigned long long)rxstat_flags_internal,
    (unsigned long long)rxstat_flags_cached,
    (unsigned long long)rxstat_flags_reparsed,
    (unsigned long long)rxstat_lastindex_reads,
    (unsigned long long)rxstat_lastindex_writes,
    (unsigned long long)rxstat_lastindex_resets,
    (unsigned long long)rxstat_lastindex_fast_reads,
    (unsigned long long)rxstat_lastindex_fast_writes,
    (unsigned long long)rxstat_result_arrays,
    (unsigned long long)rxstat_result_captures,
    (unsigned long long)rxstat_result_named_groups,
    (unsigned long long)rxstat_result_indices,
    (unsigned long long)rxstat_result_shape_fast,
    (unsigned long long)rxstat_result_shape_fallback,
    (unsigned long long)rxstat_symbol_match_calls,
    (unsigned long long)rxstat_symbol_match_results,
    (unsigned long long)rxstat_symbol_replace_calls,
    (unsigned long long)rxstat_symbol_replace_results,
    (unsigned long long)rxstat_symbol_replace_func,
    (unsigned long long)rxstat_symbol_replace_substitution,
    (unsigned long long)rxstat_batch_match_calls,
    (unsigned long long)rxstat_batch_match_results,
    (unsigned long long)rxstat_batch_replace_calls,
    (unsigned long long)rxstat_batch_replace_results,
    (unsigned long long)rxstat_batch_guard_rejects,
    (unsigned long long)rxstat_batch_reject_state,
    (unsigned long long)rxstat_batch_reject_exec_location,
    (unsigned long long)rxstat_batch_reject_exec_accessor,
    (unsigned long long)rxstat_batch_reject_exec_value,
    (unsigned long long)rxstat_batch_reject_flags,
    (unsigned long long)rxstat_batch_reject_lastindex);
}
static void rxstat_init(void) {
  static bool done;
  if (done) return;
  done = true;
  if (getenv("ANT_REGEX_STATS")) { rxstat_enabled = true; atexit(rxstat_dump); }
}

static inline void rxstat_note_result(
  uint32_t captures,
  bool named_groups,
  bool has_indices
) {
  if (!rxstat_enabled) return;
  rxstat_result_arrays++;
  rxstat_result_captures += captures;
  if (named_groups) rxstat_result_named_groups++;
  if (has_indices) rxstat_result_indices++;
}

/* Compiled patterns, PCRE2 scratch state, and RegExp.prototype mutation
   guards belong to one isolate. RegExp objects point directly to shared
   compiled entries through their native payload; the cache below only keeps
   recently used compiled entries warm across object lifetimes. */
struct ant_regex_state {
  compiled_regex_cache_table_t compiled[2];
  size_t live_object_data;

  regexp_result_shape_t result_shape;
  regexp_result_shape_t result_indices_shape;

  pcre2_match_context *match_ctx;
  pcre2_jit_stack *jit_stack;

  bool exec_write_guard_armed;
  bool exec_property_written;
  bool replace_property_written;
};

enum {
  REGEXP_NATIVE_TAG = 0x52454758u, /* REGX */
  REGEX_COMPILED_CACHE_MAX = 1024,
};

static bool regexp_result_shape_init(
  regexp_result_shape_t *cache,
  ant_object_t *array,
  bool with_indices
) {
  if (cache->shape) return true;
  if (!array || !array->shape) return false;

  const char *index_key = intern_string("index", 5);
  const char *input_key = intern_string("input", 5);
  const char *groups_key = intern_string("groups", 6);
  const char *indices_key = with_indices
    ? intern_string("indices", 7)
    : NULL;
  if (!index_key || !input_key || !groups_key || (with_indices && !indices_key))
    return false;

  ant_shape_t *shape = ant_shape_clone(array->shape);
  if (!shape) return false;

  regexp_result_shape_t built = { .shape = shape };
  bool ok =
    ant_shape_add_interned(
      shape, index_key, ANT_PROP_ATTR_DEFAULT, &built.index_slot
    ) &&
    ant_shape_add_interned(
      shape, input_key, ANT_PROP_ATTR_DEFAULT, &built.input_slot
    ) &&
    ant_shape_add_interned(
      shape, groups_key, ANT_PROP_ATTR_DEFAULT, &built.groups_slot
    );
  if (ok && with_indices) {
    ok = ant_shape_add_interned(
      shape, indices_key, ANT_PROP_ATTR_DEFAULT, &built.indices_slot
    );
  }
  if (!ok) {
    ant_shape_release(shape);
    return false;
  }

  *cache = built;
  return true;
}

static __attribute__((noinline)) bool regexp_result_apply_shape(
  ant_t *js,
  ant_value_t array_value,
  ant_value_t index,
  ant_value_t input,
  ant_value_t groups,
  ant_value_t indices,
  bool with_indices
) {
  ant_regex_state_t *state = js->regex_state;
  ant_object_t *array = js_obj_ptr(array_value);
  if (!state || !array || array->type_tag != T_ARR) goto fallback;

  regexp_result_shape_t *cache = with_indices
    ? &state->result_indices_shape
    : &state->result_shape;
  if (!regexp_result_shape_init(cache, array, with_indices)) goto fallback;

  if (array->shape != cache->shape) {
    ant_shape_retain(cache->shape);
    ant_shape_release(array->shape);
    array->shape = cache->shape;
  }
  if (!js_obj_ensure_prop_capacity(array, with_indices ? 4 : 3))
    goto fallback;

  ant_object_prop_set_unchecked(array, cache->index_slot, index);
  ant_object_prop_set_unchecked(array, cache->input_slot, input);
  ant_object_prop_set_unchecked(array, cache->groups_slot, groups);
  gc_write_barrier(js, array, input);
  gc_write_barrier(js, array, groups);
  if (with_indices) {
    ant_object_prop_set_unchecked(array, cache->indices_slot, indices);
    gc_write_barrier(js, array, indices);
  }
  if (rxstat_enabled) rxstat_result_shape_fast++;
  return true;

fallback:
  if (rxstat_enabled) rxstat_result_shape_fallback++;
  return false;
}

void regexp_note_exec_property_write(ant_t *js) {
  ant_regex_state_t *state = js->regex_state;
  if (state && state->exec_write_guard_armed)
    state->exec_property_written = true;
}

void regexp_note_replace_property_write(ant_t *js) {
  ant_regex_state_t *state = js->regex_state;
  if (state && state->exec_write_guard_armed)
    state->replace_property_written = true;
}

static inline uint8_t regexp_parse_flags_mask(const char *fstr, ant_offset_t flen) {
  uint8_t mask = 0;
  for (ant_offset_t k = 0; k < flen; k++) {
  switch (fstr[k]) {
    case 'd': mask |= REGEXP_FLAG_HAS_INDICES; break;
    case 'g': mask |= REGEXP_FLAG_GLOBAL; break;
    case 'i': mask |= REGEXP_FLAG_IGNORE_CASE; break;
    case 'm': mask |= REGEXP_FLAG_MULTILINE; break;
    case 's': mask |= REGEXP_FLAG_DOTALL; break;
    case 'u': mask |= REGEXP_FLAG_UNICODE; break;
    case 'v': mask |= REGEXP_FLAG_UNICODE_SET; break;
    case 'y': mask |= REGEXP_FLAG_STICKY; break;
    default: break;
  }}
  return mask;
}

static inline uint8_t regexp_flags_mask(
  ant_t *js,
  ant_value_t regexp,
  compiled_regex_cache_entry_t **compiled_out
) {
  if (rxstat_enabled) rxstat_flags_calls++;

  compiled_regex_cache_entry_t *compiled = js_get_native(
    regexp, REGEXP_NATIVE_TAG
  );
  if (compiled_out) *compiled_out = compiled;
  if (compiled) {
    if (rxstat_enabled) rxstat_flags_direct++;
    return compiled->flags_mask;
  }

  ant_value_t internal_mask = js_get_slot(regexp, SLOT_REGEXP_FLAGS_MASK);
  if (vtype(internal_mask) == T_NUM) {
    if (rxstat_enabled) rxstat_flags_internal++;
    return (uint8_t)tod(internal_mask);
  }

  ant_prop_loc_t flags_off = lkp(js, regexp, "flags", 5);
  if (!flags_off.obj) return 0;

  ant_value_t flags_val = js_prop_load(flags_off);
  if (vtype(flags_val) != T_STR) return 0;

  ant_value_t cached_flags = js_get_slot(regexp, SLOT_REGEXP_FLAGS_STRING);
  ant_value_t cached = js_get_slot(regexp, SLOT_REGEXP_FLAGS_MASK);
  if (flags_val == cached_flags && vtype(cached) == T_NUM) {
    if (rxstat_enabled) rxstat_flags_cached++;
    return (uint8_t)tod(cached);
  }

  if (rxstat_enabled) rxstat_flags_reparsed++;
  ant_offset_t flen, foff = vstr(js, flags_val, &flen);
  uint8_t mask = regexp_parse_flags_mask((const char *)(uintptr_t)foff, flen);
  js_set_slot(regexp, SLOT_REGEXP_FLAGS_MASK, tov((double)mask));
  js_set_slot(regexp, SLOT_REGEXP_FLAGS_STRING, flags_val);
  
  return mask;
}

static ant_value_t regexp_build_named_groups_meta(ant_t *js, pcre2_code *code) {
  uint32_t namecount = 0;
  pcre2_pattern_info(code, PCRE2_INFO_NAMECOUNT, &namecount);
  if (namecount == 0) return js_mkundef();

  uint32_t nameentrysize = 0;
  PCRE2_SPTR nametable = NULL;
  pcre2_pattern_info(code, PCRE2_INFO_NAMEENTRYSIZE, &nameentrysize);
  pcre2_pattern_info(code, PCRE2_INFO_NAMETABLE, (void *)&nametable);

  ant_value_t meta = js_mkarr(js);
  if (is_err(meta)) return meta;

  PCRE2_SPTR tabptr = nametable;
  for (uint32_t i = 0; i < namecount; i++) {
    int n = (tabptr[0] << 8) | tabptr[1];
    const char *name = (const char *)(tabptr + 2);
    ant_value_t name_val = js_mkstr(js, name, strlen(name));
    if (is_err(name_val)) return name_val;
    js_arr_push(js, meta, name_val);
    js_arr_push(js, meta, tov((double)n));
    tabptr += nameentrysize;
  }

  return meta;
}

static inline ant_value_t regexp_static_value(ant_t *js, size_t idx) {
  if (idx >= sizeof(js->mutable_roots.regexp_static_values) / sizeof(js->mutable_roots.regexp_static_values[0]))
    return js->builtins.regexp_empty_string ? js->builtins.regexp_empty_string : js_mkundef();
  return js->mutable_roots.regexp_static_values[idx] ? js->mutable_roots.regexp_static_values[idx] : (js->builtins.regexp_empty_string ? js->builtins.regexp_empty_string : js_mkundef());
}

static inline ant_value_t regexp_static_set(ant_t *js, size_t idx, ant_value_t value) {
  if (idx < sizeof(js->mutable_roots.regexp_static_values) / sizeof(js->mutable_roots.regexp_static_values[0]))
    js->mutable_roots.regexp_static_values[idx] = value;
  return js_mkundef();
}

#define REGEXP_STATIC_ACCESSORS(name, idx) \
  static ant_value_t regexp_static_get_##name(ant_t *js, ant_value_t *args, int nargs) { \
    (void)args; (void)nargs; \
    return regexp_static_value(js, idx); \
  } \
  static ant_value_t regexp_static_set_##name(ant_t *js, ant_value_t *args, int nargs) { \
    return regexp_static_set(js, idx, nargs > 0 ? args[0] : js_mkundef()); \
  }

REGEXP_STATIC_ACCESSORS(d1, 0)
REGEXP_STATIC_ACCESSORS(d2, 1)
REGEXP_STATIC_ACCESSORS(d3, 2)
REGEXP_STATIC_ACCESSORS(d4, 3)
REGEXP_STATIC_ACCESSORS(d5, 4)
REGEXP_STATIC_ACCESSORS(d6, 5)
REGEXP_STATIC_ACCESSORS(d7, 6)
REGEXP_STATIC_ACCESSORS(d8, 7)
REGEXP_STATIC_ACCESSORS(d9, 8)
REGEXP_STATIC_ACCESSORS(last_match, 9)
REGEXP_STATIC_ACCESSORS(amp, 10)

#undef REGEXP_STATIC_ACCESSORS

static void update_regexp_statics(ant_t *js, const char *str_ptr, PCRE2_SIZE *ovector, uint32_t ovcount) {
  ant_value_t empty = js->builtins.regexp_empty_string ? js->builtins.regexp_empty_string : js_mkstr(js, "", 0);
  for (int i = 1; i <= 9; i++) {
    ant_value_t val = empty;
    if ((uint32_t)i < ovcount && ovector[2*i] != PCRE2_UNSET)
      val = js_mkstr(js, str_ptr + ovector[2*i], ovector[2*i+1] - ovector[2*i]);
    js->mutable_roots.regexp_static_values[i - 1] = val;
  }

  ant_value_t match0 = (ovcount > 0 && ovector[0] != PCRE2_UNSET)
    ? js_mkstr(js, str_ptr + ovector[0], ovector[1] - ovector[0])
    : empty;
  js->mutable_roots.regexp_static_values[9] = match0;
  js->mutable_roots.regexp_static_values[10] = match0;
}

static inline bool is_pcre2_passthrough_escape(char c) {
switch (c) {
  case 'd': case 'D': case 'w': case 'W': case 's': case 'S':
  case 'b': case 'B': case 'n': case 'r': case 't': case 'f':
  case '1': case '2': case '3': case '4': case '5':
  case '6': case '7': case '8': case '9':
  case '.': case '*': case '+': case '?':
  case '(': case ')': case '[': case ']':
  case '{': case '}': case '|': case '^':
  case '$': case '\\': case '/': case '-': return true;
  default: return false;
}}

static inline bool is_class_shorthand(char c) {
  return c == 'w' || c == 'W' || c == 'd' || c == 'D' || c == 's' || c == 'S';
}

static size_t v_close_bracket(const char *src, size_t src_len, size_t open) {
  int depth = 0;
  for (size_t i = open; i < src_len; i++) {
    if (src[i] == '\\' && i + 1 < src_len) { i++; continue; }
    if (src[i] == '[') depth++;
    else if (src[i] == ']') { if (--depth == 0) return i; }
  }
  return src_len;
}

static size_t v_translate_part(const char *p, size_t len, char *out, size_t out_size) {
  if (len && p[0] == '[') return js_to_pcre2_pattern(p, len, out, out_size, false);
  char tmp[1024];
  if (len >= sizeof(tmp) - 2) return 0;
  tmp[0] = '['; memcpy(tmp + 1, p, len); tmp[len + 1] = ']';
  return js_to_pcre2_pattern(tmp, len + 2, out, out_size, false);
}

static int v_set_op(const char *src, size_t start, size_t end, size_t *op_pos) {
  int depth = 0;
  for (size_t i = start; i < end; ) {
    if (src[i] == '\\' && i + 1 < end) {
      char n = src[i + 1];
      if ((n == 'p' || n == 'P') && i + 2 < end && src[i + 2] == '{') {
        i += 3; while (i < end && src[i] != '}') i++; if (i < end) i++; continue;
      }
      if ((n == 'u' || n == 'x') && i + 2 < end && src[i + 2] == '{') {
        i += 3; while (i < end && src[i] != '}') i++; if (i < end) i++; continue;
      }
      i += 2; continue;
    }
    if (src[i] == '[') { depth++; i++; continue; }
    if (src[i] == ']') { if (depth > 0) { depth--; i++; continue; } break; }
    if (!depth && i + 1 < end) {
      if (src[i] == '&' && src[i+1] == '&') { *op_pos = i; return 1; }
      if (src[i] == '-' && src[i+1] == '-') { *op_pos = i; return 2; }
    }
    i++;
  }
  return 0;
}

size_t js_to_pcre2_pattern(const char *src, size_t src_len, char *dst, size_t dst_size, bool v_flag) {
  size_t di = 0;
  int charclass_depth = 0;

#define OUT(ch) do { if (di < dst_size - 1) dst[di++] = (ch); } while(0)

  for (size_t si = 0; si < src_len && di < dst_size - 1; si++) {
    if (src[si] == '[') {
      if (si + 2 < src_len && src[si + 1] == '^' && src[si + 2] == ']') {
        OUT('['); OUT('\\'); OUT('s'); OUT('\\'); OUT('S'); OUT(']');
        si += 2;
        continue;
      }

      if (v_flag && charclass_depth == 0) {
        size_t close = v_close_bracket(src, src_len, si);
        size_t op_pos;
        int op_type = v_set_op(src, si + 1, close, &op_pos);
        if (op_type && close < src_len) {
          char ao[1024], bo[1024];
          size_t aol = v_translate_part(&src[si + 1], op_pos - si - 1, ao, sizeof(ao));
          size_t bol = v_translate_part(&src[op_pos + 2], close - op_pos - 2, bo, sizeof(bo));
          const char *la = op_type == 1 ? ao : bo, *ra = op_type == 1 ? bo : ao;
          size_t ll = op_type == 1 ? aol : bol, rl = op_type == 1 ? bol : aol;
          OUT('('); OUT('?'); OUT(op_type == 1 ? '=' : '!');
          for (size_t k = 0; k < ll; k++) OUT(la[k]);
          OUT(')');
          for (size_t k = 0; k < rl; k++) OUT(ra[k]);
          si = close;
          continue;
        }
      }
      charclass_depth++;
      OUT('[');
      continue;
    }
    if (src[si] == ']' && charclass_depth > 0) {
      charclass_depth--;
      OUT(']');
      continue;
    }

    if (charclass_depth > 0 && src[si] == '-' && si > 0 && src[si - 1] != '[' &&
        si + 1 < src_len && src[si + 1] != ']') {
      bool prev_is_shorthand = (si >= 2 && src[si - 2] == '\\' && is_class_shorthand(src[si - 1]));
      bool next_is_shorthand = (si + 2 < src_len && src[si + 1] == '\\' && is_class_shorthand(src[si + 2]));
      if (prev_is_shorthand || next_is_shorthand) {
        OUT('\\'); OUT('-');
        continue;
      }
      OUT('-');
      continue;
    }

    if (src[si] != '\\' || si + 1 >= src_len) {
      OUT(src[si]);
      continue;
    }

    char next = src[si + 1];

    if (next == 'v') {
      OUT('\\'); OUT('x'); OUT('{'); OUT('0'); OUT('b'); OUT('}');
      si++;
      continue;
    }

    if (next == 'u' && si + 2 < src_len && src[si + 2] == '{') {
      size_t brace_start = si + 3;
      size_t brace_end = brace_start;
      while (brace_end < src_len && src[brace_end] != '}' && is_xdigit(src[brace_end])) brace_end++;
      if (brace_end < src_len && src[brace_end] == '}' && brace_end > brace_start) {
        OUT('\\'); OUT('x'); OUT('{');
        for (size_t k = brace_start; k < brace_end; k++) OUT(src[k]);
        OUT('}');
        si = brace_end;
        continue;
      }
    }

    if (next == 'u' && si + 5 < src_len &&
        is_xdigit(src[si+2]) && is_xdigit(src[si+3]) &&
        is_xdigit(src[si+4]) && is_xdigit(src[si+5])) {
      OUT('\\'); OUT('x'); OUT('{');
      OUT(src[si+2]); OUT(src[si+3]); OUT(src[si+4]); OUT(src[si+5]);
      OUT('}');
      si += 5;
      continue;
    }

    if (next == 'u') {
      si++;
      OUT('u');
      continue;
    }

    if (next == 'x' && si + 3 < src_len &&
        is_xdigit(src[si+2]) && is_xdigit(src[si+3])) {
      OUT('\\'); OUT('x'); OUT(src[si+2]); OUT(src[si+3]);
      si += 3;
      continue;
    }

    if (next == 'x') {
      si++;
      OUT('x');
      continue;
    }

    if (next == '0' && (si + 2 >= src_len || src[si+2] < '0' || src[si+2] > '9')) {
      OUT('\\'); OUT('x'); OUT('{'); OUT('0'); OUT('}');
      si++;
      continue;
    }

    if (next >= '0' && next <= '7') {
      unsigned int octal = next - '0';
      size_t advance = 1;
      if (si + 2 < src_len && src[si+2] >= '0' && src[si+2] <= '7') {
        octal = octal * 8 + (src[si+2] - '0');
        advance = 2;
        if (si + 3 < src_len && src[si+3] >= '0' && src[si+3] <= '7' && octal * 8 + (src[si+3] - '0') <= 255) {
          octal = octal * 8 + (src[si+3] - '0');
          advance = 3;
        }
      }
      
      if (advance > 1 || next == '0') {
        char hex[8];
        int hlen = snprintf(hex, sizeof(hex), "\\x{%02x}", octal);
        for (int k = 0; k < hlen && di < dst_size - 1; k++) OUT(hex[k]);
        si += advance;
        continue;
      }
    }

    if (next == 'c' && si + 2 < src_len &&
        ((src[si+2] >= 'A' && src[si+2] <= 'Z') || (src[si+2] >= 'a' && src[si+2] <= 'z'))) {
      OUT('\\'); OUT('c'); OUT(src[si+2]);
      si += 2;
      continue;
    }

    if (next == 'c') {
      OUT('\\'); OUT('\\'); OUT('c');
      si++;
      continue;
    }

    if ((next == 'p' || next == 'P') && si + 2 < src_len && src[si + 2] == '{') {
      size_t brace_start = si + 3;
      size_t brace_end = brace_start;
      while (brace_end < src_len && src[brace_end] != '}') brace_end++;
      if (brace_end < src_len && src[brace_end] == '}') {
        const char *prop = &src[brace_start];
        size_t prop_len = brace_end - brace_start;
        static const struct { const char *name; const char *code; } gc_map[] = {
          {"Letter","L"},{"Cased_Letter","LC"},{"Uppercase_Letter","Lu"},
          {"Lowercase_Letter","Ll"},{"Titlecase_Letter","Lt"},
          {"Modifier_Letter","Lm"},{"Other_Letter","Lo"},
          {"Mark","M"},{"Nonspacing_Mark","Mn"},{"Spacing_Mark","Mc"},
          {"Enclosing_Mark","Me"},
          {"Number","N"},{"Decimal_Number","Nd"},{"Letter_Number","Nl"},
          {"Other_Number","No"},
          {"Punctuation","P"},{"Connector_Punctuation","Pc"},
          {"Dash_Punctuation","Pd"},{"Open_Punctuation","Ps"},
          {"Close_Punctuation","Pe"},{"Initial_Punctuation","Pi"},
          {"Final_Punctuation","Pf"},{"Other_Punctuation","Po"},
          {"Symbol","S"},{"Math_Symbol","Sm"},{"Currency_Symbol","Sc"},
          {"Modifier_Symbol","Sk"},{"Other_Symbol","So"},
          {"Separator","Z"},{"Space_Separator","Zs"},
          {"Line_Separator","Zl"},{"Paragraph_Separator","Zp"},
          {"Other","C"},{"Control","Cc"},{"Format","Cf"},
          {"Surrogate","Cs"},{"Private_Use","Co"},{"Unassigned","Cn"},
        };
        static const struct { const char *script; const char *range; } u17_scripts[] = {
          {"Sidetic",       "\\x{10940}-\\x{1095F}"},
          {"Garay",         "\\x{10D40}-\\x{10D8F}"},
          {"Gurung_Khema",  "\\x{16100}-\\x{1613F}"},
          {"Kirat_Rai",     "\\x{16D40}-\\x{16D7F}"},
          {"Ol_Onal",       "\\x{1E5D0}-\\x{1E5FF}"},
          {"Sunuwar",       "\\x{11BC0}-\\x{11BFF}"},
          {"Tulu_Tigalari", "\\x{11380}-\\x{113FF}"},
        };
        bool has_eq = (memchr(prop, '=', prop_len) != NULL);
        bool has_colon = (memchr(prop, ':', prop_len) != NULL);
        if (!has_eq && !has_colon && next == 'p' && charclass_depth == 0) {
          static const struct { const char *name; const char *exp; } sprops[] = {
            {"Emoji_Keycap_Sequence",
             "(?:\\x{23}\\x{fe0f}\\x{20e3}|\\x{2a}\\x{fe0f}\\x{20e3}|[\\x{30}-\\x{39}]\\x{fe0f}\\x{20e3})"},
            {"RGI_Emoji",
             "(?:[\\x{1f1e6}-\\x{1f1ff}]{2}|(?:\\p{Emoji}[\\x{1f3fb}-\\x{1f3ff}]?\\x{200d})+\\p{Emoji}[\\x{1f3fb}-\\x{1f3ff}]?|\\p{Emoji}[\\x{1f3fb}-\\x{1f3ff}]|\\p{Emoji}\\x{fe0f}?)"},
          };
          for (size_t m = 0; m < sizeof(sprops)/sizeof(sprops[0]); m++) {
            if (strlen(sprops[m].name) == prop_len && memcmp(sprops[m].name, prop, prop_len) == 0) {
              for (const char *r = sprops[m].exp; *r && di < dst_size - 1; r++) OUT(*r);
              si = brace_end;
              goto next_char;
            }
          }
        }
        if (has_eq || has_colon) {
          char sep = has_eq ? '=' : ':';
          const char *val = memchr(prop, sep, prop_len);
          if (val) {
            val++;
            size_t val_len = prop_len - (size_t)(val - prop);
            for (size_t m = 0; m < sizeof(u17_scripts)/sizeof(u17_scripts[0]); m++) {
              if (strlen(u17_scripts[m].script) == val_len &&
                  memcmp(u17_scripts[m].script, val, val_len) == 0) {
                const char *r = u17_scripts[m].range;
                OUT('[');
                if (next == 'P') OUT('^');
                for (; *r; r++) OUT(*r);
                OUT(']');
                si = brace_end;
                goto next_char;
              }
            }
          }
        }
        if (!has_eq && !has_colon) {
          static const struct { const char *name; const char *range; } rangeprops[] = {
            {"ASCII", "\\x{0}-\\x{7f}"},
            {"Any",   "\\x{0}-\\x{10ffff}"},
          };
          for (size_t m = 0; m < sizeof(rangeprops)/sizeof(rangeprops[0]); m++) {
            if (strlen(rangeprops[m].name) == prop_len && memcmp(rangeprops[m].name, prop, prop_len) == 0) {
              if (charclass_depth > 0) {
                for (const char *r = rangeprops[m].range; *r; r++) OUT(*r);
              } else {
                OUT('['); if (next == 'P') OUT('^');
                for (const char *r = rangeprops[m].range; *r; r++) OUT(*r);
                OUT(']');
              }
              si = brace_end;
              goto next_char;
            }
          }
        }
        const char *replacement = NULL;
        if (!has_eq && !has_colon) {
          for (size_t m = 0; m < sizeof(gc_map)/sizeof(gc_map[0]); m++) {
            if (strlen(gc_map[m].name) == prop_len &&
                memcmp(gc_map[m].name, prop, prop_len) == 0) {
              replacement = gc_map[m].code;
              break;
            }
          }
        }
        static const struct { const char *prop; const char *extra; } u17_props[] = {
          {"Emoji", "\\x{1FACD}-\\x{1FACE}\\x{1FAE9}\\x{1FAF9}"},
        };
        const char *extra_range = NULL;
        if (!has_eq && !has_colon && !replacement) {
          for (size_t m = 0; m < sizeof(u17_props)/sizeof(u17_props[0]); m++) {
            if (strlen(u17_props[m].prop) == prop_len &&
                memcmp(u17_props[m].prop, prop, prop_len) == 0) {
              extra_range = u17_props[m].extra;
              break;
            }
          }
        }
        if (extra_range && charclass_depth == 0) {
          const char *pfx = (next == 'p') ? "(?:\\p{" : "(?:\\P{";
          for (const char *r = pfx; *r; r++) OUT(*r);
          for (size_t k = brace_start; k < brace_end; k++) OUT(src[k]);
          OUT('}'); OUT('|'); OUT('[');
          if (next == 'P') OUT('^');
          for (const char *r = extra_range; *r; r++) OUT(*r);
          OUT(']'); OUT(')');
        } else {
          OUT('\\'); OUT(next); OUT('{');
          if (replacement) {
            for (const char *r = replacement; *r; r++) OUT(*r);
          } else {
            for (size_t k = brace_start; k < brace_end; k++) OUT(src[k]);
          }
          OUT('}');
        }
        si = brace_end;
        continue;
      }
      OUT('\\'); OUT(next);
      si++;
      continue;
    }

    if (is_pcre2_passthrough_escape(next)) {
      OUT('\\'); OUT(next);
      si++;
      continue;
    }

    si++;
    OUT(next);
    next_char:;
  }

#undef OUT
  dst[di] = '\0';
  return di;
}

#define REGEXP_SET_PROP(js, obj, key, klen, val, is_new) \
  ((is_new) ? js_mkprop_fast(js, obj, key, klen, val) \
            : js_setprop(js, obj, js_mkstr(js, key, klen), val))

static void regexp_init_flags(ant_t *js, ant_value_t obj, const char *fstr, ant_offset_t flen, bool is_new) {
  uint8_t mask = regexp_parse_flags_mask(fstr, flen);
  bool d = (mask & REGEXP_FLAG_HAS_INDICES) != 0;
  bool g = (mask & REGEXP_FLAG_GLOBAL) != 0;
  bool i = (mask & REGEXP_FLAG_IGNORE_CASE) != 0;
  bool m = (mask & REGEXP_FLAG_MULTILINE) != 0;
  bool s = (mask & REGEXP_FLAG_DOTALL) != 0;
  bool u = (mask & REGEXP_FLAG_UNICODE) != 0;
  bool v = (mask & REGEXP_FLAG_UNICODE_SET) != 0;
  bool y = (mask & REGEXP_FLAG_STICKY) != 0;

  char sorted[10]; int si = 0;
  if (d) sorted[si++] = 'd';
  if (g) sorted[si++] = 'g';
  if (i) sorted[si++] = 'i';
  if (m) sorted[si++] = 'm';
  if (s) sorted[si++] = 's';
  if (u) sorted[si++] = 'u';
  if (v) sorted[si++] = 'v';
  if (y) sorted[si++] = 'y';

  ant_value_t flags_value = js_mkstr(js, sorted, si);
  REGEXP_SET_PROP(js, obj, "flags", 5, flags_value, is_new);
  REGEXP_SET_PROP(js, obj, "hasIndices", 10, mkval(T_BOOL, d ? 1 : 0), is_new);
  REGEXP_SET_PROP(js, obj, "global", 6, mkval(T_BOOL, g ? 1 : 0), is_new);
  REGEXP_SET_PROP(js, obj, "ignoreCase", 10, mkval(T_BOOL, i ? 1 : 0), is_new);
  REGEXP_SET_PROP(js, obj, "multiline", 9, mkval(T_BOOL, m ? 1 : 0), is_new);
  REGEXP_SET_PROP(js, obj, "dotAll", 6, mkval(T_BOOL, s ? 1 : 0), is_new);
  REGEXP_SET_PROP(js, obj, "unicode", 7, mkval(T_BOOL, u ? 1 : 0), is_new);
  REGEXP_SET_PROP(js, obj, "unicodeSets", 11, mkval(T_BOOL, v ? 1 : 0), is_new);
  REGEXP_SET_PROP(js, obj, "sticky", 6, mkval(T_BOOL, y ? 1 : 0), is_new);
  REGEXP_SET_PROP(js, obj, "lastIndex", 9, tov(0), is_new);
  js_set_slot(obj, SLOT_REGEXP_FLAGS_MASK, tov((double)mask));
  js_set_slot(obj, SLOT_REGEXP_FLAGS_STRING, flags_value);
  js_set_slot(obj, SLOT_REGEXP_NAMED_GROUPS, js_mkundef());
}

ant_value_t is_regexp_like(ant_t *js, ant_value_t value) {
  if (!is_object_type(value)) return js_false;

  ant_value_t match_sym = get_match_sym();
  if (vtype(match_sym) == T_SYMBOL) {
    ant_value_t match_val = js_get_sym(js, value, match_sym);
    if (is_err(match_val)) return match_val;
    if (vtype(match_val) != T_UNDEF) return js_bool(js_truthy(js, match_val));
  }

  ant_value_t regexp_ctor = js_get(js, js_glob(js), "RegExp");
  if (is_err(regexp_ctor)) return regexp_ctor;

  ant_value_t regexp_proto = js_get(js, regexp_ctor, "prototype");
  if (is_err(regexp_proto)) return regexp_proto;
  if (!is_object_type(regexp_proto)) return js_false;

  return js_bool(proto_chain_contains(js, value, regexp_proto));
}

ant_value_t reject_regexp_arg(ant_t *js, ant_value_t value, const char *method_name) {
  ant_value_t is_re = is_regexp_like(js, value);
  if (is_err(is_re)) return is_re;
  if (js_truthy(js, is_re)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "First argument to %s must not be a RegExp", method_name);
  }
  return js_mkundef();
}

static ant_value_t regexp_species_construct(ant_t *js, ant_value_t rx, ant_value_t ctor, ant_value_t *ctor_args, int nargs) {
  ant_value_t seed = js_mkobj(js);
  if (is_err(seed)) return seed;

  ant_value_t proto = js_get(js, ctor, "prototype");
  if (is_err(proto)) return proto;
  if (is_object_type(proto)) js_set_proto_init(seed, proto);

  ant_value_t saved = js->new_target;
  js->new_target = ctor;
  ant_value_t result = sv_vm_call(js->vm, js, ctor, seed, ctor_args, nargs, NULL, true);
  js->new_target = saved;

  if (is_err(result)) return result;
  if (!is_object_type(result))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp species constructor returned non-object");

  return result;
}

static ant_value_t regexp_exec_abstract(ant_t *js, ant_value_t rx, ant_value_t str);
static ant_value_t builtin_regexp_exec(ant_t *js, ant_value_t *args, int nargs);

static pcre2_match_context *regex_get_match_context(ant_t *js) {
  ant_regex_state_t *state = js->regex_state;
  if (!state) return NULL;
  if (state->match_ctx) return state->match_ctx;

  state->match_ctx = pcre2_match_context_create(NULL);
  if (!state->match_ctx) return NULL;

  state->jit_stack = pcre2_jit_stack_create(32 * 1024, 512 * 1024, NULL);
  if (state->jit_stack) {
    pcre2_jit_stack_assign(state->match_ctx, NULL, state->jit_stack);
  }

  return state->match_ctx;
}

static uint64_t compiled_regex_key_hash(
  const char *pattern, size_t pattern_len, uint8_t flags_mask
) {
  return ant_hash_mix(
    hash_key(pattern, pattern_len) ^ ant_hash_secret[0],
    ((uint64_t)flags_mask << 32) ^ ant_hash_secret[4]
  );
}

static bool compiled_regex_key_matches(
  const compiled_regex_cache_entry_t *entry,
  uint64_t key_hash,
  const char *pattern,
  size_t pattern_len,
  uint8_t flags_mask
) {
  return entry && entry->key_hash == key_hash &&
    entry->flags_mask == flags_mask &&
    entry->pattern_len == pattern_len &&
    memcmp(entry->pattern, pattern, pattern_len) == 0;
}

static void compiled_regex_entry_free(compiled_regex_cache_entry_t *entry) {
  if (!entry) return;
  ant_shape_release(entry->lastindex_shape);
  pcre2_match_data_free(entry->scratch_match_data);
  pcre2_code_free(entry->code);
  free(entry->pattern);
  free(entry);
}

static void compiled_regex_entry_maybe_free(
  compiled_regex_cache_entry_t *entry
) {
  if (entry && entry->object_refs == 0 && entry->cache_refs == 0)
    compiled_regex_entry_free(entry);
}

static bool compiled_regex_table_resize(
  compiled_regex_cache_table_t *table, size_t need
) {
  if (!table || need > REGEX_COMPILED_CACHE_MAX) return false;
  size_t cap = 64;
  while (cap < need * 2) cap <<= 1;
  if (table->cap >= cap) return true;

  compiled_regex_cache_entry_t **slots = calloc(cap, sizeof(*slots));
  if (!slots) return false;

  for (size_t i = 0; i < table->cap; i++) {
    compiled_regex_cache_entry_t *entry = table->slots[i];
    if (!entry) continue;
    size_t slot = (size_t)entry->key_hash & (cap - 1);
    while (slots[slot]) slot = (slot + 1) & (cap - 1);
    slots[slot] = entry;
  }

  free(table->slots);
  table->slots = slots;
  table->cap = cap;
  return true;
}

static compiled_regex_cache_entry_t *compiled_regex_table_lookup(
  const compiled_regex_cache_table_t *table,
  uint64_t key_hash,
  const char *pattern,
  size_t pattern_len,
  uint8_t flags_mask
) {
  if (!table || !table->slots || table->cap == 0) return NULL;
  size_t mask = table->cap - 1;
  size_t slot = (size_t)key_hash & mask;
  for (size_t probe = 0; probe < table->cap; probe++) {
    compiled_regex_cache_entry_t *entry = table->slots[slot];
    if (!entry) return NULL;
    if (compiled_regex_key_matches(
      entry, key_hash, pattern, pattern_len, flags_mask
    )) return entry;
    slot = (slot + 1) & mask;
  }
  return NULL;
}

static bool compiled_regex_table_insert(
  compiled_regex_cache_table_t *table,
  compiled_regex_cache_entry_t *entry,
  bool *inserted
) {
  *inserted = false;
  if (!table || !entry || table->count >= REGEX_COMPILED_CACHE_MAX)
    return false;
  if (
    !table->slots ||
    (table->count + 1) * 4 >= table->cap * 3
  ) {
    if (!compiled_regex_table_resize(table, table->count + 1))
      return false;
  }

  size_t mask = table->cap - 1;
  size_t slot = (size_t)entry->key_hash & mask;
  for (size_t probe = 0; probe < table->cap; probe++) {
    compiled_regex_cache_entry_t *existing = table->slots[slot];
    if (!existing) {
      table->slots[slot] = entry;
      table->count++;
      *inserted = true;
      return true;
    }
    if (existing == entry) return true;
    slot = (slot + 1) & mask;
  }
  return false;
}

static void compiled_regex_table_clear(
  compiled_regex_cache_table_t *table
) {
  if (!table) return;
  for (size_t i = 0; i < table->cap; i++) {
    compiled_regex_cache_entry_t *entry = table->slots[i];
    if (!entry) continue;
    if (entry->cache_refs > 0) entry->cache_refs--;
    if (rxstat_enabled) rxstat_compiled_evictions++;
    compiled_regex_entry_maybe_free(entry);
  }
  free(table->slots);
  memset(table, 0, sizeof(*table));
}

static compiled_regex_cache_entry_t *compiled_regex_cache_lookup(
  ant_regex_state_t *state,
  const char *pattern,
  size_t pattern_len,
  uint8_t flags_mask,
  uint64_t key_hash
) {
  compiled_regex_cache_entry_t *entry = compiled_regex_table_lookup(
    &state->compiled[0], key_hash, pattern, pattern_len, flags_mask
  );
  if (entry) {
    if (rxstat_enabled) rxstat_compiled_gen0_hit++;
    return entry;
  }

  entry = compiled_regex_table_lookup(
    &state->compiled[1], key_hash, pattern, pattern_len, flags_mask
  );
  if (!entry) {
    if (rxstat_enabled) rxstat_compiled_miss++;
    return NULL;
  }

  if (rxstat_enabled) rxstat_compiled_gen1_hit++;
  bool inserted;
  if (compiled_regex_table_insert(&state->compiled[0], entry, &inserted) &&
      inserted) {
    entry->cache_refs++;
    if (rxstat_enabled) rxstat_compiled_promotions++;
  }
  return entry;
}

static compiled_regex_cache_entry_t *compiled_regex_cache_get_or_compile(
  ant_regex_state_t *state,
  const char *pattern,
  size_t pattern_len,
  uint8_t flags_mask
) {
  uint64_t key_hash = compiled_regex_key_hash(
    pattern, pattern_len, flags_mask
  );
  compiled_regex_cache_entry_t *cached = compiled_regex_cache_lookup(
    state, pattern, pattern_len, flags_mask, key_hash
  );
  if (cached) return cached;
  if (rxstat_enabled) rxstat_compiles++;

  char pcre2_pattern[4096];
  size_t pcre2_len = js_to_pcre2_pattern(
    pattern, pattern_len, pcre2_pattern, sizeof(pcre2_pattern),
    (flags_mask & REGEXP_FLAG_UNICODE_SET) != 0
  );

  uint32_t options = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_UNSET_BACKREF | PCRE2_DUPNAMES;
  if (flags_mask & REGEXP_FLAG_IGNORE_CASE) options |= PCRE2_CASELESS;
  if (flags_mask & REGEXP_FLAG_MULTILINE) options |= PCRE2_MULTILINE;
  if (flags_mask & REGEXP_FLAG_DOTALL) options |= PCRE2_DOTALL;

  int errcode;
  PCRE2_SIZE erroffset;
  pcre2_compile_context *compile_ctx = pcre2_compile_context_create(NULL);
  if (compile_ctx) pcre2_set_newline(compile_ctx, PCRE2_NEWLINE_ANYCRLF);
  pcre2_code *re = pcre2_compile((PCRE2_SPTR)pcre2_pattern, pcre2_len, options, &errcode, &erroffset, compile_ctx);
  if (compile_ctx) pcre2_compile_context_free(compile_ctx);
  if (re == NULL) return NULL;

  compiled_regex_cache_entry_t *entry = calloc(1, sizeof(compiled_regex_cache_entry_t));
  if (!entry) {
    pcre2_code_free(re);
    return NULL;
  }

  char *pattern_copy = malloc(pattern_len ? pattern_len : 1);
  if (!pattern_copy) {
    pcre2_code_free(re);
    free(entry);
    return NULL;
  }
  if (pattern_len) memcpy(pattern_copy, pattern, pattern_len);

  entry->pattern = pattern_copy;
  entry->pattern_len = pattern_len;
  entry->key_hash = key_hash;
  entry->flags_mask = flags_mask;
  entry->code = re;
  pcre2_pattern_info(re, PCRE2_INFO_NAMECOUNT, &entry->namecount);
  entry->jit_ready = pcre2_jit_compile(re, PCRE2_JIT_COMPLETE) == 0;

  bool inserted;
  if (compiled_regex_table_insert(&state->compiled[0], entry, &inserted) &&
      inserted)
    entry->cache_refs++;
  return entry;
}

static bool regex_match_scope_begin(
  compiled_regex_cache_entry_t *entry, regex_match_scope_t *scope
) {
  memset(scope, 0, sizeof(*scope));
  scope->compiled = entry;
  if (!entry) return false;

  if (!entry->scratch_in_use) {
    if (!entry->scratch_match_data)
      entry->scratch_match_data =
        pcre2_match_data_create_from_pattern(entry->code, NULL);
    if (!entry->scratch_match_data) return false;
    entry->scratch_in_use = true;
    scope->match_data = entry->scratch_match_data;
    scope->borrowed = true;
    return true;
  }

  scope->match_data = pcre2_match_data_create_from_pattern(entry->code, NULL);
  if (!scope->match_data) return false;
  if (rxstat_enabled) rxstat_match_dynamic++;
  return true;
}

static void regex_match_scope_end(regex_match_scope_t *scope) {
  if (!scope || !scope->match_data) return;
  if (scope->borrowed) {
    scope->compiled->scratch_in_use = false;
  } else {
    pcre2_match_data_free(scope->match_data);
  }
  scope->match_data = NULL;
}

static void regexp_object_compiled_release(
  ant_t *js, ant_value_t regexp, compiled_regex_cache_entry_t *compiled
) {
  if (!compiled) return;
  js_clear_native(regexp, REGEXP_NATIVE_TAG);
  ant_regex_state_t *state = js->regex_state;
  if (state && state->live_object_data > 0) state->live_object_data--;
  if (compiled && compiled->object_refs > 0) compiled->object_refs--;
  compiled_regex_entry_maybe_free(compiled);
}

static void regexp_object_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t regexp = js_obj_from_ptr(obj);
  compiled_regex_cache_entry_t *compiled =
    js_get_native(regexp, REGEXP_NATIVE_TAG);
  regexp_object_compiled_release(js, regexp, compiled);
}

static void regexp_object_compiled_detach(ant_t *js, ant_value_t regexp) {
  compiled_regex_cache_entry_t *compiled =
    js_get_native(regexp, REGEXP_NATIVE_TAG);
  regexp_object_compiled_release(js, regexp, compiled);
}

static bool regexp_source_pattern(ant_t *js, ant_value_t regexp_obj, const char **pattern_ptr, ant_offset_t *pattern_len) {
  ant_value_t source_val = js_get_slot(regexp_obj, SLOT_DATA);
  if (vtype(source_val) == T_STR) {
    ant_offset_t poff;
    poff = vstr(js, source_val, pattern_len);
    *pattern_ptr = (const char *)(uintptr_t)poff;
    return true;
  }

  ant_prop_loc_t source_off = lkp(js, regexp_obj, "source", 6);
  if (!source_off.obj) return false;
  
  source_val = js_prop_load(source_off);
  if (vtype(source_val) != T_STR) return false;

  ant_offset_t poff;
  poff = vstr(js, source_val, pattern_len);
  *pattern_ptr = (const char *)(uintptr_t)poff;
  
  return true;
}

static bool regexp_compile_shared_from_object(
  ant_t *js,
  ant_value_t regexp_obj,
  uint8_t flags_mask,
  compiled_regex_cache_entry_t **out
) {
  ant_offset_t plen;
  const char *pattern_ptr;
  if (!regexp_source_pattern(js, regexp_obj, &pattern_ptr, &plen)) return false;

  ant_regex_state_t *state = js->regex_state;
  if (!state) return false;
  compiled_regex_cache_entry_t *compiled = compiled_regex_cache_get_or_compile(
    state, pattern_ptr, plen, flags_mask
  );
  if (!compiled) return false;
  *out = compiled;
  return true;
}

static compiled_regex_cache_entry_t *regex_get_or_compile(
  ant_t *js,
  ant_value_t regexp_obj,
  uint8_t flags_mask,
  compiled_regex_cache_entry_t *compiled
) {
  ant_regex_state_t *state = js->regex_state;
  if (!state) return NULL;
  ant_object_t *obj_ptr = js_obj_ptr(regexp_obj);

  if (!compiled) {
    compiled = js_get_native(regexp_obj, REGEXP_NATIVE_TAG);
  }
  if (compiled && compiled->flags_mask == flags_mask) {
    if (rxstat_enabled) rxstat_cache_hit++;
    return compiled;
  }
  if (compiled) regexp_object_compiled_release(js, regexp_obj, compiled);
  if (rxstat_enabled) rxstat_cache_miss++;

  if (!regexp_compile_shared_from_object(
    js, regexp_obj, flags_mask, &compiled
  )) return NULL;

  /* Metadata construction can allocate and run enough major collections to
     age this entry out of both cache generations. Pin the reference that will
     become the RegExp object's ownership before reading compiled PCRE2 data. */
  compiled->object_refs++;
  ant_value_t groups_meta = regexp_build_named_groups_meta(js, compiled->code);
  if (is_err(groups_meta)) {
    compiled->object_refs--;
    compiled_regex_entry_maybe_free(compiled);
    return NULL;
  }

  if (obj_ptr->finalizer && obj_ptr->finalizer != regexp_object_finalize) {
    compiled->object_refs--;
    compiled_regex_entry_maybe_free(compiled);
    return NULL;
  }

  js_set_native(regexp_obj, compiled, REGEXP_NATIVE_TAG);
  if (js_get_native(regexp_obj, REGEXP_NATIVE_TAG) != compiled) {
    compiled->object_refs--;
    compiled_regex_entry_maybe_free(compiled);
    return NULL;
  }
  js_set_finalizer(regexp_obj, regexp_object_finalize);
  state->live_object_data++;
  if (rxstat_enabled) {
    rxstat_inserts++;
    if (state->live_object_data > rxstat_max_cache)
      rxstat_max_cache = state->live_object_data;
  }
  js_set_slot(regexp_obj, SLOT_REGEXP_NAMED_GROUPS, groups_meta);
  return compiled;
}

static bool regexp_has_internal_slots(ant_t *js, ant_value_t value) {
  if (!is_object_type(value)) return false;
  return vtype(js_get_slot(value, SLOT_REGEXP_FLAGS_STRING)) == T_STR;
}

static bool regexp_can_use_internal_fast_path(ant_t *js, ant_value_t value) {
  return is_object_type(value) && !is_proxy(value) && regexp_has_internal_slots(js, value);
}

static ant_value_t builtin_RegExp(ant_t *js, ant_value_t *args, int nargs) {
  bool pattern_is_regexp = false;
  if (nargs > 0) {
    ant_value_t is_re = is_regexp_like(js, args[0]);
    if (is_err(is_re)) return is_re;
    pattern_is_regexp = js_truthy(js, is_re);
  }

  if (vtype(js->new_target) == T_UNDEF && nargs > 0 && pattern_is_regexp) {
    if (nargs < 2 || vtype(args[1]) == T_UNDEF) {
      ant_value_t ctor = js_getprop_fallback(js, args[0], "constructor");
      if (is_err(ctor)) return ctor;
      ant_value_t regexp_ctor = js_get(js, js_glob(js), "RegExp");
      if (is_err(regexp_ctor)) return regexp_ctor;
      if (same_ctor_identity(js, ctor, regexp_ctor)) return args[0];
    }
  }

  ant_value_t regexp_obj = js->this_val;
  bool use_this = (vtype(js->new_target) != T_UNDEF && vtype(regexp_obj) == T_OBJ);

  if (!use_this) {
    regexp_obj = mkobj(js, 0);
    if (is_err(regexp_obj)) return regexp_obj;
  }

  ant_value_t regexp_proto = js_get_ctor_proto(js, "RegExp", 6);
  ant_value_t instance_proto = js_instance_proto_from_new_target(js, regexp_proto);

  if (is_object_type(instance_proto)) js_set_proto_init(regexp_obj, instance_proto);
  if (vtype(js->new_target) == T_FUNC || vtype(js->new_target) == T_CFUNC) {
    js_set_slot(regexp_obj, SLOT_CTOR, js->new_target);
  }

  ant_value_t pattern = js_mkstr(js, "", 0);
  ant_value_t flags = js_mkstr(js, "", 0);
  if (nargs > 0) {
    if (pattern_is_regexp) {
      ant_value_t src = js_getprop_fallback(js, args[0], "source");
      if (is_err(src)) return src;
      pattern = js_tostring_val(js, src);
      if (is_err(pattern)) return pattern;
      if (nargs >= 2 && vtype(args[1]) != T_UNDEF) {
        flags = js_tostring_val(js, args[1]);
      } else {
        ant_value_t fl = js_getprop_fallback(js, args[0], "flags");
        if (is_err(fl)) return fl;
        flags = js_tostring_val(js, fl);
      }
      if (is_err(flags)) return flags;
    } else if (vtype(args[0]) == T_STR) {
      pattern = args[0];
      if (nargs > 1 && vtype(args[1]) == T_STR) flags = args[1];
    } else if (vtype(args[0]) != T_UNDEF) {
      ant_value_t s = js_tostring_val(js, args[0]);
      if (is_err(s)) return s;
      pattern = s;
      if (nargs > 1 && vtype(args[1]) == T_STR) flags = args[1];
    }
  }

  js_mkprop_fast(js, regexp_obj, "source", 6, pattern);
  js_set_slot(regexp_obj, SLOT_DATA, pattern);
  ant_offset_t flags_len, flags_off = vstr(js, flags, &flags_len);
  regexp_init_flags(js, regexp_obj, (const char *)(uintptr_t)(flags_off), flags_len, true);

  return regexp_obj;
}

static ant_value_t builtin_regexp_groups_getter(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t result_arr = js->this_val;
  if (!is_object_type(result_arr)) return js_mkundef();

  ant_value_t cached = js_get_slot(result_arr, SLOT_REGEXP_GROUPS_CACHE);
  if (is_object_type(cached)) return cached;

  ant_value_t meta = js_get_slot(result_arr, SLOT_REGEXP_RESULT_GROUPS);
  if (!is_object_type(meta)) return js_mkundef();

  ant_value_t groups = js_mkobj(js);
  if (is_err(groups)) return groups;
  js_set_proto_init(groups, js_mknull());

  for (ant_offset_t i = 0; ; i += 2) {
    ant_value_t name = js_arr_get(js, meta, i);
    if (vtype(name) == T_UNDEF) break;
    ant_value_t index_val = js_arr_get(js, meta, i + 1);
    ant_offset_t index = (vtype(index_val) == T_NUM) ? (ant_offset_t)tod(index_val) : 0;
    char idxstr[16];
    (void)uint_to_str(idxstr, sizeof(idxstr), (uint64_t)index);
    ant_value_t value = js_getprop_fallback(js, result_arr, idxstr);
    ant_offset_t name_len, name_off = vstr(js, name, &name_len);
    ant_value_t status = setprop_cstr(js, groups, (const char *)(uintptr_t)name_off, (size_t)name_len, value);
    if (is_err(status)) return status;
  }

  js_set_slot(result_arr, SLOT_REGEXP_GROUPS_CACHE, groups);
  return groups;
}

static ant_value_t regexp_build_indices_pair(ant_t *js, PCRE2_SIZE start, PCRE2_SIZE end) {
  if (start == PCRE2_UNSET) return js_mkundef();

  ant_value_t pair = js_mkarr(js);
  if (is_err(pair)) return pair;
  js_arr_push(js, pair, tov((double)start));
  js_arr_push(js, pair, tov((double)end));
  
  return pair;
}

static ant_value_t regexp_build_indices_groups(
  ant_t *js,
  ant_value_t groups_meta,
  ant_value_t indices_arr
) {
  ant_value_t groups = js_mkobj(js);
  if (is_err(groups)) return groups;
  js_set_proto_init(groups, js_mknull());

  for (ant_offset_t i = 0; ; i += 2) {
    ant_value_t name = js_arr_get(js, groups_meta, i);
    if (vtype(name) == T_UNDEF) break;
    
    ant_value_t index_val = js_arr_get(js, groups_meta, i + 1);
    ant_offset_t index = (vtype(index_val) == T_NUM) ? (ant_offset_t)tod(index_val) : 0;
    char idxstr[16];
    (void)uint_to_str(idxstr, sizeof(idxstr), (uint64_t)index);
    
    ant_value_t value = js_getprop_fallback(js, indices_arr, idxstr);
    ant_offset_t name_len, name_off = vstr(js, name, &name_len);
    ant_value_t status = setprop_cstr(js, groups, (const char *)(uintptr_t)name_off, (size_t)name_len, value);
    if (is_err(status)) return status;
  }

  return groups;
}

static ant_value_t regexp_build_indices_result(
  ant_t *js,
  ant_value_t regexp,
  PCRE2_SIZE *ovector,
  uint32_t ovcount
) {
  ant_value_t indices_arr = js_mkarr(js);
  if (is_err(indices_arr)) return indices_arr;

  for (uint32_t i = 0; i < ovcount; i++) {
    ant_value_t pair = regexp_build_indices_pair(js, ovector[2*i], ovector[2*i+1]);
    if (is_err(pair)) return pair;
    js_arr_push(js, indices_arr, pair);
  }

  ant_value_t groups_meta = js_get_slot(regexp, SLOT_REGEXP_NAMED_GROUPS);
  if (is_object_type(groups_meta)) {
    ant_value_t groups = regexp_build_indices_groups(js, groups_meta, indices_arr);
    if (is_err(groups)) return groups;
    if (is_err(setprop_cstr(js, indices_arr, "groups", 6, groups))) return js_mkerr(js, "oom");
  } else if (is_err(setprop_cstr(js, indices_arr, "groups", 6, js_mkundef()))) return js_mkerr(js, "oom");

  return indices_arr;
}

static const char *find_bytes(const char *haystack, ant_offset_t haystack_len, const char *needle, ant_offset_t needle_len);
static bool regexp_plain_literal_pattern(
  ant_t *js,
  ant_value_t rx,
  uint8_t flags_mask,
  const char **pattern_ptr,
  ant_offset_t *pattern_len
);

static ant_value_t regexp_exec_plain_literal_fast(
  ant_t *js,
  ant_value_t regexp,
  ant_value_t str_arg,
  uint8_t flags_mask,
  bool truthy_only,
  bool *used_fast_path
) {
  *used_fast_path = false;

  const char *needle;
  ant_offset_t needle_len;
  if (!regexp_plain_literal_pattern(js, regexp, flags_mask, &needle, &needle_len)) return js_mkundef();

  ant_offset_t str_len, str_off = vstr(js, str_arg, &str_len);
  const char *str_ptr = (const char *)(uintptr_t)str_off;
  const char *match = find_bytes(str_ptr, str_len, needle, needle_len);

  *used_fast_path = true;
  if (!match) return js_mknull();

  PCRE2_SIZE ovector[2];
  ovector[0] = (PCRE2_SIZE)(match - str_ptr);
  ovector[1] = ovector[0] + (PCRE2_SIZE)needle_len;
  update_regexp_statics(js, str_ptr, ovector, 1);

  if (truthy_only) return js_true;

  rxstat_note_result(1, false, false);
  ant_value_t result_arr = js_mkarr(js);
  if (is_err(result_arr)) return result_arr;

  ant_value_t match_str = js_mkstr(js, match, needle_len);
  if (is_err(match_str)) return match_str;
  js_arr_push(js, result_arr, match_str);

  if (!regexp_result_apply_shape(
    js, result_arr, tov((double)ovector[0]), str_arg,
    js_mkundef(), js_mkundef(), false
  )) {
    if (is_err(js_mkprop_fast(js, result_arr, "index", 5, tov((double)ovector[0])))) return js_mkerr(js, "oom");
    if (is_err(js_mkprop_fast(js, result_arr, "input", 5, str_arg))) return js_mkerr(js, "oom");
    if (is_err(js_mkprop_fast(js, result_arr, "groups", 6, js_mkundef()))) return js_mkerr(js, "oom");
  }
  return result_arr;
}

static __attribute__((always_inline)) inline int compiled_regex_run(
  ant_t *js,
  compiled_regex_cache_entry_t *compiled,
  const char *str_ptr,
  ant_offset_t str_len,
  PCRE2_SIZE start_offset,
  uint32_t match_options,
  bool sticky,
  regex_match_scope_t *scope,
  PCRE2_SIZE **ovector,
  uint32_t *ovcount
) {
  *ovector = NULL;
  *ovcount = 0;
  if (!regex_match_scope_begin(compiled, scope))
    return PCRE2_ERROR_NOMEMORY;

  pcre2_match_context *match_ctx = regex_get_match_context(js);
  int rc;
  if (compiled->jit_ready && !sticky) {
    if (rxstat_enabled) rxstat_jit_match++;
    rc = pcre2_jit_match(
      compiled->code, (PCRE2_SPTR)str_ptr, str_len, start_offset,
      match_options, scope->match_data, match_ctx
    );
  } else {
    if (rxstat_enabled) rxstat_interp_match++;
    rc = pcre2_match(
      compiled->code, (PCRE2_SPTR)str_ptr, str_len, start_offset,
      match_options, scope->match_data, match_ctx
    );
  }

  if (rc >= 0) {
    *ovector = pcre2_get_ovector_pointer(scope->match_data);
    *ovcount = pcre2_get_ovector_count(scope->match_data);
  }
  return rc;
}

static bool regexp_lastindex_fast_location(
  compiled_regex_cache_entry_t *compiled,
  ant_value_t regexp,
  ant_object_t **out_obj,
  uint32_t *out_slot
) {
  if (!compiled || !compiled->lastindex_shape || is_proxy(regexp)) return false;
  ant_object_t *obj = js_obj_ptr(regexp);
  if (
    !obj || obj->flags.is_exotic ||
    obj->shape != compiled->lastindex_shape ||
    compiled->lastindex_slot >= obj->prop_count
  ) return false;
  *out_obj = obj;
  *out_slot = compiled->lastindex_slot;
  return true;
}

static void regexp_lastindex_cache_location(
  compiled_regex_cache_entry_t *compiled,
  ant_value_t regexp
) {
  if (!compiled || is_proxy(regexp)) return;
  ant_object_t *obj = js_obj_ptr(regexp);
  if (!obj || obj->flags.is_exotic || !obj->shape) return;
  if (
    compiled->lastindex_shape == obj->shape &&
    compiled->lastindex_slot < obj->prop_count
  ) return;

  const char *key = intern_string("lastIndex", 9);
  if (!key) return;
  int32_t found = ant_shape_lookup_interned(obj->shape, key);
  if (found < 0 || (uint32_t)found >= obj->prop_count) return;

  const ant_shape_prop_t *prop = ant_shape_prop_at(
    obj->shape, (uint32_t)found
  );
  if (
    !prop || prop->has_getter || prop->has_setter ||
    !(prop->attrs & ANT_PROP_ATTR_WRITABLE)
  ) return;

  if (compiled->lastindex_shape != obj->shape) {
    ant_shape_retain(obj->shape);
    ant_shape_release(compiled->lastindex_shape);
    compiled->lastindex_shape = obj->shape;
  }
  compiled->lastindex_slot = (uint32_t)found;
}

static ant_value_t regexp_set_lastindex(
  ant_t *js,
  compiled_regex_cache_entry_t *compiled,
  ant_value_t regexp,
  ant_value_t value
) {
  ant_object_t *obj;
  uint32_t slot;
  if (regexp_lastindex_fast_location(
    compiled, regexp, &obj, &slot
  )) {
    ant_object_prop_set_unchecked(obj, slot, value);
    gc_write_barrier(js, obj, value);
    if (rxstat_enabled) rxstat_lastindex_fast_writes++;
    return value;
  }

  ant_object_t *receiver = js_obj_ptr(regexp);
  if (receiver && receiver->shape && !is_proxy(regexp)) {
    const char *key = intern_string("lastIndex", 9);
    int32_t found = key
      ? ant_shape_lookup_interned(receiver->shape, key)
      : -1;
    if (found >= 0) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(
        receiver->shape, (uint32_t)found
      );
      if (
        prop && !prop->has_getter && !prop->has_setter &&
        !(prop->attrs & ANT_PROP_ATTR_WRITABLE)
      ) {
        return js_mkerr_typed(
          js, JS_ERR_TYPE, "Cannot assign to read only property 'lastIndex'"
        );
      }
    }
  }
  return setprop_cstr(js, regexp, "lastIndex", 9, value);
}

static ant_value_t regexp_exec_shared_fast(
  ant_t *js,
  ant_value_t regexp,
  ant_value_t str_arg,
  uint8_t flags_mask,
  bool global_flag,
  bool sticky_flag,
  PCRE2_SIZE start_offset,
  bool truthy_only,
  bool *used_fast_path
) {
  *used_fast_path = false;
  if (global_flag || sticky_flag || (flags_mask & REGEXP_FLAG_HAS_INDICES))
    return js_mkundef();

  ant_value_t literal_result = regexp_exec_plain_literal_fast(
    js, regexp, str_arg, flags_mask, truthy_only, used_fast_path
  );
  if (is_err(literal_result) || *used_fast_path) return literal_result;

  compiled_regex_cache_entry_t *compiled =
    regex_get_or_compile(js, regexp, flags_mask, NULL);
  if (!compiled) return js_mkundef();

  if (!truthy_only && compiled->namecount != 0) return js_mkundef();

  ant_offset_t str_len, str_off = vstr(js, str_arg, &str_len);
  const char *str_ptr = (const char *)(uintptr_t)str_off;

  uint32_t match_options = sticky_flag ? PCRE2_ANCHORED : 0;
  regex_match_scope_t match_scope;
  PCRE2_SIZE *ovector;
  uint32_t ovcount;
  int rc = compiled_regex_run(
    js, compiled, str_ptr, str_len, start_offset, match_options,
    sticky_flag, &match_scope, &ovector, &ovcount
  );

  *used_fast_path = true;
  if (rc < 0) {
    regex_match_scope_end(&match_scope);
    if ((global_flag || sticky_flag) && is_err(setprop_cstr(js, regexp, "lastIndex", 9, tov(0)))) {
      return js_mkerr(js, "oom");
    }
    return js_mknull();
  }

  update_regexp_statics(js, str_ptr, ovector, ovcount);

  if (global_flag || sticky_flag) {
    ant_value_t next_idx = tov((double)ovector[1]);
    if (is_err(setprop_cstr(js, regexp, "lastIndex", 9, next_idx))) {
      regex_match_scope_end(&match_scope);
      return js_mkerr(js, "oom");
    }
  }

  if (truthy_only) {
    regex_match_scope_end(&match_scope);
    return js_true;
  }

  ant_value_t result;
  rxstat_note_result(ovcount, false, false);
  ant_value_t result_arr = js_mkarr(js);
  if (is_err(result_arr)) {
    result = result_arr;
    goto done;
  }
  for (uint32_t i = 0; i < ovcount; i++) {
    PCRE2_SIZE start = ovector[2*i];
    PCRE2_SIZE end = ovector[2*i+1];
    if (start == PCRE2_UNSET) {
      js_arr_push(js, result_arr, js_mkundef());
    } else {
      ant_value_t match_str = js_mkstr(js, str_ptr + start, end - start);
      if (is_err(match_str)) {
        result = match_str;
        goto done;
      }
      js_arr_push(js, result_arr, match_str);
    }
  }

  if (!regexp_result_apply_shape(
    js, result_arr, tov((double)ovector[0]), str_arg,
    js_mkundef(), js_mkundef(), false
  )) {
    if (is_err(js_mkprop_fast(js, result_arr, "index", 5, tov((double)ovector[0])))) {
      result = js_mkerr(js, "oom");
      goto done;
    }
    if (is_err(js_mkprop_fast(js, result_arr, "input", 5, str_arg))) {
      result = js_mkerr(js, "oom");
      goto done;
    }
    if (is_err(js_mkprop_fast(js, result_arr, "groups", 6, js_mkundef()))) {
      result = js_mkerr(js, "oom");
      goto done;
    }
  }
  result = result_arr;

done:
  regex_match_scope_end(&match_scope);
  return result;
}

static ant_value_t regexp_exec_internal(ant_t *js, ant_value_t regexp, ant_value_t str_arg, bool truthy_only) {
  if (rxstat_enabled) rxstat_exec_internal++;
  ant_offset_t str_len, str_off = vstr(js, str_arg, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  compiled_regex_cache_entry_t *compiled_hint;
  uint8_t flags_mask = regexp_flags_mask(
    js, regexp, &compiled_hint
  );
  
  bool global_flag = (flags_mask & REGEXP_FLAG_GLOBAL) != 0;
  bool has_indices = (flags_mask & REGEXP_FLAG_HAS_INDICES) != 0;
  bool sticky_flag = (flags_mask & REGEXP_FLAG_STICKY) != 0;

  // TODO: reduce nesting
  PCRE2_SIZE start_offset = 0;
  if (global_flag || sticky_flag) {
    if (rxstat_enabled) rxstat_lastindex_reads++;
    ant_object_t *lastindex_obj;
    uint32_t lastindex_slot;
    ant_value_t li_val;
    if (regexp_lastindex_fast_location(
      compiled_hint, regexp, &lastindex_obj, &lastindex_slot
    )) {
      li_val = ant_object_prop_get_unchecked(lastindex_obj, lastindex_slot);
      if (rxstat_enabled) rxstat_lastindex_fast_reads++;
    } else {
      ant_prop_loc_t lastindex_off = lkp(js, regexp, "lastIndex", 9);
      li_val = lastindex_off.obj
        ? js_prop_load(lastindex_off)
        : js_mkundef();
    }
    if (vtype(li_val) == T_NUM) {
      double li = tod(li_val);
      if (li >= 0 && li <= (double)str_len) start_offset = (PCRE2_SIZE)li;
      else {
        if (rxstat_enabled) {
          rxstat_lastindex_writes++;
          rxstat_lastindex_resets++;
        }
        ant_value_t stored = regexp_set_lastindex(
          js, compiled_hint, regexp, tov(0)
        );
        if (is_err(stored)) return stored;
        return js_mknull();
      }
    }
  }

  bool used_fast_path = false;
  ant_value_t fast_result = regexp_exec_shared_fast(
    js, regexp, str_arg, flags_mask, global_flag, sticky_flag, start_offset, truthy_only, &used_fast_path
  );
  if (is_err(fast_result) || used_fast_path) {
    if (rxstat_enabled && used_fast_path) rxstat_shared_fast++;
    return fast_result;
  }

  compiled_regex_cache_entry_t *compiled =
    regex_get_or_compile(js, regexp, flags_mask, compiled_hint);
  if (!compiled) return js_mknull();
  regexp_lastindex_cache_location(compiled, regexp);

  uint32_t match_options = 0;
  if (sticky_flag) match_options |= PCRE2_ANCHORED;

  regex_match_scope_t match_scope;
  PCRE2_SIZE *ovector;
  uint32_t ovcount;
  int rc = compiled_regex_run(
    js, compiled, str_ptr, str_len, start_offset, match_options,
    sticky_flag, &match_scope, &ovector, &ovcount
  );

  if (rc < 0) {
    regex_match_scope_end(&match_scope);
    if (rxstat_enabled && (global_flag || sticky_flag)) {
      rxstat_lastindex_writes++;
      rxstat_lastindex_resets++;
    }
    if (global_flag || sticky_flag) {
      ant_value_t stored = regexp_set_lastindex(
        js, compiled, regexp, tov(0)
      );
      if (is_err(stored)) return stored;
    }
    return js_mknull();
  }

  update_regexp_statics(js, str_ptr, ovector, ovcount);

  if (global_flag || sticky_flag) {
    ant_value_t next_idx = tov((double)ovector[1]);
    if (rxstat_enabled) rxstat_lastindex_writes++;
    ant_value_t stored = regexp_set_lastindex(
      js, compiled, regexp, next_idx
    );
    if (is_err(stored)) {
      regex_match_scope_end(&match_scope);
      return stored;
    }
  }

  if (truthy_only) {
    regex_match_scope_end(&match_scope);
    return js_true;
  }

  ant_value_t result;
  ant_value_t groups_meta = js_get_slot(regexp, SLOT_REGEXP_NAMED_GROUPS);
  rxstat_note_result(ovcount, is_object_type(groups_meta), has_indices);
  ant_value_t result_arr = js_mkarr(js);
  if (is_err(result_arr)) {
    result = result_arr;
    goto done;
  }
  for (uint32_t i = 0; i < ovcount; i++) {
    PCRE2_SIZE start = ovector[2*i];
    PCRE2_SIZE end = ovector[2*i+1];
    if (start == PCRE2_UNSET) {
      js_arr_push(js, result_arr, js_mkundef());
    } else {
      ant_value_t match_str = js_mkstr(js, str_ptr + start, end - start);
      if (is_err(match_str)) {
        result = match_str;
        goto done;
      }
      js_arr_push(js, result_arr, match_str);
    }
  }

  ant_value_t indices = js_mkundef();
  if (has_indices) {
    indices = regexp_build_indices_result(js, regexp, ovector, ovcount);
    if (is_err(indices)) {
      result = indices;
      goto done;
    }
  }

  bool shaped_result =
    !is_object_type(groups_meta) &&
    regexp_result_apply_shape(
      js, result_arr, tov((double)ovector[0]), str_arg,
      js_mkundef(), indices, has_indices
    );
  if (!shaped_result) {
    if (is_err(setprop_cstr(js, result_arr, "index", 5, tov((double)ovector[0])))) {
      result = js_mkerr(js, "oom");
      goto done;
    }
    if (is_err(setprop_cstr(js, result_arr, "input", 5, str_arg))) {
      result = js_mkerr(js, "oom");
      goto done;
    }
  }

  if (is_object_type(groups_meta)) {
    js_set_slot(result_arr, SLOT_REGEXP_RESULT_GROUPS, groups_meta);
    js_set_slot(result_arr, SLOT_REGEXP_GROUPS_CACHE, js_mkundef());
    js_set_getter_desc(js, js_as_obj(result_arr), "groups", 6, js_mkfun(builtin_regexp_groups_getter), JS_DESC_E | JS_DESC_C);
  } else if (!shaped_result &&
    is_err(setprop_cstr(js, result_arr, "groups", 6, js_mkundef()))) {
    result = js_mkerr(js, "oom");
    goto done;
  }

  if (has_indices && is_object_type(groups_meta)) {
    if (is_err(setprop_cstr(js, result_arr, "indices", 7, indices))) {
      result = js_mkerr(js, "oom");
      goto done;
    }
  }

  result = result_arr;

done:
  regex_match_scope_end(&match_scope);
  return result;
}

static ant_value_t builtin_regexp_exec(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t regexp = js->this_val;
  if (!regexp_has_internal_slots(js, regexp))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype.exec called on incompatible receiver");

  ant_value_t str_arg = nargs > 0 ? coerce_to_str(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str_arg)) return str_arg;

  return regexp_exec_internal(js, regexp, str_arg, false);
}

static ant_value_t builtin_regexp_toString(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t regexp = js->this_val;
  if (!is_object_type(regexp))
    return js_mkerr_typed(js, JS_ERR_TYPE, "toString called on non-object");

  ant_value_t source_val = js_getprop_fallback(js, regexp, "source");
  if (is_err(source_val)) return source_val;
  ant_value_t source_str = js_tostring_val(js, source_val);
  if (is_err(source_str)) return source_str;

  ant_value_t flags_val = js_getprop_fallback(js, regexp, "flags");
  if (is_err(flags_val)) return flags_val;
  ant_value_t flags_str = js_tostring_val(js, flags_val);
  if (is_err(flags_str)) return flags_str;

  ant_offset_t src_len, src_off = vstr(js, source_str, &src_len);
  ant_offset_t fl_len, fl_off = vstr(js, flags_str, &fl_len);

  size_t total = 1 + src_len + 1 + fl_len;
  char *buf = ant_calloc(total + 1);
  if (!buf) return js_mkerr(js, "oom");
  size_t n = 0;
  buf[n++] = '/';
  memcpy(buf + n, (const void *)(uintptr_t)src_off, src_len); n += src_len;
  buf[n++] = '/';
  memcpy(buf + n, (const void *)(uintptr_t)fl_off, fl_len); n += fl_len;

  ant_value_t result = js_mkstr(js, buf, n);
  free(buf);
  return result;
}

static ant_value_t builtin_regexp_compile(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t rx = js->this_val;
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "compile called on non-object");

  ant_value_t pattern = js_mkstr(js, "", 0);
  ant_value_t flags = js_mkstr(js, "", 0);

  if (nargs > 0 && vtype(args[0]) != T_UNDEF) {
    ant_value_t is_re = is_regexp_like(js, args[0]);
    if (is_err(is_re)) return is_re;
    if (js_truthy(js, is_re)) {
      ant_value_t src = js_getprop_fallback(js, args[0], "source");
      if (is_err(src)) return src;
      pattern = js_tostring_val(js, src);
      if (is_err(pattern)) return pattern;
      ant_value_t fl = js_getprop_fallback(js, args[0], "flags");
      if (is_err(fl)) return fl;
      flags = js_tostring_val(js, fl);
      if (is_err(flags)) return flags;
    } else {
      pattern = js_tostring_val(js, args[0]);
      if (is_err(pattern)) return pattern;
    }
  }
  if (nargs > 1 && vtype(args[1]) != T_UNDEF) {
    flags = js_tostring_val(js, args[1]);
    if (is_err(flags)) return flags;
  }

  js_setprop(js, rx, js_mkstr(js, "source", 6), pattern);
  js_set_slot(rx, SLOT_DATA, pattern);
  ant_offset_t flen, foff = vstr(js, flags, &flen);
  regexp_init_flags(js, rx, (const char *)(uintptr_t)(foff), flen, false);

  regexp_object_compiled_detach(js, rx);

  return rx;
}

static inline bool is_syntax_char(char c) {
  return 
    c == '^' || c == '$' || c == '\\' || c == '.' || c == '*' ||
    c == '+' || c == '?' || c == '(' || c == ')' || c == '[' ||
    c == ']' || c == '{' || c == '}' || c == '|' || c == '/';
}

static inline bool is_other_punctuator(char c) {
  return
    c == ',' || c == '-' || c == ':' || c == ';' || c == '<' ||
    c == '=' || c == '>' || c == '@' || c == '!' || c == '"' ||
    c == '#' || c == '%' || c == '&' || c == '\'' || c == '`' || c == '~';
}

static ant_value_t builtin_regexp_escape(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR)
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.escape requires a string argument");

  ant_offset_t slen, soff = vstr(js, args[0], &slen);
  const char *src = (const char *)(uintptr_t)(soff);

  size_t buf_cap = slen * 6 + 1;
  char *buf = ant_calloc(buf_cap);
  if (!buf) return js_mkerr(js, "oom");
  size_t di = 0;
  bool first = true;

  for (size_t si = 0; si < slen; ) {
    unsigned char c = (unsigned char)src[si];

    if (c >= 0x80) {
      utf8proc_int32_t cp;
      int bytes = (int)utf8_next(
        (const utf8proc_uint8_t *)&src[si],
        (utf8proc_ssize_t)(slen - si), &cp
      );
      for (int b = 0; b < bytes && si < slen; b++)
        buf[di++] = src[si++];
      first = false;
      continue;
    }

    if (first && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
      di += snprintf(buf + di, buf_cap - di, "\\x%02x", c);
      si++; first = false;
      continue;
    }

    if (is_syntax_char(c)) {
      buf[di++] = '\\'; buf[di++] = c;
      si++; first = false;
      continue;
    }

    if (is_other_punctuator(c) || c == ' ' || c == '\t' || c == '\n' ||
        c == '\r' || c == '\v' || c == '\f') {
      di += snprintf(buf + di, buf_cap - di, "\\x%02x", c);
      si++; first = false;
      continue;
    }

    buf[di++] = c;
    si++; first = false;
  }

  ant_value_t result = js_mkstr(js, buf, di);
  free(buf);
  return result;
}

static ant_value_t regexp_exec_with_exec_fn(ant_t *js, ant_value_t rx, ant_value_t str, ant_value_t exec_fn) {
  if (vtype(exec_fn) == T_FUNC || vtype(exec_fn) == T_CFUNC) {
    if (rxstat_enabled) rxstat_exec_generic++;
    ant_value_t call_args[1] = { str };
    ant_value_t result = sv_vm_call(js->vm, js, exec_fn, rx, call_args, 1, NULL, false);
    if (is_err(result)) return result;
    if (!is_object_type(result) && vtype(result) != T_NULL)
      return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp exec returned non-object");
    return result;
  }

  ant_value_t call_args[1] = { str };
  ant_value_t saved = js->this_val;
  js->this_val = rx;
  ant_value_t result = builtin_regexp_exec(js, call_args, 1);
  js->this_val = saved;

  return result;
}

static ant_value_t regexp_exec_abstract(ant_t *js, ant_value_t rx, ant_value_t str) {
  ant_value_t exec_fn = js_get(js, rx, "exec");
  if (is_err(exec_fn)) return exec_fn;
  return regexp_exec_with_exec_fn(js, rx, str, exec_fn);
}

bool regexp_exec_truthy_try_fast(
  ant_t *js,
  ant_value_t call_func,
  ant_value_t regexp,
  ant_value_t arg,
  ant_value_t *out_result
) {
  if (!out_result || vtype(call_func) != T_CFUNC) return false;
  if (!js_cfunc_same_entrypoint(call_func, builtin_regexp_exec)) return false;
  if (!is_object_type(regexp) || vtype(arg) != T_STR) return false;

  ant_value_t result = regexp_exec_internal(js, regexp, arg, true);
  if (is_err(result)) {
    *out_result = result;
    return true;
  }

  *out_result = mkval(T_BOOL, vtype(result) != T_NULL ? 1 : 0);
  return true;
}

static ant_value_t builtin_regexp_test(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t regexp = js->this_val;
  if (!is_object_type(regexp))
    return js_mkerr_typed(js, JS_ERR_TYPE, "test called on non-object");
  ant_value_t str_arg = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str_arg)) return str_arg;
  ant_value_t exec_fn = js_get(js, regexp, "exec");
  if (is_err(exec_fn)) return exec_fn;

  ant_value_t result;
  if (vtype(exec_fn) == T_CFUNC && js_cfunc_same_entrypoint(exec_fn, builtin_regexp_exec)) {
    result = regexp_exec_internal(js, regexp, str_arg, true);
  } else result = regexp_exec_with_exec_fn(js, regexp, str_arg, exec_fn);
  
  if (is_err(result)) return result;
  return mkval(T_BOOL, vtype(result) != T_NULL ? 1 : 0);
}

static ant_value_t builtin_regexp_flags_getter(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t rx = js->this_val;
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype.flags called on non-object");
    
  char buf[16]; int n = 0;
  ant_value_t v = js_getprop_fallback(js, rx, "hasIndices");
  
  if (is_err(v)) return v;
  if (js_truthy(js, v)) buf[n++] = 'd';

  v = js_getprop_fallback(js, rx, "global");
  if (is_err(v)) return v;
  if (js_truthy(js, v)) buf[n++] = 'g';

  v = js_getprop_fallback(js, rx, "ignoreCase");
  if (is_err(v)) return v;
  if (js_truthy(js, v)) buf[n++] = 'i';

  v = js_getprop_fallback(js, rx, "multiline");
  if (is_err(v)) return v;
  if (js_truthy(js, v)) buf[n++] = 'm';

  v = js_getprop_fallback(js, rx, "dotAll");
  if (is_err(v)) return v;
  if (js_truthy(js, v)) buf[n++] = 's';

  v = js_getprop_fallback(js, rx, "unicode");
  if (is_err(v)) return v;
  if (js_truthy(js, v)) buf[n++] = 'u';

  v = js_getprop_fallback(js, rx, "unicodeSets");
  if (is_err(v)) return v;
  if (js_truthy(js, v)) buf[n++] = 'v';

  v = js_getprop_fallback(js, rx, "sticky");
  if (is_err(v)) return v;
  if (js_truthy(js, v)) buf[n++] = 'y';

  return js_mkstr(js, buf, n);
}

static bool regexp_can_batch_builtin_exec(
  ant_t *js,
  ant_value_t rx,
  compiled_regex_cache_entry_t **compiled_out,
  uint8_t *flags_out
) {
  ant_regex_state_t *state = js->regex_state;
  if (
    !state || !regexp_can_use_internal_fast_path(js, rx)
  ) {
    if (rxstat_enabled) rxstat_batch_reject_state++;
    goto reject;
  }

  ant_prop_loc_t exec_loc = lkp_proto(js, rx, "exec", 4);
  if (
    !exec_loc.obj || !exec_loc.obj->shape ||
    exec_loc.obj->flags.is_exotic ||
    exec_loc.slot >= exec_loc.obj->prop_count
  ) {
    if (rxstat_enabled) rxstat_batch_reject_exec_location++;
    goto reject;
  }

  const ant_shape_prop_t *exec_prop = ant_shape_prop_at(
    exec_loc.obj->shape, exec_loc.slot
  );
  if (!exec_prop || exec_prop->has_getter || exec_prop->has_setter) {
    if (rxstat_enabled) rxstat_batch_reject_exec_accessor++;
    goto reject;
  }

  ant_value_t exec_fn = js_prop_load(exec_loc);
  if (
    vtype(exec_fn) != T_CFUNC ||
    !js_cfunc_same_entrypoint(exec_fn, builtin_regexp_exec)
  ) {
    if (rxstat_enabled) rxstat_batch_reject_exec_value++;
    goto reject;
  }

  compiled_regex_cache_entry_t *compiled_hint;
  uint8_t flags = regexp_flags_mask(js, rx, &compiled_hint);
  if (!(flags & REGEXP_FLAG_GLOBAL)) {
    if (rxstat_enabled) rxstat_batch_reject_flags++;
    goto reject;
  }

  compiled_regex_cache_entry_t *compiled = regex_get_or_compile(
    js, rx, flags, compiled_hint
  );
  if (!compiled) {
    if (rxstat_enabled) rxstat_batch_reject_flags++;
    goto reject;
  }

  regexp_lastindex_cache_location(compiled, rx);
  ant_object_t *lastindex_obj;
  uint32_t lastindex_slot;
  if (!regexp_lastindex_fast_location(
    compiled, rx, &lastindex_obj, &lastindex_slot
  )) {
    if (rxstat_enabled) rxstat_batch_reject_lastindex++;
    goto reject;
  }

  *compiled_out = compiled;
  *flags_out = flags;
  return true;

reject:
  if (rxstat_enabled) rxstat_batch_guard_rejects++;
  return false;
}

static inline PCRE2_SIZE regexp_advance_empty_match(
  const char *str_ptr,
  ant_offset_t str_len,
  PCRE2_SIZE offset,
  bool full_unicode
) {
  if (full_unicode && offset < (PCRE2_SIZE)str_len) {
    return offset + (PCRE2_SIZE)utf8_char_len_at(
      str_ptr, str_len, (ant_offset_t)offset
    );
  }
  return offset + 1;
}

static ant_value_t regexp_match_batch_fast(
  ant_t *js,
  ant_value_t rx,
  ant_value_t str,
  bool full_unicode,
  bool *used_fast_path
) {
  *used_fast_path = false;

  compiled_regex_cache_entry_t *compiled;
  uint8_t flags;
  if (!regexp_can_batch_builtin_exec(js, rx, &compiled, &flags))
    return js_mkundef();

  *used_fast_path = true;
  if (rxstat_enabled) rxstat_batch_match_calls++;

  ant_value_t stored = regexp_set_lastindex(js, compiled, rx, tov(0));
  if (is_err(stored)) return stored;

  ant_value_t matches = js_mkarr(js);
  if (is_err(matches)) return matches;

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (const char *)(uintptr_t)str_off;
  bool sticky = (flags & REGEXP_FLAG_STICKY) != 0;
  PCRE2_SIZE offset = 0;
  ant_offset_t count = 0;

  while (offset <= (PCRE2_SIZE)str_len) {
    regex_match_scope_t scope;
    PCRE2_SIZE *ovector;
    uint32_t ovcount;
    int rc = compiled_regex_run(
      js, compiled, str_ptr, str_len, offset,
      sticky ? PCRE2_ANCHORED : 0, sticky,
      &scope, &ovector, &ovcount
    );
    if (rc < 0) {
      regex_match_scope_end(&scope);
      break;
    }

    PCRE2_SIZE start = ovector[0];
    PCRE2_SIZE end = ovector[1];
    update_regexp_statics(js, str_ptr, ovector, ovcount);
    ant_value_t match = js_mkstr(js, str_ptr + start, end - start);
    regex_match_scope_end(&scope);
    if (is_err(match)) return match;
    js_arr_push(js, matches, match);
    count++;
    if (rxstat_enabled) rxstat_batch_match_results++;

    offset = end;
    if (start == end) {
      offset = regexp_advance_empty_match(
        str_ptr, str_len, offset, full_unicode
      );
    }
  }

  stored = regexp_set_lastindex(js, compiled, rx, tov(0));
  if (is_err(stored)) return stored;
  return count == 0 ? js_mknull() : matches;
}

static ant_value_t builtin_regexp_symbol_match(ant_t *js, ant_value_t *args, int nargs) {
  if (rxstat_enabled) rxstat_symbol_match_calls++;
  ant_value_t rx = js->this_val;
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@match] called on non-object");

  ant_value_t str = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str)) return str;

  ant_value_t global_val = js_getprop_fallback(js, rx, "global");
  if (is_err(global_val)) return global_val;

  if (!js_truthy(js, global_val))
    return regexp_exec_abstract(js, rx, str);

  ant_value_t unicode_val = js_getprop_fallback(js, rx, "unicode");
  if (is_err(unicode_val)) return unicode_val;

  bool full_unicode = js_truthy(js, unicode_val);
  bool used_fast_path = false;
  ant_value_t fast = regexp_match_batch_fast(
    js, rx, str, full_unicode, &used_fast_path
  );
  if (is_err(fast) || used_fast_path) return fast;

  js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), tov(0));

  ant_value_t A = js_mkarr(js);
  if (is_err(A)) return A;
  ant_offset_t n = 0;

  for (;;) {
    ant_value_t result = regexp_exec_abstract(js, rx, str);
    if (is_err(result)) return result;
    if (vtype(result) == T_NULL) return n == 0 ? js_mknull() : mkval(T_ARR, vdata(A));
    if (rxstat_enabled) rxstat_symbol_match_results++;

    ant_value_t match_str = js_tostring_val(js, js_arr_get(js, result, 0));
    if (is_err(match_str)) return match_str;
    js_arr_push(js, A, match_str);
    n++;

    ant_offset_t mlen;
    vstr(js, match_str, &mlen);
    if (mlen == 0) {
      ant_value_t li_val = js_getprop_fallback(js, rx, "lastIndex");
      if (is_err(li_val)) return li_val;
      double li = vtype(li_val) == T_NUM ? tod(li_val) : 0;
      ant_offset_t str_len, str_off = vstr(js, str, &str_len);
      double advance = 1;
      if (full_unicode && li < (double)str_len) {
        advance = (double)utf8_char_len_at((const char *)(uintptr_t)(str_off), str_len, (ant_offset_t)li);
      } js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), tov(li + advance));
    }
  }
}


static ant_value_t regexp_matchall_next(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t iter = js->this_val;
  ant_value_t rx = js_get_slot(iter, SLOT_MATCHALL_RX);
  ant_value_t str = js_get_slot(iter, SLOT_MATCHALL_STR);
  ant_value_t done_val = js_get_slot(iter, SLOT_MATCHALL_DONE);
  
  if (js_truthy(js, done_val))
    return js_iter_result(js, false, js_mkundef());

  ant_value_t result = regexp_exec_abstract(js, rx, str);
  if (is_err(result)) return result;

  if (vtype(result) == T_NULL) {
    js_set_slot(iter, SLOT_MATCHALL_DONE, js_true);
    return js_iter_result(js, false, js_mkundef());
  }

  ant_value_t global_val = js_getprop_fallback(js, rx, "global");
  if (js_truthy(js, global_val)) {
    ant_value_t match_str = js_tostring_val(js, js_arr_get(js, result, 0));
    if (is_err(match_str)) return match_str;
    ant_offset_t mlen;
    vstr(js, match_str, &mlen);
    if (mlen == 0) {
      ant_value_t li_val = js_getprop_fallback(js, rx, "lastIndex");
      double li = vtype(li_val) == T_NUM ? tod(li_val) : 0;
      js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), tov(li + 1));
    }
  } else js_set_slot(iter, SLOT_MATCHALL_DONE, js_true);

  return js_iter_result(js, true, result);
}

static ant_value_t builtin_regexp_symbol_matchAll(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t rx = js->this_val;
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@matchAll] called on non-object");

  ant_value_t str = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str)) return str;

  ant_value_t flags_val = js_getprop_fallback(js, rx, "flags");
  if (is_err(flags_val)) return flags_val;
  ant_value_t flags_str = js_tostring_val(js, flags_val);
  if (is_err(flags_str)) return flags_str;

  ant_value_t source_val = js_getprop_fallback(js, rx, "source");
  if (is_err(source_val)) return source_val;

  ant_value_t ctor_args[2] = { source_val, flags_str };
  ant_value_t regexp_ctor = js_get(js, js_glob(js), "RegExp");
  ant_value_t new_rx = sv_vm_call(js->vm, js, regexp_ctor, js_mkundef(), ctor_args, 2, NULL, true);
  if (is_err(new_rx)) return new_rx;

  ant_value_t li_val = js_getprop_fallback(js, rx, "lastIndex");
  js_setprop(js, new_rx, js_mkstr(js, "lastIndex", 9), li_val);

  ant_value_t iter = js_mkobj(js);
  js_set_slot(iter, SLOT_MATCHALL_RX, new_rx);
  js_set_slot(iter, SLOT_MATCHALL_STR, str);
  js_set_slot(iter, SLOT_MATCHALL_DONE, js_false);

  js_set_proto_init(iter, js->builtins.regexp_matchall_iter_proto_val);

  return iter;
}

static ant_value_t builtin_string_matchAll(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_unwrapped = unwrap_primitive(js, js->this_val);
  ant_value_t str = js_tostring_val(js, this_unwrapped);
  if (is_err(str)) return str;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "matchAll requires at least 1 argument");

  if (is_object_type(args[0])) {
    ant_value_t is_re = is_regexp_like(js, args[0]);
    if (js_truthy(js, is_re)) {
      ant_value_t flags_val = js_getprop_fallback(js, args[0], "flags");
      if (is_err(flags_val)) return flags_val;
      
      ant_value_t flags_str = js_tostring_val(js, flags_val);
      ant_offset_t flen, foff = vstr(js, flags_str, &flen);
      
      const char *fp = (const char *)(uintptr_t)(foff);
      bool has_g = false;
      for (ant_offset_t i = 0; i < flen; i++) if (fp[i] == 'g') has_g = true;
      if (!has_g) return js_mkerr_typed(js, JS_ERR_TYPE, "String.prototype.matchAll called with a non-global RegExp");
    }

    bool called = false;
    ant_value_t call_args[1] = { str };
    ant_value_t dispatched = maybe_call_symbol_method(
      js, args[0], get_matchAll_sym(), args[0], call_args, 1, &called
    );
    
    if (is_err(dispatched)) return dispatched;
    if (called) return dispatched;
  }

  ant_value_t pattern_str = js_tostring_val(js, args[0]);
  if (is_err(pattern_str)) return pattern_str;

  ant_value_t ctor_args[2] = { pattern_str, js_mkstr(js, "g", 1) };
  ant_value_t regexp_ctor = js_get(js, js_glob(js), "RegExp");
  ant_value_t rx = sv_vm_call(js->vm, js, regexp_ctor, js_mkundef(), ctor_args, 2, NULL, true);
  if (is_err(rx)) return rx;

  ant_value_t ma_args[1] = { str };
  js->this_val = rx;
  
  return builtin_regexp_symbol_matchAll(js, ma_args, 1);
}

static const char *find_bytes(const char *haystack, ant_offset_t haystack_len, const char *needle, ant_offset_t needle_len) {
  if (needle_len == 0 || needle_len > haystack_len) return NULL;
  ant_offset_t last = haystack_len - needle_len;
  for (ant_offset_t i = 0; i <= last; i++) {
    if (haystack[i] == needle[0] && memcmp(haystack + i, needle, needle_len) == 0)
      return haystack + i;
  }
  return NULL;
}

static bool str_buf_append(char **buf, size_t *len, size_t *cap, const char *data, size_t n);

static bool replacement_has_substitution(ant_t *js, ant_value_t replacement) {
  ant_offset_t len, off = vstr(js, replacement, &len);
  const char *ptr = (const char *)(uintptr_t)off;
  for (ant_offset_t i = 0; i < len; i++) {
    if (ptr[i] == '$') return true;
  }
  return false;
}

static bool regex_source_is_plain_literal(const char *source, ant_offset_t len) {
  if (len == 0) return false;
  for (ant_offset_t i = 0; i < len; i++) {
    switch (source[i]) {
      case '\\': case '^': case '$': case '.': case '*': case '+': case '?':
      case '(': case ')': case '[': case ']': case '{': case '}': case '|':
        return false;
      default:
        break;
    }
  }
  return true;
}

static bool regexp_plain_literal_pattern(
  ant_t *js,
  ant_value_t rx,
  uint8_t flags_mask,
  const char **pattern_ptr,
  ant_offset_t *pattern_len
) {
  if (flags_mask & (REGEXP_FLAG_IGNORE_CASE | REGEXP_FLAG_STICKY | REGEXP_FLAG_UNICODE_SET))
    return false;
  if (!regexp_can_use_internal_fast_path(js, rx)) return false;
  if (!regexp_source_pattern(js, rx, pattern_ptr, pattern_len)) return false;
  return regex_source_is_plain_literal(*pattern_ptr, *pattern_len);
}

static ant_value_t regexp_replace_plain_literal_fast(
  ant_t *js,
  ant_value_t rx,
  ant_value_t str,
  ant_value_t replace_str,
  bool global,
  bool *used_fast_path
) {
  *used_fast_path = false;

  if (!regexp_can_use_internal_fast_path(js, rx)) return js_mkundef();

  ant_value_t exec_fn = js_get(js, rx, "exec");
  if (is_err(exec_fn)) return exec_fn;
  if (!js_cfunc_same_entrypoint(exec_fn, builtin_regexp_exec)) return js_mkundef();

  uint8_t flags_mask = regexp_flags_mask(js, rx, NULL);
  const char *needle;
  ant_offset_t needle_len;
  if (!regexp_plain_literal_pattern(js, rx, flags_mask, &needle, &needle_len)) return js_mkundef();

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (const char *)(uintptr_t)str_off;
  ant_offset_t repl_len, repl_off = vstr(js, replace_str, &repl_len);
  const char *repl_ptr = (const char *)(uintptr_t)repl_off;

  const char *first = find_bytes(str_ptr, str_len, needle, needle_len);
  *used_fast_path = true;
  if (!first) return str;

  size_t cap = str_len + repl_len + 256;
  char *buf = ant_calloc(cap);
  if (!buf) return js_mkerr(js, "oom");

  size_t len = 0;
  const char *scan = str_ptr;
  const char *end = str_ptr + str_len;
  const char *match = first;
  PCRE2_SIZE ovector[2];

  for (;;) {
    if (!str_buf_append(&buf, &len, &cap, scan, (size_t)(match - scan)) ||
        !str_buf_append(&buf, &len, &cap, repl_ptr, repl_len)) {
      free(buf);
      return js_mkerr(js, "oom");
    }

    ovector[0] = (PCRE2_SIZE)(match - str_ptr);
    ovector[1] = ovector[0] + (PCRE2_SIZE)needle_len;
    update_regexp_statics(js, str_ptr, ovector, 1);

    scan = match + needle_len;
    if (!global) break;
    match = find_bytes(scan, (ant_offset_t)(end - scan), needle, needle_len);
    if (!match) break;
  }

  if (!str_buf_append(&buf, &len, &cap, scan, (size_t)(end - scan))) {
    free(buf);
    return js_mkerr(js, "oom");
  }

  if (global && is_err(setprop_cstr(js, rx, "lastIndex", 9, tov(0)))) {
    free(buf);
    return js_mkerr(js, "oom");
  }

  ant_value_t ret = js_mkstr(js, buf, len);
  free(buf);
  return ret;
}

static ant_value_t regexp_replace_batch_fast(
  ant_t *js,
  ant_value_t rx,
  ant_value_t str,
  ant_value_t replacement,
  bool full_unicode,
  bool *used_fast_path
) {
  *used_fast_path = false;

  compiled_regex_cache_entry_t *compiled;
  uint8_t flags;
  if (!regexp_can_batch_builtin_exec(js, rx, &compiled, &flags))
    return js_mkundef();

  *used_fast_path = true;
  if (rxstat_enabled) rxstat_batch_replace_calls++;

  ant_value_t stored = regexp_set_lastindex(js, compiled, rx, tov(0));
  if (is_err(stored)) return stored;

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (const char *)(uintptr_t)str_off;
  ant_offset_t replacement_len, replacement_off = vstr(
    js, replacement, &replacement_len
  );
  const char *replacement_ptr = (const char *)(uintptr_t)replacement_off;

  size_t buf_cap = (size_t)str_len + 256;
  char *buf = ant_calloc(buf_cap);
  if (!buf) return js_mkerr(js, "oom");
  size_t buf_len = 0;

  bool sticky = (flags & REGEXP_FLAG_STICKY) != 0;
  PCRE2_SIZE offset = 0;
  PCRE2_SIZE next_src_pos = 0;
  size_t match_count = 0;

  while (offset <= (PCRE2_SIZE)str_len) {
    regex_match_scope_t scope;
    PCRE2_SIZE *ovector;
    uint32_t ovcount;
    int rc = compiled_regex_run(
      js, compiled, str_ptr, str_len, offset,
      sticky ? PCRE2_ANCHORED : 0, sticky,
      &scope, &ovector, &ovcount
    );
    if (rc < 0) {
      regex_match_scope_end(&scope);
      break;
    }

    PCRE2_SIZE start = ovector[0];
    PCRE2_SIZE end = ovector[1];
    if (
      start > next_src_pos &&
      !str_buf_append(
        &buf, &buf_len, &buf_cap,
        str_ptr + next_src_pos, start - next_src_pos
      )
    ) {
      regex_match_scope_end(&scope);
      free(buf);
      return js_mkerr(js, "oom");
    }

    int capture_count = ovcount > 1 ? (int)(ovcount - 1) : 0;
    repl_capture_t captures_inline[31];
    repl_capture_t *captures = capture_count <= 31
      ? captures_inline
      : ant_calloc(sizeof(*captures) * (size_t)capture_count);
    if (capture_count > 31 && !captures) {
      regex_match_scope_end(&scope);
      free(buf);
      return js_mkerr(js, "oom");
    }
    for (int i = 0; i < capture_count; i++) {
      PCRE2_SIZE capture_start = ovector[2 * (i + 1)];
      PCRE2_SIZE capture_end = ovector[2 * (i + 1) + 1];
      captures[i] = capture_start == PCRE2_UNSET
        ? (repl_capture_t){ NULL, 0 }
        : (repl_capture_t){
            str_ptr + capture_start,
            (size_t)(capture_end - capture_start)
          };
    }

    update_regexp_statics(js, str_ptr, ovector, ovcount);
    bool replaced = repl_template(
      replacement_ptr, replacement_len,
      str_ptr + start, end - start,
      str_ptr, str_len, start,
      captures, capture_count,
      &buf, &buf_len, &buf_cap
    );
    if (captures != captures_inline) free(captures);
    regex_match_scope_end(&scope);
    if (!replaced) {
      free(buf);
      return js_mkerr(js, "oom");
    }

    match_count++;
    if (rxstat_enabled) rxstat_batch_replace_results++;
    next_src_pos = end;
    offset = end;
    if (start == end) {
      offset = regexp_advance_empty_match(
        str_ptr, str_len, offset, full_unicode
      );
    }
  }

  if (
    next_src_pos < (PCRE2_SIZE)str_len &&
    !str_buf_append(
      &buf, &buf_len, &buf_cap,
      str_ptr + next_src_pos, (size_t)str_len - next_src_pos
    )
  ) {
    free(buf);
    return js_mkerr(js, "oom");
  }

  stored = regexp_set_lastindex(js, compiled, rx, tov(0));
  if (is_err(stored)) {
    free(buf);
    return stored;
  }
  if (match_count == 0) {
    free(buf);
    return str;
  }

  ant_value_t result = js_mkstr(js, buf, buf_len);
  free(buf);
  return result;
}

static ant_value_t regexp_search_plain_literal_fast(
  ant_t *js,
  ant_value_t rx,
  ant_value_t str,
  bool *used_fast_path
) {
  *used_fast_path = false;

  if (!regexp_can_use_internal_fast_path(js, rx)) return js_mkundef();

  ant_value_t exec_fn = js_get(js, rx, "exec");
  if (is_err(exec_fn)) return exec_fn;
  if (!js_cfunc_same_entrypoint(exec_fn, builtin_regexp_exec)) return js_mkundef();

  uint8_t flags_mask = regexp_flags_mask(js, rx, NULL);
  const char *needle;
  ant_offset_t needle_len;
  if (!regexp_plain_literal_pattern(js, rx, flags_mask, &needle, &needle_len)) return js_mkundef();

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (const char *)(uintptr_t)str_off;
  const char *match = find_bytes(str_ptr, str_len, needle, needle_len);

  *used_fast_path = true;
  if (!match) return tov(-1);

  PCRE2_SIZE ovector[2];
  ovector[0] = (PCRE2_SIZE)(match - str_ptr);
  ovector[1] = ovector[0] + (PCRE2_SIZE)needle_len;
  update_regexp_statics(js, str_ptr, ovector, 1);
  return tov((double)ovector[0]);
}

static bool regexp_literal_exec_builtin_guard(ant_t *js) {
  ant_regex_state_t *state = js->regex_state;
  if (!state || state->exec_property_written) return false;

  ant_value_t ctor = js_getprop_fallback(js, js_glob(js), "RegExp");
  if (is_err(ctor)) return false;
  
  ant_value_t proto = js_getprop_fallback(js, ctor, "prototype");
  if (is_err(proto)) return false;
  if (!is_object_type(proto)) return false;

  ant_value_t exec_fn = js_getprop_fallback(js, proto, "exec");
  if (is_err(exec_fn)) return false;
  
  return vtype(exec_fn) == T_CFUNC && js_cfunc_same_entrypoint(exec_fn, builtin_regexp_exec);
}

static bool regexp_literal_exec_fast_parts(
  ant_t *js,
  ant_value_t pattern,
  ant_value_t flags,
  const char **needle,
  ant_offset_t *needle_len
) {
  if (vtype(pattern) != T_STR || vtype(flags) != T_STR) return false;

  ant_offset_t flags_len, flags_off = vstr(js, flags, &flags_len);
  uint8_t flags_mask = regexp_parse_flags_mask((const char *)(uintptr_t)flags_off, flags_len);
  
  if (
    flags_mask & 
    (REGEXP_FLAG_HAS_INDICES | REGEXP_FLAG_IGNORE_CASE | REGEXP_FLAG_STICKY | REGEXP_FLAG_UNICODE_SET)
  ) return false;

  ant_offset_t pattern_off = vstr(js, pattern, needle_len);
  *needle = (const char *)(uintptr_t)pattern_off;
  
  return regex_source_is_plain_literal(*needle, *needle_len);
}

static ant_value_t regexp_literal_object(ant_t *js, ant_value_t pattern, ant_value_t flags) {
  if (rxstat_enabled) rxstat_lit_obj++;
  ant_value_t regexp_obj = mkobj(js, 0);
  if (is_err(regexp_obj)) return regexp_obj;

  ant_value_t regexp_proto = js_get_ctor_proto(js, "RegExp", 6);
  if (is_object_type(regexp_proto)) js_set_proto_init(regexp_obj, regexp_proto);

  ant_offset_t pattern_len; vstr(js, pattern, &pattern_len);
  if (is_err(js_mkprop_fast(js, regexp_obj, "source", 6, pattern))) return js_mkerr(js, "oom");
  js_set_slot(regexp_obj, SLOT_DATA, pattern);

  ant_offset_t flags_len, flags_off = vstr(js, flags, &flags_len);
  regexp_init_flags(js, regexp_obj, (const char *)(uintptr_t)flags_off, flags_len, true);
  
  return regexp_obj;
}

ant_value_t regexp_literal_exec_call(
  ant_t *js,
  ant_value_t pattern,
  ant_value_t flags,
  ant_value_t arg
) {
  if (
    vtype(arg) == T_STR &&
    regexp_literal_exec_builtin_guard(js)
  ) {
    const char *needle;
    ant_offset_t needle_len;
    if (regexp_literal_exec_fast_parts(js, pattern, flags, &needle, &needle_len)) {
      ant_offset_t str_len, str_off = vstr(js, arg, &str_len);
      const char *str_ptr = (const char *)(uintptr_t)str_off;
      const char *match = find_bytes(str_ptr, str_len, needle, needle_len);
      if (!match) return js_mknull();

      PCRE2_SIZE ovector[2];
      ovector[0] = (PCRE2_SIZE)(match - str_ptr);
      ovector[1] = ovector[0] + (PCRE2_SIZE)needle_len;
      update_regexp_statics(js, str_ptr, ovector, 1);

      rxstat_note_result(1, false, false);
      ant_value_t result_arr = js_mkarr(js);
      if (is_err(result_arr)) return result_arr;

      ant_value_t match_str = js_mkstr(js, match, needle_len);
      if (is_err(match_str)) return match_str;
      js_arr_push(js, result_arr, match_str);

      if (!regexp_result_apply_shape(
        js, result_arr, tov((double)ovector[0]), arg,
        js_mkundef(), js_mkundef(), false
      )) {
        if (is_err(js_mkprop_fast(js, result_arr, "index", 5, tov((double)ovector[0])))) return js_mkerr(js, "oom");
        if (is_err(js_mkprop_fast(js, result_arr, "input", 5, arg))) return js_mkerr(js, "oom");
        if (is_err(js_mkprop_fast(js, result_arr, "groups", 6, js_mkundef()))) return js_mkerr(js, "oom");
      }
      return result_arr;
    }
  }

  ant_value_t regexp_obj = regexp_literal_object(js, pattern, flags);
  if (is_err(regexp_obj)) return regexp_obj;
  return regexp_exec_abstract(js, regexp_obj, arg);
}

static ant_value_t builtin_regexp_symbol_replace(ant_t *js, ant_value_t *args, int nargs) {
  if (rxstat_enabled) rxstat_symbol_replace_calls++;
  ant_value_t rx = js->this_val;
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@replace] called on non-object");

  ant_value_t str = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str)) return str;
  ant_value_t replace_value = nargs > 1 ? args[1] : js_mkundef();
  bool func_replace = (vtype(replace_value) == T_FUNC || vtype(replace_value) == T_CFUNC);
  if (rxstat_enabled && func_replace) rxstat_symbol_replace_func++;
  ant_value_t replace_str = js_mkundef();
  if (!func_replace) {
    replace_str = js_tostring_val(js, replace_value);
    if (is_err(replace_str)) return replace_str;
    if (rxstat_enabled && replacement_has_substitution(js, replace_str))
      rxstat_symbol_replace_substitution++;
  }

  ant_value_t global_val = js_getprop_fallback(js, rx, "global");
  if (is_err(global_val)) return global_val;
  bool global = js_truthy(js, global_val);

  bool full_unicode = false;
  if (global) {
    ant_value_t unicode_val = js_getprop_fallback(js, rx, "unicode");
    if (is_err(unicode_val)) return unicode_val;
    full_unicode = js_truthy(js, unicode_val);
    js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), tov(0));
  }

  if (!func_replace && !replacement_has_substitution(js, replace_str)) {
    bool used_fast_path = false;
    ant_value_t fast = regexp_replace_plain_literal_fast(js, rx, str, replace_str, global, &used_fast_path);
    if (is_err(fast) || used_fast_path) return fast;
  }

  if (global && !func_replace) {
    bool used_fast_path = false;
    ant_value_t fast = regexp_replace_batch_fast(
      js, rx, str, replace_str, full_unicode, &used_fast_path
    );
    if (is_err(fast) || used_fast_path) return fast;
  }

  ant_value_t results = js_mkarr(js);
  if (is_err(results)) return results;
  ant_offset_t nresults = 0;

  for (;;) {
    ant_value_t result = regexp_exec_abstract(js, rx, str);
    if (is_err(result)) return result;
    if (vtype(result) == T_NULL) break;
    if (rxstat_enabled) rxstat_symbol_replace_results++;
    js_arr_push(js, results, result);
    nresults++;
    if (!global) break;

    ant_value_t match_str = js_tostring_val(js, js_arr_get(js, result, 0));
    if (is_err(match_str)) return match_str;
    ant_offset_t mlen; vstr(js, match_str, &mlen);
    if (mlen == 0) {
      ant_value_t li_val = js_getprop_fallback(js, rx, "lastIndex");
      if (is_err(li_val)) return li_val;
      double li = vtype(li_val) == T_NUM ? tod(li_val) : 0;
      ant_offset_t sl, so = vstr(js, str, &sl);
      double advance = 1;
      if (full_unicode && li < (double)sl) {
        advance = (double)utf8_char_len_at((const char *)(uintptr_t)(so), sl, (ant_offset_t)li);
      }
      js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), tov(li + advance));
    }
  }

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  size_t buf_cap = str_len + 256;
  char *buf = ant_calloc(buf_cap);
  if (!buf) return js_mkerr(js, "oom");
  size_t buf_len = 0;
  ant_offset_t next_src_pos = 0;

#define SB_APPEND(data, dlen) do { \
  if (buf_len + (dlen) >= buf_cap) { \
    buf_cap = (buf_len + (dlen) + 1) * 2; \
    char *nb = ant_realloc(buf, buf_cap); \
    if (!nb) { free(buf); return js_mkerr(js, "oom"); } \
    buf = nb; \
  } \
  memcpy(buf + buf_len, data, dlen); buf_len += (dlen); \
} while(0)

  for (ant_offset_t i = 0; i < nresults; i++) {
    ant_value_t result = js_arr_get(js, results, i);
    ant_value_t matched = js_tostring_val(js, js_arr_get(js, result, 0));
    if (is_err(matched)) { free(buf); return matched; }
    ant_offset_t matched_len; vstr(js, matched, &matched_len);

    ant_value_t pos_val = js_getprop_fallback(js, result, "index");
    ant_offset_t position = 0;
    if (!is_err(pos_val) && vtype(pos_val) == T_NUM) {
      double d = tod(pos_val);
      position = d < 0 ? 0 : (ant_offset_t)d;
    }
    if (position > str_len) position = str_len;

    ant_value_t replacement;
    if (func_replace) {
      ant_offset_t ncaptures = js_arr_len(js, result);
      if (ncaptures > INT_MAX - 2) {
        free(buf);
        return js_mkerr(js, "too many regexp captures");
      }
      int call_capacity = (int)ncaptures + 2;
      ant_value_t call_args_inline[32];
      ant_value_t *call_args = call_capacity <= 32
        ? call_args_inline
        : malloc(sizeof(*call_args) * (size_t)call_capacity);
      if (!call_args) {
        free(buf);
        return js_mkerr(js, "oom");
      }
      int ca = 0;
      for (ant_offset_t c = 0; c < ncaptures; c++)
        call_args[ca++] = js_arr_get(js, result, c);
      call_args[ca++] = tov((double)position);
      call_args[ca++] = str;
      replacement = sv_vm_call(js->vm, js, replace_value, js_mkundef(), call_args, ca, NULL, false);
      if (call_args != call_args_inline) free(call_args);
    } else {
      replacement = replace_str;
    }
    if (is_err(replacement)) { free(buf); return replacement; }
    ant_value_t rep_str = js_tostring_val(js, replacement);
    if (is_err(rep_str)) { free(buf); return rep_str; }

    if (position >= next_src_pos) {
      str_off = vstr(js, str, &str_len);
      if (position > next_src_pos)
        SB_APPEND((const char *)(uintptr_t)(str_off + next_src_pos), position - next_src_pos);
      ant_offset_t rep_len, rep_off = vstr(js, rep_str, &rep_len);
      if (func_replace) {
        SB_APPEND((const char *)(uintptr_t)(rep_off), rep_len);
      } else {
        ant_offset_t ncap = js_arr_len(js, result);
        int num_caps = ncap > 1 ? (int)(ncap - 1) : 0;
        repl_capture_t caps_buf[16], *caps = num_caps <= 16 ? caps_buf : ant_calloc(sizeof(repl_capture_t) * (size_t)num_caps);
        if (num_caps > 16 && !caps) {
          free(buf);
          return js_mkerr(js, "oom");
        }
        for (int ci = 0; ci < num_caps; ci++) {
          ant_value_t cap = js_arr_get(js, result, (ant_offset_t)(ci + 1));
          if (vtype(cap) == T_STR) { ant_offset_t cl, co = vstr(js, cap, &cl); caps[ci] = (repl_capture_t){ (const char *)(uintptr_t)(co), cl }; }
          else caps[ci] = (repl_capture_t){ NULL, 0 };
        }
        ant_offset_t mlen, moff = vstr(js, matched, &mlen);
        str_off = vstr(js, str, &str_len);
        bool ok = repl_template(
          (const char *)(uintptr_t)(rep_off), rep_len,
          (const char *)(uintptr_t)(moff), mlen,
          (const char *)(uintptr_t)(str_off), str_len, position,
          caps, num_caps, &buf, &buf_len, &buf_cap
        );
        if (caps != caps_buf) free(caps);
        if (!ok) {
          free(buf);
          return js_mkerr(js, "oom");
        }
      }
      next_src_pos = position + matched_len;
    }
  }

  str_off = vstr(js, str, &str_len);
  if (next_src_pos < str_len)
    SB_APPEND((const char *)(uintptr_t)(str_off + next_src_pos), str_len - next_src_pos);

#undef SB_APPEND

  ant_value_t ret = js_mkstr(js, buf, buf_len);
  free(buf);
  return ret;
}

static ant_value_t builtin_regexp_symbol_search(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t rx = js->this_val;
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@search] called on non-object");

  ant_value_t str = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str)) return str;

  bool used_fast_path = false;
  ant_value_t fast = regexp_search_plain_literal_fast(js, rx, str, &used_fast_path);
  if (is_err(fast) || used_fast_path) return fast;

  ant_value_t prev_li = js_getprop_fallback(js, rx, "lastIndex");
  if (is_err(prev_li)) return prev_li;
  js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), tov(0));

  ant_value_t result = regexp_exec_abstract(js, rx, str);
  if (is_err(result)) return result;

  ant_value_t cur_li = js_getprop_fallback(js, rx, "lastIndex");
  if (is_err(cur_li)) return cur_li;
  js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), prev_li);

  if (vtype(result) == T_NULL) return tov(-1);

  ant_value_t idx = js_getprop_fallback(js, result, "index");
  if (is_err(idx)) return idx;
  return vtype(idx) == T_NUM ? idx : tov(-1);
}

static ant_value_t builtin_regexp_symbol_split(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t rx = js_getthis(js);
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@split] called on non-object");

  ant_value_t str = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "", 0);
  if (is_err(str)) return str;

  ant_value_t ctor = js_get(js, rx, "constructor");
  if (is_err(ctor)) return ctor;

  ant_value_t C;
  if (vtype(ctor) == T_UNDEF) {
    C = js_get(js, js_glob(js), "RegExp");
  } else if (!is_object_type(ctor)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@split]: constructor is not an object");
  } else {
    ant_value_t species = get_ctor_species_value(js, ctor);
    if (is_err(species)) return species;
    if (vtype(species) == T_UNDEF || vtype(species) == T_NULL)
      C = js_get(js, js_glob(js), "RegExp");
    else C = species;
  }

  if (is_err(C)) return C;
  if (vtype(C) != T_FUNC && vtype(C) != T_CFUNC)
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp species is not a constructor");

  ant_value_t flags_val = js_get(js, rx, "flags");
  if (is_err(flags_val)) return flags_val;
  ant_value_t flags_str = js_tostring_val(js, flags_val);
  if (is_err(flags_str)) return flags_str;

  ant_offset_t flen, foff = vstr(js, flags_str, &flen);
  const char *fptr = (const char *)(uintptr_t)(foff);
  bool unicode_matching = false, has_sticky = false;
  for (ant_offset_t i = 0; i < flen; i++) {
    if (fptr[i] == 'u' || fptr[i] == 'v') unicode_matching = true;
    if (fptr[i] == 'y') has_sticky = true;
  }

  ant_value_t new_flags;
  if (has_sticky) new_flags = flags_str; else {
    char fbuf[16];
    if (flen > 14) flen = 14;
    foff = vstr(js, flags_str, &flen);
    fptr = (const char *)(uintptr_t)(foff);
    memcpy(fbuf, fptr, flen);
    fbuf[flen] = 'y';
    new_flags = js_mkstr(js, fbuf, flen + 1);
  }

  ant_value_t ctor_args[2] = { rx, new_flags };
  ant_value_t splitter = regexp_species_construct(js, rx, C, ctor_args, 2);
  if (is_err(splitter)) return splitter;

  ant_value_t A = js_mkarr(js);
  if (is_err(A)) return A;
  ant_offset_t lengthA = 0;

  uint32_t lim = UINT32_MAX;
  if (nargs >= 2 && vtype(args[1]) != T_UNDEF) {
    double d = tod(args[1]);
    if (d >= 0 && d <= UINT32_MAX) lim = (uint32_t)d;
  } if (lim == 0) return mkval(T_ARR, vdata(A));

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  ant_offset_t size = str_len;

  if (size == 0) {
    ant_value_t z = regexp_exec_abstract(js, splitter, str);
    if (is_err(z)) return z;
    if (vtype(z) == T_NULL) js_arr_push(js, A, str);
    return mkval(T_ARR, vdata(A));
  }

  ant_offset_t p = 0, q = p;
  ant_value_t lastIndex_key = js_mkstr(js, "lastIndex", 9);

  while (q < size) {
    js_setprop(js, splitter, lastIndex_key, tov((double)q));

    ant_value_t z = regexp_exec_abstract(js, splitter, str);
    if (is_err(z)) return z;

    if (vtype(z) == T_NULL) {
      if (unicode_matching) {
        str_off = vstr(js, str, &str_len);
        q += utf8_char_len_at((const char *)(uintptr_t)(str_off), str_len, q);
      } else q++;
      continue;
    }

    ant_value_t li_val = js_get(js, splitter, "lastIndex");
    if (is_err(li_val)) return li_val;
    double e_raw = vtype(li_val) == T_NUM ? tod(li_val) : 0;
    ant_offset_t e = (ant_offset_t)(e_raw < 0 ? 0 : (e_raw > (double)size ? (double)size : e_raw));

    if (e == p) {
      if (unicode_matching) {
        str_off = vstr(js, str, &str_len);
        q += utf8_char_len_at((const char *)(uintptr_t)(str_off), str_len, q);
      } else q++;
      continue;
    }

    str_off = vstr(js, str, NULL);
    ant_value_t T_val = js_mkstr(js, (char *)(uintptr_t)(str_off + p), q - p);
    js_arr_push(js, A, T_val);
    lengthA++;
    if (lengthA == lim) return mkval(T_ARR, vdata(A));

    ant_offset_t num_caps = js_arr_len(js, z);
    for (ant_offset_t i = 1; i < num_caps; i++) {
      ant_value_t cap = js_arr_get(js, z, i);
      js_arr_push(js, A, cap);
      lengthA++;
      if (lengthA == lim) return mkval(T_ARR, vdata(A));
    }

    p = e;
    q = p;
  }

  str_off = vstr(js, str, &str_len);
  ant_value_t trailing = js_mkstr(js, (char *)(uintptr_t)(str_off + p), str_len - p);
  js_arr_push(js, A, trailing);
  return mkval(T_ARR, vdata(A));
}

ant_value_t do_regex_match_pcre2(ant_t *js, regex_match_args_t args) {
  char pcre2_pattern[4096];
  
  size_t pcre2_len = js_to_pcre2_pattern(
    args.pattern_ptr, args.pattern_len,
    pcre2_pattern, sizeof(pcre2_pattern),
    false
  );

  uint32_t options = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_UNSET_BACKREF | PCRE2_DUPNAMES;
  if (args.ignore_case) options |= PCRE2_CASELESS;
  if (args.multiline) options |= PCRE2_MULTILINE;

  int errcode;
  PCRE2_SIZE erroffset;
  pcre2_code *re = pcre2_compile((PCRE2_SPTR)pcre2_pattern, pcre2_len, options, &errcode, &erroffset, NULL);
  if (re == NULL) return js_mknull();

  pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
  uint32_t capture_count;
  pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &capture_count);

  ant_value_t result_arr = js_mkarr(js);
  if (is_err(result_arr)) {
    pcre2_match_data_free(match_data);
    pcre2_code_free(re);
    return result_arr;
  }

  PCRE2_SIZE pos = 0;
  int match_count = 0;

  while (pos <= (PCRE2_SIZE)args.str_len) {
    int rc = pcre2_match(
      re, (PCRE2_SPTR)args.str_ptr, args.str_len, pos, 0,
      match_data, regex_get_match_context(js)
    );
    if (rc < 0) break;

    PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
    PCRE2_SIZE match_start = ovector[0];
    PCRE2_SIZE match_end = ovector[1];

    if (args.global) {
      ant_value_t match_str = js_mkstr(js, args.str_ptr + match_start, match_end - match_start);
      if (is_err(match_str)) {
        pcre2_match_data_free(match_data);
        pcre2_code_free(re);
        return match_str;
      }
      js_arr_push(js, result_arr, match_str);
    } else {
      for (uint32_t i = 0; i <= capture_count; i++) {
        PCRE2_SIZE start = ovector[2*i];
        PCRE2_SIZE end = ovector[2*i+1];
        if (start == PCRE2_UNSET) {
          js_arr_push(js, result_arr, js_mkundef());
        } else {
          ant_value_t match_str = js_mkstr(js, args.str_ptr + start, end - start);
          if (is_err(match_str)) {
            pcre2_match_data_free(match_data);
            pcre2_code_free(re);
            return match_str;
          }
          js_arr_push(js, result_arr, match_str);
        }
      }
      js_setprop(js, result_arr, js_mkstr(js, "index", 5), tov((double)match_start));
    }
    match_count++;

    if (!args.global) break;
    if (match_start == match_end) {
      pos = match_end + 1;
    } else { pos = match_end; }
  }

  pcre2_match_data_free(match_data);
  pcre2_code_free(re);

  if (match_count == 0) return js_mknull();
  return result_arr;
}

static bool str_buf_append(char **buf, size_t *len, size_t *cap, const char *data, size_t n) {
  if (n == 0) return true;
  if (*len + n >= *cap) {
    size_t new_cap = (*len + n + 1) * 2;
    char *nb = (char *)ant_realloc(*buf, new_cap);
    if (!nb) return false;
    *buf = nb;
    *cap = new_cap;
  }
  memcpy(*buf + *len, data, n);
  *len += n;
  return true;
}

static inline ant_value_t emit_str_replacement(
  ant_t *js, ant_value_t replacement, bool is_func,
  const char *repl_ptr, ant_offset_t repl_len,
  const char *str_ptr, ant_value_t str,
  ant_offset_t pos, ant_offset_t match_len,
  char **buf, size_t *buf_len, size_t *buf_cap
) {
  if (is_func) {
    ant_value_t cb_args[3] = { js_mkstr(js, str_ptr + pos, match_len), tov((double)pos), str };
    ant_value_t r = sv_vm_call(js->vm, js, replacement, js_mkundef(), cb_args, 3, NULL, false);
    
    if (vtype(r) == T_ERR) return r;
    ant_value_t r_str = js_tostring_val(js, r);
    
    if (is_err(r_str)) return r_str;
    ant_offset_t rlen, roff = vstr(js, r_str, &rlen);
    
    if (!str_buf_append(buf, buf_len, buf_cap, (const char *)(uintptr_t)roff, rlen)) return js_mkerr(js, "oom");
  } else if (!str_buf_append(buf, buf_len, buf_cap, repl_ptr, repl_len)) return js_mkerr(js, "oom");
  
  return js_mkundef();
}

static ant_value_t string_replace_impl(ant_t *js, ant_value_t *args, int nargs, bool replace_all) {
  ant_value_t this_unwrapped = unwrap_primitive(js, js->this_val);
  ant_value_t str = js_tostring_val(js, this_unwrapped);
  
  if (is_err(str)) return str;
  if (nargs < 1) return str;

  if (is_object_type(args[0])) {
    if (replace_all) {
      ant_value_t global_val = js_getprop_fallback(js, args[0], "global");
      if (!js_truthy(js, global_val)) return js_mkerr_typed(js, JS_ERR_TYPE, "String.prototype.replaceAll called with a non-global RegExp");
    }
    
    bool called = false;
    ant_value_t replacement_arg = nargs > 1 ? args[1] : js_mkundef();
    ant_value_t call_args[2] = { str, replacement_arg };
    
    ant_value_t result = maybe_call_symbol_method(js, args[0], get_replace_sym(), args[0], call_args, 2, &called);
    if (is_err(result)) return result;
    if (called) return result;
    
    ant_value_t coerced_search = js_to_primitive(js, args[0], 1);
    if (is_err(coerced_search)) return coerced_search;
    
    coerced_search = js_tostring_val(js, coerced_search);
    if (is_err(coerced_search)) return coerced_search;
    
    args[0] = coerced_search;
  }
  
  if (nargs < 2) return str;
  ant_value_t search = args[0];
  ant_value_t replacement = args[1];
  if (vtype(search) != T_STR) return str;

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  ant_offset_t search_len, search_off = vstr(js, search, &search_len);
  const char *search_ptr = (char *)(uintptr_t)(search_off);

  bool is_func = (vtype(replacement) == T_FUNC);
  ant_offset_t repl_len = 0;
  const char *repl_ptr = NULL;
  
  if (!is_func) {
    if (vtype(replacement) != T_STR) return str;
    ant_offset_t repl_off = vstr(js, replacement, &repl_len);
    repl_ptr = (char *)(uintptr_t)(repl_off);
  }

  if (!replace_all) {
    if (search_len > str_len) return str;
    ant_offset_t match_pos = 0;
    bool found = false;
    
    for (ant_offset_t i = 0; i <= str_len - search_len; i++)
      if (memcmp(str_ptr + i, search_ptr, search_len) == 0) { 
        match_pos = i; found = true; break;
      }
      
    if (!found) return str;

    size_t cap = str_len + repl_len + 256, len = 0;
    char *buf = (char *)ant_calloc(cap);
    if (!buf) return js_mkerr(js, "oom");
    
    if (!str_buf_append(&buf, &len, &cap, str_ptr, match_pos)) { 
      free(buf);
      return js_mkerr(js, "oom");
    }
    
    ant_value_t err = emit_str_replacement(
      js, replacement, is_func, repl_ptr, 
      repl_len, str_ptr, str, match_pos, 
      search_len, &buf, &len, &cap
    );
    
    if (vtype(err) == T_ERR) { 
      free(buf);
      return err;
    }
    
    if (!str_buf_append(
      &buf, &len, &cap, str_ptr + match_pos + search_len, 
      str_len - match_pos - search_len)
    ) { 
      free(buf);
      return js_mkerr(js, "oom");
    }
    
    ant_value_t ret = js_mkstr(js, buf, len);
    free(buf);
    
    return ret;
  } else {
    size_t cap = str_len + repl_len + 256, len = 0;
    char *buf = (char *)ant_calloc(cap);
    if (!buf) return js_mkerr(js, "oom");
    
    ant_offset_t pos = 0;
    bool replaced = false;
    
    while (pos + (ant_offset_t)search_len <= str_len) {
      if (search_len == 0 || memcmp(str_ptr + pos, search_ptr, search_len) == 0) {
        replaced = true;
        ant_value_t err = emit_str_replacement(js, replacement, is_func, repl_ptr, repl_len, str_ptr, str, pos, search_len, &buf, &len, &cap);
        if (vtype(err) == T_ERR) { free(buf); return err; }
        if (search_len == 0) {
          if (pos < str_len && !str_buf_append(&buf, &len, &cap, str_ptr + pos, 1)) { free(buf); return js_mkerr(js, "oom"); }
          pos++;
        } else pos += search_len;
      } else {
        if (!str_buf_append(&buf, &len, &cap, str_ptr + pos, 1)) { free(buf); return js_mkerr(js, "oom"); }
        pos++;
      }
    }
    
    if (!str_buf_append(
      &buf, &len, &cap, str_ptr + pos, 
      str_len - pos)
    ) {
      free(buf);
      return js_mkerr(js, "oom");
    }
    
    if (!replaced) { 
      free(buf);
      return str;
    }
    
    ant_value_t ret = js_mkstr(js, buf, len);
    free(buf);
    
    return ret;
  }
}

static ant_value_t builtin_string_replace(ant_t *js, ant_value_t *args, int nargs) {
  return string_replace_impl(js, args, nargs, false);
}

static ant_value_t builtin_string_replaceAll(ant_t *js, ant_value_t *args, int nargs) {
  return string_replace_impl(js, args, nargs, true);
}

static bool regexp_literal_replace_builtin_guard(ant_t *js) {
  ant_regex_state_t *state = js->regex_state;
  if (
    !state ||
    state->replace_property_written ||
    state->exec_property_written
  ) return false;

  ant_value_t string_ctor = js_getprop_fallback(js, js_glob(js), "String");
  if (is_err(string_ctor)) return false;
  ant_value_t string_proto = js_getprop_fallback(js, string_ctor, "prototype");
  if (is_err(string_proto) || !is_object_type(string_proto)) return false;
  ant_value_t replace_fn = js_getprop_fallback(js, string_proto, "replace");
  if (is_err(replace_fn) || !js_cfunc_same_entrypoint(replace_fn, builtin_string_replace)) return false;

  ant_value_t regexp_ctor = js_getprop_fallback(js, js_glob(js), "RegExp");
  if (is_err(regexp_ctor)) return false;
  ant_value_t regexp_proto = js_getprop_fallback(js, regexp_ctor, "prototype");
  if (is_err(regexp_proto) || !is_object_type(regexp_proto)) return false;
  ant_value_t sym_replace = js_get_sym(js, regexp_proto, get_replace_sym());
  if (is_err(sym_replace) || !js_cfunc_same_entrypoint(sym_replace, builtin_regexp_symbol_replace)) return false;

  return true;
}

ant_value_t regexp_literal_replace_call(
  ant_t *js,
  ant_value_t str,
  ant_value_t pattern,
  ant_value_t flags,
  ant_value_t replacement
) {
  if (
    vtype(str) == T_STR &&
    vtype(replacement) == T_STR &&
    regexp_literal_replace_builtin_guard(js) &&
    !replacement_has_substitution(js, replacement)
  ) {
    const char *needle;
    ant_offset_t needle_len;
    if (regexp_literal_exec_fast_parts(js, pattern, flags, &needle, &needle_len)) {
      ant_offset_t flags_len, flags_off = vstr(js, flags, &flags_len);
      uint8_t flags_mask = regexp_parse_flags_mask((const char *)(uintptr_t)flags_off, flags_len);
      bool global = (flags_mask & REGEXP_FLAG_GLOBAL) != 0;

      ant_offset_t str_len, str_off = vstr(js, str, &str_len);
      const char *str_ptr = (const char *)(uintptr_t)str_off;
      ant_offset_t repl_len, repl_off = vstr(js, replacement, &repl_len);
      const char *repl_ptr = (const char *)(uintptr_t)repl_off;
      const char *first = find_bytes(str_ptr, str_len, needle, needle_len);
      if (!first) return str;

      size_t cap = str_len + repl_len + 256;
      char *buf = ant_calloc(cap);
      if (!buf) return js_mkerr(js, "oom");

      size_t len = 0;
      const char *scan = str_ptr;
      const char *end = str_ptr + str_len;
      const char *match = first;
      PCRE2_SIZE ovector[2];

      for (;;) {
        if (!str_buf_append(&buf, &len, &cap, scan, (size_t)(match - scan)) ||
            !str_buf_append(&buf, &len, &cap, repl_ptr, repl_len)) {
          free(buf);
          return js_mkerr(js, "oom");
        }

        ovector[0] = (PCRE2_SIZE)(match - str_ptr);
        ovector[1] = ovector[0] + (PCRE2_SIZE)needle_len;
        update_regexp_statics(js, str_ptr, ovector, 1);

        scan = match + needle_len;
        if (!global) break;
        match = find_bytes(scan, (ant_offset_t)(end - scan), needle, needle_len);
        if (!match) break;
      }

      if (!str_buf_append(&buf, &len, &cap, scan, (size_t)(end - scan))) {
        free(buf);
        return js_mkerr(js, "oom");
      }

      ant_value_t ret = js_mkstr(js, buf, len);
      free(buf);
      return ret;
    }
  }

  ant_value_t regexp_obj = regexp_literal_object(js, pattern, flags);
  if (is_err(regexp_obj)) return regexp_obj;
  ant_value_t replace_fn = js_getprop_fallback(js, str, "replace");
  if (is_err(replace_fn)) return replace_fn;
  ant_value_t call_args[2] = { regexp_obj, replacement };
  return sv_vm_call(js->vm, js, replace_fn, str, call_args, 2, NULL, false);
}

static ant_value_t builtin_string_search(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_unwrapped = unwrap_primitive(js, js->this_val);
  ant_value_t str = js_tostring_val(js, this_unwrapped);
  if (is_err(str)) return str;
  if (nargs < 1) return tov(-1);

  if (is_object_type(args[0])) {
    bool called = false;
    ant_value_t call_args[1] = { str };
    ant_value_t dispatched = maybe_call_symbol_method(
      js, args[0], get_search_sym(), args[0], call_args, 1, &called
    );
    if (is_err(dispatched)) return dispatched;
    if (called) return dispatched;
  }

  ant_value_t pattern = args[0];
  const char *pattern_ptr = NULL;
  ant_offset_t pattern_len = 0;

  if (vtype(pattern) == T_OBJ) {
    pattern = js_to_primitive(js, pattern, 1);
    if (is_err(pattern)) return pattern;
    pattern = js_tostring_val(js, pattern);
    if (is_err(pattern)) return pattern;
    goto search_string_pattern;
  } else if (vtype(pattern) == T_STR) {
search_string_pattern:;
    ant_offset_t poff;
    poff = vstr(js, pattern, &pattern_len);
    pattern_ptr = (char *)(uintptr_t)(poff);
  } else return tov(-1);

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);

  char pcre2_pattern[4096];
  size_t pcre2_len = js_to_pcre2_pattern(pattern_ptr, pattern_len, pcre2_pattern, sizeof(pcre2_pattern), false);
  uint32_t options = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_UNSET_BACKREF | PCRE2_DUPNAMES;

  int errcode;
  PCRE2_SIZE erroffset;
  pcre2_code *re = pcre2_compile((PCRE2_SPTR)pcre2_pattern, pcre2_len, options, &errcode, &erroffset, NULL);
  if (re == NULL) return tov(-1);

  pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
  int rc = pcre2_match(
    re, (PCRE2_SPTR)str_ptr, str_len, 0, 0,
    match_data, regex_get_match_context(js)
  );

  if (rc < 0) {
    pcre2_match_data_free(match_data);
    pcre2_code_free(re);
    return tov(-1);
  }

  PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
  double result = (double)ovector[0];

  pcre2_match_data_free(match_data);
  pcre2_code_free(re);

  return tov(result);
}

static ant_value_t builtin_string_match(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_unwrapped = unwrap_primitive(js, js->this_val);
  ant_value_t str = js_tostring_val(js, this_unwrapped);
  if (is_err(str)) return str;
  if (nargs < 1) return js_mknull();

  if (is_object_type(args[0])) {
    bool called = false;
    ant_value_t call_args[1] = { str };
    ant_value_t dispatched = maybe_call_symbol_method(
      js, args[0], get_match_sym(), args[0], call_args, 1, &called
    );
    if (is_err(dispatched)) return dispatched;
    if (called) return dispatched;
  }

  ant_value_t pattern = args[0];
  const char *pattern_ptr = NULL;
  ant_offset_t pattern_len = 0;
  
  const bool 
    global_flag = false, 
    ignore_case = false, 
    multiline = false;

  if (vtype(pattern) == T_OBJ) {
    pattern = js_to_primitive(js, pattern, 1);
    if (is_err(pattern)) return pattern;
    pattern = js_tostring_val(js, pattern);
    if (is_err(pattern)) return pattern;
    goto match_string_pattern;
  } else if (vtype(pattern) == T_STR) {
match_string_pattern:;
    ant_offset_t poff;
    poff = vstr(js, pattern, &pattern_len);
    pattern_ptr = (char *)(uintptr_t)(poff);
  } else return js_mknull();

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);

  ant_value_t result = do_regex_match_pcre2(js, (regex_match_args_t){
    .pattern_ptr = pattern_ptr, .pattern_len = pattern_len,
    .str_ptr = str_ptr, .str_len = str_len,
    .global = global_flag, .ignore_case = ignore_case, .multiline = multiline,
  });

  if (!global_flag && vtype(result) == T_ARR) {
    js_setprop(js, result, js_mkstr(js, "input", 5), str);
  }

  return result;
}

void init_regex_module(ant_t *js) {
  rxstat_init();
  if (!js->regex_state)
    js->regex_state = calloc(1, sizeof(*js->regex_state));
  ant_regex_state_t *state = js->regex_state;
  ant_value_t glob = js->global;
  ant_value_t object_proto = js->sym.object_proto;

  ant_value_t regexp_proto = js_mkobj(js);
  js_set_proto_init(regexp_proto, object_proto);
  
  defmethod(js, regexp_proto, "test", 4, js_mkfun(builtin_regexp_test));
  defmethod(js, regexp_proto, "exec", 4, js_mkfun(builtin_regexp_exec));
  defmethod(js, regexp_proto, "toString", 8, js_mkfun(builtin_regexp_toString));

  js_mkprop_fast(js, regexp_proto, "global", 6, js_false);
  js_mkprop_fast(js, regexp_proto, "ignoreCase", 10, js_false);
  js_mkprop_fast(js, regexp_proto, "multiline", 9, js_false);
  js_mkprop_fast(js, regexp_proto, "dotAll", 6, js_false);
  js_mkprop_fast(js, regexp_proto, "unicode", 7, js_false);
  js_mkprop_fast(js, regexp_proto, "sticky", 6, js_false);
  js_mkprop_fast(js, regexp_proto, "hasIndices", 10, js_false);
  js_mkprop_fast(js, regexp_proto, "unicodeSets", 11, js_false);

  js_set_sym(js, regexp_proto, get_split_sym(), js_mkfun(builtin_regexp_symbol_split));
  js_set_sym(js, regexp_proto, get_match_sym(), js_mkfun(builtin_regexp_symbol_match));
  js_set_sym(js, regexp_proto, get_matchAll_sym(), js_mkfun(builtin_regexp_symbol_matchAll));

  js->builtins.regexp_matchall_iter_proto_val = js_mkobj(js);
  js_set_proto_init(js->builtins.regexp_matchall_iter_proto_val, js->sym.iterator_proto);
  defmethod(js, js->builtins.regexp_matchall_iter_proto_val, "next", 4, js_mkfun(regexp_matchall_next));
  js_set_sym(js, js->builtins.regexp_matchall_iter_proto_val, get_iterator_sym(), js_mkfun(sym_this_cb));
  js_set_sym(js, regexp_proto, get_replace_sym(), js_mkfun(builtin_regexp_symbol_replace));
  js_set_sym(js, regexp_proto, get_search_sym(), js_mkfun(builtin_regexp_symbol_search));
  js_set_sym(js, regexp_proto, get_toStringTag_sym(), js_mkstr(js, "RegExp", 6));
  js_set_getter_desc(js, regexp_proto, "flags", 5, js_mkfun(builtin_regexp_flags_getter), JS_DESC_C);
  defmethod(js, regexp_proto, "compile", 7, js_mkfun(builtin_regexp_compile));

  ant_value_t regexp_ctor = js_mkobj(js);
  js_set_slot(regexp_ctor, SLOT_CFUNC, js_mkfun(builtin_RegExp));
  js_mkprop_fast(js, regexp_ctor, "prototype", 9, regexp_proto);
  js_mkprop_fast(js, regexp_ctor, "name", 4, js_mkstr(js, "RegExp", 6));
  js_set_descriptor(js, regexp_ctor, "name", 4, 0);
  js_define_species_getter(js, regexp_ctor);

  ant_value_t regexp_func = js_obj_to_func(js, regexp_ctor);
  js->builtins.regexp_ctor_value = regexp_func;
  js_setprop(js, regexp_proto, js_mkstr(js, "constructor", 11), regexp_func);
  js_set_descriptor(js, regexp_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);

  js_set(js, regexp_ctor, "escape", js_mkfun(builtin_regexp_escape));

  ant_value_t empty = js_mkstr_permanent(js, "", 0);
  js->builtins.regexp_empty_string = empty;
  for (size_t i = 0; i < sizeof(js->mutable_roots.regexp_static_values) / sizeof(js->mutable_roots.regexp_static_values[0]); i++)
    js->mutable_roots.regexp_static_values[i] = empty;

  static ant_cfunc_meta_t stat_getters[11];
  static ant_cfunc_meta_t stat_setters[11];
  ant_cfunc_t getter_fns[11] = {
    regexp_static_get_d1, regexp_static_get_d2, regexp_static_get_d3,
    regexp_static_get_d4, regexp_static_get_d5, regexp_static_get_d6,
    regexp_static_get_d7, regexp_static_get_d8, regexp_static_get_d9,
    regexp_static_get_last_match, regexp_static_get_amp
  };
  ant_cfunc_t setter_fns[11] = {
    regexp_static_set_d1, regexp_static_set_d2, regexp_static_set_d3,
    regexp_static_set_d4, regexp_static_set_d5, regexp_static_set_d6,
    regexp_static_set_d7, regexp_static_set_d8, regexp_static_set_d9,
    regexp_static_set_last_match, regexp_static_set_amp
  };
  for (int i = 1; i <= 9; i++) {
    char key[3] = {'$', (char)('0' + i), '\0'};
    stat_getters[i - 1] = (ant_cfunc_meta_t){ getter_fns[i - 1], NULL, 0, 0 };
    stat_setters[i - 1] = (ant_cfunc_meta_t){ setter_fns[i - 1], NULL, 1, 0 };
    js_set_accessor_desc(
      js, regexp_ctor, key, 2,
      js_mkfun_meta(&stat_getters[i - 1]),
      js_mkfun_meta(&stat_setters[i - 1]),
      JS_DESC_C
    );
  }
  
  stat_getters[9] = (ant_cfunc_meta_t){ getter_fns[9], NULL, 0, 0 };
  stat_setters[9] = (ant_cfunc_meta_t){ setter_fns[9], NULL, 1, 0 };
  js_set_accessor_desc(js, regexp_ctor, "lastMatch", 9, js_mkfun_meta(&stat_getters[9]), js_mkfun_meta(&stat_setters[9]), JS_DESC_C);
  stat_getters[10] = (ant_cfunc_meta_t){ getter_fns[10], NULL, 0, 0 };
  stat_setters[10] = (ant_cfunc_meta_t){ setter_fns[10], NULL, 1, 0 };
  js_set_accessor_desc(js, regexp_ctor, "$&", 2, js_mkfun_meta(&stat_getters[10]), js_mkfun_meta(&stat_setters[10]), JS_DESC_C);
  js_set(js, glob, "RegExp", regexp_func);

  ant_value_t string_ctor = js_get(js, glob, "String");
  ant_value_t string_proto = js_get(js, string_ctor, "prototype");
  
  defmethod(js, string_proto, "search", 6, js_mkfun(builtin_string_search));
  defmethod(js, string_proto, "match", 5, js_mkfun(builtin_string_match));
  defmethod(js, string_proto, "matchAll", 8, js_mkfun(builtin_string_matchAll));
  defmethod(js, string_proto, "replace", 7, js_mkfun(builtin_string_replace));
  defmethod(js, string_proto, "replaceAll", 10, js_mkfun(builtin_string_replaceAll));
  
  if (state) {
    state->exec_property_written = false;
    state->replace_property_written = false;
    state->exec_write_guard_armed = true;
  }
}

void gc_age_regex_cache(ant_t *js, bool minor) {
  ant_regex_state_t *state = js->regex_state;
  if (!state || minor) return;

  /* Generation 1 dies; generation 0 becomes generation 1. A hit in
     generation 1 is also inserted into generation 0, so frequently reused
     compiled data survives while cold entries lose their cache reference
     after two major collections. RegExp objects hold independent references. */
  compiled_regex_table_clear(&state->compiled[1]);
  state->compiled[1] = state->compiled[0];
  memset(&state->compiled[0], 0, sizeof(state->compiled[0]));
}

void cleanup_regex_module(ant_t *js) {
  ant_regex_state_t *state = js->regex_state;
  if (!state) return;

  compiled_regex_table_clear(&state->compiled[0]);
  compiled_regex_table_clear(&state->compiled[1]);

  ant_shape_release(state->result_shape.shape);
  ant_shape_release(state->result_indices_shape.shape);
  pcre2_jit_stack_free(state->jit_stack);
  pcre2_match_context_free(state->match_ctx);
  free(state);
  js->regex_state = NULL;
}
