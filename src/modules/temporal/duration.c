#include "modules/temporal.h"

#ifdef ANT_HAVE_TEMPORAL
#include "temporal_internal.h"

static bool temporal_duration_partial(
  ant_t *js, ant_value_t value, PartialDuration *out, bool require_any, ant_value_t *err
) {
  if (!is_object_type(value)) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "Duration-like value must be an object");
    return false;
  }
  memset(out, 0, sizeof(*out));
  struct {
    const char *name;
    bool floating;
    size_t offset;
  } fields[] = {
    {"days", false, offsetof(PartialDuration, days)},
    {"hours", false, offsetof(PartialDuration, hours)},
    {"microseconds", true, offsetof(PartialDuration, microseconds)},
    {"milliseconds", false, offsetof(PartialDuration, milliseconds)},
    {"minutes", false, offsetof(PartialDuration, minutes)},
    {"months", false, offsetof(PartialDuration, months)},
    {"nanoseconds", true, offsetof(PartialDuration, nanoseconds)},
    {"seconds", false, offsetof(PartialDuration, seconds)},
    {"weeks", false, offsetof(PartialDuration, weeks)},
    {"years", false, offsetof(PartialDuration, years)},
  };
  bool any = false;
  for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
    ant_value_t field = js_get(js, value, fields[i].name);
    if (is_err(field)) {
      *err = field;
      return false;
    }
    if (vtype(field) == T_UNDEF) continue;
    any = true;
    if (fields[i].floating) {
      double number;
      if (!temporal_to_number(js, field, &number, err)) return false;
      if (!isfinite(number) || trunc(number) != number) {
        *err = js_mkerr_typed(js, JS_ERR_RANGE, "Temporal field must be a finite integer");
        return false;
      }
      OptionF64 *slot = (OptionF64 *)((char *)out + fields[i].offset);
      slot->ok = number;
      slot->is_ok = true;
    } else {
      int64_t integer;
      if (!temporal_integral(js, field, 0, &integer, err)) return false;
      OptionI64 *slot = (OptionI64 *)((char *)out + fields[i].offset);
      slot->ok = integer;
      slot->is_ok = true;
    }
  }
  if (require_any && !any) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "Duration-like object must have at least one duration field");
    return false;
  }
  return true;
}

bool temporal_duration_from_value(
  ant_t *js, ant_value_t value, Duration **out, ant_value_t *err
) {
  Duration *duration = is_object_type(value)
    ? js_get_native(value, TEMPORAL_DURATION_TAG)
    : NULL;
  if (duration) {
    *out = temporal_rs_Duration_clone(duration);
    return true;
  }
  if (vtype(value) == T_STR) {
    DiplomatStringView view;
    ant_value_t root;
    if (!temporal_to_string_view(js, value, &view, &root, err)) return false;
    temporal_rs_Duration_from_utf8_result result = temporal_rs_Duration_from_utf8(view);
    if (!result.is_ok) {
      *err = temporal_error(js, result.err);
      return false;
    }
    *out = result.ok;
    return true;
  }
  PartialDuration partial;
  if (!temporal_duration_partial(js, value, &partial, true, err)) return false;
  temporal_rs_Duration_from_partial_duration_result result =
    temporal_rs_Duration_from_partial_duration(partial);
  if (!result.is_ok) {
    *err = temporal_error(js, result.err);
    return false;
  }
  *out = result.ok;
  return true;
}

static ant_value_t temporal_duration_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF) return temporal_require_new(js, "Temporal.Duration");
  int64_t fields[8] = {0};
  double microseconds = 0, nanoseconds = 0;
  ant_value_t err = js_mkundef();
  for (int i = 0; i < 8 && i < nargs; i++)
    if (!temporal_integral(js, args[i], 0, &fields[i], &err)) return err;
  if (nargs > 8 && vtype(args[8]) != T_UNDEF) {
    if (!temporal_to_number(js, args[8], &microseconds, &err)) return err;
    if (!isfinite(microseconds) || trunc(microseconds) != microseconds)
      return js_mkerr_typed(js, JS_ERR_RANGE, "Temporal field must be a finite integer");
  }
  if (nargs > 9 && vtype(args[9]) != T_UNDEF) {
    if (!temporal_to_number(js, args[9], &nanoseconds, &err)) return err;
    if (!isfinite(nanoseconds) || trunc(nanoseconds) != nanoseconds)
      return js_mkerr_typed(js, JS_ERR_RANGE, "Temporal field must be a finite integer");
  }
  temporal_rs_Duration_create_result result = temporal_rs_Duration_create(
    fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6],
    fields[7], microseconds, nanoseconds);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap_constructed(js, TEMPORAL_DURATION, result.ok);
}

