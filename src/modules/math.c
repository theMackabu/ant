#include <float.h>

#include "ant.h"
#include "cfunc.h"
#include "errors.h"
#include "internal.h"

#include "modules/crypto.h"
#include "modules/math.h"

#if DBL_MANT_DIG >= 64
#error "Unsupported double mantissa width for Math.random"
#endif

enum {
  MATH_RANDOM_MANTISSA_BITS = DBL_MANT_DIG,
  MATH_RANDOM_DISCARD_BITS = 64 - DBL_MANT_DIG,
};

static double math_to_number(ant_t *js, ant_value_t value) {
  if (vtype(value) == kTypeNumber) return tod(value);
  ant_value_t numeric = js_to_numeric(js, value);
  
  if (is_err(numeric)) return JS_NAN;
  if (vtype(numeric) == kTypeBigInt) {
    js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a BigInt value to a number");
    return JS_NAN;
  }
  
  return tod(numeric);
}

static const double math_random_scale =
  1.0 / (double)(UINT64_C(1) << MATH_RANDOM_MANTISSA_BITS);

const char *const ant_math_intrinsic_names[ANT_MATH_INTRINSIC_COUNT] = {
  [ANT_MATH_ABS] = "abs",
  [ANT_MATH_CEIL] = "ceil",
  [ANT_MATH_FLOOR] = "floor",
  [ANT_MATH_ROUND] = "round",
  [ANT_MATH_SIGN] = "sign",
  [ANT_MATH_SQRT] = "sqrt",
  [ANT_MATH_TRUNC] = "trunc",
  [ANT_MATH_IMUL] = "imul",
  [ANT_MATH_MAX] = "max",
  [ANT_MATH_MIN] = "min",
};

double ant_math_ceil(double x) { return ceil(x); }
double ant_math_floor(double x) { return floor(x); }
double ant_math_sqrt(double x) { return sqrt(x); }
double ant_math_trunc(double x) { return trunc(x); }

double ant_math_round(double x) {
  if (isnan(x) || isinf(x) || x == 0.0) return x;
  if (x < 0.0 && x >= -0.5) return -0.0;
  double r = floor(x);
  return x - r >= 0.5 ? r + 1.0 : r;
}

double ant_math_sign(double x) {
  if (x > 0) return 1.0;
  if (x < 0) return -1.0;
  return x;
}

double ant_math_imul(double a, double b) {
  return (double)(int32_t)((uint32_t)js_to_int32(a) * (uint32_t)js_to_int32(b));
}

double ant_math_max(double a, double b) {
  if (isnan(a) || isnan(b)) return JS_NAN;
  if (b > a) return b;
  if (b == 0.0 && a == 0.0 && !signbit(b) && signbit(a)) return b;
  return a;
}

double ant_math_min(double a, double b) {
  if (isnan(a) || isnan(b)) return JS_NAN;
  if (b < a) return b;
  if (b == 0.0 && a == 0.0 && signbit(b) && !signbit(a)) return b;
  return a;
}

double ant_math_js_pow(double base, double exponent) {
  if (isnan(exponent)) return JS_NAN;
  if (isinf(exponent) && fabs(base) == 1.0) return JS_NAN;
  return pow(base, exponent);
}

#define ANT_MATH_DOUBLE(id, name, argc, expr) \
  static double ant_math_double_##name(double a, double b) { (void)b; return expr; }
#include "modules/math_doubles.h"

static ant_value_t math_double_call(ant_t *js, ant_value_t *args, int nargs, ant_math_double_t id) {
  const ant_math_double_info_t *info = &ant_math_doubles[id];
  double a = nargs < 1 ? JS_NAN : math_to_number(js, args[0]);
  double b = info->argc < 2 ? 0.0 : nargs < 2 ? JS_NAN : math_to_number(js, args[1]);
  return tov(info->fn(a, b));
}

#define ANT_MATH_DOUBLE(id, name, argc, expr) \
  static ant_value_t builtin_Math_##name(ant_params_t) { return math_double_call(js, args, nargs, ANT_MATH_DOUBLE_##id); }
#include "modules/math_doubles.h"

const ant_math_double_info_t ant_math_doubles[ANT_MATH_DOUBLE_COUNT] = {
#define ANT_MATH_DOUBLE(id, name, argc, expr) [ANT_MATH_DOUBLE_##id] = {#name, ant_math_double_##name, argc},
#include "modules/math_doubles.h"
};

