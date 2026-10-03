#include <float.h>

#include "ant.h"
#include "errors.h"
#include "internal.h"

#include "modules/symbol.h"
#include "modules/crypto.h"

#if DBL_MANT_DIG >= 64
#error "Unsupported double mantissa width for Math.random"
#endif

enum {
  MATH_RANDOM_MANTISSA_BITS = DBL_MANT_DIG,
  MATH_RANDOM_DISCARD_BITS = 64 - DBL_MANT_DIG,
};

static const double math_random_scale =
  1.0 / (double)(UINT64_C(1) << MATH_RANDOM_MANTISSA_BITS);

static ant_value_t builtin_Math_abs(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(fabs(x));
}

static ant_value_t builtin_Math_acos(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(acos(x));
}

static ant_value_t builtin_Math_acosh(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(acosh(x));
}

static ant_value_t builtin_Math_asin(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(asin(x));
}

static ant_value_t builtin_Math_asinh(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(asinh(x));
}

static ant_value_t builtin_Math_atan(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(atan(x));
}

static ant_value_t builtin_Math_atanh(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(atanh(x));
}

static ant_value_t builtin_Math_atan2(ant_params_t) {
  double y = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  double x = (nargs < 2) ? JS_NAN : js_to_number(js, args[1]);
  if (isnan(y) || isnan(x)) return tov(JS_NAN);
  return tov(atan2(y, x));
}

static ant_value_t builtin_Math_cbrt(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(cbrt(x));
}

static ant_value_t builtin_Math_ceil(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(ceil(x));
}

static ant_value_t builtin_Math_clz32(ant_params_t) {
  if (nargs < 1) return tov(32);
  uint32_t n = js_to_uint32(js_to_number(js, args[0]));
  if (n == 0) return tov(32);
  int lz = __builtin_clz(n);
  if (sizeof(unsigned int) > sizeof(uint32_t)) {
    lz -= (int)((sizeof(unsigned int) - sizeof(uint32_t)) * 8);
  }
  return tov((double)lz);
}

static ant_value_t builtin_Math_cos(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(cos(x));
}

static ant_value_t builtin_Math_cosh(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(cosh(x));
}

static ant_value_t builtin_Math_exp(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(exp(x));
}

static ant_value_t builtin_Math_expm1(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(expm1(x));
}

static ant_value_t builtin_Math_floor(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(floor(x));
}

static ant_value_t builtin_Math_fround(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov((double)(float)x);
}

static ant_value_t builtin_Math_hypot(ant_params_t) {
  if (nargs == 0) return tov(0.0);
  double acc = 0.0;
  bool saw_nan = false;
  for (int i = 0; i < nargs; i++) {
    double v = js_to_number(js, args[i]);
    if (isinf(v)) return tov(JS_INF);
    if (isnan(v)) { saw_nan = true; continue; }
    acc = hypot(acc, v);
  }
  if (saw_nan) return tov(JS_NAN);
  return tov(acc);
}

static ant_value_t builtin_Math_imul(ant_params_t) {
  if (nargs < 2) return tov(0);
  int32_t a = js_to_int32(js_to_number(js, args[0]));
  int32_t b = js_to_int32(js_to_number(js, args[1]));
  return tov((double)((int32_t)((uint32_t)a * (uint32_t)b)));
}

static ant_value_t builtin_Math_log(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(log(x));
}

static ant_value_t builtin_Math_log1p(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(log1p(x));
}

static ant_value_t builtin_Math_log10(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(log10(x));
}

static ant_value_t builtin_Math_log2(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(log2(x));
}

static ant_value_t builtin_Math_max(ant_params_t) {
  if (nargs == 0) return tov(JS_NEG_INF);
  double max_val = js_to_number(js, args[0]);
  if (isnan(max_val)) return tov(JS_NAN);
  for (int i = 1; i < nargs; i++) {
    double v = js_to_number(js, args[i]);
    if (isnan(v)) return tov(JS_NAN);
    if (v > max_val) { max_val = v; continue; }
    if (v == 0.0 && max_val == 0.0 && !signbit(v) && signbit(max_val)) max_val = v;
  }
  return tov(max_val);
}

static ant_value_t builtin_Math_min(ant_params_t) {
  if (nargs == 0) return tov(JS_INF);
  double min_val = vtype(args[0]) == kTypeNumber ? tod(args[0]) : js_to_number(js, args[0]);
  
  if (isnan(min_val)) return tov(JS_NAN);
  
  for (int i = 1; i < nargs; i++) {
    double v = vtype(args[i]) == kTypeNumber ? tod(args[i]) : js_to_number(js, args[i]);
    if (isnan(v)) return tov(JS_NAN);
    
    if (v < min_val) {
      min_val = v;
      continue;
    }
    
    if (v == 0.0 
      && min_val == 0.0 
      && signbit(v) 
      && !signbit(min_val)
    ) min_val = v;
  }
  
  return tov(min_val);
}

