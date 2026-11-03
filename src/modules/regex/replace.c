#include "regex_internal.h"

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

const char *find_bytes(const char *haystack, ant_offset_t haystack_len, const char *needle, ant_offset_t needle_len) {
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
  ant_value_t error = Ant_Value_Undefined;

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

ant_value_t regexp_replace_fast_string(
  ant_t *js, const regexp_fast_t *fast, ant_value_t str, ant_value_t replace_str
) {
  ant_offset_t repl_len, repl_off = regexp_vstr(js, replace_str, &repl_len);
  const char *repl_ptr = (const char *)(uintptr_t)repl_off;
  bool literal = repl_len == 0 || !memchr(repl_ptr, '$', (size_t)repl_len);
  return literal && fast->compiled->plain_literal
    ? regexp_replace_fast_string_scan(js, fast, str, repl_ptr, repl_len, true, true)
    : regexp_replace_fast_string_scan(js, fast, str, repl_ptr, repl_len, literal, false);
}

ant_value_t regexp_replace_fast_function(
  ant_t *js, const regexp_fast_t *fast, ant_value_t str, ant_value_t replace_fn
) {
  regexp_fast_scan_t scan;
  regexp_fast_scan_begin(js, &scan, fast, str);

  utf16_pos_t pos;
  utf16_pos_init(&pos, scan.str_ptr, scan.str_len);
  regexp_ovector_list_t matches = {0};
  uint32_t stride = 0;
  ant_value_t error = Ant_Value_Undefined;
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
  ant_value_t result = Ant_Value_Undefined;

  for (size_t m = 0; m < scan.count; m++) {
    const PCRE2_SIZE *ovector = matches.items + m * stride;
    ant_offset_t str_len, str_off = regexp_vstr(js, str, &str_len);
    const char *str_ptr = (const char *)(uintptr_t)str_off;
    int ca = 0;
    for (uint32_t i = 0; i < ovcount; i++) {
      PCRE2_SIZE start = ovector[2 * i];
      ant_value_t capture = Ant_Value_Undefined;
      if (start != PCRE2_UNSET) {
        capture = js_mkstr_byte_range(js, str_ptr, start, ovector[2 * i + 1] - start);
        if (is_err(capture)) { result = capture; goto done; }
      }
      call_args[ca++] = capture;
    }
    call_args[ca++] = Ant_Value_FromNumber((double)ovector[2 * ovcount]);
    call_args[ca++] = str;
    if (has_groups) {
      ant_value_t groups = regexp_build_groups_object(js, compiled, str_ptr, ovector, ovcount);
      if (is_err(groups)) { result = groups; goto done; }
      call_args[ca++] = groups;
    }

    ant_value_t replacement = sv_vm_call(js->vm, js, replace_fn, Ant_Value_Undefined, call_args, ca, NULL, Ant_Value_Undefined);
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

ant_value_t regexp_symbol_replace_impl(
  ant_t *js, ant_value_t rx, ant_value_t str, ant_value_t replace_value, const regexp_fast_t *known
) {
  bool func_replace = (vtype(replace_value) == kTypeFunction || vtype(replace_value) == kTypeBuiltin);
  ant_value_t replace_str = Ant_Value_Undefined;
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
      js, js_get_native(rx, REGEXP_NATIVE_TAG), rx, Ant_Value_FromNumber(0)
    );
    if (is_err(stored)) return stored;
  }

  if (
    regexp_fast_get(js, rx, REGEXP_FAST_BUILTIN_EXEC, &fast) &&
    ((fast.flags & REGEXP_FLAG_GLOBAL) != 0) == global
  ) return regexp_replace_fast(js, &fast, str, func_replace ? replace_value : replace_str, func_replace);

  ant_value_t results = Ant_Array_Create(js);
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
      call_args[ca++] = Ant_Value_FromNumber(position_units);
      call_args[ca++] = str;
      if (has_named) call_args[ca++] = named_captures;
      replacement = sv_vm_call(js->vm, js, replace_value, Ant_Value_Undefined, call_args, ca, NULL, Ant_Value_Undefined);
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
        regexp_named_object_ctx_t named_ctx = { js, named_captures, Ant_Value_Undefined };
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

ant_value_t builtin_regexp_symbol_replace(ant_params_t) {
  if (!is_object_type(call_this))
    return js_mkerr_typed(js, JS_ERR_TYPE, "RegExp.prototype[@@replace] called on non-object");

  ant_value_t str = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
  if (is_err(str)) return str;
  return regexp_symbol_replace_impl(js, call_this, str, nargs > 1 ? args[1] : Ant_Value_Undefined, NULL);
}
