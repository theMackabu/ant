#include <compat.h> // IWYU pragma: keep
#include <pcre2.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "utf8.h"
#include "unicode.h"
#include "cfunc.h"
#include "builder.h"
#include "numbers.h"
#include "arrays.h"
#include "convert.h"
#include "props.h"
#include "proto.h"
#include "silver/call.h"

#include "modules/symbol.h"
#include "modules/function.h"
#include "modules/string.h"

static ant_value_t to_string_val(ant_t *js, ant_value_t val) {
  uint8_t t = vtype(val);
  if (t == kTypeString) return val;
  if (t == kTypeObject) {
    ant_value_t prim = js_get_slot(val, SLOT_PRIMITIVE);
    if (vtype(prim) == kTypeString) return prim;
  }
  return coerce_to_str_hint(js, val, 1);
}

ant_value_t Ant_String_CoerceThis(ant_t *js, ant_value_t call_this, const char *name) {
  uint8_t t = vtype(call_this);
  if (t == kTypeString) return call_this;
  if (t == kTypeUndefined || t == kTypeNull)
    return js_mkerr_typed(js, JS_ERR_TYPE, "String.prototype.%s called on null or undefined", name);
  return to_string_val(js, call_this);
}

static ant_value_t builtin_String(ant_params_t) {
  ant_value_t sval;
  
  if (nargs == 0) sval = js_mkstr(js, "", 0);
  else if (vtype(args[0]) == kTypeString) sval = args[0];
  
  else if (vtype(args[0]) == kTypeSymbol) {
    sval = js_symbol_to_string(js, args[0]);
    if (is_err(sval)) return sval;
  } else {
    sval = coerce_to_str(js, args[0]);
    if (is_err(sval)) return sval;
  }
  
  if (is_wrapper_ctor_target(js, call_this, call_new_target)) {
    js_set_slot_wb(js, call_this, SLOT_PRIMITIVE, sval);
    ant_value_t length = mkprop_exact_attrs(js, call_this, js->length_str, Ant_Value_FromNumber((double)str_utf16_len(js, sval)), 0);
    if (is_err(length)) return length;
  }
  
  return sval;
}

double string_number_to_integer(double number) {
  return isnan(number) || number == 0.0 ? 0.0 : trunc(number);
}

static ant_value_t string_to_number_arg(
  ant_t *js, ant_value_t value, double *out
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, value);

  if (is_object_type(value) || vtype(value) == kTypeBuiltin) {
    value = js_to_primitive(js, value, 2);
    if (is_err(value)) {
      GC_ROOT_RESTORE(js, root_mark);
      return value;
    }
  }

  if (vtype(value) == kTypeSymbol || vtype(value) == kTypeBigInt) {
    ant_value_t err = js_mkerr_typed(
      js, JS_ERR_TYPE, "Cannot convert a %s value to a number", 
      vtype(value) == kTypeSymbol ? "Symbol" : "BigInt");
    GC_ROOT_RESTORE(js, root_mark);
    return err;
  }

  *out = js_to_number(js, value);
  GC_ROOT_RESTORE(js, root_mark);
  
  return Ant_Value_Undefined;
}

ant_value_t string_to_integer_or_infinity(
  ant_t *js, ant_value_t value, double *out
) {
  ant_value_t status = string_to_number_arg(js, value, out);
  if (!is_err(status)) *out = string_number_to_integer(*out);
  return status;
}

static __attribute__((always_inline)) inline const char *string_bytes_find(
  const char *hay, size_t hay_len, const char *needle, size_t needle_len
) {
  if (needle_len > hay_len) return NULL;
  const char *p = hay, *last = hay + hay_len - needle_len;
  while (p <= last) {
    p = memchr(p, (unsigned char)needle[0], (size_t)(last - p) + 1);
    if (!p) return NULL;
    if (needle_len == 1 || memcmp(p + 1, needle + 1, needle_len - 1) == 0) return p;
    p++;
  }
  return NULL;
}

static __attribute__((always_inline)) inline const char *string_bytes_find_last(
  const char *hay, size_t hay_len, const char *needle, size_t needle_len, size_t max_start
) {
  if (needle_len > hay_len) return NULL;
  if (max_start > hay_len - needle_len) max_start = hay_len - needle_len;
  char first = needle[0];
  for (size_t i = max_start + 1; i > 0; i--) {
    if (hay[i - 1] == first && memcmp(hay + i - 1, needle, needle_len) == 0)
      return hay + i - 1;
  }
  return NULL;
}

static size_t string_split_view_byte(const char *bytes, size_t len, size_t units) {
  size_t byte = 0;
  for (size_t u = 0; u < units && byte < len; u++)
    byte += utf8_char_len_at(bytes, len, byte);
  return byte;
}

enum { STRING_FIND_NONE = -1, STRING_FIND_OOM = -2 };

static ptrdiff_t string_find_split_pairs(
  const char *hay, size_t hay_len, const char *needle, size_t needle_len,
  size_t position, bool backward
) {
  size_t view_len, view_needle_len;
  char *view = wtf8_split_pairs_dup(hay, hay_len, &view_len);
  char *view_needle = view ? wtf8_split_pairs_dup(needle, needle_len, &view_needle_len) : NULL;
  if (!view_needle) {
    free(view);
    return STRING_FIND_OOM;
  }

  size_t byte = string_split_view_byte(view, view_len, position);
  const char *match = backward
    ? string_bytes_find_last(view, view_len, view_needle, view_needle_len, byte)
    : string_bytes_find(view + byte, view_len - byte, view_needle, view_needle_len);

  ptrdiff_t result = match ? (ptrdiff_t)utf16_strlen(view, (size_t)(match - view)) : STRING_FIND_NONE;
  free(view);
  free(view_needle);
  return result;
}

static constexpr size_t STRING_FIND_DIRECT_UNITS_MAX = 256;

static __attribute__((noinline)) ptrdiff_t string_find_utf16_wide(
  const char *hay, size_t hay_len, const char *needle, size_t needle_len,
  size_t position, bool backward
) {
  if (wtf8_needle_splits_pairs(needle, needle_len) && wtf8_has_supplementary(hay, hay_len))
    return string_find_split_pairs(hay, hay_len, needle, needle_len, position, backward);

  ant_offset_t known_units = str_flat_cached_utf16_len(str_flat_from_bytes(hay));
  bool seek = backward
    ? position < hay_len && (known_units == STR_UTF16_LEN_UNKNOWN || position < (size_t)known_units)
    : position > 0;

  utf16_pos_t pos;
  size_t byte = backward ? hay_len : 0;
  if (seek) {
    utf16_pos_init(&pos, hay, hay_len);
    byte = utf16_pos_seek(&pos, position);
    if (!backward && pos.units < position && byte < hay_len) byte += utf8_char_len_at(hay, hay_len, byte);
  }

  const char *match = backward
    ? string_bytes_find_last(hay, hay_len, needle, needle_len, byte)
    : string_bytes_find(hay + byte, hay_len - byte, needle, needle_len);

  ptrdiff_t result = STRING_FIND_NONE;
  if (match) {
    size_t offset = (size_t)(match - hay);
    if (seek) result = (ptrdiff_t)utf16_pos_units(&pos, offset);
    else if (offset <= STRING_FIND_DIRECT_UNITS_MAX) result = (ptrdiff_t)utf16_strlen(hay, offset);
    else result = (ptrdiff_t)byte_offset_to_utf16(hay, offset);
  }
  if (seek) utf16_pos_save(&pos);
  return result;
}

static __attribute__((always_inline)) inline ptrdiff_t string_find_utf16(
  const char *hay, size_t hay_len, const char *needle, size_t needle_len,
  size_t position, bool backward
) {
  if (!str_is_ascii(hay))
    return string_find_utf16_wide(hay, hay_len, needle, needle_len, position, backward);
  if (needle_len > hay_len || !str_is_ascii(needle)) return STRING_FIND_NONE;
  if (!backward && position > hay_len) return STRING_FIND_NONE;
  const char *match = backward
    ? string_bytes_find_last(hay, hay_len, needle, needle_len, position)
    : string_bytes_find(hay + position, hay_len - position, needle, needle_len);
  return match ? match - hay : STRING_FIND_NONE;
}

static ant_value_t string_index_of_strings(
  ant_t *js, ant_value_t str, ant_value_t search, double position
) {
  ant_offset_t str_len, search_len;
  ant_offset_t search_off = vstr(js, search, &search_len);
  ant_offset_t str_off = vstr(js, str, &str_len);

  size_t start = 0;
  if (position > 0.0) {
    size_t str_utf16 = (size_t)str_utf16_len(js, str);
    start = position >= (double)str_utf16 ? str_utf16 : (size_t)position;
  }
  if (search_len == 0) return Ant_Value_FromNumber((double)start);

  ptrdiff_t found = string_find_utf16(
    (const char *)(uintptr_t)str_off, (size_t)str_len,
    (const char *)(uintptr_t)search_off, (size_t)search_len, start, false);
  if (found == STRING_FIND_OOM) return js_mkerr(js, "oom");
  return Ant_Value_FromNumber((double)found);
}

static ant_value_t string_index_of_impl(ant_t *js, ant_value_t this_val, ant_args_t) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t receiver = this_val;
  ant_value_t search_arg = nargs > 0 ? args[0] : Ant_Value_Undefined;
  ant_value_t position_arg = nargs > 1 ? args[1] : Ant_Value_Undefined;
  
  GC_ROOT_PIN(js, receiver);
  GC_ROOT_PIN(js, search_arg);
  GC_ROOT_PIN(js, position_arg);

  ant_value_t result;
  uint8_t receiver_type = vtype(receiver);
  if (receiver_type == kTypeNull || receiver_type == kTypeUndefined) {
    result = js_mkerr_typed(
      js, JS_ERR_TYPE, "String.prototype.indexOf called on null or undefined");
    goto done;
  }

  ant_value_t str = coerce_to_str_hint(js, receiver, 1);
  GC_ROOT_PIN(js, str);
  if (is_err(str)) { result = str; goto done; }

  ant_value_t search = coerce_to_str_hint(js, search_arg, 1);
  GC_ROOT_PIN(js, search);
  if (is_err(search)) { result = search; goto done; }

  double position = 0.0;
  if (nargs > 1) {
    ant_value_t status = string_to_integer_or_infinity(
      js, position_arg, &position);
    if (is_err(status)) { result = status; goto done; }
  }

  result = string_index_of_strings(js, str, search, position);