static const ant_cfunc_t math_double_builtins[ANT_MATH_DOUBLE_COUNT] = {
#define ANT_MATH_DOUBLE(id, name, argc, expr) [ANT_MATH_DOUBLE_##id] = builtin_Math_##name,
#include "modules/math_doubles.h"
};

int ant_math_double_of(ant_value_t func) {
  if (vtype(func) != kTypeBuiltin) return -1;
  ant_cfunc_t fn = js_as_cfunc(func);
  for (int i = 0; i < ANT_MATH_DOUBLE_COUNT; i++)
    if (math_double_builtins[i] == fn) return i;
  return -1;
}

static ant_value_t builtin_Math_abs(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : math_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(fabs(x));
}

static ant_value_t builtin_Math_ceil(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : math_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(ant_math_ceil(x));
}

static ant_value_t builtin_Math_clz32(ant_params_t) {
  if (nargs < 1) return tov(32);
  uint32_t n = js_to_uint32(math_to_number(js, args[0]));
  
  if (n == 0) return tov(32);
  int lz = __builtin_clz(n);
  
  if (sizeof(unsigned int) > sizeof(uint32_t))
    lz -= (int)((sizeof(unsigned int) - sizeof(uint32_t)) * 8);
  
  return tov((double)lz);
}

static ant_value_t builtin_Math_floor(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : math_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(ant_math_floor(x));
}

static ant_value_t builtin_Math_fround(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : math_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov((double)(float)x);
}

static ant_value_t builtin_Math_hypot(ant_params_t) {
  if (nargs == 0) return tov(0.0);
  
  double acc = 0.0;
  bool saw_nan = false, saw_inf = false;
  
  for (int i = 0; i < nargs; i++) {
    double v = math_to_number(js, args[i]);
    if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
    if (isinf(v)) saw_inf = true;
    else if (isnan(v)) saw_nan = true;
    else acc = hypot(acc, v);
  }
  
  if (saw_inf) return tov(JS_INF);
  if (saw_nan) return tov(JS_NAN);
  
  return tov(acc);
}

static ant_value_t builtin_Math_imul(ant_params_t) {
  double a = math_to_number(js, nargs > 0 ? args[0] : js_mkundef());
  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
  
  double b = math_to_number(js, nargs > 1 ? args[1] : js_mkundef());
  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
  
  return tov(ant_math_imul(a, b));
}

static ant_value_t math_min_max(ant_t *js, ant_value_t *args, int nargs, bool max) {
  double result = max ? JS_NEG_INF : JS_INF;
  
  for (int i = 0; i < nargs; i++) {
    double v;
    if (vtype(args[i]) == kTypeNumber) v = tod(args[i]);
    else {
      v = math_to_number(js, args[i]);
      if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
    }
    
    result = max ? ant_math_max(result, v) : ant_math_min(result, v);
  }
  
  return tov(result);
}

static ant_value_t builtin_Math_max(ant_params_t) {
  return math_min_max(js, args, nargs, true);
}

static ant_value_t builtin_Math_min(ant_params_t) {
  return math_min_max(js, args, nargs, false);
}

static ant_value_t builtin_Math_random(ant_params_t) {
  uint64_t r = 0;
  if (crypto_fill_random(&r, sizeof(r)) < 0) {
    return js_mkerr(js, "secure random generation failed");
  }
  
  uint64_t fraction = r >> MATH_RANDOM_DISCARD_BITS;
  return tov((double)fraction * math_random_scale);
}

static ant_value_t builtin_Math_round(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : math_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(ant_math_round(x));
}

static ant_value_t builtin_Math_sign(ant_params_t) {
  double v = (nargs < 1) ? JS_NAN : math_to_number(js, args[0]);
  if (isnan(v)) return tov(JS_NAN);
  return tov(ant_math_sign(v));
}

static ant_value_t builtin_Math_sqrt(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : math_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(ant_math_sqrt(x));
}

static ant_value_t builtin_Math_trunc(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : math_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(ant_math_trunc(x));
}

