#include "modules/temporal.h"

#ifdef ANT_HAVE_TEMPORAL
#include "temporal_internal.h"

static bool temporal_plain_monthday_from_value(
  ant_t *js, ant_value_t value, PlainMonthDay **out, ant_value_t *err
) {
  PlainMonthDay *monthday = is_object_type(value)
    ? js_get_native(value, TEMPORAL_PLAIN_MONTHDAY_TAG) : NULL;
  if (monthday) { *out = temporal_rs_PlainMonthDay_clone(monthday); return true; }
  if (vtype(value) == kTypeString) {
    DiplomatStringView view; ant_value_t root;
    if (!temporal_to_string_view(js, value, &view, &root, err)) return false;
    temporal_rs_PlainMonthDay_from_utf8_result result = temporal_rs_PlainMonthDay_from_utf8(view);
    if (!result.is_ok) { *err = temporal_error(js, result.err); return false; }
    *out = result.ok; return true;
  }
  temporal_partial_date_t partial;
  if (!temporal_partial_date(js, value, AnyCalendarKind_Iso, &partial, true, true, err)) return false;
  ArithmeticOverflow_option overflow = {0};
  temporal_rs_PlainMonthDay_from_partial_result result =
    temporal_rs_PlainMonthDay_from_partial(partial.partial, overflow);
  if (!result.is_ok) { *err = temporal_error(js, result.err); return false; }
  *out = result.ok; return true;
}

static ant_value_t temporal_plain_monthday_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == kTypeUndefined) return temporal_require_new(js, "Temporal.PlainMonthDay");
  int64_t month = 0, day = 0, reference_year = 1972; ant_value_t err = js_mkundef();
  if ((nargs > 0 && !temporal_integer(js, args[0], 0, &month, &err)) ||
      (nargs > 1 && !temporal_integer(js, args[1], 0, &day, &err))) return err;
  AnyCalendarKind calendar;
  if (!temporal_calendar_identifier_kind(js, nargs > 2 ? args[2] : js_mkundef(),
                                         AnyCalendarKind_Iso, &calendar, &err)) return err;
  if (nargs > 3 && !temporal_integer(js, args[3], 1972, &reference_year, &err)) return err;
  if (month < 0 || month > UINT8_MAX || day < 0 || day > UINT8_MAX ||
      reference_year < INT32_MIN || reference_year > INT32_MAX)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Temporal.PlainMonthDay field is outside the supported range");
  OptionI32 ref_year = {.ok = (int32_t)reference_year, .is_ok = true};
  temporal_rs_PlainMonthDay_try_new_with_overflow_result result =
    temporal_rs_PlainMonthDay_try_new_with_overflow(
      (uint8_t)month, (uint8_t)day, calendar, ArithmeticOverflow_Reject, ref_year);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap_constructed(js, TEMPORAL_PLAIN_MONTHDAY, result.ok);
}

static ant_value_t temporal_plain_monthday_from(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Temporal.PlainMonthDay.from requires an argument");
  ant_value_t err = js_mkundef(); ArithmeticOverflow_option overflow = {0};
  if (vtype(args[0]) == kTypeString || js_get_native(args[0], TEMPORAL_PLAIN_MONTHDAY_TAG)) {
    PlainMonthDay *value;
    if (!temporal_plain_monthday_from_value(js, args[0], &value, &err)) return err;
    if (!temporal_overflow_option(js, nargs > 1 ? args[1] : js_mkundef(), &overflow, &err)) {
      temporal_rs_PlainMonthDay_destroy(value);
      return err;
    }
    return temporal_wrap(js, TEMPORAL_PLAIN_MONTHDAY, value);
  }
  temporal_partial_date_t partial;
  if (!temporal_partial_date(js, args[0], AnyCalendarKind_Iso, &partial, true, true, &err)) return err;
  if (!temporal_overflow_option(js, nargs > 1 ? args[1] : js_mkundef(), &overflow, &err)) return err;
  temporal_rs_PlainMonthDay_from_partial_result result =
    temporal_rs_PlainMonthDay_from_partial(partial.partial, overflow);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap(js, TEMPORAL_PLAIN_MONTHDAY, result.ok);
}

static PlainMonthDay *temporal_plain_monthday_this(ant_t *js, const char *method, ant_value_t *err) {
  return temporal_unwrap(js, js_getthis(js), TEMPORAL_PLAIN_MONTHDAY, method, err);
}

static ant_value_t temporal_plain_monthday_get_calendar_id(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs; ant_value_t err = js_mkundef();
  PlainMonthDay *self = temporal_plain_monthday_this(js, "Temporal.PlainMonthDay.prototype.calendarId", &err);
  return self ? temporal_calendar_identifier(js, temporal_rs_PlainMonthDay_calendar(self)) : err;
}

static ant_value_t temporal_plain_monthday_get_day(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs; ant_value_t err = js_mkundef();
  PlainMonthDay *self = temporal_plain_monthday_this(js, "Temporal.PlainMonthDay.prototype.day", &err);
  return self ? js_mknum(temporal_rs_PlainMonthDay_day(self)) : err;
}

static ant_value_t temporal_plain_monthday_get_month_code(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs; ant_value_t err = js_mkundef();
  PlainMonthDay *self = temporal_plain_monthday_this(js, "Temporal.PlainMonthDay.prototype.monthCode", &err);
  if (!self) return err;
  DiplomatWrite *write = diplomat_buffer_write_create(4);
  temporal_rs_PlainMonthDay_month_code(self, write);
  return temporal_string_from_write(js, write);
}