static ant_value_t builtin_Math_pow(ant_params_t) {
  double base = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  double exp = (nargs < 2) ? JS_NAN : js_to_number(js, args[1]);
  if (isnan(base) || isnan(exp)) return tov(JS_NAN);
  return tov(pow(base, exp));
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
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x) || isinf(x) || x == 0.0) return tov(x);
  if (x < 0.0 && x >= -0.5) return tov(-0.0);
  return tov(floor(x + 0.5));
}

static ant_value_t builtin_Math_sign(ant_params_t) {
  double v = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(v)) return tov(JS_NAN);
  if (v > 0) return tov(1.0);
  if (v < 0) return tov(-1.0);
  return tov(v);
}

static ant_value_t builtin_Math_sin(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(sin(x));
}

static ant_value_t builtin_Math_sinh(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(sinh(x));
}

static ant_value_t builtin_Math_sqrt(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(sqrt(x));
}

static ant_value_t builtin_Math_tan(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(tan(x));
}

static ant_value_t builtin_Math_tanh(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(tanh(x));
}

static ant_value_t builtin_Math_trunc(ant_params_t) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(trunc(x));
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
  
  defmethod(js, math_obj, "abs", 3, js_mkfun_arity(builtin_Math_abs, 1));
  defmethod(js, math_obj, "acos", 4, js_mkfun_arity(builtin_Math_acos, 1));
  defmethod(js, math_obj, "acosh", 5, js_mkfun_arity(builtin_Math_acosh, 1));
  defmethod(js, math_obj, "asin", 4, js_mkfun_arity(builtin_Math_asin, 1));
  defmethod(js, math_obj, "asinh", 5, js_mkfun_arity(builtin_Math_asinh, 1));
  defmethod(js, math_obj, "atan", 4, js_mkfun_arity(builtin_Math_atan, 1));
  defmethod(js, math_obj, "atanh", 5, js_mkfun_arity(builtin_Math_atanh, 1));
  defmethod(js, math_obj, "atan2", 5, js_mkfun_arity(builtin_Math_atan2, 2));
  defmethod(js, math_obj, "cbrt", 4, js_mkfun_arity(builtin_Math_cbrt, 1));
  defmethod(js, math_obj, "ceil", 4, js_mkfun_arity(builtin_Math_ceil, 1));
  defmethod(js, math_obj, "clz32", 5, js_mkfun_arity(builtin_Math_clz32, 1));
  defmethod(js, math_obj, "cos", 3, js_mkfun_arity(builtin_Math_cos, 1));
  defmethod(js, math_obj, "cosh", 4, js_mkfun_arity(builtin_Math_cosh, 1));
  defmethod(js, math_obj, "exp", 3, js_mkfun_arity(builtin_Math_exp, 1));
  defmethod(js, math_obj, "expm1", 5, js_mkfun_arity(builtin_Math_expm1, 1));
  defmethod(js, math_obj, "floor", 5, js_mkfun_arity(builtin_Math_floor, 1));
  defmethod(js, math_obj, "fround", 6, js_mkfun_arity(builtin_Math_fround, 1));
  defmethod(js, math_obj, "hypot", 5, js_mkfun_arity(builtin_Math_hypot, 2));
  defmethod(js, math_obj, "imul", 4, js_mkfun_arity(builtin_Math_imul, 2));
  defmethod(js, math_obj, "log", 3, js_mkfun_arity(builtin_Math_log, 1));
  defmethod(js, math_obj, "log1p", 5, js_mkfun_arity(builtin_Math_log1p, 1));
  defmethod(js, math_obj, "log10", 5, js_mkfun_arity(builtin_Math_log10, 1));
  defmethod(js, math_obj, "log2", 4, js_mkfun_arity(builtin_Math_log2, 1));
  defmethod(js, math_obj, "max", 3, js_mkfun_arity(builtin_Math_max, 2));
  defmethod(js, math_obj, "min", 3, js_mkfun_arity(builtin_Math_min, 2));
  defmethod(js, math_obj, "pow", 3, js_mkfun_arity(builtin_Math_pow, 2));
  defmethod(js, math_obj, "random", 6, js_mkfun_arity(builtin_Math_random, 0));
  defmethod(js, math_obj, "round", 5, js_mkfun_arity(builtin_Math_round, 1));
  defmethod(js, math_obj, "sign", 4, js_mkfun_arity(builtin_Math_sign, 1));
  defmethod(js, math_obj, "sin", 3, js_mkfun_arity(builtin_Math_sin, 1));
  defmethod(js, math_obj, "sinh", 4, js_mkfun_arity(builtin_Math_sinh, 1));
  defmethod(js, math_obj, "sqrt", 4, js_mkfun_arity(builtin_Math_sqrt, 1));
  defmethod(js, math_obj, "tan", 3, js_mkfun_arity(builtin_Math_tan, 1));
  defmethod(js, math_obj, "tanh", 4, js_mkfun_arity(builtin_Math_tanh, 1));
  defmethod(js, math_obj, "trunc", 5, js_mkfun_arity(builtin_Math_trunc, 1));
  
  js_set_sym(js, math_obj, get_toStringTag_sym(), js_mkstr(js, "Math", 4));
  js_set_global_builtin(js, "Math", math_obj);
}
