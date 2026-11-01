#include "regex_internal.h"

static ant_value_t regexp_match_fast_global(ant_t *js, const regexp_fast_t *fast, ant_value_t str) {
  ant_value_t matches = Ant_Array_Create(js);
  if (is_err(matches)) return matches;

  regexp_fast_scan_t scan;
  regexp_fast_scan_begin(js, &scan, fast, str);
  ant_value_t error = Ant_Value_Undefined;
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
  return scan.count == 0 ? Ant_Value_Null : matches;
}

ant_value_t regexp_match_fast(ant_t *js, ant_value_t rx, const regexp_fast_t *fast, ant_value_t str) {
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

  js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), Ant_Value_FromNumber(0));

  ant_value_t A = Ant_Array_Create(js);
  if (is_err(A)) return A;
  ant_offset_t n = 0;

  for (;;) {
    ant_value_t result = regexp_exec_abstract(js, rx, str);
    if (is_err(result)) return result;
    if (vtype(result) == kTypeNull) return n == 0 ? Ant_Value_Null : mkval(kTypeArray, vdata(A));

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

ant_value_t builtin_regexp_symbol_match(ant_params_t) {
  if (!is_object_type(call_this))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@match] called on non-object");

  ant_value_t str = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str)) return str;
  return regexp_symbol_match_impl(js, call_this, str);
}

enum {
  MATCHALL_DONE = 1 << 0,
  MATCHALL_GLOBAL = 1 << 1,
  MATCHALL_UNICODE = 1 << 2,
};

ant_value_t regexp_matchall_next(ant_params_t) {
  ant_value_t rx = js_get_slot(call_this, SLOT_MATCHALL_RX);
  ant_value_t str = js_get_slot(call_this, SLOT_MATCHALL_STR);
  ant_value_t state_val = js_get_slot(call_this, SLOT_MATCHALL_STATE);
  uint32_t state = vtype(state_val) == kTypeNumber ? (uint32_t)Ant_Value_AsNumberUnchecked(state_val) : MATCHALL_DONE;
  
  if (state & MATCHALL_DONE)
    return js_iter_result(js, false, Ant_Value_Undefined);

  regexp_fast_t fast;
  if (regexp_fast_get(js, rx, REGEXP_FAST_PRISTINE, &fast)) {
    ant_value_t result = regexp_exec_internal(js, rx, str, false);
    if (is_err(result)) return result;
    if (vtype(result) == kTypeNull || !(state & MATCHALL_GLOBAL))
      js_set_slot(call_this, SLOT_MATCHALL_STATE, Ant_Value_FromNumber(state | MATCHALL_DONE));
    if (vtype(result) == kTypeNull) return js_iter_result(js, false, Ant_Value_Undefined);
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
    js_set_slot(call_this, SLOT_MATCHALL_STATE, Ant_Value_FromNumber(state | MATCHALL_DONE));
    return js_iter_result(js, false, Ant_Value_Undefined);
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
  } else js_set_slot(call_this, SLOT_MATCHALL_STATE, Ant_Value_FromNumber(state | MATCHALL_DONE));

  return js_iter_result(js, true, result);
}

static ant_value_t regexp_matchall_iterator(ant_t *js, ant_value_t matcher, ant_value_t str, uint32_t state) {
  ant_value_t iter = Ant_Object_CreateBare(js);
  if (is_err(iter)) return iter;
  js_set_slot(iter, SLOT_MATCHALL_RX, matcher);
  js_set_slot(iter, SLOT_MATCHALL_STR, str);
  js_set_slot(iter, SLOT_MATCHALL_STATE, Ant_Value_FromNumber(state));
  js_set_proto_init(iter, js->builtins.regexp_matchall_iter_proto_val);
  return iter;
}

static ant_value_t regexp_matchall_fast(ant_t *js, ant_value_t rx, const regexp_fast_t *fast, ant_value_t str) {
  ant_value_t matcher = mkobj(js, 0);
  if (is_err(matcher)) return matcher;
  js_set_proto_init(matcher, js->builtins.regexp_proto);
  ant_value_t flags = regexp_flags_value(js, rx);
  if (!regexp_literal_init_fast(js, matcher, js_get_slot(rx, SLOT_DATA), flags, fast->compiled))
    return Ant_Value_Undefined;
  ant_object_prop_set_unchecked(
    js_obj_ptr(matcher), REGEXP_LAST_INDEX_SLOT,
    ant_object_prop_get_unchecked(fast->obj, fast->li_slot)
  );
  uint32_t state = 0;
  if (fast->flags & REGEXP_FLAG_GLOBAL) state |= MATCHALL_GLOBAL;
  if (fast->flags & (REGEXP_FLAG_UNICODE | REGEXP_FLAG_UNICODE_SET)) state |= MATCHALL_UNICODE;
  return regexp_matchall_iterator(js, matcher, str, state);
}

