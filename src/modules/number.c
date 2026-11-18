#include <compat.h> // IWYU pragma: keep
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "cfunc.h"
#include "proto.h"
#include "numbers.h"
#include "silver/lexer.h"
#include "modules/number.h"
#include "modules/bigint.h"
#include "modules/intl.h"
#include "modules/string.h"

typedef struct {
  char raw[64];
  const char *digits;
  int count;
  int point;
} number_decimal_t;

static void number_decimal_parse(number_decimal_t *out, int raw_len) {
  char *raw = out->raw + 1;
  int count = 0, point = -1, exponent = 0;
  int i = 0;

  for (; i < raw_len; i++) {
    char c = raw[i];
    if (c == '.') { point = count; continue; }
    if (c == 'e' || c == 'E') { exponent = atoi(raw + i + 1); break; }
    raw[count++] = c;
  }
  if (point < 0) point = count;

  int lead = 0;
  while (lead < count && raw[lead] == '0') lead++;

  out->digits = raw + lead;
  out->count = count - lead;
  out->point = out->count ? point - lead + exponent : 0;
}

static void number_decimal_round(number_decimal_t *value, int max_fraction) {
  char *digits = (char *)value->digits;
  int keep = value->point + max_fraction;
  
  if (keep < value->count) {
    bool up = keep >= 0 && digits[keep] >= '5';
    value->count = keep > 0 ? keep : 0;
    if (up) {
      int at = value->count - 1;
      while (at >= 0 && digits[at] == '9') digits[at--] = '0';
      if (at >= 0) digits[at]++;
      else {
        digits--;
        digits[0] = '1';
        value->digits = digits;
        value->count++;
        value->point++;
      }
    }
  }

  while (value->count > 0 && value->count > value->point && digits[value->count - 1] == '0') value->count--;
  if (value->count == 0) value->point = 0;
}

size_t Ant_Number_FormatLocale(double value, const ant_number_locale_t *format, char *buf, size_t capacity) {
  static const char infinity[] = "\xe2\x88\x9e";
  bool negative = signbit(value) && !isnan(value);
  size_t pos = 0;

  if (__builtin_expect(!isfinite(value), 0)) {
    const char *text = isnan(value) ? "NaN" : infinity;
    size_t text_len = strlen(text);
    if (negative + text_len + format->percent > capacity) return 0;
    if (negative) buf[pos++] = '-';
    memcpy(buf + pos, text, text_len);
    pos += text_len;
    if (format->percent) buf[pos++] = '%';
    return pos;
  }

  number_decimal_t decimal;
  char *raw = decimal.raw + 1;
  size_t raw_len = js_number_to_chars(Ant_Value_FromNumber(fabs(value)), raw, sizeof(decimal.raw) - 1);
  bool plain = format->min_integer == 1 && !format->min_fraction && !format->percent && !format->ungrouped;

  if (plain && !memchr(raw, 'e', raw_len)) {
    char *dot = memchr(raw, '.', raw_len);
    size_t int_len = dot ? (size_t)(dot - raw) : raw_len;
    size_t frac_len = raw_len - int_len;
    
    if (frac_len <= (size_t)format->max_fraction + 1 && (frac_len || format->max_fraction)) {
      size_t needed = negative + int_len + (int_len - 1) / 3 + frac_len;
      if (needed > capacity) return 0;
      if (negative) buf[pos++] = '-';
      
      for (size_t i = 0; i < int_len; i++) {
        buf[pos++] = raw[i];
        size_t remaining = int_len - 1 - i;
        if (remaining > 0 && remaining % 3 == 0) buf[pos++] = ',';
      }
      
      if (frac_len) memcpy(buf + pos, dot, frac_len);
      return pos + frac_len;
    }
  }

  number_decimal_parse(&decimal, (int)raw_len);
  if (format->percent && decimal.count) decimal.point += 2;
  number_decimal_round(&decimal, format->max_fraction);

  int point = decimal.point, count = decimal.count;
  int integer = point > format->min_integer ? point : format->min_integer;
  int fraction = count > point ? count - point : 0;
  int padded_fraction = fraction < format->min_fraction ? format->min_fraction : fraction;

  size_t separators = format->ungrouped ? 0 : (size_t)(integer - 1) / 3;
  size_t needed = negative + (size_t)integer + separators + (padded_fraction ? 1 + (size_t)padded_fraction : 0) + format->percent;
  if (needed > capacity) return 0;

  if (negative) buf[pos++] = '-';
  
  int group = format->ungrouped ? integer : (integer - 1) % 3 + 1;
  int source = point - integer;
  int stored = point < count ? point : count;
  
  for (int left = integer; left > 0;) {
    for (int i = 0; i < group; i++, source++) buf[pos++] = source >= 0 && source < stored ? decimal.digits[source] : '0';
    left -= group;
    if (left > 0) buf[pos++] = ',';
    group = 3;
  }

  if (padded_fraction) {
    buf[pos++] = '.';
    int zeros = point < 0 ? -point : 0;
    if (zeros > padded_fraction) zeros = padded_fraction;
    memset(buf + pos, '0', (size_t)padded_fraction);
    int copy = fraction - zeros;
    if (copy > 0) memcpy(buf + pos + zeros, decimal.digits + (point > 0 ? point : 0), (size_t)copy);
    pos += (size_t)padded_fraction;
  }

  if (format->percent) buf[pos++] = '%';
  return pos;
}

