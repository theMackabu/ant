#include "modules/temporal.h"

#ifdef ANT_HAVE_TEMPORAL
#  include "temporal_internal.h"

typedef struct {
  PartialDateTime partial;
  ant_value_t month_code_root;
  ant_value_t era_root;
} temporal_partial_datetime_t;

static bool temporal_partial_datetime_impl(
  ant_t *js,
  ant_value_t value,
  AnyCalendarKind default_calendar,
  temporal_partial_datetime_t *out,
  bool require_any,
  bool read_calendar,
  ant_value_t *err
) {
  if (!is_object_type(value)) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "Date-time-like value must be an object");
    return false;
  }
  memset(out, 0, sizeof(*out));
  out->month_code_root = js_mkundef();
  out->era_root = js_mkundef();
  out->partial.date.calendar = default_calendar;
  if (read_calendar) {
    ant_value_t calendar = js_get(js, value, "calendar");
    if (is_err(calendar)) {
      *err = calendar;
      return false;
    }
    if (!temporal_calendar_kind_from_property(js, calendar, default_calendar, &out->partial.date.calendar, err))
      return false;
  }
  bool any = false, present;
  int64_t integer;
#  define DATETIME_INTEGER(name, minimum, maximum, target, option_type, cast_type)                                     \
    do {                                                                                                               \
      if (!temporal_integer_property(js, value, (name), (minimum), (maximum), &present, &integer, err)) return false;  \
      if (present) {                                                                                                   \
        (target) = (option_type){.ok = (cast_type)integer, .is_ok = true};                                             \
        any = true;                                                                                                    \
      }                                                                                                                \
    } while (0)
  DATETIME_INTEGER("day", 1, UINT8_MAX, out->partial.date.day, OptionU8, uint8_t);
  if (out->partial.date.calendar != AnyCalendarKind_Iso) {
    ant_value_t era = js_get(js, value, "era");
    if (is_err(era)) {
      *err = era;
      return false;
    }
    if (vtype(era) != kTypeUndefined) {
      if (!temporal_to_string_view(js, era, &out->partial.date.era, &out->era_root, err)) return false;
      any = true;
    }
    DATETIME_INTEGER("eraYear", INT32_MIN, INT32_MAX, out->partial.date.era_year, OptionI32, int32_t);
  }
  DATETIME_INTEGER("hour", 0, UINT8_MAX, out->partial.time.hour, OptionU8, uint8_t);
  DATETIME_INTEGER("microsecond", 0, UINT16_MAX, out->partial.time.microsecond, OptionU16, uint16_t);
  DATETIME_INTEGER("millisecond", 0, UINT16_MAX, out->partial.time.millisecond, OptionU16, uint16_t);
  DATETIME_INTEGER("minute", 0, UINT8_MAX, out->partial.time.minute, OptionU8, uint8_t);
  DATETIME_INTEGER("month", 1, UINT8_MAX, out->partial.date.month, OptionU8, uint8_t);
  ant_value_t month_code = js_get(js, value, "monthCode");
  if (is_err(month_code)) {
    *err = month_code;
    return false;
  }
  if (vtype(month_code) != kTypeUndefined) {
    if (!temporal_month_code_syntax(js, month_code, &out->partial.date.month_code, &out->month_code_root, err))
      return false;
    any = true;
  }
  DATETIME_INTEGER("nanosecond", 0, UINT16_MAX, out->partial.time.nanosecond, OptionU16, uint16_t);
  DATETIME_INTEGER("second", 0, UINT8_MAX, out->partial.time.second, OptionU8, uint8_t);
  DATETIME_INTEGER("year", INT32_MIN, INT32_MAX, out->partial.date.year, OptionI32, int32_t);
#  undef DATETIME_INTEGER
  if (require_any && !any) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "Date-time-like object must have at least one field");
    return false;
  }
  return true;
}

static bool temporal_partial_datetime(
  ant_t *js,
  ant_value_t value,
  AnyCalendarKind default_calendar,
  temporal_partial_datetime_t *out,
  bool require_any,
  ant_value_t *err
) {
  return temporal_partial_datetime_impl(js, value, default_calendar, out, require_any, true, err);
}