done:
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static ant_value_t js_mkstr_utf16_range(
  ant_t *js, const char *str, size_t byte_len,
  size_t utf16_start, size_t utf16_end
) {
  size_t byte_start, byte_end;
  utf16_range_splits_t splits = utf16_range_to_byte_range(
    str, byte_len, utf16_start, utf16_end, &byte_start, &byte_end
  );

  if (!splits.prefix_surrogate && !splits.suffix_surrogate)
    return js_mkstr_byte_range(js, str, byte_start, byte_end - byte_start);

  string_builder_t sb;
  char static_buf[64]; char encoded[4];
  string_builder_init(&sb, static_buf, sizeof(static_buf));

  if (splits.prefix_surrogate) {
    size_t len = utf16_code_unit_to_wtf8(splits.prefix_surrogate, encoded);
    if (!string_builder_append(&sb, encoded, len)) goto oom;
  }

  if (!string_builder_append(
    &sb, str + byte_start, byte_end - byte_start
  )) goto oom;

  if (splits.suffix_surrogate) {
    size_t len = utf16_code_unit_to_wtf8(splits.suffix_surrogate, encoded);
    if (!string_builder_append(&sb, encoded, len)) goto oom;
  }

  return string_builder_finalize(js, &sb);

oom:
  string_builder_dispose(&sb);
  return js_mkerr(js, "oom");
}

static inline bool utf16_range_equals_bytes(
  const char *str, size_t byte_len,
  size_t utf16_start, size_t utf16_end,
  const char *expected, size_t expected_len
) {
  size_t byte_start, byte_end;
  utf16_range_splits_t splits = utf16_range_to_byte_range(
    str, byte_len, utf16_start, utf16_end, &byte_start, &byte_end
  );

  size_t body_len = byte_end - byte_start;
  if (!splits.prefix_surrogate && !splits.suffix_surrogate)
    return body_len == expected_len && memcmp(str + byte_start, expected, expected_len) == 0;

  size_t prefix_len = splits.prefix_surrogate ? 3 : 0;
  size_t suffix_len = splits.suffix_surrogate ? 3 : 0;
  if (prefix_len + body_len + suffix_len != expected_len) return false;

  char encoded[4];
  size_t pos = 0;

  if (splits.prefix_surrogate) {
    utf16_code_unit_to_wtf8(splits.prefix_surrogate, encoded);
    if (memcmp(expected, encoded, 3) != 0) return false;
    pos += 3;
  }

  if (body_len > 0 && memcmp(expected + pos, str + byte_start, body_len) != 0) return false;
  pos += body_len;

  if (splits.suffix_surrogate) {
    utf16_code_unit_to_wtf8(splits.suffix_surrogate, encoded);
    if (memcmp(expected + pos, encoded, 3) != 0) return false;
  }

  return true;
}

static ant_value_t js_mkstr_utf16_range_cached(
  ant_t *js, const char *str, size_t byte_len,
  size_t utf16_start, size_t utf16_end
) {
  if (utf16_end == utf16_start + 1) {
    size_t byte_start, byte_end;
    utf16_range_splits_t splits = utf16_range_to_byte_range(
      str, byte_len, utf16_start, utf16_end, &byte_start, &byte_end);
    if (!splits.prefix_surrogate && !splits.suffix_surrogate &&
        byte_end == byte_start + 1 && (unsigned char)str[byte_start] < 128)
      return js_mk_ascii_char_cached(js, (unsigned char)str[byte_start]);
  }

  return js_mkstr_utf16_range(
    js, str, byte_len, utf16_start, utf16_end);
}

static size_t string_substring_index(double value, size_t length) {
  if (value <= 0.0) return 0;
  if (value >= (double)length) return length;
  return (size_t)value;
}

static ant_value_t string_substring_strings(
  ant_t *js, ant_value_t str,
  double start_number, double end_number, bool default_end
) {
  size_t utf16_len = (size_t)str_utf16_len(js, str);
  if (default_end) end_number = (double)utf16_len;

  size_t start = string_substring_index(start_number, utf16_len);
  size_t end = string_substring_index(end_number, utf16_len);
  
  if (start > end) {
    size_t tmp = start;
    start = end;
    end = tmp;
  }

  if (start == 0 && end == utf16_len) return str;

  ant_offset_t byte_len, str_off = vstr(js, str, &byte_len);
  const char *str_ptr = (const char *)(uintptr_t)str_off;
  return js_mkstr_utf16_range_cached(js, str_ptr, (size_t)byte_len, start, end);
}

static ant_value_t string_substring_impl(ant_t *js, ant_value_t this_val, ant_args_t) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t receiver = this_val;
  ant_value_t start_arg = nargs > 0 ? args[0] : Ant_Value_Undefined;
  ant_value_t end_arg = nargs > 1 ? args[1] : Ant_Value_Undefined;
  
  GC_ROOT_PIN(js, receiver);
  GC_ROOT_PIN(js, start_arg);
  GC_ROOT_PIN(js, end_arg);

  ant_value_t result;
  uint8_t receiver_type = vtype(receiver);
  if (receiver_type == kTypeNull || receiver_type == kTypeUndefined) {
    result = js_mkerr_typed(
      js, JS_ERR_TYPE, "String.prototype.substring called on null or undefined");
    goto done;
  }

  ant_value_t str = coerce_to_str_hint(js, receiver, 1);
  GC_ROOT_PIN(js, str);
  if (is_err(str)) { result = str; goto done; }

  double start_number = 0.0;
  ant_value_t status = string_to_integer_or_infinity(
    js, start_arg, &start_number);
  if (is_err(status)) { result = status; goto done; }

  bool default_end = nargs <= 1 || vtype(end_arg) == kTypeUndefined;
  double end_number = 0.0;
  if (!default_end) {
    status = string_to_integer_or_infinity(js, end_arg, &end_number);
    if (is_err(status)) { result = status; goto done; }
  }

  result = string_substring_strings(
    js, str, start_number, end_number, default_end);

done:
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static bool string_fast_integer_arg(ant_args_t, int index, double *out) {
  if (index >= nargs || vtype(args[index]) == kTypeUndefined) {
    *out = 0.0;
    return true;
  }
  if (vtype(args[index]) != kTypeNumber) return false;
  *out = string_number_to_integer(Ant_Value_AsNumberUnchecked(args[index]));
  return true;
}

ant_value_t js_string_intrinsic_call(
  ant_t *js, ant_string_intrinsic_kind_t kind,
  ant_value_t this_val, ant_args_t
) {
  switch (kind) {
  case ANT_STRING_INTRINSIC_INDEX_OF: {
    double position;
    if (nargs > 0 && ant_str_flat_ptr(this_val) &&
        ant_str_flat_ptr(args[0]) &&
        string_fast_integer_arg(args, nargs, 1, &position))
      return string_index_of_strings(js, this_val, args[0], position);
    return string_index_of_impl(js, this_val, args, nargs);
  }
  case ANT_STRING_INTRINSIC_SUBSTRING: {
    double start_number, end_number = 0.0;
    bool default_end = nargs <= 1 || vtype(args[1]) == kTypeUndefined;
    if (ant_str_flat_ptr(this_val) &&
        string_fast_integer_arg(args, nargs, 0, &start_number) &&
        (default_end ||
         string_fast_integer_arg(args, nargs, 1, &end_number))) {
      GC_ROOT_SAVE(root_mark, js);
      GC_ROOT_PIN(js, this_val);
      ant_value_t result = string_substring_strings(
        js, this_val, start_number, end_number, default_end);
      GC_ROOT_RESTORE(js, root_mark);
      return result;
    }
    return string_substring_impl(js, this_val, args, nargs);
  }}
  return js_mkerr(js, "invalid string intrinsic");
}

static ant_value_t builtin_string_indexOf(ant_params_t) {
  return js_string_intrinsic_call(js, ANT_STRING_INTRINSIC_INDEX_OF, call_this, args, nargs);
}

static ant_value_t builtin_string_substring(ant_params_t) {
  return js_string_intrinsic_call(js, ANT_STRING_INTRINSIC_SUBSTRING, call_this, args, nargs);
}

bool js_string_intrinsic_builtin_matches(ant_value_t func, ant_string_intrinsic_kind_t kind) {
  if (vtype(func) != kTypeBuiltin) return false;
  switch (kind) {
    case ANT_STRING_INTRINSIC_INDEX_OF: return js_cfunc_same_entrypoint(func, builtin_string_indexOf);
    case ANT_STRING_INTRINSIC_SUBSTRING: return js_cfunc_same_entrypoint(func, builtin_string_substring);
  }
  return false;
}

static ant_value_t builtin_string_substr(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "substr");
  if (is_err(str)) return str;
  ant_offset_t byte_len, str_off = vstr(js, str, &byte_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  size_t utf16_len = (size_t)str_utf16_len(js, str);
  
  if (nargs < 1) return str;
  
  double d_start = js_to_number(js, args[0]);
  if (isnan(d_start)) d_start = 0;
  else d_start = trunc(d_start);

  ant_offset_t start;
  if (d_start <= -(double)utf16_len) {
    start = 0;
  } else if (d_start < 0) {
    start = (ant_offset_t)((double)utf16_len + d_start);
  } else if (d_start >= (double)utf16_len) {
    start = (ant_offset_t)utf16_len;
  } else {
    start = (ant_offset_t)d_start;
  }
  
  ant_offset_t len = (ant_offset_t)utf16_len - start;
  if (nargs >= 2 && vtype(args[1]) != kTypeUndefined) {
    double d = js_to_number(js, args[1]);
    if (isnan(d) || d <= 0) len = 0;
    else if (d < (double)len) len = (ant_offset_t)d;
  }
  
  return js_mkstr_utf16_range(js, str_ptr, byte_len, start, start + len);
}

static ant_value_t string_split_code_units(ant_t *js, ant_value_t str, ant_value_t arr, uint32_t limit) {
  GC_ROOT_SAVE(roots, js);
  GC_ROOT_PIN(js, str);
  GC_ROOT_PIN(js, arr);
  
  ant_value_t part = Ant_Value_Undefined;
  GC_ROOT_PIN(js, part);

  ant_offset_t len;
  const char *bytes = (const char *)(uintptr_t)vstr(js, str, &len);
  utf16_iterator_t iterator = utf16_iterator(bytes, len);
  bool ascii = str_is_ascii(bytes);
  
  uint16_t unit;
  for (uint32_t index = 0; index < limit && utf16_next_code_unit(&iterator, &unit); index++) {
    part = ascii 
      ? js_mk_ascii_char_cached(js, (unsigned char)bytes[index])
      : js_string_from_utf16_code_unit(js, unit);
    if (is_err(part)) break;
    arr_set(js, arr, index, part);
  }

  ant_value_t result = is_err(part) ? part : arr;
  GC_ROOT_RESTORE(js, roots);
  
  return result;
}

