#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ant.h"
#include "cfunc.h"
#include "ascii.h"
#include "errors.h"
#include "internal.h"

#include "modules/intl.h"
#include "modules/intl_tag.h"

typedef struct {
  int hour12;
  int minute;
  int second;
  const char *day_period;
} intl_dtf_fields_t;

static ant_value_t intl_create_instance(ant_t *js, ant_value_t fallback_proto, ant_value_t new_target) {
  ant_value_t obj = Ant_Object_CreateBare(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, fallback_proto, new_target);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  return obj;
}

static ant_value_t intl_default_locale(ant_t *js) {
  return js_mkstr(js, "en-US", 5);
}

static ant_value_t intl_canonicalize_tag(ant_t *js, ant_value_t locale) {
  size_t len = 0;
  const char *tag = js_getstr(js, locale, &len);

  char *canonical = NULL;
  size_t canonical_len = 0;
  intl_tag_status_t status = Ant_Intl_CanonicalizeTag(tag, len, &canonical, &canonical_len);
  
  if (status == INTL_TAG_OOM) return js_mkerr(js, "oom");
  if (status == INTL_TAG_INVALID)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid language tag: %.*s", (int)len, tag ? tag : "");

  ant_value_t result = js_mkstr(js, canonical, canonical_len);
  free(canonical);
  
  return result;
}

static ant_value_t intl_locale_from_list_entry(ant_t *js, ant_value_t input) {
  if (vtype(input) != kTypeString && !is_object_type(input))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Language ID should be string or object.");

  ant_value_t locale = js_tostring_val(js, input);
  if (is_err(locale)) return locale;
  return intl_canonicalize_tag(js, locale);
}

static bool intl_list_has_tag(ant_t *js, ant_value_t list, ant_value_t tag) {
  size_t tag_len = 0;
  const char *tag_str = js_getstr(js, tag, &tag_len);
  ant_offset_t count = js_arr_len(js, list);
  
  for (ant_offset_t i = 0; i < count; i++) {
    size_t prev_len = 0;
    const char *prev = js_getstr(js, js_arr_get(js, list, i), &prev_len);
    if (prev_len == tag_len && memcmp(prev, tag_str, tag_len) == 0) return true;
  }
  
  return false;
}

static ant_value_t intl_canonical_locale_list_fill(ant_t *js, ant_value_t out, ant_value_t locales) {
  if (vtype(locales) == kTypeString) {
    ant_value_t tag = intl_canonicalize_tag(js, locales);
    if (is_err(tag)) return tag;
    js_arr_push(js, out, tag);
    return out;
  }
  if (!is_object_type(locales)) return out;

  ant_value_t len_val = js_get(js, locales, "length");
  if (is_err(len_val)) return len_val;
  
  ant_value_t len_num = vtype(len_val) == kTypeUndefined ? Ant_Value_FromNumber(0) : js_to_numeric(js, len_val);
  if (is_err(len_num)) return len_num;
  
  if (vtype(len_num) == kTypeBigInt) 
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a BigInt value to a number");
  double len = Ant_Value_AsNumberUnchecked(len_num);

  for (double k = 0; k < len; k++) {
    ant_value_t present = do_in(js, Ant_Value_FromNumber(k), locales);
    if (is_err(present)) return present;
    if (present != Ant_Value_True) continue;

    char idx[32];
    snprintf(idx, sizeof(idx), "%.0f", k);
    ant_value_t entry = js_get(js, locales, idx);
    if (is_err(entry)) return entry;
    
    ant_value_t tag = intl_locale_from_list_entry(js, entry);
    if (is_err(tag)) return tag;
    if (!intl_list_has_tag(js, out, tag)) js_arr_push(js, out, tag);
  }

  return out;
}

