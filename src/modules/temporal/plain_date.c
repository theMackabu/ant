#include "modules/temporal.h"

#ifdef ANT_HAVE_TEMPORAL
#include "temporal_internal.h"

static bool temporal_plain_date_from_value(
  ant_t *js, ant_value_t value, PlainDate **out, ant_value_t *err
) {
  PlainDate *date = is_object_type(value) ? js_get_native(value, TEMPORAL_PLAIN_DATE_TAG) : NULL;
  if (date) { *out = temporal_rs_PlainDate_clone(date); return true; }
  PlainDateTime *datetime = is_object_type(value)
    ? js_get_native(value, TEMPORAL_PLAIN_DATETIME_TAG) : NULL;
  if (datetime) { *out = temporal_rs_PlainDateTime_to_plain_date(datetime); return true; }
  ZonedDateTime *zdt = is_object_type(value)
    ? js_get_native(value, TEMPORAL_ZONED_DATETIME_TAG) : NULL;
  if (zdt) { *out = temporal_rs_ZonedDateTime_to_plain_date(zdt); return true; }
  if (vtype(value) == T_STR) {
    DiplomatStringView view;
    ant_value_t root;
    if (!temporal_to_string_view(js, value, &view, &root, err)) return false;
    temporal_rs_PlainDate_from_utf8_result result = temporal_rs_PlainDate_from_utf8(view);
    if (!result.is_ok) { *err = temporal_error(js, result.err); return false; }
    *out = result.ok;
    return true;
  }
  temporal_partial_date_t partial;
  if (!temporal_partial_date(js, value, AnyCalendarKind_Iso, &partial, true, true, err)) return false;
  ArithmeticOverflow_option overflow = {0};
  temporal_rs_PlainDate_from_partial_result result =
    temporal_rs_PlainDate_from_partial(partial.partial, overflow);
  if (!result.is_ok) { *err = temporal_error(js, result.err); return false; }
  *out = result.ok;
  return true;
}

static ant_value_t temporal_plain_date_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF) return temporal_require_new(js, "Temporal.PlainDate");
  int64_t year = 0, month = 0, day = 0;
  ant_value_t err = js_mkundef();
  if ((nargs > 0 && vtype(args[0]) == T_UNDEF) ||
      (nargs > 1 && vtype(args[1]) == T_UNDEF) ||
      (nargs > 2 && vtype(args[2]) == T_UNDEF))
    return js_mkerr_typed(js, JS_ERR_RANGE, "Temporal.PlainDate fields must be finite numbers");
  if ((nargs > 0 && !temporal_integer(js, args[0], 0, &year, &err)) ||
      (nargs > 1 && !temporal_integer(js, args[1], 0, &month, &err)) ||
      (nargs > 2 && !temporal_integer(js, args[2], 0, &day, &err))) return err;
  if (year < INT32_MIN || year > INT32_MAX || month < 0 || month > UINT8_MAX ||
      day < 0 || day > UINT8_MAX)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Temporal.PlainDate field is outside the supported range");
  AnyCalendarKind calendar;
  ant_value_t calendar_value = nargs > 3 ? args[3] : js_mkundef();
  if (!temporal_calendar_identifier_kind(js, calendar_value, AnyCalendarKind_Iso, &calendar, &err)) return err;
  temporal_rs_PlainDate_try_new_result result = temporal_rs_PlainDate_try_new(
    (int32_t)year, (uint8_t)month, (uint8_t)day, calendar);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap_constructed(js, TEMPORAL_PLAIN_DATE, result.ok);
}