static ant_value_t string_split_ascii_byte(ant_t *js, const char *str_ptr, size_t str_len, char sep) {
  uint32_t count = 1;
  for (const char *at = str_ptr, *end = str_ptr + str_len; (at = memchr(at, sep, (size_t)(end - at))); at++) count++;
  
  ant_value_t arr = alloc_array_with_proto_capacity(js, js->sym.array_proto, count, 0, true);
  if (is_err(arr)) return arr;
  
  ant_object_t *obj = array_obj_ptr(arr);
  if (!obj || !obj->u.array.data || obj->u.array.cap < count) return js_mkerr(js, "oom");
  obj->flags.may_have_dense_elements = 1;
  
  size_t start = 0;
  for (uint32_t i = 0; i < count; i++) {
    const char *found = i + 1 < count ? memchr(str_ptr + start, sep, str_len - start) : NULL;
    size_t end = found ? (size_t)(found - str_ptr) : str_len;
    size_t len = end - start;
    
    ant_value_t part = len == 0 ? js_empty_string(js)
      : len == 1 && (unsigned char)str_ptr[start] < 0x80 ? js_mk_ascii_char_cached(js, (unsigned char)str_ptr[start])
      : js_mkstr_byte_range(js, str_ptr, start, len);
    if (is_err(part)) return part;
    
    obj->u.array.data[i] = part;
    obj->u.array.len = i + 1;
    gc_write_barrier_elem(js, obj, i, part);
    start = end + 1;
  }
  
  return arr;
}

static ant_value_t string_split_impl(ant_t *js, ant_value_t str, ant_args_t) {
  if (vtype(str) != kTypeString) return js_mkerr(js, "split called on non-string");
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  
  if (nargs >= 1 && vtype(args[0]) == kTypeString && (nargs < 2 || vtype(args[1]) == kTypeUndefined)) {
    ant_offset_t sep_len, sep_off = vstr(js, args[0], &sep_len);
    char sep = sep_len == 1 ? *(const char *)(uintptr_t)sep_off : 0;
    if (sep_len == 1 && (unsigned char)sep < 0x80) return string_split_ascii_byte(js, str_ptr, (size_t)str_len, sep);
  }
  
  ant_value_t arr = Ant_Array_Create(js);
  if (is_err(arr)) return arr;

  uint32_t limit;
  ant_value_t limit_status = regexp_split_limit(js, nargs >= 2 ? args[1] : Ant_Value_Undefined, &limit);
  if (is_err(limit_status)) return limit_status;
  
  if (limit == 0) return mkval(kTypeArray, vdata(arr));
  if (nargs == 0) goto return_whole;

  ant_value_t sep_arg = args[0];
  if (vtype(sep_arg) == kTypeObject) {
    ant_prop_loc_t source_off = lkp(js, sep_arg, "source", 6);
    if (!source_off.obj) {
      sep_arg = js_to_primitive(js, sep_arg, 1);
      if (is_err(sep_arg)) return sep_arg;
      
      sep_arg = js_tostring_val(js, sep_arg);
      if (is_err(sep_arg)) return sep_arg;
      
      args[0] = sep_arg;
      goto split_string_separator;
    }
    
    ant_value_t source_val = js_prop_load(source_off);
    if (vtype(source_val) != kTypeString) goto return_whole;

    ant_offset_t plen, poff = vstr(js, source_val, &plen);
    const char *pattern_ptr = (char *)(uintptr_t)(poff);

    if (plen == 0 || (plen == 4 && memcmp(pattern_ptr, "(?:)", 4) == 0)) {
      ant_offset_t idx = 0;
      for (ant_offset_t i = 0; i < str_len && idx < limit; i++) {
        ant_value_t part = js_mkstr_byte_range(js, str_ptr, i, 1);
        arr_set(js, arr, idx, part);
        idx++;
      }
      return mkval(kTypeArray, vdata(arr));
    }

    char pcre2_pattern_stack[512];
    js_cstr_t pcre2_pattern = js_to_pcre2_pattern_cstr(
      pattern_ptr, plen, pcre2_pattern_stack,
      sizeof(pcre2_pattern_stack), 0
    );
    
    if (!pcre2_pattern.ptr) goto return_whole;
    uint32_t options = PCRE2_UTF | PCRE2_MATCH_UNSET_BACKREF | PCRE2_DOLLAR_ENDONLY;
    int errcode;
    
    PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile(
      (PCRE2_SPTR)pcre2_pattern.ptr, pcre2_pattern.len,
      options, &errcode, &erroffset, NULL
    );

    if (pcre2_pattern.needs_free) free((void *)pcre2_pattern.ptr);
    if (re == NULL) goto return_whole;

    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
    uint32_t capture_count;
    pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &capture_count);

    if (str_len == 0) {
      int rc = pcre2_match(re, (PCRE2_SPTR)str_ptr, 0, 0, 0, match_data, NULL);
      if (rc >= 0) {
        pcre2_match_data_free(match_data);
        pcre2_code_free(re);
        return mkval(kTypeArray, vdata(arr));
      }
    }

    ant_offset_t idx = 0;
    PCRE2_SIZE search_pos = 0;
    PCRE2_SIZE segment_start = 0;
    PCRE2_SIZE last_match_end = (PCRE2_SIZE)-1;
    bool had_any_split = false;
    
    while (idx < limit && search_pos <= str_len) {
      int rc = pcre2_match(re, (PCRE2_SPTR)str_ptr, str_len, search_pos, 0, match_data, NULL);
      if (rc < 0) break;

      PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
      PCRE2_SIZE match_start = ovector[0];
      PCRE2_SIZE match_end = ovector[1];

      if (match_start == match_end && match_start == last_match_end) {
        search_pos = match_end + 1;
        continue;
      }

      if (match_start == match_end && capture_count > 0) {
        bool is_pure_empty_capture = true;
        for (uint32_t i = 1; i <= capture_count; i++) {
          PCRE2_SIZE cap_start = ovector[2*i];
          PCRE2_SIZE cap_end = ovector[2*i+1];
          if (cap_start == PCRE2_UNSET || cap_end != cap_start) {
            is_pure_empty_capture = false;
            break;
          }
        }
        if (is_pure_empty_capture) {
          search_pos = match_end + 1;
          continue;
        }
      }
      
      had_any_split = true;

      ant_value_t part = js_mkstr_byte_range(js, str_ptr, segment_start, match_start - segment_start);
      arr_set(js, arr, idx, part);
      idx++;

      for (uint32_t i = 1; i <= capture_count && idx < limit; i++) {
        PCRE2_SIZE cap_start = ovector[2*i];
        PCRE2_SIZE cap_end = ovector[2*i+1];
        if (cap_start == PCRE2_UNSET) {
          arr_set(js, arr, idx, Ant_Value_Undefined);
        } else {
          part = js_mkstr_byte_range(js, str_ptr, cap_start, cap_end - cap_start);
          arr_set(js, arr, idx, part);
        }
        idx++;
      }

      last_match_end = match_end;
      segment_start = match_end;
      if (match_start == match_end) {
        search_pos = match_end + 1;
      } else {
        search_pos = match_end;
      }
    }

    if (!had_any_split) {
      pcre2_match_data_free(match_data);
      pcre2_code_free(re);
      arr_set(js, arr, 0, js_mkstr_byte_range(js, str_ptr, 0, str_len));
      return mkval(kTypeArray, vdata(arr));
    }

    if (idx < limit) {
      ant_value_t part = js_mkstr_byte_range(js, str_ptr, segment_start, str_len - segment_start);
      arr_set(js, arr, idx, part);
      idx++;
    }

    pcre2_match_data_free(match_data);
    pcre2_code_free(re);
    return mkval(kTypeArray, vdata(arr));
  }

  if (vtype(sep_arg) != kTypeString) goto return_whole;

split_string_separator:;
  ant_offset_t sep_len, sep_off = vstr(js, sep_arg, &sep_len);
  const char *sep_ptr = (char *)(uintptr_t)(sep_off);
  ant_offset_t idx = 0, start = 0;

  if (sep_len == 0)
    return string_split_code_units(js, str, arr, limit);

  char *view = NULL, *view_sep = NULL;
  if (
    !str_is_ascii(str_ptr) && wtf8_needle_splits_pairs(sep_ptr, (size_t)sep_len) &&
    wtf8_has_supplementary(str_ptr, (size_t)str_len)
  ) {
    size_t view_len, view_sep_len;
    view = wtf8_split_pairs_dup(str_ptr, (size_t)str_len, &view_len);
    view_sep = view ? wtf8_split_pairs_dup(sep_ptr, (size_t)sep_len, &view_sep_len) : NULL;
    if (!view_sep) {
      free(view);
      return js_mkerr(js, "oom");
    }
    str_ptr = view;
    str_len = (ant_offset_t)view_len;
    sep_ptr = view_sep;
    sep_len = (ant_offset_t)view_sep_len;
  }

  for (bool last = false; idx < limit && !last; idx++) {
    const char *match = string_bytes_find(
      str_ptr + start, (size_t)(str_len - start), sep_ptr, (size_t)sep_len);
    last = match == NULL;
    ant_offset_t end = last ? str_len : (ant_offset_t)(match - str_ptr);
    ant_value_t part = view
      ? js_mkstr(js, str_ptr + start, (size_t)(end - start))
      : js_mkstr_byte_range(js, str_ptr, (size_t)start, (size_t)(end - start));
    if (is_err(part)) {
      free(view);
      free(view_sep);
      return part;
    }
    arr_set(js, arr, idx, part);
    start = end + sep_len;
  }

  free(view);
  free(view_sep);
  return mkval(kTypeArray, vdata(arr));

return_whole:
  if (limit > 0) {
    arr_set(js, arr, 0, str);
  }
  return mkval(kTypeArray, vdata(arr));
}