static ant_value_t intl_canonical_locale_list(ant_t *js, ant_value_t locales) {
  if (vtype(locales) == kTypeNull)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert undefined or null to object");

  ant_value_t out = Ant_Array_Create(js);
  if (is_err(out) || vtype(locales) == kTypeUndefined) return out;

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, out);
  ant_value_t result = intl_canonical_locale_list_fill(js, out, locales);
  GC_ROOT_RESTORE(js, mark);
  
  return is_err(result) ? result : out;
}

static ant_value_t intl_get_canonical_locales(ant_params_t) {
  return intl_canonical_locale_list(js, nargs > 0 ? args[0] : Ant_Value_Undefined);
}

static ant_value_t intl_resolve_locale(ant_t *js, ant_value_t input) {
  if (vtype(input) == kTypeUndefined) return intl_default_locale(js);
  if (vtype(input) == kTypeString) return intl_canonicalize_tag(js, input);

  ant_value_t list = intl_canonical_locale_list(js, input);
  if (is_err(list)) return list;
  return js_arr_len(js, list) > 0 ? js_arr_get(js, list, 0) : intl_default_locale(js);
}

static ant_value_t intl_get_option_string(ant_t *js, ant_value_t options, const char *key, const char *fallback) {
  if (vtype(options) != kTypeObject) return js_mkstr(js, fallback, strlen(fallback));

  ant_value_t value = js_get(js, options, key);
  if (vtype(value) == kTypeUndefined) return js_mkstr(js, fallback, strlen(fallback));

  ant_value_t str = js_tostring_val(js, value);
  if (is_err(str)) return str;

  size_t len = 0;
  const char *ptr = js_getstr(js, str, &len);
  if (!ptr || len == 0) return js_mkstr(js, fallback, strlen(fallback));
  
  return str;
}

static ant_value_t intl_collator_compare(ant_params_t) {
  ant_value_t left = js_tostring_val(js, nargs > 0 ? args[0] : js_mkstr(js, "", 0));
  if (is_err(left)) return left;

  ant_value_t right = js_tostring_val(js, nargs > 1 ? args[1] : js_mkstr(js, "", 0));
  if (is_err(right)) return right;

  const char *left_str = js_getstr(js, left, NULL);
  const char *right_str = js_getstr(js, right, NULL);

  int result = strcoll(left_str ? left_str : "", right_str ? right_str : "");
  if (result < 0) return Ant_Value_FromNumber(-1);
  if (result > 0) return Ant_Value_FromNumber(1);
  
  return Ant_Value_FromNumber(0);
}

static ant_value_t intl_collator_compare_numeric(ant_params_t) {
  ant_value_t left = js_tostring_val(js, nargs > 0 ? args[0] : js_mkstr(js, "", 0));
  if (is_err(left)) return left;

  ant_value_t right = js_tostring_val(js, nargs > 1 ? args[1] : js_mkstr(js, "", 0));
  if (is_err(right)) return right;

  size_t left_len = 0;
  size_t right_len = 0;
  const char *left_str = js_getstr(js, left, &left_len);
  const char *right_str = js_getstr(js, right, &right_len);
  if (!left_str) left_str = "";
  if (!right_str) right_str = "";

  size_t i = 0;
  size_t j = 0;
  
  while (i < left_len && j < right_len) {
    char lc = left_str[i];
    char rc = right_str[j];

    if (ant_ascii_is_digit(lc) && ant_ascii_is_digit(rc)) {
      size_t left_run = i;
      size_t right_run = j;
      while (left_run < left_len && left_str[left_run] == '0') left_run++;
      while (right_run < right_len && right_str[right_run] == '0') right_run++;

      size_t left_end = left_run;
      size_t right_end = right_run;
      while (left_end < left_len && ant_ascii_is_digit(left_str[left_end])) left_end++;
      while (right_end < right_len && ant_ascii_is_digit(right_str[right_end])) right_end++;

      size_t left_digits = left_end - left_run;
      size_t right_digits = right_end - right_run;
      if (left_digits != right_digits) return Ant_Value_FromNumber(left_digits < right_digits ? -1 : 1);

      int cmp = left_digits > 0 ? memcmp(left_str + left_run, right_str + right_run, left_digits) : 0;
      if (cmp != 0) return Ant_Value_FromNumber(cmp < 0 ? -1 : 1);

      while (i < left_end && ant_ascii_is_digit(left_str[i])) i++;
      while (j < right_end && ant_ascii_is_digit(right_str[j])) j++;
      continue;
    }

    if (lc != rc) return Ant_Value_FromNumber((unsigned char)lc < (unsigned char)rc ? -1 : 1);
    i++;
    j++;
  }

  if (i < left_len) return Ant_Value_FromNumber(1);
  if (j < right_len) return Ant_Value_FromNumber(-1);
  return Ant_Value_FromNumber(0);
}

