#include "modules/temporal.h"

#ifdef ANT_HAVE_TEMPORAL
#  include "temporal_internal.h"

bool temporal_partial_zdt(
  ant_t *js,
  ant_value_t value,
  AnyCalendarKind default_calendar,
  temporal_partial_zdt_t *out,
  bool require_any,
  bool require_time_zone,
  bool read_calendar,
  bool read_time_zone,
  bool *has_time_zone,
  ant_value_t *err
) {
  if (!is_object_type(value)) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "ZonedDateTime-like value must be an object");
    return false;
  }
  memset(out, 0, sizeof(*out));
  out->month_code_root = js_mkundef();
  out->era_root = js_mkundef();
  out->offset_root = js_mkundef();
  if (has_time_zone) *has_time_zone = false;
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
#  define ZDT_INTEGER(name, minimum, maximum, target, option_type, cast_type)                                          \
    do {                                                                                                               \
      if (!temporal_integer_property(js, value, (name), (minimum), (maximum), &present, &integer, err)) return false;  \
      if (present) {                                                                                                   \
        (target) = (option_type){.ok = (cast_type)integer, .is_ok = true};                                             \
        any = true;                                                                                                    \
      }                                                                                                                \
    } while (0)
  ZDT_INTEGER("day", 1, UINT8_MAX, out->partial.date.day, OptionU8, uint8_t);
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
    ZDT_INTEGER("eraYear", INT32_MIN, INT32_MAX, out->partial.date.era_year, OptionI32, int32_t);
  }
  ZDT_INTEGER("hour", 0, UINT8_MAX, out->partial.time.hour, OptionU8, uint8_t);
  ZDT_INTEGER("microsecond", 0, UINT16_MAX, out->partial.time.microsecond, OptionU16, uint16_t);
  ZDT_INTEGER("millisecond", 0, UINT16_MAX, out->partial.time.millisecond, OptionU16, uint16_t);
  ZDT_INTEGER("minute", 0, UINT8_MAX, out->partial.time.minute, OptionU8, uint8_t);
  ZDT_INTEGER("month", 1, UINT8_MAX, out->partial.date.month, OptionU8, uint8_t);
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
  ZDT_INTEGER("nanosecond", 0, UINT16_MAX, out->partial.time.nanosecond, OptionU16, uint16_t);
  ant_value_t offset = js_get(js, value, "offset");
  if (is_err(offset)) {
    *err = offset;
    return false;
  }
  if (vtype(offset) != kTypeUndefined) {
    if (vtype(offset) != kTypeString && !is_object_type(offset)) {
      *err = js_mkerr_typed(js, JS_ERR_TYPE, "offset must be a string");
      return false;
    }
    DiplomatStringView view;
    ant_value_t root;
    if (!temporal_to_string_view(js, offset, &view, &root, err)) return false;
    temporal_rs_TimeZone_try_from_offset_str_result parsed_offset = temporal_rs_TimeZone_try_from_offset_str(view);
    if (!parsed_offset.is_ok) {
      *err = temporal_error(js, parsed_offset.err);
      return false;
    }
    out->offset_root = root;
    out->partial.offset = (OptionStringView){.ok = view, .is_ok = true};
    any = true;
  }
  ZDT_INTEGER("second", 0, UINT8_MAX, out->partial.time.second, OptionU8, uint8_t);
  if (read_time_zone) {
    ant_value_t zone = js_get(js, value, "timeZone");
    if (is_err(zone)) {
      *err = zone;
      return false;
    }
    if (vtype(zone) == kTypeUndefined && require_time_zone) {
      *err = js_mkerr_typed(js, JS_ERR_TYPE, "timeZone is required");
      return false;
    }
    if (vtype(zone) != kTypeUndefined) {
      TimeZone parsed;
      if (!temporal_time_zone_from_value(js, zone, &parsed, err)) return false;
      out->partial.timezone = (TimeZone_option){.ok = parsed, .is_ok = true};
      if (has_time_zone) *has_time_zone = true;
    }
  }
  ZDT_INTEGER("year", INT32_MIN, INT32_MAX, out->partial.date.year, OptionI32, int32_t);
#  undef ZDT_INTEGER
  if (require_any && !any) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "ZonedDateTime-like object must have fields");
    return false;
  }
  return true;
}