static ant_value_t builtin_string_split(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "split");
  if (is_err(str)) return str;

  if (nargs > 0 && is_object_type(args[0])) {
    ant_value_t fast_result;
    if (regexp_split_try_fast(js, args[0], str, nargs >= 2 ? args[1] : Ant_Value_Undefined, &fast_result)) return fast_result;
    bool called = false;
    ant_value_t call_args[2];
    int call_nargs = 1;
    call_args[0] = str;
    if (nargs >= 2) {
      call_args[1] = args[1];
      call_nargs = 2;
    }
    ant_value_t dispatched = maybe_call_symbol_method(
      js, args[0], js->sym.split_sym, args[0], call_args, call_nargs, &called
    );
    if (is_err(dispatched)) return dispatched;
    if (called) return dispatched;
  }

  return string_split_impl(js, str, args, nargs);
}

static ant_value_t builtin_string_slice(ant_params_t) {
  ant_value_t receiver = call_this;
  if (str_is_heap_rope(receiver)) receiver = rope_cached_flat(receiver);
  ant_flat_string_t *flat = ant_str_flat_ptr(receiver);
  
  bool numeric_start = nargs == 0 || vtype(args[0]) == kTypeUndefined ||
    (vtype(args[0]) == kTypeNumber && Ant_Value_AsNumberUnchecked(args[0]) >= 0);
    
  bool numeric_end = nargs < 2 || vtype(args[1]) == kTypeUndefined ||
    (vtype(args[1]) == kTypeNumber && Ant_Value_AsNumberUnchecked(args[1]) >= 0);
    
  if (flat && str_flat_ascii_state(flat) == STR_ASCII_YES && numeric_start && numeric_end) {
    size_t len = (size_t)flat->len;
    double start_arg = nargs && vtype(args[0]) == kTypeNumber ? Ant_Value_AsNumberUnchecked(args[0]) : 0;
    double end_arg = nargs >= 2 && vtype(args[1]) == kTypeNumber ? Ant_Value_AsNumberUnchecked(args[1]) : (double)len;
    
    size_t start = start_arg >= (double)len ? len : (size_t)start_arg;
    size_t end = end_arg >= (double)len ? len : (size_t)end_arg;
    
    if (start == 0 && end == len) return receiver;
    size_t result_len = end > start ? end - start : 0;

    if (result_len == 1) return js_mk_ascii_char_cached(js, (unsigned char)flat->bytes[start]);
    GC_ROOT_SAVE(mark, js);
    GC_ROOT_PIN(js, receiver);
    
    ant_value_t result = js_mkstr(js, NULL, result_len);
    if (!is_err(result)) {
      ant_flat_string_t *out = ant_str_flat_ptr(result);
      memcpy(out->bytes, flat->bytes + start, result_len);
      str_flat_init_meta(out, STR_ASCII_YES);
    } GC_ROOT_RESTORE(js, mark);
    
    return result;
  }

  ant_value_t str = Ant_String_CoerceThis(js, call_this, "slice");
  if (is_err(str)) return str;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, str);
  size_t utf16_len = (size_t)str_utf16_len(js, str);
  ant_offset_t start = 0, end = (ant_offset_t)utf16_len;
  double dstr_len = (double)utf16_len;

  if (nargs >= 1) {
    double d;
    ant_value_t status = string_to_integer_or_infinity(js, args[0], &d);
    if (is_err(status)) { GC_ROOT_RESTORE(js, root_mark); return status; }
    if (d < 0) start = (ant_offset_t) (d + dstr_len < 0 ? 0 : d + dstr_len);
    else start = (ant_offset_t) (d > dstr_len ? dstr_len : d);
  }

  if (nargs >= 2 && vtype(args[1]) != kTypeUndefined) {
    double d;
    ant_value_t status = string_to_integer_or_infinity(js, args[1], &d);
    if (is_err(status)) { GC_ROOT_RESTORE(js, root_mark); return status; }
    if (d < 0) end = (ant_offset_t) (d + dstr_len < 0 ? 0 : d + dstr_len);
    else end = (ant_offset_t) (d > dstr_len ? dstr_len : d);
  }

  if (start > end) start = end;
  ant_offset_t byte_len, str_off = vstr(js, str, &byte_len);
  ant_value_t result = js_mkstr_utf16_range(js, (char *)(uintptr_t)str_off, byte_len, start, end);
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

ant_value_t js_string_slice(ant_t *js, ant_value_t str, double start, double end) {
  ant_value_t args[2] = { Ant_Value_FromNumber(start), Ant_Value_FromNumber(end) };
  return sv_call_native_this(js, builtin_string_slice, str, args, 2);
}

static ant_value_t builtin_string_includes(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "includes");
  if (vtype(str) != kTypeString) return str;
  ant_value_t search = nargs > 0 ? args[0] : Ant_Value_Undefined;
  
  if (vtype(str) != kTypeString) 
    return js_mkerr(js, "includes called on non-string");

  if (nargs > 0 && is_object_type(args[0])) {
    ant_value_t maybe_err = reject_regexp_arg(js, args[0], "includes");
    if (is_err(maybe_err)) return maybe_err;
  }

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, str);
  ant_value_t result;

  search = js_tostring_val(js, search);
  GC_ROOT_PIN(js, search);
  if (is_err(search)) { result = search; goto done; }

  double position = 0.0;
  if (nargs >= 2) {
    ant_value_t status = string_to_integer_or_infinity(js, args[1], &position);
    if (is_err(status)) { result = status; goto done; }
  }

  ant_offset_t str_len, search_len;
  ant_offset_t search_off = vstr(js, search, &search_len);
  ant_offset_t str_off = vstr(js, str, &str_len);
  if (search_len == 0) { result = Ant_Value_True; goto done; }

  size_t start = 0;
  if (position > 0.0) {
    size_t str_units = (size_t)str_utf16_len(js, str);
    start = position >= (double)str_units ? str_units : (size_t)position;
  }

  ptrdiff_t found = string_find_utf16(
    (const char *)(uintptr_t)str_off, (size_t)str_len,
    (const char *)(uintptr_t)search_off, (size_t)search_len, start, false);
  result = found == STRING_FIND_OOM ? js_mkerr(js, "oom") : Ant_Value_FromBool(found >= 0);

done:
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static inline ant_offset_t string_clamped_position(ant_t *js, ant_value_t value, ant_offset_t len) {
  double pos = js_to_number(js, value);
  if (isnan(pos) || pos < 0) return 0;
  if (pos > (double)len) return len;
  return (ant_offset_t)pos;
}

static ant_value_t builtin_string_startsWith(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "startsWith");
  if (is_err(str)) return str;
  if (nargs == 0) return mkval(kTypeBool, 0);
  ant_value_t search = args[0];
  
  if (is_object_type(search)) {
    ant_value_t maybe_err = reject_regexp_arg(js, search, "startsWith");
    if (is_err(maybe_err)) return maybe_err;
  }
  
  search = js_tostring_val(js, search);
  if (is_err(search)) return search;
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  ant_offset_t search_len, search_off = vstr(js, search, &search_len);
  
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  const char *search_ptr = (char *)(uintptr_t)(search_off);

  if (str_is_ascii(str_ptr)) {
    ant_offset_t start = nargs >= 2 ? string_clamped_position(js, args[1], str_len) : 0;
    if (search_len > str_len - start) return mkval(kTypeBool, 0);
    if (search_len == 0) return mkval(kTypeBool, 1);
    return mkval(kTypeBool, memcmp(str_ptr + start, search_ptr, search_len) == 0 ? 1 : 0);
  }

  ant_offset_t str_units = str_utf16_len(js, str);
  ant_offset_t search_units = str_utf16_len(js, search);
  ant_offset_t start = nargs >= 2 ? string_clamped_position(js, args[1], str_units) : 0;
  
  if (search_units > str_units - start) return mkval(kTypeBool, 0);
  if (search_len == 0) return mkval(kTypeBool, 1);

  return mkval(kTypeBool, utf16_range_equals_bytes(
    str_ptr, str_len, start, start + search_units,
    search_ptr, search_len
  ) ? 1 : 0);
}

static ant_value_t builtin_string_endsWith(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "endsWith");
  if (is_err(str)) return str;
  if (nargs == 0) return mkval(kTypeBool, 0);
  ant_value_t search = args[0];
  
  if (is_object_type(search)) {
    ant_value_t maybe_err = reject_regexp_arg(js, search, "endsWith");
    if (is_err(maybe_err)) return maybe_err;
  }
  
  search = js_tostring_val(js, search);
  if (is_err(search)) return search;
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  ant_offset_t search_len, search_off = vstr(js, search, &search_len);
  
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  const char *search_ptr = (char *)(uintptr_t)(search_off);

  if (str_is_ascii(str_ptr)) {
    ant_offset_t end = (nargs >= 2 && vtype(args[1]) != kTypeUndefined)
      ? string_clamped_position(js, args[1], str_len)
      : str_len;
    ant_offset_t start = end - search_len;
    if (start < 0) return mkval(kTypeBool, 0);
    if (search_len == 0) return mkval(kTypeBool, 1);
    return mkval(kTypeBool, memcmp(str_ptr + start, search_ptr, search_len) == 0 ? 1 : 0);
  }

  ant_offset_t str_units = str_utf16_len(js, str);
  ant_offset_t search_units = str_utf16_len(js, search);
  
  ant_offset_t end = (nargs >= 2 && vtype(args[1]) != kTypeUndefined)
    ? string_clamped_position(js, args[1], str_units)
    : str_units;
  
  ant_offset_t start = end - search_units;
  if (start < 0) return mkval(kTypeBool, 0);
  if (search_len == 0) return mkval(kTypeBool, 1);

  return mkval(kTypeBool, utf16_range_equals_bytes(
    str_ptr, str_len, start, start + search_units,
    search_ptr, search_len
  ) ? 1 : 0);
}

static ant_value_t builtin_string_template(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "template");
  if (vtype(str) != kTypeString) return str;
  if (nargs < 1 || vtype(args[0]) != kTypeObject) return str;
  
  ant_value_t data = args[0];
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  
  string_builder_t sb;
  char static_buf[512];
  string_builder_init(&sb, static_buf, sizeof(static_buf));

  ant_offset_t i = 0;
  ant_offset_t literal_start = 0;
  
  while (i < str_len) {
    if (i >= str_len - 3) goto next_char;
    if (str_ptr[i] != '{' || str_ptr[i + 1] != '{') goto next_char;
    
    ant_offset_t key_start = i + 2;
    ant_offset_t key_end = key_start;
    
    while (
      key_end < str_len - 1 && 
      !(str_ptr[key_end] == '}' && 
      str_ptr[key_end + 1] == '}')
    ) key_end++;
    
    if (key_end >= str_len - 1) goto next_char;
    if (!string_builder_append(&sb, str_ptr + literal_start, (size_t)(i - literal_start))) goto oom;
    
    ant_prop_loc_t prop_off = lkp(js, data, str_ptr + key_start, key_end - key_start);
    if (prop_off.obj) {
      ant_value_t err = Ant_Value_Undefined;
      ant_value_t value = js_prop_load(prop_off);
      if (!string_template_append_value(js, &sb, value, &err)) {
        string_builder_dispose(&sb);
        return is_err(err) ? err : js_mkerr(js, "oom");
      }
    }
    
    i = key_end + 2;
    literal_start = i;
    continue;

  next_char:
    i++;
  }

  if (!string_builder_append(&sb, str_ptr + literal_start, (size_t)(str_len - literal_start))) goto oom;
  return string_builder_finalize(js, &sb);

