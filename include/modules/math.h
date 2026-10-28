#ifndef MATH_MODULE_H
#define MATH_MODULE_H

#include "types.h"

typedef enum {
  ANT_MATH_ABS,
  ANT_MATH_CEIL,
  ANT_MATH_FLOOR,
  ANT_MATH_ROUND,
  ANT_MATH_SIGN,
  ANT_MATH_SQRT,
  ANT_MATH_TRUNC,
  ANT_MATH_IMUL,
  ANT_MATH_MAX,
  ANT_MATH_MIN,
  ANT_MATH_INTRINSIC_COUNT,
} ant_math_intrinsic_t;

typedef enum {
#define ANT_MATH_DOUBLE(id, name, argc, expr) ANT_MATH_DOUBLE_##id,
#include "modules/math_doubles.h"
  ANT_MATH_DOUBLE_COUNT,
} ant_math_double_t;

static constexpr int ANT_MATH_FIRST_BINARY = ANT_MATH_IMUL;
extern const char *const ant_math_intrinsic_names[ANT_MATH_INTRINSIC_COUNT];
typedef double (*ant_math_double_fn_t)(double a, double b);

double ant_math_ceil(double x);
double ant_math_floor(double x);
double ant_math_round(double x);
double ant_math_sign(double x);
double ant_math_sqrt(double x);
double ant_math_trunc(double x);

double ant_math_imul(double a, double b);
double ant_math_max(double a, double b);
double ant_math_min(double a, double b);
double ant_math_js_pow(double base, double exponent);

void init_math_module(ant_t *js);
int ant_math_double_of(ant_value_t func);

typedef struct {
  const char *name;
  ant_math_double_fn_t fn;
  uint8_t argc;
} ant_math_double_info_t;

extern const ant_math_double_info_t ant_math_doubles[ANT_MATH_DOUBLE_COUNT];


#endif