static bool temporal_zdt_from_value_options(
  ant_t *js,
  ant_value_t value,
  Disambiguation_option disambiguation,
  OffsetDisambiguation_option offset,
  ArithmeticOverflow_option overflow,
  ZonedDateTime **out,
  ant_value_t *err
) {
  ZonedDateTime *zdt = is_object_type(value) ? js_get_native(value, TEMPORAL_ZONED_DATETIME_TAG) : NULL;
  if (zdt) {
    *out = temporal_rs_ZonedDateTime_clone(zdt);
    return true;
  }
  if (vtype(value) == kTypeString) {
    DiplomatStringView view;
    ant_value_t root;
    if (!temporal_to_string_view(js, value, &view, &root, err)) return false;
    temporal_rs_ZonedDateTime_from_utf8_with_provider_result result = temporal_rs_ZonedDateTime_from_utf8_with_provider(
      view,
      disambiguation.is_ok ? disambiguation.ok : Disambiguation_Compatible,
      offset.is_ok ? offset.ok : OffsetDisambiguation_Reject,
      temporal_provider(js)
    );
    if (!result.is_ok) {
      *err = temporal_error(js, result.err);
      return false;
    }
    *out = result.ok;
    return true;
  }
  temporal_partial_zdt_t partial;
  if (!temporal_partial_zdt(js, value, AnyCalendarKind_Iso, &partial, true, true, true, true, NULL, err)) return false;
  temporal_rs_ZonedDateTime_from_partial_with_provider_result result =
    temporal_rs_ZonedDateTime_from_partial_with_provider(
      partial.partial, overflow, disambiguation, offset, temporal_provider(js)
    );
  if (!result.is_ok) {
    *err = temporal_error(js, result.err);
    return false;
  }
  *out = result.ok;
  return true;
}

static bool temporal_zdt_from_value(ant_t *js, ant_value_t value, ZonedDateTime **out, ant_value_t *err) {
  return temporal_zdt_from_value_options(
    js, value, (Disambiguation_option){0}, (OffsetDisambiguation_option){0}, (ArithmeticOverflow_option){0}, out, err
  );
}

static ant_value_t temporal_zdt_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == kTypeUndefined) return temporal_require_new(js, "Temporal.ZonedDateTime");
  if (nargs < 2)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Temporal.ZonedDateTime requires epoch nanoseconds and a time zone");
  I128Nanoseconds ns;
  ant_value_t err = js_mkundef();
  if (!temporal_i128_from_value(js, args[0], &ns, &err)) return err;
  if (vtype(args[1]) != kTypeString) return js_mkerr_typed(js, JS_ERR_TYPE, "time zone must be a string");
  DiplomatStringView zone_view;
  ant_value_t zone_root;
  if (!temporal_to_string_view(js, args[1], &zone_view, &zone_root, &err)) return err;
  temporal_rs_TimeZone_try_from_identifier_str_with_provider_result zone_result =
    temporal_rs_TimeZone_try_from_identifier_str_with_provider(zone_view, temporal_provider(js));
  if (!zone_result.is_ok) return temporal_error(js, zone_result.err);
  TimeZone zone = zone_result.ok;
  AnyCalendarKind calendar;
  if (!temporal_calendar_identifier_kind(js, nargs > 2 ? args[2] : js_mkundef(), AnyCalendarKind_Iso, &calendar, &err))
    return err;
  temporal_rs_ZonedDateTime_try_new_with_provider_result result =
    temporal_rs_ZonedDateTime_try_new_with_provider(ns, calendar, zone, temporal_provider(js));
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap_constructed(js, TEMPORAL_ZONED_DATETIME, result.ok);
}