static bool temporal_plain_datetime_from_value(ant_t *js, ant_value_t value, PlainDateTime **out, ant_value_t *err) {
  PlainDateTime *datetime = is_object_type(value) ? js_get_native(value, TEMPORAL_PLAIN_DATETIME_TAG) : NULL;
  if (datetime) {
    *out = temporal_rs_PlainDateTime_clone(datetime);
    return true;
  }
  PlainDate *date = is_object_type(value) ? js_get_native(value, TEMPORAL_PLAIN_DATE_TAG) : NULL;
  if (date) {
    temporal_rs_PlainDate_to_plain_date_time_result result = temporal_rs_PlainDate_to_plain_date_time(date, NULL);
    if (!result.is_ok) {
      *err = temporal_error(js, result.err);
      return false;
    }
    *out = result.ok;
    return true;
  }
  ZonedDateTime *zdt = is_object_type(value) ? js_get_native(value, TEMPORAL_ZONED_DATETIME_TAG) : NULL;
  if (zdt) {
    PlainDate *zdt_date = temporal_rs_ZonedDateTime_to_plain_date(zdt);
    PlainTime *zdt_time = temporal_rs_ZonedDateTime_to_plain_time(zdt);
    temporal_rs_PlainDate_to_plain_date_time_result result =
      temporal_rs_PlainDate_to_plain_date_time(zdt_date, zdt_time);
    temporal_rs_PlainDate_destroy(zdt_date);
    temporal_rs_PlainTime_destroy(zdt_time);
    if (!result.is_ok) {
      *err = temporal_error(js, result.err);
      return false;
    }
    *out = result.ok;
    return true;
  }
  if (vtype(value) == kTypeString) {
    DiplomatStringView view;
    ant_value_t root;
    if (!temporal_to_string_view(js, value, &view, &root, err)) return false;
    temporal_rs_PlainDateTime_from_utf8_result result = temporal_rs_PlainDateTime_from_utf8(view);
    if (!result.is_ok) {
      *err = temporal_error(js, result.err);
      return false;
    }
    *out = result.ok;
    return true;
  }
  temporal_partial_datetime_t partial;
  if (!temporal_partial_datetime(js, value, AnyCalendarKind_Iso, &partial, true, err)) return false;
  ArithmeticOverflow_option overflow = {0};
  temporal_rs_PlainDateTime_from_partial_result result =
    temporal_rs_PlainDateTime_from_partial(partial.partial, overflow);
  if (!result.is_ok) {
    *err = temporal_error(js, result.err);
    return false;
  }
  *out = result.ok;
  return true;
}

static ant_value_t temporal_plain_datetime_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == kTypeUndefined) return temporal_require_new(js, "Temporal.PlainDateTime");
  int64_t fields[9] = {0};
  ant_value_t err = js_mkundef();
  if (
    (nargs > 0 && vtype(args[0]) == kTypeUndefined) || (nargs > 1 && vtype(args[1]) == kTypeUndefined) ||
    (nargs > 2 && vtype(args[2]) == kTypeUndefined)
  )
    return js_mkerr_typed(js, JS_ERR_RANGE, "Temporal.PlainDateTime date fields must be finite numbers");
  for (int i = 0; i < 9 && i < nargs; i++)
    if (!temporal_integer(js, args[i], 0, &fields[i], &err)) return err;
  if (
    fields[0] < INT32_MIN || fields[0] > INT32_MAX || fields[1] < 0 || fields[1] > UINT8_MAX || fields[2] < 0 ||
    fields[2] > UINT8_MAX || fields[3] < 0 || fields[3] > UINT8_MAX || fields[4] < 0 || fields[4] > UINT8_MAX ||
    fields[5] < 0 || fields[5] > UINT8_MAX || fields[6] < 0 || fields[6] > UINT16_MAX || fields[7] < 0 ||
    fields[7] > UINT16_MAX || fields[8] < 0 || fields[8] > UINT16_MAX
  )
    return js_mkerr_typed(js, JS_ERR_RANGE, "Temporal.PlainDateTime field is outside the supported range");
  AnyCalendarKind calendar;
  if (!temporal_calendar_identifier_kind(js, nargs > 9 ? args[9] : js_mkundef(), AnyCalendarKind_Iso, &calendar, &err))
    return err;
  temporal_rs_PlainDateTime_try_new_result result = temporal_rs_PlainDateTime_try_new(
    (int32_t)fields[0],
    (uint8_t)fields[1],
    (uint8_t)fields[2],
    (uint8_t)fields[3],
    (uint8_t)fields[4],
    (uint8_t)fields[5],
    (uint16_t)fields[6],
    (uint16_t)fields[7],
    (uint16_t)fields[8],
    calendar
  );
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap_constructed(js, TEMPORAL_PLAIN_DATETIME, result.ok);
}