static ant_value_t intl_collator_resolved_options(ant_params_t) {
  ant_value_t obj = Ant_Object_Create(js);
  
  ant_value_t locale = is_object_type(call_this) 
    ? js_get(js, call_this, "locale") 
    : Ant_Value_Undefined;
  
  if (vtype(locale) != kTypeString) locale = js_mkstr(js, "en-US", 5);
  js_set(js, obj, "locale", locale);
  
  return obj;
}

static ant_value_t intl_numberformat_format(ant_params_t) {
  double number = nargs > 0 ? js_to_number(js, args[0]) : 0.0;
  ant_value_t raw_val = js_tostring_val(js, Ant_Value_FromNumber(number));
  if (is_err(raw_val)) return raw_val;

  size_t raw_len = 0;
  const char *raw = js_getstr(js, raw_val, &raw_len);
  if (!raw || raw_len == 0) return js_mkstr(js, "0", 1);

  if (
    !isfinite(number) ||
    memchr(raw, 'e', raw_len) || 
    memchr(raw, 'E', raw_len)
  ) return raw_val;

  const char *dot = memchr(raw, '.', raw_len);
  size_t int_len = dot ? (size_t)(dot - raw) : raw_len;
  size_t start = raw[0] == '-' ? 1 : 0;
  size_t frac_len = dot ? (raw_len - int_len) : 0;

  char buf[128];
  size_t pos = 0;
  if (start) buf[pos++] = '-';

  for (size_t i = start; i < int_len; i++) {
    buf[pos++] = raw[i];
    size_t remaining = int_len - 1 - i;
    if (remaining > 0 && remaining % 3 == 0) buf[pos++] = ',';
  }

  if (dot && frac_len > 0) {
    memcpy(buf + pos, dot, frac_len);
    pos += frac_len;
  }

  buf[pos] = '\0';
  return js_mkstr(js, buf, pos);
}

static ant_value_t intl_numberformat_resolved_options(ant_params_t) {
  return intl_collator_resolved_options(js, args, nargs, Ant_Value_Undefined, call_this, call_func);
}

static void intl_dtf_extract_fields(ant_native_params_t, intl_dtf_fields_t *out) {
  time_t t = time(NULL);
  if (nargs >= 1) t = (time_t)(js_to_number(js, args[0]) / 1000.0);

  struct tm local;
#ifdef _WIN32
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif

  out->hour12 = local.tm_hour % 12;
  if (out->hour12 == 0) out->hour12 = 12;
  out->minute = local.tm_min;
  out->second = local.tm_sec;
  out->day_period = local.tm_hour < 12 ? "AM" : "PM";
}

static ant_value_t intl_dtf_format(ant_params_t) {
  intl_dtf_fields_t fields;
  intl_dtf_extract_fields(js, args, nargs, &fields);

  char buf[64];
  snprintf(
    buf, sizeof(buf), "%d:%02d:%02d %s",
    fields.hour12, fields.minute, fields.second, fields.day_period
  );
  
  return js_mkstr(js, buf, strlen(buf));
}

