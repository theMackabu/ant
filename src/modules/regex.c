// TODO: cleanup module, make cleaner. MODULARIZE.

#include "ant.h"
#include "cfunc.h"
#include "utf8.h"
#include "errors.h"
#include "internal.h"
#include "utils.h"
#include "ascii.h"
#include "descriptors.h"
#include "ptr.h"
#include "gc/roots.h"
#include "index.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>
#include <pcre2.h>

#include "silver/call.h"
#include "modules/regex.h"
#include "realm.h"
#include "protectors.h"
#include "modules/symbol.h"
#include "modules/iterator.h"

static constexpr size_t REGEXP_REQUIRED_LITERAL_MAX = 32;

typedef struct {
  const char *name;
  uint32_t length;
  uint32_t group;
} regexp_group_name_t;

typedef struct compiled_regex_cache_entry {
  char *pattern;
  size_t pattern_len;
  uint64_t key_hash;
  uint8_t flags_mask;
  pcre2_code *code;
  pcre2_code *line_code;
  pcre2_match_data *scratch_match_data;
  ant_shape_t *lastindex_shape;
  uint32_t lastindex_slot;
  uint32_t namecount;
  regexp_group_name_t *group_names;
  bool jit_ready;
  bool line_jit_ready;
  bool legacy_backrefs;
  uint8_t line_state;
  bool scratch_in_use;
  bool plain_literal;
  size_t object_refs;
  size_t active_refs;
  uint8_t cache_refs;
  uint8_t required_literal_len;
  char required_literal[REGEXP_REQUIRED_LITERAL_MAX];
} compiled_regex_cache_entry_t;

enum {
  REGEXP_LINE_NONE,
  REGEXP_LINE_PENDING,
  REGEXP_LINE_READY,
  REGEXP_LINE_FAILED,
};

typedef struct {
  compiled_regex_cache_entry_t **slots;
  size_t count;
  size_t cap;
} compiled_regex_cache_table_t;

typedef struct {
  compiled_regex_cache_entry_t *compiled;
  pcre2_match_data *match_data;
  PCRE2_SIZE literal[2];
  bool borrowed;
  bool line_checked;
  bool line_code;
} regex_match_scope_t;

typedef struct {
  ant_shape_t *shape;
  uint32_t index_slot;
  uint32_t input_slot;
  uint32_t groups_slot;
  uint32_t indices_slot;
} regexp_result_shape_t;

typedef struct {
  ant_value_t array;
  ant_value_t index;
  ant_value_t input;
  ant_value_t groups_meta;
  ant_value_t indices;
  bool with_indices;
} regexp_exec_result_ctx_t;

enum {
#define REGEXP_FLAG(id, name, ch) REGEXP_FLAG_INDEX_##id,
#include "modules/regex_flags.h"
  REGEXP_FLAG_COUNT
};

enum {
#define REGEXP_FLAG(id, name, ch) REGEXP_FLAG_##id = 1 << REGEXP_FLAG_INDEX_##id,
#include "modules/regex_flags.h"
};

static constexpr uint8_t REGEXP_PLAIN_LITERAL_EXCLUDED_FLAGS =
  REGEXP_FLAG_IGNORE_CASE | REGEXP_FLAG_STICKY | REGEXP_FLAG_UNICODE_SET;

static constexpr int REGEXP_INLINE_CAPTURES = 32;
static constexpr ant_offset_t REGEXP_PLAIN_LITERAL_MAX = 32;

enum {
#define REGEXP_STATIC(id, name, alias) REGEXP_STATIC_INDEX_##id,
#include "modules/regex_statics.h"
  REGEXP_STATIC_VALUE_COUNT
};

static_assert(sizeof(PCRE2_SIZE) == sizeof(size_t), "realm ovector element");
static_assert(
  sizeof(((ant_t *)0)->mutable_roots.regexp_static_values) / sizeof(ant_value_t) == REGEXP_STATIC_VALUE_COUNT,
  "regexp_static_values must hold every RegExp legacy static"
);

typedef struct {
  ant_shape_t *base;
  ant_shape_t *shape;
} regexp_instance_shape_t;

typedef struct {
  ant_value_t pattern;
  ant_value_t flags;
  uint64_t epoch;
  compiled_regex_cache_entry_t *entry;
} regexp_site_entry_t;

typedef struct {
  ant_value_t pattern;
  uint64_t epoch;
  uint8_t flags_mask;
} regexp_validated_entry_t;

static constexpr size_t REGEXP_SITE_CACHE_SIZE = 64;

struct ant_regex_state {
  compiled_regex_cache_table_t compiled[2];
  regexp_site_entry_t sites[REGEXP_SITE_CACHE_SIZE];
  regexp_validated_entry_t validated[REGEXP_SITE_CACHE_SIZE];
  size_t live_object_data;

  regexp_instance_shape_t instance_shape;
  const ant_shape_t *pristine_flags_shape;
  const ant_realm_t *pristine_flags_realm;
  uint32_t pristine_flags_epoch;
  regexp_result_shape_t result_shape;
  regexp_result_shape_t result_indices_shape;

  pcre2_match_context *match_ctx;
  pcre2_jit_stack *jit_stack;
  ant_value_t flag_strings[256];

  char *out_buf;
  size_t out_cap;
  bool out_busy;
};

static constexpr uint32_t REGEXP_NATIVE_TAG = Ant_NativeTag("REGX");
static constexpr uint16_t REGEXP_STATIC_ALL_MATERIALIZED = (1u << REGEXP_STATIC_VALUE_COUNT) - 1;

static constexpr size_t REGEX_COMPILED_CACHE_MAX = 1024;
static constexpr size_t REGEXP_OUT_SCRATCH_MAX = 1024 * 1024;
static constexpr size_t REGEX_COMPILED_TABLE_LOAD_NUM = 3;
static constexpr size_t REGEX_COMPILED_TABLE_LOAD_DEN = 4;

static bool regexp_result_shape_init(
  ant_t *js, regexp_result_shape_t *cache,
  ant_object_t *array,
  bool with_indices
) {
  if (!array || !array->shape) return false;
  if (cache->shape && ant_shape_proto(cache->shape) == array->proto) return true;
  if (ant_shape_proto(array->shape) != array->proto) return false;

  const char *index_key = intern_string(js, "index", 5);
  const char *input_key = intern_string(js, "input", 5);
  const char *groups_key = intern_string(js, "groups", 6);
  const char *indices_key = with_indices
    ? intern_string(js, "indices", 7)
    : NULL;
  if (!index_key || !input_key || !groups_key || (with_indices && !indices_key))
    return false;

  ant_shape_t *shape = ant_shape_clone(array->shape);
  if (!shape) return false;

  if (cache->shape) ant_shape_release(cache->shape);
  *cache = (regexp_result_shape_t){0};
  regexp_result_shape_t built = { .shape = shape };
  bool ok =
    ant_shape_add_interned(js, shape, index_key, ANT_PROP_ATTR_DEFAULT, &built.index_slot) &&
    ant_shape_add_interned(js, shape, input_key, ANT_PROP_ATTR_DEFAULT, &built.input_slot) &&
    ant_shape_add_interned(js, shape, groups_key, ANT_PROP_ATTR_DEFAULT, &built.groups_slot);
  
  if (ok && with_indices) 
    ok = ant_shape_add_interned(js, shape, indices_key, ANT_PROP_ATTR_DEFAULT, &built.indices_slot);
  
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
  if (!state || !array || array->type_tag != kTypeArray) goto fallback;

  regexp_result_shape_t *cache = with_indices
    ? &state->result_indices_shape
    : &state->result_shape;
  if (!regexp_result_shape_init(js, cache, array, with_indices)) goto fallback;

  if (array->shape != cache->shape) {
    ant_shape_retain(cache->shape);
    ant_shape_release(array->shape);
    array->shape = cache->shape;
  }
  
  if (!js_obj_ensure_prop_capacity(js, array, with_indices ? 4 : 3)) goto fallback;

  ant_object_prop_set_unchecked(array, cache->index_slot, index);
  ant_object_prop_set_unchecked(array, cache->input_slot, input);
  ant_object_prop_set_unchecked(array, cache->groups_slot, groups);
  
  gc_write_barrier(js, array, input);
  gc_write_barrier(js, array, groups);
  
  if (with_indices) {
    ant_object_prop_set_unchecked(array, cache->indices_slot, indices);
    gc_write_barrier(js, array, indices);
  }
  
  return true;

fallback:
  return false;
}

void regexp_note_replace_property_write(ant_t *js) {
  if (js->regexp.write_guard_armed) js->regexp.replace_written = true;
  if (__builtin_expect(!ant_realm_multiple(js), 1)) return;

  for (ant_realm_t *realm = js->realms; realm; realm = realm->next)
    if (realm != js->realm && realm->regexp.write_guard_armed) realm->regexp.replace_written = true;
}

static inline uint8_t regexp_parse_flags_mask(const char *fstr, ant_offset_t flen) {
  uint8_t mask = 0;
  for (ant_offset_t k = 0; k < flen; k++) {
  switch (fstr[k]) {
#define REGEXP_FLAG(id, name, ch) case ch: mask |= REGEXP_FLAG_##id; break;
#include "modules/regex_flags.h"
    default: break;
  }}
  return mask;
}

static inline uint8_t regexp_flags_mask(
  ant_t *js,
  ant_value_t regexp,
  compiled_regex_cache_entry_t **compiled_out
) {
  compiled_regex_cache_entry_t *compiled = js_get_native(
    regexp, REGEXP_NATIVE_TAG
  );
  if (compiled_out) *compiled_out = compiled;
  if (compiled) {
    return compiled->flags_mask;
  }

  ant_value_t internal_mask = js_get_slot(regexp, SLOT_REGEXP_FLAGS_MASK);
  return vtype(internal_mask) == kTypeNumber ? (uint8_t)tod(internal_mask) : 0;
}

static int regexp_group_name_cmp(const void *a, const void *b) {
  const regexp_group_name_t *x = a, *y = b;
  return (x->group > y->group) - (x->group < y->group);
}

static regexp_group_name_t *regexp_collect_group_names(pcre2_code *code, uint32_t namecount) {
  uint32_t nameentrysize = 0;
  PCRE2_SPTR nametable = NULL;
  pcre2_pattern_info(code, PCRE2_INFO_NAMEENTRYSIZE, &nameentrysize);
  pcre2_pattern_info(code, PCRE2_INFO_NAMETABLE, (void *)&nametable);

  regexp_group_name_t *names = malloc(sizeof(*names) * namecount);
  if (!names) return NULL;
  for (uint32_t i = 0; i < namecount; i++) {
    PCRE2_SPTR entry = nametable + (size_t)i * nameentrysize;
    const char *name = (const char *)(entry + 2);
    names[i] = (regexp_group_name_t){
      .name = name, .length = (uint32_t)strlen(name), .group = (uint32_t)((entry[0] << 8) | entry[1]),
    };
  }
  qsort(names, namecount, sizeof(*names), regexp_group_name_cmp);
  return names;
}

static ant_value_t regexp_build_named_groups_meta(ant_t *js, const compiled_regex_cache_entry_t *compiled) {
  if (compiled->namecount == 0 || !compiled->group_names) return js_mkundef();

  ant_value_t meta = js_mkarr(js);
  if (is_err(meta)) return meta;

  for (uint32_t i = 0; i < compiled->namecount; i++) {
    const regexp_group_name_t *entry = &compiled->group_names[i];
    ant_value_t name_val = js_mkstr(js, entry->name, entry->length);
    if (is_err(name_val)) return name_val;
    js_arr_push(js, meta, name_val);
    js_arr_push(js, meta, tov((double)entry->group));
  }

  return meta;
}

static inline ant_value_t regexp_static_empty(ant_t *js) {
  return js->builtins.regexp_empty_string
    ? js->builtins.regexp_empty_string
    : js_mkundef();
}

static inline void regexp_statics_clear_materialized(ant_t *js) {
  ant_regexp_realm_t *statics = &js->regexp;
  uint16_t mask = statics->static_materialized_mask;
  if (__builtin_expect(mask == 0, 1)) return;

  ant_value_t empty = regexp_static_empty(js);
  while (mask) {
    unsigned idx = (unsigned)__builtin_ctz((unsigned)mask);
    js->mutable_roots.regexp_static_values[idx] = empty;
    mask &= (uint16_t)(mask - 1);
  }
  statics->static_materialized_mask = 0;
}

static ant_value_t regexp_static_compute(
  ant_t *js, size_t idx, ant_value_t subject,
  const PCRE2_SIZE *ovector, uint32_t last_group, const PCRE2_SIZE *paren
) {
  if (idx == REGEXP_STATIC_INDEX_INPUT) return subject;

  ant_offset_t subject_len;
  const char *subject_ptr = (const char *)(uintptr_t)vstr(js, subject, &subject_len);
  PCRE2_SIZE start = PCRE2_UNSET, end = PCRE2_UNSET;
  bool matched = ovector[0] != PCRE2_UNSET;

  switch (idx) {
    case REGEXP_STATIC_INDEX_LAST_MATCH: if (matched) start = ovector[0], end = ovector[1]; break;
    case REGEXP_STATIC_INDEX_LAST_PAREN:
      if (last_group >= ANT_REGEXP_STATIC_OVECTOR_COUNT) start = paren[0], end = paren[1];
      else if (last_group) start = ovector[2 * last_group], end = ovector[2 * last_group + 1];
      break;
    case REGEXP_STATIC_INDEX_LEFT_CONTEXT: if (matched) start = 0, end = ovector[0]; break;
    case REGEXP_STATIC_INDEX_RIGHT_CONTEXT: if (matched) start = ovector[1], end = (PCRE2_SIZE)subject_len; break;
    default: {
      size_t capture = idx - REGEXP_STATIC_INDEX_D1 + 1;
      if (capture <= last_group) start = ovector[2 * capture], end = ovector[2 * capture + 1];
    }
  }

  if (start == PCRE2_UNSET) return regexp_static_empty(js);
  if (end < start || end > (PCRE2_SIZE)subject_len)
    return js_mkerr(js, "invalid regexp static snapshot");
  return js_mkstr_byte_range(js, subject_ptr, start, end - start);
}

static ant_value_t regexp_static_value(ant_t *js, size_t idx) {
  ant_regexp_realm_t *statics = &js->regexp;
  uint16_t bit = (uint16_t)(1u << idx);
  if (!statics->static_pending || (statics->static_materialized_mask & bit)) {
    ant_value_t value = js->mutable_roots.regexp_static_values[idx];
    return value ? value : regexp_static_empty(js);
  }

  ant_value_t value = regexp_static_compute(
    js, idx, js->mutable_roots.regexp_static_subject,
    js->regexp.static_ovector, js->regexp.static_last_group, js->regexp.static_paren
  );
  if (is_err(value)) return value;

  js->mutable_roots.regexp_static_values[idx] = value;
  js->regexp.static_materialized_mask |= bit;
  if (js->regexp.static_materialized_mask == REGEXP_STATIC_ALL_MATERIALIZED) {
    js->regexp.static_pending = false;
    js->mutable_roots.regexp_static_subject = regexp_static_empty(js);
  }
  return value;
}

static ant_value_t regexp_static_store(ant_t *js, size_t idx, ant_value_t value) {
  if (idx != REGEXP_STATIC_INDEX_INPUT) return js_mkundef();
  ant_value_t input = js_template_to_string(js, value);
  if (is_err(input)) return input;

  js->mutable_roots.regexp_static_values[idx] = input;
  js->regexp.static_materialized_mask |= (uint16_t)(1u << idx);
  return js_mkundef();
}

#define REGEXP_STATIC(id, name, alias) \
  static ant_value_t regexp_static_get_##id(ant_params_t) { \
    return regexp_static_value(js, REGEXP_STATIC_INDEX_##id); \
  } \
  static ant_value_t regexp_static_set_##id(ant_params_t) { \
    return regexp_static_store(js, REGEXP_STATIC_INDEX_##id, nargs > 0 ? args[0] : js_mkundef()); \
  }
#include "modules/regex_statics.h"

static __attribute__((noinline, cold)) void update_regexp_statics_eager(
  ant_t *js, ant_value_t subject,
  PCRE2_SIZE *ovector, uint32_t ovcount
) {
  PCRE2_SIZE paren[2] = { PCRE2_UNSET, PCRE2_UNSET };
  if (ovcount > ANT_REGEXP_STATIC_OVECTOR_COUNT) {
    paren[0] = ovector[2 * (ovcount - 1)];
    paren[1] = ovector[2 * (ovcount - 1) + 1];
  }
  ant_value_t empty = regexp_static_empty(js);
  for (size_t idx = 0; idx < REGEXP_STATIC_VALUE_COUNT; idx++) {
    ant_value_t value = regexp_static_compute(js, idx, subject, ovector, ovcount - 1, paren);
    js->mutable_roots.regexp_static_values[idx] = is_err(value) ? empty : value;
  }
}

