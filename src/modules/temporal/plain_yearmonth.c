#include "modules/temporal.h"

#ifdef ANT_HAVE_TEMPORAL
#  include "temporal_internal.h"

static bool temporal_plain_yearmonth_from_value(ant_t *js, ant_value_t value, PlainYearMonth **out, ant_value_t *err) {
  PlainYearMonth *yearmonth = is_object_type(value) ? js_get_native(value, TEMPORAL_PLAIN_YEARMONTH_TAG) : NULL;
  if (yearmonth) {
    *out = temporal_rs_PlainYearMonth_clone(yearmonth);
    return true;
  }
  if (vtype(value) == kTypeString) {
    DiplomatStringView view;
    ant_value_t root;
    if (!temporal_to_string_view(js, value, &view, &root, err)) return false;
    temporal_rs_PlainYearMonth_from_utf8_result result = temporal_rs_PlainYearMonth_from_utf8(view);
    if (!result.is_ok) {
      *err = temporal_error(js, result.err);
      return false;
    }
    *out = result.ok;
    return true;
  }
  temporal_partial_date_t partial;
  if (!temporal_partial_date(js, value, AnyCalendarKind_Iso, &partial, false, true, err)) return false;
  ArithmeticOverflow_option overflow = {0};
  temporal_rs_PlainYearMonth_from_partial_result result =
    temporal_rs_PlainYearMonth_from_partial(partial.partial, overflow);
  if (!result.is_ok) {
    *err = temporal_error(js, result.err);
    return false;
  }
  *out = result.ok;
  return true;
}

static ant_value_t temporal_plain_yearmonth_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == kTypeUndefined) return temporal_require_new(js, "Temporal.PlainYearMonth");
  int64_t year = 0, month = 0, reference_day = 1;
  ant_value_t err = js_mkundef();
  if ((nargs > 0 && vtype(args[0]) == kTypeUndefined) || (nargs > 1 && vtype(args[1]) == kTypeUndefined))
    return js_mkerr_typed(js, JS_ERR_RANGE, "Temporal.PlainYearMonth fields must be finite numbers");
  if (
    (nargs > 0 && !temporal_integer(js, args[0], 0, &year, &err)) ||
    (nargs > 1 && !temporal_integer(js, args[1], 0, &month, &err))
  )
    return err;
  AnyCalendarKind calendar;
  if (!temporal_calendar_identifier_kind(js, nargs > 2 ? args[2] : js_mkundef(), AnyCalendarKind_Iso, &calendar, &err))
    return err;
  if (nargs > 3 && !temporal_integer(js, args[3], 1, &reference_day, &err)) return err;
  if (
    year < INT32_MIN || year > INT32_MAX || month < 0 || month > UINT8_MAX || reference_day < 0 ||
    reference_day > UINT8_MAX
  )
    return js_mkerr_typed(js, JS_ERR_RANGE, "Temporal.PlainYearMonth field is outside the supported range");
  OptionU8 ref_day = {.ok = (uint8_t)reference_day, .is_ok = true};
  temporal_rs_PlainYearMonth_try_new_with_overflow_result result = temporal_rs_PlainYearMonth_try_new_with_overflow(
    (int32_t)year, (uint8_t)month, ref_day, calendar, ArithmeticOverflow_Reject
  );
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap_constructed(js, TEMPORAL_PLAIN_YEARMONTH, result.ok);
}

static ant_value_t temporal_plain_yearmonth_from(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Temporal.PlainYearMonth.from requires an argument");
  ant_value_t err = js_mkundef();
  ArithmeticOverflow_option overflow = {0};
  if (vtype(args[0]) == kTypeString || js_get_native(args[0], TEMPORAL_PLAIN_YEARMONTH_TAG)) {
    PlainYearMonth *value;
    if (!temporal_plain_yearmonth_from_value(js, args[0], &value, &err)) return err;
    if (!temporal_overflow_option(js, nargs > 1 ? args[1] : js_mkundef(), &overflow, &err)) {
      temporal_rs_PlainYearMonth_destroy(value);
      return err;
    }
    return temporal_wrap(js, TEMPORAL_PLAIN_YEARMONTH, value);
  }
  temporal_partial_date_t partial;
  if (!temporal_partial_date(js, args[0], AnyCalendarKind_Iso, &partial, false, true, &err)) return err;
  if (!temporal_overflow_option(js, nargs > 1 ? args[1] : js_mkundef(), &overflow, &err)) return err;
  temporal_rs_PlainYearMonth_from_partial_result result =
    temporal_rs_PlainYearMonth_from_partial(partial.partial, overflow);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap(js, TEMPORAL_PLAIN_YEARMONTH, result.ok);
}