static ant_value_t temporal_plain_datetime_from(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Temporal.PlainDateTime.from requires an argument");
  ant_value_t err = js_mkundef();
  ArithmeticOverflow_option overflow = {0};
  if (
    vtype(args[0]) == kTypeString || js_get_native(args[0], TEMPORAL_PLAIN_DATETIME_TAG) ||
    js_get_native(args[0], TEMPORAL_PLAIN_DATE_TAG) || js_get_native(args[0], TEMPORAL_ZONED_DATETIME_TAG)
  ) {
    PlainDateTime *value;
    if (!temporal_plain_datetime_from_value(js, args[0], &value, &err)) return err;
    if (!temporal_overflow_option(js, nargs > 1 ? args[1] : js_mkundef(), &overflow, &err)) {
      temporal_rs_PlainDateTime_destroy(value);
      return err;
    }
    return temporal_wrap(js, TEMPORAL_PLAIN_DATETIME, value);
  }
  temporal_partial_datetime_t partial;
  if (!temporal_partial_datetime(js, args[0], AnyCalendarKind_Iso, &partial, true, &err)) return err;
  if (!temporal_overflow_option(js, nargs > 1 ? args[1] : js_mkundef(), &overflow, &err)) return err;
  temporal_rs_PlainDateTime_from_partial_result result =
    temporal_rs_PlainDateTime_from_partial(partial.partial, overflow);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap(js, TEMPORAL_PLAIN_DATETIME, result.ok);
}

static ant_value_t temporal_plain_datetime_compare(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "Temporal.PlainDateTime.compare requires two arguments");
  PlainDateTime *one = NULL, *two = NULL;
  ant_value_t err = js_mkundef();
  if (!temporal_plain_datetime_from_value(js, args[0], &one, &err)) return err;
  if (!temporal_plain_datetime_from_value(js, args[1], &two, &err)) {
    temporal_rs_PlainDateTime_destroy(one);
    return err;
  }
  int8_t comparison = temporal_rs_PlainDateTime_compare(one, two);
  temporal_rs_PlainDateTime_destroy(one);
  temporal_rs_PlainDateTime_destroy(two);
  return js_mknum(comparison);
}

static PlainDateTime *temporal_plain_datetime_this(ant_t *js, const char *method, ant_value_t *err) {
  return temporal_unwrap(js, js_getthis(js), TEMPORAL_PLAIN_DATETIME, method, err);
}

#  define PLAIN_DATETIME_NUMBER_GETTER(name, capi)                                                                     \
    static ant_value_t temporal_plain_datetime_get_##name(ant_t *js, ant_value_t *args, int nargs) {                   \
      (void)args;                                                                                                      \
      (void)nargs;                                                                                                     \
      ant_value_t err = js_mkundef();                                                                                  \
      PlainDateTime *self = temporal_plain_datetime_this(js, "Temporal.PlainDateTime.prototype." #name, &err);         \
      return self ? js_mknum((double)capi(self)) : err;                                                                \
    }