static inline void update_regexp_statics(
  ant_t *js,
  ant_value_t subject,
  PCRE2_SIZE *ovector,
  uint32_t ovcount
) {
  ant_regex_state_t *state = js->regex_state;
  if (!state) {
    update_regexp_statics_eager(js, subject, ovector, ovcount);
    return;
  }

  regexp_statics_clear_materialized(js);
  js->mutable_roots.regexp_static_subject = subject;
  js->regexp.static_ovector[0] = ovector[0];
  js->regexp.static_ovector[1] = ovector[1];
  js->regexp.static_last_group = ovcount - 1;
  if (ovcount > 1) {
    uint32_t stored = ovcount < ANT_REGEXP_STATIC_OVECTOR_COUNT ? ovcount : ANT_REGEXP_STATIC_OVECTOR_COUNT;
    memcpy(js->regexp.static_ovector + 2, ovector + 2, (size_t)(stored - 1) * 2 * sizeof(*ovector));
    if (__builtin_expect(ovcount > ANT_REGEXP_STATIC_OVECTOR_COUNT, 0)) {
      js->regexp.static_paren[0] = ovector[2 * (ovcount - 1)];
      js->regexp.static_paren[1] = ovector[2 * (ovcount - 1) + 1];
    }
  }
  js->regexp.static_pending = true;
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

static const char regexp_space_ranges[] =
  "\\x{9}-\\x{d}\\x{20}\\x{a0}\\x{1680}\\x{2000}-\\x{200a}"
  "\\x{2028}\\x{2029}\\x{202f}\\x{205f}\\x{3000}\\x{feff}";

static const char regexp_non_space_ranges[] =
  "\\x{0}-\\x{8}\\x{e}-\\x{1f}\\x{21}-\\x{9f}\\x{a1}-\\x{167f}"
  "\\x{1681}-\\x{1fff}\\x{200b}-\\x{2027}\\x{202a}-\\x{202e}"
  "\\x{2030}-\\x{205e}\\x{2060}-\\x{2fff}\\x{3001}-\\x{fefe}"
  "\\x{ff00}-\\x{10ffff}";

static const char regexp_folded_word_ranges[] = "\\w\\x{17f}\\x{212a}";

static const char regexp_folded_non_word_ranges[] =
  "\\x{0}-\\x{2f}\\x{3a}-\\x{40}\\x{5b}-\\x{5e}\\x{60}"
  "\\x{7b}-\\x{17e}\\x{180}-\\x{2129}\\x{212b}-\\x{10ffff}";

static const char regexp_folded_boundary[] =
  "(?:(?<=[\\w\\x{17f}\\x{212a}])(?![\\w\\x{17f}\\x{212a}])"
  "|(?<![\\w\\x{17f}\\x{212a}])(?=[\\w\\x{17f}\\x{212a}]))";

static const char regexp_folded_non_boundary[] =
  "(?:(?<=[\\w\\x{17f}\\x{212a}])(?=[\\w\\x{17f}\\x{212a}])"
  "|(?<![\\w\\x{17f}\\x{212a}])(?![\\w\\x{17f}\\x{212a}]))";

static size_t v_close_bracket(const char *src, size_t src_len, size_t open) {
  int depth = 0;
  for (size_t i = open; i < src_len; i++) {
    if (src[i] == '\\' && i + 1 < src_len) { i++; continue; }
    if (src[i] == '[') depth++;
    else if (src[i] == ']') { if (--depth == 0) return i; }
  }
  return src_len;
}

typedef struct {
  char *data;
  size_t capacity;
  size_t length;
  bool plain_line_anchors;
  bool line_anchors;
} regexp_pattern_builder_t;

static inline void regexp_pattern_builder_puts(
  regexp_pattern_builder_t *builder,
  const char *value,
  size_t length
) {
  size_t offset = builder->length;
  size_t available = offset < builder->capacity
    ? builder->capacity - offset
    : 0;
  size_t copy_length = length < available ? length : available;
  if (copy_length) memcpy(builder->data + offset, value, copy_length);
  builder->length += length;
}

static inline void regexp_pattern_builder_put_any_char(
  regexp_pattern_builder_t *builder
) {
  static const char any_char[] = "[\\s\\S]";
  regexp_pattern_builder_puts(builder, any_char, sizeof(any_char) - 1);
}

static void js_to_pcre2_pattern_append(
  const char *src,
  size_t src_len,
  regexp_pattern_builder_t *builder,
  bool v_flag,
  uint8_t flags,
  int initial_charclass_depth
);

static void v_translate_part_append(
  const char *part,
  size_t length,
  regexp_pattern_builder_t *builder,
  uint8_t flags
) {
  if (length && part[0] == '[') {
    js_to_pcre2_pattern_append(part, length, builder, false, flags, 0);
    return;
  }

  if (length == 1 && part[0] == '^') {
    regexp_pattern_builder_put_any_char(builder);
    return;
  }

  if (length >= 2 && part[0] == '^' && part[1] == ']') {
    regexp_pattern_builder_put_any_char(builder);
    js_to_pcre2_pattern_append(part + 2, length - 2, builder, false, flags, 0);
    regexp_pattern_builder_puts(builder, "]", 1);
    return;
  }

  regexp_pattern_builder_puts(builder, "[", 1);
  js_to_pcre2_pattern_append(part, length, builder, false, flags, 1);
  regexp_pattern_builder_puts(builder, "]", 1);
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

static const struct {
  uint32_t cp;
  const char *utf8;
  const char *pcre2;
  uint8_t partners;
} regexp_legacy_caseless_fixes[] = {
  { 0xDF, "\xC3\x9F", "(?-i:\xC3\x9F)", 1 << 1 },
  { 0x1E9E, "\xE1\xBA\x9E", "(?-i:\xE1\xBA\x9E)", 1 << 0 },
  { 0x2126, "\xE2\x84\xA6", "(?-i:\xE2\x84\xA6)", 1 << 4 | 1 << 5 },
  { 0x212B, "\xE2\x84\xAB", "(?-i:\xE2\x84\xAB)", 1 << 6 | 1 << 7 },
  { 0x3C9, "\xCF\x89", "(?-i:[\xCF\x89\xCE\xA9])", 1 << 2 },
  { 0x3A9, "\xCE\xA9", "(?-i:[\xCF\x89\xCE\xA9])", 1 << 2 },
  { 0xE5, "\xC3\xA5", "(?-i:[\xC3\xA5\xC3\x85])", 1 << 3 },
  { 0xC5, "\xC3\x85", "(?-i:[\xC3\xA5\xC3\x85])", 1 << 3 },
};

static constexpr size_t REGEXP_LEGACY_CASELESS_FIX_COUNT =
  sizeof(regexp_legacy_caseless_fixes) / sizeof(regexp_legacy_caseless_fixes[0]);

static int regexp_legacy_caseless_index(uint32_t cp) {
  if (cp < 0xC5) return -1;
  for (size_t i = 0; i < REGEXP_LEGACY_CASELESS_FIX_COUNT; i++)
    if (regexp_legacy_caseless_fixes[i].cp == cp) return (int)i;
  return -1;
}

static bool regexp_hex_value(const char *src, size_t src_len, size_t at, size_t digits, uint32_t *out) {
  if (at + digits > src_len) return false;
  uint32_t value = 0;
  for (size_t k = 0; k < digits; k++) {
    char c = src[at + k];
    if (!ant_ascii_is_xdigit(c)) return false;
    value = value * 16 + (uint32_t)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
  }
  *out = value;
  return true;
}

static bool regexp_legacy_atom(const char *src, size_t src_len, size_t at, bool in_class, uint32_t *cp, size_t *next) {
  unsigned char c = (unsigned char)src[at];
  if (c != '\\') {
    utf8proc_int32_t decoded = c;
    utf8proc_ssize_t n = c < 0x80 ? 1 : utf8_next((const utf8proc_uint8_t *)src + at, (utf8proc_ssize_t)(src_len - at), &decoded);
    *cp = decoded < 0 ? c : (uint32_t)decoded;
    *next = at + (n > 0 ? (size_t)n : 1);
    return true;
  }
  if (at + 1 >= src_len) return false;
  unsigned char n = (unsigned char)src[at + 1];
  if (n >= 0x80) return regexp_legacy_atom(src, src_len, at + 1, in_class, cp, next);
  if (n == 'u' && regexp_hex_value(src, src_len, at + 2, 4, cp)) { *next = at + 6; return true; }
  if (n == 'x' && regexp_hex_value(src, src_len, at + 2, 2, cp)) { *next = at + 4; return true; }
  if (n == 'b' && !in_class) return false;
  const char *controls = strchr("tnvfrb", n);
  if (n && controls) {
    static const uint8_t control_values[] = { '\t', '\n', '\v', '\f', '\r', '\b' };
    *cp = control_values[controls - "tnvfrb"];
  } else if ((n | 0x20) >= 'a' && (n | 0x20) <= 'z') return false;
  else if (n >= '0' && n <= '9') return false;
  else *cp = n;
  *next = at + 2;
  return true;
}

static uint32_t regexp_legacy_canonicalize(uint32_t cp) {
  if (cp > 0xFFFF) return cp;
  utf8proc_int32_t upper[3];
  if (utf8_full_case_map(NULL, 0, 0, 0, (utf8proc_int32_t)cp, true, upper) != 1) return cp;
  if (upper[0] < 0 || upper[0] > 0xFFFF || (cp >= 128 && upper[0] < 128)) return cp;
  return (uint32_t)upper[0];
}

static int regexp_legacy_backref_callout(pcre2_callout_block *block, void *data) {
  const compiled_regex_cache_entry_t *compiled = data;
  uint32_t group = block->callout_number;
  if (block->callout_string) {
    group = 0;
    for (uint32_t i = 0; compiled && i < compiled->namecount && !group; i++) {
      const regexp_group_name_t *entry = &compiled->group_names[i];
      if (
        entry->length == block->callout_string_length &&
        memcmp(entry->name, block->callout_string, entry->length) == 0 &&
        entry->group < block->capture_top && block->offset_vector[2 * entry->group] != PCRE2_UNSET
      ) group = entry->group;
    }
  }
  if (!group || group >= block->capture_top) return 0;

  PCRE2_SIZE at = block->offset_vector[2 * group], end = block->offset_vector[2 * group + 1];
  if (at == PCRE2_UNSET) return 0;
  const utf8proc_uint8_t *subject = block->subject;
  PCRE2_SIZE pos = block->current_position, len = block->subject_length;
  while (at < end) {
    if (pos >= len) return 1;
    utf8proc_int32_t a, b;
    utf8proc_ssize_t na = utf8_next(subject + at, (utf8proc_ssize_t)(end - at), &a);
    utf8proc_ssize_t nb = utf8_next(subject + pos, (utf8proc_ssize_t)(len - pos), &b);
    if (na <= 0 || nb <= 0 || a < 0 || b < 0) {
      size_t width = (size_t)utf8_char_len_at((const char *)subject, (size_t)end, (size_t)at);
      if (pos + width > len || memcmp(subject + at, subject + pos, width) != 0) return 1;
      at += width;
      pos += width;
      continue;
    }
    if (a != b && regexp_legacy_canonicalize((uint32_t)a) != regexp_legacy_canonicalize((uint32_t)b)) return 1;
    at += (PCRE2_SIZE)na;
    pos += (PCRE2_SIZE)nb;
  }
  return 0;
}

static int regexp_callout_found(pcre2_callout_enumerate_block *block, void *data) {
  (void)block;
  *(bool *)data = true;
  return 1;
}

static uint8_t regexp_legacy_fixes_in_range(uint32_t lo, uint32_t hi) {
  uint8_t present = 0;
  for (size_t i = 0; i < REGEXP_LEGACY_CASELESS_FIX_COUNT; i++) {
    uint32_t cp = regexp_legacy_caseless_fixes[i].cp;
    if (cp >= lo && cp <= hi) present |= (uint8_t)(1u << i);
  }
  return present;
}

static uint8_t regexp_legacy_class_extras(const char *src, size_t src_len, size_t open, size_t *close) {
  uint8_t present = 0;
  size_t i = open + 1;
  if (i < src_len && src[i] == '^') i++;
  while (i < src_len && src[i] != ']') {
    uint32_t lo, hi;
    size_t after;
    if (!regexp_legacy_atom(src, src_len, i, true, &lo, &after)) {
      i += i + 1 < src_len ? 2 : 1;
      continue;
    }
    hi = lo;
    if (after + 1 < src_len && src[after] == '-' && src[after + 1] != ']') {
      size_t range_end;
      if (regexp_legacy_atom(src, src_len, after + 1, true, &hi, &range_end) && hi >= lo) after = range_end;
      else hi = lo;
    }
    present |= regexp_legacy_fixes_in_range(lo, hi);
    i = after;
  }
  *close = i;
  if (i >= src_len) return 0;
  uint8_t partners = 0;
  for (size_t fix = 0; fix < REGEXP_LEGACY_CASELESS_FIX_COUNT; fix++)
    if (present & (1u << fix)) partners |= regexp_legacy_caseless_fixes[fix].partners;
  return (uint8_t)(partners & ~present);
}

static size_t regexp_group_name_end(const char *src, size_t src_len, size_t open) {
  size_t end = open;
  while (end < src_len && src[end] != '>') end++;
  return end;
}

static bool regexp_source_has_group_names(const char *src, size_t src_len) {
  for (size_t i = 0; i + 3 < src_len; i++) {
    if (src[i] == '\\') { i++; continue; }
    if (src[i] == '(' && src[i + 1] == '?' && src[i + 2] == '<' && src[i + 3] != '=' && src[i + 3] != '!') return true;
  }
  return false;
}

static void js_to_pcre2_pattern_append(
  const char *src,
  size_t src_len,
  regexp_pattern_builder_t *builder,
  bool v_flag,
  uint8_t flags,
  int initial_charclass_depth
) {
  bool unicode_ignore_case = (flags & REGEXP_FLAG_IGNORE_CASE) && (flags & (REGEXP_FLAG_UNICODE | REGEXP_FLAG_UNICODE_SET));
  bool legacy_ignore_case = (flags & REGEXP_FLAG_IGNORE_CASE) && !(flags & (REGEXP_FLAG_UNICODE | REGEXP_FLAG_UNICODE_SET));
  size_t legacy_class_close = 0;
  int charclass_depth = initial_charclass_depth;
  char *output = builder->data;
  size_t output_capacity = builder->capacity;
  size_t output_length = builder->length;

#define OUT(ch) do { \
  size_t output_offset = output_length++; \
  if (output_offset < output_capacity) output[output_offset] = (ch); \
} while (0)

#define OUTS(str) do { \
  for (const char *output_str = (str); *output_str; output_str++) OUT(*output_str); \
} while (0)

  for (size_t si = 0; si < src_len; si++) {
    if (
      charclass_depth == 0 && src[si] == '(' && si + 3 < src_len && src[si + 1] == '?' &&
      src[si + 2] == '<' && src[si + 3] != '=' && src[si + 3] != '!'
    ) {
      size_t name_end = regexp_group_name_end(src, src_len, si + 3);
      for (; si < name_end; si++) OUT(src[si]);
      if (si < src_len) OUT(src[si]);
      continue;
    }

    if (src[si] == '[') {
      if (si + 2 < src_len && src[si + 1] == '^' && src[si + 2] == ']') {
        OUT('['); OUT('\\'); OUT('s'); OUT('\\'); OUT('S'); OUT(']');
        si += 2;
        continue;
      }

      if (charclass_depth == 0 && si + 1 < src_len && src[si + 1] == ']') {
        OUTS("[^\\s\\S]");
        si++;
        continue;
      }

      if (v_flag && charclass_depth == 0) {
        size_t close = v_close_bracket(src, src_len, si);
        size_t op_pos;
        int op_type = v_set_op(src, si + 1, close, &op_pos);
        if (op_type && close < src_len) {
          const char *left = &src[si + 1];
          size_t left_len = op_pos - si - 1;
          const char *right = &src[op_pos + 2];
          size_t right_len = close - op_pos - 2;
          const char *lookahead = op_type == 1 ? left : right;
          size_t lookahead_len = op_type == 1 ? left_len : right_len;
          const char *match = op_type == 1 ? right : left;
          size_t match_len = op_type == 1 ? right_len : left_len;
          OUT('('); OUT('?'); OUT(op_type == 1 ? '=' : '!');
          builder->length = output_length;
          v_translate_part_append(lookahead, lookahead_len, builder, flags);
          output_length = builder->length;
          OUT(')');
          builder->length = output_length;
          v_translate_part_append(match, match_len, builder, flags);
          output_length = builder->length;
          si = close;
          continue;
        }
      }
      if (legacy_ignore_case && charclass_depth == 0) {
        size_t close;
        uint8_t extras = regexp_legacy_class_extras(src, src_len, si, &close);
        if (extras) {
          bool negated = si + 1 < src_len && src[si + 1] == '^';
          OUTS(negated ? "(?:(?-i:[" : "(?:(?!(?-i:[");
          for (size_t i = 0; i < REGEXP_LEGACY_CASELESS_FIX_COUNT; i++)
            if (extras & (1u << i)) OUTS(regexp_legacy_caseless_fixes[i].utf8);
          OUTS(negated ? "])|" : "]))");
          legacy_class_close = close + 1;
        }
      }
      charclass_depth++;
      OUT('[');
      continue;
    }
    if (src[si] == ']' && charclass_depth > 0) {
      charclass_depth--;
      OUT(']');
      if (legacy_class_close == si + 1) {
        OUT(')');
        legacy_class_close = 0;
      }
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

    if (legacy_ignore_case && charclass_depth == 0 && ((unsigned char)src[si] >= 0xC3 || src[si] == '\\')) {
      uint32_t cp;
      size_t after;
      int fix = regexp_legacy_atom(src, src_len, si, false, &cp, &after) ? regexp_legacy_caseless_index(cp) : -1;
      if (fix >= 0) {
        OUTS(regexp_legacy_caseless_fixes[fix].pcre2);
        si = after - 1;
        continue;
      }
    }

    if (charclass_depth == 0 && src[si] == '.' && !(flags & REGEXP_FLAG_DOTALL)) {
      OUTS("[^\\n\\r\\x{2028}\\x{2029}]");
      continue;
    }

    if (charclass_depth == 0 && (src[si] == '^' || src[si] == '$') && (flags & REGEXP_FLAG_MULTILINE)) {
      if (src[si] == '^') builder->line_anchors = true;
      if (builder->plain_line_anchors) OUT(src[si]);
      else OUTS(src[si] == '^' ? "(?:^|(?<=[\\r\\x{2028}\\x{2029}]))" : "(?:$|(?=[\\r\\x{2028}\\x{2029}]))");
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
      while (brace_end < src_len && src[brace_end] != '}' && ant_ascii_is_xdigit(src[brace_end])) brace_end++;
      if (brace_end < src_len && src[brace_end] == '}' && brace_end > brace_start) {
        OUT('\\'); OUT('x'); OUT('{');
        for (size_t k = brace_start; k < brace_end; k++) OUT(src[k]);
        OUT('}');
        si = brace_end;
        continue;
      }
    }

    if (next == 'u' && si + 5 < src_len &&
        ant_ascii_is_xdigit(src[si+2]) && ant_ascii_is_xdigit(src[si+3]) &&
        ant_ascii_is_xdigit(src[si+4]) && ant_ascii_is_xdigit(src[si+5])) {
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
        ant_ascii_is_xdigit(src[si+2]) && ant_ascii_is_xdigit(src[si+3])) {
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
        for (int k = 0; k < hlen; k++) OUT(hex[k]);
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
             "(?:"
             "[\\x{1f1e6}-\\x{1f1ff}]{2}"
             "|(?:(?:\\p{Emoji_Modifier_Base}\\p{Emoji_Modifier}|(?!\\p{Emoji_Modifier})\\p{Emoji_Presentation}|(?![#*0-9]|\\p{Emoji_Presentation})\\p{Emoji}\\x{fe0f})\\x{200d})+"
             "(?:\\p{Emoji_Modifier_Base}\\p{Emoji_Modifier}|(?!\\p{Emoji_Modifier})\\p{Emoji_Presentation}|(?![#*0-9]|\\p{Emoji_Presentation})\\p{Emoji}\\x{fe0f})"
             "|\\p{Emoji_Modifier_Base}\\p{Emoji_Modifier}"
             "|[#*0-9]\\x{fe0f}\\x{20e3}"
             "|\\x{1f3f4}\\x{e0067}\\x{e0062}(?:\\x{e0065}\\x{e006e}\\x{e0067}|\\x{e0073}\\x{e0063}\\x{e0074}|\\x{e0077}\\x{e006c}\\x{e0073})\\x{e007f}"
             "|\\p{Emoji_Presentation}"
             "|(?![#*0-9]|\\p{Emoji_Presentation})\\p{Emoji}\\x{fe0f}"
             ")"},
          };
          for (size_t m = 0; m < sizeof(sprops)/sizeof(sprops[0]); m++) {
            if (strlen(sprops[m].name) == prop_len && memcmp(sprops[m].name, prop, prop_len) == 0) {
              for (const char *r = sprops[m].exp; *r; r++) OUT(*r);
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

    if (next == 's' || next == 'S') {
      if (charclass_depth > 0) OUTS(next == 's' ? regexp_space_ranges : regexp_non_space_ranges);
      else {
        OUT('['); if (next == 'S') OUT('^');
        OUTS(regexp_space_ranges);
        OUT(']');
      }
      si++;
      continue;
    }

    if (unicode_ignore_case && (next == 'w' || next == 'W')) {
      if (charclass_depth > 0) OUTS(next == 'w' ? regexp_folded_word_ranges : regexp_folded_non_word_ranges);
      else {
        OUT('['); if (next == 'W') OUT('^');
        OUTS(regexp_folded_word_ranges);
        OUT(']');
      }
      si++;
      continue;
    }

    if (unicode_ignore_case && charclass_depth == 0 && (next == 'b' || next == 'B')) {
      OUTS(next == 'b' ? regexp_folded_boundary : regexp_folded_non_boundary);
      si++;
      continue;
    }

    if (
      next == 'k' && charclass_depth == 0 && si + 2 < src_len && src[si + 2] == '<' &&
      regexp_source_has_group_names(src, src_len)
    ) {
      size_t name_end = regexp_group_name_end(src, src_len, si + 3);
      if (name_end < src_len) {
        if (legacy_ignore_case) {
          OUTS("(?C\"");
          for (size_t k = si + 3; k < name_end; k++) OUT(src[k]);
          OUTS("\")");
        }
        for (; si <= name_end; si++) OUT(src[si]);
        si = name_end;
        continue;
      }
    }

    if (legacy_ignore_case && charclass_depth == 0 && next >= '1' && next <= '9') {
      unsigned group = 0;
      for (size_t k = si + 1; k < src_len && src[k] >= '0' && src[k] <= '9' && group <= 255; k++)
        group = group * 10 + (unsigned)(src[k] - '0');
      if (group <= 255) {
        char callout[12];
        int callout_len = snprintf(callout, sizeof(callout), "(?C%u)", group);
        for (int k = 0; k < callout_len; k++) OUT(callout[k]);
      }
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

#undef OUTS
#undef OUT
  builder->length = output_length;
}

static size_t regexp_translate(
  const char *src, size_t src_len,
  char *dst, size_t dst_size,
  uint8_t flags, bool plain_line_anchors, bool *line_anchors
) {
  regexp_pattern_builder_t builder = {
    .data = dst,
    .capacity = dst_size ? dst_size - 1 : 0,
    .plain_line_anchors = plain_line_anchors,
  };
  js_to_pcre2_pattern_append(src, src_len, &builder, (flags & REGEXP_FLAG_UNICODE_SET) != 0, flags, 0);
  if (dst_size) {
    size_t terminator = builder.length < dst_size
      ? builder.length
      : dst_size - 1;
    dst[terminator] = '\0';
  }
  if (line_anchors) *line_anchors = builder.line_anchors;
  return builder.length;
}

size_t js_to_pcre2_pattern(
  const char *src,
  size_t src_len,
  char *dst,
  size_t dst_size,
  uint8_t flags
) {
  return regexp_translate(src, src_len, dst, dst_size, flags, false, NULL);
}

static js_cstr_t regexp_translate_cstr(
  const char *src, size_t src_len,
  char *stack_buf, size_t stack_size,
  uint8_t flags, bool plain_line_anchors, bool *line_anchors
) {
  js_cstr_t result = {0};
  size_t required = regexp_translate(
    src, src_len, stack_buf, stack_size, flags, plain_line_anchors, line_anchors
  );
  if (required < stack_size) {
    result.ptr = stack_buf;
    result.len = required;
    return result;
  }
  if (required == SIZE_MAX) return result;

  char *allocated = malloc(required + 1);
  if (!allocated) return result;
  size_t translated = regexp_translate(
    src, src_len, allocated, required + 1, flags, plain_line_anchors, NULL
  );
  if (translated != required) {
    free(allocated);
    return result;
  }

  result.ptr = allocated;
  result.len = translated;
  result.needs_free = true;
  return result;
}

js_cstr_t js_to_pcre2_pattern_cstr(
  const char *src,
  size_t src_len,
  char *stack_buf,
  size_t stack_size,
  uint8_t flags
) {
  return regexp_translate_cstr(src, src_len, stack_buf, stack_size, flags, false, NULL);
}

static constexpr uint32_t REGEXP_LAST_INDEX_SLOT = 0;

static bool regexp_init_last_index(ant_t *js, ant_value_t value) {
  ant_object_t *obj = js_obj_ptr(value);
  
  if (!js->regex_state || !obj || obj->prop_count != 0) return false;
  regexp_instance_shape_t *cache = &js->regex_state->instance_shape;
  
  if (!cache->shape) {
    if (ant_shape_count(obj->shape) != 0) return false;
    ant_shape_t *shape = ant_shape_clone(obj->shape);
    const char *key = intern_string(js, "lastIndex", 9);
    uint32_t slot;
    
    if (!shape) return false;
    
    if (!key || !ant_shape_add_interned(js, shape, key, ANT_PROP_ATTR_WRITABLE, &slot)) {
      ant_shape_release(shape);
      return false;
    }
    
    ant_shape_retain(obj->shape);
    *cache = (regexp_instance_shape_t){.base = obj->shape, .shape = shape};
  }
  
  if (obj->shape != cache->base || !js_obj_ensure_prop_capacity(js, obj, 1)) return false;
  ant_shape_transition_existing(&obj->shape, cache->shape);
  ant_object_invalidate_guarded_absence(js, obj);
  ant_object_prop_set_unchecked(obj, REGEXP_LAST_INDEX_SLOT, tov(0));
  
  return true;
}

static void regexp_init_flags(ant_t *js, ant_value_t obj, ant_value_t flags, bool is_new) {
  ant_offset_t flen;
  const char *fstr = (const char *)(uintptr_t)vstr(js, flags, &flen);
  uint8_t mask = regexp_parse_flags_mask(fstr, flen);

  char sorted[REGEXP_FLAG_COUNT]; int si = 0;
#define REGEXP_FLAG(id, name, ch) if (mask & REGEXP_FLAG_##id) sorted[si++] = ch;
#include "modules/regex_flags.h"

  ant_value_t flags_value = flen == (ant_offset_t)si && memcmp(fstr, sorted, si) == 0
    ? flags : js_mkstr(js, sorted, si);
  if (!is_new) setprop_cstr(js, obj, "lastIndex", 9, tov(0));
  else if (!regexp_init_last_index(js, obj)) mkprop_exact_attrs(js, obj, ANT_STRING("lastIndex"), tov(0), ANT_PROP_ATTR_WRITABLE);
  if (is_new) {
    unsigned count = ant_object_extra_count(js_obj_ptr(obj));
    if (count <= UINT8_MAX - 3) (void)js_reserve_slots(obj, (uint8_t)(count + 3));
  }
  js_set_slot(obj, SLOT_REGEXP_FLAGS_MASK, tov((double)mask));
  js_set_slot(obj, SLOT_REGEXP_FLAGS_STRING, flags_value);
  js_set_slot(obj, SLOT_REGEXP_NAMED_GROUPS, js_mkundef());
}

ant_value_t is_regexp_like(ant_t *js, ant_value_t value) {
  if (!is_object_type(value)) return js_false;

  ant_value_t match_sym = js->sym.match_sym;
  if (vtype(match_sym) == kTypeSymbol) {
    ant_value_t match_val = js_get_sym(js, value, match_sym);
    if (is_err(match_val)) return match_val;
    if (vtype(match_val) != kTypeUndefined) return js_bool(js_truthy(js, match_val));
  }

  return js_bool(proto_chain_contains(js, value, js->builtins.regexp_proto));
}

ant_value_t reject_regexp_arg(ant_t *js, ant_value_t value, const char *method_name) {
  ant_value_t is_re = is_regexp_like(js, value);
  if (is_err(is_re)) return is_re;
  if (js_truthy(js, is_re))
    return js_mkerr_typed(js, JS_ERR_TYPE, "First argument to String.prototype.%s must not be a regular expression", method_name);
  return js_mkundef();
}

static ant_value_t regexp_species_ctor(ant_t *js, ant_value_t rx, const char *method) {
  ant_value_t ctor = js_get(js, rx, "constructor");
  if (is_err(ctor)) return ctor;

  ant_value_t C = js->builtins.regexp_ctor_value;
  if (vtype(ctor) != kTypeUndefined) {
    if (!is_object_type(ctor))
      return js_mkerr_typed(js, JS_ERR_TYPE, "%s: constructor is not an object", method);
    ant_value_t species = get_ctor_species_value(js, ctor);
    if (is_err(species)) return species;
    if (vtype(species) != kTypeUndefined && vtype(species) != kTypeNull) C = species;
  }

  if (vtype(C) != kTypeFunction && vtype(C) != kTypeBuiltin)
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp species is not a constructor");
  return C;
}

static ant_value_t regexp_species_construct(ant_t *js, ant_value_t rx, ant_value_t ctor, ant_value_t *ctor_args, int nargs) {
  if (vtype(ctor) == kTypeObject && is_proxy(ctor))
    return js_proxy_construct(js, ctor, ctor_args, nargs, ctor);

  ant_value_t new_target = ctor;
  ant_value_t proto = sv_prepare_construct_meta(js, ctor, ctor, &new_target);
  if (is_err(proto)) return proto;

  ant_value_t seed = Ant_Object_CreateBare(js);
  if (is_err(seed)) return seed;
  if (is_object_type(proto)) js_set_proto_init(seed, proto);

  ant_value_t ctor_this = seed;
  ant_value_t result = sv_vm_call(js->vm, js, ctor, seed, ctor_args, nargs, &ctor_this, new_target);

  if (is_err(result)) return result;
  if (!is_object_type(result)) result = is_object_type(ctor_this) ? ctor_this : seed;
  return result;
}

static ant_value_t regexp_exec_abstract(ant_t *js, ant_value_t rx, ant_value_t str);
static ant_value_t builtin_regexp_exec(ant_params_t);

static __attribute__((aligned(16)))
pcre2_match_context *regex_get_match_context(ant_t *js) {
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
  free(entry->group_names);
  pcre2_code_free(entry->line_code);
  pcre2_code_free(entry->code);
  free(entry->pattern);
  free(entry);
}

static void compiled_regex_entry_maybe_free(
  compiled_regex_cache_entry_t *entry
) {
  if (
    entry && entry->object_refs == 0 &&
    entry->active_refs == 0 && entry->cache_refs == 0
  ) compiled_regex_entry_free(entry);
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
  compiled_regex_cache_entry_t *entry
) {
  if (!table || !entry || table->count >= REGEX_COMPILED_CACHE_MAX)
    return false;
  if (
    !table->slots ||
    (table->count + 1) * REGEX_COMPILED_TABLE_LOAD_DEN >=
      table->cap * REGEX_COMPILED_TABLE_LOAD_NUM
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
      return true;
    }
    if (existing == entry) return false;
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
    compiled_regex_entry_maybe_free(entry);
  }
  free(table->slots);
  memset(table, 0, sizeof(*table));
}

static void compiled_regex_cache_rotate(ant_regex_state_t *state) {
  compiled_regex_table_clear(&state->compiled[1]);
  state->compiled[1] = state->compiled[0];
  memset(&state->compiled[0], 0, sizeof(state->compiled[0]));
}

static void compiled_regex_cache_admit_new(
  ant_regex_state_t *state,
  compiled_regex_cache_entry_t *entry
) {
  bool admitted = compiled_regex_table_insert(&state->compiled[0], entry);
  if (!admitted && state->compiled[0].count >= REGEX_COMPILED_CACHE_MAX) {
    compiled_regex_cache_rotate(state);
    admitted = compiled_regex_table_insert(&state->compiled[0], entry);
  }
  if (admitted) entry->cache_refs++;
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
    return entry;
  }

  entry = compiled_regex_table_lookup(
    &state->compiled[1], key_hash, pattern, pattern_len, flags_mask
  );
  if (!entry) {
    return NULL;
  }

  if (compiled_regex_table_insert(&state->compiled[0], entry)) {
    entry->cache_refs++;
  }
  return entry;
}

static void regexp_keep_required_literal(
  compiled_regex_cache_entry_t *entry, const char *run, size_t length
) {
  if (length > REGEXP_REQUIRED_LITERAL_MAX) length = REGEXP_REQUIRED_LITERAL_MAX;
  if (length < 4 || length <= entry->required_literal_len) return;
  memcpy(entry->required_literal, run, length);
  entry->required_literal_len = (uint8_t)length;
}

static bool regex_source_is_plain_literal(const char *source, ant_offset_t len) {
  if (len == 0 || len > REGEXP_PLAIN_LITERAL_MAX) return false;
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

static void regexp_find_required_literal(compiled_regex_cache_entry_t *entry) {
  if (entry->flags_mask & (REGEXP_FLAG_IGNORE_CASE | REGEXP_FLAG_UNICODE_SET)) return;
  uint32_t first_type = 0;
  pcre2_pattern_info(entry->code, PCRE2_INFO_FIRSTCODETYPE, &first_type);
  if (first_type != 0) return;

  const char *pattern = entry->pattern;
  size_t length = entry->pattern_len, run_len = 0;
  char run[REGEXP_REQUIRED_LITERAL_MAX];
  unsigned depth = 0;
  bool in_class = false;
  for (size_t i = 0; i < length; i++) {
    unsigned char c = (unsigned char)pattern[i];
    bool literal = false;
    if (c == '\\') {
      if (++i == length) goto unsupported;
      c = (unsigned char)pattern[i];
      if (c == 0 || c >= 0x80) goto unsupported;
      if (strchr("\\^$.*+?()[]{}|/-\"'", c)) literal = true;
      else if (!strchr("dDsSwWbBfnrtv", c)) goto unsupported;
    } else if (in_class) {
      if (c == '[') goto unsupported;
      if (c == ']') in_class = false;
      continue;
    } else if (c == '[') {
      size_t first = i + 1;
      if (first < length && pattern[first] == '^') first++;
      if (first == length || pattern[first] == ']') goto unsupported;
      in_class = true;
    } else if (c == '(') {
      if (i + 1 < length && pattern[i + 1] == '*') goto unsupported;
      if (i + 1 < length && pattern[i + 1] == '?') {
        if (i + 2 == length || pattern[i + 2] != ':') goto unsupported;
        i += 2;
      }
      depth++;
    } else if (c == ')') {
      if (depth == 0) goto unsupported;
      depth--;
    } else if (c == '|') {
      if (depth == 0) goto unsupported;
    } else if (c == '*' || c == '+' || c == '?' || c == '{') {
      // The preceding atom may be optional. Even a required repetition can
      // safely lose its last literal byte from this conservative candidate.
      if (run_len) run_len--;
      if (c == '{') {
        while (++i < length && pattern[i] != '}') {
          if ((pattern[i] < '0' || pattern[i] > '9') && pattern[i] != ',') goto unsupported;
        }
        if (i == length) goto unsupported;
      }
    } else if (c == ']' || c == '}') {
      goto unsupported;
    } else {
      literal = c < 0x80 && c != '.' && c != '^' && c != '$';
    }

    if (literal && depth == 0 && !in_class) {
      if (run_len < sizeof(run)) run[run_len] = (char)c;
      run_len++;
    } else {
      regexp_keep_required_literal(entry, run, run_len);
      run_len = 0;
    }
  }
  if (depth || in_class) goto unsupported;
  regexp_keep_required_literal(entry, run, run_len);
  return;

unsupported:
  entry->required_literal_len = 0;
}

static pcre2_code *regexp_compile_code(
  const char *pattern, size_t pattern_len, uint8_t flags_mask,
  bool plain_line_anchors, bool *line_anchors
) {
  char pcre2_pattern_stack[4096];
  js_cstr_t pcre2_pattern = regexp_translate_cstr(
    pattern, pattern_len, pcre2_pattern_stack, sizeof(pcre2_pattern_stack),
    flags_mask, plain_line_anchors, line_anchors
  );
  if (!pcre2_pattern.ptr) return NULL;

  uint32_t options = PCRE2_UTF | PCRE2_MATCH_UNSET_BACKREF | PCRE2_DUPNAMES | PCRE2_DOLLAR_ENDONLY;
  if (flags_mask & REGEXP_FLAG_IGNORE_CASE) options |= PCRE2_CASELESS;
  if (flags_mask & REGEXP_FLAG_MULTILINE) options |= PCRE2_MULTILINE | PCRE2_ALT_CIRCUMFLEX;
  if (flags_mask & REGEXP_FLAG_DOTALL) options |= PCRE2_DOTALL;

  int errcode;
  PCRE2_SIZE erroffset;
  pcre2_compile_context *compile_ctx = pcre2_compile_context_create(NULL);
  if (compile_ctx) {
    pcre2_set_newline(compile_ctx, PCRE2_NEWLINE_LF);
    if ((flags_mask & REGEXP_FLAG_IGNORE_CASE) && !(flags_mask & (REGEXP_FLAG_UNICODE | REGEXP_FLAG_UNICODE_SET)))
      pcre2_set_compile_extra_options(compile_ctx, PCRE2_EXTRA_CASELESS_RESTRICT);
  }
  pcre2_code *re = pcre2_compile(
    (PCRE2_SPTR)pcre2_pattern.ptr, pcre2_pattern.len,
    options, &errcode, &erroffset, compile_ctx
  );
  if (compile_ctx) pcre2_compile_context_free(compile_ctx);
  if (pcre2_pattern.needs_free) free((void *)pcre2_pattern.ptr);
  return re;
}

static __attribute__((noinline)) bool regexp_line_code_prepare(compiled_regex_cache_entry_t *entry) {
  if (entry->line_state == REGEXP_LINE_READY) return true;
  if (entry->line_state != REGEXP_LINE_PENDING) return false;
  uint32_t captures = 0, line_captures = 0;
  pcre2_code *code = regexp_compile_code(entry->pattern, entry->pattern_len, entry->flags_mask, true, NULL);
  pcre2_pattern_info(entry->code, PCRE2_INFO_CAPTURECOUNT, &captures);
  if (code) pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &line_captures);
  if (!code || captures != line_captures) {
    pcre2_code_free(code);
    entry->line_state = REGEXP_LINE_FAILED;
    return false;
  }
  entry->line_code = code;
  entry->line_jit_ready = pcre2_jit_compile(code, PCRE2_JIT_COMPLETE) == 0;
  entry->line_state = REGEXP_LINE_READY;
  return true;
}

static inline const char *regexp_pattern_syntax_error(const char *pattern, size_t pattern_len, uint8_t flags_mask) {
  return regexp_syntax_error(
    pattern, pattern_len,
    (flags_mask & (REGEXP_FLAG_UNICODE | REGEXP_FLAG_UNICODE_SET)) != 0,
    (flags_mask & REGEXP_FLAG_UNICODE_SET) != 0
  );
}

static compiled_regex_cache_entry_t *compiled_regex_cache_get_or_compile(
  ant_regex_state_t *state,
  const char *pattern,
  size_t pattern_len,
  uint8_t flags_mask,
  const char **syntax_error
) {
  uint64_t key_hash = compiled_regex_key_hash(
    pattern, pattern_len, flags_mask
  );
  compiled_regex_cache_entry_t *cached = compiled_regex_cache_lookup(
    state, pattern, pattern_len, flags_mask, key_hash
  );
  if (cached) return cached;

  const char *error = regexp_pattern_syntax_error(pattern, pattern_len, flags_mask);
  if (error) {
    if (syntax_error) *syntax_error = error;
    return NULL;
  }

  bool line_anchors = false;
  pcre2_code *re = regexp_compile_code(pattern, pattern_len, flags_mask, false, &line_anchors);
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
  entry->plain_literal =
    !(flags_mask & REGEXP_PLAIN_LITERAL_EXCLUDED_FLAGS) &&
    regex_source_is_plain_literal(pattern, pattern_len);
  entry->code = re;
  entry->line_state = line_anchors ? REGEXP_LINE_PENDING : REGEXP_LINE_NONE;
  pcre2_pattern_info(re, PCRE2_INFO_NAMECOUNT, &entry->namecount);
  if (entry->namecount) entry->group_names = regexp_collect_group_names(re, entry->namecount);
  if ((flags_mask & REGEXP_FLAG_IGNORE_CASE) && !(flags_mask & (REGEXP_FLAG_UNICODE | REGEXP_FLAG_UNICODE_SET)))
    pcre2_callout_enumerate(re, regexp_callout_found, &entry->legacy_backrefs);
  entry->jit_ready = pcre2_jit_compile(re, PCRE2_JIT_COMPLETE) == 0;
  regexp_find_required_literal(entry);

  compiled_regex_cache_admit_new(state, entry);
  return entry;
}

static compiled_regex_cache_entry_t *compiled_regex_cache_acquire(
  ant_t *js,
  const char *pattern,
  size_t pattern_len,
  uint8_t flags_mask,
  const char **syntax_error
) {
  ant_regex_state_t *state = js->regex_state;
  if (!state) return NULL;
  compiled_regex_cache_entry_t *entry = compiled_regex_cache_get_or_compile(
    state, pattern, pattern_len, flags_mask, syntax_error
  );
  if (entry) entry->active_refs++;
  return entry;
}

static void compiled_regex_cache_release(
  compiled_regex_cache_entry_t *entry
) {
  if (!entry) return;
  ANT_ASSERT(entry->active_refs > 0, "active compiled regexp reference underflow");
  entry->active_refs--;
  compiled_regex_entry_maybe_free(entry);
}

static __attribute__((noinline)) bool regex_match_scope_begin_slow(
  compiled_regex_cache_entry_t *entry, regex_match_scope_t *scope
) {
  if (entry->scratch_in_use) {
    scope->match_data = pcre2_match_data_create_from_pattern(entry->code, NULL);
    return scope->match_data != NULL;
  }
  entry->scratch_match_data = pcre2_match_data_create_from_pattern(entry->code, NULL);
  if (!entry->scratch_match_data) return false;
  entry->scratch_in_use = true;
  scope->match_data = entry->scratch_match_data;
  scope->borrowed = true;
  return true;
}

static __attribute__((always_inline)) inline bool regex_match_scope_begin(
  compiled_regex_cache_entry_t *entry, regex_match_scope_t *scope
) {
  scope->compiled = entry;
  if (__builtin_expect(entry->scratch_in_use || !entry->scratch_match_data, 0))
    return regex_match_scope_begin_slow(entry, scope);
  entry->scratch_in_use = true;
  scope->match_data = entry->scratch_match_data;
  scope->borrowed = true;
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

static void regexp_object_compiled_unref(ant_t *js, compiled_regex_cache_entry_t *compiled) {
  ant_regex_state_t *state = js->regex_state;
  if (state && state->live_object_data > 0) state->live_object_data--;
  if (compiled->object_refs > 0) compiled->object_refs--;
  compiled_regex_entry_maybe_free(compiled);
}

static void regexp_object_compiled_release(
  ant_t *js, ant_value_t regexp, compiled_regex_cache_entry_t *compiled
) {
  if (!compiled) return;
  js_clear_native(regexp, REGEXP_NATIVE_TAG);
  regexp_object_compiled_unref(js, compiled);
}

static void regexp_object_finalize(ant_t *js, ant_object_t *obj) {
  if (obj->native_tag == REGEXP_NATIVE_TAG && obj->native_ptr && !ant_object_sidecar(obj)) {
    compiled_regex_cache_entry_t *compiled = obj->native_ptr;
    obj->native_ptr = NULL;
    obj->native_tag = 0;
    regexp_object_compiled_unref(js, compiled);
    return;
  }
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

static void regexp_site_release(regexp_site_entry_t *site) {
  compiled_regex_cache_entry_t *entry = site->entry;
  *site = (regexp_site_entry_t){0};
  if (!entry) return;
  if (entry->cache_refs > 0) entry->cache_refs--;
  compiled_regex_entry_maybe_free(entry);
}

static void regexp_sites_clear(ant_regex_state_t *state) {
  for (size_t i = 0; i < REGEXP_SITE_CACHE_SIZE; i++) regexp_site_release(&state->sites[i]);
}

static inline regexp_site_entry_t *regexp_site_for(ant_regex_state_t *state, ant_value_t source) {
  return &state->sites[(vdata(source) >> 4) % REGEXP_SITE_CACHE_SIZE];
}

static compiled_regex_cache_entry_t *regexp_site_lookup(ant_t *js, ant_value_t source, ant_value_t flags, uint8_t flags_mask) {
  ant_regex_state_t *state = js->regex_state;
  if (!state || vtype(source) != kTypeString) return NULL;

  uint64_t epoch = gc_strings_sweep_epoch();
  regexp_site_entry_t *site = regexp_site_for(state, source);
  if (site->pattern == source && site->epoch == epoch && site->entry->flags_mask == flags_mask) return site->entry;

  ant_offset_t plen;
  const char *pattern_ptr = (const char *)(uintptr_t)vstr(js, source, &plen);
  compiled_regex_cache_entry_t *compiled = compiled_regex_cache_get_or_compile(
    state, pattern_ptr, plen, flags_mask, NULL
  );
  if (!compiled) return NULL;
  compiled->cache_refs++;
  regexp_site_release(site);
  *site = (regexp_site_entry_t){ .pattern = source, .flags = flags, .epoch = epoch, .entry = compiled };
  return compiled;
}

static bool regexp_compile_shared_from_object(
  ant_t *js,
  ant_value_t regexp_obj,
  uint8_t flags_mask,
  compiled_regex_cache_entry_t **out
) {
  compiled_regex_cache_entry_t *compiled = regexp_site_lookup(js, js_get_slot(regexp_obj, SLOT_DATA), 0, flags_mask);
  if (!compiled) return false;
  *out = compiled;
  return true;
}

static compiled_regex_cache_entry_t *regexp_attach_compiled(
  ant_t *js, ant_value_t regexp_obj, compiled_regex_cache_entry_t *compiled
) {
  ant_object_t *obj_ptr = js_obj_ptr(regexp_obj);
  compiled->object_refs++;
  ant_value_t groups_meta = regexp_build_named_groups_meta(js, compiled);
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
  js->regex_state->live_object_data++;
  if (groups_meta != js_get_slot(regexp_obj, SLOT_REGEXP_NAMED_GROUPS))
    js_set_slot(regexp_obj, SLOT_REGEXP_NAMED_GROUPS, groups_meta);
  return compiled;
}

static compiled_regex_cache_entry_t *regex_get_or_compile(
  ant_t *js,
  ant_value_t regexp_obj,
  uint8_t flags_mask,
  compiled_regex_cache_entry_t *compiled
) {
  ant_regex_state_t *state = js->regex_state;
  if (!state) return NULL;

  if (!compiled) {
    compiled = js_get_native(regexp_obj, REGEXP_NATIVE_TAG);
  }
  if (compiled && compiled->flags_mask == flags_mask) {
    return compiled;
  }
  if (compiled) regexp_object_compiled_release(js, regexp_obj, compiled);

  if (!regexp_compile_shared_from_object(
    js, regexp_obj, flags_mask, &compiled
  )) return NULL;

  return regexp_attach_compiled(js, regexp_obj, compiled);
}

static bool regexp_has_internal_slots(ant_t *js, ant_value_t value) {
  if (!is_object_type(value)) return false;
  ant_object_t *obj = js_obj_ptr(value);
  return obj && obj->flags.regexp_brand;
}

static ant_value_t regexp_check_flags(ant_t *js, ant_value_t flags) {
  ant_offset_t flen;
  const char *fstr = (const char *)(uintptr_t)vstr(js, flags, &flen);
  if (regexp_flags_valid(fstr, flen)) return js_mkundef();
  return js_mkerr_typed(
    js, JS_ERR_SYNTAX, "Invalid flags supplied to RegExp constructor '%.*s'", (int)flen, fstr
  );
}

static ant_value_t regexp_syntax_error_value(
  ant_t *js, const char *pattern, size_t pattern_len, uint8_t flags_mask, const char *message
) {
  char sorted[REGEXP_FLAG_COUNT]; int si = 0;
#define REGEXP_FLAG(id, name, ch) if (flags_mask & REGEXP_FLAG_##id) sorted[si++] = ch;
#include "modules/regex_flags.h"
  return js_mkerr_typed(
    js, JS_ERR_SYNTAX, "Invalid regular expression: /%.*s/%.*s: %s",
    (int)pattern_len, pattern, si, sorted, message
  );
}

static ant_value_t regexp_check_pattern(ant_t *js, ant_value_t pattern, ant_value_t flags) {
  ant_offset_t flen, plen;
  const char *fstr = (const char *)(uintptr_t)vstr(js, flags, &flen);
  uint8_t mask = regexp_parse_flags_mask(fstr, flen);
  ant_regex_state_t *state = js->regex_state;
  uint64_t epoch = gc_strings_sweep_epoch();
  regexp_validated_entry_t *validated = NULL;
  if (state && vtype(pattern) == kTypeString) {
    const regexp_site_entry_t *site = regexp_site_for(state, pattern);
    if (site->pattern == pattern && site->epoch == epoch && site->entry->flags_mask == mask) return js_mkundef();
    validated = &state->validated[(vdata(pattern) >> 4) % REGEXP_SITE_CACHE_SIZE];
    if (validated->pattern == pattern && validated->epoch == epoch && validated->flags_mask == mask) return js_mkundef();
  }

  const char *src = (const char *)(uintptr_t)vstr(js, pattern, &plen);
  const char *error = regexp_pattern_syntax_error(src, (size_t)plen, mask);
  if (error) return regexp_syntax_error_value(js, src, (size_t)plen, mask, error);
  if (validated) *validated = (regexp_validated_entry_t){ .pattern = pattern, .epoch = epoch, .flags_mask = mask };
  return js_mkundef();
}

static ant_value_t regexp_escape_source(ant_t *js, ant_value_t pattern) {
  size_t len = 0;
  const char *src = js_getstr(js, pattern, &len);
  if (!src) return pattern;
  if (len == 0) return ANT_STRING("(?:)");

  bool needs_escape = false;
  for (size_t i = 0; i < len && !needs_escape; i++) {
    unsigned char c = (unsigned char)src[i];
    needs_escape = c == '/' || c == '\n' || c == '\r' || c == 0xe2;
  }
  if (!needs_escape) return pattern;

  char *out = malloc(len * 2);
  if (!out) return Ant_Error_CreateOutOfMemory(js);
  size_t n = 0;
  bool in_class = false;

  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c == '\\' && i + 1 < len) {
      out[n++] = src[i++];
      out[n++] = src[i];
    } else if (c == '/' && !in_class) {
      out[n++] = '\\';
      out[n++] = '/';
    } else if (c == '\n' || c == '\r') {
      out[n++] = '\\';
      out[n++] = c == '\n' ? 'n' : 'r';
    } else if (c == 0xe2 && i + 2 < len && (unsigned char)src[i + 1] == 0x80 &&
               ((unsigned char)src[i + 2] == 0xa8 || (unsigned char)src[i + 2] == 0xa9)) {
      memcpy(out + n, (unsigned char)src[i + 2] == 0xa8 ? "\\u2028" : "\\u2029", 6);
      n += 6;
      i += 2;
    } else {
      if (c == '[') in_class = true;
      else if (c == ']') in_class = false;
      out[n++] = (char)c;
    }
  }

  ant_value_t escaped = js_mkstr(js, out, n);
  free(out);
  return escaped;
}

static ant_value_t builtin_RegExp(ant_params_t) {
  bool pattern_is_regexp = false;
  if (nargs > 0) {
    ant_value_t is_re = is_regexp_like(js, args[0]);
    if (is_err(is_re)) return is_re;
    pattern_is_regexp = js_truthy(js, is_re);
  }

  if (vtype(call_new_target) == kTypeUndefined && nargs > 0 && pattern_is_regexp) {
    if (nargs < 2 || vtype(args[1]) == kTypeUndefined) {
      ant_value_t ctor = js_getprop_fallback(js, args[0], "constructor");
      if (is_err(ctor)) return ctor;
      if (same_ctor_identity(js, ctor, js->builtins.regexp_ctor_value)) return args[0];
    }
  }

  ant_value_t regexp_obj = js->this_val;
  bool use_this = (vtype(call_new_target) != kTypeUndefined && vtype(regexp_obj) == kTypeObject);

  if (!use_this) {
    regexp_obj = mkobj(js, 0);
    if (is_err(regexp_obj)) return regexp_obj;
  }

  ant_value_t regexp_proto = js->builtins.regexp_proto;
  ant_value_t instance_proto = js_instance_proto_from_new_target(js, regexp_proto, call_new_target);

  if (is_object_type(instance_proto)) js_set_proto_init(regexp_obj, instance_proto);
  if (vtype(call_new_target) == kTypeFunction || vtype(call_new_target) == kTypeBuiltin) {
    js_set_slot(regexp_obj, SLOT_CTOR, call_new_target);
  }

  ant_value_t pattern = js_mkstr(js, "", 0);
  ant_value_t flags = js_mkstr(js, "", 0);
  if (nargs > 0) {
    if (regexp_has_internal_slots(js, args[0])) {
      ant_value_t original = js_get_slot(args[0], SLOT_DATA);
      if (vtype(original) == kTypeString) pattern = original;
      if (nargs >= 2 && vtype(args[1]) != kTypeUndefined) flags = js_tostring_val(js, args[1]);
      else {
        ant_value_t original_flags = regexp_flags_value(js, args[0]);
        if (vtype(original_flags) == kTypeString) flags = original_flags;
      }
      if (is_err(flags)) return flags;
    } else if (pattern_is_regexp) {
      ant_value_t src = js_getprop_fallback(js, args[0], "source");
      if (is_err(src)) return src;
      pattern = js_tostring_val(js, src);
      if (is_err(pattern)) return pattern;
      if (nargs >= 2 && vtype(args[1]) != kTypeUndefined) {
        flags = js_tostring_val(js, args[1]);
      } else {
        ant_value_t fl = js_getprop_fallback(js, args[0], "flags");
        if (is_err(fl)) return fl;
        flags = js_tostring_val(js, fl);
      }
      if (is_err(flags)) return flags;
    } else {
      if (vtype(args[0]) == kTypeString) pattern = args[0];
      else if (vtype(args[0]) != kTypeUndefined) {
        ant_value_t s = js_tostring_val(js, args[0]);
        if (is_err(s)) return s;
        pattern = s;
      }
      if (nargs > 1 && vtype(args[1]) != kTypeUndefined) {
        flags = vtype(args[1]) == kTypeString ? args[1] : js_tostring_val(js, args[1]);
        if (is_err(flags)) return flags;
      }
    }
  }

  ant_value_t flags_error = regexp_check_flags(js, flags);
  if (is_err(flags_error)) return flags_error;
  ant_value_t pattern_error = regexp_check_pattern(js, pattern, flags);
  if (is_err(pattern_error)) return pattern_error;
  
  js_set_slot(regexp_obj, SLOT_DATA, pattern);
  regexp_init_flags(js, regexp_obj, flags, true);

  return regexp_obj;
}

static ant_value_t builtin_regexp_groups_getter(ant_params_t) {
  ant_value_t result_arr = js->this_val;
  if (!is_object_type(result_arr)) return js_mkundef();

  ant_value_t cached = js_get_slot(result_arr, SLOT_REGEXP_GROUPS_CACHE);
  if (is_object_type(cached)) return cached;

  ant_value_t meta = js_get_slot(result_arr, SLOT_REGEXP_RESULT_GROUPS);
  if (!is_object_type(meta)) return js_mkundef();

  ant_value_t groups = Ant_Object_CreateBare(js);
  if (is_err(groups)) return groups;
  js_set_proto_init(groups, js_mknull());

  for (ant_offset_t i = 0; ; i += 2) {
    ant_value_t name = js_arr_get(js, meta, i);
    if (vtype(name) == kTypeUndefined) break;
    ant_value_t index_val = js_arr_get(js, meta, i + 1);
    ant_offset_t index = (vtype(index_val) == kTypeNumber) ? (ant_offset_t)tod(index_val) : 0;
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

static inline ant_offset_t regexp_vstr(ant_t *js, ant_value_t value, ant_offset_t *len) {
  ant_flat_string_t *flat = ant_str_flat_ptr(value);
  if (!flat) return vstr(js, value, len);
  *len = flat->len;
  return (ant_offset_t)(uintptr_t)flat->bytes;
}

static bool regexp_pos_last_index(utf16_pos_t *pos, double index, uint8_t flags, PCRE2_SIZE *start) {
  if (!(index > 0)) index = 0;
  if (index >= (double)pos->len + 1) return false;
  size_t units = (size_t)index;
  *start = utf16_pos_seek(pos, units);
  if (pos->units == units) return true;
  if (pos->byte >= pos->len) return false;
  if (flags & (REGEXP_FLAG_UNICODE | REGEXP_FLAG_UNICODE_SET)) return true;
  if (flags & REGEXP_FLAG_STICKY) return false;
  *start = pos->byte + utf8_char_len_at(pos->str, pos->len, pos->byte);
  utf16_pos_units(pos, *start);
  return true;
}

static double regexp_advance_string_index(
  ant_t *js, const char *str, ant_offset_t len, double index, bool full_unicode
) {
  if (!(index > 0)) index = 0;
  index = floor(index);
  if (!full_unicode || index >= (double)len) return index + 1;
  utf16_pos_t pos;
  utf16_pos_init(&pos, str, len);
  if (pos.ascii) return index + 1;
  PCRE2_SIZE byte = utf16_pos_seek(&pos, (size_t)index);
  bool pair = pos.units == (size_t)index && byte < pos.len && (unsigned char)str[byte] >= 0xF0;
  return index + (pair ? 2 : 1);
}

static ant_value_t regexp_build_indices_pair(ant_t *js, utf16_pos_t *pos, PCRE2_SIZE start, PCRE2_SIZE end) {
  if (start == PCRE2_UNSET) return js_mkundef();

  ant_value_t pair = js_mkarr(js);
  if (is_err(pair)) return pair;
  js_arr_push(js, pair, tov((double)utf16_pos_units(pos, start)));
  js_arr_push(js, pair, tov((double)utf16_pos_units(pos, end)));
  
  return pair;
}

static ant_value_t regexp_build_indices_groups(
  ant_t *js,
  ant_value_t groups_meta,
  ant_value_t indices_arr
) {
  ant_value_t groups = Ant_Object_CreateBare(js);
  if (is_err(groups)) return groups;
  js_set_proto_init(groups, js_mknull());

  for (ant_offset_t i = 0; ; i += 2) {
    ant_value_t name = js_arr_get(js, groups_meta, i);
    if (vtype(name) == kTypeUndefined) break;
    
    ant_value_t index_val = js_arr_get(js, groups_meta, i + 1);
    ant_offset_t index = (vtype(index_val) == kTypeNumber) ? (ant_offset_t)tod(index_val) : 0;
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
  ant_value_t groups_meta,
  utf16_pos_t *pos,
  const PCRE2_SIZE *ovector,
  uint32_t ovcount
) {
  ant_value_t indices_arr = js_mkarr(js);
  if (is_err(indices_arr)) return indices_arr;

  for (uint32_t i = 0; i < ovcount; i++) {
    ant_value_t pair = regexp_build_indices_pair(js, pos, ovector[2*i], ovector[2*i+1]);
    if (is_err(pair)) return pair;
    js_arr_push(js, indices_arr, pair);
  }

  if (is_object_type(groups_meta)) {
    ant_value_t groups = regexp_build_indices_groups(js, groups_meta, indices_arr);
    if (is_err(groups)) return groups;
    if (is_err(setprop_cstr(js, indices_arr, "groups", 6, groups))) return js_mkerr(js, "oom");
  } else if (is_err(setprop_cstr(js, indices_arr, "groups", 6, js_mkundef()))) return js_mkerr(js, "oom");

  return indices_arr;
}

static ant_value_t regexp_attach_exec_result(ant_t *js,const regexp_exec_result_ctx_t *ctx) {
  bool has_named_groups = is_object_type(ctx->groups_meta);
  
  if (!has_named_groups && regexp_result_apply_shape(
    js, ctx->array, ctx->index, ctx->input, js_mkundef(),
    ctx->indices, ctx->with_indices
  )) return ctx->array;

  if (
    is_err(setprop_cstr(js, ctx->array, "index", 5, ctx->index)) ||
    is_err(setprop_cstr(js, ctx->array, "input", 5, ctx->input))
  ) return js_mkerr(js, "oom");

  if (has_named_groups) {
    js_set_slot(ctx->array, SLOT_REGEXP_RESULT_GROUPS, ctx->groups_meta);
    js_set_slot(ctx->array, SLOT_REGEXP_GROUPS_CACHE, js_mkundef());
    js_set_getter_desc(
      js, js_as_obj(ctx->array), "groups", 6,
      Ant_Function_Create(builtin_regexp_groups_getter), JS_DESC_E | JS_DESC_C
    );
  }
  else if (is_err(setprop_cstr(js, ctx->array, "groups", 6, js_mkundef()))) 
    return js_mkerr(js, "oom");

  if (
    ctx->with_indices &&
    is_err(setprop_cstr(js, ctx->array, "indices", 7, ctx->indices))
  ) return js_mkerr(js, "oom");

  return ctx->array;
}

static const char *find_bytes(const char *haystack, ant_offset_t haystack_len, const char *needle, ant_offset_t needle_len);

static __attribute__((noinline)) bool regexp_subject_has_js_line_breaks(const char *str, size_t len) {
  static _Thread_local struct {
    const char *str;
    size_t len;
    uint64_t epoch;
    bool found;
  } cache;

  uint64_t epoch = gc_strings_sweep_epoch();
  if (cache.str == str && cache.len == len && cache.epoch == epoch) return cache.found;

  bool found = memchr(str, '\r', len) != NULL;
  if (!found && !str_is_ascii(str)) {
    const char *at = str, *end = str + len;
    while (!found && (at = memchr(at, 0xE2, (size_t)(end - at))) && end - at >= 3) {
      found = (unsigned char)at[1] == 0x80 && ((unsigned char)at[2] == 0xA8 || (unsigned char)at[2] == 0xA9);
      at++;
    }
  }

  cache.str = str;
  cache.len = len;
  cache.epoch = epoch;
  cache.found = found;
  return found;
}

static __attribute__((always_inline)) inline int regexp_match_literal(
  const compiled_regex_cache_entry_t *compiled,
  const char *str_ptr,
  ant_offset_t str_len,
  PCRE2_SIZE start_offset,
  regex_match_scope_t *scope,
  PCRE2_SIZE **ovector,
  uint32_t *ovcount
) {
  if (start_offset > (PCRE2_SIZE)str_len) return PCRE2_ERROR_NOMATCH;
  const char *match = find_bytes(
    str_ptr + start_offset, str_len - (ant_offset_t)start_offset,
    compiled->pattern, (ant_offset_t)compiled->pattern_len
  );
  if (!match) return PCRE2_ERROR_NOMATCH;
  scope->literal[0] = (PCRE2_SIZE)(match - str_ptr);
  scope->literal[1] = scope->literal[0] + compiled->pattern_len;
  *ovector = scope->literal;
  *ovcount = 1;
  return 1;
}

static __attribute__((always_inline)) inline int regexp_match_code(
  ant_t *js,
  compiled_regex_cache_entry_t *compiled,
  const char *str_ptr,
  ant_offset_t str_len,
  PCRE2_SIZE start_offset,
  bool sticky,
  regex_match_scope_t *scope,
  PCRE2_SIZE **ovector,
  uint32_t *ovcount
) {
  *ovector = NULL;
  *ovcount = 0;

  if (compiled->required_literal_len && str_len >= 128 && start_offset <= str_len &&
      !find_bytes(str_ptr + start_offset, str_len - start_offset,
                  compiled->required_literal, compiled->required_literal_len))
    return PCRE2_ERROR_NOMATCH;

  if (!scope->match_data && !regex_match_scope_begin(compiled, scope)) return PCRE2_ERROR_NOMEMORY;
  pcre2_match_context *match_ctx = regex_get_match_context(js);
  if (__builtin_expect(compiled->legacy_backrefs, 0) && match_ctx)
    pcre2_set_callout(match_ctx, regexp_legacy_backref_callout, compiled);
  uint32_t match_options = sticky ? PCRE2_ANCHORED : 0;
  pcre2_code *code = compiled->code;
  bool jit_ready = compiled->jit_ready;
  if (__builtin_expect(compiled->line_state != REGEXP_LINE_NONE, 0)) {
    if (!scope->line_checked) {
      scope->line_code =
        !regexp_subject_has_js_line_breaks(str_ptr, (size_t)str_len) &&
        regexp_line_code_prepare(compiled);
      scope->line_checked = true;
    }
    if (scope->line_code) {
      code = compiled->line_code;
      jit_ready = compiled->line_jit_ready;
    }
  }
  int rc;
  if (__builtin_expect(jit_ready && !sticky, 1)) {
    rc = pcre2_jit_match(
      code, (PCRE2_SPTR)str_ptr, str_len, start_offset,
      match_options, scope->match_data, match_ctx
    );
  } else {
    if (str_is_valid_wtf8(str_ptr)) match_options |= PCRE2_NO_UTF_CHECK;
    rc = pcre2_match(
      code, (PCRE2_SPTR)str_ptr, str_len, start_offset,
      match_options, scope->match_data, match_ctx
    );
  }

  if (rc >= 0) {
    *ovector = pcre2_get_ovector_pointer(scope->match_data);
    *ovcount = pcre2_get_ovector_count(scope->match_data);
  }
  return rc;
}

static __attribute__((always_inline)) inline int regexp_match(
  ant_t *js, compiled_regex_cache_entry_t *compiled,
  const char *str_ptr, ant_offset_t str_len, PCRE2_SIZE start_offset, bool sticky,
  regex_match_scope_t *scope, PCRE2_SIZE **ovector, uint32_t *ovcount
) {
  if (compiled->plain_literal && !sticky)
    return regexp_match_literal(compiled, str_ptr, str_len, start_offset, scope, ovector, ovcount);
  return regexp_match_code(js, compiled, str_ptr, str_len, start_offset, sticky, scope, ovector, ovcount);
}

static const ant_shape_prop_t *regexp_lastindex_lookup_own_property(
  ant_t *js,
  ant_object_t *obj,
  uint32_t *out_slot
) {
  if (!obj || !obj->shape) return NULL;

  const char *key = intern_string(js, "lastIndex", 9);
  if (!key) return NULL;
  int32_t found = ant_shape_lookup_interned(obj->shape, key);
  if (found < 0 || (uint32_t)found >= obj->prop_count) return NULL;

  uint32_t slot = (uint32_t)found;
  if (out_slot) *out_slot = slot;
  return ant_shape_prop_at(obj->shape, slot);
}

static bool regexp_lastindex_fast_location(
  compiled_regex_cache_entry_t *compiled,
  ant_value_t regexp,
  ant_object_t **out_obj,
  uint32_t *out_slot
) {
  if (!compiled || !compiled->lastindex_shape) return false;
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
  ant_t *js,
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

  uint32_t slot;
  const ant_shape_prop_t *prop = regexp_lastindex_lookup_own_property(
    js, obj, &slot
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
  compiled->lastindex_slot = slot;
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
    return value;
  }

  ant_object_t *receiver = js_obj_ptr(regexp);
  if (!is_proxy(regexp)) {
    const ant_shape_prop_t *prop = regexp_lastindex_lookup_own_property(
      js, receiver, NULL
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
  return setprop_cstr(js, regexp, "lastIndex", 9, value);
}

typedef struct {
  ant_object_t *obj;
  compiled_regex_cache_entry_t *compiled;
  uint32_t li_slot;
  uint8_t flags;
} regexp_fast_t;

typedef enum {
  REGEXP_FAST_PRISTINE,
  REGEXP_FAST_BUILTIN_EXEC,
} regexp_fast_tier_t;

static bool regexp_has_builtin_exec(ant_t *js, ant_value_t rx) {
  ant_prop_loc_t exec_loc = lkp_proto(js, rx, "exec", 4);
  if (
    !exec_loc.obj || !exec_loc.obj->shape ||
    exec_loc.obj->flags.is_exotic ||
    exec_loc.slot >= exec_loc.obj->prop_count
  ) return false;

  const ant_shape_prop_t *exec_prop = ant_shape_prop_at(exec_loc.obj->shape, exec_loc.slot);
  if (!exec_prop || exec_prop->has_getter || exec_prop->has_setter) return false;

  ant_value_t exec_fn = js_prop_load(exec_loc);
  return vtype(exec_fn) == kTypeBuiltin && js_cfunc_same_entrypoint(exec_fn, builtin_regexp_exec);
}

static __attribute__((noinline)) bool regexp_fast_get_builtin_exec(
  ant_t *js, ant_value_t rx, ant_object_t *obj, regexp_fast_t *fast
) {
  if (!obj->flags.regexp_brand || obj->flags.is_exotic || !regexp_has_builtin_exec(js, rx)) return false;

  compiled_regex_cache_entry_t *hint;
  uint8_t flags = regexp_flags_mask(js, rx, &hint);
  compiled_regex_cache_entry_t *compiled = regex_get_or_compile(js, rx, flags, hint);
  if (!compiled) return false;

  ant_object_t *li_obj;
  uint32_t li_slot;
  regexp_lastindex_cache_location(js, compiled, rx);
  if (
    !regexp_lastindex_fast_location(compiled, rx, &li_obj, &li_slot) ||
    vtype(ant_object_prop_get_unchecked(li_obj, li_slot)) != kTypeNumber
  ) return false;

  *fast = (regexp_fast_t){ .obj = li_obj, .compiled = compiled, .li_slot = li_slot, .flags = flags };
  return true;
}

static bool regexp_fast_get(ant_t *js, ant_value_t rx, regexp_fast_tier_t tier, regexp_fast_t *fast) {
  ant_regex_state_t *state = js->regex_state;
  if (!state || vtype(rx) != kTypeObject) return false;

  ant_object_t *obj = js_obj_ptr(rx);
  if (
    js->regexp.protector_invalid ||
    obj->shape != state->instance_shape.shape ||
    obj->proto != js->builtins.regexp_proto ||
    !obj->flags.regexp_brand || obj->flags.is_exotic
  ) return tier == REGEXP_FAST_BUILTIN_EXEC && regexp_fast_get_builtin_exec(js, rx, obj, fast);

  if (vtype(ant_object_prop_get_unchecked(obj, REGEXP_LAST_INDEX_SLOT)) != kTypeNumber) return false;
  compiled_regex_cache_entry_t *compiled = js_get_native(rx, REGEXP_NATIVE_TAG);
  if (!compiled) compiled = regex_get_or_compile(js, rx, regexp_flags_mask(js, rx, NULL), NULL);
  if (!compiled) return false;

  *fast = (regexp_fast_t){
    .obj = obj, .compiled = compiled,
    .li_slot = REGEXP_LAST_INDEX_SLOT, .flags = compiled->flags_mask,
  };
  return true;
}

static inline double regexp_fast_last_index(const regexp_fast_t *fast) {
  return fast->obj ? tod(ant_object_prop_get_unchecked(fast->obj, fast->li_slot)) : 0;
}

static __attribute__((always_inline)) inline ant_value_t regexp_fast_set_last_index(
  ant_t *js, ant_value_t rx, const regexp_fast_t *fast, double value
) {
  if (fast->obj) {
    ant_object_prop_set_unchecked(fast->obj, fast->li_slot, tov(value));
    return js_mkundef();
  }
  if (vtype(rx) == kTypeUndefined) return js_mkundef();
  return regexp_set_lastindex(js, fast->compiled, rx, tov(value));
}

static __attribute__((noinline)) ant_value_t regexp_read_lastindex_slow(
  ant_t *js, compiled_regex_cache_entry_t *compiled_hint, ant_value_t regexp, ant_value_t str_arg, double *out
) {
  ant_object_t *lastindex_obj;
  uint32_t lastindex_slot;
  ant_value_t li_val;
  if (compiled_hint) regexp_lastindex_cache_location(js, compiled_hint, regexp);
  if (regexp_lastindex_fast_location(compiled_hint, regexp, &lastindex_obj, &lastindex_slot))
    li_val = ant_object_prop_get_unchecked(lastindex_obj, lastindex_slot);
  else {
    ant_prop_loc_t lastindex_off = lkp(js, regexp, "lastIndex", 9);
    li_val = lastindex_off.obj ? js_prop_load(lastindex_off) : js_mkundef();
  }
  if (vtype(li_val) == kTypeNumber) {
    *out = tod(li_val);
    return js_mkundef();
  }
  GC_ROOT_SAVE(li_mark, js);
  GC_ROOT_PIN(js, regexp);
  GC_ROOT_PIN(js, str_arg);
  size_t length = 0;
  ant_value_t status = js_to_length_slow(js, li_val, &length);
  GC_ROOT_RESTORE(js, li_mark);
  if (is_err(status)) return status;
  *out = (double)length;
  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : js_mkundef();
}

static ant_value_t regexp_exec_result(
  ant_t *js, ant_value_t regexp, const compiled_regex_cache_entry_t *compiled, ant_value_t str_arg,
  utf16_pos_t *pos, double index, const PCRE2_SIZE *ovector, uint32_t ovcount
) {
  const char *str_ptr = pos->str;
  bool has_indices = (compiled->flags_mask & REGEXP_FLAG_HAS_INDICES) != 0;
  ant_value_t groups_meta = vtype(regexp) != kTypeUndefined
    ? js_get_slot(regexp, SLOT_REGEXP_NAMED_GROUPS)
    : regexp_build_named_groups_meta(js, compiled);
  if (is_err(groups_meta)) return groups_meta;
  ant_value_t result_arr = js_mkarr(js);
  if (is_err(result_arr)) return result_arr;
  for (uint32_t i = 0; i < ovcount; i++) {
    PCRE2_SIZE start = ovector[2*i];
    PCRE2_SIZE end = ovector[2*i+1];
    if (start == PCRE2_UNSET) {
      js_arr_push(js, result_arr, js_mkundef());
    } else {
      ant_value_t match_str = js_mkstr_byte_range(js, str_ptr, start, end - start);
      if (is_err(match_str)) return match_str;
      js_arr_push(js, result_arr, match_str);
    }
  }

  ant_value_t indices = js_mkundef();
  if (has_indices) {
    indices = regexp_build_indices_result(js, groups_meta, pos, ovector, ovcount);
    if (is_err(indices)) return indices;
  }

  return regexp_attach_exec_result(js, &(regexp_exec_result_ctx_t) {
    .array = result_arr,
    .index = tov(index),
    .input = str_arg,
    .groups_meta = groups_meta,
    .indices = indices,
    .with_indices = has_indices,
  });
}

static __attribute__((always_inline)) inline ant_value_t regexp_exec_run(
  ant_t *js, ant_value_t regexp, const regexp_fast_t *fast,
  double last_index, ant_value_t str_arg, bool truthy_only
) {
  compiled_regex_cache_entry_t *compiled = fast->compiled;
  uint8_t flags = fast->flags;
  ant_offset_t str_len, str_off = regexp_vstr(js, str_arg, &str_len);
  const char *str_ptr = (const char *)(uintptr_t)str_off;
  bool uses_last_index = (flags & (REGEXP_FLAG_GLOBAL | REGEXP_FLAG_STICKY)) != 0;
  PCRE2_SIZE start = 0;
  utf16_pos_t pos = {0};
  if (uses_last_index || !truthy_only) utf16_pos_init(&pos, str_ptr, str_len);
  if (uses_last_index && !regexp_pos_last_index(&pos, last_index, flags, &start)) {
    ant_value_t stored = regexp_fast_set_last_index(js, regexp, fast, 0);
    return is_err(stored) ? stored : js_mknull();
  }

  regex_match_scope_t scope = {0};
  PCRE2_SIZE *ovector;
  uint32_t ovcount;
  int rc = regexp_match(
    js, compiled, str_ptr, str_len, start, (flags & REGEXP_FLAG_STICKY) != 0,
    &scope, &ovector, &ovcount
  );
  if (rc < 0) {
    regex_match_scope_end(&scope);
    if (!uses_last_index) return js_mknull();
    ant_value_t stored = regexp_fast_set_last_index(js, regexp, fast, 0);
    return is_err(stored) ? stored : js_mknull();
  }

  update_regexp_statics(js, str_arg, ovector, ovcount);
  double index = truthy_only ? 0 : (double)utf16_pos_units(&pos, ovector[0]);
  if (uses_last_index) {
    ant_value_t stored = regexp_fast_set_last_index(js, regexp, fast, (double)utf16_pos_units(&pos, ovector[1]));
    utf16_pos_save(&pos);
    if (is_err(stored)) {
      regex_match_scope_end(&scope);
      return stored;
    }
  }

  if (truthy_only) {
    regex_match_scope_end(&scope);
    return js_true;
  }

  ant_value_t result = regexp_exec_result(js, regexp, compiled, str_arg, &pos, index, ovector, ovcount);
  utf16_pos_save(&pos);
  regex_match_scope_end(&scope);
  return result;
}

static __attribute__((noinline))
ant_value_t regexp_exec_general(ant_t *js, ant_value_t regexp, ant_value_t str_arg, bool truthy_only) {
  compiled_regex_cache_entry_t *compiled_hint;
  uint8_t flags_mask = regexp_flags_mask(js, regexp, &compiled_hint);

  double li = 0;
  ant_object_t *lastindex_obj = js_obj_ptr(regexp);
  uint32_t lastindex_slot = REGEXP_LAST_INDEX_SLOT;
  ant_value_t li_val = js_mkundef();
  bool known = lastindex_obj && !lastindex_obj->flags.is_exotic && js->regex_state &&
    lastindex_obj->shape == js->regex_state->instance_shape.shape;
  if (!known) known = regexp_lastindex_fast_location(compiled_hint, regexp, &lastindex_obj, &lastindex_slot);
  if (known && !is_tagged(li_val = ant_object_prop_get_unchecked(lastindex_obj, lastindex_slot))) li = tod(li_val);
  else {
    ant_value_t status = regexp_read_lastindex_slow(js, compiled_hint, regexp, str_arg, &li);
    if (is_err(status)) return status;
    flags_mask = regexp_flags_mask(js, regexp, &compiled_hint);
  }

  regexp_fast_t fast = {
    .compiled = regex_get_or_compile(js, regexp, flags_mask, compiled_hint),
    .flags = flags_mask,
  };
  if (!fast.compiled) {
    if (!(flags_mask & (REGEXP_FLAG_GLOBAL | REGEXP_FLAG_STICKY))) return js_mknull();
    ant_value_t stored = regexp_set_lastindex(js, NULL, regexp, tov(0));
    return is_err(stored) ? stored : js_mknull();
  }
  regexp_lastindex_cache_location(js, fast.compiled, regexp);
  return regexp_exec_run(js, regexp, &fast, li, str_arg, truthy_only);
}

static __attribute__((aligned(16)))
ant_value_t regexp_exec_internal(ant_t *js, ant_value_t regexp, ant_value_t str_arg, bool truthy_only) {
  ant_object_t *obj = js_obj_ptr(regexp);
  ant_regex_state_t *state = js->regex_state;
  if (!state || obj->shape != state->instance_shape.shape || obj->flags.is_exotic)
    return regexp_exec_general(js, regexp, str_arg, truthy_only);
  compiled_regex_cache_entry_t *compiled = js_get_native(regexp, REGEXP_NATIVE_TAG);
  if (!compiled) compiled = regex_get_or_compile(js, regexp, regexp_flags_mask(js, regexp, NULL), NULL);
  if (!compiled) return regexp_exec_general(js, regexp, str_arg, truthy_only);
  ant_value_t li_val = ant_object_prop_get_unchecked(obj, REGEXP_LAST_INDEX_SLOT);
  if (vtype(li_val) != kTypeNumber) return regexp_exec_general(js, regexp, str_arg, truthy_only);

  regexp_fast_t fast = {
    .obj = obj, .compiled = compiled,
    .li_slot = REGEXP_LAST_INDEX_SLOT, .flags = compiled->flags_mask,
  };
  return regexp_exec_run(js, regexp, &fast, tod(li_val), str_arg, truthy_only);
}

static ant_value_t builtin_regexp_exec(ant_params_t) {
  ant_value_t regexp = js->this_val;
  if (!regexp_has_internal_slots(js, regexp))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype.exec called on incompatible receiver");

  ant_value_t str_arg = nargs > 0 ? coerce_to_str(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str_arg)) return str_arg;

  return regexp_exec_internal(js, regexp, str_arg, false);
}

static ant_value_t builtin_regexp_toString(ant_params_t) {
  ant_value_t regexp = js->this_val;
  if (!is_object_type(regexp))
    return js_mkerr_typed(js, JS_ERR_TYPE, "toString called on non-object");

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, regexp);

  ant_value_t source_val = js_getprop_fallback(js, regexp, "source");
  if (is_err(source_val)) {
    GC_ROOT_RESTORE(js, root_mark);
    return source_val;
  }
  
  ant_value_t source_str = js_tostring_val(js, source_val);
  GC_ROOT_PIN(js, source_str);
  if (is_err(source_str)) {
    GC_ROOT_RESTORE(js, root_mark);
    return source_str;
  }

  ant_value_t flags_val = js_getprop_fallback(js, regexp, "flags");
  if (is_err(flags_val)) {
    GC_ROOT_RESTORE(js, root_mark);
    return flags_val;
  }
  
  ant_value_t flags_str = js_tostring_val(js, flags_val);
  GC_ROOT_PIN(js, flags_str);
  if (is_err(flags_str)) {
    GC_ROOT_RESTORE(js, root_mark);
    return flags_str;
  }

  ant_offset_t src_len, src_off = vstr(js, source_str, &src_len);
  ant_offset_t fl_len, fl_off = vstr(js, flags_str, &fl_len);

  if ((size_t)fl_len > SIZE_MAX - 2 ||
      (size_t)src_len > SIZE_MAX - (size_t)fl_len - 2) {
    ant_value_t err = js_mkerr(js, "string too large");
    GC_ROOT_RESTORE(js, root_mark);
    return err;
  }
  
  size_t total = (size_t)src_len + (size_t)fl_len + 2;
  ant_value_t result = js_mkstr(js, NULL, total);
  GC_ROOT_PIN(js, result);
  if (is_err(result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  ant_flat_string_t *flat = (ant_flat_string_t *)vptr(result);
  src_off = vstr(js, source_str, &src_len);
  fl_off = vstr(js, flags_str, &fl_len);
  
  size_t n = 0;
  flat->bytes[n++] = '/';
  memcpy(flat->bytes + n, (const void *)(uintptr_t)src_off, (size_t)src_len);
  
  n += (size_t)src_len;
  flat->bytes[n++] = '/';
  memcpy(flat->bytes + n, (const void *)(uintptr_t)fl_off, (size_t)fl_len);
  
  n += (size_t)fl_len;
  flat->bytes[n] = '\0';
  str_flat_init_meta(flat, str_detect_ascii_bytes(flat->bytes, n));

  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static ant_value_t builtin_regexp_compile(ant_params_t) {
  ant_value_t rx = js->this_val;
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "compile called on non-object");

  ant_value_t pattern = js_mkstr(js, "", 0);
  ant_value_t flags = js_mkstr(js, "", 0);

  bool pattern_has_slots = nargs > 0 && regexp_has_internal_slots(js, args[0]);
  if (pattern_has_slots) {
    if (nargs > 1 && vtype(args[1]) != kTypeUndefined)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot supply flags when constructing one RegExp from another");
    ant_value_t original = js_get_slot(args[0], SLOT_DATA);
    ant_value_t original_flags = regexp_flags_value(js, args[0]);
    if (vtype(original) == kTypeString) pattern = original;
    if (vtype(original_flags) == kTypeString) flags = original_flags;
  } else if (nargs > 0 && vtype(args[0]) != kTypeUndefined) {
    pattern = js_tostring_val(js, args[0]);
    if (is_err(pattern)) return pattern;
  }
  if (!pattern_has_slots && nargs > 1 && vtype(args[1]) != kTypeUndefined) {
    flags = js_tostring_val(js, args[1]);
    if (is_err(flags)) return flags;
  }

  ant_value_t flags_error = regexp_check_flags(js, flags);
  if (is_err(flags_error)) return flags_error;
  ant_value_t pattern_error = regexp_check_pattern(js, pattern, flags);
  if (is_err(pattern_error)) return pattern_error;
  
  js_set_slot(rx, SLOT_DATA, pattern);
  
  regexp_init_flags(js, rx, flags, false);
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

static ant_value_t builtin_regexp_escape(ant_params_t) {
  if (nargs < 1 || vtype(args[0]) != kTypeString)
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.escape requires a string argument");

  ant_offset_t slen, soff = vstr(js, args[0], &slen);
  const char *src = (const char *)(uintptr_t)(soff);
  if ((size_t)slen > (SIZE_MAX - 1) / 6) return js_mkerr(js, "string too large");
  
  size_t capacity = (size_t)slen * 6 + 1;
  char static_buf[256];
  
  bool needs_free = capacity > sizeof(static_buf);
  char *buf = needs_free ? (char *)calloc(1, capacity) : static_buf;
  if (!buf) return js_mkerr(js, "oom");

  static const char hex[] = "0123456789abcdef";
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
      if (bytes <= 0) bytes = 1;
      if ((size_t)bytes > (size_t)slen - si) bytes = (int)((size_t)slen - si);
      memcpy(buf + di, src + si, (size_t)bytes);
      di += (size_t)bytes;
      si += (size_t)bytes;
      first = false;
      continue;
    }

    bool short_escape = c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
    bool hex_escape = 
      (first && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) 
      || is_other_punctuator(c) || c == ' ';

    if (short_escape) {
      buf[di++] = '\\';
      switch (c) {
        case '\t': buf[di++] = 't'; break;
        case '\n': buf[di++] = 'n'; break;
        case '\v': buf[di++] = 'v'; break;
        case '\f': buf[di++] = 'f'; break;
        default:   buf[di++] = 'r'; break;
      }
    } else if (hex_escape) {
      buf[di++] = '\\';
      buf[di++] = 'x';
      buf[di++] = hex[c >> 4];
      buf[di++] = hex[c & 0x0F];
    } else if (is_syntax_char(c)) {
      buf[di++] = '\\';
      buf[di++] = (char)c;
    } else buf[di++] = (char)c;

    si++;
    first = false;
  }

  ant_value_t result = js_mkstr(js, buf, di);
  if (needs_free) free(buf);
  return result;
}

static inline PCRE2_SIZE regexp_advance_empty_match(const char *str_ptr, ant_offset_t str_len, PCRE2_SIZE offset) {
  if (offset < (PCRE2_SIZE)str_len) return offset + (PCRE2_SIZE)utf8_char_len_at(str_ptr, str_len, (ant_offset_t)offset);
  return offset + 1;
}

typedef struct {
  const compiled_regex_cache_entry_t *compiled;
  const char *str;
  const PCRE2_SIZE *ovector;
  uint32_t ovcount;
} regexp_named_ctx_t;

static bool regexp_named_capture(void *ctx, const char *name, size_t len, repl_capture_t *out) {
  const regexp_named_ctx_t *named = ctx;
  const compiled_regex_cache_entry_t *compiled = named->compiled;
  for (uint32_t i = 0; i < compiled->namecount; i++) {
    const regexp_group_name_t *entry = &compiled->group_names[i];
    if (entry->length != len || memcmp(entry->name, name, len) != 0) continue;
    if (entry->group >= named->ovcount) continue;
    PCRE2_SIZE start = named->ovector[2 * entry->group];
    if (start == PCRE2_UNSET) continue;
    *out = (repl_capture_t){ named->str + start, (size_t)(named->ovector[2 * entry->group + 1] - start) };
    return true;
  }
  return true;
}

static ant_value_t regexp_build_groups_object(
  ant_t *js, const compiled_regex_cache_entry_t *compiled,
  const char *str_ptr, const PCRE2_SIZE *ovector, uint32_t ovcount
) {
  ant_value_t groups = Ant_Object_CreateBare(js);
  if (is_err(groups)) return groups;
  js_set_proto_init(groups, js_mknull());

  for (uint32_t i = 0; i < compiled->namecount; i++) {
    const regexp_group_name_t *entry = &compiled->group_names[i];
    PCRE2_SIZE start = entry->group < ovcount ? ovector[2 * entry->group] : PCRE2_UNSET;
    ant_value_t value = js_mkundef();
    if (start != PCRE2_UNSET) {
      value = js_mkstr_byte_range(js, str_ptr, start, ovector[2 * entry->group + 1] - start);
      if (is_err(value)) return value;
    } else if (lkp(js, groups, entry->name, entry->length).obj) continue;
    ant_value_t status = setprop_cstr(js, groups, entry->name, entry->length, value);
    if (is_err(status)) return status;
  }

  return groups;
}

typedef struct {
  const regexp_fast_t *fast;
  ant_value_t str;
  const char *str_ptr;
  ant_offset_t str_len;
  PCRE2_SIZE offset;
  utf16_pos_t pos;
  regex_match_scope_t scope;
  bool global;
  bool sticky;
  bool done;
  size_t count;
  PCRE2_SIZE last_end;
} regexp_fast_scan_t;

static __attribute__((always_inline)) inline void regexp_fast_scan_begin(
  ant_t *js, regexp_fast_scan_t *scan, const regexp_fast_t *fast, ant_value_t str
) {
  ant_offset_t str_len, str_off = regexp_vstr(js, str, &str_len);
  scan->fast = fast;
  scan->str = str;
  scan->str_ptr = (const char *)(uintptr_t)str_off;
  scan->str_len = str_len;
  scan->offset = 0;
  scan->scope.match_data = NULL;
  scan->scope.line_checked = false;
  scan->global = (fast->flags & REGEXP_FLAG_GLOBAL) != 0;
  scan->sticky = (fast->flags & REGEXP_FLAG_STICKY) != 0;
  scan->done = false;
  scan->count = 0;
  scan->last_end = 0;
  if (scan->global || !scan->sticky) return;
  utf16_pos_init(&scan->pos, scan->str_ptr, str_len);
  if (regexp_pos_last_index(&scan->pos, regexp_fast_last_index(fast), fast->flags, &scan->offset)) return;
  regexp_fast_set_last_index(js, js_mkundef(), fast, 0);
  scan->done = true;
}

static __attribute__((always_inline)) inline int regexp_fast_scan_next(
  ant_t *js, regexp_fast_scan_t *scan, PCRE2_SIZE **ovector, uint32_t *ovcount, bool literal
) {
  if (scan->done || scan->offset > (PCRE2_SIZE)scan->str_len) return PCRE2_ERROR_NOMATCH;
  int rc = literal
    ? regexp_match_literal(
        scan->fast->compiled, scan->str_ptr, scan->str_len, scan->offset,
        &scan->scope, ovector, ovcount
      )
    : regexp_match(
        js, scan->fast->compiled, scan->str_ptr, scan->str_len, scan->offset,
        scan->sticky, &scan->scope, ovector, ovcount
      );
  if (rc < 0) {
    scan->done = true;
    return rc;
  }

  PCRE2_SIZE start = (*ovector)[0], end = (*ovector)[1];
  update_regexp_statics(js, scan->str, *ovector, *ovcount);
  scan->count++;
  scan->last_end = end;
  if (!scan->global) scan->done = true;
  scan->offset = start == end ? regexp_advance_empty_match(scan->str_ptr, scan->str_len, end) : end;
  return rc;
}

static __attribute__((always_inline)) inline void regexp_fast_scan_end(ant_t *js, regexp_fast_scan_t *scan) {
  regex_match_scope_end(&scan->scope);
  if (scan->global) regexp_fast_set_last_index(js, js_mkundef(), scan->fast, 0);
  else if (scan->sticky) regexp_fast_set_last_index(
    js, js_mkundef(), scan->fast,
    scan->count ? (double)utf16_pos_units(&scan->pos, scan->last_end) : 0
  );
}

static ant_value_t regexp_exec_with_exec_fn(ant_t *js, ant_value_t rx, ant_value_t str, ant_value_t exec_fn) {
  if (vtype(exec_fn) == kTypeFunction || vtype(exec_fn) == kTypeBuiltin) {
    ant_value_t call_args[1] = { str };
    ant_value_t result = sv_vm_call(js->vm, js, exec_fn, rx, call_args, 1, NULL, js_mkundef());
    if (is_err(result)) return result;
    if (!is_object_type(result) && vtype(result) != kTypeNull)
      return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp exec returned non-object");
    return result;
  }

  ant_value_t call_args[1] = { str };
  return sv_call_native_this(js, builtin_regexp_exec, rx, call_args, 1);
}

static ant_value_t regexp_exec_abstract(ant_t *js, ant_value_t rx, ant_value_t str) {
  ant_value_t exec_fn = js_getprop_fallback(js, rx, "exec");
  if (is_err(exec_fn)) return exec_fn;
  return regexp_exec_with_exec_fn(js, rx, str, exec_fn);
}

static ant_value_t regexp_read_flags(ant_t *js, ant_value_t rx, bool *global, bool *full_unicode) {
  ant_value_t flags_val = js_getprop_fallback(js, rx, "flags");
  if (is_err(flags_val)) return flags_val;
  ant_value_t flags_str = js_tostring_val(js, flags_val);
  if (is_err(flags_str)) return flags_str;
  ant_offset_t len, off = vstr(js, flags_str, &len);
  const char *flags = (const char *)(uintptr_t)off;
  if (global) *global = memchr(flags, 'g', (size_t)len) != NULL;
  if (full_unicode) *full_unicode = memchr(flags, 'u', (size_t)len) || memchr(flags, 'v', (size_t)len);
  return js_mkundef();
}

static ant_value_t regexp_advance_last_index(ant_t *js, ant_value_t rx, ant_value_t str, bool full_unicode) {
  ant_value_t li_val = js_getprop_fallback(js, rx, "lastIndex");
  if (is_err(li_val)) return li_val;
  double li = vtype(li_val) == kTypeNumber ? tod(li_val) : 0;
  ant_offset_t len, off = vstr(js, str, &len);
  double next = regexp_advance_string_index(js, (const char *)(uintptr_t)off, len, li, full_unicode);
  return js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), tov(next));
}

bool regexp_exec_truthy_try_fast(
  ant_t *js,
  ant_value_t call_func,
  ant_value_t regexp,
  ant_value_t arg,
  ant_value_t *out_result
) {
  if (!out_result || vtype(call_func) != kTypeBuiltin) return false;
  if (!js_cfunc_same_entrypoint(call_func, builtin_regexp_exec)) return false;
  if (!is_object_type(regexp) || vtype(arg) != kTypeString) return false;

  ant_value_t result = regexp_exec_internal(js, regexp, arg, true);
  if (is_err(result)) {
    *out_result = result;
    return true;
  }

  *out_result = mkval(kTypeBool, vtype(result) != kTypeNull ? 1 : 0);
  return true;
}

static ant_value_t builtin_regexp_test(ant_params_t) {
  ant_value_t regexp = js->this_val;
  if (!is_object_type(regexp))
    return js_mkerr_typed(js, JS_ERR_TYPE, "test called on non-object");
  ant_value_t str_arg = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str_arg)) return str_arg;
  regexp_fast_t fast;
  if (regexp_fast_get(js, regexp, REGEXP_FAST_PRISTINE, &fast)) {
    ant_value_t result = regexp_exec_internal(js, regexp, str_arg, true);
    return is_err(result) ? result : mkval(kTypeBool, vtype(result) != kTypeNull ? 1 : 0);
  }
  ant_value_t exec_fn = js_getprop_fallback(js, regexp, "exec");
  if (is_err(exec_fn)) return exec_fn;

  ant_value_t result;
  if (vtype(exec_fn) == kTypeBuiltin && js_cfunc_same_entrypoint(exec_fn, builtin_regexp_exec)) {
    result = regexp_exec_internal(js, regexp, str_arg, true);
  } else result = regexp_exec_with_exec_fn(js, regexp, str_arg, exec_fn);
  
  if (is_err(result)) return result;
  return mkval(kTypeBool, vtype(result) != kTypeNull ? 1 : 0);
}

static inline bool regexp_is_proto(ant_t *js, ant_value_t value) {
  return is_object_type(value) && vdata(js_as_obj(value)) == vdata(js_as_obj(js->builtins.regexp_proto));
}

static ant_value_t regexp_flag_getter(ant_t *js, ant_value_t rx, uint8_t bit, const char *name) {
  if (regexp_has_internal_slots(js, rx)) return js_bool((regexp_flags_mask(js, rx, NULL) & bit) != 0);
  if (regexp_is_proto(js, rx)) return js_mkundef();
  return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype.%s getter called on incompatible receiver", name);
}

#define REGEXP_FLAG(id, name, ch)                                       \
  static ant_value_t builtin_regexp_get_##name(ant_params_t) {          \
    return regexp_flag_getter(js, js->this_val, REGEXP_FLAG_##id, #name); \
  }
#include "modules/regex_flags.h"

typedef struct {
  const char *name;
  uint8_t len;
  uint8_t bit;
  char ch;
  ant_cfunc_t getter;
} regexp_flag_info_t;

static const regexp_flag_info_t regexp_flag_info[REGEXP_FLAG_COUNT] = {
#define REGEXP_FLAG(id, name, ch) { #name, sizeof(#name) - 1, REGEXP_FLAG_##id, ch, builtin_regexp_get_##name },
#include "modules/regex_flags.h"
};

static bool regexp_flag_getter_is_builtin(ant_t *js, const ant_shape_t *own_shape, const regexp_flag_info_t *flag) {
  const char *key = intern_string(js, flag->name, flag->len);
  if (!key || (own_shape && ant_shape_lookup_interned(own_shape, key) >= 0)) return false;
  
  ant_object_t *proto = js_obj_ptr(js->builtins.regexp_proto);
  if (!proto || !proto->shape || proto->flags.is_exotic) return false;
  int32_t slot = ant_shape_lookup_interned(proto->shape, key);
  
  const ant_shape_prop_t *prop = slot >= 0 ? ant_shape_prop_at(proto->shape, (uint32_t)slot) : NULL;
  return prop && prop->has_getter && js_cfunc_same_entrypoint(prop->getter, flag->getter);
}

static bool regexp_shape_flags_pristine(ant_t *js, const ant_shape_t *own_shape) {
  ant_regex_state_t *state = js->regex_state;
  if (!state) return false;
  if (
    state->pristine_flags_epoch == js->ic.epoch &&
    state->pristine_flags_shape == own_shape &&
    state->pristine_flags_realm == js->realm
  ) return true;
  
  for (int i = 0; i < REGEXP_FLAG_COUNT; i++)
    if (!regexp_flag_getter_is_builtin(js, own_shape, &regexp_flag_info[i])) return false;
  
  state->pristine_flags_epoch = js->ic.epoch;
  state->pristine_flags_shape = own_shape;
  state->pristine_flags_realm = js->realm;
  return true;
}

static bool regexp_flags_pristine(ant_t *js, ant_value_t rx) {
  if (!regexp_has_internal_slots(js, rx)) return false;
  ant_object_t *obj = js_obj_ptr(rx);
  return 
    !obj->flags.is_exotic && 
    js_get_proto(js, rx) == js->builtins.regexp_proto &&
    regexp_shape_flags_pristine(js, obj->shape);
}

static ant_value_t regexp_read_flag(ant_t *js, ant_value_t rx, int index) {
  const regexp_flag_info_t *flag = &regexp_flag_info[index];
  if (regexp_flags_pristine(js, rx)) return js_bool((regexp_flags_mask(js, rx, NULL) & flag->bit) != 0);
  return js_getprop_fallback(js, rx, flag->name);
}

ant_value_t regexp_source_value(ant_t *js, ant_value_t rx) {
  ant_value_t pattern = js_get_slot(rx, SLOT_DATA);
  return vtype(pattern) == kTypeString ? regexp_escape_source(js, pattern) : ANT_STRING("(?:)");
}

static ant_value_t builtin_regexp_source_getter(ant_params_t) {
  ant_value_t rx = js->this_val;
  if (regexp_has_internal_slots(js, rx)) return regexp_source_value(js, rx);
  if (regexp_is_proto(js, rx)) return ANT_STRING("(?:)");
  return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype.source getter called on incompatible receiver");
}

static ant_value_t builtin_regexp_flags_getter(ant_params_t) {
  ant_value_t rx = js->this_val;
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype.flags called on non-object");
  if (regexp_flags_pristine(js, rx)) return regexp_flags_value(js, rx);

  char buf[REGEXP_FLAG_COUNT]; int n = 0;
  for (int i = 0; i < REGEXP_FLAG_COUNT; i++) {
    ant_value_t v = regexp_read_flag(js, rx, i);
    if (is_err(v)) return v;
    if (js_truthy(js, v)) buf[n++] = regexp_flag_info[i].ch;
  }

  return js_mkstr(js, buf, n);
}

static ant_value_t regexp_match_fast_global(ant_t *js, const regexp_fast_t *fast, ant_value_t str) {
  ant_value_t matches = js_mkarr(js);
  if (is_err(matches)) return matches;

  regexp_fast_scan_t scan;
  regexp_fast_scan_begin(js, &scan, fast, str);
  ant_value_t error = js_mkundef();
  for (;;) {
    PCRE2_SIZE *ovector;
    uint32_t ovcount;
    int rc = regexp_fast_scan_next(js, &scan, &ovector, &ovcount, false);
    if (rc == PCRE2_ERROR_NOMEMORY) { error = js_mkerr(js, "oom"); break; }
    if (rc < 0) break;
    ant_value_t match = js_mkstr_byte_range(js, scan.str_ptr, ovector[0], ovector[1] - ovector[0]);
    if (is_err(match)) { error = match; break; }
    js_arr_push(js, matches, match);
  }
  regexp_fast_scan_end(js, &scan);

  if (is_err(error)) return error;
  return scan.count == 0 ? js_mknull() : matches;
}

static ant_value_t regexp_match_fast(ant_t *js, ant_value_t rx, const regexp_fast_t *fast, ant_value_t str) {
  return (fast->flags & REGEXP_FLAG_GLOBAL)
    ? regexp_match_fast_global(js, fast, str)
    : regexp_exec_run(js, rx, fast, regexp_fast_last_index(fast), str, false);
}

static ant_value_t regexp_symbol_match_impl(ant_t *js, ant_value_t rx, ant_value_t str) {
  regexp_fast_t fast;
  if (regexp_fast_get(js, rx, REGEXP_FAST_PRISTINE, &fast)) return regexp_match_fast(js, rx, &fast, str);

  bool global = false;
  bool full_unicode = false;
  ant_value_t flags_status = regexp_read_flags(js, rx, &global, &full_unicode);
  if (is_err(flags_status)) return flags_status;

  if (!global) return regexp_exec_abstract(js, rx, str);

  if (regexp_fast_get(js, rx, REGEXP_FAST_BUILTIN_EXEC, &fast) && (fast.flags & REGEXP_FLAG_GLOBAL))
    return regexp_match_fast_global(js, &fast, str);

  js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), tov(0));

  ant_value_t A = js_mkarr(js);
  if (is_err(A)) return A;
  ant_offset_t n = 0;

  for (;;) {
    ant_value_t result = regexp_exec_abstract(js, rx, str);
    if (is_err(result)) return result;
    if (vtype(result) == kTypeNull) return n == 0 ? js_mknull() : mkval(kTypeArray, vdata(A));

    ant_value_t match_str = js_tostring_val(js, js_arr_get(js, result, 0));
    if (is_err(match_str)) return match_str;
    js_arr_push(js, A, match_str);
    n++;

    ant_offset_t mlen;
    vstr(js, match_str, &mlen);
    if (mlen == 0) {
      ant_value_t advanced = regexp_advance_last_index(js, rx, str, full_unicode);
      if (is_err(advanced)) return advanced;
    }
  }
}