oom:
  string_builder_dispose(&sb);
  return js_mkerr(js, "oom");
}

static size_t html_attr_escaped_len(const char *s, ant_offset_t len) {
  size_t out = 0;
  for (ant_offset_t i = 0; i < len; i++) out += (s[i] == '"') ? 6 : 1;
  return out;
}

static void html_attr_append_escaped(char *out, size_t *pos, const char *s, ant_offset_t len) {
  for (ant_offset_t i = 0; i < len; i++) if (s[i] == '"') {
    memcpy(out + *pos, "&quot;", 6);
    *pos += 6;
  } else out[(*pos)++] = s[i];
}

static ant_value_t builtin_string_html(ant_native_params_t, ant_value_t call_this, const char *name, const char *tag, const char *attr) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, name);
  if (is_err(str)) return str;

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (const char *)(uintptr_t)str_off;
  
  size_t tag_len = strlen(tag);
  size_t attr_len = attr ? strlen(attr) : 0;
  
  ant_value_t attr_val = Ant_Value_Undefined;
  ant_offset_t value_len = 0;
  
  const char *value_ptr = NULL;
  size_t escaped_len = 0;

  if (attr) {
    attr_val = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
    if (is_err(attr_val)) return attr_val;
    ant_offset_t value_off = vstr(js, attr_val, &value_len);
    value_ptr = (const char *)(uintptr_t)value_off;
    escaped_len = html_attr_escaped_len(value_ptr, value_len);
  }

  size_t total = 1 + tag_len + (attr 
    ? (1 + attr_len + 2 + escaped_len + 1) 
    : 0) + 1 + (size_t)str_len + 2 + tag_len + 1;
  
  char *buf = (char *)calloc(1, total);
  if (!buf) return js_mkerr(js, "oom");

  size_t pos = 0;
  buf[pos++] = '<';
  memcpy(buf + pos, tag, tag_len); pos += tag_len;
  
  if (attr) {
    buf[pos++] = ' ';
    memcpy(buf + pos, attr, attr_len); pos += attr_len;
    buf[pos++] = '=';
    buf[pos++] = '"';
    html_attr_append_escaped(buf, &pos, value_ptr, value_len);
    buf[pos++] = '"';
  }
  
  buf[pos++] = '>';
  memcpy(buf + pos, str_ptr, (size_t)str_len); pos += (size_t)str_len;
  buf[pos++] = '<';
  buf[pos++] = '/';
  memcpy(buf + pos, tag, tag_len); pos += tag_len;
  buf[pos++] = '>';

  ant_value_t out = js_mkstr(js, buf, pos);
  free(buf);
  
  return out;
}
#define STRING_HTML_METHOD(name, tag, attr, arity)                            \
  static ant_value_t builtin_string_##name(ant_params_t) {                    \
    return builtin_string_html(js, args, nargs, call_this, #name, tag, attr); \
  }
#include "string/html_methods.h"


ant_value_t builtin_string_charCodeAt(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "charCodeAt");
  
  if (is_err(str)) return str;
  if (vtype(str) != kTypeString) return js_mkerr(js, "charCodeAt called on non-string");
  
  double idx_d = nargs < 1 ? 0.0 : js_to_number(js, args[0]);
  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
  if (isnan(idx_d)) idx_d = 0.0;
  if (idx_d >= (double)LONG_MAX || idx_d <= (double)LONG_MIN) return Ant_Value_FromNumber(ANT_NAN);
  
  long idx_l = (long) idx_d;
  if (idx_l < 0) return Ant_Value_FromNumber(ANT_NAN);
  
  ant_offset_t byte_len; ant_offset_t str_off = vstr(js, str, &byte_len);
  const char *str_data = (const char *)(uintptr_t)(str_off);
  
  uint32_t code_unit = utf16_code_unit_at(str_data, byte_len, idx_l);
  if (code_unit == 0xFFFFFFFF) return Ant_Value_FromNumber(ANT_NAN);
  
  return Ant_Value_FromNumber((double) code_unit);
}

bool js_try_char_code_at(
  ant_t *js, ant_value_t func, ant_value_t receiver,
  ant_value_t *args, int argc, ant_value_t *result
) {
  if (vtype(func) == kTypeFunction) {
    sv_closure_t *closure = js_func_closure(func);
    if (closure->func || (closure->call_flags & SV_CALL_HAS_BOUND_ARGS)) return false;
    func = js_get_slot(closure->func_obj, SLOT_CFUNC);
    
    if (closure->call_flags & SV_CALL_HAS_BOUND_THIS) receiver = closure->bound_this;
    if (vtype(func) == kTypeBuiltin && js_as_cfunc(func) == builtin_function_call) {
      if (argc == 0) return false;
      func = receiver; receiver = *args++; argc--;
    }
  }
  
  if (vtype(func) != kTypeBuiltin || js_as_cfunc(func) != builtin_string_charCodeAt) return false;
  if (vtype(receiver) != kTypeString) return false;
  if (argc > 0 && vtype(args[0]) != kTypeNumber) return false;
  if (str_is_heap_rope(receiver)) receiver = rope_cached_flat(receiver);
  if (vtype(receiver) != kTypeString) return false;
  if (str_is_heap_rope(receiver) || str_is_heap_builder(receiver)) return false;

  ant_offset_t length;
  const char *bytes = (const char *)(uintptr_t)vstr(js, receiver, &length);
  
  if (!str_is_ascii(bytes)) return false;
  double index = argc ? Ant_Value_AsNumberUnchecked(args[0]) : 0;
  
  index = isnan(index) ? 0 : trunc(index);
  *result = index < 0 || index >= (double)length
    ? Ant_Value_FromNumber(ANT_NAN) : Ant_Value_FromNumber((double)(unsigned char)bytes[(size_t)index]);
  
  return true;
}

static ant_value_t builtin_string_codePointAt(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "codePointAt");
  if (is_err(str)) return str;
  
  double idx_d = nargs < 1 ? 0.0 : js_to_number(js, args[0]);
  if (isnan(idx_d)) idx_d = 0.0;
  if (isinf(idx_d) || idx_d > (double)LONG_MAX) return Ant_Value_Undefined;
  
  long idx_l = (long) idx_d;
  if (idx_l < 0) return Ant_Value_Undefined;
  
  ant_offset_t byte_len;
  ant_offset_t str_off = vstr(js, str, &byte_len);
  const char *str_data = (const char *)(uintptr_t)(str_off);
  
  uint32_t cp = utf16_codepoint_at(str_data, byte_len, idx_l);
  if (cp == 0xFFFFFFFF) return Ant_Value_Undefined;
  
  return Ant_Value_FromNumber((double) cp);
}

static ant_value_t string_case_map(ant_t *js, ant_value_t str, bool upper) {
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  if (str_len == 0) return js_mkstr(js, "", 0);

  if (str_is_ascii(str_ptr)) {
    unsigned char lo = upper ? 'a' : 'A';
    ant_offset_t first = 0;
    while (first < str_len && (unsigned char)(str_ptr[first] - lo) > 'z' - 'a') first++;
    if (first == str_len) return str;
    
    GC_ROOT_SAVE(ascii_mark, js);
    GC_ROOT_PIN(js, str);
    ant_value_t result = js_mkstr(js, NULL, str_len);
    GC_ROOT_RESTORE(js, ascii_mark);
    if (is_err(result)) return result;
    str_ptr = (const char *)(uintptr_t)vstr(js, str, NULL);
    ant_offset_t result_len, result_off = vstr(js, result, &result_len);
    char *out = (char *)(uintptr_t)(result_off);
    memcpy(out, str_ptr, (size_t)first);
    for (ant_offset_t i = first; i < str_len; i++) {
      char c = str_ptr[i];
      out[i] = (unsigned char)(c - lo) <= 'z' - 'a' ? (char)(c ^ 0x20) : c;
    }
    str_set_ascii_state(out, STR_ASCII_YES);
    return result;
  }

  const utf8proc_uint8_t *src = (const utf8proc_uint8_t *)str_ptr;
  utf8proc_ssize_t src_len = (utf8proc_ssize_t)str_len;

  ant_offset_t out_len = 0;
  utf8proc_ssize_t pos = 0;
  
  while (pos < src_len) {
    utf8proc_int32_t cp;
    utf8proc_ssize_t n = utf8_next(src + pos, src_len - pos, &cp);
    if (cp < 0) { out_len++; pos++; continue; }
    if (cp < 0x80) { out_len++; pos += n; continue; }
    utf8proc_int32_t mapped[3];
    int count = utf8_full_case_map(src, src_len, pos, n, cp, upper, mapped);
    utf8proc_uint8_t tmp[4];
    for (int k = 0; k < count; k++) out_len += (ant_offset_t)utf8proc_encode_char(mapped[k], tmp);
    pos += n;
  }

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, str);
  ant_value_t result = js_mkstr(js, NULL, out_len);
  GC_ROOT_RESTORE(js, root_mark);
  if (is_err(result)) return result;
  
  ant_offset_t result_len, result_off = vstr(js, result, &result_len);
  char *result_ptr = (char *)(uintptr_t)(result_off);
  uint8_t ascii_state = STR_ASCII_YES;

  pos = 0;
  ant_offset_t wpos = 0;
  
  while (pos < src_len) {
    utf8proc_int32_t cp;
    utf8proc_ssize_t n = utf8_next(src + pos, src_len - pos, &cp);
    
    if (cp < 0) {
      unsigned char byte = src[pos];
      if (byte >= 0x80) ascii_state = STR_ASCII_NO;
      result_ptr[wpos++] = (char)byte;
      pos++; continue;
    }
    
    if (cp < 0x80) {
      char lo = upper ? 'a' : 'A', hi = upper ? 'z' : 'Z';
      result_ptr[wpos++] = (cp >= lo && cp <= hi) ? (char)(cp ^ 0x20) : (char)cp;
      pos += n;
      continue;
    }
    utf8proc_int32_t mapped[3];
    int count = utf8_full_case_map(src, src_len, pos, n, cp, upper, mapped);
    for (int k = 0; k < count; k++) {
      if (mapped[k] >= 0x80) ascii_state = STR_ASCII_NO;
      wpos += (ant_offset_t)utf8proc_encode_char(mapped[k], (utf8proc_uint8_t *)(result_ptr + wpos));
    }
    pos += n;
  }
  
  str_set_ascii_state(result_ptr, ascii_state);
  return result;
}