static ant_value_t temporal_plain_date_from(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Temporal.PlainDate.from requires an argument");
  ArithmeticOverflow_option overflow = {0};
  ant_value_t err = js_mkundef();
  if (vtype(args[0]) == T_STR || js_get_native(args[0], TEMPORAL_PLAIN_DATE_TAG) ||
      js_get_native(args[0], TEMPORAL_PLAIN_DATETIME_TAG) ||
      js_get_native(args[0], TEMPORAL_ZONED_DATETIME_TAG)) {
    PlainDate *date;
    if (!temporal_plain_date_from_value(js, args[0], &date, &err)) return err;
    if (!temporal_overflow_option(js, nargs > 1 ? args[1] : js_mkundef(), &overflow, &err)) {
      temporal_rs_PlainDate_destroy(date);
      return err;
    }
    return temporal_wrap(js, TEMPORAL_PLAIN_DATE, date);
  }
  temporal_partial_date_t partial;
  if (!temporal_partial_date(js, args[0], AnyCalendarKind_Iso, &partial, true, true, &err)) return err;
  if (!temporal_overflow_option(js, nargs > 1 ? args[1] : js_mkundef(), &overflow, &err)) return err;
  temporal_rs_PlainDate_from_partial_result result =
    temporal_rs_PlainDate_from_partial(partial.partial, overflow);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap(js, TEMPORAL_PLAIN_DATE, result.ok);
#if 0
  PlainDate *date;
  if (!temporal_plain_date_from_value(js, args[0], &date, &err)) return err;
  return temporal_wrap(js, TEMPORAL_PLAIN_DATE, date);
#endif
}

static ant_value_t temporal_plain_date_compare(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "Temporal.PlainDate.compare requires two arguments");
  PlainDate *one = NULL, *two = NULL;
  ant_value_t err = js_mkundef();
  if (!temporal_plain_date_from_value(js, args[0], &one, &err)) return err;
  if (!temporal_plain_date_from_value(js, args[1], &two, &err)) {
    temporal_rs_PlainDate_destroy(one); return err;
  }
  int8_t comparison = temporal_rs_PlainDate_compare(one, two);
  temporal_rs_PlainDate_destroy(one);
  temporal_rs_PlainDate_destroy(two);
  return js_mknum(comparison);
}

static PlainDate *temporal_plain_date_this(ant_t *js, const char *method, ant_value_t *err) {
  return temporal_unwrap(js, js_getthis(js), TEMPORAL_PLAIN_DATE, method, err);
}

#define PLAIN_DATE_NUMBER_GETTER(name, capi) \
  static ant_value_t temporal_plain_date_get_##name(ant_t *js, ant_value_t *args, int nargs) { \
    (void)args; (void)nargs; ant_value_t err = js_mkundef(); \
    PlainDate *self = temporal_plain_date_this(js, "Temporal.PlainDate.prototype." #name, &err); \
    return self ? js_mknum((double)capi(self)) : err; \
  }

PLAIN_DATE_NUMBER_GETTER(year, temporal_rs_PlainDate_year)
PLAIN_DATE_NUMBER_GETTER(month, temporal_rs_PlainDate_month)
PLAIN_DATE_NUMBER_GETTER(day, temporal_rs_PlainDate_day)
PLAIN_DATE_NUMBER_GETTER(day_of_week, temporal_rs_PlainDate_day_of_week)
PLAIN_DATE_NUMBER_GETTER(day_of_year, temporal_rs_PlainDate_day_of_year)
PLAIN_DATE_NUMBER_GETTER(days_in_week, temporal_rs_PlainDate_days_in_week)
PLAIN_DATE_NUMBER_GETTER(days_in_month, temporal_rs_PlainDate_days_in_month)
PLAIN_DATE_NUMBER_GETTER(days_in_year, temporal_rs_PlainDate_days_in_year)
PLAIN_DATE_NUMBER_GETTER(months_in_year, temporal_rs_PlainDate_months_in_year)

static ant_value_t temporal_plain_date_get_calendar_id(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs; ant_value_t err = js_mkundef();
  PlainDate *self = temporal_plain_date_this(js, "Temporal.PlainDate.prototype.calendarId", &err);
  return self ? temporal_calendar_identifier(js, temporal_rs_PlainDate_calendar(self)) : err;
}