static ant_value_t temporal_plain_yearmonth_compare(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "Temporal.PlainYearMonth.compare requires two arguments");
  PlainYearMonth *one = NULL, *two = NULL;
  ant_value_t err = js_mkundef();
  if (!temporal_plain_yearmonth_from_value(js, args[0], &one, &err)) return err;
  if (!temporal_plain_yearmonth_from_value(js, args[1], &two, &err)) {
    temporal_rs_PlainYearMonth_destroy(one);
    return err;
  }
  int8_t comparison = temporal_rs_PlainYearMonth_compare(one, two);
  temporal_rs_PlainYearMonth_destroy(one);
  temporal_rs_PlainYearMonth_destroy(two);
  return js_mknum(comparison);
}

static PlainYearMonth *temporal_plain_yearmonth_this(ant_t *js, const char *method, ant_value_t *err) {
  return temporal_unwrap(js, js_getthis(js), TEMPORAL_PLAIN_YEARMONTH, method, err);
}

#  define PLAIN_YEARMONTH_NUMBER_GETTER(name, capi)                                                                    \
    static ant_value_t temporal_plain_yearmonth_get_##name(ant_t *js, ant_value_t *args, int nargs) {                  \
      (void)args;                                                                                                      \
      (void)nargs;                                                                                                     \
      ant_value_t err = js_mkundef();                                                                                  \
      PlainYearMonth *self = temporal_plain_yearmonth_this(js, "Temporal.PlainYearMonth.prototype." #name, &err);      \
      return self ? js_mknum((double)capi(self)) : err;                                                                \
    }

PLAIN_YEARMONTH_NUMBER_GETTER(year, temporal_rs_PlainYearMonth_year)
PLAIN_YEARMONTH_NUMBER_GETTER(month, temporal_rs_PlainYearMonth_month)
PLAIN_YEARMONTH_NUMBER_GETTER(days_in_month, temporal_rs_PlainYearMonth_days_in_month)
PLAIN_YEARMONTH_NUMBER_GETTER(days_in_year, temporal_rs_PlainYearMonth_days_in_year)
PLAIN_YEARMONTH_NUMBER_GETTER(months_in_year, temporal_rs_PlainYearMonth_months_in_year)

static ant_value_t temporal_plain_yearmonth_get_calendar_id(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  PlainYearMonth *self = temporal_plain_yearmonth_this(js, "Temporal.PlainYearMonth.prototype.calendarId", &err);
  return self ? temporal_calendar_identifier(js, temporal_rs_PlainYearMonth_calendar(self)) : err;
}
static ant_value_t temporal_plain_yearmonth_get_month_code(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  PlainYearMonth *self = temporal_plain_yearmonth_this(js, "Temporal.PlainYearMonth.prototype.monthCode", &err);
  if (!self) return err;
  DiplomatWrite *write = diplomat_buffer_write_create(4);
  temporal_rs_PlainYearMonth_month_code(self, write);
  return temporal_string_from_write(js, write);
}
static ant_value_t temporal_plain_yearmonth_get_in_leap_year(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  PlainYearMonth *self = temporal_plain_yearmonth_this(js, "Temporal.PlainYearMonth.prototype.inLeapYear", &err);
  return self ? js_bool(temporal_rs_PlainYearMonth_in_leap_year(self)) : err;
}
static ant_value_t temporal_plain_yearmonth_get_era(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  PlainYearMonth *self = temporal_plain_yearmonth_this(js, "Temporal.PlainYearMonth.prototype.era", &err);
  if (!self) return err;
  DiplomatWrite *write = diplomat_buffer_write_create(8);
  temporal_rs_PlainYearMonth_era(self, write);
  if (diplomat_buffer_write_len(write) == 0) {
    diplomat_buffer_write_destroy(write);
    return js_mkundef();
  }
  return temporal_string_from_write(js, write);
}
static ant_value_t temporal_plain_yearmonth_get_era_year(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  PlainYearMonth *self = temporal_plain_yearmonth_this(js, "Temporal.PlainYearMonth.prototype.eraYear", &err);
  if (!self) return err;
  temporal_rs_PlainYearMonth_era_year_result result = temporal_rs_PlainYearMonth_era_year(self);
  return result.is_ok ? js_mknum(result.ok) : js_mkundef();
}

