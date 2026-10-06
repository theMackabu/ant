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

static constexpr int ANT_MATH_FIRST_BINARY = ANT_MATH_IMUL;
extern const char *const ant_math_intrinsic_names[ANT_MATH_INTRINSIC_COUNT];

double ant_math_ceil(double x);
double ant_math_floor(double x);
double ant_math_round(double x);
double ant_math_sign(double x);
double ant_math_sqrt(double x);
double ant_math_trunc(double x);
double ant_math_imul(double a, double b);
double ant_math_max(double a, double b);
double ant_math_min(double a, double b);

void init_math_module(ant_t *js);

#endif
