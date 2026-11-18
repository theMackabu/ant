#pragma once

#include "internal.h"

enum { ANT_NUMBER_LOCALE_CHARS = 640 };

typedef struct {
  uint8_t min_integer;
  uint8_t min_fraction;
  uint8_t max_fraction;
  bool percent;
  bool ungrouped;
} ant_number_locale_t;

static const ant_number_locale_t ANT_NUMBER_LOCALE_DEFAULT = { .min_integer = 1, .max_fraction = 3 };

ant_value_t builtin_number_toLocaleString(ant_params_t);
ant_value_t Ant_Number_ReadLocaleOptions(ant_t *js, ant_value_t options, ant_number_locale_t *out);

size_t Ant_Number_FormatLocale(double value, const ant_number_locale_t *format, char *buf, size_t capacity);
size_t format_number_to_locale_string(ant_value_t num, char *buf, size_t capacity);

void js_number_init_prototype(ant_t *js, ant_value_t number_proto);
void js_number_init_globals(ant_t *js, ant_value_t glob);
void js_number_init_constructor(ant_t *js, ant_value_t number_ctor_obj, ant_value_t number_proto, ant_value_t *parse_int, ant_value_t *parse_float);
