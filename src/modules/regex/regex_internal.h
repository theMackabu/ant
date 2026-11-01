#ifndef ANT_MODULES_REGEX_REGEX_INTERNAL_H
#define ANT_MODULES_REGEX_REGEX_INTERNAL_H

#include "silver/call.h"

#include "cfunc.h"
#include "utf8.h"
#include "utils.h"
#include "ascii.h"
#include "index.h"
#include "modules/regex.h"
#include "protectors.h"
#include "modules/symbol.h"
#include "modules/iterator.h"

#include <pcre2.h>

static constexpr size_t REGEXP_REQUIRED_LITERAL_MAX = 32;
static constexpr size_t REGEXP_JIT_EAGER_SUBJECT = 1024;

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
  bool jit_pending;
  uint8_t interpreted_matches;
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
  bool interpreted;
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

static constexpr uint32_t REGEXP_LAST_INDEX_SLOT = 0;

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

typedef struct {
  const compiled_regex_cache_entry_t *compiled;
  const char *str;
  const PCRE2_SIZE *ovector;
  uint32_t ovcount;
} regexp_named_ctx_t;

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

#define REGEXP_FLAG(id, name, ch) ant_value_t builtin_regexp_get_##name(ant_params_t);
#include "modules/regex_flags.h"

#define REGEXP_STATIC(id, name, alias) \
  ant_value_t regexp_static_get_##id(ant_params_t); \
  ant_value_t regexp_static_set_##id(ant_params_t);
#include "modules/regex_statics.h"

ant_value_t regexp_build_named_groups_meta(ant_t *js, const compiled_regex_cache_entry_t *compiled);
__attribute__((noinline, cold)) void update_regexp_statics_eager(
  ant_t *js, ant_value_t subject,
  PCRE2_SIZE *ovector, uint32_t ovcount
);
int regexp_legacy_backref_callout(pcre2_callout_block *block, void *data);
int regexp_callout_found(pcre2_callout_enumerate_block *block, void *data);
js_cstr_t regexp_translate_cstr(
  const char *src, size_t src_len,
  char *stack_buf, size_t stack_size,
  uint8_t flags, bool plain_line_anchors, bool *line_anchors
);
typedef struct {
  size_t start, end;
  uint64_t hash;
  bool escaped;
} regexp_name_span_t;

typedef struct {
  regexp_name_span_t *items;
  uint32_t count, capacity;
  regexp_name_span_t inline_items[16];
} regexp_name_spans_t;