static ant_value_t temporal_plain_monthday_equals(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainMonthDay *self = temporal_plain_monthday_this(js, "Temporal.PlainMonthDay.prototype.equals", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "PlainMonthDay argument is required");
  PlainMonthDay *other;
  if (!temporal_plain_monthday_from_value(js, args[0], &other, &err)) return err;
  bool equal = temporal_rs_PlainMonthDay_equals(self, other);
  temporal_rs_PlainMonthDay_destroy(other);
  return js_bool(equal);
}

static ant_value_t temporal_plain_monthday_with(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainMonthDay *self = temporal_plain_monthday_this(js, "Temporal.PlainMonthDay.prototype.with", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Date-like argument is required");
  if (!temporal_validate_partial_object(js, args[0], &err)) return err;
  AnyCalendarKind calendar = temporal_rs_Calendar_kind(temporal_rs_PlainMonthDay_calendar(self));
  temporal_partial_date_t partial;
  if (!temporal_partial_date_impl(
      js, args[0], calendar, &partial, true, true, false, &err)) return err;
  ArithmeticOverflow_option overflow = {0};
  if (!temporal_overflow_option(js, nargs > 1 ? args[1] : js_mkundef(), &overflow, &err)) return err;
  temporal_rs_PlainMonthDay_with_result result =
    temporal_rs_PlainMonthDay_with(self, partial.partial, overflow);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap(js, TEMPORAL_PLAIN_MONTHDAY, result.ok);
}

static ant_value_t temporal_plain_monthday_to_plain_date(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainMonthDay *self = temporal_plain_monthday_this(js, "Temporal.PlainMonthDay.prototype.toPlainDate", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "year object is required");
  if (!is_object_type(args[0])) return js_mkerr_typed(js, JS_ERR_TYPE, "year must be provided in an object");
  AnyCalendarKind calendar = temporal_rs_Calendar_kind(temporal_rs_PlainMonthDay_calendar(self));
  PartialDate partial = {.calendar = calendar}; bool present; int64_t year;
  if (!temporal_integer_property(
      js, args[0], "year", INT32_MIN, INT32_MAX, &present, &year, &err)) return err;
  if (!present) return js_mkerr_typed(js, JS_ERR_TYPE, "year is required");
  partial.year = (OptionI32){.ok = (int32_t)year, .is_ok = true};
  PartialDate_option option = {.ok = partial, .is_ok = true};
  temporal_rs_PlainMonthDay_to_plain_date_result result =
    temporal_rs_PlainMonthDay_to_plain_date(self, option);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap(js, TEMPORAL_PLAIN_DATE, result.ok);
}

static ant_value_t temporal_plain_monthday_to_string(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainMonthDay *self = temporal_plain_monthday_this(js, "Temporal.PlainMonthDay.prototype.toString", &err);
  if (!self) return err;
  DiplomatWrite *write = diplomat_buffer_write_create(20);
  temporal_to_string_options_t options;
  if (!temporal_to_string_options(js, nargs > 0 ? args[0] : js_mkundef(),
      TEMPORAL_TOSTRING_CALENDAR, &options, &err)) {
    diplomat_buffer_write_destroy(write); return err;
  }
  temporal_rs_PlainMonthDay_to_ixdtf_string(self, options.calendar, write);
  return temporal_string_from_write(js, write);
}

static ant_value_t temporal_plain_monthday_to_string_default(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs; return temporal_plain_monthday_to_string(js, NULL, 0);
}

static ant_value_t temporal_plain_monthday_value_of(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert Temporal.PlainMonthDay to a primitive value");
}

void temporal_init_plain_monthday(ant_t *js, ant_value_t temporal) {
  ant_value_t proto = js_mkobj(js); js_set_proto_init(proto, js->sym.object_proto);
  js->builtins.temporal_plain_monthday_proto = proto;
  TEMPORAL_GETTER(js, proto, "calendarId", temporal_plain_monthday_get_calendar_id);
  TEMPORAL_GETTER(js, proto, "day", temporal_plain_monthday_get_day);
  TEMPORAL_GETTER(js, proto, "monthCode", temporal_plain_monthday_get_month_code);
  TEMPORAL_METHOD(js, proto, "equals", temporal_plain_monthday_equals, 1);
  TEMPORAL_METHOD(js, proto, "toJSON", temporal_plain_monthday_to_string_default, 0);
  TEMPORAL_METHOD(js, proto, "toLocaleString", temporal_plain_monthday_to_string_default, 0);
  TEMPORAL_METHOD(js, proto, "toPlainDate", temporal_plain_monthday_to_plain_date, 1);
  TEMPORAL_METHOD(js, proto, "toString", temporal_plain_monthday_to_string, 0);
  TEMPORAL_METHOD(js, proto, "valueOf", temporal_plain_monthday_value_of, 0);
  TEMPORAL_METHOD(js, proto, "with", temporal_plain_monthday_with, 1);
  temporal_set_to_string_tag(js, proto, "Temporal.PlainMonthDay");
  ant_value_t ctor = js_make_ctor(js, temporal_plain_monthday_ctor, proto, "PlainMonthDay", 13);
  temporal_set_length(js, ctor, 2);
  TEMPORAL_METHOD(js, ctor, "from", temporal_plain_monthday_from, 1);
  temporal_set_namespace_property(js, temporal, "PlainMonthDay", ctor);
}

#endif
