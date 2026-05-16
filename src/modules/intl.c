#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "descriptors.h"

#include "modules/intl.h"
#include "modules/symbol.h"


typedef struct {
  int hour12;
  int minute;
  int second;
  const char *day_period;
} intl_dtf_fields_t;

static ant_value_t intl_create_instance(ant_t *js, ant_value_t fallback_proto) {
  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, fallback_proto);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  return obj;
}

// TODO: docs/exec-plans/tech-debt.md
static inline bool intl_ascii_is_alpha(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static inline bool intl_ascii_is_digit(char c) {
  return c >= '0' && c <= '9';
}

static inline bool intl_ascii_is_alnum(char c) {
  return intl_ascii_is_alpha(c) || intl_ascii_is_digit(c);
}

static inline char intl_ascii_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static bool intl_ascii_all(const char *s, size_t len, bool (*pred)(char)) {
  if (!s || len == 0) return false;
  for (size_t i = 0; i < len; i++) if (!pred(s[i])) return false;
  return true;
}

static bool intl_is_valid_language_tag(const char *tag, size_t len) {
  if (!tag || len == 0) return false;

  size_t start = 0;
  size_t end = 0;
  while (end < len && tag[end] != '-') end++;

  size_t first_len = end - start;
  if (first_len < 2 || first_len > 8) return false;
  if (!intl_ascii_all(tag, first_len, intl_ascii_is_alpha)) return false;

  bool need_extension_subtag = false;
  bool in_private_use = false;
  bool saw_private_use_subtag = false;

  while (end < len) {
    start = end + 1;
    if (start >= len) return false;
    
    end = start;
    while (end < len && tag[end] != '-') end++;
    
    size_t subtag_len = end - start;
    if (subtag_len == 0 || subtag_len > 8) return false;
    
    const char *subtag = tag + start;
    if (!intl_ascii_all(subtag, subtag_len, intl_ascii_is_alnum)) return false;
    
    if (in_private_use) {
      saw_private_use_subtag = true;
      continue;
    }
    
    if (need_extension_subtag) {
      if (subtag_len < 2) return false;
      need_extension_subtag = false;
      continue;
    }

    if (subtag_len == 1) {
      char singleton = intl_ascii_lower(subtag[0]);
      if (singleton == 'x') in_private_use = true;
      else need_extension_subtag = true;
    }
  }

  if (need_extension_subtag) return false;
  if (in_private_use && !saw_private_use_subtag) return false;
  
  return true;
}

static ant_value_t intl_resolve_locale(ant_t *js, ant_value_t input) {
  if (vtype(input) == T_ARR) input = js_get(js, input, "0");
  if (vtype(input) == T_UNDEF) return js_mkstr(js, "en-US", 5);

  ant_value_t locale = js_tostring_val(js, input);
  if (is_err(locale)) return locale;

  size_t len = 0;
  const char *tag = js_getstr(js, locale, &len);
  if (!intl_is_valid_language_tag(tag, len))
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid language tag");

  return locale;
}

static ant_value_t intl_get_option_string(ant_t *js, ant_value_t options, const char *key, const char *fallback) {
  if (vtype(options) != T_OBJ) return js_mkstr(js, fallback, strlen(fallback));

  ant_value_t value = js_get(js, options, key);
  if (vtype(value) == T_UNDEF) return js_mkstr(js, fallback, strlen(fallback));

  ant_value_t str = js_tostring_val(js, value);
  if (is_err(str)) return str;

  size_t len = 0;
  const char *ptr = js_getstr(js, str, &len);
  if (!ptr || len == 0) return js_mkstr(js, fallback, strlen(fallback));
  
  return str;
}

static ant_value_t intl_collator_compare(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t left = js_tostring_val(js, nargs > 0 ? args[0] : js_mkstr(js, "", 0));
  if (is_err(left)) return left;

  ant_value_t right = js_tostring_val(js, nargs > 1 ? args[1] : js_mkstr(js, "", 0));
  if (is_err(right)) return right;

  const char *left_str = js_getstr(js, left, NULL);
  const char *right_str = js_getstr(js, right, NULL);

  int result = strcoll(left_str ? left_str : "", right_str ? right_str : "");
  if (result < 0) return js_mknum(-1);
  if (result > 0) return js_mknum(1);
  
  return js_mknum(0);
}

static ant_value_t intl_collator_resolved_options(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t obj = js_mkobj(js);
  ant_value_t this_obj = js_getthis(js);
  
  ant_value_t locale = is_object_type(this_obj) 
    ? js_get(js, this_obj, "locale") 
    : js_mkundef();
  
  if (vtype(locale) != T_STR) locale = js_mkstr(js, "en-US", 5);
  js_set(js, obj, "locale", locale);
  
  return obj;
}

static ant_value_t intl_numberformat_format(ant_t *js, ant_value_t *args, int nargs) {
  double number = nargs > 0 ? js_to_number(js, args[0]) : 0.0;
  ant_value_t raw_val = js_tostring_val(js, js_mknum(number));
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

static ant_value_t intl_numberformat_resolved_options(ant_t *js, ant_value_t *args, int nargs) {
  return intl_collator_resolved_options(js, args, nargs);
}

static void intl_dtf_extract_fields(ant_t *js, ant_value_t *args, int nargs, intl_dtf_fields_t *out) {
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

static ant_value_t intl_dtf_format(ant_t *js, ant_value_t *args, int nargs) {
  intl_dtf_fields_t fields;
  intl_dtf_extract_fields(js, args, nargs, &fields);

  char buf[64];
  snprintf(
    buf, sizeof(buf), "%d:%02d:%02d %s",
    fields.hour12, fields.minute, fields.second, fields.day_period
  );
  
  return js_mkstr(js, buf, strlen(buf));
}

static ant_value_t intl_dtf_resolved_options(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t obj = js_mkobj(js);
  ant_value_t this_obj = js_getthis(js);
  
  ant_value_t locale = is_object_type(this_obj) ? js_get(js, this_obj, "locale") : js_mkundef();
  ant_value_t time_zone = is_object_type(this_obj) ? js_get(js, this_obj, "timeZone") : js_mkundef();

  if (vtype(locale) != T_STR) locale = js_mkstr(js, "en-US", 5);
  if (vtype(time_zone) != T_STR) time_zone = js_mkstr(js, "UTC", 3);

  js_set(js, obj, "locale", locale);
  js_set(js, obj, "timeZone", time_zone);
  
  return obj;
}

static ant_value_t intl_dtf_make_part(ant_t *js, const char *type, const char *value) {
  ant_value_t obj = js_mkobj(js);
  js_set(js, obj, "type", js_mkstr(js, type, strlen(type)));
  js_set(js, obj, "value", js_mkstr(js, value, strlen(value)));
  return obj;
}

static ant_value_t intl_dtf_format_to_parts(ant_t *js, ant_value_t *args, int nargs) {
  intl_dtf_fields_t fields;
  intl_dtf_extract_fields(js, args, nargs, &fields);

  char hour[8];
  char minute[8];
  char second[8];
  
  snprintf(hour, sizeof(hour), "%d", fields.hour12);
  snprintf(minute, sizeof(minute), "%02d", fields.minute);
  snprintf(second, sizeof(second), "%02d", fields.second);

  ant_value_t parts = js_mkarr(js);
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
  return
    (c >= '0' && c <= '9') ||
    (c >= 'A' && c <= 'Z') ||
    (c >= 'a' && c <= 'z') ||
    c == '_';
}

static const char *intl_segmenter_granularity(ant_t *js, ant_value_t segmenter, size_t *len) {
  ant_value_t granularity = js_get(js, segmenter, "granularity");
  if (vtype(granularity) != T_STR) {
    if (len) *len = 8;
    return "grapheme";
  }

  return js_getstr(js, granularity, len);
}

static ant_value_t intl_segmenter_segment(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t input = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "", 0);
  if (is_err(input)) return input;

  size_t input_len = 0;
  char *input_str = js_getstr(js, input, &input_len);
  ant_value_t segments = js_mkarr(js);

  ant_value_t this_obj = js_getthis(js);
  size_t granularity_len = 0;
  const char *granularity = intl_segmenter_granularity(js, this_obj, &granularity_len);
  bool word_granularity = granularity_len == 4 && memcmp(granularity, "word", 4) == 0;

  for (size_t offset = 0; offset < input_len;) {
    size_t segment_len = intl_utf8_segment_len(input_str + offset, input_len - offset);
    ant_value_t record = js_mkobj(js);
    
    js_set(js, record, "segment", js_mkstr(js, input_str + offset, segment_len));
    js_set(js, record, "index", js_mknum((double)offset));
    js_set(js, record, "input", input);
    
    if (word_granularity) js_set(
      js, record, "isWordLike",
      js_bool(intl_ascii_is_word_byte(input_str + offset, segment_len))
    );
    
    js_arr_push(js, segments, record);
    offset += segment_len;
  }

  return segments;
}

static ant_value_t intl_segmenter_resolved_options(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t obj = js_mkobj(js);
  ant_value_t this_obj = js_getthis(js);

  size_t granularity_len = 0;
  const char *granularity = intl_segmenter_granularity(js, this_obj, &granularity_len);

  ant_value_t locale = is_object_type(this_obj) ? js_get(js, this_obj, "locale") : js_mkundef();
  if (vtype(locale) != T_STR) locale = js_mkstr(js, "en-US", 5);

  js_set(js, obj, "locale", locale);
  js_set(js, obj, "granularity", js_mkstr(js, granularity, granularity_len));
  
  return obj;
}

static ant_value_t intl_collator_constructor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t locale = intl_resolve_locale(js, nargs > 0 ? args[0] : js_mkundef());
  if (is_err(locale)) return locale;

  ant_value_t obj = intl_create_instance(js, js->sym.intl_collator_proto);
  js_set(js, obj, "locale", locale);
  
  return obj;
}

static ant_value_t intl_numberformat_constructor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t locale = intl_resolve_locale(js, nargs > 0 ? args[0] : js_mkundef());
  if (is_err(locale)) return locale;

  ant_value_t obj = intl_create_instance(js, js->sym.intl_numberformat_proto);
  js_set(js, obj, "locale", locale);
  
  return obj;
}