void regexp_name_spans_init(regexp_name_spans_t *spans, const char *src, size_t src_len, bool v_flag);
void regexp_name_spans_free(regexp_name_spans_t *spans);
size_t regexp_name_span_decode(const char *src, const regexp_name_span_t *span, char *out);
void regexp_init_flags(ant_t *js, ant_value_t obj, ant_value_t flags, bool is_new);
ant_value_t regexp_species_ctor(ant_t *js, ant_value_t rx, const char *method);
ant_value_t regexp_species_construct(ant_t *js, ant_value_t rx, ant_value_t ctor, ant_value_t *ctor_args, int nargs);
__attribute__((aligned(16)))
pcre2_match_context *regex_get_match_context(ant_t *js);
void compiled_regex_table_clear(
  compiled_regex_cache_table_t *table
);
void compiled_regex_cache_rotate(ant_regex_state_t *state);
__attribute__((noinline)) bool regexp_line_code_prepare(compiled_regex_cache_entry_t *entry);
__attribute__((noinline, cold)) bool regexp_jit_tier_up(compiled_regex_cache_entry_t *entry, size_t subject_len);
compiled_regex_cache_entry_t *compiled_regex_cache_acquire(
  ant_t *js,
  const char *pattern,
  size_t pattern_len,
  uint8_t flags_mask,
  const char **syntax_error
);
void compiled_regex_cache_release(
  compiled_regex_cache_entry_t *entry
);
__attribute__((noinline)) bool regex_match_scope_begin_slow(
  compiled_regex_cache_entry_t *entry, regex_match_scope_t *scope
);
void regex_match_scope_end(regex_match_scope_t *scope);
void regexp_object_finalize(ant_t *js, ant_object_t *obj);
void regexp_object_compiled_detach(ant_t *js, ant_value_t regexp);
void regexp_sites_clear(ant_regex_state_t *state);
compiled_regex_cache_entry_t *regexp_site_lookup(ant_t *js, ant_value_t source, ant_value_t flags, uint8_t flags_mask);
compiled_regex_cache_entry_t *regexp_attach_compiled(
  ant_t *js, ant_value_t regexp_obj, compiled_regex_cache_entry_t *compiled
);
compiled_regex_cache_entry_t *regex_get_or_compile(
  ant_t *js,
  ant_value_t regexp_obj,
  uint8_t flags_mask,
  compiled_regex_cache_entry_t *compiled
);
bool regexp_has_internal_slots(ant_t *js, ant_value_t value);
ant_value_t regexp_syntax_error_value(
  ant_t *js, const char *pattern, size_t pattern_len, uint8_t flags_mask, const char *message
);
ant_value_t builtin_RegExp(ant_params_t);
bool regexp_pos_last_index(utf16_pos_t *pos, double index, uint8_t flags, PCRE2_SIZE *start);
double regexp_advance_string_index(
  ant_t *js, const char *str, ant_offset_t len, double index, bool full_unicode
);
__attribute__((noinline)) bool regexp_subject_has_js_line_breaks(const char *str, size_t len);
bool regexp_lastindex_fast_location(
  compiled_regex_cache_entry_t *compiled,
  ant_value_t regexp,
  ant_object_t **out_obj,
  uint32_t *out_slot
);
void regexp_lastindex_cache_location(
  ant_t *js,
  compiled_regex_cache_entry_t *compiled,
  ant_value_t regexp
);
ant_value_t regexp_set_lastindex(
  ant_t *js,
  compiled_regex_cache_entry_t *compiled,
  ant_value_t regexp,
  ant_value_t value
);
bool regexp_fast_get(ant_t *js, ant_value_t rx, regexp_fast_tier_t tier, regexp_fast_t *fast);
__attribute__((noinline)) ant_value_t regexp_read_lastindex_slow(
  ant_t *js, compiled_regex_cache_entry_t *compiled_hint, ant_value_t regexp, ant_value_t str_arg, double *out
);
ant_value_t regexp_exec_result(
  ant_t *js, ant_value_t regexp, const compiled_regex_cache_entry_t *compiled, ant_value_t str_arg,
  utf16_pos_t *pos, double index, const PCRE2_SIZE *ovector, uint32_t ovcount
);
__attribute__((aligned(16)))
ant_value_t regexp_exec_internal(ant_t *js, ant_value_t regexp, ant_value_t str_arg, bool truthy_only);
ant_value_t builtin_regexp_exec(ant_params_t);
ant_value_t builtin_regexp_toString(ant_params_t);
ant_value_t builtin_regexp_compile(ant_params_t);
ant_value_t builtin_regexp_escape(ant_params_t);
bool regexp_named_capture(void *ctx, const char *name, size_t len, repl_capture_t *out);
ant_value_t regexp_build_groups_object(
  ant_t *js, const compiled_regex_cache_entry_t *compiled,
  const char *str_ptr, const PCRE2_SIZE *ovector, uint32_t ovcount
);
ant_value_t regexp_exec_abstract(ant_t *js, ant_value_t rx, ant_value_t str);
ant_value_t regexp_read_flags(ant_t *js, ant_value_t rx, bool *global, bool *full_unicode);
ant_value_t regexp_advance_last_index(ant_t *js, ant_value_t rx, ant_value_t str, bool full_unicode);
ant_value_t builtin_regexp_test(ant_params_t);
bool regexp_shape_flags_pristine(ant_t *js, const ant_shape_t *own_shape);
ant_value_t regexp_read_flag(ant_t *js, ant_value_t rx, int index);
ant_value_t builtin_regexp_source_getter(ant_params_t);
ant_value_t builtin_regexp_flags_getter(ant_params_t);
ant_value_t regexp_match_fast(ant_t *js, ant_value_t rx, const regexp_fast_t *fast, ant_value_t str);
ant_value_t builtin_regexp_symbol_match(ant_params_t);
ant_value_t regexp_matchall_next(ant_params_t);
ant_value_t builtin_regexp_symbol_matchAll(ant_params_t);
ant_value_t builtin_string_matchAll(ant_params_t);
const char *find_bytes(const char *haystack, ant_offset_t haystack_len, const char *needle, ant_offset_t needle_len);
bool regexp_literal_init_fast(
  ant_t *js, ant_value_t regexp_obj, ant_value_t pattern, ant_value_t flags,
  compiled_regex_cache_entry_t *compiled
);
ant_value_t regexp_replace_fast_string(
  ant_t *js, const regexp_fast_t *fast, ant_value_t str, ant_value_t replace_str
);
ant_value_t regexp_replace_fast_function(
  ant_t *js, const regexp_fast_t *fast, ant_value_t str, ant_value_t replace_fn
);
ant_value_t regexp_symbol_replace_impl(
  ant_t *js, ant_value_t rx, ant_value_t str, ant_value_t replace_value, const regexp_fast_t *known
);
ant_value_t builtin_regexp_symbol_replace(ant_params_t);
ant_value_t builtin_regexp_symbol_search(ant_params_t);
ant_value_t regexp_search_fast(ant_t *js, const regexp_fast_t *fast, ant_value_t str);
ant_value_t builtin_regexp_symbol_split(ant_params_t);
bool str_buf_append(char **buf, size_t *len, size_t *cap, const char *data, size_t n);
ant_value_t builtin_string_replace(ant_params_t);
ant_value_t builtin_string_replaceAll(ant_params_t);
ant_value_t regexp_string_pattern_call(ant_t *js, ant_value_t str, ant_value_t pattern, bool search);
ant_value_t builtin_string_search(ant_params_t);
ant_value_t builtin_string_match(ant_params_t);

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
  return vtype(internal_mask) == kTypeNumber ? (uint8_t)Ant_Value_AsNumberUnchecked(internal_mask) : 0;
}