static ant_value_t builtin_regexp_symbol_match(ant_params_t) {
  ant_value_t rx = js->this_val;
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@match] called on non-object");

  ant_value_t str = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str)) return str;
  return regexp_symbol_match_impl(js, rx, str);
}


enum {
  MATCHALL_DONE = 1 << 0,
  MATCHALL_GLOBAL = 1 << 1,
  MATCHALL_UNICODE = 1 << 2,
};

static ant_value_t regexp_matchall_next(ant_params_t) {
  ant_value_t iter = js->this_val;
  ant_value_t rx = js_get_slot(iter, SLOT_MATCHALL_RX);
  ant_value_t str = js_get_slot(iter, SLOT_MATCHALL_STR);
  ant_value_t state_val = js_get_slot(iter, SLOT_MATCHALL_STATE);
  uint32_t state = vtype(state_val) == kTypeNumber ? (uint32_t)js_getnum(state_val) : MATCHALL_DONE;
  
  if (state & MATCHALL_DONE)
    return js_iter_result(js, false, js_mkundef());

  regexp_fast_t fast;
  if (regexp_fast_get(js, rx, REGEXP_FAST_PRISTINE, &fast)) {
    ant_value_t result = regexp_exec_internal(js, rx, str, false);
    if (is_err(result)) return result;
    if (vtype(result) == kTypeNull || !(state & MATCHALL_GLOBAL))
      js_set_slot(iter, SLOT_MATCHALL_STATE, js_mknum(state | MATCHALL_DONE));
    if (vtype(result) == kTypeNull) return js_iter_result(js, false, js_mkundef());
    if (state & MATCHALL_GLOBAL) {
      ant_offset_t match_len;
      vstr(js, js_arr_get(js, result, 0), &match_len);
      if (match_len == 0) {
        ant_offset_t str_len, str_off = vstr(js, str, &str_len);
        double li = regexp_fast_last_index(&fast);
        double next = regexp_advance_string_index(
          js, (const char *)(uintptr_t)str_off, str_len, li,
          (state & MATCHALL_UNICODE) != 0
        );
        regexp_fast_set_last_index(js, rx, &fast, next);
      }
    }
    return js_iter_result(js, true, result);
  }

  ant_value_t result = regexp_exec_abstract(js, rx, str);
  if (is_err(result)) return result;

  if (vtype(result) == kTypeNull) {
    js_set_slot(iter, SLOT_MATCHALL_STATE, js_mknum(state | MATCHALL_DONE));
    return js_iter_result(js, false, js_mkundef());
  }

  if (state & MATCHALL_GLOBAL) {
    ant_value_t match_str = js_tostring_val(js, js_arr_get(js, result, 0));
    if (is_err(match_str)) return match_str;
    ant_offset_t mlen;
    vstr(js, match_str, &mlen);
    if (mlen == 0) {
      ant_value_t advanced = regexp_advance_last_index(js, rx, str, (state & MATCHALL_UNICODE) != 0);
      if (is_err(advanced)) return advanced;
    }
  } else js_set_slot(iter, SLOT_MATCHALL_STATE, js_mknum(state | MATCHALL_DONE));

  return js_iter_result(js, true, result);
}