static ant_value_t intl_dtf_resolved_options(ant_params_t) {
  ant_value_t obj = Ant_Object_Create(js);
  
  ant_value_t locale = is_object_type(call_this) ? js_get(js, call_this, "locale") : Ant_Value_Undefined;
  ant_value_t time_zone = is_object_type(call_this) ? js_get(js, call_this, "timeZone") : Ant_Value_Undefined;

  if (vtype(locale) != kTypeString) locale = js_mkstr(js, "en-US", 5);
  if (vtype(time_zone) != kTypeString) time_zone = js_mkstr(js, "UTC", 3);

  js_set(js, obj, "locale", locale);
  js_set(js, obj, "timeZone", time_zone);
  
  return obj;
}

static ant_value_t intl_dtf_make_part(ant_t *js, const char *type, const char *value) {
  ant_value_t obj = Ant_Object_Create(js);
  js_set(js, obj, "type", js_mkstr(js, type, strlen(type)));
  js_set(js, obj, "value", js_mkstr(js, value, strlen(value)));
  return obj;
}

static ant_value_t intl_dtf_format_to_parts(ant_params_t) {
  intl_dtf_fields_t fields;
  intl_dtf_extract_fields(js, args, nargs, &fields);

  char hour[8];
  char minute[8];
  char second[8];
  
  snprintf(hour, sizeof(hour), "%d", fields.hour12);
  snprintf(minute, sizeof(minute), "%02d", fields.minute);
  snprintf(second, sizeof(second), "%02d", fields.second);

  ant_value_t parts = Ant_Array_Create(js);
  js_arr_push(js, parts, intl_dtf_make_part(js, "hour", hour));
  js_arr_push(js, parts, intl_dtf_make_part(js, "literal", ":"));
  js_arr_push(js, parts, intl_dtf_make_part(js, "minute", minute));
  js_arr_push(js, parts, intl_dtf_make_part(js, "literal", ":"));
  js_arr_push(js, parts, intl_dtf_make_part(js, "second", second));
  js_arr_push(js, parts, intl_dtf_make_part(js, "literal", " "));
  js_arr_push(js, parts, intl_dtf_make_part(js, "dayPeriod", fields.day_period));
  
  return parts;
}

static size_t intl_utf8_segment_len(const char *input, size_t remaining) {
  if (remaining == 0) return 0;

  const unsigned char *s = (const unsigned char *)input;
  unsigned char c = s[0];
  size_t len = 1;

  if ((c & 0x80) == 0) return 1;
  if ((c & 0xe0) == 0xc0) len = 2;
  else if ((c & 0xf0) == 0xe0) len = 3;
  else if ((c & 0xf8) == 0xf0) len = 4;

  if (len > remaining) return 1;
  for (size_t i = 1; i < len; i++) if ((s[i] & 0xc0) != 0x80) return 1;

  return len;
}

static bool intl_ascii_is_word_byte(const char *segment, size_t len) {
  if (len != 1) return true;

  unsigned char c = (unsigned char)segment[0];
  return ant_ascii_is_alnum(c) || c == '_';
}

static const char *intl_segmenter_granularity(ant_t *js, ant_value_t segmenter, size_t *len) {
  ant_value_t granularity = js_get(js, segmenter, "granularity");
  if (vtype(granularity) != kTypeString) {
    if (len) *len = 8;
    return "grapheme";
  }

  return js_getstr(js, granularity, len);
}

static ant_value_t intl_segmenter_segment(ant_params_t) {
  ant_value_t input = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "", 0);
  if (is_err(input)) return input;

  size_t input_len = 0;
  char *input_str = js_getstr(js, input, &input_len);
  ant_value_t segments = Ant_Array_Create(js);

  size_t granularity_len = 0;
  const char *granularity = intl_segmenter_granularity(js, call_this, &granularity_len);
  bool word_granularity = granularity_len == 4 && memcmp(granularity, "word", 4) == 0;

  for (size_t offset = 0; offset < input_len;) {
    size_t segment_len = intl_utf8_segment_len(input_str + offset, input_len - offset);
    ant_value_t record = Ant_Object_Create(js);
    
    js_set(js, record, "segment", js_mkstr(js, input_str + offset, segment_len));
    js_set(js, record, "index", Ant_Value_FromNumber((double)offset));
    js_set(js, record, "input", input);
    
    if (word_granularity) js_set(
      js, record, "isWordLike",
      Ant_Value_FromBool(intl_ascii_is_word_byte(input_str + offset, segment_len))
    );
    
    js_arr_push(js, segments, record);
    offset += segment_len;
  }

  return segments;
}

