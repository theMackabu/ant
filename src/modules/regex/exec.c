#include "regex_internal.h"

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

static ant_value_t builtin_regexp_groups_getter(ant_params_t) {
  if (!is_object_type(call_this)) return Ant_Value_Undefined;

  ant_value_t cached = js_get_slot(call_this, SLOT_REGEXP_GROUPS_CACHE);
  if (is_object_type(cached)) return cached;

  ant_value_t meta = js_get_slot(call_this, SLOT_REGEXP_RESULT_GROUPS);
  if (!is_object_type(meta)) return Ant_Value_Undefined;

  ant_value_t groups = Ant_Object_CreateBare(js);
  if (is_err(groups)) return groups;
  js_set_proto_init(groups, Ant_Value_Null);

  for (ant_offset_t i = 0; ; i += 2) {
    ant_value_t name = js_arr_get(js, meta, i);
    if (vtype(name) == kTypeUndefined) break;
    ant_value_t index_val = js_arr_get(js, meta, i + 1);
    ant_offset_t index = (vtype(index_val) == kTypeNumber) ? (ant_offset_t)Ant_Value_AsNumberUnchecked(index_val) : 0;
    char idxstr[16];
    (void)uint_to_str(idxstr, sizeof(idxstr), (uint64_t)index);
    ant_value_t value = js_getprop_fallback(js, call_this, idxstr);
    ant_offset_t name_len, name_off = vstr(js, name, &name_len);
    ant_value_t status = setprop_cstr(js, groups, (const char *)(uintptr_t)name_off, (size_t)name_len, value);
    if (is_err(status)) return status;
  }

  js_set_slot(call_this, SLOT_REGEXP_GROUPS_CACHE, groups);
  return groups;
}