static bool regexp_literal_init_fast(ant_t *js, ant_value_t regexp_obj, ant_value_t pattern, ant_value_t flags, compiled_regex_cache_entry_t *compiled);

static ant_value_t regexp_matchall_iterator(ant_t *js, ant_value_t matcher, ant_value_t str, uint32_t state) {
  ant_value_t iter = Ant_Object_CreateBare(js);
  if (is_err(iter)) return iter;
  js_set_slot(iter, SLOT_MATCHALL_RX, matcher);
  js_set_slot(iter, SLOT_MATCHALL_STR, str);
  js_set_slot(iter, SLOT_MATCHALL_STATE, js_mknum(state));
  js_set_proto_init(iter, js->builtins.regexp_matchall_iter_proto_val);
  return iter;
}

static ant_value_t regexp_matchall_fast(ant_t *js, ant_value_t rx, const regexp_fast_t *fast, ant_value_t str) {
  ant_value_t matcher = mkobj(js, 0);
  if (is_err(matcher)) return matcher;
  js_set_proto_init(matcher, js->builtins.regexp_proto);
  ant_value_t flags = regexp_flags_value(js, rx);
  if (!regexp_literal_init_fast(js, matcher, js_get_slot(rx, SLOT_DATA), flags, fast->compiled))
    return js_mkundef();
  ant_object_prop_set_unchecked(
    js_obj_ptr(matcher), REGEXP_LAST_INDEX_SLOT,
    ant_object_prop_get_unchecked(fast->obj, fast->li_slot)
  );
  uint32_t state = 0;
  if (fast->flags & REGEXP_FLAG_GLOBAL) state |= MATCHALL_GLOBAL;
  if (fast->flags & (REGEXP_FLAG_UNICODE | REGEXP_FLAG_UNICODE_SET)) state |= MATCHALL_UNICODE;
  return regexp_matchall_iterator(js, matcher, str, state);
}

