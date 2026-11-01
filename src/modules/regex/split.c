#include "regex_internal.h"

static ant_value_t regexp_split_fast(ant_t *js, const regexp_fast_t *fast, ant_value_t str, uint32_t limit) {
  ant_value_t result = Ant_Array_Create(js);
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
        ? Ant_Value_Undefined
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
    regexp_fast_set_last_index(js, Ant_Value_Undefined, fast, (double)byte_offset_to_utf16((const char *)(uintptr_t)str_off, last_index));
  }
  return result;
}

ant_value_t regexp_split_limit(ant_t *js, ant_value_t limit, uint32_t *out) {
  *out = UINT32_MAX;
  if (vtype(limit) == kTypeUndefined) return Ant_Value_Undefined;
  if (vtype(limit) == kTypeNumber) {
    *out = js_to_uint32(Ant_Value_AsNumberUnchecked(limit));
    return Ant_Value_Undefined;
  }
  ant_value_t primitive = limit;
  if (is_object_type(limit) || vtype(limit) == kTypeBuiltin) {
    primitive = js_to_primitive(js, limit, 2);
    if (is_err(primitive)) return primitive;
  }
  if (vtype(primitive) == kTypeBigInt || vtype(primitive) == kTypeSymbol)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert value to a number");
  *out = js_to_uint32(js_to_number(js, primitive));
  return Ant_Value_Undefined;
}

bool regexp_split_try_fast(ant_t *js, ant_value_t rx, ant_value_t str, ant_value_t limit, ant_value_t *out) {
  uint32_t lim = UINT32_MAX;
  if (vtype(limit) == kTypeNumber) lim = js_to_uint32(Ant_Value_AsNumberUnchecked(limit));
  else if (vtype(limit) != kTypeUndefined) return false;

  regexp_fast_t fast;
  if (!regexp_fast_get(js, rx, REGEXP_FAST_PRISTINE, &fast)) return false;

  fast.obj = NULL;
  *out = regexp_split_fast(js, &fast, str, lim);
  return true;
}

ant_value_t builtin_regexp_symbol_split(ant_params_t) {
  if (!is_object_type(call_this))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@split] called on non-object");

  ant_value_t str = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "", 0);
  if (is_err(str)) return str;

  ant_value_t fast_split;
  if (regexp_split_try_fast(js, call_this, str, nargs >= 2 ? args[1] : Ant_Value_Undefined, &fast_split)) return fast_split;

  ant_value_t C = regexp_species_ctor(js, call_this, "RegExp.prototype[@@split]");
  if (is_err(C)) return C;

  ant_value_t flags_val = js_get(js, call_this, "flags");
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

  ant_value_t ctor_args[2] = { call_this, new_flags };
  ant_value_t splitter = regexp_species_construct(js, call_this, C, ctor_args, 2);
  if (is_err(splitter)) return splitter;

  ant_value_t A = Ant_Array_Create(js);
  if (is_err(A)) return A;
  ant_offset_t lengthA = 0;

  uint32_t lim;
  ant_value_t lim_status = regexp_split_limit(js, nargs >= 2 ? args[1] : Ant_Value_Undefined, &lim);
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
    js_setprop(js, splitter, lastIndex_key, Ant_Value_FromNumber((double)q));

    ant_value_t z = regexp_exec_abstract(js, splitter, str);
    if (is_err(z)) return z;

    if (vtype(z) == kTypeNull) {
      str_off = vstr(js, str, &str_len);
      q = (ant_offset_t)regexp_advance_string_index(js, (const char *)(uintptr_t)str_off, str_len, (double)q, full_unicode);
      continue;
    }

    ant_value_t li_val = js_get(js, splitter, "lastIndex");
    if (is_err(li_val)) return li_val;
    double e_raw = vtype(li_val) == kTypeNumber ? Ant_Value_AsNumberUnchecked(li_val) : 0;
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