static ant_value_t string_case_map_this(ant_t *js, ant_value_t call_this, const char *name, bool upper) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, name);
  if (vtype(str) != kTypeString) return str;
  return string_case_map(js, str, upper);
}

static ant_value_t builtin_string_toLowerCase(ant_params_t) {
  return string_case_map_this(js, call_this, "toLowerCase", false);
}

static ant_value_t builtin_string_toLocaleLowerCase(ant_params_t) {
  return string_case_map_this(js, call_this, "toLocaleLowerCase", false);
}

static ant_value_t builtin_string_toUpperCase(ant_params_t) {
  return string_case_map_this(js, call_this, "toUpperCase", true);
}

static ant_value_t builtin_string_toLocaleUpperCase(ant_params_t) {
  return string_case_map_this(js, call_this, "toLocaleUpperCase", true);
}

static inline size_t string_trim_prefix_len(const char *p, size_t len) {
  unsigned char c = (unsigned char)p[0];
  if (c < 0x80) return (c == ' ' || (c >= '\t' && c <= '\r')) ? 1 : 0;
  return ant_js_trim_prefix_len(p, len);
}

static inline size_t string_trim_suffix_len(const char *p, size_t len) {
  unsigned char c = (unsigned char)p[len - 1];
  if (c < 0x80) return (c == ' ' || (c >= '\t' && c <= '\r')) ? 1 : 0;
  return ant_js_trim_suffix_len(p, len);
}

static ant_value_t builtin_string_trim(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "trim");
  if (is_err(str)) return str;
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  
  ant_offset_t start = 0, end = str_len;
  size_t n;
  while (start < end && (n = string_trim_prefix_len(str_ptr + start, (size_t)(end - start)))) start += n;
  while (end > start && (n = string_trim_suffix_len(str_ptr + start, (size_t)(end - start)))) end -= n;
  
  return js_mkstr_byte_range(js, str_ptr, start, end - start);
}

static ant_value_t builtin_string_trimStart(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "trimStart");
  if (is_err(str)) return str;
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  
  ant_offset_t start = 0;
  size_t n;
  while (start < str_len && (n = string_trim_prefix_len(str_ptr + start, (size_t)(str_len - start)))) start += n;
  
  return js_mkstr_byte_range(js, str_ptr, start, str_len - start);
}

static ant_value_t builtin_string_trimEnd(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "trimEnd");
  if (is_err(str)) return str;
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  
  ant_offset_t end = str_len;
  size_t n;
  while (end > 0 && (n = string_trim_suffix_len(str_ptr, (size_t)end))) end -= n;
  
  return js_mkstr_byte_range(js, str_ptr, 0, end);
}

static ant_value_t builtin_string_repeat(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "repeat");
  if (is_err(str)) return str;

  double count_d = 0;
  if (nargs > 0) {
    ant_value_t status = string_to_integer_or_infinity(js, args[0], &count_d);
    if (is_err(status)) return status;
  }
  
  if (isinf(count_d))
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid count value: %sInfinity", count_d < 0 ? "-" : "");
  if (count_d < 0)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid count value: %.0f", count_d);
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  
  if (count_d == 0 || str_len == 0) return js_mkstr(js, "", 0);
  if (count_d * (double)str_utf16_len(js, str) > ANT_STRING_MAX_LENGTH)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid string length");
  ant_offset_t count = (ant_offset_t)count_d;
  
  ant_value_t result = js_mkstr(js, NULL, str_len * count);
  if (is_err(result)) return result;
  
  ant_offset_t result_len, result_off = vstr(js, result, &result_len);
  char *result_ptr = (char *)(uintptr_t)(result_off);
  
  for (ant_offset_t i = 0; i < count; i++)
    memcpy(result_ptr + i * str_len, str_ptr, str_len);
  
  str_flat_finish(
    str_flat_from_bytes(result_ptr),
    str_is_ascii(str_ptr) ? STR_ASCII_YES : STR_ASCII_NO
  );
  
  return result;
}

static ant_value_t string_pad_impl(ant_native_params_t, ant_value_t call_this, bool at_start) {
  const char *name = at_start ? "padStart" : "padEnd";
  ant_value_t str = Ant_String_CoerceThis(js, call_this, name);
  if (vtype(str) != kTypeString) return str;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, str);
  ant_value_t result = str;

  double max_length = 0.0;
  if (nargs >= 1) {
    ant_value_t status = string_to_integer_or_infinity(js, args[0], &max_length);
    if (is_err(status)) { result = status; goto done; }
  }

  size_t str_units = (size_t)str_utf16_len(js, str);
  if (max_length <= (double)str_units) goto done;

  ant_value_t pad_val = nargs >= 2 && vtype(args[1]) != kTypeUndefined
    ? coerce_to_str(js, args[1]) : js_mkstr(js, " ", 1);
  if (is_err(pad_val)) { result = pad_val; goto done; }
  GC_ROOT_PIN(js, pad_val);

  size_t pad_units = (size_t)str_utf16_len(js, pad_val);
  if (pad_units == 0) goto done;
  if (max_length > ANT_STRING_MAX_LENGTH) {
    result = js_mkerr_typed(js, JS_ERR_RANGE, "Invalid string length");
    goto done;
  }

  ant_offset_t pad_len, pad_off = vstr(js, pad_val, &pad_len);
  const char *pad_str = (const char *)(uintptr_t)pad_off;

  size_t fill_units = (size_t)max_length - str_units;
  size_t full_repeats = fill_units / pad_units;
  size_t rem_units = fill_units % pad_units;
  size_t rem_start = 0, rem_bytes = 0;
  utf16_range_splits_t splits = {0};
  if (rem_units > 0) splits = utf16_range_to_byte_range(
    pad_str, (size_t)pad_len, 0, rem_units, &rem_start, &rem_bytes);

  char suffix[4];
  size_t suffix_len = splits.suffix_surrogate
    ? utf16_code_unit_to_wtf8(splits.suffix_surrogate, suffix) : 0;
  size_t fill_bytes = full_repeats * (size_t)pad_len + rem_bytes + suffix_len;

  ant_offset_t str_len = 0;
  vstr(js, str, &str_len);
  result = js_mkstr(js, NULL, (ant_offset_t)(fill_bytes + (size_t)str_len));
  if (is_err(result)) goto done;

  ant_offset_t result_len, result_off = vstr(js, result, &result_len);
  char *result_ptr = (char *)(uintptr_t)result_off;
  ant_offset_t str_off = vstr(js, str, &str_len);
  pad_off = vstr(js, pad_val, &pad_len);
  const char *str_ptr = (const char *)(uintptr_t)str_off;
  pad_str = (const char *)(uintptr_t)pad_off;

  char *fill = at_start ? result_ptr : result_ptr + str_len;
  memcpy(at_start ? result_ptr + fill_bytes : result_ptr, str_ptr, (size_t)str_len);
  for (size_t i = 0; i < full_repeats; i++) {
    memcpy(fill, pad_str, (size_t)pad_len);
    fill += pad_len;
  }
  memcpy(fill, pad_str, rem_bytes);
  memcpy(fill + rem_bytes, suffix, suffix_len);

  str_flat_finish(
    str_flat_from_bytes(result_ptr),
    (str_is_ascii(pad_str) && str_is_ascii(str_ptr)) ? STR_ASCII_YES : STR_ASCII_NO
  );

done:
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static ant_value_t builtin_string_padStart(ant_params_t) {
  return string_pad_impl(js, args, nargs, call_this, true);
}

static ant_value_t builtin_string_padEnd(ant_params_t) {
  return string_pad_impl(js, args, nargs, call_this, false);
}

static ant_value_t builtin_string_charAt(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "charAt");
  if (is_err(str)) return str;
  
  double idx_d = nargs < 1 ? 0.0 : js_to_number(js, args[0]);
  if (isnan(idx_d)) idx_d = 0;
  else if (idx_d < 0) idx_d = -floor(-idx_d);
  else idx_d = floor(idx_d);
  if (idx_d < 0 || isinf(idx_d)) return js_mkstr(js, "", 0);
  
  ant_offset_t idx = (ant_offset_t) idx_d;
  ant_offset_t byte_len;
  ant_offset_t str_off = vstr(js, str, &byte_len);
  
  const char *str_data = (const char *)(uintptr_t)(str_off);
  uint32_t code_unit = utf16_code_unit_at(str_data, byte_len, idx);
  if (code_unit == 0xFFFFFFFF) return js_mkstr(js, "", 0);
  
  return js_string_from_utf16_code_unit(js, code_unit);
}

static ant_value_t builtin_string_at(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "at");
  if (is_err(str)) return str;
  
  double idx_d = 0.0;
  if (nargs >= 1) {
    GC_ROOT_SAVE(root_mark, js);
    GC_ROOT_PIN(js, str);
    ant_value_t status = string_to_integer_or_infinity(js, args[0], &idx_d);
    GC_ROOT_RESTORE(js, root_mark);
    if (is_err(status)) return status;
  }

  ant_offset_t byte_len; ant_offset_t str_off = vstr(js, str, &byte_len);
  const char *str_data = (const char *)(uintptr_t)(str_off);
  size_t utf16_len = (size_t)str_utf16_len(js, str);
  
  if (idx_d < 0) idx_d += (double)utf16_len;
  if (idx_d < 0 || idx_d >= (double)utf16_len) return Ant_Value_Undefined;
  size_t idx = (size_t)idx_d;
  
  uint32_t code_unit = utf16_code_unit_at(str_data, byte_len, idx);
  if (code_unit == 0xFFFFFFFF) return Ant_Value_Undefined;
  
  return js_string_from_utf16_code_unit(js, code_unit);
}