static ant_value_t builtin_regexp_symbol_matchAll(ant_params_t) {
  ant_value_t rx = js->this_val;
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@matchAll] called on non-object");

  ant_value_t str = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str)) return str;

  regexp_fast_t fast;
  if (regexp_fast_get(js, rx, REGEXP_FAST_PRISTINE, &fast)) {
    ant_value_t iter = regexp_matchall_fast(js, rx, &fast, str);
    if (vtype(iter) != kTypeUndefined) return iter;
  }

  ant_value_t C = regexp_species_ctor(js, rx, "RegExp.prototype[@@matchAll]");
  if (is_err(C)) return C;

  ant_value_t flags_val = js_getprop_fallback(js, rx, "flags");
  if (is_err(flags_val)) return flags_val;
  ant_value_t flags_str = js_tostring_val(js, flags_val);
  if (is_err(flags_str)) return flags_str;

  ant_value_t ctor_args[2] = { rx, flags_str };
  ant_value_t new_rx = regexp_species_construct(js, rx, C, ctor_args, 2);
  if (is_err(new_rx)) return new_rx;

  ant_value_t li_val = js_getprop_fallback(js, rx, "lastIndex");
  if (is_err(li_val)) return li_val;
  size_t last_index = 0;
  ant_value_t status = js_to_length_fast(js, li_val, &last_index);
  if (is_err(status)) return status;
  status = js_setprop(js, new_rx, js_mkstr(js, "lastIndex", 9), tov((double)last_index));
  if (is_err(status)) return status;

  uint32_t state = 0;
  ant_offset_t flags_len, flags_off = vstr(js, flags_str, &flags_len);
  const char *flags = (const char *)(uintptr_t)flags_off;
  if (memchr(flags, 'g', flags_len)) state |= MATCHALL_GLOBAL;
  if (memchr(flags, 'u', flags_len) || memchr(flags, 'v', flags_len)) state |= MATCHALL_UNICODE;

  return regexp_matchall_iterator(js, new_rx, str, state);
}