size_t format_number_to_locale_string(ant_value_t num, char *buf, size_t capacity) {
  return Ant_Number_FormatLocale(Ant_Value_AsNumberUnchecked(num), &ANT_NUMBER_LOCALE_DEFAULT, buf, capacity);
}

static ant_value_t number_locale_digits(ant_t *js, ant_value_t options, const char *key, int low, int high, int *out) {
  ant_value_t value = js_get(js, options, key);
  if (is_err(value)) return value;
  
  *out = -1;
  if (vtype(value) == kTypeUndefined) return Ant_Value_Undefined;
  double number = js_to_number(js, value);
  
  if (isnan(number) || number < low || number > high)
    return js_mkerr_typed(js, JS_ERR_RANGE, "%s value is out of range", key);
    
  *out = (int)floor(number);
  return Ant_Value_Undefined;
}

ant_value_t Ant_Number_ReadLocaleOptions(ant_t *js, ant_value_t options, ant_number_locale_t *out) {
  *out = ANT_NUMBER_LOCALE_DEFAULT;
  if (vtype(options) == kTypeUndefined) return Ant_Value_Undefined;
  if (vtype(options) == kTypeNull) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert undefined or null to object");
  if (!is_object_type(options)) return Ant_Value_Undefined;

  ant_value_t style = js_get(js, options, "style");
  if (is_err(style)) return style;
  
  if (vtype(style) != kTypeUndefined) {
    style = js_tostring_val(js, style);
    if (is_err(style)) return style;
    
    size_t style_len = 0;
    const char *text = js_getstr(js, style, &style_len);
    
    bool known = false;
    static const char *const styles[] = { "decimal", "percent", "currency", "unit" };
    for (size_t i = 0; i < sizeof(styles) / sizeof(styles[0]); i++)
      known = known || (strlen(styles[i]) == style_len && memcmp(styles[i], text, style_len) == 0);
      
    if (!known) return js_mkerr_typed(js, JS_ERR_RANGE, "Value %.*s out of range for Intl.NumberFormat options property style", (int)style_len, text);
    out->percent = style_len == 7 && memcmp(text, "percent", 7) == 0;
  }

  int min_integer, min_fraction, max_fraction;
  ant_value_t status = number_locale_digits(js, options, "minimumIntegerDigits", 1, 21, &min_integer);
  
  if (is_err(status)) return status;
  status = number_locale_digits(js, options, "minimumFractionDigits", 0, 100, &min_fraction);
  
  if (is_err(status)) return status;
  status = number_locale_digits(js, options, "maximumFractionDigits", 0, 100, &max_fraction);
  if (is_err(status)) return status;

  int default_max = out->percent ? 0 : 3;
  if (min_fraction < 0 && max_fraction < 0) { min_fraction = 0; max_fraction = default_max; }
  else if (min_fraction < 0) min_fraction = 0;
  else if (max_fraction < 0) max_fraction = min_fraction > default_max ? min_fraction : default_max;
  else if (min_fraction > max_fraction) return js_mkerr_typed(js, JS_ERR_RANGE, "maximumFractionDigits value is out of range");

  ant_value_t grouping = js_get(js, options, "useGrouping");
  if (is_err(grouping)) return grouping;

  if (min_integer > 0) out->min_integer = (uint8_t)min_integer;
  out->min_fraction = (uint8_t)min_fraction;
  out->max_fraction = (uint8_t)max_fraction;
  out->ungrouped = grouping == Ant_Value_False;
  
  return Ant_Value_Undefined;
}

