#include "regex_internal.h"

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

ant_value_t regexp_build_named_groups_meta(ant_t *js, const compiled_regex_cache_entry_t *compiled) {
  if (compiled->namecount == 0 || !compiled->group_names) return Ant_Value_Undefined;

  ant_value_t meta = Ant_Array_Create(js);
  if (is_err(meta)) return meta;

  for (uint32_t i = 0; i < compiled->namecount; i++) {
    const regexp_group_name_t *entry = &compiled->group_names[i];
    ant_value_t name_val = js_mkstr(js, entry->name, entry->length);
    if (is_err(name_val)) return name_val;
    js_arr_push(js, meta, name_val);
    js_arr_push(js, meta, Ant_Value_FromNumber((double)entry->group));
  }

  return meta;
}

__attribute__((aligned(16)))
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

void compiled_regex_table_clear(
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

void compiled_regex_cache_rotate(ant_regex_state_t *state) {
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

__attribute__((noinline)) bool regexp_line_code_prepare(compiled_regex_cache_entry_t *entry) {
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

compiled_regex_cache_entry_t *compiled_regex_cache_acquire(
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

void compiled_regex_cache_release(
  compiled_regex_cache_entry_t *entry
) {
  if (!entry) return;
  Ant_Assert(entry->active_refs > 0, "active compiled regexp reference underflow");
  entry->active_refs--;
  compiled_regex_entry_maybe_free(entry);
}

__attribute__((noinline)) bool regex_match_scope_begin_slow(
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

void regex_match_scope_end(regex_match_scope_t *scope) {
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

void regexp_object_finalize(ant_t *js, ant_object_t *obj) {
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

void regexp_object_compiled_detach(ant_t *js, ant_value_t regexp) {
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

void regexp_sites_clear(ant_regex_state_t *state) {
  for (size_t i = 0; i < REGEXP_SITE_CACHE_SIZE; i++) regexp_site_release(&state->sites[i]);
}

compiled_regex_cache_entry_t *regexp_site_lookup(ant_t *js, ant_value_t source, ant_value_t flags, uint8_t flags_mask) {
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

compiled_regex_cache_entry_t *regexp_attach_compiled(
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

compiled_regex_cache_entry_t *regex_get_or_compile(
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
