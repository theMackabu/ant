#include "modules/temporal.h"

#ifdef ANT_HAVE_TEMPORAL
#include "temporal_internal.h"

static bool temporal_instant_from_value(
  ant_t *js, ant_value_t value, Instant **out, ant_value_t *err
) {
  Instant *instant = is_object_type(value) ? js_get_native(value, TEMPORAL_INSTANT_TAG) : NULL;
  if (instant) {
    *out = temporal_rs_Instant_clone(instant);
    return true;
  }
  ZonedDateTime *zdt = is_object_type(value)
    ? js_get_native(value, TEMPORAL_ZONED_DATETIME_TAG) : NULL;
  if (zdt) {
    *out = temporal_rs_ZonedDateTime_to_instant(zdt);
    return true;
  }
  if (vtype(value) != T_STR && !is_object_type(value)) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "Instant-like value must be a string or Temporal object");
    return false;
  }
  DiplomatStringView view;
  ant_value_t root;
  if (!temporal_to_string_view(js, value, &view, &root, err)) return false;
  temporal_rs_Instant_from_utf8_result result = temporal_rs_Instant_from_utf8(view);
  if (!result.is_ok) { *err = temporal_error(js, result.err); return false; }
  *out = result.ok;
  return true;
}

static ant_value_t temporal_instant_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF) return temporal_require_new(js, "Temporal.Instant");
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Temporal.Instant requires epoch nanoseconds");
  I128Nanoseconds ns;
  ant_value_t err = js_mkundef();
  if (!temporal_i128_from_value(js, args[0], &ns, &err)) return err;
  temporal_rs_Instant_try_new_result result = temporal_rs_Instant_try_new(ns);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap_constructed(js, TEMPORAL_INSTANT, result.ok);
}

static ant_value_t temporal_instant_from(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Temporal.Instant.from requires an argument");
  Instant *instant;
  ant_value_t err = js_mkundef();
  if (!temporal_instant_from_value(js, args[0], &instant, &err)) return err;
  return temporal_wrap(js, TEMPORAL_INSTANT, instant);
}

static ant_value_t temporal_instant_from_epoch_milliseconds(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_RANGE, "epoch milliseconds must be an integral number");
  int64_t milliseconds;
  ant_value_t err = js_mkundef();
  if (!temporal_integral(js, args[0], 0, &milliseconds, &err)) return err;
  temporal_rs_Instant_from_epoch_milliseconds_result result =
    temporal_rs_Instant_from_epoch_milliseconds(milliseconds);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap(js, TEMPORAL_INSTANT, result.ok);
}

static ant_value_t temporal_instant_from_epoch_nanoseconds(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "epoch nanoseconds are required");
  I128Nanoseconds ns;
  ant_value_t err = js_mkundef();
  if (!temporal_i128_from_value(js, args[0], &ns, &err)) return err;
  temporal_rs_Instant_try_new_result result = temporal_rs_Instant_try_new(ns);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap(js, TEMPORAL_INSTANT, result.ok);
}

static ant_value_t temporal_instant_compare(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "Temporal.Instant.compare requires two arguments");
  Instant *one = NULL, *two = NULL;
  ant_value_t err = js_mkundef();
  if (!temporal_instant_from_value(js, args[0], &one, &err)) return err;
  if (!temporal_instant_from_value(js, args[1], &two, &err)) {
    temporal_rs_Instant_destroy(one);
    return err;
  }
  int8_t comparison = temporal_rs_Instant_compare(one, two);
  temporal_rs_Instant_destroy(one);
  temporal_rs_Instant_destroy(two);
  return js_mknum(comparison);
}

static Instant *temporal_instant_this(ant_t *js, const char *method, ant_value_t *err) {
  return temporal_unwrap(js, js_getthis(js), TEMPORAL_INSTANT, method, err);
}

static ant_value_t temporal_instant_get_epoch_milliseconds(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t err = js_mkundef();
  Instant *self = temporal_instant_this(js, "Temporal.Instant.prototype.epochMilliseconds", &err);
  return self ? js_mknum((double)temporal_rs_Instant_epoch_milliseconds(self)) : err;
}