bool regexp_pos_last_index(utf16_pos_t *pos, double index, uint8_t flags, PCRE2_SIZE *start) {
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

double regexp_advance_string_index(
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
  if (start == PCRE2_UNSET) return Ant_Value_Undefined;

  ant_value_t pair = Ant_Array_Create(js);
  if (is_err(pair)) return pair;
  js_arr_push(js, pair, Ant_Value_FromNumber((double)utf16_pos_units(pos, start)));
  js_arr_push(js, pair, Ant_Value_FromNumber((double)utf16_pos_units(pos, end)));
  
  return pair;
}

static ant_value_t regexp_build_indices_groups(
  ant_t *js,
  ant_value_t groups_meta,
  ant_value_t indices_arr
) {
  ant_value_t groups = Ant_Object_CreateBare(js);
  if (is_err(groups)) return groups;
  js_set_proto_init(groups, Ant_Value_Null);

  for (ant_offset_t i = 0; ; i += 2) {
    ant_value_t name = js_arr_get(js, groups_meta, i);
    if (vtype(name) == kTypeUndefined) break;
    
    ant_value_t index_val = js_arr_get(js, groups_meta, i + 1);
    ant_offset_t index = (vtype(index_val) == kTypeNumber) ? (ant_offset_t)Ant_Value_AsNumberUnchecked(index_val) : 0;
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
  ant_value_t indices_arr = Ant_Array_Create(js);
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
  } else if (is_err(setprop_cstr(js, indices_arr, "groups", 6, Ant_Value_Undefined))) return js_mkerr(js, "oom");

  return indices_arr;
}

static ant_value_t regexp_attach_exec_result(ant_t *js,const regexp_exec_result_ctx_t *ctx) {
  bool has_named_groups = is_object_type(ctx->groups_meta);
  
  if (!has_named_groups && regexp_result_apply_shape(
    js, ctx->array, ctx->index, ctx->input, Ant_Value_Undefined,
    ctx->indices, ctx->with_indices
  )) return ctx->array;

  if (
    is_err(setprop_cstr(js, ctx->array, "index", 5, ctx->index)) ||
    is_err(setprop_cstr(js, ctx->array, "input", 5, ctx->input))
  ) return js_mkerr(js, "oom");

  if (has_named_groups) {
    js_set_slot(ctx->array, SLOT_REGEXP_RESULT_GROUPS, ctx->groups_meta);
    js_set_slot(ctx->array, SLOT_REGEXP_GROUPS_CACHE, Ant_Value_Undefined);
    js_set_getter_desc(
      js, js_as_obj(ctx->array), "groups", 6,
      Ant_Function_Create(builtin_regexp_groups_getter), JS_DESC_E | JS_DESC_C
    );
  }
  else if (is_err(setprop_cstr(js, ctx->array, "groups", 6, Ant_Value_Undefined))) 
    return js_mkerr(js, "oom");

  if (
    ctx->with_indices &&
    is_err(setprop_cstr(js, ctx->array, "indices", 7, ctx->indices))
  ) return js_mkerr(js, "oom");

  return ctx->array;
}

__attribute__((noinline)) bool regexp_subject_has_js_line_breaks(const char *str, size_t len) {
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

ant_value_t regexp_exec_result(
  ant_t *js, ant_value_t regexp, const compiled_regex_cache_entry_t *compiled, ant_value_t str_arg,
  utf16_pos_t *pos, double index, const PCRE2_SIZE *ovector, uint32_t ovcount
) {
  const char *str_ptr = pos->str;
  bool has_indices = (compiled->flags_mask & REGEXP_FLAG_HAS_INDICES) != 0;
  ant_value_t groups_meta = vtype(regexp) != kTypeUndefined
    ? js_get_slot(regexp, SLOT_REGEXP_NAMED_GROUPS)
    : regexp_build_named_groups_meta(js, compiled);
  if (is_err(groups_meta)) return groups_meta;
  ant_value_t result_arr = Ant_Array_Create(js);
  if (is_err(result_arr)) return result_arr;
  for (uint32_t i = 0; i < ovcount; i++) {
    PCRE2_SIZE start = ovector[2*i];
    PCRE2_SIZE end = ovector[2*i+1];
    if (start == PCRE2_UNSET) {
      js_arr_push(js, result_arr, Ant_Value_Undefined);
    } else {
      ant_value_t match_str = js_mkstr_byte_range(js, str_ptr, start, end - start);
      if (is_err(match_str)) return match_str;
      js_arr_push(js, result_arr, match_str);
    }
  }

  ant_value_t indices = Ant_Value_Undefined;
  if (has_indices) {
    indices = regexp_build_indices_result(js, groups_meta, pos, ovector, ovcount);
    if (is_err(indices)) return indices;
  }

  return regexp_attach_exec_result(js, &(regexp_exec_result_ctx_t) {
    .array = result_arr,
    .index = Ant_Value_FromNumber(index),
    .input = str_arg,
    .groups_meta = groups_meta,
    .indices = indices,
    .with_indices = has_indices,
  });
}

static __attribute__((noinline))
ant_value_t regexp_exec_general(ant_t *js, ant_value_t regexp, ant_value_t str_arg, bool truthy_only) {
  compiled_regex_cache_entry_t *compiled_hint;
  uint8_t flags_mask = regexp_flags_mask(js, regexp, &compiled_hint);

  double li = 0;
  ant_object_t *lastindex_obj = js_obj_ptr(regexp);
  uint32_t lastindex_slot = REGEXP_LAST_INDEX_SLOT;
  ant_value_t li_val = Ant_Value_Undefined;
  bool known = lastindex_obj && !lastindex_obj->flags.is_exotic && js->regex_state &&
    lastindex_obj->shape == js->regex_state->instance_shape.shape;
  if (!known) known = regexp_lastindex_fast_location(compiled_hint, regexp, &lastindex_obj, &lastindex_slot);
  if (known && !is_tagged(li_val = ant_object_prop_get_unchecked(lastindex_obj, lastindex_slot))) li = Ant_Value_AsNumberUnchecked(li_val);
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
    if (!(flags_mask & (REGEXP_FLAG_GLOBAL | REGEXP_FLAG_STICKY))) return Ant_Value_Null;
    ant_value_t stored = regexp_set_lastindex(js, NULL, regexp, Ant_Value_FromNumber(0));
    return is_err(stored) ? stored : Ant_Value_Null;
  }
  regexp_lastindex_cache_location(js, fast.compiled, regexp);
  return regexp_exec_run(js, regexp, &fast, li, str_arg, truthy_only);
}

__attribute__((aligned(16)))
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
  return regexp_exec_run(js, regexp, &fast, Ant_Value_AsNumberUnchecked(li_val), str_arg, truthy_only);
}

ant_value_t builtin_regexp_exec(ant_params_t) {
  if (!regexp_has_internal_slots(js, call_this))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype.exec called on incompatible receiver");

  ant_value_t str_arg = nargs > 0 ? coerce_to_str(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str_arg)) return str_arg;

  return regexp_exec_internal(js, call_this, str_arg, false);
}