static ant_value_t intl_segmenter_resolved_options(ant_params_t) {
  ant_value_t obj = Ant_Object_Create(js);

  size_t granularity_len = 0;
  const char *granularity = intl_segmenter_granularity(js, call_this, &granularity_len);

  ant_value_t locale = is_object_type(call_this) ? js_get(js, call_this, "locale") : Ant_Value_Undefined;
  if (vtype(locale) != kTypeString) locale = js_mkstr(js, "en-US", 5);

  js_set(js, obj, "locale", locale);
  js_set(js, obj, "granularity", js_mkstr(js, granularity, granularity_len));
  
  return obj;
}

static ant_value_t intl_collator_constructor(ant_params_t) {
  ant_value_t locale = intl_resolve_locale(js, nargs > 0 ? args[0] : Ant_Value_Undefined);
  if (is_err(locale)) return locale;

  ant_value_t obj = intl_create_instance(js, js->builtins.intl_collator_proto, call_new_target);
  defhidden(js, obj, "locale", 6, locale);

  if (nargs > 1 && vtype(args[1]) == kTypeObject && js_truthy(js, js_get(js, args[1], "numeric"))) {
    defhidden(js, obj, "compare", 7, Ant_Function_Create(intl_collator_compare_numeric));
    defhidden(js, obj, "numeric", 7, Ant_Value_True);
  }
  
  return obj;
}

static ant_value_t intl_numberformat_constructor(ant_params_t) {
  ant_value_t locale = intl_resolve_locale(js, nargs > 0 ? args[0] : Ant_Value_Undefined);
  if (is_err(locale)) return locale;

  ant_value_t obj = intl_create_instance(js, js->builtins.intl_numberformat_proto, call_new_target);
  defhidden(js, obj, "locale", 6, locale);
  
  return obj;
}

static ant_value_t intl_dtf_constructor(ant_params_t) {
  ant_value_t locale = intl_resolve_locale(js, nargs > 0 ? args[0] : Ant_Value_Undefined);
  if (is_err(locale)) return locale;

  ant_value_t time_zone = intl_get_option_string(
    js, nargs > 1 ? args[1] : Ant_Value_Undefined,
    "timeZone", "UTC"
  );
  if (is_err(time_zone)) return time_zone;

  ant_value_t obj = intl_create_instance(js, js->builtins.intl_datetimeformat_proto, call_new_target);
  defhidden(js, obj, "locale", 6, locale);
  defhidden(js, obj, "timeZone", 8, time_zone);
  
  return obj;
}

static ant_value_t intl_segmenter_constructor(ant_params_t) {
  ant_value_t locale = intl_resolve_locale(js, nargs > 0 ? args[0] : Ant_Value_Undefined);
  if (is_err(locale)) return locale;

  ant_value_t granularity = intl_get_option_string(
    js, nargs > 1 ? args[1] : Ant_Value_Undefined,
    "granularity", "grapheme"
  );
  if (is_err(granularity)) return granularity;

  ant_value_t obj = intl_create_instance(js, js->builtins.intl_segmenter_proto, call_new_target);
  defhidden(js, obj, "locale", 6, locale);
  defhidden(js, obj, "granularity", 11, granularity);
  
  return obj;
}

