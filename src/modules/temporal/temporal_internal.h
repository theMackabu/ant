#ifndef ANT_MODULES_TEMPORAL_INTERNAL_H
#define ANT_MODULES_TEMPORAL_INTERNAL_H

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "ant.h"
#include "descriptors.h"
#include "errors.h"
#include "internal.h"
#include "ptr.h"
#include "modules/bigint.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wundef"
#include "AnyCalendarKind.h"
#include "Calendar.h"
#include "Duration.h"
#include "Instant.h"
#include "PlainDate.h"
#include "PlainDateTime.h"
#include "PlainMonthDay.h"
#include "PlainTime.h"
#include "PlainYearMonth.h"
#include "ParsedZonedDateTime.h"
#include "Provider.h"
#include "TimeZone.h"
#include "ZonedDateTime.h"
#pragma clang diagnostic pop
#include "modules/temporal_capi_ext.h"
#include "modules/symbol.h"

static constexpr uint32_t TEMPORAL_PROVIDER_TAG = 0x54505256u;        /* TPRV */
static constexpr uint32_t TEMPORAL_DURATION_TAG = 0x54445552u;        /* TDUR */
static constexpr uint32_t TEMPORAL_INSTANT_TAG = 0x54494e53u;         /* TINS */
static constexpr uint32_t TEMPORAL_PLAIN_DATE_TAG = 0x54504441u;      /* TPDA */
static constexpr uint32_t TEMPORAL_PLAIN_DATETIME_TAG = 0x54504454u;  /* TPDT */
static constexpr uint32_t TEMPORAL_PLAIN_MONTHDAY_TAG = 0x54504d44u;  /* TPMD */
static constexpr uint32_t TEMPORAL_PLAIN_TIME_TAG = 0x54505449u;      /* TPTI */
static constexpr uint32_t TEMPORAL_PLAIN_YEARMONTH_TAG = 0x5450594du; /* TPYM */
static constexpr uint32_t TEMPORAL_ZONED_DATETIME_TAG = 0x545a4454u;  /* TZDT */

typedef enum {
  TEMPORAL_DURATION,
  TEMPORAL_INSTANT,
  TEMPORAL_PLAIN_DATE,
  TEMPORAL_PLAIN_DATETIME,
  TEMPORAL_PLAIN_MONTHDAY,
  TEMPORAL_PLAIN_TIME,
  TEMPORAL_PLAIN_YEARMONTH,
  TEMPORAL_ZONED_DATETIME,
} temporal_kind_t;

typedef struct {
  PartialDate partial;
  ant_value_t month_code_root;
  ant_value_t era_root;
} temporal_partial_date_t;

typedef struct {
  ToStringRoundingOptions rounding;
  DisplayCalendar calendar;
  DisplayOffset offset;
  DisplayTimeZone time_zone_name;
  TimeZone_option time_zone;
} temporal_to_string_options_t;

enum {
  TEMPORAL_TOSTRING_CALENDAR       = 1u << 0,
  TEMPORAL_TOSTRING_DIGITS         = 1u << 1,
  TEMPORAL_TOSTRING_OFFSET         = 1u << 2,
  TEMPORAL_TOSTRING_ROUNDING_MODE  = 1u << 3,
  TEMPORAL_TOSTRING_SMALLEST_UNIT  = 1u << 4,
  TEMPORAL_TOSTRING_TIME_ZONE      = 1u << 5,
  TEMPORAL_TOSTRING_TIME_ZONE_NAME = 1u << 6,
};

typedef struct {
  RelativeTo value;
  PlainDate *date;
  ZonedDateTime *zoned;
} temporal_relative_to_t;

typedef struct {
  PartialZonedDateTime partial;
  ant_value_t month_code_root;
  ant_value_t era_root;
  ant_value_t offset_root;
} temporal_partial_zdt_t;

Provider *temporal_provider(ant_t *js);

size_t temporal_system_time_zone(char *buffer, size_t capacity);
uint32_t temporal_native_tag(temporal_kind_t kind);

ant_value_t temporal_make_function(
  ant_t *js, ant_cfunc_t callback,
  const char *name, size_t name_len, int length
);

ant_value_t temporal_require_new(ant_t *js, const char *name);
ant_value_t temporal_error(ant_t *js, TemporalError err);
ant_value_t temporal_wrap(ant_t *js, temporal_kind_t kind, void *ptr);
ant_value_t temporal_wrap_constructed(ant_t *js, temporal_kind_t kind, void *ptr);
ant_value_t temporal_string_from_write(ant_t *js, DiplomatWrite *write);
ant_value_t temporal_calendar_identifier(ant_t *js, const Calendar *calendar);
ant_value_t temporal_i128_to_bigint(ant_t *js, I128Nanoseconds value);
ant_value_t temporal_time_zone_identifier(ant_t *js, TimeZone zone);

bool temporal_string_equals(DiplomatStringView view, const char *literal);
bool temporal_to_number(ant_t *js, ant_value_t value, double *out, ant_value_t *err);
bool temporal_i128_from_value(ant_t *js, ant_value_t value, I128Nanoseconds *out, ant_value_t *err);
bool temporal_time_zone_from_value(ant_t *js, ant_value_t value, TimeZone *out, ant_value_t *err);
bool temporal_rounding_increment(ant_t *js, ant_value_t options, OptionU32 *out, ant_value_t *err);
bool temporal_duration_from_value(ant_t *js, ant_value_t value, Duration **out, ant_value_t *err);
bool temporal_validate_partial_object(ant_t *js, ant_value_t value, ant_value_t *err);

void *temporal_unwrap(
  ant_t *js, ant_value_t value,
  temporal_kind_t kind, const char *method,
  ant_value_t *err
);