static ant_value_t builtin_Number_isNaN(ant_params_t) {
  if (nargs == 0) return mkval(kTypeBool, 0);
  ant_value_t arg = args[0];
  
  if (vtype(arg) != kTypeNumber) return mkval(kTypeBool, 0);
  
  double val = Ant_Value_AsNumberUnchecked(arg);
  return mkval(kTypeBool, isnan(val) ? 1 : 0);
}

static ant_value_t builtin_Number_isFinite(ant_params_t) {
  if (nargs == 0) return mkval(kTypeBool, 0);
  ant_value_t arg = args[0];
  
  if (vtype(arg) != kTypeNumber) return mkval(kTypeBool, 0);
  
  double val = Ant_Value_AsNumberUnchecked(arg);
  return mkval(kTypeBool, isfinite(val) ? 1 : 0);
}

static ant_value_t builtin_global_isNaN(ant_params_t) {
  if (nargs == 0) return mkval(kTypeBool, 1);
  double val = js_to_number(js, args[0]);
  return mkval(kTypeBool, isnan(val) ? 1 : 0);
}

static ant_value_t builtin_global_isFinite(ant_params_t) {
  if (nargs == 0) return mkval(kTypeBool, 0);
  double val = js_to_number(js, args[0]);
  return mkval(kTypeBool, isfinite(val) ? 1 : 0);
}

static ant_value_t builtin_Number_isInteger(ant_params_t) {
  if (nargs == 0) return mkval(kTypeBool, 0);
  ant_value_t arg = args[0];
  
  if (vtype(arg) != kTypeNumber) return mkval(kTypeBool, 0);
  
  double val = Ant_Value_AsNumberUnchecked(arg);
  if (!isfinite(val)) return mkval(kTypeBool, 0);
  return mkval(kTypeBool, (val == floor(val)) ? 1 : 0);
}

static ant_value_t builtin_Number_isSafeInteger(ant_params_t) {
  if (nargs == 0) return mkval(kTypeBool, 0);
  ant_value_t arg = args[0];
  
  if (vtype(arg) != kTypeNumber) return mkval(kTypeBool, 0);
  
  double val = Ant_Value_AsNumberUnchecked(arg);
  if (!isfinite(val)) return mkval(kTypeBool, 0);
  if (val != floor(val)) return mkval(kTypeBool, 0);
  
  return mkval(kTypeBool, (val >= -9007199254740991.0 && val <= 9007199254740991.0) ? 1 : 0);
}

__attribute__((noinline)) 
static ant_value_t number_from_value(ant_t *js, ant_value_t source) {
  if (is_object_type(source)) {
    source = js_to_primitive(js, source, 2);
    if (is_err(source)) return source;
  }

  return Ant_Value_FromNumber(vtype(source) == kTypeBigInt ? bigint_to_double(js, source) : js_to_number(js, source));
}

static ant_value_t builtin_Number(ant_params_t) {
  ant_value_t source = nargs > 0 ? args[0] : Ant_Value_FromNumber(0.0);

  ant_value_t nval = source;
  if (vtype(source) != kTypeNumber) {
    nval = number_from_value(js, source);
    if (is_err(nval)) return nval;
  }

  if (is_wrapper_ctor_target(js, call_this, call_new_target)) js_set_slot(call_this, SLOT_PRIMITIVE, nval);
  return nval;
}