static ant_value_t temporal_zdt_from(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Temporal.ZonedDateTime.from requires an argument");
  ZonedDateTime *value = NULL;
  ant_value_t err = js_mkundef();
  Disambiguation_option disambiguation = {0};
  OffsetDisambiguation_option offset = {0};
  ArithmeticOverflow_option overflow = {0};
  ZonedDateTime *native = is_object_type(args[0]) ? js_get_native(args[0], TEMPORAL_ZONED_DATETIME_TAG) : NULL;
  if (vtype(args[0]) == kTypeString) {
    DiplomatStringView view;
    ant_value_t root;
    if (!temporal_to_string_view(js, args[0], &view, &root, &err)) return err;
    temporal_rs_ParsedZonedDateTime_from_utf8_with_provider_result parsed =
      temporal_rs_ParsedZonedDateTime_from_utf8_with_provider(view, temporal_provider(js));
    if (!parsed.is_ok) return temporal_error(js, parsed.err);
    temporal_rs_ParsedZonedDateTime_destroy(parsed.ok);
    if (!temporal_zdt_options(js, nargs > 1 ? args[1] : js_mkundef(), &disambiguation, &offset, &overflow, &err))
      return err;
    if (!temporal_zdt_from_value_options(js, args[0], disambiguation, offset, overflow, &value, &err)) return err;
    return temporal_wrap(js, TEMPORAL_ZONED_DATETIME, value);
  }
  if (native) {
    value = temporal_rs_ZonedDateTime_clone(native);
    if (!temporal_zdt_options(js, nargs > 1 ? args[1] : js_mkundef(), &disambiguation, &offset, &overflow, &err)) {
      temporal_rs_ZonedDateTime_destroy(value);
      return err;
    }
    return temporal_wrap(js, TEMPORAL_ZONED_DATETIME, value);
  }
  temporal_partial_zdt_t partial;
  if (!temporal_partial_zdt(js, args[0], AnyCalendarKind_Iso, &partial, true, true, true, true, NULL, &err)) return err;
  if (!temporal_zdt_options(js, nargs > 1 ? args[1] : js_mkundef(), &disambiguation, &offset, &overflow, &err))
    return err;
  temporal_rs_ZonedDateTime_from_partial_with_provider_result result =
    temporal_rs_ZonedDateTime_from_partial_with_provider(
      partial.partial, overflow, disambiguation, offset, temporal_provider(js)
    );
  if (!result.is_ok) return temporal_error(js, result.err);
  value = result.ok;
  return temporal_wrap(js, TEMPORAL_ZONED_DATETIME, value);
}

static ant_value_t temporal_zdt_compare(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "Temporal.ZonedDateTime.compare requires two arguments");
  ZonedDateTime *one = NULL, *two = NULL;
  ant_value_t err = js_mkundef();
  if (!temporal_zdt_from_value(js, args[0], &one, &err)) return err;
  if (!temporal_zdt_from_value(js, args[1], &two, &err)) {
    temporal_rs_ZonedDateTime_destroy(one);
    return err;
  }
  int8_t comparison = temporal_rs_ZonedDateTime_compare_instant(one, two);
  temporal_rs_ZonedDateTime_destroy(one);
  temporal_rs_ZonedDateTime_destroy(two);
  return js_mknum(comparison);
}

static ZonedDateTime *temporal_zdt_this(ant_t *js, const char *method, ant_value_t *err) {
  return temporal_unwrap(js, js_getthis(js), TEMPORAL_ZONED_DATETIME, method, err);
}

#  define ZDT_NUMBER_GETTER(name, capi)                                                                                \
    static ant_value_t temporal_zdt_get_##name(ant_t *js, ant_value_t *args, int nargs) {                              \
      (void)args;                                                                                                      \
      (void)nargs;                                                                                                     \
      ant_value_t err = js_mkundef();                                                                                  \
      ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype." #name, &err);                    \
      return self ? js_mknum((double)capi(self)) : err;                                                                \
    }

ZDT_NUMBER_GETTER(year, temporal_rs_ZonedDateTime_year)
ZDT_NUMBER_GETTER(month, temporal_rs_ZonedDateTime_month)
ZDT_NUMBER_GETTER(day, temporal_rs_ZonedDateTime_day)
ZDT_NUMBER_GETTER(hour, temporal_rs_ZonedDateTime_hour)
ZDT_NUMBER_GETTER(minute, temporal_rs_ZonedDateTime_minute)
ZDT_NUMBER_GETTER(second, temporal_rs_ZonedDateTime_second)
ZDT_NUMBER_GETTER(millisecond, temporal_rs_ZonedDateTime_millisecond)
ZDT_NUMBER_GETTER(microsecond, temporal_rs_ZonedDateTime_microsecond)
ZDT_NUMBER_GETTER(nanosecond, temporal_rs_ZonedDateTime_nanosecond)
ZDT_NUMBER_GETTER(day_of_week, temporal_rs_ZonedDateTime_day_of_week)
ZDT_NUMBER_GETTER(day_of_year, temporal_rs_ZonedDateTime_day_of_year)
ZDT_NUMBER_GETTER(days_in_week, temporal_rs_ZonedDateTime_days_in_week)
ZDT_NUMBER_GETTER(days_in_month, temporal_rs_ZonedDateTime_days_in_month)
ZDT_NUMBER_GETTER(days_in_year, temporal_rs_ZonedDateTime_days_in_year)
ZDT_NUMBER_GETTER(months_in_year, temporal_rs_ZonedDateTime_months_in_year)
ZDT_NUMBER_GETTER(epoch_milliseconds, temporal_rs_ZonedDateTime_epoch_milliseconds)
ZDT_NUMBER_GETTER(offset_nanoseconds, temporal_rs_ZonedDateTime_offset_nanoseconds)

