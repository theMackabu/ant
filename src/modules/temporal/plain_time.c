#include "modules/temporal.h"

#ifdef ANT_HAVE_TEMPORAL
#include "temporal_internal.h"

bool temporal_plain_time_from_value(
  ant_t *js, ant_value_t value, PlainTime **out, ant_value_t *err
) {
  PlainTime *time = is_object_type(value) ? js_get_native(value, TEMPORAL_PLAIN_TIME_TAG) : NULL;
  if (time) { *out = temporal_rs_PlainTime_clone(time); return true; }
  PlainDateTime *datetime = is_object_type(value)
    ? js_get_native(value, TEMPORAL_PLAIN_DATETIME_TAG) : NULL;
  if (datetime) { *out = temporal_rs_PlainDateTime_to_plain_time(datetime); return true; }
  ZonedDateTime *zdt = is_object_type(value)
    ? js_get_native(value, TEMPORAL_ZONED_DATETIME_TAG) : NULL;
  if (zdt) { *out = temporal_rs_ZonedDateTime_to_plain_time(zdt); return true; }
  if (vtype(value) == kTypeString) {
    DiplomatStringView view;
    ant_value_t root;
    if (!temporal_to_string_view(js, value, &view, &root, err)) return false;
    temporal_rs_PlainTime_from_utf8_result result = temporal_rs_PlainTime_from_utf8(view);
    if (!result.is_ok) { *err = temporal_error(js, result.err); return false; }
    *out = result.ok;
    return true;
  }
  PartialTime partial;
  if (!temporal_partial_time(js, value, &partial, true, err)) return false;
  ArithmeticOverflow_option overflow = {0};
  temporal_rs_PlainTime_from_partial_result result = temporal_rs_PlainTime_from_partial(partial, overflow);
  if (!result.is_ok) { *err = temporal_error(js, result.err); return false; }
  *out = result.ok;
  return true;
}

static ant_value_t temporal_plain_time_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == kTypeUndefined) return temporal_require_new(js, "Temporal.PlainTime");
  int64_t fields[6] = {0};
  ant_value_t err = js_mkundef();
  for (int i = 0; i < 6 && i < nargs; i++)
    if (!temporal_integer(js, args[i], 0, &fields[i], &err)) return err;
  if (fields[0] < 0 || fields[0] > UINT8_MAX || fields[1] < 0 || fields[1] > UINT8_MAX ||
      fields[2] < 0 || fields[2] > UINT8_MAX || fields[3] < 0 || fields[3] > UINT16_MAX ||
      fields[4] < 0 || fields[4] > UINT16_MAX || fields[5] < 0 || fields[5] > UINT16_MAX)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Temporal.PlainTime field is outside the supported range");
  temporal_rs_PlainTime_try_new_result result = temporal_rs_PlainTime_try_new(
    (uint8_t)fields[0], (uint8_t)fields[1], (uint8_t)fields[2],
    (uint16_t)fields[3], (uint16_t)fields[4], (uint16_t)fields[5]);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap_constructed(js, TEMPORAL_PLAIN_TIME, result.ok);
}

static ant_value_t temporal_plain_time_from(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Temporal.PlainTime.from requires an argument");
  ant_value_t err = js_mkundef();
  ArithmeticOverflow_option overflow = {0};
  if (vtype(args[0]) == kTypeString || js_get_native(args[0], TEMPORAL_PLAIN_TIME_TAG) ||
      js_get_native(args[0], TEMPORAL_PLAIN_DATETIME_TAG) ||
      js_get_native(args[0], TEMPORAL_ZONED_DATETIME_TAG)) {
    PlainTime *time;
    if (!temporal_plain_time_from_value(js, args[0], &time, &err)) return err;
    if (!temporal_overflow_option(js, nargs > 1 ? args[1] : js_mkundef(), &overflow, &err)) {
      temporal_rs_PlainTime_destroy(time);
      return err;
    }
    return temporal_wrap(js, TEMPORAL_PLAIN_TIME, time);
  }
  PartialTime partial;
  if (!temporal_partial_time(js, args[0], &partial, true, &err)) return err;
  if (!temporal_overflow_option(js, nargs > 1 ? args[1] : js_mkundef(), &overflow, &err)) return err;
  temporal_rs_PlainTime_from_partial_result result =
    temporal_rs_PlainTime_from_partial(partial, overflow);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap(js, TEMPORAL_PLAIN_TIME, result.ok);
}

static ant_value_t temporal_plain_time_compare(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "Temporal.PlainTime.compare requires two arguments");
  PlainTime *one = NULL, *two = NULL;
  ant_value_t err = js_mkundef();
  if (!temporal_plain_time_from_value(js, args[0], &one, &err)) return err;
  if (!temporal_plain_time_from_value(js, args[1], &two, &err)) {
    temporal_rs_PlainTime_destroy(one); return err;
  }
  int8_t comparison = temporal_rs_PlainTime_compare(one, two);
  temporal_rs_PlainTime_destroy(one);
  temporal_rs_PlainTime_destroy(two);
  return js_mknum(comparison);
}