static ant_value_t builtin_number_toString(ant_params_t) {
  ant_value_t num = unwrap_primitive(js, call_this);
  if (vtype(num) != kTypeNumber) return js_mkerr(js, "toString called on non-number");
  
  double radix = 10;
  if (nargs >= 1 && vtype(args[0]) == kTypeNumber) radix = string_number_to_integer(Ant_Value_AsNumberUnchecked(args[0]));
  else if (nargs >= 1 && vtype(args[0]) != kTypeUndefined) {
    ant_value_t status = string_to_integer_or_infinity(js, args[0], &radix);
    if (is_err(status)) return status;
  }
  
  if (radix < 2 || radix > 36) return js_mkerr_typed(js, JS_ERR_RANGE, "toString() radix argument must be between 2 and 36");
  if (radix == 10) return js_tostring_val(js, num);
  
  double val = Ant_Value_AsNumberUnchecked(num);
  if (isnan(val)) return js_mkstr(js, "NaN", 3);
  if (isinf(val)) return val > 0 ? js_mkstr(js, "Infinity", 8) : js_mkstr(js, "-Infinity", 9);
  if (val == 0) return js_mkstr(js, "0", 1);
  
  if (fabs(val) <= 9007199254740992.0 && val == trunc(val)) {
    static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char digits[66];
    char *p = digits + sizeof(digits);
    uint64_t u = (uint64_t)fabs(val);
    do { *--p = chars[u % (unsigned)radix]; u /= (unsigned)radix; } while (u);
    if (val < 0) *--p = '-';
    return js_mkstr_ascii(js, p, (size_t)(digits + sizeof(digits) - p));
  }
  
  char buffer[ANT_NUMBER_RADIX_BUFFER_SIZE];
  const char *start = NULL;
  size_t len = ant_number_to_radix(val, (int)radix, buffer, sizeof(buffer), &start);
  
  return js_mkstr_ascii(js, start, len);
}

static ant_value_t builtin_number_toFixed(ant_params_t) {
  ant_value_t num = unwrap_primitive(js, call_this);
  if (vtype(num) != kTypeNumber) return js_mkerr(js, "toFixed called on non-number");
  
  double d = Ant_Value_AsNumberUnchecked(num);
  if (isnan(d)) return js_mkstr(js, "NaN", 3);
  if (isinf(d)) return d > 0 ? js_mkstr(js, "Infinity", 8) : js_mkstr(js, "-Infinity", 9);
  
  int digits = 0;
  if (nargs >= 1 && vtype(args[0]) != kTypeUndefined) {
    digits = (int) Ant_Value_AsNumberUnchecked(args[0]);
    if (digits < 0 || digits > 100) {
      return js_mkerr_typed(js, JS_ERR_RANGE, "toFixed() digits argument must be between 0 and 100");
    }
  }
  
  if (fabs(d) >= 1e21) {
    char buf[64];
    size_t len = js_number_to_chars(num, buf, sizeof(buf));
    return js_mkstr(js, buf, len);
  }

  char buf[160];
  size_t len = ant_number_to_fixed(d, digits, buf, sizeof(buf));
  if (len == 0) return js_mkerr(js, "number formatting failed");
  
  return js_mkstr(js, buf, len);
}

static ant_value_t builtin_number_toPrecision(ant_params_t) {
  ant_value_t num = unwrap_primitive(js, call_this);
  if (vtype(num) != kTypeNumber) return js_mkerr(js, "toPrecision called on non-number");
  
  double d = Ant_Value_AsNumberUnchecked(num);
  if (isnan(d)) return js_mkstr(js, "NaN", 3);
  if (isinf(d)) return d > 0 ? js_mkstr(js, "Infinity", 8) : js_mkstr(js, "-Infinity", 9);
  
  if (nargs < 1 || vtype(args[0]) == kTypeUndefined) {
    char buf[64];
    size_t len = js_number_to_chars(num, buf, sizeof(buf));
    return js_mkstr(js, buf, len);
  }
  
  int precision = (int) Ant_Value_AsNumberUnchecked(args[0]);
  if (precision < 1 || precision > 100) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "toPrecision() argument must be between 1 and 100");
  }
  
  char buf[160];
  size_t len = ant_number_to_precision(d, precision, buf, sizeof(buf));
  if (len == 0) return js_mkerr(js, "number formatting failed");
  
  return js_mkstr(js, buf, len);
}

static ant_value_t builtin_number_toExponential(ant_params_t) {
  ant_value_t num = unwrap_primitive(js, call_this);
  if (vtype(num) != kTypeNumber) return js_mkerr(js, "toExponential called on non-number");
  
  double d = Ant_Value_AsNumberUnchecked(num);
  if (isnan(d)) return js_mkstr(js, "NaN", 3);
  if (isinf(d)) return d > 0 ? js_mkstr(js, "Infinity", 8) : js_mkstr(js, "-Infinity", 9);
  
  int digits = -1;
  if (nargs >= 1 && vtype(args[0]) != kTypeUndefined) {
    digits = (int) Ant_Value_AsNumberUnchecked(args[0]);
    if (digits < 0 || digits > 100) {
      return js_mkerr_typed(js, JS_ERR_RANGE, "toExponential() argument must be between 0 and 100");
    }
  }
  
  char buf[160];
  size_t len = ant_number_to_exponential(d, digits, buf, sizeof(buf));
  if (len == 0) return js_mkerr(js, "number formatting failed");
  
  return js_mkstr(js, buf, len);
}