static ant_value_t builtin_string_localeCompare(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "localeCompare");
  if (is_err(str)) return str;
  if (nargs < 1) return Ant_Value_FromNumber(0);
  
  ant_value_t that = args[0];
  if (vtype(that) != kTypeString) {
    that = coerce_to_str(js, that);
    if (is_err(that)) return that;
  }
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  ant_offset_t that_len, that_off = vstr(js, that, &that_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  const char *that_ptr = (char *)(uintptr_t)(that_off);
  
  int result = strcoll(str_ptr, that_ptr);
  if (result < 0) return Ant_Value_FromNumber(-1.0);
  if (result > 0) return Ant_Value_FromNumber(1.0);
  
  return Ant_Value_FromNumber(0);
}

static __attribute__((noinline)) ant_value_t string_last_index_of_strings(
  ant_t *js, ant_value_t str, const char *hay, ant_offset_t str_len,
  ant_value_t search, double position
) {
  ant_offset_t search_len;
  const char *needle = (const char *)(uintptr_t)vstr(js, search, &search_len);

  size_t start = SIZE_MAX;
  if (isnan(position)) position = ANT_INFINITY;
  if (position < 1.0) start = 0;
  else if (search_len == 0 || position < (double)str_len) {
    size_t str_units = (size_t)str_utf16_len(js, str);
    start = position >= (double)str_units ? str_units : (size_t)position;
  }
  if (search_len == 0) return Ant_Value_FromNumber((double)start);

  ptrdiff_t found = string_find_utf16(hay, (size_t)str_len, needle, (size_t)search_len, start, true);
  return found == STRING_FIND_OOM ? js_mkerr(js, "oom") : Ant_Value_FromNumber((double)found);
}

static __attribute__((noinline)) 
ant_value_t string_last_index_of_slow(ant_t *js, ant_value_t str, ant_args_t) {
  ant_value_t search = nargs > 0 ? args[0] : Ant_Value_Undefined;
  ant_value_t position_arg = nargs > 1 ? args[1] : Ant_Value_Undefined;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, str);
  GC_ROOT_PIN(js, position_arg);
  ant_value_t result;

  if (vtype(search) != kTypeString) search = coerce_to_str(js, search);
  GC_ROOT_PIN(js, search);
  if (is_err(search)) { result = search; goto done; }

  double position = ANT_NAN;
  if (vtype(position_arg) != kTypeUndefined) {
    ant_value_t status = string_to_number_arg(js, position_arg, &position);
    if (is_err(status)) { result = status; goto done; }
  }
  ant_offset_t str_len;
  const char *hay = (const char *)(uintptr_t)vstr(js, str, &str_len);
  result = string_last_index_of_strings(js, str, hay, str_len, search, position);

done:
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static ant_value_t builtin_string_lastIndexOf(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "lastIndexOf");
  if (vtype(str) != kTypeString) return str;
  bool has_position = nargs >= 2 && vtype(args[1]) != kTypeUndefined;
  if (nargs < 1 || vtype(args[0]) != kTypeString || (has_position && vtype(args[1]) != kTypeNumber))
    return string_last_index_of_slow(js, str, args, nargs);

  ant_offset_t str_len, search_len;
  const char *hay = (const char *)(uintptr_t)vstr(js, str, &str_len);
  if (!str_is_ascii(hay))
    return string_last_index_of_strings(js, str, hay, str_len, args[0], has_position ? Ant_Value_AsNumberUnchecked(args[1]) : ANT_NAN);

  size_t max_start = (size_t)str_len;
  if (has_position) {
    double position = Ant_Value_AsNumberUnchecked(args[1]);
    if (position < 0) max_start = 0;
    else if (position < (double)str_len) max_start = (size_t)position;
  }

  const char *needle = (const char *)(uintptr_t)vstr(js, args[0], &search_len);
  if (search_len == 0) return Ant_Value_FromNumber((double)max_start);
  if (search_len > str_len || !str_is_ascii(needle)) return Ant_Value_FromNumber(-1);
  const char *match = string_bytes_find_last(hay, (size_t)str_len, needle, (size_t)search_len, max_start);
  return Ant_Value_FromNumber(match ? (double)(match - hay) : -1.0);
}

static ant_value_t builtin_string_concat(ant_params_t) {
  if (vtype(call_this) == kTypeUndefined || vtype(call_this) == kTypeNull)
    return js_mkerr_typed(js, JS_ERR_TYPE, "String.prototype.concat called on null or undefined");
  GC_ROOT_SAVE(root_mark, js);
  
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "concat");
  
  GC_ROOT_PIN(js, str);
  if (is_err(str)) {
    GC_ROOT_RESTORE(js, root_mark);
    return str;
  }

  if (nargs == 0) {
    GC_ROOT_RESTORE(js, root_mark);
    return str;
  }

  ant_offset_t base_len = 0;
  vstr(js, str, &base_len);
  size_t total_len = (size_t)base_len;
  
  ant_value_t stack_args[8];
  ant_value_t *str_args = stack_args;
  
  bool args_need_free = 
    nargs > (int)(sizeof(stack_args) / sizeof(stack_args[0]));
  
  if (args_need_free) {
    if ((size_t)nargs > SIZE_MAX / sizeof(*str_args)) {
      ant_value_t err = js_mkerr(js, "string too large");
      GC_ROOT_RESTORE(js, root_mark);
      return err;
    }
    
    str_args = (ant_value_t *)calloc(1, (size_t)nargs * sizeof(*str_args));
    if (!str_args) {
      ant_value_t err = js_mkerr(js, "oom");
      GC_ROOT_RESTORE(js, root_mark);
      return err;
    }
  }

  for (int i = 0; i < nargs; i++) {
    str_args[i] = coerce_to_str_hint(js, args[i], 1);
    GC_ROOT_PIN(js, str_args[i]);
    if (is_err(str_args[i])) {
      ant_value_t err = str_args[i];
      if (args_need_free) free(str_args);
      GC_ROOT_RESTORE(js, root_mark);
      return err;
    }

    ant_offset_t arg_len = 0;
    vstr(js, str_args[i], &arg_len);
    if ((size_t)arg_len > SIZE_MAX - total_len) {
      ant_value_t err = js_mkerr(js, "string too large");
      if (args_need_free) free(str_args);
      GC_ROOT_RESTORE(js, root_mark);
      return err;
    }
    total_len += (size_t)arg_len;
  }

  ant_value_t ret = js_mkstr(js, NULL, total_len);
  GC_ROOT_PIN(js, ret);
  
  if (is_err(ret)) {
    if (args_need_free) free(str_args);
    GC_ROOT_RESTORE(js, root_mark);
    return ret;
  }

  ant_flat_string_t *flat = (ant_flat_string_t *)vptr(ret);
  ant_offset_t base_off = vstr(js, str, &base_len);
  
  memcpy(flat->bytes, (const void *)(uintptr_t)base_off, (size_t)base_len);
  size_t pos = (size_t)base_len;

  for (int i = 0; i < nargs; i++) {
    ant_offset_t arg_len, arg_off = vstr(js, str_args[i], &arg_len);
    memcpy(flat->bytes + pos, (const void *)(uintptr_t)arg_off, (size_t)arg_len);
    pos += (size_t)arg_len;
  }
  
  flat->bytes[pos] = '\0';
  str_flat_finish(flat, str_detect_ascii_bytes(flat->bytes, pos));
  
  if (args_need_free) free(str_args);
  GC_ROOT_RESTORE(js, root_mark);
  
  return ret;
}

static ant_value_t builtin_string_isWellFormed(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "isWellFormed");
  if (vtype(str) != kTypeString) return str;
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  return Ant_Value_FromBool(str_is_well_formed((const char *)(uintptr_t)str_off, (size_t)str_len));
}

static ant_value_t builtin_string_toWellFormed(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "toWellFormed");
  if (vtype(str) != kTypeString) return str;
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (const char *)(uintptr_t)str_off;
  if (str_is_well_formed(str_ptr, (size_t)str_len)) return str;
  size_t len = (size_t)str_len;
  char *utf8 = wtf8_to_utf8_dup(str_ptr, &len);
  if (!utf8) return js_mkerr(js, "oom");
  ant_value_t result = js_mkstr(js, utf8, len);
  free(utf8);
  return result;
}

static ant_value_t builtin_string_normalize(ant_params_t) {
  ant_value_t str = Ant_String_CoerceThis(js, call_this, "normalize");
  if (is_err(str)) return str;

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (const char *)(uintptr_t)(str_off);

  if (str_len == 0) return js_mkstr(js, "", 0);
  unicode_form_t form = UNICODE_NFC;

  if (nargs >= 1 && vtype(args[0]) != kTypeUndefined) {
    ant_value_t form_val = js_tostring_val(js, args[0]);
    if (is_err(form_val)) return form_val;
    ant_offset_t flen, foff = vstr(js, form_val, &flen);
    const char *name = (const char *)(uintptr_t)(foff);

    if (flen == 3 && memcmp(name, "NFC", 3) == 0) form = UNICODE_NFC;
    else if (flen == 3 && memcmp(name, "NFD", 3) == 0) form = UNICODE_NFD;
    else if (flen == 4 && memcmp(name, "NFKC", 4) == 0) form = UNICODE_NFKC;
    else if (flen == 4 && memcmp(name, "NFKD", 4) == 0) form = UNICODE_NFKD;
    else return js_mkerr_typed(js, JS_ERR_RANGE, "The normalization form should be one of NFC, NFD, NFKC, NFKD");
  }

  if (str_is_ascii(str_ptr) || unicode_is_normalized(str_ptr, (size_t)str_len, form)) return str;

  size_t rlen = 0;
  char *result = unicode_normalize(str_ptr, (size_t)str_len, form, &rlen);
  if (!result) return js_mkstr(js, str_ptr, str_len);

  ant_value_t ret = js_mkstr(js, result, (ant_offset_t)rlen);
  free(result);

  return ret;
}