static ant_value_t builtin_string_matchAll(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, "matchAll");
  
  if (is_err(str)) return str;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "matchAll requires at least 1 argument");

  regexp_fast_t fast;
  if (regexp_fast_get(js, args[0], REGEXP_FAST_PRISTINE, &fast)) {
    if (!(fast.flags & REGEXP_FLAG_GLOBAL))
      return js_mkerr_typed(js, JS_ERR_TYPE, "String.prototype.matchAll called with a non-global RegExp");
    ant_value_t iter = regexp_matchall_fast(js, args[0], &fast, str);
    if (vtype(iter) != kTypeUndefined) return iter;
  }

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
      js, args[0], js->sym.matchAll_sym, args[0], call_args, 1, &called
    );
    
    if (is_err(dispatched)) return dispatched;
    if (called) return dispatched;
  }

  ant_value_t pattern_str = js_tostring_val(js, args[0]);
  if (is_err(pattern_str)) return pattern_str;

  ant_value_t ctor_args[2] = { pattern_str, js_mkstr(js, "g", 1) };
  ant_value_t regexp_ctor = js->builtins.regexp_ctor_value;
  ant_value_t rx = sv_vm_call(js->vm, js, regexp_ctor, js_mkundef(), ctor_args, 2, NULL, regexp_ctor);
  if (is_err(rx)) return rx;

  ant_value_t ma_args[1] = { str };
  return sv_call_native_this(js, builtin_regexp_symbol_matchAll, rx, ma_args, 1);
}

static __attribute__((noinline)) const char *find_bytes_long(
  const char *haystack,
  size_t haystack_len,
  const char *needle,
  size_t needle_len
) {
  size_t prefix_inline[128];
  size_t *prefix = NULL;
  if (needle_len <= sizeof(prefix_inline) / sizeof(prefix_inline[0]))
    prefix = prefix_inline;
  else if (needle_len <= SIZE_MAX / sizeof(*prefix))
    prefix = malloc(needle_len * sizeof(*prefix));
  if (!prefix) {
    size_t last = haystack_len - needle_len;
    for (size_t i = 0; i <= last; i++) {
      if (
        haystack[i] == needle[0] &&
        memcmp(haystack + i, needle, needle_len) == 0
      ) return haystack + i;
    }
    return NULL;
  }

  prefix[0] = 0;
  for (size_t i = 1, matched = 0; i < needle_len; i++) {
    while (matched && needle[i] != needle[matched])
      matched = prefix[matched - 1];
    if (needle[i] == needle[matched]) matched++;
    prefix[i] = matched;
  }

  const char *result = NULL;
  for (size_t i = 0, matched = 0; i < haystack_len; i++) {
    while (matched && haystack[i] != needle[matched])
      matched = prefix[matched - 1];
    if (haystack[i] == needle[matched]) matched++;
    if (matched == needle_len) {
      result = haystack + i + 1 - needle_len;
      break;
    }
  }

  if (prefix != prefix_inline) free(prefix);
  return result;
}

static const char *find_bytes(const char *haystack, ant_offset_t haystack_len, const char *needle, ant_offset_t needle_len) {
  if (needle_len == 0 || needle_len > haystack_len) return NULL;
  if (needle_len > REGEXP_PLAIN_LITERAL_MAX) {
    return find_bytes_long(
      haystack, (size_t)haystack_len, needle, (size_t)needle_len
    );
  }

  const char *end = haystack + haystack_len - needle_len + 1;
  if (needle_len > 1) {
    const char *last = memchr(haystack + needle_len - 1,
        (unsigned char)needle[needle_len - 1], (size_t)(end - haystack));
    if (!last) return NULL;
    haystack = last - needle_len + 1;
  }
  while (haystack < end) {
    const char *match = memchr(haystack, (unsigned char)needle[0], (size_t)(end - haystack));
    if (!match) return NULL;
    if (needle_len == 1 || memcmp(match, needle, needle_len) == 0) return match;
    haystack = match + 1;
  }
  return NULL;
}

static bool str_buf_append(char **buf, size_t *len, size_t *cap, const char *data, size_t n);

static bool regexp_literal_exec_builtin_guard(ant_t *js) {
  return js->regex_state && !js->regexp.protector_invalid;
}

static bool regexp_canonical_flags_mask(ant_t *js, ant_value_t flags, uint8_t *out) {
  ant_offset_t flen;
  const char *fstr = (const char *)(uintptr_t)regexp_vstr(js, flags, &flen);
  uint8_t mask = 0;
  for (ant_offset_t i = 0; i < flen; i++) {
    uint8_t bit = regexp_parse_flags_mask(fstr + i, 1);
    if (bit <= mask) return false;
    mask |= bit;
  }
  *out = mask;
  return true;
}

static __attribute__((always_inline)) inline compiled_regex_cache_entry_t *regexp_literal_compiled(
  ant_t *js, ant_value_t pattern, ant_value_t flags
) {
  ant_regex_state_t *state = js->regex_state;
  if (!state || vtype(pattern) != kTypeString || vtype(flags) != kTypeString) return NULL;

  const regexp_site_entry_t *site = regexp_site_for(state, pattern);
  if (site->pattern == pattern && site->flags == flags && site->epoch == gc_strings_sweep_epoch())
    return site->entry;

  uint8_t mask;
  if (!regexp_canonical_flags_mask(js, flags, &mask)) return NULL;
  return regexp_site_lookup(js, pattern, flags, mask);
}

static bool regexp_literal_init_fast(
  ant_t *js, ant_value_t regexp_obj, ant_value_t pattern, ant_value_t flags,
  compiled_regex_cache_entry_t *compiled
) {
  ant_regex_state_t *state = js->regex_state;
  if (
    !state || !state->instance_shape.shape ||
    vtype(pattern) != kTypeString || vtype(flags) != kTypeString
  ) return false;

  ant_object_t *obj = js_obj_ptr(regexp_obj);
  if (
    obj->shape != state->instance_shape.base || obj->prop_count != 0 ||
    ant_object_inobj_limit(obj) <= REGEXP_LAST_INDEX_SLOT || obj->finalizer || obj->native_tag
  ) return false;

  uint8_t mask;
  if (!compiled) compiled = regexp_literal_compiled(js, pattern, flags);
  else if (!regexp_canonical_flags_mask(js, flags, &mask) || compiled->flags_mask != mask) return false;
  if (!compiled) return false;
  obj->u.data.value = pattern;

  if (compiled->namecount) {
    if (!regexp_attach_compiled(js, regexp_obj, compiled)) return false;
  } else {
    compiled->object_refs++;
    obj->native_ptr = compiled;
    obj->native_tag = REGEXP_NATIVE_TAG;
    obj->finalizer = regexp_object_finalize;
    state->live_object_data++;
  }

  ant_shape_transition_existing(&obj->shape, state->instance_shape.shape);
  obj->prop_count = 1;
  ant_object_prop_set_unchecked(obj, REGEXP_LAST_INDEX_SLOT, tov(0));
  obj->flags.regexp_brand = 1;
  return true;
}

static ant_value_t regexp_flags_string_for_mask(ant_t *js, uint8_t mask) {
  ant_regex_state_t *state = js->regex_state;
  ant_value_t cached = state ? state->flag_strings[mask] : 0;
  if (cached) return cached;
  char sorted[REGEXP_FLAG_COUNT]; int si = 0;
#define REGEXP_FLAG(id, name, ch) if (mask & REGEXP_FLAG_##id) sorted[si++] = ch;
#include "modules/regex_flags.h"
  if (!state) return js_mkstr(js, sorted, (size_t)si);
  ant_value_t value = js_mkstr_permanent(js, sorted, (size_t)si);
  if (!is_err(value)) state->flag_strings[mask] = value;
  return value;
}

ant_value_t regexp_flags_value(ant_t *js, ant_value_t rx) {
  ant_value_t flags = js_get_slot(rx, SLOT_REGEXP_FLAGS_STRING);
  if (vtype(flags) == kTypeString) return flags;
  compiled_regex_cache_entry_t *compiled = js_get_native(rx, REGEXP_NATIVE_TAG);
  return compiled ? regexp_flags_string_for_mask(js, compiled->flags_mask) : js_mkundef();
}

ant_value_t regexp_create_literal(ant_t *js, ant_value_t pattern, ant_value_t flags) {
  ant_value_t regexp_obj = mkobj(js, 0);
  if (is_err(regexp_obj)) return regexp_obj;
  ant_object_set_proto(js, js_obj_ptr(regexp_obj), js->builtins.regexp_proto);
  if (regexp_literal_init_fast(js, regexp_obj, pattern, flags, NULL)) return regexp_obj;

  js_set_slot(regexp_obj, SLOT_DATA, pattern);
  regexp_init_flags(js, regexp_obj, flags, true);
  
  return regexp_obj;
}

ant_value_t regexp_literal_exec_call(
  ant_t *js,
  ant_value_t pattern,
  ant_value_t flags,
  ant_value_t arg
) {
  compiled_regex_cache_entry_t *compiled;
  if (
    vtype(arg) == kTypeString && regexp_literal_exec_builtin_guard(js) &&
    (compiled = regexp_literal_compiled(js, pattern, flags))
  ) {
    regexp_fast_t fast = { .compiled = compiled, .flags = compiled->flags_mask };
    compiled->active_refs++;
    ant_value_t result = regexp_exec_run(js, js_mkundef(), &fast, 0, arg, false);
    compiled_regex_cache_release(compiled);
    return result;
  }

  ant_value_t regexp_obj = regexp_create_literal(js, pattern, flags);
  if (is_err(regexp_obj)) return regexp_obj;
  if (vtype(arg) == kTypeString && regexp_literal_exec_builtin_guard(js))
    return regexp_exec_internal(js, regexp_obj, arg, false);
  return regexp_exec_abstract(js, regexp_obj, arg);
}

typedef struct {
  PCRE2_SIZE *items;
  size_t len;
  size_t cap;
} regexp_ovector_list_t;

static bool regexp_ovector_list_push(regexp_ovector_list_t *list, const PCRE2_SIZE *ovector, size_t count) {
  if (list->len + count > list->cap) {
    size_t cap = list->cap ? list->cap * 2 : 64;
    while (cap < list->len + count) cap *= 2;
    PCRE2_SIZE *items = realloc(list->items, cap * sizeof(*items));
    if (!items) return false;
    list->items = items;
    list->cap = cap;
  }
  memcpy(list->items + list->len, ovector, count * sizeof(*ovector));
  list->len += count;
  return true;
}

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  bool scratch;
  bool ascii;
} regexp_out_t;

static inline bool regexp_bytes_known_ascii(const char *bytes) {
  return str_flat_ascii_state(str_flat_from_bytes(bytes)) == STR_ASCII_YES;
}

static bool regexp_out_begin(ant_t *js, regexp_out_t *out, size_t need) {
  ant_regex_state_t *state = js->regex_state;
  *out = (regexp_out_t){ .cap = need };
  if (state->out_busy) {
    out->buf = malloc(need);
    return out->buf != NULL;
  }
  if (state->out_cap < need) {
    char *buf = realloc(state->out_buf, need);
    if (!buf) return false;
    state->out_buf = buf;
    state->out_cap = need;
  }
  state->out_busy = true;
  *out = (regexp_out_t){ .buf = state->out_buf, .cap = state->out_cap, .scratch = true };
  return true;
}

static void regexp_out_end(ant_t *js, regexp_out_t *out) {
  ant_regex_state_t *state = js->regex_state;
  if (!out->scratch) free(out->buf);
  else if (out->cap > REGEXP_OUT_SCRATCH_MAX) {
    free(out->buf);
    state->out_buf = NULL;
    state->out_cap = 0;
  } else {
    state->out_buf = out->buf;
    state->out_cap = out->cap;
  }
  if (out->scratch) state->out_busy = false;
  *out = (regexp_out_t){0};
}

static ant_value_t regexp_out_finish(ant_t *js, regexp_out_t *out, const char *tail, size_t tail_len) {
  ant_value_t result = !str_buf_append(&out->buf, &out->len, &out->cap, tail, tail_len)
    ? js_mkerr(js, "oom")
    : out->ascii ? js_mkstr_ascii(js, out->buf, out->len) : js_mkstr(js, out->buf, out->len);
  regexp_out_end(js, out);
  return result;
}

static __attribute__((noinline)) bool regexp_replace_append_template(
  const regexp_fast_t *fast, const regexp_fast_scan_t *scan,
  const char *repl_ptr, size_t repl_len,
  const PCRE2_SIZE *ovector, uint32_t ovcount, regexp_out_t *out
) {
  int capture_count = ovcount > 1 ? (int)(ovcount - 1) : 0;
  repl_capture_t captures_inline[REGEXP_INLINE_CAPTURES];
  repl_capture_t *captures = capture_count <= REGEXP_INLINE_CAPTURES
    ? captures_inline
    : malloc(sizeof(*captures) * (size_t)capture_count);
  if (!captures) return false;

  for (int i = 0; i < capture_count; i++) {
    PCRE2_SIZE capture_start = ovector[2 * (i + 1)];
    captures[i] = capture_start == PCRE2_UNSET
      ? (repl_capture_t){ NULL, 0 }
      : (repl_capture_t){ scan->str_ptr + capture_start, (size_t)(ovector[2 * (i + 1) + 1] - capture_start) };
  }

  PCRE2_SIZE start = ovector[0];
  regexp_named_ctx_t named_ctx = { fast->compiled, scan->str_ptr, ovector, ovcount };
  repl_named_t named = { regexp_named_capture, &named_ctx };
  bool ok = repl_template(
    repl_ptr, repl_len, scan->str_ptr + start, ovector[1] - start,
    scan->str_ptr, (size_t)scan->str_len, start, captures, capture_count,
    fast->compiled->namecount ? &named : NULL, &out->buf, &out->len, &out->cap
  );
  if (captures != captures_inline) free(captures);
  return ok;
}

static __attribute__((always_inline)) inline ant_value_t regexp_replace_fast_string_scan(
  ant_t *js, const regexp_fast_t *fast, ant_value_t str,
  const char *repl_ptr, ant_offset_t repl_len, bool literal, bool literal_pattern
) {
  regexp_fast_scan_t scan;
  regexp_fast_scan_begin(js, &scan, fast, str);

  regexp_out_t out = {0};
  PCRE2_SIZE next_src_pos = 0;
  ant_value_t error = js_mkundef();

  for (;;) {
    PCRE2_SIZE *ovector;
    uint32_t ovcount;
    int rc = regexp_fast_scan_next(js, &scan, &ovector, &ovcount, literal_pattern);
    if (rc == PCRE2_ERROR_NOMEMORY) { error = js_mkerr(js, "oom"); break; }
    if (rc < 0) break;

    PCRE2_SIZE start = ovector[0];
    if (!out.buf) {
      if (!regexp_out_begin(js, &out, (size_t)scan.str_len + (size_t)repl_len + 256)) {
        error = js_mkerr(js, "oom");
        break;
      }
      out.ascii = regexp_bytes_known_ascii(scan.str_ptr) && regexp_bytes_known_ascii(repl_ptr);
    }
    if (
      !str_buf_append(&out.buf, &out.len, &out.cap, scan.str_ptr + next_src_pos, start - next_src_pos) ||
      !(literal
        ? str_buf_append(&out.buf, &out.len, &out.cap, repl_ptr, (size_t)repl_len)
        : regexp_replace_append_template(fast, &scan, repl_ptr, (size_t)repl_len, ovector, ovcount, &out))
    ) {
      error = js_mkerr(js, "oom");
      break;
    }
    next_src_pos = ovector[1];
  }

  regexp_fast_scan_end(js, &scan);
  if (is_err(error)) {
    if (out.buf) regexp_out_end(js, &out);
    return error;
  }
  if (scan.count == 0) return str;
  return regexp_out_finish(js, &out, scan.str_ptr + next_src_pos, (size_t)scan.str_len - next_src_pos);
}

static ant_value_t regexp_replace_fast_string(
  ant_t *js, const regexp_fast_t *fast, ant_value_t str, ant_value_t replace_str
) {
  ant_offset_t repl_len, repl_off = vstr(js, replace_str, &repl_len);
  const char *repl_ptr = (const char *)(uintptr_t)repl_off;
  bool literal = repl_len == 0 || !memchr(repl_ptr, '$', (size_t)repl_len);
  return literal && fast->compiled->plain_literal
    ? regexp_replace_fast_string_scan(js, fast, str, repl_ptr, repl_len, true, true)
    : regexp_replace_fast_string_scan(js, fast, str, repl_ptr, repl_len, literal, false);
}