static ant_value_t temporal_plain_date_get_month_code(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs; ant_value_t err = js_mkundef();
  PlainDate *self = temporal_plain_date_this(js, "Temporal.PlainDate.prototype.monthCode", &err);
  if (!self) return err;
  DiplomatWrite *write = diplomat_buffer_write_create(4);
  temporal_rs_PlainDate_month_code(self, write);
  return temporal_string_from_write(js, write);
}

static ant_value_t temporal_plain_date_get_week_of_year(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs; ant_value_t err = js_mkundef();
  PlainDate *self = temporal_plain_date_this(js, "Temporal.PlainDate.prototype.weekOfYear", &err);
  if (!self) return err;
  temporal_rs_PlainDate_week_of_year_result result = temporal_rs_PlainDate_week_of_year(self);
  return result.is_ok ? js_mknum(result.ok) : js_mkundef();
}

static ant_value_t temporal_plain_date_get_year_of_week(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs; ant_value_t err = js_mkundef();
  PlainDate *self = temporal_plain_date_this(js, "Temporal.PlainDate.prototype.yearOfWeek", &err);
  if (!self) return err;
  temporal_rs_PlainDate_year_of_week_result result = temporal_rs_PlainDate_year_of_week(self);
  return result.is_ok ? js_mknum(result.ok) : js_mkundef();
}

static ant_value_t temporal_plain_date_get_in_leap_year(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs; ant_value_t err = js_mkundef();
  PlainDate *self = temporal_plain_date_this(js, "Temporal.PlainDate.prototype.inLeapYear", &err);
  return self ? js_bool(temporal_rs_PlainDate_in_leap_year(self)) : err;
}

static ant_value_t temporal_plain_date_get_era(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs; ant_value_t err = js_mkundef();
  PlainDate *self = temporal_plain_date_this(js, "Temporal.PlainDate.prototype.era", &err);
  if (!self) return err;
  DiplomatWrite *write = diplomat_buffer_write_create(8);
  temporal_rs_PlainDate_era(self, write);
  if (diplomat_buffer_write_len(write) == 0) {
    diplomat_buffer_write_destroy(write); return js_mkundef();
  }
  return temporal_string_from_write(js, write);
}

static ant_value_t temporal_plain_date_get_era_year(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs; ant_value_t err = js_mkundef();
  PlainDate *self = temporal_plain_date_this(js, "Temporal.PlainDate.prototype.eraYear", &err);
  if (!self) return err;
  temporal_rs_PlainDate_era_year_result result = temporal_rs_PlainDate_era_year(self);
  return result.is_ok ? js_mknum(result.ok) : js_mkundef();
}