static ant_value_t temporal_zdt_get_epoch_nanoseconds(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.epochNanoseconds", &err);
  return self ? temporal_i128_to_bigint(js, temporal_rs_ZonedDateTime_epoch_nanoseconds(self)) : err;
}
static ant_value_t temporal_zdt_get_calendar_id(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.calendarId", &err);
  return self ? temporal_calendar_identifier(js, temporal_rs_ZonedDateTime_calendar(self)) : err;
}
static ant_value_t temporal_zdt_get_time_zone_id(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.timeZoneId", &err);
  return self ? temporal_time_zone_identifier(js, temporal_rs_ZonedDateTime_timezone(self)) : err;
}
static ant_value_t temporal_zdt_get_month_code(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.monthCode", &err);
  if (!self) return err;
  DiplomatWrite *write = diplomat_buffer_write_create(4);
  temporal_rs_ZonedDateTime_month_code(self, write);
  return temporal_string_from_write(js, write);
}
static ant_value_t temporal_zdt_get_week_of_year(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.weekOfYear", &err);
  if (!self) return err;
  temporal_rs_ZonedDateTime_week_of_year_result r = temporal_rs_ZonedDateTime_week_of_year(self);
  return r.is_ok ? js_mknum(r.ok) : js_mkundef();
}
static ant_value_t temporal_zdt_get_year_of_week(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.yearOfWeek", &err);
  if (!self) return err;
  temporal_rs_ZonedDateTime_year_of_week_result r = temporal_rs_ZonedDateTime_year_of_week(self);
  return r.is_ok ? js_mknum(r.ok) : js_mkundef();
}
static ant_value_t temporal_zdt_get_in_leap_year(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.inLeapYear", &err);
  return self ? js_bool(temporal_rs_ZonedDateTime_in_leap_year(self)) : err;
}
static ant_value_t temporal_zdt_get_era(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.era", &err);
  if (!self) return err;
  DiplomatWrite *write = diplomat_buffer_write_create(8);
  temporal_rs_ZonedDateTime_era(self, write);
  if (diplomat_buffer_write_len(write) == 0) {
    diplomat_buffer_write_destroy(write);
    return js_mkundef();
  }
  return temporal_string_from_write(js, write);
}
static ant_value_t temporal_zdt_get_era_year(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.eraYear", &err);
  if (!self) return err;
  temporal_rs_ZonedDateTime_era_year_result r = temporal_rs_ZonedDateTime_era_year(self);
  return r.is_ok ? js_mknum(r.ok) : js_mkundef();
}
static ant_value_t temporal_zdt_get_offset(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.offset", &err);
  if (!self) return err;
  DiplomatWrite *write = diplomat_buffer_write_create(16);
  temporal_rs_ZonedDateTime_offset_result r = temporal_rs_ZonedDateTime_offset(self, write);
  if (!r.is_ok) {
    diplomat_buffer_write_destroy(write);
    return temporal_error(js, r.err);
  }
  return temporal_string_from_write(js, write);
}
static ant_value_t temporal_zdt_get_hours_in_day(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.hoursInDay", &err);
  if (!self) return err;
  temporal_rs_ZonedDateTime_hours_in_day_with_provider_result r =
    temporal_rs_ZonedDateTime_hours_in_day_with_provider(self, temporal_provider(js));
  return r.is_ok ? js_mknum(r.ok) : temporal_error(js, r.err);
}