static ant_value_t regexp_replace_fast_function(
  ant_t *js, const regexp_fast_t *fast, ant_value_t str, ant_value_t replace_fn
) {
  regexp_fast_scan_t scan;
  regexp_fast_scan_begin(js, &scan, fast, str);

  utf16_pos_t pos;
  utf16_pos_init(&pos, scan.str_ptr, scan.str_len);
  regexp_ovector_list_t matches = {0};
  uint32_t stride = 0;
  ant_value_t error = js_mkundef();
  for (;;) {
    PCRE2_SIZE *ovector;
    uint32_t ovcount;
    int rc = regexp_fast_scan_next(js, &scan, &ovector, &ovcount, false);
    if (rc == PCRE2_ERROR_NOMEMORY) { error = js_mkerr(js, "oom"); break; }
    if (rc < 0) break;
    stride = ovcount * 2 + 1;
    PCRE2_SIZE index = utf16_pos_units(&pos, ovector[0]);
    if (
      !regexp_ovector_list_push(&matches, ovector, stride - 1) ||
      !regexp_ovector_list_push(&matches, &index, 1)
    ) { error = js_mkerr(js, "oom"); break; }
  }
  regexp_fast_scan_end(js, &scan);

  if (is_err(error) || scan.count == 0) {
    free(matches.items);
    return is_err(error) ? error : str;
  }

  compiled_regex_cache_entry_t *compiled = fast->compiled;
  bool has_groups = compiled->namecount != 0 && compiled->group_names;
  uint32_t ovcount = stride / 2;
  int argc = (int)ovcount + 2 + (has_groups ? 1 : 0);
  ant_value_t call_args_inline[REGEXP_INLINE_CAPTURES + 3];
  ant_value_t *call_args = argc <= REGEXP_INLINE_CAPTURES + 3
    ? call_args_inline
    : malloc(sizeof(*call_args) * (size_t)argc);
  if (!call_args) {
    free(matches.items);
    return js_mkerr(js, "oom");
  }
  compiled->active_refs++;

  regexp_out_t out = {0};
  PCRE2_SIZE next_src_pos = 0;
  ant_value_t result = js_mkundef();

  for (size_t m = 0; m < scan.count; m++) {
    const PCRE2_SIZE *ovector = matches.items + m * stride;
    ant_offset_t str_len, str_off = regexp_vstr(js, str, &str_len);
    const char *str_ptr = (const char *)(uintptr_t)str_off;
    int ca = 0;
    for (uint32_t i = 0; i < ovcount; i++) {
      PCRE2_SIZE start = ovector[2 * i];
      ant_value_t capture = js_mkundef();
      if (start != PCRE2_UNSET) {
        capture = js_mkstr_byte_range(js, str_ptr, start, ovector[2 * i + 1] - start);
        if (is_err(capture)) { result = capture; goto done; }
      }
      call_args[ca++] = capture;
    }
    call_args[ca++] = tov((double)ovector[2 * ovcount]);
    call_args[ca++] = str;
    if (has_groups) {
      ant_value_t groups = regexp_build_groups_object(js, compiled, str_ptr, ovector, ovcount);
      if (is_err(groups)) { result = groups; goto done; }
      call_args[ca++] = groups;
    }

    ant_value_t replacement = sv_vm_call(js->vm, js, replace_fn, js_mkundef(), call_args, ca, NULL, js_mkundef());
    if (is_err(replacement)) { result = replacement; goto done; }
    ant_value_t rep_str = js_tostring_val(js, replacement);
    if (is_err(rep_str)) { result = rep_str; goto done; }

    str_off = regexp_vstr(js, str, &str_len);
    str_ptr = (const char *)(uintptr_t)str_off;
    ant_offset_t rep_len, rep_off = vstr(js, rep_str, &rep_len);
    if (!out.buf) {
      if (!regexp_out_begin(js, &out, (size_t)str_len + (size_t)rep_len + 256)) {
        result = js_mkerr(js, "oom");
        goto done;
      }
      out.ascii = regexp_bytes_known_ascii(str_ptr);
    }
    out.ascii = out.ascii && regexp_bytes_known_ascii((const char *)(uintptr_t)rep_off);
    if (
      !str_buf_append(&out.buf, &out.len, &out.cap, str_ptr + next_src_pos, ovector[0] - next_src_pos) ||
      !str_buf_append(&out.buf, &out.len, &out.cap, (const char *)(uintptr_t)rep_off, (size_t)rep_len)
    ) { result = js_mkerr(js, "oom"); goto done; }
    next_src_pos = ovector[1];
  }

  {
    ant_offset_t str_len, str_off = regexp_vstr(js, str, &str_len);
    result = regexp_out_finish(js, &out, (const char *)(uintptr_t)str_off + next_src_pos, (size_t)str_len - next_src_pos);
  }

done:
  if (out.buf) regexp_out_end(js, &out);
  if (call_args != call_args_inline) free(call_args);
  free(matches.items);
  compiled_regex_cache_release(compiled);
  return result;
}

static inline ant_value_t regexp_replace_fast(
  ant_t *js, const regexp_fast_t *fast, ant_value_t str, ant_value_t replacement, bool func_replace
) {
  return func_replace
    ? regexp_replace_fast_function(js, fast, str, replacement)
    : regexp_replace_fast_string(js, fast, str, replacement);
}

typedef struct {
  ant_t *js;
  ant_value_t groups;
  ant_value_t error;
} regexp_named_object_ctx_t;

static bool regexp_named_object_capture(void *ctx, const char *name, size_t len, repl_capture_t *out) {
  regexp_named_object_ctx_t *named = ctx;
  ant_value_t value = js_getprop_fallback_len(named->js, named->groups, name, len);
  if (!is_err(value) && vtype(value) == kTypeUndefined) return true;
  if (!is_err(value)) value = js_tostring_val(named->js, value);
  if (is_err(value)) {
    named->error = value;
    return false;
  }
  ant_offset_t value_len, value_off = vstr(named->js, value, &value_len);
  *out = (repl_capture_t){ (const char *)(uintptr_t)value_off, (size_t)value_len };
  return true;
}

static ant_value_t regexp_symbol_replace_impl(
  ant_t *js, ant_value_t rx, ant_value_t str, ant_value_t replace_value, const regexp_fast_t *known
) {
  bool func_replace = (vtype(replace_value) == kTypeFunction || vtype(replace_value) == kTypeBuiltin);
  ant_value_t replace_str = js_mkundef();
  if (!func_replace) {
    if (vtype(replace_value) != kTypeString) known = NULL;
    replace_str = js_tostring_val(js, replace_value);
    if (is_err(replace_str)) return replace_str;
  }

  regexp_fast_t fast;
  if (known) fast = *known;
  if (known || regexp_fast_get(js, rx, REGEXP_FAST_PRISTINE, &fast))
    return regexp_replace_fast(js, &fast, str, func_replace ? replace_value : replace_str, func_replace);

  bool global = false;
  bool full_unicode = false;
  ant_value_t flags_status = regexp_read_flags(js, rx, &global, &full_unicode);
  if (is_err(flags_status)) return flags_status;

  if (global) {
    ant_value_t stored = regexp_set_lastindex(
      js, js_get_native(rx, REGEXP_NATIVE_TAG), rx, tov(0)
    );
    if (is_err(stored)) return stored;
  }

  if (
    regexp_fast_get(js, rx, REGEXP_FAST_BUILTIN_EXEC, &fast) &&
    ((fast.flags & REGEXP_FLAG_GLOBAL) != 0) == global
  ) return regexp_replace_fast(js, &fast, str, func_replace ? replace_value : replace_str, func_replace);

  ant_value_t results = js_mkarr(js);
  if (is_err(results)) return results;
  ant_offset_t nresults = 0;

  for (;;) {
    ant_value_t result = regexp_exec_abstract(js, rx, str);
    if (is_err(result)) return result;
    if (vtype(result) == kTypeNull) break;
    js_arr_push(js, results, result);
    nresults++;
    if (!global) break;

    ant_value_t match_str = js_tostring_val(js, js_arr_get(js, result, 0));
    if (is_err(match_str)) return match_str;
    ant_offset_t mlen; vstr(js, match_str, &mlen);
    if (mlen == 0) {
      ant_value_t advanced = regexp_advance_last_index(js, rx, str, full_unicode);
      if (is_err(advanced)) return advanced;
    }
  }

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  ant_offset_t str_units_len = str_utf16_len(js, str);
  size_t buf_cap = str_len + 256;
  char *buf = malloc(buf_cap);
  if (!buf) return js_mkerr(js, "oom");
  size_t buf_len = 0;
  ant_offset_t next_src_pos = 0;

#define SB_APPEND(data, dlen) do { \
  if (buf_len + (dlen) >= buf_cap) { \
    buf_cap = (buf_len + (dlen) + 1) * 2; \
    char *nb = realloc(buf, buf_cap); \
    if (!nb) { free(buf); return js_mkerr(js, "oom"); } \
    buf = nb; \
  } \
  memcpy(buf + buf_len, data, dlen); buf_len += (dlen); \
} while(0)

  for (ant_offset_t i = 0; i < nresults; i++) {
    ant_value_t result = js_arr_get(js, results, i);
    ant_value_t matched = js_tostring_val(js, js_arr_get(js, result, 0));
    if (is_err(matched)) { free(buf); return matched; }

    ant_value_t pos_val = js_getprop_fallback(js, result, "index");
    if (is_err(pos_val)) { free(buf); return pos_val; }
    if (is_object_type(pos_val) || vtype(pos_val) == kTypeBuiltin) {
      pos_val = js_to_primitive(js, pos_val, 2);
      if (is_err(pos_val)) { free(buf); return pos_val; }
    }
    if (vtype(pos_val) == kTypeBigInt || vtype(pos_val) == kTypeSymbol) {
      free(buf);
      return js_mkerr_typed(
        js, JS_ERR_TYPE, "Cannot convert a %s value to a number",
        vtype(pos_val) == kTypeSymbol ? "Symbol" : "BigInt");
    }

    double position_units = js_to_number(js, pos_val);
    position_units = isnan(position_units) ? 0 : trunc(position_units);
    if (position_units < 0) position_units = 0;
    if (position_units > (double)str_units_len)
      position_units = (double)str_units_len;

    str_off = vstr(js, str, &str_len);
    int position_bytes = utf16_index_to_byte_offset(
      (const char *)(uintptr_t)str_off, (size_t)str_len,
      (size_t)position_units, NULL
    );
    ant_offset_t position = position_bytes < 0
      ? str_len
      : (ant_offset_t)position_bytes;

    ant_value_t named_captures = js_getprop_fallback(js, result, "groups");
    if (is_err(named_captures)) { free(buf); return named_captures; }
    bool has_named = vtype(named_captures) != kTypeUndefined;

    ant_value_t replacement;
    if (func_replace) {
      ant_offset_t ncaptures = js_arr_len(js, result);
      if (ncaptures > INT_MAX - 3) {
        free(buf);
        return js_mkerr(js, "too many regexp captures");
      }
      int call_capacity = (int)ncaptures + 3;
      ant_value_t call_args_inline[REGEXP_INLINE_CAPTURES];
      ant_value_t *call_args = call_capacity <= REGEXP_INLINE_CAPTURES
        ? call_args_inline
        : malloc(sizeof(*call_args) * (size_t)call_capacity);
      if (!call_args) {
        free(buf);
        return js_mkerr(js, "oom");
      }
      int ca = 0;
      for (ant_offset_t c = 0; c < ncaptures; c++)
        call_args[ca++] = js_arr_get(js, result, c);
      call_args[ca++] = tov(position_units);
      call_args[ca++] = str;
      if (has_named) call_args[ca++] = named_captures;
      replacement = sv_vm_call(js->vm, js, replace_value, js_mkundef(), call_args, ca, NULL, js_mkundef());
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
        repl_capture_t caps_buf[16], *caps = num_caps <= 16 ? caps_buf : malloc(sizeof(repl_capture_t) * (size_t)num_caps);
        if (num_caps > 16 && !caps) {
          free(buf);
          return js_mkerr(js, "oom");
        }
        for (int ci = 0; ci < num_caps; ci++) {
          ant_value_t cap = js_arr_get(js, result, (ant_offset_t)(ci + 1));
          if (vtype(cap) == kTypeString) { ant_offset_t cl, co = vstr(js, cap, &cl); caps[ci] = (repl_capture_t){ (const char *)(uintptr_t)(co), cl }; }
          else caps[ci] = (repl_capture_t){ NULL, 0 };
        }
        ant_offset_t mlen, moff = vstr(js, matched, &mlen);
        str_off = vstr(js, str, &str_len);
        if (has_named && vtype(named_captures) == kTypeNull) {
          if (caps != caps_buf) free(caps);
          free(buf);
          return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp result groups is not an object");
        }
        regexp_named_object_ctx_t named_ctx = { js, named_captures, js_mkundef() };
        repl_named_t named = { regexp_named_object_capture, &named_ctx };
        bool ok = repl_template(
          (const char *)(uintptr_t)(rep_off), rep_len,
          (const char *)(uintptr_t)(moff), mlen,
          (const char *)(uintptr_t)(str_off), str_len, position,
          caps, num_caps, has_named ? &named : NULL, &buf, &buf_len, &buf_cap
        );
        if (caps != caps_buf) free(caps);
        if (!ok) {
          free(buf);
          return is_err(named_ctx.error) ? named_ctx.error : js_mkerr(js, "oom");
        }
      }
      double match_end_units = position_units + (double)str_utf16_len(js, matched);
      if (match_end_units > (double)str_units_len)
        match_end_units = (double)str_units_len;
      str_off = vstr(js, str, &str_len);
      int match_end_bytes = utf16_index_to_byte_offset(
        (const char *)(uintptr_t)str_off, (size_t)str_len,
        (size_t)match_end_units, NULL
      );
      next_src_pos = match_end_bytes < 0
        ? str_len
        : (ant_offset_t)match_end_bytes;
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

static ant_value_t builtin_regexp_symbol_replace(ant_params_t) {
  ant_value_t rx = js->this_val;
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@replace] called on non-object");

  ant_value_t str = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str)) return str;
  return regexp_symbol_replace_impl(js, rx, str, nargs > 1 ? args[1] : js_mkundef(), NULL);
}

static ant_value_t regexp_symbol_search_impl(ant_t *js, ant_value_t rx, ant_value_t str);

static ant_value_t builtin_regexp_symbol_search(ant_params_t) {
  ant_value_t rx = js->this_val;
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@search] called on non-object");

  ant_value_t str = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str)) return str;
  return regexp_symbol_search_impl(js, rx, str);
}

static ant_value_t regexp_search_fast(ant_t *js, const regexp_fast_t *fast, ant_value_t str) {
  ant_offset_t str_len, str_off = regexp_vstr(js, str, &str_len);
  const char *str_ptr = (const char *)(uintptr_t)str_off;
  regex_match_scope_t scope = {0};
  PCRE2_SIZE *ovector;
  uint32_t ovcount;
  int rc = regexp_match(
    js, fast->compiled, str_ptr, str_len, 0,
    (fast->flags & REGEXP_FLAG_STICKY) != 0, &scope, &ovector, &ovcount
  );
  if (rc < 0) {
    regex_match_scope_end(&scope);
    return rc == PCRE2_ERROR_NOMEMORY ? js_mkerr(js, "oom") : tov(-1);
  }

  update_regexp_statics(js, str, ovector, ovcount);
  ant_value_t index = tov((double)byte_offset_to_utf16(str_ptr, ovector[0]));
  regex_match_scope_end(&scope);
  return index;
}

static ant_value_t regexp_symbol_search_impl(ant_t *js, ant_value_t rx, ant_value_t str) {
  regexp_fast_t fast;
  if (regexp_fast_get(js, rx, REGEXP_FAST_BUILTIN_EXEC, &fast)) return regexp_search_fast(js, &fast, str);

  ant_value_t prev_li = js_getprop_fallback(js, rx, "lastIndex");
  if (is_err(prev_li)) return prev_li;
  if (!same_value_values(js, prev_li, tov(0))) {
    ant_value_t stored = js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), tov(0));
    if (is_err(stored)) return stored;
  }

  ant_value_t result = regexp_exec_abstract(js, rx, str);
  if (is_err(result)) return result;

  ant_value_t cur_li = js_getprop_fallback(js, rx, "lastIndex");
  if (is_err(cur_li)) return cur_li;
  if (!same_value_values(js, cur_li, prev_li)) {
    ant_value_t stored = js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), prev_li);
    if (is_err(stored)) return stored;
  }

  if (vtype(result) == kTypeNull) return tov(-1);

  ant_value_t idx = js_getprop_fallback(js, result, "index");
  if (is_err(idx)) return idx;
  return vtype(idx) == kTypeNumber ? idx : tov(-1);
}

static ant_value_t regexp_split_fast(ant_t *js, const regexp_fast_t *fast, ant_value_t str, uint32_t limit) {
  ant_value_t result = js_mkarr(js);
  if (is_err(result) || limit == 0) return result;

  ant_offset_t str_len, str_off = regexp_vstr(js, str, &str_len);
  const char *str_ptr = (const char *)(uintptr_t)str_off;
  compiled_regex_cache_entry_t *compiled = fast->compiled;
  regex_match_scope_t scope = {0};

  PCRE2_SIZE *ovector;
  uint32_t ovcount;
  PCRE2_SIZE size = (PCRE2_SIZE)str_len;
  if (size == 0) {
    int rc = regexp_match(js, compiled, str_ptr, str_len, 0, true, &scope, &ovector, &ovcount);
    if (rc >= 0) update_regexp_statics(js, str, ovector, ovcount);
    else js_arr_push(js, result, str);
    regex_match_scope_end(&scope);
    return result;
  }

  PCRE2_SIZE p = 0, q = 0, last_index = 0;
  uint32_t result_len = 0;
  while (q < size) {
    int rc = regexp_match(js, compiled, str_ptr, str_len, q, false, &scope, &ovector, &ovcount);
    if (rc < 0 || ovector[0] >= size) {
      last_index = 0;
      break;
    }

    PCRE2_SIZE start = ovector[0], end = ovector[1];
    update_regexp_statics(js, str, ovector, ovcount);
    last_index = end;
    if (end == p) {
      q = regexp_advance_empty_match(str_ptr, str_len, start);
      continue;
    }

    ant_value_t piece = js_mkstr_byte_range(js, str_ptr, p, start - p);
    if (is_err(piece)) { result = piece; goto done; }
    js_arr_push(js, result, piece);
    if (++result_len == limit) goto done;

    for (uint32_t i = 1; i < ovcount; i++) {
      PCRE2_SIZE capture_start = ovector[2 * i];
      ant_value_t capture = capture_start == PCRE2_UNSET
        ? js_mkundef()
        : js_mkstr_byte_range(js, str_ptr, capture_start, ovector[2 * i + 1] - capture_start);
      if (is_err(capture)) { result = capture; goto done; }
      js_arr_push(js, result, capture);
      if (++result_len == limit) goto done;
    }
    p = q = end;
  }

  {
    ant_value_t trailing = p == 0 ? str : js_mkstr_byte_range(js, str_ptr, p, size - p);
    if (is_err(trailing)) result = trailing;
    else js_arr_push(js, result, trailing);
  }

done:
  regex_match_scope_end(&scope);
  if (fast->obj && !is_err(result)) {
    str_off = regexp_vstr(js, str, &str_len);
    regexp_fast_set_last_index(js, js_mkundef(), fast, (double)byte_offset_to_utf16((const char *)(uintptr_t)str_off, last_index));
  }
  return result;
}

ant_value_t regexp_split_limit(ant_t *js, ant_value_t limit, uint32_t *out) {
  *out = UINT32_MAX;
  if (vtype(limit) == kTypeUndefined) return js_mkundef();
  if (vtype(limit) == kTypeNumber) {
    *out = js_to_uint32(tod(limit));
    return js_mkundef();
  }
  ant_value_t primitive = limit;
  if (is_object_type(limit) || vtype(limit) == kTypeBuiltin) {
    primitive = js_to_primitive(js, limit, 2);
    if (is_err(primitive)) return primitive;
  }
  if (vtype(primitive) == kTypeBigInt || vtype(primitive) == kTypeSymbol)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert value to a number");
  *out = js_to_uint32(js_to_number(js, primitive));
  return js_mkundef();
}

bool regexp_split_try_fast(ant_t *js, ant_value_t rx, ant_value_t str, ant_value_t limit, ant_value_t *out) {
  uint32_t lim = UINT32_MAX;
  if (vtype(limit) == kTypeNumber) lim = js_to_uint32(tod(limit));
  else if (vtype(limit) != kTypeUndefined) return false;

  regexp_fast_t fast;
  if (!regexp_fast_get(js, rx, REGEXP_FAST_PRISTINE, &fast)) return false;

  fast.obj = NULL;
  *out = regexp_split_fast(js, &fast, str, lim);
  return true;
}

static ant_value_t builtin_regexp_symbol_split(ant_params_t) {
  ant_value_t rx = js_getthis(js);
  if (!is_object_type(rx))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@split] called on non-object");

  ant_value_t str = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "", 0);
  if (is_err(str)) return str;

  ant_value_t fast_split;
  if (regexp_split_try_fast(js, rx, str, nargs >= 2 ? args[1] : js_mkundef(), &fast_split)) return fast_split;

  ant_value_t C = regexp_species_ctor(js, rx, "RegExp.prototype[@@split]");
  if (is_err(C)) return C;

  ant_value_t flags_val = js_get(js, rx, "flags");
  if (is_err(flags_val)) return flags_val;
  ant_value_t flags_str = js_tostring_val(js, flags_val);
  if (is_err(flags_str)) return flags_str;

  ant_offset_t flen, foff = vstr(js, flags_str, &flen);
  const char *fptr = (const char *)(uintptr_t)(foff);
  bool has_sticky = false;
  bool full_unicode = false;
  for (ant_offset_t i = 0; i < flen; i++) {
    if (fptr[i] == 'y') has_sticky = true;
    if (fptr[i] == 'u' || fptr[i] == 'v') full_unicode = true;
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

  uint32_t lim;
  ant_value_t lim_status = regexp_split_limit(js, nargs >= 2 ? args[1] : js_mkundef(), &lim);
  if (is_err(lim_status)) return lim_status;
  if (lim == 0) return mkval(kTypeArray, vdata(A));

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  ant_offset_t size = str_utf16_len(js, str);

  if (size == 0) {
    ant_value_t z = regexp_exec_abstract(js, splitter, str);
    if (is_err(z)) return z;
    if (vtype(z) == kTypeNull) js_arr_push(js, A, str);
    return mkval(kTypeArray, vdata(A));
  }

  regexp_fast_t fast;
  if (regexp_fast_get(js, splitter, REGEXP_FAST_BUILTIN_EXEC, &fast) && (fast.flags & REGEXP_FLAG_STICKY))
    return regexp_split_fast(js, &fast, str, lim);

  ant_offset_t p = 0, q = p;
  ant_value_t lastIndex_key = js_mkstr(js, "lastIndex", 9);

  while (q < size) {
    js_setprop(js, splitter, lastIndex_key, tov((double)q));

    ant_value_t z = regexp_exec_abstract(js, splitter, str);
    if (is_err(z)) return z;

    if (vtype(z) == kTypeNull) {
      str_off = vstr(js, str, &str_len);
      q = (ant_offset_t)regexp_advance_string_index(js, (const char *)(uintptr_t)str_off, str_len, (double)q, full_unicode);
      continue;
    }

    ant_value_t li_val = js_get(js, splitter, "lastIndex");
    if (is_err(li_val)) return li_val;
    double e_raw = vtype(li_val) == kTypeNumber ? tod(li_val) : 0;
    ant_offset_t e = (ant_offset_t)(e_raw < 0 ? 0 : (e_raw > (double)size ? (double)size : e_raw));

    if (e == p) {
      str_off = vstr(js, str, &str_len);
      q = (ant_offset_t)regexp_advance_string_index(js, (const char *)(uintptr_t)str_off, str_len, (double)q, full_unicode);
      continue;
    }

    str_off = vstr(js, str, &str_len);
    utf16_pos_t pos;
    utf16_pos_init(&pos, (const char *)(uintptr_t)str_off, str_len);
    PCRE2_SIZE piece_start = utf16_pos_seek(&pos, (size_t)p);
    PCRE2_SIZE piece_end = utf16_pos_seek(&pos, (size_t)q);
    ant_value_t T_val = js_mkstr_byte_range(js, (const char *)(uintptr_t)str_off, piece_start, piece_end - piece_start);
    if (is_err(T_val)) return T_val;
    js_arr_push(js, A, T_val);
    lengthA++;
    if (lengthA == lim) return mkval(kTypeArray, vdata(A));

    ant_offset_t num_caps = js_arr_len(js, z);
    for (ant_offset_t i = 1; i < num_caps; i++) {
      ant_value_t cap = js_arr_get(js, z, i);
      js_arr_push(js, A, cap);
      lengthA++;
      if (lengthA == lim) return mkval(kTypeArray, vdata(A));
    }

    p = e;
    q = p;
  }

  str_off = vstr(js, str, &str_len);
  utf16_pos_t pos;
  utf16_pos_init(&pos, (const char *)(uintptr_t)str_off, str_len);
  PCRE2_SIZE trailing_start = utf16_pos_seek(&pos, (size_t)p);
  ant_value_t trailing = js_mkstr_byte_range(js, (const char *)(uintptr_t)str_off, trailing_start, (size_t)str_len - trailing_start);
  if (is_err(trailing)) return trailing;
  js_arr_push(js, A, trailing);
  return mkval(kTypeArray, vdata(A));
}

