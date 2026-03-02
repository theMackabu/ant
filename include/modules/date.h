#ifndef DATE_H
#define DATE_H

#include "types.h"

#define DATE_METHOD_ENTRY(name_lit, fn_ptr) \
  { name_lit, sizeof(name_lit) - 1, fn_ptr }
  
#define DATE_COUNT_OF(a) \
  ((int)(sizeof(a) / sizeof((a)[0])))

typedef struct {
  double year;
  double month;
  double day;
  double hour;
  double minute;
  double second;
  double millisecond;
  double weekday;
  double tz_minutes;
} date_fields_t;

typedef enum {
  DATE_FIELD_YEAR = 0,
  DATE_FIELD_MONTH = 1,
  DATE_FIELD_DAY_OF_MONTH = 2,
  DATE_FIELD_HOUR = 3,
  DATE_FIELD_MINUTE = 4,
  DATE_FIELD_SECOND = 5,
  DATE_FIELD_MILLISECOND = 6,
  DATE_FIELD_DAY_OF_WEEK = 7,
  DATE_FIELD_TZ_MINUTES = 8,
} date_field_index_t;

typedef enum {
  DATE_STRING_FMT_UTC = 0,
  DATE_STRING_FMT_LOCAL = 1,
  DATE_STRING_FMT_ISO = 2,
  DATE_STRING_FMT_LOCALE = 3,
} date_string_fmt_t;

typedef enum {
  DATE_STRING_PART_DATE = 1,
  DATE_STRING_PART_TIME = 2,
  DATE_STRING_PART_ALL = 3,
} date_string_part_t;

typedef struct {
  date_string_fmt_t fmt;
  date_string_part_t part;
} date_string_spec_t;

typedef struct {
  date_field_index_t field;
  bool local_time;
  bool legacy_get_year;
} date_get_field_spec_t;

typedef struct {
  date_field_index_t first_field;
  date_field_index_t end_field_exclusive;
  bool local_time;
} date_set_field_spec_t;

typedef struct {
  const char *name;
  size_t len;
  jsval_t (*fn)(ant_t *, jsval_t *, int);
} date_method_entry_t;

void init_date_module(void);

jsval_t get_date_string(
  ant_t *js, jsval_t this_val, 
  date_string_spec_t spec
);

#endif