static ant_value_t temporal_zdt_binary_duration(ant_t *js, ant_value_t *args, int nargs, bool subtract) {
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(
    js, subtract ? "Temporal.ZonedDateTime.prototype.subtract" : "Temporal.ZonedDateTime.prototype.add", &err
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
  ZonedDateTime *value = NULL;
  TemporalError capi_err = {0};
  bool is_ok;
  if (subtract) {
    temporal_rs_ZonedDateTime_subtract_with_provider_result r =
      temporal_rs_ZonedDateTime_subtract_with_provider(self, duration, overflow, temporal_provider(js));
    is_ok = r.is_ok;
    if (is_ok) value = r.ok;
    else capi_err = r.err;
  } else {
    temporal_rs_ZonedDateTime_add_with_provider_result r =
      temporal_rs_ZonedDateTime_add_with_provider(self, duration, overflow, temporal_provider(js));
    is_ok = r.is_ok;
    if (is_ok) value = r.ok;
    else capi_err = r.err;
  }
  temporal_rs_Duration_destroy(duration);
  if (!is_ok) return temporal_error(js, capi_err);
  return temporal_wrap(js, TEMPORAL_ZONED_DATETIME, value);
}
static ant_value_t temporal_zdt_add(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_zdt_binary_duration(js, args, nargs, false);
}
static ant_value_t temporal_zdt_subtract(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_zdt_binary_duration(js, args, nargs, true);
}
static ant_value_t temporal_zdt_equals(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.equals", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "ZonedDateTime argument is required");
  ZonedDateTime *other;
  if (!temporal_zdt_from_value(js, args[0], &other, &err)) return err;
  temporal_rs_ZonedDateTime_equals_with_provider_result r =
    temporal_rs_ZonedDateTime_equals_with_provider(self, other, temporal_provider(js));
  temporal_rs_ZonedDateTime_destroy(other);
  return r.is_ok ? js_bool(r.ok) : temporal_error(js, r.err);
}
static ant_value_t temporal_zdt_difference(ant_t *js, ant_value_t *args, int nargs, bool since) {
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(
    js, since ? "Temporal.ZonedDateTime.prototype.since" : "Temporal.ZonedDateTime.prototype.until", &err
  );
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "ZonedDateTime argument is required");
  ZonedDateTime *other;
  if (!temporal_zdt_from_value(js, args[0], &other, &err)) return err;
  DifferenceSettings settings;
  if (!temporal_difference_settings(js, nargs > 1 ? args[1] : js_mkundef(), &settings, &err)) {
    temporal_rs_ZonedDateTime_destroy(other);
    return err;
  }
  Duration *value = NULL;
  TemporalError capi_err = {0};
  bool is_ok;
  if (since) {
    temporal_rs_ZonedDateTime_since_with_provider_result r =
      temporal_rs_ZonedDateTime_since_with_provider(self, other, settings, temporal_provider(js));
    is_ok = r.is_ok;
    if (is_ok) value = r.ok;
    else capi_err = r.err;
  } else {
    temporal_rs_ZonedDateTime_until_with_provider_result r =
      temporal_rs_ZonedDateTime_until_with_provider(self, other, settings, temporal_provider(js));
    is_ok = r.is_ok;
    if (is_ok) value = r.ok;
    else capi_err = r.err;
  }
  temporal_rs_ZonedDateTime_destroy(other);
  if (!is_ok) return temporal_error(js, capi_err);
  return temporal_wrap(js, TEMPORAL_DURATION, value);
}
static ant_value_t temporal_zdt_since(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_zdt_difference(js, args, nargs, true);
}
static ant_value_t temporal_zdt_until(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_zdt_difference(js, args, nargs, false);
}
static ant_value_t temporal_zdt_start_of_day(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.startOfDay", &err);
  if (!self) return err;
  temporal_rs_ZonedDateTime_start_of_day_with_provider_result r =
    temporal_rs_ZonedDateTime_start_of_day_with_provider(self, temporal_provider(js));
  return r.is_ok ? temporal_wrap(js, TEMPORAL_ZONED_DATETIME, r.ok) : temporal_error(js, r.err);
}
static ant_value_t temporal_zdt_to_instant(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.toInstant", &err);
  return self ? temporal_wrap(js, TEMPORAL_INSTANT, temporal_rs_ZonedDateTime_to_instant(self)) : err;
}
static ant_value_t temporal_zdt_to_plain_date(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.toPlainDate", &err);
  return self ? temporal_wrap(js, TEMPORAL_PLAIN_DATE, temporal_rs_ZonedDateTime_to_plain_date(self)) : err;
}
static ant_value_t temporal_zdt_to_plain_datetime(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.toPlainDateTime", &err);
  return self ? temporal_wrap(js, TEMPORAL_PLAIN_DATETIME, temporal_rs_ZonedDateTime_to_plain_datetime(self)) : err;
}
static ant_value_t temporal_zdt_to_plain_time(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.toPlainTime", &err);
  return self ? temporal_wrap(js, TEMPORAL_PLAIN_TIME, temporal_rs_ZonedDateTime_to_plain_time(self)) : err;
}
static ant_value_t temporal_zdt_with_calendar(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.withCalendar", &err);
  if (!self) return err;
  if (nargs < 1 || vtype(args[0]) == kTypeUndefined) return js_mkerr_typed(js, JS_ERR_TYPE, "calendar is required");
  AnyCalendarKind calendar;
  if (!temporal_calendar_kind(js, args[0], AnyCalendarKind_Iso, &calendar, &err)) return err;
  return temporal_wrap(js, TEMPORAL_ZONED_DATETIME, temporal_rs_ZonedDateTime_with_calendar(self, calendar));
}
static ant_value_t temporal_zdt_with_timezone(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.withTimeZone", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "time zone is required");
  TimeZone zone;
  if (!temporal_time_zone_from_value(js, args[0], &zone, &err)) return err;
  temporal_rs_ZonedDateTime_with_timezone_with_provider_result r =
    temporal_rs_ZonedDateTime_with_timezone_with_provider(self, zone, temporal_provider(js));
  return r.is_ok ? temporal_wrap(js, TEMPORAL_ZONED_DATETIME, r.ok) : temporal_error(js, r.err);
}
static ant_value_t temporal_zdt_with_plain_time(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.withPlainTime", &err);
  if (!self) return err;
  PlainTime *time = NULL;
  if (nargs > 0 && vtype(args[0]) != kTypeUndefined && !temporal_plain_time_from_value(js, args[0], &time, &err)) return err;
  temporal_rs_ZonedDateTime_with_plain_time_and_provider_result r =
    temporal_rs_ZonedDateTime_with_plain_time_and_provider(self, time, temporal_provider(js));
  if (time) temporal_rs_PlainTime_destroy(time);
  return r.is_ok ? temporal_wrap(js, TEMPORAL_ZONED_DATETIME, r.ok) : temporal_error(js, r.err);
}
static ant_value_t temporal_zdt_with(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.with", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "ZonedDateTime-like argument is required");
  if (!is_object_type(args[0])) return js_mkerr_typed(js, JS_ERR_TYPE, "ZonedDateTime-like argument must be an object");
  for (int kind = TEMPORAL_DURATION; kind <= TEMPORAL_ZONED_DATETIME; kind++) {
    if (js_get_native(args[0], temporal_native_tag((temporal_kind_t)kind)))
      return js_mkerr_typed(js, JS_ERR_TYPE, "Temporal objects are not valid partial ZonedDateTime values");
  }
  ant_value_t calendar_value = js_get(js, args[0], "calendar");
  if (is_err(calendar_value)) return calendar_value;
  if (vtype(calendar_value) != kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "calendar is not allowed in ZonedDateTime.with");
  ant_value_t zone_value = js_get(js, args[0], "timeZone");
  if (is_err(zone_value)) return zone_value;
  if (vtype(zone_value) != kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "timeZone is not allowed in ZonedDateTime.with");
  AnyCalendarKind calendar = temporal_rs_Calendar_kind(temporal_rs_ZonedDateTime_calendar(self));
  temporal_partial_zdt_t partial;
  if (!temporal_partial_zdt(js, args[0], calendar, &partial, true, false, false, false, NULL, &err)) return err;
  ArithmeticOverflow_option overflow = {0};
  Disambiguation_option disambiguation = {0};
  OffsetDisambiguation_option offset = {0};
  if (!temporal_zdt_options(js, nargs > 1 ? args[1] : js_mkundef(), &disambiguation, &offset, &overflow, &err))
    return err;
  temporal_rs_ZonedDateTime_with_with_provider_result r = temporal_rs_ZonedDateTime_with_with_provider(
    self, partial.partial, disambiguation, offset, overflow, temporal_provider(js)
  );
  return r.is_ok ? temporal_wrap(js, TEMPORAL_ZONED_DATETIME, r.ok) : temporal_error(js, r.err);
}
static ant_value_t temporal_zdt_get_transition(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.getTimeZoneTransition", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "direction is required");
  ant_value_t direction_value = args[0];
  if (is_object_type(args[0])) {
    direction_value = js_get(js, args[0], "direction");
    if (is_err(direction_value)) return direction_value;
  } else if (vtype(args[0]) != kTypeString) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "options must be a string or object");
  }
  DiplomatStringView view;
  ant_value_t root;
  if (!temporal_to_string_view(js, direction_value, &view, &root, &err)) return err;
  TransitionDirection direction;
  if (view.len == 4 && memcmp(view.data, "next", 4) == 0) direction = TransitionDirection_Next;
  else if (view.len == 8 && memcmp(view.data, "previous", 8) == 0) direction = TransitionDirection_Previous;
  else return js_mkerr_typed(js, JS_ERR_RANGE, "direction must be 'next' or 'previous'");
  temporal_rs_ZonedDateTime_get_time_zone_transition_with_provider_result r =
    temporal_rs_ZonedDateTime_get_time_zone_transition_with_provider(self, direction, temporal_provider(js));
  if (!r.is_ok) return temporal_error(js, r.err);
  if (!r.ok) return js_mknull();
  return temporal_wrap(js, TEMPORAL_ZONED_DATETIME, r.ok);
}
static ant_value_t temporal_zdt_to_string(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.toString", &err);
  if (!self) return err;
  DiplomatWrite *write = diplomat_buffer_write_create(64);
  temporal_to_string_options_t options;
  if (!temporal_to_string_options(
      js,nargs > 0 ? args[0] : js_mkundef(),
      TEMPORAL_TOSTRING_CALENDAR | TEMPORAL_TOSTRING_DIGITS | TEMPORAL_TOSTRING_OFFSET |
        TEMPORAL_TOSTRING_ROUNDING_MODE | TEMPORAL_TOSTRING_SMALLEST_UNIT | TEMPORAL_TOSTRING_TIME_ZONE_NAME,
      &options, &err
    )) {
    diplomat_buffer_write_destroy(write);
    return err;
  }
  temporal_rs_ZonedDateTime_to_ixdtf_string_with_provider_result r =
    temporal_rs_ZonedDateTime_to_ixdtf_string_with_provider(
      self, options.offset, options.time_zone_name, options.calendar, options.rounding, temporal_provider(js), write
    );
  if (!r.is_ok) {
    diplomat_buffer_write_destroy(write);
    return temporal_error(js, r.err);
  }
  return temporal_string_from_write(js, write);
}
static ant_value_t temporal_zdt_to_string_default(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  return temporal_zdt_to_string(js, NULL, 0);
}
static ant_value_t temporal_zdt_round(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  ZonedDateTime *self = temporal_zdt_this(js, "Temporal.ZonedDateTime.prototype.round", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "round options are required");
  RoundingOptions options;
  if (!temporal_rounding_options(js, args[0], true, false, &options, &err)) return err;
  temporal_rs_ZonedDateTime_round_with_provider_result r =
    temporal_rs_ZonedDateTime_round_with_provider(self, options, temporal_provider(js));
  return r.is_ok ? temporal_wrap(js, TEMPORAL_ZONED_DATETIME, r.ok) : temporal_error(js, r.err);
}
static ant_value_t temporal_zdt_value_of(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert Temporal.ZonedDateTime to a primitive value");
}

