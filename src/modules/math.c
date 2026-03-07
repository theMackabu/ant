#include <stdlib.h>
#include <time.h>

#include "ant.h"
#include "internal.h"
#include "runtime.h"

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
  double x = js_to_number(js, args[0]);
  if (isnan(x) || isinf(x)) return tov(32);
  uint32_t n = (uint32_t)x;
  if (n == 0) return tov(32);
  int count = 0;
  while ((n & 0x80000000U) == 0) { count++; n <<= 1; }
  return tov((double)count);
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
  double sum = 0.0;
  for (int i = 0; i < nargs; i++) {
    double v = js_to_number(js, args[i]);
    if (isnan(v)) return tov(JS_NAN);
    sum += v * v;
  }
  return tov(sqrt(sum));
}

static int32_t toInt32(double d) {
  if (isnan(d) || isinf(d) || d == 0) return 0;
  double int_val = trunc(d);
  double two32 = (double)(1ULL << 32);
  double two31 = (double)(1ULL << 31);
  double mod_val = fmod(int_val, two32);
  if (mod_val < 0) mod_val += two32;
  if (mod_val >= two31) mod_val -= two32;
  return (int32_t)mod_val;
}

static ant_value_t builtin_Math_imul(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return tov(0);
  int32_t a = toInt32(js_to_number(js, args[0]));
  int32_t b = toInt32(js_to_number(js, args[1]));
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
  double max_val = JS_NEG_INF;
  for (int i = 0; i < nargs; i++) {
    double v = js_to_number(js, args[i]);
    if (isnan(v)) return tov(JS_NAN);
    if (v > max_val) max_val = v;
  }
  return tov(max_val);
}

static ant_value_t builtin_Math_min(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return tov(JS_INF);
  double min_val = JS_INF;
  for (int i = 0; i < nargs; i++) {
    double v = js_to_number(js, args[i]);
    if (isnan(v)) return tov(JS_NAN);
    if (v < min_val) min_val = v;
  }
  return tov(min_val);
}

static ant_value_t builtin_Math_pow(ant_t *js, ant_value_t *args, int nargs) {
  double base = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  double exp = (nargs < 2) ? JS_NAN : js_to_number(js, args[1]);
  if (isnan(base) || isnan(exp)) return tov(JS_NAN);
  return tov(pow(base, exp));
}

static bool random_seeded = false;

static ant_value_t builtin_Math_random(ant_t *js, ant_value_t *args, int nargs) {
  (void)js;
  (void)args;
  (void)nargs;
  if (!random_seeded) {
    srand((unsigned int)time(NULL));
    random_seeded = true;
  }
  return tov((double)rand() / ((double)RAND_MAX + 1.0));
}

static ant_value_t builtin_Math_round(ant_t *js, ant_value_t *args, int nargs) {
  double x = (nargs < 1) ? JS_NAN : js_to_number(js, args[0]);
  if (isnan(x) || isinf(x)) return tov(x);
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
  
  js_set_proto(js, math_obj, object_proto);
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