bool temporal_to_string_view(
  ant_t *js, ant_value_t value,
  DiplomatStringView *out, ant_value_t *root,
  ant_value_t *err
);

bool temporal_integer(
  ant_t *js, ant_value_t value,
  int64_t default_value, int64_t *out,
  ant_value_t *err
);

bool temporal_integral(
  ant_t *js, ant_value_t value,
  int64_t default_value, int64_t *out,
  ant_value_t *err
);

bool temporal_partial_time(
  ant_t *js, ant_value_t value,
  PartialTime *out, bool require_any,
  ant_value_t *err
);

bool temporal_calendar_kind(
  ant_t *js, ant_value_t value,
  AnyCalendarKind default_kind, AnyCalendarKind *out,
  ant_value_t *err
);

bool temporal_calendar_identifier_kind(
  ant_t *js, ant_value_t value,
  AnyCalendarKind default_kind, AnyCalendarKind *out,
  ant_value_t *err
);

bool temporal_calendar_kind_from_property(
  ant_t *js, ant_value_t value,
  AnyCalendarKind default_kind, AnyCalendarKind *out,
  ant_value_t *err
);

bool temporal_month_code_syntax(
  ant_t *js, ant_value_t value,
  DiplomatStringView *view, ant_value_t *root,
  ant_value_t *err
);

bool temporal_partial_date_impl(
  ant_t *js, ant_value_t value,
  AnyCalendarKind default_calendar, temporal_partial_date_t *out,
  bool include_day, bool require_any, bool read_calendar,
  ant_value_t *err
);

bool temporal_partial_date(
  ant_t *js, ant_value_t value,
  AnyCalendarKind default_calendar, temporal_partial_date_t *out,
  bool include_day, bool require_any,
  ant_value_t *err
);

bool temporal_integer_property(
  ant_t *js, ant_value_t object, const char *name,
  int64_t minimum, int64_t maximum,
  bool *present, int64_t *integer, ant_value_t *err
);

bool temporal_options_object(
  ant_t *js, ant_value_t value,
  bool string_allowed, ant_value_t *out,
  ant_value_t *err
);

bool temporal_unit_from_value(
  ant_t *js, ant_value_t value,
  bool allow_auto, Unit *out,
  ant_value_t *err
);

bool temporal_rounding_mode_from_value(
  ant_t *js, ant_value_t value,
  RoundingMode *out, ant_value_t *err
);

bool temporal_rounding_options(
  ant_t *js, ant_value_t input,
  bool required, bool allow_auto,
  RoundingOptions *out, ant_value_t *err
);

bool temporal_difference_settings(
  ant_t *js, ant_value_t input,
  DifferenceSettings *out, ant_value_t *err
);

bool temporal_overflow_option(
  ant_t *js, ant_value_t input,
  ArithmeticOverflow_option *out, ant_value_t *err
);

bool temporal_disambiguation_value(
  ant_t *js, ant_value_t value,
  Disambiguation *out, ant_value_t *err
);

bool temporal_offset_disambiguation_value(
  ant_t *js, ant_value_t value,
  OffsetDisambiguation *out, ant_value_t *err
);

bool temporal_zdt_options(
  ant_t *js, ant_value_t input,
  Disambiguation_option *disambiguation,
  OffsetDisambiguation_option *offset,
  ArithmeticOverflow_option *overflow,
  ant_value_t *err
);

bool temporal_to_string_options(
  ant_t *js, ant_value_t input, unsigned fields,
  temporal_to_string_options_t *out, ant_value_t *err
);

bool temporal_relative_to(
  ant_t *js, ant_value_t input,
  temporal_relative_to_t *out, ant_value_t *err
);

bool temporal_relative_to_from_options(
  ant_t *js, ant_value_t options,
  temporal_relative_to_t *out, ant_value_t *err
);

void temporal_set_namespace_property(
  ant_t *js, ant_value_t temporal,
  const char *name, ant_value_t value
);

bool temporal_plain_time_from_value(
  ant_t *js, ant_value_t value,
  PlainTime **out, ant_value_t *err
);

bool temporal_partial_zdt(
  ant_t *js, ant_value_t value,
  AnyCalendarKind default_calendar, temporal_partial_zdt_t *out,
  bool require_any, bool require_time_zone,
  bool read_calendar, bool read_time_zone,
  bool *has_time_zone, ant_value_t *err
);

void temporal_set_to_string_tag(ant_t *js, ant_value_t obj, const char *tag);
void temporal_set_length(ant_t *js, ant_value_t fn, int length);
void temporal_relative_to_destroy(temporal_relative_to_t *relative);
void temporal_init_duration(ant_t *js, ant_value_t temporal);
void temporal_init_instant(ant_t *js, ant_value_t temporal);
void temporal_init_plain_date(ant_t *js, ant_value_t temporal);
void temporal_init_plain_datetime(ant_t *js, ant_value_t temporal);
void temporal_init_plain_monthday(ant_t *js, ant_value_t temporal);
void temporal_init_plain_time(ant_t *js, ant_value_t temporal);
void temporal_init_plain_yearmonth(ant_t *js, ant_value_t temporal);
void temporal_init_zdt(ant_t *js, ant_value_t temporal);
void temporal_init_now(ant_t *js, ant_value_t temporal);

#define TEMPORAL_METHOD(js, obj, name, fn, arity)                       \
  defmethod((js), (obj), (name), sizeof(name) - 1,                      \
  temporal_make_function((js), (fn), (name), sizeof(name) - 1, (arity)))

#define TEMPORAL_GETTER(js, obj, name, fn)                                               \
  js_set_getter_desc((js), (obj), (name), sizeof(name) - 1,                              \
  temporal_make_function((js), (fn), "get " name, sizeof("get " name) - 1, 0), JS_DESC_C)

#endif