static ant_value_t builtin_number_valueOf(ant_params_t) {
  (void) args; (void) nargs;
  ant_value_t num = unwrap_primitive(js, call_this);
  if (vtype(num) != kTypeNumber) return js_mkerr(js, "valueOf called on non-number");
  return num;
}

ant_value_t builtin_number_toLocaleString(ant_params_t) {
  ant_value_t num = unwrap_primitive(js, call_this);
  if (vtype(num) != kTypeNumber) return js_mkerr(js, "toLocaleString called on non-number");

  ant_number_locale_t format = ANT_NUMBER_LOCALE_DEFAULT;
  if (nargs > 0 && vtype(args[0]) != kTypeUndefined) {
    ant_value_t locale = Ant_Intl_ResolveLocale(js, args[0]);
    if (is_err(locale)) return locale;
  }
  
  if (nargs > 1) {
    ant_value_t status = Ant_Number_ReadLocaleOptions(js, args[1], &format);
    if (is_err(status)) return status;
  }

  char buf[ANT_NUMBER_LOCALE_CHARS];
  size_t len = Ant_Number_FormatLocale(Ant_Value_AsNumberUnchecked(num), &format, buf, sizeof(buf));
  if (len == 0) return js_mkerr(js, "number formatting failed");
  
  return js_mkstr(js, buf, len);
}

static ant_value_t builtin_parseInt(ant_params_t) {
  ant_value_t str_val = coerce_to_str(js, nargs > 0 ? args[0] : Ant_Value_Undefined);
  if (is_err(str_val)) return str_val;
  
  int32_t radix = 0;
  if (nargs >= 2 && vtype(args[1]) == kTypeNumber) radix = js_to_int32(Ant_Value_AsNumberUnchecked(args[1]));
  else if (nargs >= 2 && vtype(args[1]) != kTypeUndefined) {
    GC_ROOT_SAVE(root_mark, js);
    GC_ROOT_PIN(js, str_val);
    
    double number = 0;
    ant_value_t status = string_to_integer_or_infinity(js, args[1], &number);
    
    GC_ROOT_RESTORE(js, root_mark);
    if (is_err(status)) return status;
    radix = js_to_int32(number);
  }
  
  if (radix != 0 && (radix < 2 || radix > 36)) return Ant_Value_FromNumber(ANT_NAN);
  
  ant_offset_t str_len = 0;
  const char *str = (const char *)(uintptr_t)vstr(js, str_val, &str_len);
  return Ant_Value_FromNumber(ant_number_parse_int(str, (size_t)str_len, radix));
}

static ant_value_t builtin_parseFloat(ant_params_t) {
  if (nargs < 1) return Ant_Value_FromNumber(ANT_NAN);
  
  ant_value_t str_val = args[0];
  if (vtype(str_val) != kTypeString) {
    const char *str = js_str(js, str_val);
    str_val = js_mkstr(js, str, strlen(str));
  }
  
  ant_offset_t str_len, str_off = vstr(js, str_val, &str_len);
  const char *str = (char *)(uintptr_t)(str_off);
  
  ant_offset_t i = 0;
  while (i < str_len && is_space(str[i])) i++;
  
  if (i >= str_len) return Ant_Value_FromNumber(ANT_NAN);
  
  double result = ANT_NAN;
  size_t processed = 0;
  
  if (!ant_number_parse(
    str + i,
    (size_t)(str_len - i),
    ANT_NUMBER_PARSE_FLOAT_PREFIX,
    &result, &processed
  ) || processed == 0) return Ant_Value_FromNumber(ANT_NAN);

  return Ant_Value_FromNumber(result);
}

double js_parse_int_value(ant_t *js, ant_value_t arg) {
  ant_value_t result = builtin_parseInt(js, &arg, 1, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined);
  return vtype(result) == kTypeNumber ? Ant_Value_AsNumberUnchecked(result) : ANT_NAN;
}

double js_parse_float_value(ant_t *js, ant_value_t arg) {
  ant_value_t result = builtin_parseFloat(js, &arg, 1, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined);
  return vtype(result) == kTypeNumber ? Ant_Value_AsNumberUnchecked(result) : ANT_NAN;
}