static inline ant_value_t regexp_static_empty(ant_t *js) {
  return js->builtins.regexp_empty_string
    ? js->builtins.regexp_empty_string
    : Ant_Value_Undefined;
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

static inline const char *regexp_pattern_syntax_error(const char *pattern, size_t pattern_len, uint8_t flags_mask) {
  return regexp_syntax_error(
    pattern, pattern_len,
    (flags_mask & (REGEXP_FLAG_UNICODE | REGEXP_FLAG_UNICODE_SET)) != 0,
    (flags_mask & REGEXP_FLAG_UNICODE_SET) != 0
  );
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

static inline regexp_site_entry_t *regexp_site_for(ant_regex_state_t *state, ant_value_t source) {
  return &state->sites[(vdata(source) >> 4) % REGEXP_SITE_CACHE_SIZE];
}

static inline ant_offset_t regexp_vstr(ant_t *js, ant_value_t value, ant_offset_t *len) {
  ant_flat_string_t *flat = ant_str_flat_ptr(value);
  if (!flat) return vstr(js, value, len);
  *len = flat->len;
  return (ant_offset_t)(uintptr_t)flat->bytes;
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
  if (
    __builtin_expect(jit_ready && !sticky, 1) ||
    (compiled->jit_pending && !sticky && code == compiled->code && regexp_jit_tier_up(compiled, (size_t)str_len))
  ) {
    rc = pcre2_jit_match(
      code, (PCRE2_SPTR)str_ptr, str_len, start_offset,
      match_options, scope->match_data, match_ctx
    );
  } else {
    if (compiled->interpreted_matches < UINT8_MAX) compiled->interpreted_matches++;
    scope->interpreted = true;
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

static inline double regexp_fast_last_index(const regexp_fast_t *fast) {
  return fast->obj ? Ant_Value_AsNumberUnchecked(ant_object_prop_get_unchecked(fast->obj, fast->li_slot)) : 0;
}

static __attribute__((always_inline)) inline ant_value_t regexp_fast_set_last_index(
  ant_t *js, ant_value_t rx, const regexp_fast_t *fast, double value
) {
  if (fast->obj) {
    ant_object_prop_set_unchecked(fast->obj, fast->li_slot, Ant_Value_FromNumber(value));
    return Ant_Value_Undefined;
  }
  if (vtype(rx) == kTypeUndefined) return Ant_Value_Undefined;
  return regexp_set_lastindex(js, fast->compiled, rx, Ant_Value_FromNumber(value));
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
    return is_err(stored) ? stored : Ant_Value_Null;
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
    if (!uses_last_index) return Ant_Value_Null;
    ant_value_t stored = regexp_fast_set_last_index(js, regexp, fast, 0);
    return is_err(stored) ? stored : Ant_Value_Null;
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
    return Ant_Value_True;
  }

  ant_value_t result = regexp_exec_result(js, regexp, compiled, str_arg, &pos, index, ovector, ovcount);
  utf16_pos_save(&pos);
  regex_match_scope_end(&scope);
  return result;
}

static inline PCRE2_SIZE regexp_advance_empty_match(const char *str_ptr, ant_offset_t str_len, PCRE2_SIZE offset) {
  if (offset < (PCRE2_SIZE)str_len) return offset + (PCRE2_SIZE)utf8_char_len_at(str_ptr, str_len, (ant_offset_t)offset);
  return offset + 1;
}

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
  scan->scope.interpreted = false;
  scan->global = (fast->flags & REGEXP_FLAG_GLOBAL) != 0;
  scan->sticky = (fast->flags & REGEXP_FLAG_STICKY) != 0;
  scan->done = false;
  scan->count = 0;
  scan->last_end = 0;
  if (scan->global || !scan->sticky) return;
  utf16_pos_init(&scan->pos, scan->str_ptr, str_len);
  if (regexp_pos_last_index(&scan->pos, regexp_fast_last_index(fast), fast->flags, &scan->offset)) return;
  regexp_fast_set_last_index(js, Ant_Value_Undefined, fast, 0);
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
  if (scan->global) regexp_fast_set_last_index(js, Ant_Value_Undefined, scan->fast, 0);
  else if (scan->sticky) regexp_fast_set_last_index(
    js, Ant_Value_Undefined, scan->fast,
    scan->count ? (double)utf16_pos_units(&scan->pos, scan->last_end) : 0
  );
}

static inline ant_value_t regexp_replace_fast(
  ant_t *js, const regexp_fast_t *fast, ant_value_t str, ant_value_t replacement, bool func_replace
) {
  return func_replace
    ? regexp_replace_fast_function(js, fast, str, replacement)
    : regexp_replace_fast_string(js, fast, str, replacement);
}

#endif