static ant_value_t temporal_plain_yearmonth_binary_duration(ant_t *js, ant_value_t *args, int nargs, bool subtract) {
  ant_value_t err = js_mkundef();
  PlainYearMonth *self = temporal_plain_yearmonth_this(
    js, subtract ? "Temporal.PlainYearMonth.prototype.subtract" : "Temporal.PlainYearMonth.prototype.add", &err
  );
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Duration argument is required");
  Duration *duration;
  if (!temporal_duration_from_value(js, args[0], &duration, &err)) return err;
  ArithmeticOverflow_option overflow_option = {0};
  if (!temporal_overflow_option(js, nargs > 1 ? args[1] : js_mkundef(), &overflow_option, &err)) {
    temporal_rs_Duration_destroy(duration);
    return err;
  }
  ArithmeticOverflow overflow = overflow_option.is_ok ? overflow_option.ok : ArithmeticOverflow_Constrain;
  PlainYearMonth *value = NULL;
  TemporalError capi_err = {0};
  bool is_ok;
  if (subtract) {
    temporal_rs_PlainYearMonth_subtract_result result = temporal_rs_PlainYearMonth_subtract(self, duration, overflow);
    is_ok = result.is_ok;
    if (is_ok) value = result.ok;
    else capi_err = result.err;
  } else {
    temporal_rs_PlainYearMonth_add_result result = temporal_rs_PlainYearMonth_add(self, duration, overflow);
    is_ok = result.is_ok;
    if (is_ok) value = result.ok;
    else capi_err = result.err;
  }
  temporal_rs_Duration_destroy(duration);
  if (!is_ok) return temporal_error(js, capi_err);
  return temporal_wrap(js, TEMPORAL_PLAIN_YEARMONTH, value);
}
static ant_value_t temporal_plain_yearmonth_add(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_plain_yearmonth_binary_duration(js, args, nargs, false);
}
static ant_value_t temporal_plain_yearmonth_subtract(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_plain_yearmonth_binary_duration(js, args, nargs, true);
}
static ant_value_t temporal_plain_yearmonth_equals(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainYearMonth *self = temporal_plain_yearmonth_this(js, "Temporal.PlainYearMonth.prototype.equals", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "PlainYearMonth argument is required");
  PlainYearMonth *other;
  if (!temporal_plain_yearmonth_from_value(js, args[0], &other, &err)) return err;
  bool equal = temporal_rs_PlainYearMonth_equals(self, other);
  temporal_rs_PlainYearMonth_destroy(other);
  return js_bool(equal);
}
static ant_value_t temporal_plain_yearmonth_difference(ant_t *js, ant_value_t *args, int nargs, bool since) {
  ant_value_t err = js_mkundef();
  PlainYearMonth *self = temporal_plain_yearmonth_this(
    js, since ? "Temporal.PlainYearMonth.prototype.since" : "Temporal.PlainYearMonth.prototype.until", &err
  );
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "PlainYearMonth argument is required");
  PlainYearMonth *other;
  if (!temporal_plain_yearmonth_from_value(js, args[0], &other, &err)) return err;
  DifferenceSettings settings;
  if (!temporal_difference_settings(js, nargs > 1 ? args[1] : js_mkundef(), &settings, &err)) {
    temporal_rs_PlainYearMonth_destroy(other);
    return err;
  }
  Duration *value = NULL;
  TemporalError capi_err = {0};
  bool is_ok;
  if (since) {
    temporal_rs_PlainYearMonth_since_result r = temporal_rs_PlainYearMonth_since(self, other, settings);
    is_ok = r.is_ok;
    if (is_ok) value = r.ok;
    else capi_err = r.err;
  } else {
    temporal_rs_PlainYearMonth_until_result r = temporal_rs_PlainYearMonth_until(self, other, settings);
    is_ok = r.is_ok;
    if (is_ok) value = r.ok;
    else capi_err = r.err;
  }
  temporal_rs_PlainYearMonth_destroy(other);
  if (!is_ok) return temporal_error(js, capi_err);
  return temporal_wrap(js, TEMPORAL_DURATION, value);
}
static ant_value_t temporal_plain_yearmonth_since(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_plain_yearmonth_difference(js, args, nargs, true);
}
static ant_value_t temporal_plain_yearmonth_until(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_plain_yearmonth_difference(js, args, nargs, false);
}
static ant_value_t temporal_plain_yearmonth_with(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainYearMonth *self = temporal_plain_yearmonth_this(js, "Temporal.PlainYearMonth.prototype.with", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Date-like argument is required");
  for (int kind = TEMPORAL_DURATION; kind <= TEMPORAL_ZONED_DATETIME; kind++) {
    if (is_object_type(args[0]) && js_get_native(args[0], temporal_native_tag((temporal_kind_t)kind)))
      return js_mkerr_typed(js, JS_ERR_TYPE, "Temporal objects are not valid partial year-month values");
  }
  ant_value_t disallowed = js_get(js, args[0], "calendar");
  if (is_err(disallowed)) return disallowed;
  if (vtype(disallowed) != kTypeUndefined) return js_mkerr_typed(js, JS_ERR_TYPE, "calendar is not allowed");
  disallowed = js_get(js, args[0], "timeZone");
  if (is_err(disallowed)) return disallowed;
  if (vtype(disallowed) != kTypeUndefined) return js_mkerr_typed(js, JS_ERR_TYPE, "timeZone is not allowed");
  AnyCalendarKind calendar = temporal_rs_Calendar_kind(temporal_rs_PlainYearMonth_calendar(self));
  temporal_partial_date_t partial;
  if (!temporal_partial_date_impl(js, args[0], calendar, &partial, false, true, false, &err)) return err;
  ArithmeticOverflow_option overflow = {0};
  if (!temporal_overflow_option(js, nargs > 1 ? args[1] : js_mkundef(), &overflow, &err)) return err;
  temporal_rs_PlainYearMonth_with_result result = temporal_rs_PlainYearMonth_with(self, partial.partial, overflow);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap(js, TEMPORAL_PLAIN_YEARMONTH, result.ok);
}
static ant_value_t temporal_plain_yearmonth_to_plain_date(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainYearMonth *self = temporal_plain_yearmonth_this(js, "Temporal.PlainYearMonth.prototype.toPlainDate", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "day object is required");
  if (!is_object_type(args[0])) return js_mkerr_typed(js, JS_ERR_TYPE, "day must be provided in an object");
  AnyCalendarKind calendar = temporal_rs_Calendar_kind(temporal_rs_PlainYearMonth_calendar(self));
  PartialDate partial = {.calendar = calendar};
  bool present;
  int64_t day;
  if (!temporal_integer_property(js, args[0], "day", 1, UINT8_MAX, &present, &day, &err)) return err;
  if (!present) return js_mkerr_typed(js, JS_ERR_TYPE, "day is required");
  partial.day = (OptionU8){.ok = (uint8_t)day, .is_ok = true};
  PartialDate_option option = {.ok = partial, .is_ok = true};
  temporal_rs_PlainYearMonth_to_plain_date_result result = temporal_rs_PlainYearMonth_to_plain_date(self, option);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap(js, TEMPORAL_PLAIN_DATE, result.ok);
}
static ant_value_t temporal_plain_yearmonth_to_string(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainYearMonth *self = temporal_plain_yearmonth_this(js, "Temporal.PlainYearMonth.prototype.toString", &err);
  if (!self) return err;
  DiplomatWrite *write = diplomat_buffer_write_create(20);
  temporal_to_string_options_t options;
  if (!temporal_to_string_options(js, nargs > 0 ? args[0] : js_mkundef(), TEMPORAL_TOSTRING_CALENDAR, &options, &err)) {
    diplomat_buffer_write_destroy(write);
    return err;
  }
  temporal_rs_PlainYearMonth_to_ixdtf_string(self, options.calendar, write);
  return temporal_string_from_write(js, write);
}
static ant_value_t temporal_plain_yearmonth_to_string_default(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  return temporal_plain_yearmonth_to_string(js, NULL, 0);
}
static ant_value_t temporal_plain_yearmonth_value_of(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert Temporal.PlainYearMonth to a primitive value");
}

void temporal_init_plain_yearmonth(ant_t *js, ant_value_t temporal) {
  ant_value_t proto = js_mkobj(js);
  js_set_proto_init(proto, js->sym.object_proto);
  js->builtins.temporal_plain_yearmonth_proto = proto;
  TEMPORAL_GETTER(js, proto, "calendarId", temporal_plain_yearmonth_get_calendar_id);
  TEMPORAL_GETTER(js, proto, "year", temporal_plain_yearmonth_get_year);
  TEMPORAL_GETTER(js, proto, "month", temporal_plain_yearmonth_get_month);
  TEMPORAL_GETTER(js, proto, "monthCode", temporal_plain_yearmonth_get_month_code);
  TEMPORAL_GETTER(js, proto, "daysInMonth", temporal_plain_yearmonth_get_days_in_month);
  TEMPORAL_GETTER(js, proto, "daysInYear", temporal_plain_yearmonth_get_days_in_year);
  TEMPORAL_GETTER(js, proto, "monthsInYear", temporal_plain_yearmonth_get_months_in_year);
  TEMPORAL_GETTER(js, proto, "inLeapYear", temporal_plain_yearmonth_get_in_leap_year);
  TEMPORAL_GETTER(js, proto, "era", temporal_plain_yearmonth_get_era);
  TEMPORAL_GETTER(js, proto, "eraYear", temporal_plain_yearmonth_get_era_year);
  TEMPORAL_METHOD(js, proto, "add", temporal_plain_yearmonth_add, 1);
  TEMPORAL_METHOD(js, proto, "equals", temporal_plain_yearmonth_equals, 1);
  TEMPORAL_METHOD(js, proto, "since", temporal_plain_yearmonth_since, 1);
  TEMPORAL_METHOD(js, proto, "subtract", temporal_plain_yearmonth_subtract, 1);
  TEMPORAL_METHOD(js, proto, "toJSON", temporal_plain_yearmonth_to_string_default, 0);
  TEMPORAL_METHOD(js, proto, "toLocaleString", temporal_plain_yearmonth_to_string_default, 0);
  TEMPORAL_METHOD(js, proto, "toPlainDate", temporal_plain_yearmonth_to_plain_date, 1);
  TEMPORAL_METHOD(js, proto, "toString", temporal_plain_yearmonth_to_string, 0);
  TEMPORAL_METHOD(js, proto, "until", temporal_plain_yearmonth_until, 1);
  TEMPORAL_METHOD(js, proto, "valueOf", temporal_plain_yearmonth_value_of, 0);
  TEMPORAL_METHOD(js, proto, "with", temporal_plain_yearmonth_with, 1);
  temporal_set_to_string_tag(js, proto, "Temporal.PlainYearMonth");
  ant_value_t ctor = js_make_ctor(js, temporal_plain_yearmonth_ctor, proto, "PlainYearMonth", 14);
  temporal_set_length(js, ctor, 2);
  TEMPORAL_METHOD(js, ctor, "compare", temporal_plain_yearmonth_compare, 2);
  TEMPORAL_METHOD(js, ctor, "from", temporal_plain_yearmonth_from, 1);
  temporal_set_namespace_property(js, temporal, "PlainYearMonth", ctor);
}

#endif