static ant_value_t temporal_duration_from(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Temporal.Duration.from requires an argument");
  Duration *duration;
  ant_value_t err = js_mkundef();
  if (!temporal_duration_from_value(js, args[0], &duration, &err)) return err;
  return temporal_wrap(js, TEMPORAL_DURATION, duration);
}

static ant_value_t temporal_duration_compare(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "Temporal.Duration.compare requires two arguments");
  Duration *one = NULL, *two = NULL;
  ant_value_t err = js_mkundef();
  if (!temporal_duration_from_value(js, args[0], &one, &err)) return err;
  if (!temporal_duration_from_value(js, args[1], &two, &err)) {
    temporal_rs_Duration_destroy(one);
    return err;
  }
  temporal_relative_to_t relative;
  if (!temporal_relative_to_from_options(js, nargs > 2 ? args[2] : js_mkundef(), &relative, &err)) {
    temporal_rs_Duration_destroy(one); temporal_rs_Duration_destroy(two); return err;
  }
  temporal_rs_Duration_compare_with_provider_result result =
    temporal_rs_Duration_compare_with_provider(one, two, relative.value, temporal_provider(js));
  temporal_rs_Duration_destroy(one);
  temporal_rs_Duration_destroy(two);
  temporal_relative_to_destroy(&relative);
  if (!result.is_ok) return temporal_error(js, result.err);
  return js_mknum(result.ok);
}

static Duration *temporal_duration_this(ant_t *js, const char *method, ant_value_t *err) {
  return temporal_unwrap(js, js_getthis(js), TEMPORAL_DURATION, method, err);
}

#define DURATION_NUMBER_GETTER(name, capi) \
  static ant_value_t temporal_duration_get_##name(ant_t *js, ant_value_t *args, int nargs) { \
    (void)args; (void)nargs; ant_value_t err = js_mkundef(); \
    Duration *self = temporal_duration_this(js, "Temporal.Duration.prototype." #name, &err); \
    return self ? js_mknum((double)capi(self)) : err; \
  }

DURATION_NUMBER_GETTER(years, temporal_rs_Duration_years)
DURATION_NUMBER_GETTER(months, temporal_rs_Duration_months)
DURATION_NUMBER_GETTER(weeks, temporal_rs_Duration_weeks)
DURATION_NUMBER_GETTER(days, temporal_rs_Duration_days)
DURATION_NUMBER_GETTER(hours, temporal_rs_Duration_hours)
DURATION_NUMBER_GETTER(minutes, temporal_rs_Duration_minutes)
DURATION_NUMBER_GETTER(seconds, temporal_rs_Duration_seconds)
DURATION_NUMBER_GETTER(milliseconds, temporal_rs_Duration_milliseconds)
DURATION_NUMBER_GETTER(microseconds, temporal_rs_Duration_microseconds)
DURATION_NUMBER_GETTER(nanoseconds, temporal_rs_Duration_nanoseconds)
DURATION_NUMBER_GETTER(sign, temporal_rs_Duration_sign)

static ant_value_t temporal_duration_get_blank(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t err = js_mkundef();
  Duration *self = temporal_duration_this(js, "Temporal.Duration.prototype.blank", &err);
  return self ? js_bool(temporal_rs_Duration_is_zero(self)) : err;
}

static ant_value_t temporal_duration_abs(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t err = js_mkundef();
  Duration *self = temporal_duration_this(js, "Temporal.Duration.prototype.abs", &err);
  return self ? temporal_wrap(js, TEMPORAL_DURATION, temporal_rs_Duration_abs(self)) : err;
}

static ant_value_t temporal_duration_negated(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t err = js_mkundef();
  Duration *self = temporal_duration_this(js, "Temporal.Duration.prototype.negated", &err);
  return self ? temporal_wrap(js, TEMPORAL_DURATION, temporal_rs_Duration_negated(self)) : err;
}