static bool str_buf_append(char **buf, size_t *len, size_t *cap, const char *data, size_t n) {
  if (n == 0) return true;
  if (*len + n >= *cap) {
    size_t new_cap = (*len + n + 1) * 2;
    char *nb = (char *)realloc(*buf, new_cap);
    if (!nb) return false;
    *buf = nb;
    *cap = new_cap;
  }
  memcpy(*buf + *len, data, n);
  *len += n;
  return true;
}

static void regexp_wtf8_surrogate_halves(const char *bytes, char out[6]) {
  const unsigned char *b = (const unsigned char *)bytes;
  uint32_t cp = ((uint32_t)(b[0] & 0x07) << 18) | ((uint32_t)(b[1] & 0x3F) << 12) |
    ((uint32_t)(b[2] & 0x3F) << 6) | (uint32_t)(b[3] & 0x3F);
  uint32_t units[2] = { 0xD800 + ((cp - 0x10000) >> 10), 0xDC00 + ((cp - 0x10000) & 0x3FF) };
  for (int i = 0; i < 2; i++) {
    out[3 * i] = (char)0xED;
    out[3 * i + 1] = (char)(0x80 | ((units[i] >> 6) & 0x3F));
    out[3 * i + 2] = (char)(0x80 | (units[i] & 0x3F));
  }
}

static inline ant_value_t emit_str_replacement(
  ant_t *js, ant_value_t replacement, bool is_func,
  const char *repl_ptr, ant_offset_t repl_len, bool repl_template_needed,
  const char *str_ptr, ant_offset_t str_len, ant_value_t str, bool detached,
  ant_offset_t pos, size_t position, ant_offset_t match_len,
  char **buf, size_t *buf_len, size_t *buf_cap
) {
  if (!is_func && repl_template_needed) {
    if (!repl_template(
      repl_ptr, repl_len, str_ptr + pos, match_len,
      str_ptr, str_len, pos, NULL, 0, NULL,
      buf, buf_len, buf_cap
    )) return js_mkerr(js, "oom");
  } else if (is_func) {
    ant_value_t matched = detached
      ? js_mkstr(js, str_ptr + pos, (size_t)match_len)
      : js_mkstr_byte_range(js, str_ptr, pos, match_len);
    ant_value_t cb_args[3] = { matched, tov((double)position), str };
    ant_value_t r = sv_vm_call(js->vm, js, replacement, js_mkundef(), cb_args, 3, NULL, js_mkundef());
    
    if (vtype(r) == kTypeError) return r;
    ant_value_t r_str = js_tostring_val(js, r);
    
    if (is_err(r_str)) return r_str;
    ant_offset_t rlen, roff = vstr(js, r_str, &rlen);
    
    if (!str_buf_append(buf, buf_len, buf_cap, (const char *)(uintptr_t)roff, rlen)) return js_mkerr(js, "oom");
  } else if (!str_buf_append(buf, buf_len, buf_cap, repl_ptr, repl_len)) return js_mkerr(js, "oom");
  
  return js_mkundef();
}

static __attribute__((noinline, cold)) ant_value_t string_replace_split_pairs(
  ant_t *js, ant_value_t str, const char *str_ptr, ant_offset_t str_len,
  const char *search_ptr, ant_offset_t search_len, ant_value_t replacement,
  bool is_func, const char *repl_ptr, ant_offset_t repl_len,
  bool repl_template_needed, bool replace_all
) {
  size_t view_len = 0, needle_len = 0;
  char *view = wtf8_split_pairs_dup(str_ptr, (size_t)str_len, &view_len);
  char *needle = view ? wtf8_split_pairs_dup(search_ptr, (size_t)search_len, &needle_len) : NULL;
  size_t cap = view_len + (size_t)repl_len + 256, len = 0;
  char *buf = needle ? malloc(cap) : NULL;
  ant_value_t result = js_mkundef();
  if (!buf) goto oom;

  size_t pos = 0, position = 0;
  bool replaced = false;
  for (;;) {
    const char *match = find_bytes(view + pos, (ant_offset_t)(view_len - pos), needle, (ant_offset_t)needle_len);
    if (!match) break;
    size_t match_pos = (size_t)(match - view);
    position += utf16_strlen(view + pos, match_pos - pos);
    if (!str_buf_append(&buf, &len, &cap, view + pos, match_pos - pos)) goto oom;
    replaced = true;
    ant_value_t err = emit_str_replacement(
      js, replacement, is_func, repl_ptr, repl_len, repl_template_needed,
      view, (ant_offset_t)view_len, str, true, (ant_offset_t)match_pos, position,
      (ant_offset_t)needle_len, &buf, &len, &cap
    );
    if (is_err(err)) { result = err; goto out; }
    position += utf16_strlen(match, needle_len);
    pos = match_pos + needle_len;
    if (!replace_all) break;
  }

  if (!replaced) { result = str; goto out; }
  if (!str_buf_append(&buf, &len, &cap, view + pos, view_len - pos)) goto oom;
  result = js_mkstr(js, buf, len);
  goto out;

oom:
  result = js_mkerr(js, "oom");

out:
  free(buf);
  free(needle);
  free(view);
  return result;
}

static ant_value_t string_replace_impl(ant_native_params_t, bool replace_all) {
  ant_value_t str = Ant_String_CoerceThis(js, replace_all ? "replaceAll" : "replace");
  
  if (is_err(str)) return str;
  if (nargs < 1) return str;

  if (is_object_type(args[0])) {
    regexp_fast_t fast;
    if (regexp_fast_get(js, args[0], REGEXP_FAST_PRISTINE, &fast)) {
      if (replace_all && !(fast.flags & REGEXP_FLAG_GLOBAL))
        return js_mkerr_typed(js, JS_ERR_TYPE, "String.prototype.replaceAll called with a non-global RegExp");
      return regexp_symbol_replace_impl(js, args[0], str, nargs > 1 ? args[1] : js_mkundef(), &fast);
    }
    if (replace_all) {
      ant_value_t global_val = regexp_read_flag(js, args[0], REGEXP_FLAG_INDEX_GLOBAL);
      if (!js_truthy(js, global_val)) return js_mkerr_typed(js, JS_ERR_TYPE, "String.prototype.replaceAll called with a non-global RegExp");
    }
    
    bool called = false;
    ant_value_t replacement_arg = nargs > 1 ? args[1] : js_mkundef();
    ant_value_t call_args[2] = { str, replacement_arg };
    
    ant_value_t result = maybe_call_symbol_method(js, args[0], js->sym.replace_sym, args[0], call_args, 2, &called);
    if (is_err(result)) return result;
    if (called) return result;
    
    ant_value_t coerced_search = js_to_primitive(js, args[0], 1);
    if (is_err(coerced_search)) return coerced_search;
    
    coerced_search = js_tostring_val(js, coerced_search);
    if (is_err(coerced_search)) return coerced_search;
    
    args[0] = coerced_search;
  }
  
  ant_value_t search = js_tostring_val(js, args[0]);
  if (is_err(search)) return search;
  ant_value_t replacement = nargs > 1 ? args[1] : js_mkundef();

  bool is_func = is_callable(replacement);
  ant_offset_t repl_len = 0;
  const char *repl_ptr = NULL;
  bool repl_template_needed = false;
  
  if (!is_func) {
    replacement = js_tostring_val(js, replacement);
    if (is_err(replacement)) return replacement;
    ant_offset_t repl_off = vstr(js, replacement, &repl_len);
    repl_ptr = (char *)(uintptr_t)(repl_off);
    repl_template_needed = repl_len && memchr(repl_ptr, '$', repl_len) != NULL;
  }
  bool repl_plain = !is_func && !repl_template_needed;

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  ant_offset_t search_len, search_off = vstr(js, search, &search_len);
  const char *search_ptr = (char *)(uintptr_t)(search_off);

  if (
    search_len > 0 && !str_is_ascii(str_ptr) &&
    wtf8_needle_splits_pairs(search_ptr, (size_t)search_len) &&
    wtf8_has_supplementary(str_ptr, (size_t)str_len)
  ) return string_replace_split_pairs(
    js, str, str_ptr, str_len, search_ptr, search_len, replacement,
    is_func, repl_ptr, repl_len, repl_template_needed, replace_all
  );

  if (!replace_all) {
    if (search_len > str_len) return str;
    ant_offset_t match_pos = 0;
    bool found = search_len == 0;
    if (!found) {
      const char *match = find_bytes(
        str_ptr, str_len, search_ptr, search_len
      );
      if (match) {
        match_pos = (ant_offset_t)(match - str_ptr);
        found = true;
      }
    }
      
    if (!found) return str;

    size_t cap = str_len + repl_len + 256, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return js_mkerr(js, "oom");
    
    if (!str_buf_append(&buf, &len, &cap, str_ptr, match_pos)) { 
      free(buf);
      return js_mkerr(js, "oom");
    }
    
    ant_value_t err = emit_str_replacement(
      js, replacement, is_func, repl_ptr, 
      repl_len, repl_template_needed, str_ptr, str_len, str, false, match_pos,
      is_func ? byte_offset_to_utf16(str_ptr, (size_t)match_pos) : 0,
      search_len, &buf, &len, &cap
    );
    
    if (vtype(err) == kTypeError) {
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
    char *buf = (char *)malloc(cap);
    if (!buf) return js_mkerr(js, "oom");
    
    ant_offset_t pos = 0;
    bool replaced = false;
    utf16_pos_t units;
    utf16_pos_init(&units, str_ptr, str_len);

    if (search_len != 0) {
      for (;;) {
        const char *match = find_bytes(
          str_ptr + pos, str_len - pos, search_ptr, search_len
        );
        if (!match) break;
        ant_offset_t match_pos = (ant_offset_t)(match - str_ptr);
        if (!str_buf_append(
          &buf, &len, &cap, str_ptr + pos, match_pos - pos
        )) {
          free(buf);
          return js_mkerr(js, "oom");
        }
        replaced = true;
        if (repl_plain) {
          if (!str_buf_append(&buf, &len, &cap, repl_ptr, repl_len)) { free(buf); return js_mkerr(js, "oom"); }
        } else {
          ant_value_t err = emit_str_replacement(
            js, replacement, is_func, repl_ptr, repl_len, repl_template_needed, str_ptr, str_len, str, false, match_pos,
            is_func ? utf16_pos_units(&units, (PCRE2_SIZE)match_pos) : 0,
            search_len, &buf, &len, &cap
          );
          if (vtype(err) == kTypeError) { free(buf); return err; }
        }
        pos = match_pos + search_len;
      }
    } else {
      size_t position = 0;
      for (;;) {
        replaced = true;
        ant_value_t err = emit_str_replacement(
          js, replacement, is_func, repl_ptr, repl_len, repl_template_needed,
          str_ptr, str_len, str, false, pos, position, 0, &buf, &len, &cap
        );
        if (vtype(err) == kTypeError) { free(buf); return err; }
        if (pos >= str_len) break;
        size_t width = utf8_char_len_at(str_ptr, (size_t)str_len, (size_t)pos);
        bool split = width == 4;
        char halves[6];
        if (split) {
          regexp_wtf8_surrogate_halves(str_ptr + pos, halves);
          if (!str_buf_append(&buf, &len, &cap, halves, 3)) { free(buf); return js_mkerr(js, "oom"); }
          char *split_str = !is_func && repl_template_needed ? malloc((size_t)str_len + 2) : NULL;
          if (split_str) {
            memcpy(split_str, str_ptr, (size_t)pos);
            memcpy(split_str + pos, halves, 6);
            memcpy(split_str + pos + 6, str_ptr + pos + 4, (size_t)(str_len - pos - 4));
          } else if (!is_func && repl_template_needed) { free(buf); return js_mkerr(js, "oom"); }
          err = emit_str_replacement(
            js, replacement, is_func, repl_ptr, repl_len, repl_template_needed,
            split_str ? split_str : str_ptr, split_str ? str_len + 2 : str_len, str, split_str != NULL,
            split_str ? pos + 3 : pos, position + 1, 0, &buf, &len, &cap
          );
          free(split_str);
          if (vtype(err) == kTypeError) { free(buf); return err; }
        }
        if (!str_buf_append(&buf, &len, &cap, split ? halves + 3 : str_ptr + pos, split ? 3 : width)) {
          free(buf);
          return js_mkerr(js, "oom");
        }
        pos += (ant_offset_t)width;
        position += split ? 2 : 1;
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

static ant_value_t builtin_string_replace(ant_params_t) {
  return string_replace_impl(js, args, nargs, false);
}

static ant_value_t builtin_string_replaceAll(ant_params_t) {
  return string_replace_impl(js, args, nargs, true);
}

static bool regexp_literal_replace_builtin_guard(ant_t *js) {
  ant_regex_state_t *state = js->regex_state;
  if (
    !state ||
    !js->regexp.write_guard_armed ||
    js->regexp.replace_written ||
    js->regexp.protector_invalid ||
    !regexp_shape_flags_pristine(js, state->instance_shape.shape)
  ) return false;

  ant_value_t replace_fn;
  return
    js_try_get_own_data_prop(js, js->sym.string_proto, "replace", 7, &replace_fn) &&
    js_cfunc_same_entrypoint(replace_fn, builtin_string_replace);
}

ant_value_t regexp_literal_replace_call(
  ant_t *js,
  ant_value_t str,
  ant_value_t pattern,
  ant_value_t flags,
  ant_value_t replacement
) {
  bool direct = vtype(str) == kTypeString && regexp_literal_replace_builtin_guard(js);
  bool func_replace = vtype(replacement) == kTypeFunction || vtype(replacement) == kTypeBuiltin;
  compiled_regex_cache_entry_t *compiled;
  if (
    direct && (func_replace || vtype(replacement) == kTypeString) &&
    (compiled = regexp_literal_compiled(js, pattern, flags))
  ) {
    regexp_fast_t fast = { .compiled = compiled, .flags = compiled->flags_mask };
    compiled->active_refs++;
    ant_value_t result = regexp_replace_fast(js, &fast, str, replacement, func_replace);
    compiled_regex_cache_release(compiled);
    return result;
  }

  ant_value_t regexp_obj = regexp_create_literal(js, pattern, flags);
  if (is_err(regexp_obj)) return regexp_obj;
  if (direct) return regexp_symbol_replace_impl(js, regexp_obj, str, replacement, NULL);
  ant_value_t replace_fn = js_getprop_fallback(js, str, "replace");
  if (is_err(replace_fn)) return replace_fn;
  ant_value_t call_args[2] = { regexp_obj, replacement };
  return sv_vm_call(js->vm, js, replace_fn, str, call_args, 2, NULL, js_mkundef());
}

static ant_value_t regexp_string_pattern_call(ant_t *js, ant_value_t str, ant_value_t pattern, bool search) {
  pattern = vtype(pattern) == kTypeUndefined ? js_mkstr(js, "", 0) : js_template_to_string(js, pattern);
  if (is_err(pattern)) return pattern;

  ant_offset_t pattern_len, pattern_off = vstr(js, pattern, &pattern_len);
  const char *syntax_error = NULL;
  compiled_regex_cache_entry_t *compiled = compiled_regex_cache_acquire(
    js, (const char *)(uintptr_t)pattern_off, pattern_len, 0, &syntax_error
  );
  if (syntax_error)
    return regexp_syntax_error_value(js, (const char *)(uintptr_t)pattern_off, pattern_len, 0, syntax_error);
  if (compiled && regexp_literal_exec_builtin_guard(js)) {
    regexp_fast_t fast = { .compiled = compiled };
    ant_value_t result = search
      ? regexp_search_fast(js, &fast, str)
      : regexp_match_fast(js, js_mkundef(), &fast, str);
    compiled_regex_cache_release(compiled);
    return result;
  }
  compiled_regex_cache_release(compiled);

  ant_value_t rx = regexp_create_literal(js, pattern, js_mkstr(js, "", 0));
  if (is_err(rx)) return rx;
  ant_value_t method = js_get_sym(js, rx, search ? js->sym.search_sym : js->sym.match_sym);
  if (is_err(method)) return method;
  if (!is_callable(method))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[%s] is not a function", search ? "@@search" : "@@match");
  ant_value_t call_args[1] = { str };
  return sv_vm_call(js->vm, js, method, rx, call_args, 1, NULL, js_mkundef());
}

static ant_value_t builtin_string_search(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, "search");
  
  if (is_err(str)) return str;
  ant_value_t pattern = nargs > 0 ? args[0] : js_mkundef();

  if (is_object_type(pattern)) {
    regexp_fast_t fast;
    if (regexp_fast_get(js, pattern, REGEXP_FAST_PRISTINE, &fast)) return regexp_search_fast(js, &fast, str);
    bool called = false;
    ant_value_t call_args[1] = { str };
    
    ant_value_t dispatched = maybe_call_symbol_method(
      js, pattern, js->sym.search_sym, pattern, call_args, 1, &called
    );
    
    if (is_err(dispatched)) return dispatched;
    if (called) return dispatched;
  }

  return regexp_string_pattern_call(js, str, pattern, true);
}

static ant_value_t builtin_string_match(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, "match");
  
  if (is_err(str)) return str;
  ant_value_t pattern = nargs > 0 ? args[0] : js_mkundef();

  if (is_object_type(pattern)) {
    regexp_fast_t fast;
    if (regexp_fast_get(js, pattern, REGEXP_FAST_PRISTINE, &fast)) return regexp_match_fast(js, pattern, &fast, str);
    bool called = false;
    ant_value_t call_args[1] = { str };
    
    ant_value_t dispatched = maybe_call_symbol_method(
      js, pattern, js->sym.match_sym, pattern, call_args, 1, &called
    );
    
    if (is_err(dispatched)) return dispatched;
    if (called) return dispatched;
  }

  return regexp_string_pattern_call(js, str, pattern, false);
}

void init_regex_module(ant_t *js) {
  if (!js->regex_state) js->regex_state = calloc(1, sizeof(*js->regex_state));
  
  ant_value_t glob = js->global;
  ant_value_t object_proto = js->sym.object_proto;

  ant_value_t regexp_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(regexp_proto, object_proto);
  js->builtins.regexp_proto = regexp_proto;
  
  defmethod(js, regexp_proto, "test", 4, Ant_Function_CreateWithArity(builtin_regexp_test, 1));
  defmethod(js, regexp_proto, "exec", 4, Ant_Function_CreateWithArity(builtin_regexp_exec, 1));
  defmethod(js, regexp_proto, "toString", 8, Ant_Function_Create(builtin_regexp_toString));

  js_set_getter_desc(js, regexp_proto, "source", 6, Ant_Function_Create(builtin_regexp_source_getter), JS_DESC_C);
#define REGEXP_FLAG(id, name, ch) \
  js_set_getter_desc(js, regexp_proto, #name, sizeof(#name) - 1, Ant_Function_Create(builtin_regexp_get_##name), JS_DESC_C);
#include "modules/regex_flags.h"

  defmethod_sym(js, regexp_proto, js->sym.split_sym, Ant_Function_CreateWithArity(builtin_regexp_symbol_split, 2));
  defmethod_sym(js, regexp_proto, js->sym.match_sym, Ant_Function_CreateWithArity(builtin_regexp_symbol_match, 1));
  defmethod_sym(js, regexp_proto, js->sym.matchAll_sym, Ant_Function_CreateWithArity(builtin_regexp_symbol_matchAll, 1));

  js->builtins.regexp_matchall_iter_proto_val = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.regexp_matchall_iter_proto_val, js->sym.iterator_proto);
  defmethod(js, js->builtins.regexp_matchall_iter_proto_val, "next", 4, Ant_Function_Create(regexp_matchall_next));
  mkprop(js, js->builtins.regexp_matchall_iter_proto_val, js->sym.toStringTag_sym, ANT_STRING("RegExp String Iterator"), ANT_PROP_ATTR_CONFIGURABLE);
  
  defmethod_sym(js, regexp_proto, js->sym.replace_sym, Ant_Function_CreateWithArity(builtin_regexp_symbol_replace, 2));
  defmethod_sym(js, regexp_proto, js->sym.search_sym, Ant_Function_CreateWithArity(builtin_regexp_symbol_search, 1));
  js_set_getter_desc(js, regexp_proto, "flags", 5, Ant_Function_Create(builtin_regexp_flags_getter), JS_DESC_C);
  defmethod(js, regexp_proto, "compile", 7, Ant_Function_CreateWithArity(builtin_regexp_compile, 2));

  ant_value_t regexp_func = Ant_Function_CreateIntrinsicConstructor(js, builtin_RegExp, regexp_proto, "RegExp", 2);
  ant_value_t regexp_ctor = js_as_obj(regexp_func);
  js_define_species_getter(js, regexp_func);
  js->builtins.regexp_ctor_value = regexp_func;

  ant_value_t empty = js_mkstr_permanent(js, "", 0);
  js->builtins.regexp_empty_string = empty;
  js->mutable_roots.regexp_static_subject = empty;
  for (size_t i = 0; i < sizeof(js->mutable_roots.regexp_static_values) / sizeof(js->mutable_roots.regexp_static_values[0]); i++)
    js->mutable_roots.regexp_static_values[i] = empty;

  static const struct {
    const char *name, *alias;
    ant_cfunc_meta_t get, set, alias_get, alias_set;
  } statics[] = {
#define REGEXP_STATIC(id, name, alias) { name, alias, \
    { regexp_static_get_##id, "get " name, 0, 0 }, { regexp_static_set_##id, "set " name, 1, 0 }, \
    { regexp_static_get_##id, "get " alias, 0, 0 }, { regexp_static_set_##id, "set " alias, 1, 0 } },
#include "modules/regex_statics.h"
  };
  for (size_t i = 0; i < sizeof(statics) / sizeof(statics[0]); i++) {
    js_set_accessor_desc(
      js, regexp_ctor, statics[i].name, strlen(statics[i].name),
      js_mkfun_meta(&statics[i].get), js_mkfun_meta(&statics[i].set), JS_DESC_C
    );
    if (statics[i].alias[0]) js_set_accessor_desc(
      js, regexp_ctor, statics[i].alias, strlen(statics[i].alias),
      js_mkfun_meta(&statics[i].alias_get), js_mkfun_meta(&statics[i].alias_set), JS_DESC_C
    );
  }
  defmethod(js, regexp_ctor, "escape", 6, Ant_Function_CreateWithArity(builtin_regexp_escape, 1));
  js_set_global_builtin(js, "RegExp", regexp_func);

  ant_value_t string_ctor = js_get(js, glob, "String");
  ant_value_t string_proto = js_get(js, string_ctor, "prototype");
  
  defmethod(js, string_proto, "search", 6, Ant_Function_CreateWithArity(builtin_string_search, 1));
  defmethod(js, string_proto, "match", 5, Ant_Function_CreateWithArity(builtin_string_match, 1));
  defmethod(js, string_proto, "matchAll", 8, Ant_Function_CreateWithArity(builtin_string_matchAll, 1));
  defmethod(js, string_proto, "replace", 7, Ant_Function_CreateWithArity(builtin_string_replace, 2));
  defmethod(js, string_proto, "replaceAll", 10, Ant_Function_CreateWithArity(builtin_string_replaceAll, 2));
  
  js->regexp.replace_written = false;
  js->regexp.write_guard_armed = true;

  js->regexp.proto_ptr = js_obj_ptr(regexp_proto);
  js->regexp.ctor_ptr = js_obj_ptr(js_func_obj(regexp_func));
  js->regexp.protector_invalid = false;
  ant_regexp_protector_holder_mark(regexp_proto);
  ant_regexp_protector_holder_mark(js_func_obj(regexp_func));
}

void gc_age_regex_cache(ant_t *js, bool minor) {
  ant_regex_state_t *state = js->regex_state;
  if (!state || minor) return;
  ant_gc_shapes_mark(js, state->instance_shape.base);
  regexp_sites_clear(state);
  compiled_regex_cache_rotate(state);
}

void cleanup_regex_module(ant_t *js) {
  ant_regex_state_t *state = js->regex_state;
  if (!state) return;

  regexp_sites_clear(state);
  compiled_regex_table_clear(&state->compiled[0]);
  compiled_regex_table_clear(&state->compiled[1]);

  ant_shape_release(state->instance_shape.base);
  ant_shape_release(state->instance_shape.shape);
  ant_shape_release(state->result_shape.shape);
  ant_shape_release(state->result_indices_shape.shape);
  free(state->out_buf);
  pcre2_jit_stack_free(state->jit_stack);
  pcre2_match_context_free(state->match_ctx);
  free(state);
  js->regex_state = NULL;
}