bool regexp_named_capture(void *ctx, const char *name, size_t len, repl_capture_t *out) {
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

ant_value_t regexp_build_groups_object(
  ant_t *js, const compiled_regex_cache_entry_t *compiled,
  const char *str_ptr, const PCRE2_SIZE *ovector, uint32_t ovcount
) {
  ant_value_t groups = Ant_Object_CreateBare(js);
  if (is_err(groups)) return groups;
  js_set_proto_init(groups, Ant_Value_Null);

  for (uint32_t i = 0; i < compiled->namecount; i++) {
    const regexp_group_name_t *entry = &compiled->group_names[i];
    PCRE2_SIZE start = entry->group < ovcount ? ovector[2 * entry->group] : PCRE2_UNSET;
    ant_value_t value = Ant_Value_Undefined;
    if (start != PCRE2_UNSET) {
      value = js_mkstr_byte_range(js, str_ptr, start, ovector[2 * entry->group + 1] - start);
      if (is_err(value)) return value;
    } else if (lkp(js, groups, entry->name, entry->length).obj) continue;
    ant_value_t status = setprop_cstr(js, groups, entry->name, entry->length, value);
    if (is_err(status)) return status;
  }

  return groups;
}

static ant_value_t regexp_exec_with_exec_fn(ant_t *js, ant_value_t rx, ant_value_t str, ant_value_t exec_fn) {
  if (vtype(exec_fn) == kTypeFunction || vtype(exec_fn) == kTypeBuiltin) {
    ant_value_t call_args[1] = { str };
    ant_value_t result = sv_vm_call(js->vm, js, exec_fn, rx, call_args, 1, NULL, Ant_Value_Undefined);
    if (is_err(result)) return result;
    if (!is_object_type(result) && vtype(result) != kTypeNull)
      return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp exec returned non-object");
    return result;
  }

  ant_value_t call_args[1] = { str };
  return sv_call_native_this(js, builtin_regexp_exec, rx, call_args, 1);
}

ant_value_t regexp_exec_abstract(ant_t *js, ant_value_t rx, ant_value_t str) {
  ant_value_t exec_fn = js_getprop_fallback(js, rx, "exec");
  if (is_err(exec_fn)) return exec_fn;
  return regexp_exec_with_exec_fn(js, rx, str, exec_fn);
}

ant_value_t regexp_read_flags(ant_t *js, ant_value_t rx, bool *global, bool *full_unicode) {
  ant_value_t flags_val = js_getprop_fallback(js, rx, "flags");
  if (is_err(flags_val)) return flags_val;
  ant_value_t flags_str = js_tostring_val(js, flags_val);
  if (is_err(flags_str)) return flags_str;
  ant_offset_t len, off = vstr(js, flags_str, &len);
  const char *flags = (const char *)(uintptr_t)off;
  if (global) *global = memchr(flags, 'g', (size_t)len) != NULL;
  if (full_unicode) *full_unicode = memchr(flags, 'u', (size_t)len) || memchr(flags, 'v', (size_t)len);
  return Ant_Value_Undefined;
}

ant_value_t regexp_advance_last_index(ant_t *js, ant_value_t rx, ant_value_t str, bool full_unicode) {
  ant_value_t li_val = js_getprop_fallback(js, rx, "lastIndex");
  if (is_err(li_val)) return li_val;
  double li = vtype(li_val) == kTypeNumber ? Ant_Value_AsNumberUnchecked(li_val) : 0;
  ant_offset_t len, off = vstr(js, str, &len);
  double next = regexp_advance_string_index(js, (const char *)(uintptr_t)off, len, li, full_unicode);
  return js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), Ant_Value_FromNumber(next));
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

ant_value_t builtin_regexp_test(ant_params_t) {
  if (!is_object_type(call_this))
    return js_mkerr_typed(js, JS_ERR_TYPE, "test called on non-object");
  ant_value_t str_arg = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str_arg)) return str_arg;
  regexp_fast_t fast;
  if (regexp_fast_get(js, call_this, REGEXP_FAST_PRISTINE, &fast)) {
    ant_value_t result = regexp_exec_internal(js, call_this, str_arg, true);
    return is_err(result) ? result : mkval(kTypeBool, vtype(result) != kTypeNull ? 1 : 0);
  }
  ant_value_t exec_fn = js_getprop_fallback(js, call_this, "exec");
  if (is_err(exec_fn)) return exec_fn;

  ant_value_t result;
  if (vtype(exec_fn) == kTypeBuiltin && js_cfunc_same_entrypoint(exec_fn, builtin_regexp_exec)) {
    result = regexp_exec_internal(js, call_this, str_arg, true);
  } else result = regexp_exec_with_exec_fn(js, call_this, str_arg, exec_fn);
  
  if (is_err(result)) return result;
  return mkval(kTypeBool, vtype(result) != kTypeNull ? 1 : 0);
}