void init_math_module(ant_t *js) {
  ant_value_t math_obj = mkobj(js, 0);
  ant_value_t object_proto = js->sym.object_proto;

  js_set_proto_init(math_obj, object_proto);
  mkprop_exact_attrs(js, math_obj, js_mkstr(js, "E", 1), tov(M_E), 0);
  mkprop_exact_attrs(js, math_obj, js_mkstr(js, "LN10", 4), tov(M_LN10), 0);
  mkprop_exact_attrs(js, math_obj, js_mkstr(js, "LN2", 3), tov(M_LN2), 0);
  mkprop_exact_attrs(js, math_obj, js_mkstr(js, "LOG10E", 6), tov(M_LOG10E), 0);
  mkprop_exact_attrs(js, math_obj, js_mkstr(js, "LOG2E", 5), tov(M_LOG2E), 0);
  mkprop_exact_attrs(js, math_obj, js_mkstr(js, "PI", 2), tov(M_PI), 0);
  mkprop_exact_attrs(js, math_obj, js_mkstr(js, "SQRT1_2", 7), tov(M_SQRT1_2), 0);
  mkprop_exact_attrs(js, math_obj, js_mkstr(js, "SQRT2", 5), tov(M_SQRT2), 0);
  
  defmethod(js, math_obj, "abs", 3, Ant_Function_CreateEx(builtin_Math_abs, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "acos", 4, Ant_Function_CreateEx(builtin_Math_acos, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "acosh", 5, Ant_Function_CreateEx(builtin_Math_acosh, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "asin", 4, Ant_Function_CreateEx(builtin_Math_asin, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "asinh", 5, Ant_Function_CreateEx(builtin_Math_asinh, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "atan", 4, Ant_Function_CreateEx(builtin_Math_atan, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "atanh", 5, Ant_Function_CreateEx(builtin_Math_atanh, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "atan2", 5, Ant_Function_CreateEx(builtin_Math_atan2, 2, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "cbrt", 4, Ant_Function_CreateEx(builtin_Math_cbrt, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "ceil", 4, Ant_Function_CreateEx(builtin_Math_ceil, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "clz32", 5, Ant_Function_CreateEx(builtin_Math_clz32, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "cos", 3, Ant_Function_CreateEx(builtin_Math_cos, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "cosh", 4, Ant_Function_CreateEx(builtin_Math_cosh, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "exp", 3, Ant_Function_CreateEx(builtin_Math_exp, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "expm1", 5, Ant_Function_CreateEx(builtin_Math_expm1, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "floor", 5, Ant_Function_CreateEx(builtin_Math_floor, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "fround", 6, Ant_Function_CreateEx(builtin_Math_fround, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "hypot", 5, Ant_Function_CreateEx(builtin_Math_hypot, 2, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "imul", 4, Ant_Function_CreateEx(builtin_Math_imul, 2, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "log", 3, Ant_Function_CreateEx(builtin_Math_log, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "log1p", 5, Ant_Function_CreateEx(builtin_Math_log1p, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "log10", 5, Ant_Function_CreateEx(builtin_Math_log10, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "log2", 4, Ant_Function_CreateEx(builtin_Math_log2, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "max", 3, Ant_Function_CreateEx(builtin_Math_max, 2, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "min", 3, Ant_Function_CreateEx(builtin_Math_min, 2, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "pow", 3, Ant_Function_CreateEx(builtin_Math_pow, 2, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "random", 6, Ant_Function_CreateEx(builtin_Math_random, 0, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "round", 5, Ant_Function_CreateEx(builtin_Math_round, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "sign", 4, Ant_Function_CreateEx(builtin_Math_sign, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "sin", 3, Ant_Function_CreateEx(builtin_Math_sin, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "sinh", 4, Ant_Function_CreateEx(builtin_Math_sinh, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "sqrt", 4, Ant_Function_CreateEx(builtin_Math_sqrt, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "tan", 3, Ant_Function_CreateEx(builtin_Math_tan, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "tanh", 4, Ant_Function_CreateEx(builtin_Math_tanh, 1, CFUNC_REALM_FREE));
  defmethod(js, math_obj, "trunc", 5, Ant_Function_CreateEx(builtin_Math_trunc, 1, CFUNC_REALM_FREE));
  
  for (int i = 0; i < ANT_MATH_INTRINSIC_COUNT; i++)
    js->sym.math_fns[i] = js_get(js, math_obj, ant_math_intrinsic_names[i]);

  js_set_tostringtag(js, math_obj, js_mkstr(js, "Math", 4));
  js_set_global_builtin(js, "Math", math_obj);
}