void temporal_init_zdt(ant_t *js, ant_value_t temporal) {
  ant_value_t proto = js_mkobj(js);
  js_set_proto_init(proto, js->sym.object_proto);
  js->builtins.temporal_zoned_datetime_proto = proto;
  TEMPORAL_GETTER(js, proto, "calendarId", temporal_zdt_get_calendar_id);
  TEMPORAL_GETTER(js, proto, "timeZoneId", temporal_zdt_get_time_zone_id);
  TEMPORAL_GETTER(js, proto, "year", temporal_zdt_get_year);
  TEMPORAL_GETTER(js, proto, "month", temporal_zdt_get_month);
  TEMPORAL_GETTER(js, proto, "monthCode", temporal_zdt_get_month_code);
  TEMPORAL_GETTER(js, proto, "day", temporal_zdt_get_day);
  TEMPORAL_GETTER(js, proto, "hour", temporal_zdt_get_hour);
  TEMPORAL_GETTER(js, proto, "minute", temporal_zdt_get_minute);
  TEMPORAL_GETTER(js, proto, "second", temporal_zdt_get_second);
  TEMPORAL_GETTER(js, proto, "millisecond", temporal_zdt_get_millisecond);
  TEMPORAL_GETTER(js, proto, "microsecond", temporal_zdt_get_microsecond);
  TEMPORAL_GETTER(js, proto, "nanosecond", temporal_zdt_get_nanosecond);
  TEMPORAL_GETTER(js, proto, "epochMilliseconds", temporal_zdt_get_epoch_milliseconds);
  TEMPORAL_GETTER(js, proto, "epochNanoseconds", temporal_zdt_get_epoch_nanoseconds);
  TEMPORAL_GETTER(js, proto, "offset", temporal_zdt_get_offset);
  TEMPORAL_GETTER(js, proto, "offsetNanoseconds", temporal_zdt_get_offset_nanoseconds);
  TEMPORAL_GETTER(js, proto, "hoursInDay", temporal_zdt_get_hours_in_day);
  TEMPORAL_GETTER(js, proto, "dayOfWeek", temporal_zdt_get_day_of_week);
  TEMPORAL_GETTER(js, proto, "dayOfYear", temporal_zdt_get_day_of_year);
  TEMPORAL_GETTER(js, proto, "weekOfYear", temporal_zdt_get_week_of_year);
  TEMPORAL_GETTER(js, proto, "yearOfWeek", temporal_zdt_get_year_of_week);
  TEMPORAL_GETTER(js, proto, "daysInWeek", temporal_zdt_get_days_in_week);
  TEMPORAL_GETTER(js, proto, "daysInMonth", temporal_zdt_get_days_in_month);
  TEMPORAL_GETTER(js, proto, "daysInYear", temporal_zdt_get_days_in_year);
  TEMPORAL_GETTER(js, proto, "monthsInYear", temporal_zdt_get_months_in_year);
  TEMPORAL_GETTER(js, proto, "inLeapYear", temporal_zdt_get_in_leap_year);
  TEMPORAL_GETTER(js, proto, "era", temporal_zdt_get_era);
  TEMPORAL_GETTER(js, proto, "eraYear", temporal_zdt_get_era_year);
  TEMPORAL_METHOD(js, proto, "add", temporal_zdt_add, 1);
  TEMPORAL_METHOD(js, proto, "equals", temporal_zdt_equals, 1);
  TEMPORAL_METHOD(js, proto, "getTimeZoneTransition", temporal_zdt_get_transition, 1);
  TEMPORAL_METHOD(js, proto, "since", temporal_zdt_since, 1);
  TEMPORAL_METHOD(js, proto, "round", temporal_zdt_round, 1);
  TEMPORAL_METHOD(js, proto, "startOfDay", temporal_zdt_start_of_day, 0);
  TEMPORAL_METHOD(js, proto, "subtract", temporal_zdt_subtract, 1);
  TEMPORAL_METHOD(js, proto, "toInstant", temporal_zdt_to_instant, 0);
  TEMPORAL_METHOD(js, proto, "toJSON", temporal_zdt_to_string_default, 0);
  TEMPORAL_METHOD(js, proto, "toLocaleString", temporal_zdt_to_string_default, 0);
  TEMPORAL_METHOD(js, proto, "toPlainDate", temporal_zdt_to_plain_date, 0);
  TEMPORAL_METHOD(js, proto, "toPlainDateTime", temporal_zdt_to_plain_datetime, 0);
  TEMPORAL_METHOD(js, proto, "toPlainTime", temporal_zdt_to_plain_time, 0);
  TEMPORAL_METHOD(js, proto, "toString", temporal_zdt_to_string, 0);
  TEMPORAL_METHOD(js, proto, "until", temporal_zdt_until, 1);
  TEMPORAL_METHOD(js, proto, "valueOf", temporal_zdt_value_of, 0);
  TEMPORAL_METHOD(js, proto, "with", temporal_zdt_with, 1);
  TEMPORAL_METHOD(js, proto, "withCalendar", temporal_zdt_with_calendar, 1);
  TEMPORAL_METHOD(js, proto, "withPlainTime", temporal_zdt_with_plain_time, 0);
  TEMPORAL_METHOD(js, proto, "withTimeZone", temporal_zdt_with_timezone, 1);
  temporal_set_to_string_tag(js, proto, "Temporal.ZonedDateTime");
  ant_value_t ctor = js_make_ctor(js, temporal_zdt_ctor, proto, "ZonedDateTime", 13);
  temporal_set_length(js, ctor, 2);
  TEMPORAL_METHOD(js, ctor, "compare", temporal_zdt_compare, 2);
  TEMPORAL_METHOD(js, ctor, "from", temporal_zdt_from, 1);
  temporal_set_namespace_property(js, temporal, "ZonedDateTime", ctor);
}

#endif