static ant_value_t temporal_instant_get_epoch_nanoseconds(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t err = js_mkundef();
  Instant *self = temporal_instant_this(js, "Temporal.Instant.prototype.epochNanoseconds", &err);
  return self ? temporal_i128_to_bigint(js, temporal_rs_Instant_epoch_nanoseconds(self)) : err;
}

static ant_value_t temporal_instant_binary_duration(
  ant_t *js, ant_value_t *args, int nargs, bool subtract
) {
  ant_value_t err = js_mkundef();
  Instant *self = temporal_instant_this(js,
    subtract ? "Temporal.Instant.prototype.subtract" : "Temporal.Instant.prototype.add", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Duration argument is required");
  Duration *duration;
  if (!temporal_duration_from_value(js, args[0], &duration, &err)) return err;
  Instant *value = NULL;
  TemporalError capi_err = {0};
  bool is_ok;
  if (subtract) {
    temporal_rs_Instant_subtract_result result = temporal_rs_Instant_subtract(self, duration);
    is_ok = result.is_ok;
    if (is_ok) value = result.ok; else capi_err = result.err;
  } else {
    temporal_rs_Instant_add_result result = temporal_rs_Instant_add(self, duration);
    is_ok = result.is_ok;
    if (is_ok) value = result.ok; else capi_err = result.err;
  }
  temporal_rs_Duration_destroy(duration);
  if (!is_ok) return temporal_error(js, capi_err);
  return temporal_wrap(js, TEMPORAL_INSTANT, value);
}

static ant_value_t temporal_instant_add(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_instant_binary_duration(js, args, nargs, false);
}

static ant_value_t temporal_instant_subtract(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_instant_binary_duration(js, args, nargs, true);
}

static ant_value_t temporal_instant_equals(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  Instant *self = temporal_instant_this(js, "Temporal.Instant.prototype.equals", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Instant argument is required");
  Instant *other;
  if (!temporal_instant_from_value(js, args[0], &other, &err)) return err;
  bool equal = temporal_rs_Instant_equals(self, other);
  temporal_rs_Instant_destroy(other);
  return js_bool(equal);
}

static ant_value_t temporal_instant_difference(
  ant_t *js, ant_value_t *args, int nargs, bool since
) {
  ant_value_t err = js_mkundef();
  Instant *self = temporal_instant_this(js,
    since ? "Temporal.Instant.prototype.since" : "Temporal.Instant.prototype.until", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Instant argument is required");
  Instant *other;
  if (!temporal_instant_from_value(js, args[0], &other, &err)) return err;
  DifferenceSettings settings;
  if (!temporal_difference_settings(js, nargs > 1 ? args[1] : js_mkundef(), &settings, &err)) {
    temporal_rs_Instant_destroy(other); return err;
  }
  Duration *value = NULL;
  TemporalError capi_err = {0};
  bool is_ok;
  if (since) {
    temporal_rs_Instant_since_result result = temporal_rs_Instant_since(self, other, settings);
    is_ok = result.is_ok;
    if (is_ok) value = result.ok; else capi_err = result.err;
  } else {
    temporal_rs_Instant_until_result result = temporal_rs_Instant_until(self, other, settings);
    is_ok = result.is_ok;
    if (is_ok) value = result.ok; else capi_err = result.err;
  }
  temporal_rs_Instant_destroy(other);
  if (!is_ok) return temporal_error(js, capi_err);
  return temporal_wrap(js, TEMPORAL_DURATION, value);
}

static ant_value_t temporal_instant_since(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_instant_difference(js, args, nargs, true);
}

static ant_value_t temporal_instant_until(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_instant_difference(js, args, nargs, false);
}

static ant_value_t temporal_instant_to_string(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  Instant *self = temporal_instant_this(js, "Temporal.Instant.prototype.toString", &err);
  if (!self) return err;
  DiplomatWrite *write = diplomat_buffer_write_create(40);
  if (!write) return js_mkerr_typed(js, JS_ERR_INTERNAL, "Temporal string allocation failed");
  temporal_to_string_options_t options;
  if (!temporal_to_string_options(js, nargs > 0 ? args[0] : js_mkundef(),
      TEMPORAL_TOSTRING_DIGITS | TEMPORAL_TOSTRING_ROUNDING_MODE |
        TEMPORAL_TOSTRING_SMALLEST_UNIT | TEMPORAL_TOSTRING_TIME_ZONE,
      &options, &err)) {
    diplomat_buffer_write_destroy(write); return err;
  }
  temporal_rs_Instant_to_ixdtf_string_with_provider_result result =
    temporal_rs_Instant_to_ixdtf_string_with_provider(
      self, options.time_zone, options.rounding, temporal_provider(js), write);
  if (!result.is_ok) {
    diplomat_buffer_write_destroy(write);
    return temporal_error(js, result.err);
  }
  return temporal_string_from_write(js, write);
}

static ant_value_t temporal_instant_to_string_default(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs; return temporal_instant_to_string(js, NULL, 0);
}

static ant_value_t temporal_instant_round(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  Instant *self = temporal_instant_this(js, "Temporal.Instant.prototype.round", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "round options are required");
  RoundingOptions options;
  if (!temporal_rounding_options(js, args[0], true, false, &options, &err)) return err;
  temporal_rs_Instant_round_result result = temporal_rs_Instant_round(self, options);
  return result.is_ok ? temporal_wrap(js, TEMPORAL_INSTANT, result.ok) : temporal_error(js, result.err);
}

static ant_value_t temporal_instant_to_zdt_iso(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  Instant *self = temporal_instant_this(js, "Temporal.Instant.prototype.toZonedDateTimeISO", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "time zone is required");
  TimeZone zone;
  if (!temporal_time_zone_from_value(js, args[0], &zone, &err)) return err;
  temporal_rs_Instant_to_zoned_date_time_iso_with_provider_result result =
    temporal_rs_Instant_to_zoned_date_time_iso_with_provider(self, zone, temporal_provider(js));
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap(js, TEMPORAL_ZONED_DATETIME, result.ok);
}

static ant_value_t temporal_instant_value_of(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert Temporal.Instant to a primitive value");
}

void temporal_init_instant(ant_t *js, ant_value_t temporal) {
  ant_value_t proto = js_mkobj(js);
  js_set_proto_init(proto, js->sym.object_proto);
  js->builtins.temporal_instant_proto = proto;
  TEMPORAL_GETTER(js, proto, "epochMilliseconds", temporal_instant_get_epoch_milliseconds);
  TEMPORAL_GETTER(js, proto, "epochNanoseconds", temporal_instant_get_epoch_nanoseconds);
  TEMPORAL_METHOD(js, proto, "add", temporal_instant_add, 1);
  TEMPORAL_METHOD(js, proto, "equals", temporal_instant_equals, 1);
  TEMPORAL_METHOD(js, proto, "round", temporal_instant_round, 1);
  TEMPORAL_METHOD(js, proto, "since", temporal_instant_since, 1);
  TEMPORAL_METHOD(js, proto, "subtract", temporal_instant_subtract, 1);
  TEMPORAL_METHOD(js, proto, "toString", temporal_instant_to_string, 0);
  TEMPORAL_METHOD(js, proto, "toJSON", temporal_instant_to_string_default, 0);
  TEMPORAL_METHOD(js, proto, "toLocaleString", temporal_instant_to_string_default, 0);
  TEMPORAL_METHOD(js, proto, "toZonedDateTimeISO", temporal_instant_to_zdt_iso, 1);
  TEMPORAL_METHOD(js, proto, "until", temporal_instant_until, 1);
  TEMPORAL_METHOD(js, proto, "valueOf", temporal_instant_value_of, 0);
  temporal_set_to_string_tag(js, proto, "Temporal.Instant");
  ant_value_t ctor = js_make_ctor(js, temporal_instant_ctor, proto, "Instant", 7);
  temporal_set_length(js, ctor, 1);
  TEMPORAL_METHOD(js, ctor, "compare", temporal_instant_compare, 2);
  TEMPORAL_METHOD(js, ctor, "from", temporal_instant_from, 1);
  TEMPORAL_METHOD(js, ctor, "fromEpochMilliseconds", temporal_instant_from_epoch_milliseconds, 1);
  TEMPORAL_METHOD(js, ctor, "fromEpochNanoseconds", temporal_instant_from_epoch_nanoseconds, 1);
  temporal_set_namespace_property(js, temporal, "Instant", ctor);
}

#endif