ant_value_t builtin_regexp_symbol_matchAll(ant_params_t) {
  if (!is_object_type(call_this))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@matchAll] called on non-object");

  ant_value_t str = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str)) return str;

  regexp_fast_t fast;
  if (regexp_fast_get(js, call_this, REGEXP_FAST_PRISTINE, &fast)) {
    ant_value_t iter = regexp_matchall_fast(js, call_this, &fast, str);
    if (vtype(iter) != kTypeUndefined) return iter;
  }

  ant_value_t C = regexp_species_ctor(js, call_this, "RegExp.prototype[@@matchAll]");
  if (is_err(C)) return C;

  ant_value_t flags_val = js_getprop_fallback(js, call_this, "flags");
  if (is_err(flags_val)) return flags_val;
  ant_value_t flags_str = js_tostring_val(js, flags_val);
  if (is_err(flags_str)) return flags_str;

  ant_value_t ctor_args[2] = { call_this, flags_str };
  ant_value_t new_rx = regexp_species_construct(js, call_this, C, ctor_args, 2);
  if (is_err(new_rx)) return new_rx;

  ant_value_t li_val = js_getprop_fallback(js, call_this, "lastIndex");
  if (is_err(li_val)) return li_val;
  size_t last_index = 0;
  ant_value_t status = js_to_length_fast(js, li_val, &last_index);
  if (is_err(status)) return status;
  status = js_setprop(js, new_rx, js_mkstr(js, "lastIndex", 9), Ant_Value_FromNumber((double)last_index));
  if (is_err(status)) return status;

  uint32_t state = 0;
  ant_offset_t flags_len, flags_off = vstr(js, flags_str, &flags_len);
  const char *flags = (const char *)(uintptr_t)flags_off;
  if (memchr(flags, 'g', flags_len)) state |= MATCHALL_GLOBAL;
  if (memchr(flags, 'u', flags_len) || memchr(flags, 'v', flags_len)) state |= MATCHALL_UNICODE;

  return regexp_matchall_iterator(js, new_rx, str, state);
}

ant_value_t builtin_string_matchAll(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "matchAll");
  
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
  ant_value_t rx = sv_vm_call(js->vm, js, regexp_ctor, Ant_Value_Undefined, ctor_args, 2, NULL, regexp_ctor);
  if (is_err(rx)) return rx;

  ant_value_t ma_args[1] = { str };
  return sv_call_native_this(js, builtin_regexp_symbol_matchAll, rx, ma_args, 1);
}

static ant_value_t regexp_symbol_search_impl(ant_t *js, ant_value_t rx, ant_value_t str);

ant_value_t builtin_regexp_symbol_search(ant_params_t) {
  if (!is_object_type(call_this))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@search] called on non-object");

  ant_value_t str = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str)) return str;
  return regexp_symbol_search_impl(js, call_this, str);
}

ant_value_t regexp_search_fast(ant_t *js, const regexp_fast_t *fast, ant_value_t str) {
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
    return rc == PCRE2_ERROR_NOMEMORY ? js_mkerr(js, "oom") : Ant_Value_FromNumber(-1);
  }

  update_regexp_statics(js, str, ovector, ovcount);
  ant_value_t index = Ant_Value_FromNumber((double)byte_offset_to_utf16(str_ptr, ovector[0]));
  regex_match_scope_end(&scope);
  return index;
}

static ant_value_t regexp_symbol_search_impl(ant_t *js, ant_value_t rx, ant_value_t str) {
  regexp_fast_t fast;
  if (regexp_fast_get(js, rx, REGEXP_FAST_BUILTIN_EXEC, &fast)) return regexp_search_fast(js, &fast, str);

  ant_value_t prev_li = js_getprop_fallback(js, rx, "lastIndex");
  if (is_err(prev_li)) return prev_li;
  if (!same_value_values(js, prev_li, Ant_Value_FromNumber(0))) {
    ant_value_t stored = js_setprop(js, rx, js_mkstr(js, "lastIndex", 9), Ant_Value_FromNumber(0));
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

  if (vtype(result) == kTypeNull) return Ant_Value_FromNumber(-1);

  ant_value_t idx = js_getprop_fallback(js, result, "index");
  if (is_err(idx)) return idx;
  return vtype(idx) == kTypeNumber ? idx : Ant_Value_FromNumber(-1);
}

ant_value_t builtin_string_search(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "search");
  
  if (is_err(str)) return str;
  ant_value_t pattern = nargs > 0 ? args[0] : Ant_Value_Undefined;

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

ant_value_t builtin_string_match(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "match");
  
  if (is_err(str)) return str;
  ant_value_t pattern = nargs > 0 ? args[0] : Ant_Value_Undefined;

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