PLAIN_DATETIME_NUMBER_GETTER(year, temporal_rs_PlainDateTime_year)
PLAIN_DATETIME_NUMBER_GETTER(month, temporal_rs_PlainDateTime_month)
PLAIN_DATETIME_NUMBER_GETTER(day, temporal_rs_PlainDateTime_day)
PLAIN_DATETIME_NUMBER_GETTER(hour, temporal_rs_PlainDateTime_hour)
PLAIN_DATETIME_NUMBER_GETTER(minute, temporal_rs_PlainDateTime_minute)
PLAIN_DATETIME_NUMBER_GETTER(second, temporal_rs_PlainDateTime_second)
PLAIN_DATETIME_NUMBER_GETTER(millisecond, temporal_rs_PlainDateTime_millisecond)
PLAIN_DATETIME_NUMBER_GETTER(microsecond, temporal_rs_PlainDateTime_microsecond)
PLAIN_DATETIME_NUMBER_GETTER(nanosecond, temporal_rs_PlainDateTime_nanosecond)
PLAIN_DATETIME_NUMBER_GETTER(day_of_week, temporal_rs_PlainDateTime_day_of_week)
PLAIN_DATETIME_NUMBER_GETTER(day_of_year, temporal_rs_PlainDateTime_day_of_year)
PLAIN_DATETIME_NUMBER_GETTER(days_in_week, temporal_rs_PlainDateTime_days_in_week)
PLAIN_DATETIME_NUMBER_GETTER(days_in_month, temporal_rs_PlainDateTime_days_in_month)
PLAIN_DATETIME_NUMBER_GETTER(days_in_year, temporal_rs_PlainDateTime_days_in_year)
PLAIN_DATETIME_NUMBER_GETTER(months_in_year, temporal_rs_PlainDateTime_months_in_year)

static ant_value_t temporal_plain_datetime_get_calendar_id(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  PlainDateTime *self = temporal_plain_datetime_this(js, "Temporal.PlainDateTime.prototype.calendarId", &err);
  return self ? temporal_calendar_identifier(js, temporal_rs_PlainDateTime_calendar(self)) : err;
}
static ant_value_t temporal_plain_datetime_get_month_code(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  PlainDateTime *self = temporal_plain_datetime_this(js, "Temporal.PlainDateTime.prototype.monthCode", &err);
  if (!self) return err;
  DiplomatWrite *write = diplomat_buffer_write_create(4);
  temporal_rs_PlainDateTime_month_code(self, write);
  return temporal_string_from_write(js, write);
}
static ant_value_t temporal_plain_datetime_get_week_of_year(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  PlainDateTime *self = temporal_plain_datetime_this(js, "Temporal.PlainDateTime.prototype.weekOfYear", &err);
  if (!self) return err;
  temporal_rs_PlainDateTime_week_of_year_result r = temporal_rs_PlainDateTime_week_of_year(self);
  return r.is_ok ? js_mknum(r.ok) : js_mkundef();
}
static ant_value_t temporal_plain_datetime_get_year_of_week(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  PlainDateTime *self = temporal_plain_datetime_this(js, "Temporal.PlainDateTime.prototype.yearOfWeek", &err);
  if (!self) return err;
  temporal_rs_PlainDateTime_year_of_week_result r = temporal_rs_PlainDateTime_year_of_week(self);
  return r.is_ok ? js_mknum(r.ok) : js_mkundef();
}
static ant_value_t temporal_plain_datetime_get_in_leap_year(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  PlainDateTime *self = temporal_plain_datetime_this(js, "Temporal.PlainDateTime.prototype.inLeapYear", &err);
  return self ? js_bool(temporal_rs_PlainDateTime_in_leap_year(self)) : err;
}
static ant_value_t temporal_plain_datetime_get_era(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  PlainDateTime *self = temporal_plain_datetime_this(js, "Temporal.PlainDateTime.prototype.era", &err);
  if (!self) return err;
  DiplomatWrite *write = diplomat_buffer_write_create(8);
  temporal_rs_PlainDateTime_era(self, write);
  if (diplomat_buffer_write_len(write) == 0) {
    diplomat_buffer_write_destroy(write);
    return js_mkundef();
  }
  return temporal_string_from_write(js, write);
}
static ant_value_t temporal_plain_datetime_get_era_year(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  PlainDateTime *self = temporal_plain_datetime_this(js, "Temporal.PlainDateTime.prototype.eraYear", &err);
  if (!self) return err;
  temporal_rs_PlainDateTime_era_year_result r = temporal_rs_PlainDateTime_era_year(self);
  return r.is_ok ? js_mknum(r.ok) : js_mkundef();
}

