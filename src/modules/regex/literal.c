#include "regex_internal.h"

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

bool regexp_literal_init_fast(
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
  ant_object_prop_set_unchecked(obj, REGEXP_LAST_INDEX_SLOT, Ant_Value_FromNumber(0));
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
  return compiled ? regexp_flags_string_for_mask(js, compiled->flags_mask) : Ant_Value_Undefined;
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
    ant_value_t result = regexp_exec_run(js, Ant_Value_Undefined, &fast, 0, arg, false);
    compiled_regex_cache_release(compiled);
    return result;
  }

  ant_value_t regexp_obj = regexp_create_literal(js, pattern, flags);
  if (is_err(regexp_obj)) return regexp_obj;
  if (vtype(arg) == kTypeString && regexp_literal_exec_builtin_guard(js))
    return regexp_exec_internal(js, regexp_obj, arg, false);
  return regexp_exec_abstract(js, regexp_obj, arg);
}

static bool regexp_string_replace_slot_is_builtin(ant_t *js, ant_object_t *proto, uint32_t slot) {
  if (slot >= proto->prop_count) return false;
  const ant_shape_prop_t *prop = ant_shape_prop_at(proto->shape, slot);
  return
    prop && prop->type == ANT_SHAPE_KEY_STRING && prop->key.interned == js->intern.replace &&
    !prop->has_getter && !prop->has_setter &&
    js_cfunc_same_entrypoint(ant_object_prop_get_unchecked(proto, slot), builtin_string_replace);
}

static bool regexp_string_replace_is_builtin(ant_t *js, ant_regex_state_t *state) {
  if (!is_object_type(js->sym.string_proto)) return false;
  ant_object_t *proto = js_obj_ptr(js->sym.string_proto);
  if (!proto->shape) return false;
  if (regexp_string_replace_slot_is_builtin(js, proto, state->string_replace_slot)) return true;

  int32_t slot = ant_shape_lookup_interned(proto->shape, js->intern.replace);
  if (slot < 0) return false;
  state->string_replace_slot = (uint32_t)slot;
  return regexp_string_replace_slot_is_builtin(js, proto, (uint32_t)slot);
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

  return regexp_string_replace_is_builtin(js, state);
}

ant_value_t regexp_literal_replace_call(
  ant_t *js, ant_value_t str, ant_value_t pattern, ant_value_t flags, ant_value_t replacement
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
  return sv_vm_call(js->vm, js, replace_fn, str, call_args, 2, NULL, Ant_Value_Undefined);
}

ant_value_t regexp_string_pattern_call(ant_t *js, ant_value_t str, ant_value_t pattern, bool search) {
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
      : regexp_match_fast(js, Ant_Value_Undefined, &fast, str);
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
  return sv_vm_call(js->vm, js, method, rx, call_args, 1, NULL, Ant_Value_Undefined);
}