static PlainTime *temporal_plain_time_this(ant_t *js, const char *method, ant_value_t *err) {
  return temporal_unwrap(js, js_getthis(js), TEMPORAL_PLAIN_TIME, method, err);
}

#define PLAIN_TIME_GETTER(name, capi) \
  static ant_value_t temporal_plain_time_get_##name(ant_t *js, ant_value_t *args, int nargs) { \
    (void)args; (void)nargs; ant_value_t err = js_mkundef(); \
    PlainTime *self = temporal_plain_time_this(js, "Temporal.PlainTime.prototype." #name, &err); \
    return self ? js_mknum((double)capi(self)) : err; \
  }

PLAIN_TIME_GETTER(hour, temporal_rs_PlainTime_hour)
PLAIN_TIME_GETTER(minute, temporal_rs_PlainTime_minute)
PLAIN_TIME_GETTER(second, temporal_rs_PlainTime_second)
PLAIN_TIME_GETTER(millisecond, temporal_rs_PlainTime_millisecond)
PLAIN_TIME_GETTER(microsecond, temporal_rs_PlainTime_microsecond)
PLAIN_TIME_GETTER(nanosecond, temporal_rs_PlainTime_nanosecond)

static ant_value_t temporal_plain_time_binary_duration(
  ant_t *js, ant_value_t *args, int nargs, bool subtract
) {
  ant_value_t err = js_mkundef();
  PlainTime *self = temporal_plain_time_this(js,
    subtract ? "Temporal.PlainTime.prototype.subtract" : "Temporal.PlainTime.prototype.add", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Duration argument is required");
  Duration *duration;
  if (!temporal_duration_from_value(js, args[0], &duration, &err)) return err;
  PlainTime *value = NULL;
  TemporalError capi_err = {0};
  bool is_ok;
  if (subtract) {
    temporal_rs_PlainTime_subtract_result result = temporal_rs_PlainTime_subtract(self, duration);
    is_ok = result.is_ok; if (is_ok) value = result.ok; else capi_err = result.err;
  } else {
    temporal_rs_PlainTime_add_result result = temporal_rs_PlainTime_add(self, duration);
    is_ok = result.is_ok; if (is_ok) value = result.ok; else capi_err = result.err;
  }
  temporal_rs_Duration_destroy(duration);
  if (!is_ok) return temporal_error(js, capi_err);
  return temporal_wrap(js, TEMPORAL_PLAIN_TIME, value);
}

static ant_value_t temporal_plain_time_add(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_plain_time_binary_duration(js, args, nargs, false);
}

static ant_value_t temporal_plain_time_subtract(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_plain_time_binary_duration(js, args, nargs, true);
}

static ant_value_t temporal_plain_time_equals(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainTime *self = temporal_plain_time_this(js, "Temporal.PlainTime.prototype.equals", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "PlainTime argument is required");
  PlainTime *other;
  if (!temporal_plain_time_from_value(js, args[0], &other, &err)) return err;
  bool equal = temporal_rs_PlainTime_equals(self, other);
  temporal_rs_PlainTime_destroy(other);
  return js_bool(equal);
}

static ant_value_t temporal_plain_time_difference(
  ant_t *js, ant_value_t *args, int nargs, bool since
) {
  ant_value_t err = js_mkundef();
  PlainTime *self = temporal_plain_time_this(js,
    since ? "Temporal.PlainTime.prototype.since" : "Temporal.PlainTime.prototype.until", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "PlainTime argument is required");
  PlainTime *other;
  if (!temporal_plain_time_from_value(js, args[0], &other, &err)) return err;
  DifferenceSettings settings;
  if (!temporal_difference_settings(js, nargs > 1 ? args[1] : js_mkundef(), &settings, &err)) {
    temporal_rs_PlainTime_destroy(other); return err;
  }
  Duration *value = NULL;
  TemporalError capi_err = {0};
  bool is_ok;
  if (since) {
    temporal_rs_PlainTime_since_result result = temporal_rs_PlainTime_since(self, other, settings);
    is_ok = result.is_ok; if (is_ok) value = result.ok; else capi_err = result.err;
  } else {
    temporal_rs_PlainTime_until_result result = temporal_rs_PlainTime_until(self, other, settings);
    is_ok = result.is_ok; if (is_ok) value = result.ok; else capi_err = result.err;
  }
  temporal_rs_PlainTime_destroy(other);
  if (!is_ok) return temporal_error(js, capi_err);
  return temporal_wrap(js, TEMPORAL_DURATION, value);
}

static ant_value_t temporal_plain_time_since(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_plain_time_difference(js, args, nargs, true);
}

static ant_value_t temporal_plain_time_until(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_plain_time_difference(js, args, nargs, false);
}

static ant_value_t temporal_plain_time_with(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainTime *self = temporal_plain_time_this(js, "Temporal.PlainTime.prototype.with", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Time-like argument is required");
  if (!temporal_validate_partial_object(js, args[0], &err)) return err;
  PartialTime partial;
  if (!temporal_partial_time(js, args[0], &partial, true, &err)) return err;
  ArithmeticOverflow_option overflow = {0};
  if (!temporal_overflow_option(js, nargs > 1 ? args[1] : js_mkundef(), &overflow, &err)) return err;
  temporal_rs_PlainTime_with_result result = temporal_rs_PlainTime_with(self, partial, overflow);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap(js, TEMPORAL_PLAIN_TIME, result.ok);
}

static ant_value_t temporal_plain_time_to_string(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  PlainTime *self = temporal_plain_time_this(js, "Temporal.PlainTime.prototype.toString", &err);
  if (!self) return err;
  DiplomatWrite *write = diplomat_buffer_write_create(24);
  if (!write) return js_mkerr_typed(js, JS_ERR_INTERNAL, "Temporal string allocation failed");
  temporal_to_string_options_t options;
  if (!temporal_to_string_options(js, nargs > 0 ? args[0] : js_mkundef(),
      TEMPORAL_TOSTRING_DIGITS | TEMPORAL_TOSTRING_ROUNDING_MODE |
        TEMPORAL_TOSTRING_SMALLEST_UNIT,
      &options, &err)) {
    diplomat_buffer_write_destroy(write); return err;
  }
  temporal_rs_PlainTime_to_ixdtf_string_result result =
    temporal_rs_PlainTime_to_ixdtf_string(self, options.rounding, write);
  if (!result.is_ok) {
    diplomat_buffer_write_destroy(write);
    return temporal_error(js, result.err);
  }
  return temporal_string_from_write(js, write);
}

static ant_value_t temporal_plain_time_to_string_default(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs; return temporal_plain_time_to_string(js, NULL, 0);
}

static ant_value_t temporal_plain_time_round(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef(); PlainTime *self = temporal_plain_time_this(js, "Temporal.PlainTime.prototype.round", &err);
  if (!self) return err; if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "round options are required");
  RoundingOptions options; if (!temporal_rounding_options(js, args[0], true, false, &options, &err)) return err;
  temporal_rs_PlainTime_round_result result = temporal_rs_PlainTime_round(self, options);
  return result.is_ok ? temporal_wrap(js, TEMPORAL_PLAIN_TIME, result.ok) : temporal_error(js, result.err);
}

static ant_value_t temporal_plain_time_value_of(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert Temporal.PlainTime to a primitive value");
}

void temporal_init_plain_time(ant_t *js, ant_value_t temporal) {
  ant_value_t proto = js_mkobj(js);
  js_set_proto_init(proto, js->sym.object_proto);
  js->builtins.temporal_plain_time_proto = proto;
  TEMPORAL_GETTER(js, proto, "hour", temporal_plain_time_get_hour);
  TEMPORAL_GETTER(js, proto, "minute", temporal_plain_time_get_minute);
  TEMPORAL_GETTER(js, proto, "second", temporal_plain_time_get_second);
  TEMPORAL_GETTER(js, proto, "millisecond", temporal_plain_time_get_millisecond);
  TEMPORAL_GETTER(js, proto, "microsecond", temporal_plain_time_get_microsecond);
  TEMPORAL_GETTER(js, proto, "nanosecond", temporal_plain_time_get_nanosecond);
  TEMPORAL_METHOD(js, proto, "add", temporal_plain_time_add, 1);
  TEMPORAL_METHOD(js, proto, "equals", temporal_plain_time_equals, 1);
  TEMPORAL_METHOD(js, proto, "round", temporal_plain_time_round, 1);
  TEMPORAL_METHOD(js, proto, "since", temporal_plain_time_since, 1);
  TEMPORAL_METHOD(js, proto, "subtract", temporal_plain_time_subtract, 1);
  TEMPORAL_METHOD(js, proto, "toString", temporal_plain_time_to_string, 0);
  TEMPORAL_METHOD(js, proto, "toJSON", temporal_plain_time_to_string_default, 0);
  TEMPORAL_METHOD(js, proto, "toLocaleString", temporal_plain_time_to_string_default, 0);
  TEMPORAL_METHOD(js, proto, "until", temporal_plain_time_until, 1);
  TEMPORAL_METHOD(js, proto, "valueOf", temporal_plain_time_value_of, 0);
  TEMPORAL_METHOD(js, proto, "with", temporal_plain_time_with, 1);
  temporal_set_to_string_tag(js, proto, "Temporal.PlainTime");
  ant_value_t ctor = js_make_ctor(js, temporal_plain_time_ctor, proto, "PlainTime", 9);
  temporal_set_length(js, ctor, 0);
  TEMPORAL_METHOD(js, ctor, "compare", temporal_plain_time_compare, 2);
  TEMPORAL_METHOD(js, ctor, "from", temporal_plain_time_from, 1);
  temporal_set_namespace_property(js, temporal, "PlainTime", ctor);
}

#endif