static ant_value_t temporal_plain_date_binary_duration(
  ant_t *js, ant_value_t *args, int nargs, bool subtract
) {
  ant_value_t err = js_mkundef();
  PlainDate *self = temporal_plain_date_this(js,
    subtract ? "Temporal.PlainDate.prototype.subtract" : "Temporal.PlainDate.prototype.add", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Duration argument is required");
  Duration *duration;
  if (!temporal_duration_from_value(js, args[0], &duration, &err)) return err;
  ArithmeticOverflow_option overflow = {0};
  if (!temporal_overflow_option(js, nargs > 1 ? args[1] : js_mkundef(), &overflow, &err)) {
    temporal_rs_Duration_destroy(duration); return err;
  }
  PlainDate *value = NULL;
  TemporalError capi_err = {0};
  bool is_ok;
  if (subtract) {
    temporal_rs_PlainDate_subtract_result result = temporal_rs_PlainDate_subtract(self, duration, overflow);
    is_ok = result.is_ok; if (is_ok) value = result.ok; else capi_err = result.err;
  } else {
    temporal_rs_PlainDate_add_result result = temporal_rs_PlainDate_add(self, duration, overflow);
    is_ok = result.is_ok; if (is_ok) value = result.ok; else capi_err = result.err;
  }
  temporal_rs_Duration_destroy(duration);
  if (!is_ok) return temporal_error(js, capi_err);
  return temporal_wrap(js, TEMPORAL_PLAIN_DATE, value);
}

static ant_value_t temporal_plain_date_add(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_plain_date_binary_duration(js, args, nargs, false);
}
static ant_value_t temporal_plain_date_subtract(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_plain_date_binary_duration(js, args, nargs, true);
}

static ant_value_t temporal_plain_date_equals(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainDate *self = temporal_plain_date_this(js, "Temporal.PlainDate.prototype.equals", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "PlainDate argument is required");
  PlainDate *other;
  if (!temporal_plain_date_from_value(js, args[0], &other, &err)) return err;
  bool equal = temporal_rs_PlainDate_equals(self, other);
  temporal_rs_PlainDate_destroy(other);
  return js_bool(equal);
}

static ant_value_t temporal_plain_date_difference(
  ant_t *js, ant_value_t *args, int nargs, bool since
) {
  ant_value_t err = js_mkundef();
  PlainDate *self = temporal_plain_date_this(js,
    since ? "Temporal.PlainDate.prototype.since" : "Temporal.PlainDate.prototype.until", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "PlainDate argument is required");
  PlainDate *other;
  if (!temporal_plain_date_from_value(js, args[0], &other, &err)) return err;
  DifferenceSettings settings;
  if (!temporal_difference_settings(js, nargs > 1 ? args[1] : js_mkundef(), &settings, &err)) {
    temporal_rs_PlainDate_destroy(other); return err;
  }
  Duration *value = NULL; TemporalError capi_err = {0}; bool is_ok;
  if (since) {
    temporal_rs_PlainDate_since_result result = temporal_rs_PlainDate_since(self, other, settings);
    is_ok = result.is_ok; if (is_ok) value = result.ok; else capi_err = result.err;
  } else {
    temporal_rs_PlainDate_until_result result = temporal_rs_PlainDate_until(self, other, settings);
    is_ok = result.is_ok; if (is_ok) value = result.ok; else capi_err = result.err;
  }
  temporal_rs_PlainDate_destroy(other);
  if (!is_ok) return temporal_error(js, capi_err);
  return temporal_wrap(js, TEMPORAL_DURATION, value);
}

static ant_value_t temporal_plain_date_since(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_plain_date_difference(js, args, nargs, true);
}
static ant_value_t temporal_plain_date_until(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_plain_date_difference(js, args, nargs, false);
}

static ant_value_t temporal_plain_date_with(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainDate *self = temporal_plain_date_this(js, "Temporal.PlainDate.prototype.with", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Date-like argument is required");
  if (!temporal_validate_partial_object(js, args[0], &err)) return err;
  temporal_partial_date_t partial;
  AnyCalendarKind calendar = temporal_rs_Calendar_kind(temporal_rs_PlainDate_calendar(self));
  if (!temporal_partial_date_impl(
      js, args[0], calendar, &partial, true, true, false, &err)) return err;
  ArithmeticOverflow_option overflow = {0};
  if (!temporal_overflow_option(js, nargs > 1 ? args[1] : js_mkundef(), &overflow, &err)) return err;
  temporal_rs_PlainDate_with_result result = temporal_rs_PlainDate_with(self, partial.partial, overflow);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap(js, TEMPORAL_PLAIN_DATE, result.ok);
}

static ant_value_t temporal_plain_date_with_calendar(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainDate *self = temporal_plain_date_this(js, "Temporal.PlainDate.prototype.withCalendar", &err);
  if (!self) return err;
  if (nargs < 1 || vtype(args[0]) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "calendar is required");
  AnyCalendarKind calendar;
  if (!temporal_calendar_kind(js, args[0], AnyCalendarKind_Iso, &calendar, &err)) return err;
  return temporal_wrap(js, TEMPORAL_PLAIN_DATE, temporal_rs_PlainDate_with_calendar(self, calendar));
}

static ant_value_t temporal_plain_date_to_plain_datetime(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainDate *self = temporal_plain_date_this(js, "Temporal.PlainDate.prototype.toPlainDateTime", &err);
  if (!self) return err;
  PlainTime *time = NULL;
  if (nargs > 0 && vtype(args[0]) != T_UNDEF &&
      !temporal_plain_time_from_value(js, args[0], &time, &err)) return err;
  temporal_rs_PlainDate_to_plain_date_time_result result =
    temporal_rs_PlainDate_to_plain_date_time(self, time);
  if (time) temporal_rs_PlainTime_destroy(time);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap(js, TEMPORAL_PLAIN_DATETIME, result.ok);
}

static ant_value_t temporal_plain_date_to_plain_monthday(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs; ant_value_t err = js_mkundef();
  PlainDate *self = temporal_plain_date_this(js, "Temporal.PlainDate.prototype.toPlainMonthDay", &err);
  if (!self) return err;
  temporal_rs_PlainDate_to_plain_month_day_result result = temporal_rs_PlainDate_to_plain_month_day(self);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap(js, TEMPORAL_PLAIN_MONTHDAY, result.ok);
}

static ant_value_t temporal_plain_date_to_plain_yearmonth(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs; ant_value_t err = js_mkundef();
  PlainDate *self = temporal_plain_date_this(js, "Temporal.PlainDate.prototype.toPlainYearMonth", &err);
  if (!self) return err;
  temporal_rs_PlainDate_to_plain_year_month_result result = temporal_rs_PlainDate_to_plain_year_month(self);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap(js, TEMPORAL_PLAIN_YEARMONTH, result.ok);
}

static ant_value_t temporal_plain_date_to_zdt(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainDate *self = temporal_plain_date_this(js, "Temporal.PlainDate.prototype.toZonedDateTime", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "time zone is required");
  ant_value_t zone_value = args[0];
  ant_value_t time_value = js_mkundef();
  if (is_object_type(args[0])) {
    zone_value = js_get(js, args[0], "timeZone");
    if (is_err(zone_value)) return zone_value;
    time_value = js_get(js, args[0], "plainTime");
    if (is_err(time_value)) return time_value;
  }
  TimeZone zone;
  if (!temporal_time_zone_from_value(js, zone_value, &zone, &err)) return err;
  PlainTime *time = NULL;
  if (vtype(time_value) != T_UNDEF &&
      !temporal_plain_time_from_value(js, time_value, &time, &err)) return err;
  temporal_rs_PlainDate_to_zoned_date_time_with_provider_result result =
    temporal_rs_PlainDate_to_zoned_date_time_with_provider(self, zone, time, temporal_provider(js));
  if (time) temporal_rs_PlainTime_destroy(time);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap(js, TEMPORAL_ZONED_DATETIME, result.ok);
}

static ant_value_t temporal_plain_date_to_string(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainDate *self = temporal_plain_date_this(js, "Temporal.PlainDate.prototype.toString", &err);
  if (!self) return err;
  DiplomatWrite *write = diplomat_buffer_write_create(24);
  if (!write) return js_mkerr_typed(js, JS_ERR_INTERNAL, "Temporal string allocation failed");
  temporal_to_string_options_t options;
  if (!temporal_to_string_options(js, nargs > 0 ? args[0] : js_mkundef(),
      TEMPORAL_TOSTRING_CALENDAR, &options, &err)) {
    diplomat_buffer_write_destroy(write); return err;
  }
  temporal_rs_PlainDate_to_ixdtf_string(self, options.calendar, write);
  return temporal_string_from_write(js, write);
}

static ant_value_t temporal_plain_date_to_string_default(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs; return temporal_plain_date_to_string(js, NULL, 0);
}

static ant_value_t temporal_plain_date_value_of(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert Temporal.PlainDate to a primitive value");
}

void temporal_init_plain_date(ant_t *js, ant_value_t temporal) {
  ant_value_t proto = js_mkobj(js); js_set_proto_init(proto, js->sym.object_proto);
  js->builtins.temporal_plain_date_proto = proto;
  TEMPORAL_GETTER(js, proto, "calendarId", temporal_plain_date_get_calendar_id);
  TEMPORAL_GETTER(js, proto, "year", temporal_plain_date_get_year);
  TEMPORAL_GETTER(js, proto, "month", temporal_plain_date_get_month);
  TEMPORAL_GETTER(js, proto, "monthCode", temporal_plain_date_get_month_code);
  TEMPORAL_GETTER(js, proto, "day", temporal_plain_date_get_day);
  TEMPORAL_GETTER(js, proto, "dayOfWeek", temporal_plain_date_get_day_of_week);
  TEMPORAL_GETTER(js, proto, "dayOfYear", temporal_plain_date_get_day_of_year);
  TEMPORAL_GETTER(js, proto, "weekOfYear", temporal_plain_date_get_week_of_year);
  TEMPORAL_GETTER(js, proto, "yearOfWeek", temporal_plain_date_get_year_of_week);
  TEMPORAL_GETTER(js, proto, "daysInWeek", temporal_plain_date_get_days_in_week);
  TEMPORAL_GETTER(js, proto, "daysInMonth", temporal_plain_date_get_days_in_month);
  TEMPORAL_GETTER(js, proto, "daysInYear", temporal_plain_date_get_days_in_year);
  TEMPORAL_GETTER(js, proto, "monthsInYear", temporal_plain_date_get_months_in_year);
  TEMPORAL_GETTER(js, proto, "inLeapYear", temporal_plain_date_get_in_leap_year);
  TEMPORAL_GETTER(js, proto, "era", temporal_plain_date_get_era);
  TEMPORAL_GETTER(js, proto, "eraYear", temporal_plain_date_get_era_year);
  TEMPORAL_METHOD(js, proto, "add", temporal_plain_date_add, 1);
  TEMPORAL_METHOD(js, proto, "equals", temporal_plain_date_equals, 1);
  TEMPORAL_METHOD(js, proto, "since", temporal_plain_date_since, 1);
  TEMPORAL_METHOD(js, proto, "subtract", temporal_plain_date_subtract, 1);
  TEMPORAL_METHOD(js, proto, "toJSON", temporal_plain_date_to_string_default, 0);
  TEMPORAL_METHOD(js, proto, "toLocaleString", temporal_plain_date_to_string_default, 0);
  TEMPORAL_METHOD(js, proto, "toPlainDateTime", temporal_plain_date_to_plain_datetime, 0);
  TEMPORAL_METHOD(js, proto, "toPlainMonthDay", temporal_plain_date_to_plain_monthday, 0);
  TEMPORAL_METHOD(js, proto, "toPlainYearMonth", temporal_plain_date_to_plain_yearmonth, 0);
  TEMPORAL_METHOD(js, proto, "toString", temporal_plain_date_to_string, 0);
  TEMPORAL_METHOD(js, proto, "toZonedDateTime", temporal_plain_date_to_zdt, 1);
  TEMPORAL_METHOD(js, proto, "until", temporal_plain_date_until, 1);
  TEMPORAL_METHOD(js, proto, "valueOf", temporal_plain_date_value_of, 0);
  TEMPORAL_METHOD(js, proto, "with", temporal_plain_date_with, 1);
  TEMPORAL_METHOD(js, proto, "withCalendar", temporal_plain_date_with_calendar, 1);
  temporal_set_to_string_tag(js, proto, "Temporal.PlainDate");
  ant_value_t ctor = js_make_ctor(js, temporal_plain_date_ctor, proto, "PlainDate", 9);
  temporal_set_length(js, ctor, 3);
  TEMPORAL_METHOD(js, ctor, "compare", temporal_plain_date_compare, 2);
  TEMPORAL_METHOD(js, ctor, "from", temporal_plain_date_from, 1);
  temporal_set_namespace_property(js, temporal, "PlainDate", ctor);
}

#endif
