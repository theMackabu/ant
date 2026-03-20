#include <sodium.h>
#include <float.h>

#include "ant.h"
#include "errors.h"
#include "internal.h"
#include "runtime.h"
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

static ant_value_t builtin_Math_abs(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(fabs(x));
}

static ant_value_t builtin_Math_acos(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(acos(x));
}

static ant_value_t builtin_Math_acosh(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(acosh(x));
}

static ant_value_t builtin_Math_asin(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(asin(x));
}

static ant_value_t builtin_Math_asinh(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(asinh(x));
}

static ant_value_t builtin_Math_atan(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(atan(x));
}

static ant_value_t builtin_Math_atanh(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(atanh(x));
}

static ant_value_t builtin_Math_atan2(ant_t *js, ant_value_t *args, int nargs) {
  double y = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  double x = (nargs < 2) ? JS_NAN : js_to_number(js, args[1]);
  if (isnan(y) || isnan(x)) return tov(JS_NAN);
  return tov(atan2(y, x));
}

static ant_value_t builtin_Math_cbrt(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(cbrt(x));
}

static ant_value_t builtin_Math_ceil(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(ceil(x));
}

static ant_value_t builtin_Math_clz32(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return tov(32);
  uint32_t n = js_to_uint32(js_to_number(js, args[0]));
  if (n == 0) return tov(32);
  int lz = __builtin_clz(n);
  if (sizeof(unsigned int) > sizeof(uint32_t)) {
    lz -= (int)((sizeof(unsigned int) - sizeof(uint32_t)) * 8);
  }
  return tov((double)lz);
}

static ant_value_t builtin_Math_cos(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(cos(x));
}

static ant_value_t builtin_Math_cosh(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(cosh(x));
}

static ant_value_t builtin_Math_exp(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(exp(x));
}

static ant_value_t builtin_Math_expm1(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(expm1(x));
}

static ant_value_t builtin_Math_floor(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(floor(x));
}

static ant_value_t builtin_Math_fround(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov((double)(float)x);
}

static ant_value_t builtin_Math_hypot(ant_t *js, ant_value_t *args, int nargs) {
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

static ant_value_t builtin_Math_imul(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return tov(0);
  int32_t a = js_to_int32(js_to_number(js, args[0]));
  int32_t b = js_to_int32(js_to_number(js, args[1]));
  return tov((double)((int32_t)((uint32_t)a * (uint32_t)b)));
}

static ant_value_t builtin_Math_log(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(log(x));
}

static ant_value_t builtin_Math_log1p(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(log1p(x));
}

static ant_value_t builtin_Math_log10(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(log10(x));
}

static ant_value_t builtin_Math_log2(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(log2(x));
}

static ant_value_t builtin_Math_max(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return tov(JS_NEG_INF);
  double max_val = js_to_number(js, args[0]);
  if (isnan(max_val)) return tov(JS_NAN);
  for (int i = 1; i < nargs; i++) {
    double v = js_to_number(js, args[i]);
    if (isnan(v)) return tov(JS_NAN);
    if (v > max_val) { max_val = v; continue; }
    if (v == 0.0 && max_val == 0.0 && !signbit(v) && signbit(max_val)) {
      max_val = v;
    }
  }
  return tov(max_val);
}

static ant_value_t builtin_Math_min(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return tov(JS_INF);
  double min_val = js_to_number(js, args[0]);
  if (isnan(min_val)) return tov(JS_NAN);
  for (int i = 1; i < nargs; i++) {
    double v = js_to_number(js, args[i]);
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

static ant_value_t builtin_Math_pow(ant_t *js, ant_value_t *args, int nargs) {
  double base = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  double exp = (nargs < 2) ? JS_NAN : js_to_number(js, args[1]);
  if (isnan(base) || isnan(exp)) return tov(JS_NAN);
  return tov(pow(base, exp));
}

static ant_value_t builtin_Math_random(ant_t *js, ant_value_t *args, int nargs) {
  if (ensure_crypto_init() < 0) {
    return js_mkerr(js, "libsodium initialization failed");
  }

  uint64_t r = 0; randombytes_buf(&r, sizeof(r));
  uint64_t fraction = r >> MATH_RANDOM_DISCARD_BITS;
  
  return tov((double)fraction * math_random_scale);
}

static ant_value_t builtin_Math_round(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x) || isinf(x) || x == 0.0) return tov(x);
  if (x < 0.0 && x >= -0.5) return tov(-0.0);
  return tov(floor(x + 0.5));
}

static ant_value_t builtin_Math_sign(ant_t *js, ant_value_t *args, int nargs) {
  double v = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(v)) return tov(JS_NAN);
  if (v > 0) return tov(1.0);
  if (v < 0) return tov(-1.0);
  return tov(v);
}

static ant_value_t builtin_Math_sin(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(sin(x));
}

static ant_value_t builtin_Math_sinh(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(sinh(x));
}

static ant_value_t builtin_Math_sqrt(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(sqrt(x));
}

static ant_value_t builtin_Math_tan(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(tan(x));
}

static ant_value_t builtin_Math_tanh(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(tanh(x));
}

static ant_value_t builtin_Math_trunc(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x)) return tov(JS_NAN);
  return tov(trunc(x));
}