static ant_value_t builtin_string_fromCharCode(ant_params_t) {
  if (nargs == 0) return js_mkstr(js, "", 0);

  if ((size_t)nargs > SIZE_MAX / 3) return js_mkerr(js, "string too large");
  size_t capacity = (size_t)nargs * 3;
  char static_buf[256];
  bool needs_free = capacity > sizeof(static_buf);
  char *buf = needs_free ? (char *)malloc(capacity) : static_buf;
  if (!buf) return js_mkerr(js, "oom");

  size_t len = 0;
  for (int i = 0; i < nargs; i++) {
    double d = vtype(args[i]) == kTypeNumber ? Ant_Value_AsNumberUnchecked(args[i]) : js_to_number(js, args[i]);
    uint32_t code_unit = isfinite(d) ? ((uint32_t)d & 0xFFFFu) : 0;

    if (code_unit < 0x80) {
      buf[len++] = (char)code_unit;
    } else if (code_unit < 0x800) {
      buf[len++] = (char)(0xC0 | (code_unit >> 6));
      buf[len++] = (char)(0x80 | (code_unit & 0x3F));
    } else {
      buf[len++] = (char)(0xE0 | (code_unit >> 12));
      buf[len++] = (char)(0x80 | ((code_unit >> 6) & 0x3F));
      buf[len++] = (char)(0x80 | (code_unit & 0x3F));
    }
  }

  ant_value_t result = js_mkstr(js, buf, len);
  if (needs_free) free(buf);
  return result;
}

static ant_value_t builtin_string_fromCodePoint(ant_params_t) {
  if (nargs == 0) return js_mkstr(js, "", 0);

  if ((size_t)nargs > SIZE_MAX / 4) return js_mkerr(js, "string too large");
  size_t capacity = (size_t)nargs * 4;
  char static_buf[256];
  bool needs_free = capacity > sizeof(static_buf);
  char *buf = needs_free ? (char *)calloc(1, capacity) : static_buf;
  if (!buf) return js_mkerr(js, "oom");

  size_t len = 0;
  for (int i = 0; i < nargs; i++) {
    if (vtype(args[i]) != kTypeNumber) continue;
    double d = Ant_Value_AsNumberUnchecked(args[i]);
    
    if (d < 0 || d > 0x10FFFF || d != floor(d)) {
      if (needs_free) free(buf);
      return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid code point");
    }
    
    uint32_t cp = (uint32_t)d;

    if (nargs >= 16 && cp >= 0x10000) {
      buf[len++] = (char)(0xF0 | (cp >> 18));
      buf[len++] = (char)(0x80 | ((cp >> 12) & 0x3F));
      buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      buf[len++] = (char)(0x80 | (cp & 0x3F));
      continue;
    }
    
    if (cp < 0x80) {
      buf[len++] = (char)cp;
    } else if (cp < 0x800) {
      buf[len++] = (char)(0xC0 | (cp >> 6));
      buf[len++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      buf[len++] = (char)(0xE0 | (cp >> 12));
      buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      buf[len++] = (char)(0x80 | (cp & 0x3F));
    } else {
      buf[len++] = (char)(0xF0 | (cp >> 18));
      buf[len++] = (char)(0x80 | ((cp >> 12) & 0x3F));
      buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      buf[len++] = (char)(0x80 | (cp & 0x3F));
    }
  }

  ant_value_t result = js_mkstr(js, buf, len);
  if (needs_free) free(buf);
  return result;
}

static ant_value_t builtin_string_raw(ant_params_t) {
  if (nargs < 1 || is_null(args[0]) || is_undefined(args[0])) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "String.raw requires a template object");
  }

  ant_value_t tmpl = args[0];
  if (!is_object_type(tmpl)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "String.raw requires a template object");
  }

  ant_value_t raw = js_get(js, tmpl, "raw");
  if (is_null(raw) || is_undefined(raw) || !is_object_type(raw)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "String.raw requires template.raw");
  }

  ant_value_t raw_len_val = js_get(js, raw, "length");
  double raw_len_num = js_to_number(js, raw_len_val);
  if (!isfinite(raw_len_num) || raw_len_num <= 0) return js_mkstr(js, "", 0);

  size_t literal_count = (size_t)raw_len_num;
  if (literal_count == 0) return js_mkstr(js, "", 0);

  string_builder_t sb;
  char static_buf[256];
  string_builder_init(&sb, static_buf, sizeof(static_buf));
  
  ant_value_t err = Ant_Value_Undefined;
  for (size_t i = 0; i < literal_count; i++) {
    ant_value_t chunk = Ant_Value_Undefined;
    if (vtype(raw) == kTypeArray) chunk = js_arr_get(js, raw, (ant_offset_t)i);
    else {
      char key[32];
      snprintf(key, sizeof(key), "%zu", i);
      chunk = js_get(js, raw, key);
    }
    
    if (!string_builder_append_value(js, &sb, chunk, &err)) {
      string_builder_dispose(&sb);
      return is_err(err) ? err : js_mkerr(js, "oom");
    }
    
    if (i + 1 >= literal_count || (int)(i + 1) >= nargs) continue;
    if (!string_builder_append_value(js, &sb, args[i + 1], &err)) {
      string_builder_dispose(&sb);
      return is_err(err) ? err : js_mkerr(js, "oom");
    }
  }

  return string_builder_finalize(js, &sb);
}

static ant_value_t string_this_value(ant_t *js, ant_value_t call_this, const char *method) {
  if (vtype(call_this) == kTypeString) return call_this;
  
  ant_value_t wrapped = js_string_wrapper_value(call_this);
  if (vtype(wrapped) == kTypeString) return wrapped;
  
  return js_mkerr_typed(js, JS_ERR_TYPE, "String.prototype.%s requires that 'this' be a String", method);
}

static ant_value_t builtin_string_valueOf(ant_params_t) {
  return string_this_value(js, call_this, "valueOf");
}

ant_value_t builtin_string_toString(ant_params_t) {
  return string_this_value(js, call_this, "toString");
}

void js_string_init_prototype(ant_t *js, ant_value_t string_proto) {
  js_set_slot(string_proto, SLOT_PRIMITIVE, js_mkstr(js, "", 0));
  
  defmethod(js, string_proto, "indexOf", 7, Ant_Function_CreateWithArity(builtin_string_indexOf, 1));
  defmethod(js, string_proto, "substring", 9, Ant_Function_CreateWithArity(builtin_string_substring, 2));
  defmethod(js, string_proto, "substr", 6, Ant_Function_CreateWithArity(builtin_string_substr, 2));
  defmethod(js, string_proto, "split", 5, Ant_Function_CreateWithArity(builtin_string_split, 2));
  defmethod(js, string_proto, "slice", 5, Ant_Function_CreateWithArity(builtin_string_slice, 2));
  defmethod(js, string_proto, "includes", 8, Ant_Function_CreateWithArity(builtin_string_includes, 1));
  defmethod(js, string_proto, "startsWith", 10, Ant_Function_CreateWithArity(builtin_string_startsWith, 1));
  defmethod(js, string_proto, "endsWith", 8, Ant_Function_CreateWithArity(builtin_string_endsWith, 1));
  defmethod(js, string_proto, "template", 8, Ant_Function_Create(builtin_string_template));
  defmethod(js, string_proto, "charCodeAt", 10, Ant_Function_CreateWithArity(builtin_string_charCodeAt, 1));
  defmethod(js, string_proto, "codePointAt", 11, Ant_Function_CreateWithArity(builtin_string_codePointAt, 1));
  defmethod(js, string_proto, "isWellFormed", 12, Ant_Function_Create(builtin_string_isWellFormed));
  defmethod(js, string_proto, "toWellFormed", 12, Ant_Function_Create(builtin_string_toWellFormed));
  defmethod(js, string_proto, "toLowerCase", 11, Ant_Function_Create(builtin_string_toLowerCase));
  defmethod(js, string_proto, "toUpperCase", 11, Ant_Function_Create(builtin_string_toUpperCase));
  defmethod(js, string_proto, "toLocaleLowerCase", 17, Ant_Function_Create(builtin_string_toLocaleLowerCase));
  defmethod(js, string_proto, "toLocaleUpperCase", 17, Ant_Function_Create(builtin_string_toLocaleUpperCase));
  defmethod(js, string_proto, "trim", 4, Ant_Function_Create(builtin_string_trim));
  defmethod(js, string_proto, "trimStart", 9, Ant_Function_Create(builtin_string_trimStart));
  defmethod(js, string_proto, "trimEnd", 7, Ant_Function_Create(builtin_string_trimEnd));
  defalias_of(js, string_proto, "trimLeft", "trimStart");
  defalias_of(js, string_proto, "trimRight", "trimEnd");
  defmethod(js, string_proto, "repeat", 6, Ant_Function_CreateWithArity(builtin_string_repeat, 1));
  defmethod(js, string_proto, "padStart", 8, Ant_Function_CreateWithArity(builtin_string_padStart, 1));
  defmethod(js, string_proto, "padEnd", 6, Ant_Function_CreateWithArity(builtin_string_padEnd, 1));
  defmethod(js, string_proto, "charAt", 6, Ant_Function_CreateWithArity(builtin_string_charAt, 1));
  defmethod(js, string_proto, "at", 2, Ant_Function_CreateWithArity(builtin_string_at, 1));
  defmethod(js, string_proto, "lastIndexOf", 11, Ant_Function_CreateWithArity(builtin_string_lastIndexOf, 1));
  defmethod(js, string_proto, "concat", 6, Ant_Function_CreateWithArity(builtin_string_concat, 1));
  defmethod(js, string_proto, "localeCompare", 13, Ant_Function_CreateWithArity(builtin_string_localeCompare, 1));
  defmethod(js, string_proto, "normalize", 9, Ant_Function_Create(builtin_string_normalize));
  defmethod(js, string_proto, "valueOf", 7, Ant_Function_Create(builtin_string_valueOf));
  defmethod(js, string_proto, "toString", 8, Ant_Function_Create(builtin_string_toString));
  #define STRING_HTML_METHOD(name, tag, attr, arity) \
    defmethod(js, string_proto, #name, sizeof(#name) - 1, Ant_Function_CreateWithArity(builtin_string_##name, arity));
  #include "string/html_methods.h"
}

void js_string_init_constructor(ant_t *js, ant_value_t str_ctor_obj, ant_value_t string_proto) {
  js_set_slot(str_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(builtin_String, 1));
  js_setprop_readonly_nonconfigurable(js, str_ctor_obj, "prototype", 9, string_proto);
  defmethod(js, str_ctor_obj, "fromCharCode", 12, Ant_Function_CreateWithArity(builtin_string_fromCharCode, 1));
  defmethod(js, str_ctor_obj, "fromCodePoint", 13, Ant_Function_CreateWithArity(builtin_string_fromCodePoint, 1));
  defmethod(js, str_ctor_obj, "raw", 3, Ant_Function_CreateWithArity(builtin_string_raw, 1));
  js_set_function_name(js, str_ctor_obj, "String", sizeof("String") - 1);
}