static ant_value_t intl_dtf_constructor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t locale = intl_resolve_locale(js, nargs > 0 ? args[0] : js_mkundef());
  if (is_err(locale)) return locale;

  ant_value_t time_zone = intl_get_option_string(
    js, nargs > 1 ? args[1] : js_mkundef(),
    "timeZone", "UTC"
  );
  if (is_err(time_zone)) return time_zone;

  ant_value_t obj = intl_create_instance(js, js->sym.intl_datetimeformat_proto);
  js_set(js, obj, "locale", locale);
  js_set(js, obj, "timeZone", time_zone);
  
  return obj;
}

static ant_value_t intl_segmenter_constructor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t locale = intl_resolve_locale(js, nargs > 0 ? args[0] : js_mkundef());
  if (is_err(locale)) return locale;

  ant_value_t granularity = intl_get_option_string(
    js, nargs > 1 ? args[1] : js_mkundef(),
    "granularity", "grapheme"
  );
  if (is_err(granularity)) return granularity;

  ant_value_t obj = intl_create_instance(js, js->sym.intl_segmenter_proto);
  js_set(js, obj, "locale", locale);
  js_set(js, obj, "granularity", granularity);
  
  return obj;
}

void init_intl_module(void) {
  ant_t *js = rt->js;
  
  ant_value_t global = js_glob(js);
  ant_value_t intl = js_mkobj(js);
  ant_value_t object_proto = js->sym.object_proto;

  if (is_object_type(object_proto)) js_set_proto_init(intl, object_proto);
  js_set_sym(js, intl, get_toStringTag_sym(), js_mkstr(js, "Intl", 4));

  js->sym.intl_collator_proto = js_mkobj(js);
  js_set(js, js->sym.intl_collator_proto, "compare", js_mkfun(intl_collator_compare));
  js_set(js, js->sym.intl_collator_proto, "resolvedOptions", js_mkfun(intl_collator_resolved_options));
  js_set_sym(js, js->sym.intl_collator_proto, get_toStringTag_sym(), js_mkstr(js, "Intl.Collator", 13));
  ant_value_t collator_ctor = js_make_ctor(js, intl_collator_constructor, js->sym.intl_collator_proto, "Collator", 8);
  js_set(js, intl, "Collator", collator_ctor);

  js->sym.intl_numberformat_proto = js_mkobj(js);
  js_set(js, js->sym.intl_numberformat_proto, "format", js_mkfun(intl_numberformat_format));
  js_set(js, js->sym.intl_numberformat_proto, "resolvedOptions", js_mkfun(intl_numberformat_resolved_options));
  js_set_sym(js, js->sym.intl_numberformat_proto, get_toStringTag_sym(), js_mkstr(js, "Intl.NumberFormat", 17));
  ant_value_t numberformat_ctor = js_make_ctor(js, intl_numberformat_constructor, js->sym.intl_numberformat_proto, "NumberFormat", 12);
  js_set(js, intl, "NumberFormat", numberformat_ctor);

  js->sym.intl_datetimeformat_proto = js_mkobj(js);
  js_set(js, js->sym.intl_datetimeformat_proto, "format", js_mkfun(intl_dtf_format));
  js_set(js, js->sym.intl_datetimeformat_proto, "resolvedOptions", js_mkfun(intl_dtf_resolved_options));
  js_set(js, js->sym.intl_datetimeformat_proto, "formatToParts", js_mkfun(intl_dtf_format_to_parts));
  js_set_sym(js, js->sym.intl_datetimeformat_proto, get_toStringTag_sym(), js_mkstr(js, "Intl.DateTimeFormat", 19));
  ant_value_t dtf_ctor = js_make_ctor(js, intl_dtf_constructor, js->sym.intl_datetimeformat_proto, "DateTimeFormat", 14);
  js_set(js, intl, "DateTimeFormat", dtf_ctor);

  js->sym.intl_segmenter_proto = js_mkobj(js);
  js_set(js, js->sym.intl_segmenter_proto, "segment", js_mkfun(intl_segmenter_segment));
  js_set(js, js->sym.intl_segmenter_proto, "resolvedOptions", js_mkfun(intl_segmenter_resolved_options));
  js_set_sym(js, js->sym.intl_segmenter_proto, get_toStringTag_sym(), js_mkstr(js, "Intl.Segmenter", 14));
  ant_value_t segmenter_ctor = js_make_ctor(js, intl_segmenter_constructor, js->sym.intl_segmenter_proto, "Segmenter", 9);
  js_set(js, intl, "Segmenter", segmenter_ctor);

  js_set(js, global, "Intl", intl);
  js_set_descriptor(js, global, "Intl", 4, JS_DESC_W | JS_DESC_C);
}