static ant_value_t temporal_duration_binary(
  ant_t *js, ant_value_t *args, int nargs, bool subtract
) {
  ant_value_t err = js_mkundef();
  Duration *self = temporal_duration_this(js,
    subtract ? "Temporal.Duration.prototype.subtract" : "Temporal.Duration.prototype.add", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Duration argument is required");
  Duration *other;
  if (!temporal_duration_from_value(js, args[0], &other, &err)) return err;
  Duration *value = NULL;
  TemporalError capi_err = {0};
  bool is_ok;
  if (subtract) {
    temporal_rs_Duration_subtract_result result = temporal_rs_Duration_subtract(self, other);
    is_ok = result.is_ok;
    if (is_ok) value = result.ok;
    else capi_err = result.err;
  } else {
    temporal_rs_Duration_add_result result = temporal_rs_Duration_add(self, other);
    is_ok = result.is_ok;
    if (is_ok) value = result.ok;
    else capi_err = result.err;
  }
  temporal_rs_Duration_destroy(other);
  if (!is_ok) return temporal_error(js, capi_err);
  return temporal_wrap(js, TEMPORAL_DURATION, value);
}

static ant_value_t temporal_duration_add(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_duration_binary(js, args, nargs, false);
}

static ant_value_t temporal_duration_subtract(ant_t *js, ant_value_t *args, int nargs) {
  return temporal_duration_binary(js, args, nargs, true);
}

static ant_value_t temporal_duration_with(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  Duration *self = temporal_duration_this(js, "Temporal.Duration.prototype.with", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Duration-like argument is required");
  PartialDuration partial;
  if (!temporal_duration_partial(js, args[0], &partial, true, &err)) return err;
#define DURATION_FILL(field, getter, option_type) \
  if (!partial.field.is_ok) partial.field = (option_type){.ok = getter(self), .is_ok = true}
  DURATION_FILL(years, temporal_rs_Duration_years, OptionI64);
  DURATION_FILL(months, temporal_rs_Duration_months, OptionI64);
  DURATION_FILL(weeks, temporal_rs_Duration_weeks, OptionI64);
  DURATION_FILL(days, temporal_rs_Duration_days, OptionI64);
  DURATION_FILL(hours, temporal_rs_Duration_hours, OptionI64);
  DURATION_FILL(minutes, temporal_rs_Duration_minutes, OptionI64);
  DURATION_FILL(seconds, temporal_rs_Duration_seconds, OptionI64);
  DURATION_FILL(milliseconds, temporal_rs_Duration_milliseconds, OptionI64);
  DURATION_FILL(microseconds, temporal_rs_Duration_microseconds, OptionF64);
  DURATION_FILL(nanoseconds, temporal_rs_Duration_nanoseconds, OptionF64);
#undef DURATION_FILL
  temporal_rs_Duration_from_partial_duration_result result =
    temporal_rs_Duration_from_partial_duration(partial);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap(js, TEMPORAL_DURATION, result.ok);
}

static ant_value_t temporal_duration_to_string(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  Duration *self = temporal_duration_this(js, "Temporal.Duration.prototype.toString", &err);
  if (!self) return err;
  DiplomatWrite *write = diplomat_buffer_write_create(32);
  if (!write) return js_mkerr_typed(js, JS_ERR_INTERNAL, "Temporal string allocation failed");
  temporal_to_string_options_t options;
  if (!temporal_to_string_options(js, nargs > 0 ? args[0] : js_mkundef(),
      TEMPORAL_TOSTRING_DIGITS | TEMPORAL_TOSTRING_ROUNDING_MODE |
        TEMPORAL_TOSTRING_SMALLEST_UNIT,
      &options, &err)) {
    diplomat_buffer_write_destroy(write); return err;
  }
  temporal_rs_Duration_to_string_result result = temporal_rs_Duration_to_string(self, options.rounding, write);
  if (!result.is_ok) {
    diplomat_buffer_write_destroy(write);
    return temporal_error(js, result.err);
  }
  return temporal_string_from_write(js, write);
}

static ant_value_t temporal_duration_to_string_default(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs; return temporal_duration_to_string(js, NULL, 0);
}

static ant_value_t temporal_duration_round(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  Duration *self = temporal_duration_this(js, "Temporal.Duration.prototype.round", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "round options are required");
  RoundingOptions options = {0};
  temporal_relative_to_t relative = {0};
  if (vtype(args[0]) == T_STR) {
    Unit unit;
    if (!temporal_unit_from_value(js, args[0], false, &unit, &err)) return err;
    options.smallest_unit = (Unit_option){.ok = unit, .is_ok = true};
  } else {
    ant_value_t object;
    if (!temporal_options_object(js, args[0], false, &object, &err)) return err;
    if (!is_object_type(object))
      return js_mkerr_typed(js, JS_ERR_TYPE, "round options are required");
    ant_value_t value = js_get(js, object, "largestUnit");
    if (is_err(value)) return value;
    if (vtype(value) != T_UNDEF) {
      Unit unit; if (!temporal_unit_from_value(js, value, true, &unit, &err)) return err;
      options.largest_unit = (Unit_option){.ok = unit, .is_ok = true};
    }
    value = js_get(js, object, "relativeTo");
    if (is_err(value)) return value;
    if (!temporal_relative_to(js, value, &relative, &err)) return err;
    value = js_get(js, object, "roundingIncrement");
    if (is_err(value)) { temporal_relative_to_destroy(&relative); return value; }
    if (!temporal_rounding_increment(js, value, &options.increment, &err)) {
      temporal_relative_to_destroy(&relative); return err;
    }
    value = js_get(js, object, "roundingMode");
    if (is_err(value)) { temporal_relative_to_destroy(&relative); return value; }
    if (vtype(value) != T_UNDEF) {
      RoundingMode mode;
      if (!temporal_rounding_mode_from_value(js, value, &mode, &err)) {
        temporal_relative_to_destroy(&relative); return err;
      }
      options.rounding_mode = (RoundingMode_option){.ok = mode, .is_ok = true};
    }
    value = js_get(js, object, "smallestUnit");
    if (is_err(value)) { temporal_relative_to_destroy(&relative); return value; }
    if (vtype(value) != T_UNDEF) {
      Unit unit;
      if (!temporal_unit_from_value(js, value, false, &unit, &err)) {
        temporal_relative_to_destroy(&relative); return err;
      }
      options.smallest_unit = (Unit_option){.ok = unit, .is_ok = true};
    }
  }
  relative.value.date = relative.date;
  relative.value.zoned = relative.zoned;
  temporal_rs_Duration_round_with_provider_result result = temporal_rs_Duration_round_with_provider(
    self, options, relative.value, temporal_provider(js));
  temporal_relative_to_destroy(&relative);
  if (!result.is_ok) return temporal_error(js, result.err);
  return temporal_wrap(js, TEMPORAL_DURATION, result.ok);
}

static bool temporal_duration_exact_integral_total(
  const Duration *duration, Unit unit, double *out
) {
  if (temporal_rs_Duration_years(duration) != 0 ||
      temporal_rs_Duration_months(duration) != 0 ||
      temporal_rs_Duration_weeks(duration) != 0) return false;
  double microseconds = temporal_rs_Duration_microseconds(duration);
  double nanoseconds = temporal_rs_Duration_nanoseconds(duration);
  if (!isfinite(microseconds) || !isfinite(nanoseconds) ||
      trunc(microseconds) != microseconds || trunc(nanoseconds) != nanoseconds)
    return false;
  __int128 days = (__int128)temporal_rs_Duration_weeks(duration) * 7 +
    temporal_rs_Duration_days(duration);
  __int128 total = days;
  total = total * 24 + temporal_rs_Duration_hours(duration);
  total = total * 60 + temporal_rs_Duration_minutes(duration);
  total = total * 60 + temporal_rs_Duration_seconds(duration);
  total = total * 1000 + temporal_rs_Duration_milliseconds(duration);
  total = total * 1000 + (__int128)microseconds;
  total = total * 1000 + (__int128)nanoseconds;
  __int128 divisor;
  switch (unit) {
    case Unit_Nanosecond: divisor = 1; break;
    case Unit_Microsecond: divisor = 1000; break;
    case Unit_Millisecond: divisor = 1000000; break;
    case Unit_Second: divisor = 1000000000; break;
    case Unit_Minute: divisor = (__int128)60 * 1000000000; break;
    case Unit_Hour: divisor = (__int128)3600 * 1000000000; break;
    case Unit_Day: divisor = (__int128)86400 * 1000000000; break;
    default: return false;
  }
  if (total % divisor != 0) return false;
  __int128 quotient = total / divisor;
  *out = (double)quotient;
  return true;
}

static ant_value_t temporal_duration_total(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err = js_mkundef();
  Duration *self = temporal_duration_this(js, "Temporal.Duration.prototype.total", &err);
  if (!self) return err;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "total options are required");
  ant_value_t unit_value;
  temporal_relative_to_t relative = {0};
  if (vtype(args[0]) == T_STR) unit_value = args[0];
  else if (is_object_type(args[0])) {
    ant_value_t relative_value = js_get(js, args[0], "relativeTo");
    if (is_err(relative_value)) return relative_value;
    if (!temporal_relative_to(js, relative_value, &relative, &err)) return err;
    unit_value = js_get(js, args[0], "unit");
  }
  else return js_mkerr_typed(js, JS_ERR_TYPE, "total options must be a string or object");
  if (is_err(unit_value)) { temporal_relative_to_destroy(&relative); return unit_value; }
  if (vtype(unit_value) == T_UNDEF) {
    temporal_relative_to_destroy(&relative);
    return js_mkerr_typed(js, JS_ERR_RANGE, "unit is required");
  }
  Unit unit;
  if (!temporal_unit_from_value(js, unit_value, false, &unit, &err)) {
    temporal_relative_to_destroy(&relative); return err;
  }
  double exact_total;
  if (!relative.date && !relative.zoned &&
      temporal_duration_exact_integral_total(self, unit, &exact_total)) {
    temporal_relative_to_destroy(&relative);
    return js_mknum(exact_total);
  }
  temporal_rs_Duration_total_with_provider_result result = temporal_rs_Duration_total_with_provider(
    self, unit, relative.value, temporal_provider(js));
  temporal_relative_to_destroy(&relative);
  return result.is_ok ? js_mknum(result.ok) : temporal_error(js, result.err);
}

static ant_value_t temporal_duration_value_of(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert Temporal.Duration to a primitive value");
}

void temporal_init_duration(ant_t *js, ant_value_t temporal) {
  ant_value_t proto = js_mkobj(js);
  js_set_proto_init(proto, js->sym.object_proto);
  js->builtins.temporal_duration_proto = proto;

  TEMPORAL_GETTER(js, proto, "years", temporal_duration_get_years);
  TEMPORAL_GETTER(js, proto, "months", temporal_duration_get_months);
  TEMPORAL_GETTER(js, proto, "weeks", temporal_duration_get_weeks);
  TEMPORAL_GETTER(js, proto, "days", temporal_duration_get_days);
  TEMPORAL_GETTER(js, proto, "hours", temporal_duration_get_hours);
  TEMPORAL_GETTER(js, proto, "minutes", temporal_duration_get_minutes);
  TEMPORAL_GETTER(js, proto, "seconds", temporal_duration_get_seconds);
  TEMPORAL_GETTER(js, proto, "milliseconds", temporal_duration_get_milliseconds);
  TEMPORAL_GETTER(js, proto, "microseconds", temporal_duration_get_microseconds);
  TEMPORAL_GETTER(js, proto, "nanoseconds", temporal_duration_get_nanoseconds);
  TEMPORAL_GETTER(js, proto, "sign", temporal_duration_get_sign);
  TEMPORAL_GETTER(js, proto, "blank", temporal_duration_get_blank);
  TEMPORAL_METHOD(js, proto, "abs", temporal_duration_abs, 0);
  TEMPORAL_METHOD(js, proto, "add", temporal_duration_add, 1);
  TEMPORAL_METHOD(js, proto, "negated", temporal_duration_negated, 0);
  TEMPORAL_METHOD(js, proto, "round", temporal_duration_round, 1);
  TEMPORAL_METHOD(js, proto, "subtract", temporal_duration_subtract, 1);
  TEMPORAL_METHOD(js, proto, "toString", temporal_duration_to_string, 0);
  TEMPORAL_METHOD(js, proto, "total", temporal_duration_total, 1);
  TEMPORAL_METHOD(js, proto, "toJSON", temporal_duration_to_string_default, 0);
  TEMPORAL_METHOD(js, proto, "toLocaleString", temporal_duration_to_string_default, 0);
  TEMPORAL_METHOD(js, proto, "valueOf", temporal_duration_value_of, 0);
  TEMPORAL_METHOD(js, proto, "with", temporal_duration_with, 1);
  temporal_set_to_string_tag(js, proto, "Temporal.Duration");

  ant_value_t ctor = js_make_ctor(
    js, temporal_duration_ctor, proto, "Duration", sizeof("Duration") - 1);
  temporal_set_length(js, ctor, 0);
  TEMPORAL_METHOD(js, ctor, "compare", temporal_duration_compare, 2);
  TEMPORAL_METHOD(js, ctor, "from", temporal_duration_from, 1);
  temporal_set_namespace_property(js, temporal, "Duration", ctor);
}

#endif