void init_math_module(void) {
  ant_t *js = rt->js;
  
  ant_value_t glob = js_glob(js);
  ant_value_t object_proto = js->object;
  ant_value_t math_obj = mkobj(js, 0);
  
  js_set_proto_init(math_obj, object_proto);
  js_setprop(js, math_obj, js_mkstr(js, "E", 1), tov(M_E));
  js_setprop(js, math_obj, js_mkstr(js, "LN10", 4), tov(M_LN10));
  js_setprop(js, math_obj, js_mkstr(js, "LN2", 3), tov(M_LN2));
  js_setprop(js, math_obj, js_mkstr(js, "LOG10E", 6), tov(M_LOG10E));
  js_setprop(js, math_obj, js_mkstr(js, "LOG2E", 5), tov(M_LOG2E));
  js_setprop(js, math_obj, js_mkstr(js, "PI", 2), tov(M_PI));
  js_setprop(js, math_obj, js_mkstr(js, "SQRT1_2", 7), tov(M_SQRT1_2));
  js_setprop(js, math_obj, js_mkstr(js, "SQRT2", 5), tov(M_SQRT2));
  js_setprop(js, math_obj, js_mkstr(js, "abs", 3), js_mkfun(builtin_Math_abs));
  js_setprop(js, math_obj, js_mkstr(js, "acos", 4), js_mkfun(builtin_Math_acos));
  js_setprop(js, math_obj, js_mkstr(js, "acosh", 5), js_mkfun(builtin_Math_acosh));
  js_setprop(js, math_obj, js_mkstr(js, "asin", 4), js_mkfun(builtin_Math_asin));
  js_setprop(js, math_obj, js_mkstr(js, "asinh", 5), js_mkfun(builtin_Math_asinh));
  js_setprop(js, math_obj, js_mkstr(js, "atan", 4), js_mkfun(builtin_Math_atan));
  js_setprop(js, math_obj, js_mkstr(js, "atanh", 5), js_mkfun(builtin_Math_atanh));
  js_setprop(js, math_obj, js_mkstr(js, "atan2", 5), js_mkfun(builtin_Math_atan2));
  js_setprop(js, math_obj, js_mkstr(js, "cbrt", 4), js_mkfun(builtin_Math_cbrt));
  js_setprop(js, math_obj, js_mkstr(js, "ceil", 4), js_mkfun(builtin_Math_ceil));
  js_setprop(js, math_obj, js_mkstr(js, "clz32", 5), js_mkfun(builtin_Math_clz32));
  js_setprop(js, math_obj, js_mkstr(js, "cos", 3), js_mkfun(builtin_Math_cos));
  js_setprop(js, math_obj, js_mkstr(js, "cosh", 4), js_mkfun(builtin_Math_cosh));
  js_setprop(js, math_obj, js_mkstr(js, "exp", 3), js_mkfun(builtin_Math_exp));
  js_setprop(js, math_obj, js_mkstr(js, "expm1", 5), js_mkfun(builtin_Math_expm1));
  js_setprop(js, math_obj, js_mkstr(js, "floor", 5), js_mkfun(builtin_Math_floor));
  js_setprop(js, math_obj, js_mkstr(js, "fround", 6), js_mkfun(builtin_Math_fround));
  js_setprop(js, math_obj, js_mkstr(js, "hypot", 5), js_mkfun(builtin_Math_hypot));
  js_setprop(js, math_obj, js_mkstr(js, "imul", 4), js_mkfun(builtin_Math_imul));
  js_setprop(js, math_obj, js_mkstr(js, "log", 3), js_mkfun(builtin_Math_log));
  js_setprop(js, math_obj, js_mkstr(js, "log1p", 5), js_mkfun(builtin_Math_log1p));
  js_setprop(js, math_obj, js_mkstr(js, "log10", 5), js_mkfun(builtin_Math_log10));
  js_setprop(js, math_obj, js_mkstr(js, "log2", 4), js_mkfun(builtin_Math_log2));
  js_setprop(js, math_obj, js_mkstr(js, "max", 3), js_mkfun(builtin_Math_max));
  js_setprop(js, math_obj, js_mkstr(js, "min", 3), js_mkfun(builtin_Math_min));
  js_setprop(js, math_obj, js_mkstr(js, "pow", 3), js_mkfun(builtin_Math_pow));
  js_setprop(js, math_obj, js_mkstr(js, "random", 6), js_mkfun(builtin_Math_random));
  js_setprop(js, math_obj, js_mkstr(js, "round", 5), js_mkfun(builtin_Math_round));
  js_setprop(js, math_obj, js_mkstr(js, "sign", 4), js_mkfun(builtin_Math_sign));
  js_setprop(js, math_obj, js_mkstr(js, "sin", 3), js_mkfun(builtin_Math_sin));
  js_setprop(js, math_obj, js_mkstr(js, "sinh", 4), js_mkfun(builtin_Math_sinh));
  js_setprop(js, math_obj, js_mkstr(js, "sqrt", 4), js_mkfun(builtin_Math_sqrt));
  js_setprop(js, math_obj, js_mkstr(js, "tan", 3), js_mkfun(builtin_Math_tan));
  js_setprop(js, math_obj, js_mkstr(js, "tanh", 4), js_mkfun(builtin_Math_tanh));
  js_setprop(js, math_obj, js_mkstr(js, "trunc", 5), js_mkfun(builtin_Math_trunc));
  js_setprop(js, glob, js_mkstr(js, "Math", 4), math_obj);
}