static ant_value_t temporal_plain_datetime_binary_duration(ant_t *js, ant_value_t *args, int nargs, bool subtract) {
  ant_value_t err = js_mkundef();
  PlainDateTime *self = temporal_plain_datetime_this(
    js, subtract ? "Temporal.PlainDateTime.prototype.subtract" : "Temporal.PlainDateTime.prototype.add", &err
  );
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Duration argument is required");
  Duration *duration;
  if (!temporal_duration_from_value(js, args[0], &duration, &err)) return err;
  ArithmeticOverflow_option overflow = {0};
  if (!temporal_overflow_option(js, nargs > 1 ? args[1] : js_mkundef(), &overflow, &err)) {
    temporal_rs_Duration_destroy(duration);
    return err;
  }
  PlainDateTime *value = NULL;
  TemporalError capi_err = {0};
  bool is_ok;
  if (subtract) {
    temporal_rs_PlainDateTime_subtract_result r = temporal_rs_PlainDateTime_subtract(self, duration, overflow);
    is_ok = r.is_ok;
    if (is_ok) value = r.ok;
    else capi_err = r.err;
  } else {
    temporal_rs_PlainDateTime_add_result r = temporal_rs_PlainDateTime_add(self, duration, overflow);
    is_ok = r.is_ok;
    if (is_ok) value = r.ok;
    else capi_err = r.err;
  }
  temporal_rs_Duration_destroy(duration);
  if (!is_ok) return temporal_error(js, capi_err);
  return temporal_wrap(js, TEMPORAL_PLAIN_DATETIME, value);
}
static ant_value_t temporal_plain_datetime_add(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_plain_datetime_binary_duration(js, args, nargs, false);
}
static ant_value_t temporal_plain_datetime_subtract(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_plain_datetime_binary_duration(js, args, nargs, true);
}
static ant_value_t temporal_plain_datetime_equals(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainDateTime *self = temporal_plain_datetime_this(js, "Temporal.PlainDateTime.prototype.equals", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "PlainDateTime argument is required");
  PlainDateTime *other;
  if (!temporal_plain_datetime_from_value(js, args[0], &other, &err)) return err;
  bool equal = temporal_rs_PlainDateTime_equals(self, other);
  temporal_rs_PlainDateTime_destroy(other);
  return js_bool(equal);
}
static ant_value_t temporal_plain_datetime_difference(ant_t *js, ant_value_t *args, int nargs, bool since) {
  ant_value_t err = js_mkundef();
  PlainDateTime *self = temporal_plain_datetime_this(
    js, since ? "Temporal.PlainDateTime.prototype.since" : "Temporal.PlainDateTime.prototype.until", &err
  );
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "PlainDateTime argument is required");
  PlainDateTime *other;
  if (!temporal_plain_datetime_from_value(js, args[0], &other, &err)) return err;
  DifferenceSettings settings;
  if (!temporal_difference_settings(js, nargs > 1 ? args[1] : js_mkundef(), &settings, &err)) {
    temporal_rs_PlainDateTime_destroy(other);
    return err;
  }
  Duration *value = NULL;
  TemporalError capi_err = {0};
  bool is_ok;
  if (since) {
    temporal_rs_PlainDateTime_since_result r = temporal_rs_PlainDateTime_since(self, other, settings);
    is_ok = r.is_ok;
    if (is_ok) value = r.ok;
    else capi_err = r.err;
  } else {
    temporal_rs_PlainDateTime_until_result r = temporal_rs_PlainDateTime_until(self, other, settings);
    is_ok = r.is_ok;
    if (is_ok) value = r.ok;
    else capi_err = r.err;
  }
  temporal_rs_PlainDateTime_destroy(other);
  if (!is_ok) return temporal_error(js, capi_err);
  return temporal_wrap(js, TEMPORAL_DURATION, value);
}
static ant_value_t temporal_plain_datetime_since(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_plain_datetime_difference(js, args, nargs, true);
}
static ant_value_t temporal_plain_datetime_until(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_plain_datetime_difference(js, args, nargs, false);
}
static ant_value_t temporal_plain_datetime_with(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainDateTime *self = temporal_plain_datetime_this(js, "Temporal.PlainDateTime.prototype.with", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Date-time-like argument is required");
  if (!temporal_validate_partial_object(js, args[0], &err)) return err;
  AnyCalendarKind calendar = temporal_rs_Calendar_kind(temporal_rs_PlainDateTime_calendar(self));
  temporal_partial_datetime_t partial;
  if (!temporal_partial_datetime_impl(js, args[0], calendar, &partial, true, false, &err)) return err;
  ArithmeticOverflow_option overflow = {0};
  if (!temporal_overflow_option(js, nargs > 1 ? args[1] : js_mkundef(), &overflow, &err)) return err;
  temporal_rs_PlainDateTime_with_result r = temporal_rs_PlainDateTime_with(self, partial.partial, overflow);
  if (!r.is_ok) return temporal_error(js, r.err);
  return temporal_wrap(js, TEMPORAL_PLAIN_DATETIME, r.ok);
}
static ant_value_t temporal_plain_datetime_with_calendar(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainDateTime *self = temporal_plain_datetime_this(js, "Temporal.PlainDateTime.prototype.withCalendar", &err);
  if (!self) return err;
  if (nargs < 1 || vtype(args[0]) == kTypeUndefined) return js_mkerr_typed(js, JS_ERR_TYPE, "calendar is required");
  AnyCalendarKind calendar;
  if (!temporal_calendar_kind(js, args[0], AnyCalendarKind_Iso, &calendar, &err)) return err;
  return temporal_wrap(js, TEMPORAL_PLAIN_DATETIME, temporal_rs_PlainDateTime_with_calendar(self, calendar));
}
static ant_value_t temporal_plain_datetime_with_plain_time(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainDateTime *self = temporal_plain_datetime_this(js, "Temporal.PlainDateTime.prototype.withPlainTime", &err);
  if (!self) return err;
  PlainTime *time = NULL;
  if (nargs > 0 && vtype(args[0]) != kTypeUndefined && !temporal_plain_time_from_value(js, args[0], &time, &err)) return err;
  temporal_rs_PlainDateTime_with_time_result r = temporal_rs_PlainDateTime_with_time(self, time);
  if (time) temporal_rs_PlainTime_destroy(time);
  if (!r.is_ok) return temporal_error(js, r.err);
  return temporal_wrap(js, TEMPORAL_PLAIN_DATETIME, r.ok);
}
static ant_value_t temporal_plain_datetime_to_plain_date(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  PlainDateTime *self = temporal_plain_datetime_this(js, "Temporal.PlainDateTime.prototype.toPlainDate", &err);
  return self ? temporal_wrap(js, TEMPORAL_PLAIN_DATE, temporal_rs_PlainDateTime_to_plain_date(self)) : err;
}
static ant_value_t temporal_plain_datetime_to_plain_time(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  PlainDateTime *self = temporal_plain_datetime_this(js, "Temporal.PlainDateTime.prototype.toPlainTime", &err);
  return self ? temporal_wrap(js, TEMPORAL_PLAIN_TIME, temporal_rs_PlainDateTime_to_plain_time(self)) : err;
}
static ant_value_t temporal_plain_datetime_to_zdt(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainDateTime *self = temporal_plain_datetime_this(js, "Temporal.PlainDateTime.prototype.toZonedDateTime", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "time zone is required");
  TimeZone zone;
  if (!temporal_time_zone_from_value(js, args[0], &zone, &err)) return err;
  Disambiguation disambiguation = Disambiguation_Compatible;
  if (nargs > 1 && vtype(args[1]) != kTypeUndefined) {
    ant_value_t options;
    if (!temporal_options_object(js, args[1], false, &options, &err)) return err;
    ant_value_t value = js_get(js, options, "disambiguation");
    if (is_err(value)) return value;
    if (vtype(value) != kTypeUndefined && !temporal_disambiguation_value(js, value, &disambiguation, &err)) return err;
  }
  temporal_rs_PlainDateTime_to_zoned_date_time_with_provider_result r =
    temporal_rs_PlainDateTime_to_zoned_date_time_with_provider(self, zone, disambiguation, temporal_provider(js));
  if (!r.is_ok) return temporal_error(js, r.err);
  return temporal_wrap(js, TEMPORAL_ZONED_DATETIME, r.ok);
}
static ant_value_t temporal_plain_datetime_to_string(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainDateTime *self = temporal_plain_datetime_this(js, "Temporal.PlainDateTime.prototype.toString", &err);
  if (!self) return err;
  DiplomatWrite *write = diplomat_buffer_write_create(32);
  temporal_to_string_options_t options;
  if (!temporal_to_string_options(
      js, nargs > 0 ? args[0] : js_mkundef(),
      TEMPORAL_TOSTRING_CALENDAR | TEMPORAL_TOSTRING_DIGITS | TEMPORAL_TOSTRING_ROUNDING_MODE | TEMPORAL_TOSTRING_SMALLEST_UNIT,
      &options, &err
    )) {
    diplomat_buffer_write_destroy(write);
    return err;
  }
  temporal_rs_PlainDateTime_to_ixdtf_string_result r =
    temporal_rs_PlainDateTime_to_ixdtf_string(self, options.rounding, options.calendar, write);
  if (!r.is_ok) {
    diplomat_buffer_write_destroy(write);
    return temporal_error(js, r.err);
  }
  return temporal_string_from_write(js, write);
}
static ant_value_t temporal_plain_datetime_to_string_default(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  return temporal_plain_datetime_to_string(js, NULL, 0);
}
static ant_value_t temporal_plain_datetime_round(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainDateTime *self = temporal_plain_datetime_this(js, "Temporal.PlainDateTime.prototype.round", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "round options are required");
  RoundingOptions options;
  if (!temporal_rounding_options(js, args[0], true, false, &options, &err)) return err;
  temporal_rs_PlainDateTime_round_result r = temporal_rs_PlainDateTime_round(self, options);
  return r.is_ok ? temporal_wrap(js, TEMPORAL_PLAIN_DATETIME, r.ok) : temporal_error(js, r.err);
}
static ant_value_t temporal_plain_datetime_value_of(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert Temporal.PlainDateTime to a primitive value");
}

void temporal_init_plain_datetime(ant_t *js, ant_value_t temporal) {
  ant_value_t proto = js_mkobj(js);
  js_set_proto_init(proto, js->sym.object_proto);
  js->builtins.temporal_plain_datetime_proto = proto;
  TEMPORAL_GETTER(js, proto, "calendarId", temporal_plain_datetime_get_calendar_id);
  TEMPORAL_GETTER(js, proto, "year", temporal_plain_datetime_get_year);
  TEMPORAL_GETTER(js, proto, "month", temporal_plain_datetime_get_month);
  TEMPORAL_GETTER(js, proto, "monthCode", temporal_plain_datetime_get_month_code);
  TEMPORAL_GETTER(js, proto, "day", temporal_plain_datetime_get_day);
  TEMPORAL_GETTER(js, proto, "hour", temporal_plain_datetime_get_hour);
  TEMPORAL_GETTER(js, proto, "minute", temporal_plain_datetime_get_minute);
  TEMPORAL_GETTER(js, proto, "second", temporal_plain_datetime_get_second);
  TEMPORAL_GETTER(js, proto, "millisecond", temporal_plain_datetime_get_millisecond);
  TEMPORAL_GETTER(js, proto, "microsecond", temporal_plain_datetime_get_microsecond);
  TEMPORAL_GETTER(js, proto, "nanosecond", temporal_plain_datetime_get_nanosecond);
  TEMPORAL_GETTER(js, proto, "dayOfWeek", temporal_plain_datetime_get_day_of_week);
  TEMPORAL_GETTER(js, proto, "dayOfYear", temporal_plain_datetime_get_day_of_year);
  TEMPORAL_GETTER(js, proto, "weekOfYear", temporal_plain_datetime_get_week_of_year);
  TEMPORAL_GETTER(js, proto, "yearOfWeek", temporal_plain_datetime_get_year_of_week);
  TEMPORAL_GETTER(js, proto, "daysInWeek", temporal_plain_datetime_get_days_in_week);
  TEMPORAL_GETTER(js, proto, "daysInMonth", temporal_plain_datetime_get_days_in_month);
  TEMPORAL_GETTER(js, proto, "daysInYear", temporal_plain_datetime_get_days_in_year);
  TEMPORAL_GETTER(js, proto, "monthsInYear", temporal_plain_datetime_get_months_in_year);
  TEMPORAL_GETTER(js, proto, "inLeapYear", temporal_plain_datetime_get_in_leap_year);
  TEMPORAL_GETTER(js, proto, "era", temporal_plain_datetime_get_era);
  TEMPORAL_GETTER(js, proto, "eraYear", temporal_plain_datetime_get_era_year);
  TEMPORAL_METHOD(js, proto, "add", temporal_plain_datetime_add, 1);
  TEMPORAL_METHOD(js, proto, "equals", temporal_plain_datetime_equals, 1);
  TEMPORAL_METHOD(js, proto, "round", temporal_plain_datetime_round, 1);
  TEMPORAL_METHOD(js, proto, "since", temporal_plain_datetime_since, 1);
  TEMPORAL_METHOD(js, proto, "subtract", temporal_plain_datetime_subtract, 1);
  TEMPORAL_METHOD(js, proto, "toJSON", temporal_plain_datetime_to_string_default, 0);
  TEMPORAL_METHOD(js, proto, "toLocaleString", temporal_plain_datetime_to_string_default, 0);
  TEMPORAL_METHOD(js, proto, "toPlainDate", temporal_plain_datetime_to_plain_date, 0);
  TEMPORAL_METHOD(js, proto, "toPlainTime", temporal_plain_datetime_to_plain_time, 0);
  TEMPORAL_METHOD(js, proto, "toString", temporal_plain_datetime_to_string, 0);
  TEMPORAL_METHOD(js, proto, "toZonedDateTime", temporal_plain_datetime_to_zdt, 1);
  TEMPORAL_METHOD(js, proto, "until", temporal_plain_datetime_until, 1);
  TEMPORAL_METHOD(js, proto, "valueOf", temporal_plain_datetime_value_of, 0);
  TEMPORAL_METHOD(js, proto, "with", temporal_plain_datetime_with, 1);
  TEMPORAL_METHOD(js, proto, "withCalendar", temporal_plain_datetime_with_calendar, 1);
  TEMPORAL_METHOD(js, proto, "withPlainTime", temporal_plain_datetime_with_plain_time, 0);
  temporal_set_to_string_tag(js, proto, "Temporal.PlainDateTime");
  ant_value_t ctor = js_make_ctor(js, temporal_plain_datetime_ctor, proto, "PlainDateTime", 13);
  temporal_set_length(js, ctor, 3);
  TEMPORAL_METHOD(js, ctor, "compare", temporal_plain_datetime_compare, 2);
  TEMPORAL_METHOD(js, ctor, "from", temporal_plain_datetime_from, 1);
  temporal_set_namespace_property(js, temporal, "PlainDateTime", ctor);
}

#endif
