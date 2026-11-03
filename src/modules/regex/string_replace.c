#include "regex_internal.h"

bool str_buf_append(char **buf, size_t *len, size_t *cap, const char *data, size_t n) {
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
    ant_value_t cb_args[3] = { matched, Ant_Value_FromNumber((double)position), str };
    ant_value_t r = sv_vm_call(js->vm, js, replacement, Ant_Value_Undefined, cb_args, 3, NULL, Ant_Value_Undefined);
    
    if (vtype(r) == kTypeError) return r;
    ant_value_t r_str = js_tostring_val(js, r);
    
    if (is_err(r_str)) return r_str;
    ant_offset_t rlen, roff = vstr(js, r_str, &rlen);
    
    if (!str_buf_append(buf, buf_len, buf_cap, (const char *)(uintptr_t)roff, rlen)) return js_mkerr(js, "oom");
  } else if (!str_buf_append(buf, buf_len, buf_cap, repl_ptr, repl_len)) return js_mkerr(js, "oom");
  
  return Ant_Value_Undefined;
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
  ant_value_t result = Ant_Value_Undefined;
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

static ant_value_t string_replace_impl(ant_native_params_t, ant_value_t call_this, bool replace_all) {
  regexp_fast_t direct;
  if (
    nargs > 1 && vtype(call_this) == kTypeString && vtype(args[1]) == kTypeString &&
    regexp_fast_get_pristine(js, args[0], &direct) && (!replace_all || (direct.flags & REGEXP_FLAG_GLOBAL))
  ) return regexp_replace_fast_string(js, &direct, call_this, args[1]);

  ant_value_t str = Ant_String_CoerceThis(js, call_this, replace_all ? "replaceAll" : "replace");
  
  if (is_err(str)) return str;
  if (nargs < 1) return str;

  if (is_object_type(args[0])) {
    regexp_fast_t fast;
    if (regexp_fast_get(js, args[0], REGEXP_FAST_PRISTINE, &fast)) {
      if (replace_all && !(fast.flags & REGEXP_FLAG_GLOBAL))
        return js_mkerr_typed(js, JS_ERR_TYPE, "String.prototype.replaceAll called with a non-global RegExp");
      return regexp_symbol_replace_impl(js, args[0], str, nargs > 1 ? args[1] : Ant_Value_Undefined, &fast);
    }
    if (replace_all) {
      ant_value_t global_val = regexp_read_flag(js, args[0], REGEXP_FLAG_INDEX_GLOBAL);
      if (!js_truthy(js, global_val)) return js_mkerr_typed(js, JS_ERR_TYPE, "String.prototype.replaceAll called with a non-global RegExp");
    }
    
    bool called = false;
    ant_value_t replacement_arg = nargs > 1 ? args[1] : Ant_Value_Undefined;
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
  ant_value_t replacement = nargs > 1 ? args[1] : Ant_Value_Undefined;

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

ant_value_t builtin_string_replace(ant_params_t) {
  return string_replace_impl(js, args, nargs, call_this, false);
}

ant_value_t builtin_string_replaceAll(ant_params_t) {
  return string_replace_impl(js, args, nargs, call_this, true);
}