void init_intl_module(ant_t *js) {
  ant_value_t intl = Ant_Object_CreateBare(js);
  ant_value_t object_proto = js->sym.object_proto;

  if (is_object_type(object_proto)) js_set_proto_init(intl, object_proto);
  js_set_tostringtag(js, intl, js_mkstr(js, "Intl", 4));

  js->builtins.intl_collator_proto = Ant_Object_Create(js);
  defmethod(js, js->builtins.intl_collator_proto, "compare", 7, Ant_Function_Create(intl_collator_compare));
  defmethod(js, js->builtins.intl_collator_proto, "resolvedOptions", 15, Ant_Function_Create(intl_collator_resolved_options));
  js_set_tostringtag(js, js->builtins.intl_collator_proto, js_mkstr(js, "Intl.Collator", 13));
  ant_value_t collator_ctor = Ant_Function_CreateIntrinsicConstructor(js, intl_collator_constructor, js->builtins.intl_collator_proto, "Collator", 0);
  defhidden(js, intl, "Collator", 8, collator_ctor);

  js->builtins.intl_numberformat_proto = Ant_Object_Create(js);
  defmethod(js, js->builtins.intl_numberformat_proto, "format", 6, Ant_Function_Create(intl_numberformat_format));
  defmethod(js, js->builtins.intl_numberformat_proto, "resolvedOptions", 15, Ant_Function_Create(intl_numberformat_resolved_options));
  js_set_tostringtag(js, js->builtins.intl_numberformat_proto, js_mkstr(js, "Intl.NumberFormat", 17));
  ant_value_t numberformat_ctor = Ant_Function_CreateIntrinsicConstructor(js, intl_numberformat_constructor, js->builtins.intl_numberformat_proto, "NumberFormat", 0);
  defhidden(js, intl, "NumberFormat", 12, numberformat_ctor);

  js->builtins.intl_datetimeformat_proto = Ant_Object_Create(js);
  defmethod(js, js->builtins.intl_datetimeformat_proto, "format", 6, Ant_Function_Create(intl_dtf_format));
  defmethod(js, js->builtins.intl_datetimeformat_proto, "resolvedOptions", 15, Ant_Function_Create(intl_dtf_resolved_options));
  defmethod(js, js->builtins.intl_datetimeformat_proto, "formatToParts", 13, Ant_Function_CreateWithArity(intl_dtf_format_to_parts, 1));
  js_set_tostringtag(js, js->builtins.intl_datetimeformat_proto, js_mkstr(js, "Intl.DateTimeFormat", 19));
  ant_value_t dtf_ctor = Ant_Function_CreateIntrinsicConstructor(js, intl_dtf_constructor, js->builtins.intl_datetimeformat_proto, "DateTimeFormat", 0);
  defhidden(js, intl, "DateTimeFormat", 14, dtf_ctor);

  js->builtins.intl_segmenter_proto = Ant_Object_Create(js);
  defmethod(js, js->builtins.intl_segmenter_proto, "segment", 7, Ant_Function_CreateWithArity(intl_segmenter_segment, 1));
  defmethod(js, js->builtins.intl_segmenter_proto, "resolvedOptions", 15, Ant_Function_Create(intl_segmenter_resolved_options));
  js_set_tostringtag(js, js->builtins.intl_segmenter_proto, js_mkstr(js, "Intl.Segmenter", 14));
  ant_value_t segmenter_ctor = Ant_Function_CreateIntrinsicConstructor(js, intl_segmenter_constructor, js->builtins.intl_segmenter_proto, "Segmenter", 0);
  defhidden(js, intl, "Segmenter", 9, segmenter_ctor);

  ant_value_t locale_ctors[] = { collator_ctor, numberformat_ctor, dtf_ctor, segmenter_ctor };
  for (size_t i = 0; i < sizeof(locale_ctors) / sizeof(locale_ctors[0]); i++)
    defmethod(js, locale_ctors[i], "supportedLocalesOf", 18, Ant_Function_CreateWithArity(intl_get_canonical_locales, 1));
  defmethod(js, intl, "getCanonicalLocales", 19, Ant_Function_CreateWithArity(intl_get_canonical_locales, 1));

  js_set_global_builtin(js, "Intl", intl);
}
