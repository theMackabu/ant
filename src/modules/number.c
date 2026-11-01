#include <compat.h> // IWYU pragma: keep
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "cfunc.h"
#include "proto.h"
#include "numbers.h"
#include "silver/lexer.h"
#include "modules/number.h"
#include "modules/string.h"

size_t format_number_to_locale_string(ant_value_t num, char *buf, size_t capacity) {
  char raw[64];
  size_t raw_len = js_number_to_chars(num, raw, sizeof(raw));
  
  double d = Ant_Value_AsNumberUnchecked(num);
  if (!isfinite(d) || memchr(raw, 'e', raw_len) || memchr(raw, 'E', raw_len)) {
    if (raw_len > capacity) return 0;
    memcpy(buf, raw, raw_len);
    return raw_len;
  }

  char *dot = memchr(raw, '.', raw_len);
  size_t int_len = dot ? (size_t)(dot - raw) : raw_len;
  size_t start = raw[0] == '-' ? 1 : 0;
  size_t frac_len = dot ? raw_len - int_len : 0;
  size_t separators = int_len > start ? (int_len - start - 1) / 3 : 0;
  size_t needed = int_len + separators + frac_len;
  if (needed > capacity) return 0;

  size_t pos = 0;
  if (start) buf[pos++] = '-';
  for (size_t i = start; i < int_len; i++) {
    buf[pos++] = raw[i];
    size_t remaining = int_len - 1 - i;
    if (remaining > 0 && remaining % 3 == 0) buf[pos++] = ',';
  }
  if (frac_len) memcpy(buf + pos, dot, frac_len);
  return pos + frac_len;
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

static ant_value_t builtin_Number(ant_params_t) {
  ant_value_t nval = Ant_Value_FromNumber(nargs > 0 ? js_to_number(js, args[0]) : 0.0);
  if (is_wrapper_ctor_target(js, call_this, call_new_target))
    set_slot(call_this, SLOT_PRIMITIVE, nval);
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
  (void) args; (void) nargs;
  ant_value_t num = unwrap_primitive(js, call_this);
  if (vtype(num) != kTypeNumber) return js_mkerr(js, "toLocaleString called on non-number");
  char buf[128];
  size_t len = format_number_to_locale_string(num, buf, sizeof(buf));
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
  set_slot(number_proto, SLOT_PRIMITIVE, Ant_Value_FromNumber(0.0));
  
  js->sym.number_to_string_fn = defmethod(js, number_proto, "toString", 8, Ant_Function_CreateWithArity(builtin_number_toString, 1));
  defmethod(js, number_proto, "toFixed", 7, Ant_Function_CreateWithArity(builtin_number_toFixed, 1));
  defmethod(js, number_proto, "toPrecision", 11, Ant_Function_CreateWithArity(builtin_number_toPrecision, 1));
  defmethod(js, number_proto, "toExponential", 13, Ant_Function_CreateWithArity(builtin_number_toExponential, 1));
  defmethod(js, number_proto, "valueOf", 7, Ant_Function_Create(builtin_number_valueOf));
  defmethod(js, number_proto, "toLocaleString", 14, Ant_Function_Create(builtin_number_toLocaleString));
}

void js_number_init_constructor(ant_t *js, ant_value_t number_ctor_obj, ant_value_t number_proto, ant_value_t *parse_int, ant_value_t *parse_float) {
  set_slot(number_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(builtin_Number, 1));
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