void js_number_init_prototype(ant_t *js, ant_value_t number_proto) {
  js_set_slot(number_proto, SLOT_PRIMITIVE, Ant_Value_FromNumber(0.0));
  
  js->sym.number_to_string_fn = defmethod(js, number_proto, "toString", 8, Ant_Function_CreateWithArity(builtin_number_toString, 1));
  defmethod(js, number_proto, "toFixed", 7, Ant_Function_CreateWithArity(builtin_number_toFixed, 1));
  defmethod(js, number_proto, "toPrecision", 11, Ant_Function_CreateWithArity(builtin_number_toPrecision, 1));
  defmethod(js, number_proto, "toExponential", 13, Ant_Function_CreateWithArity(builtin_number_toExponential, 1));
  defmethod(js, number_proto, "valueOf", 7, Ant_Function_Create(builtin_number_valueOf));
  defmethod(js, number_proto, "toLocaleString", 14, Ant_Function_Create(builtin_number_toLocaleString));
}

void js_number_init_constructor(ant_t *js, ant_value_t number_ctor_obj, ant_value_t number_proto, ant_value_t *parse_int, ant_value_t *parse_float) {
  js_set_slot(number_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(builtin_Number, 1));
  defmethod(js, number_ctor_obj, "isNaN", 5, Ant_Function_CreateEx(builtin_Number_isNaN, 1, CFUNC_REALM_FREE));
  defmethod(js, number_ctor_obj, "isFinite", 8, Ant_Function_CreateEx(builtin_Number_isFinite, 1, CFUNC_REALM_FREE));
  defmethod(js, number_ctor_obj, "isInteger", 9, Ant_Function_CreateEx(builtin_Number_isInteger, 1, CFUNC_REALM_FREE));
  defmethod(js, number_ctor_obj, "isSafeInteger", 13, Ant_Function_CreateEx(builtin_Number_isSafeInteger, 1, CFUNC_REALM_FREE));
  *parse_int = defmethod(js, number_ctor_obj, "parseInt", 8, Ant_Function_CreateEx(builtin_parseInt, 2, CFUNC_REALM_FREE));
  *parse_float = defmethod(js, number_ctor_obj, "parseFloat", 10, Ant_Function_CreateEx(builtin_parseFloat, 1, CFUNC_REALM_FREE));
  
  js_setprop_readonly_nonconfigurable(js, number_ctor_obj, "MAX_VALUE", 9, Ant_Value_FromNumber(1.7976931348623157e+308));
  js_setprop_readonly_nonconfigurable(js, number_ctor_obj, "MIN_VALUE", 9, Ant_Value_FromNumber(5e-324));
  js_setprop_readonly_nonconfigurable(js, number_ctor_obj, "MAX_SAFE_INTEGER", 16, Ant_Value_FromNumber(9007199254740991.0));
  js_setprop_readonly_nonconfigurable(js, number_ctor_obj, "MIN_SAFE_INTEGER", 16, Ant_Value_FromNumber(-9007199254740991.0));
  js_setprop_readonly_nonconfigurable(js, number_ctor_obj, "POSITIVE_INFINITY", 17, Ant_Value_FromNumber(ANT_INFINITY));
  js_setprop_readonly_nonconfigurable(js, number_ctor_obj, "NEGATIVE_INFINITY", 17, Ant_Value_FromNumber(ANT_NEG_INFINITY));
  js_setprop_readonly_nonconfigurable(js, number_ctor_obj, "NaN", 3, Ant_Value_FromNumber(ANT_NAN));
  js_setprop_readonly_nonconfigurable(js, number_ctor_obj, "EPSILON", 7, Ant_Value_FromNumber(2.220446049250313e-16));
  
  js_setprop_readonly_nonconfigurable(js, number_ctor_obj, "prototype", 9, number_proto);
  js_set_function_name(js, number_ctor_obj, "Number", sizeof("Number") - 1);
}

void js_number_init_globals(ant_t *js, ant_value_t glob) {
  defmethod(js, glob, "isNaN", 5, Ant_Function_CreateEx(builtin_global_isNaN, 1, CFUNC_REALM_FREE));
  defmethod(js, glob, "isFinite", 8, Ant_Function_CreateEx(builtin_global_isFinite, 1, CFUNC_REALM_FREE));
}
