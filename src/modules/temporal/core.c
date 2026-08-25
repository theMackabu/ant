#include "modules/temporal.h"

#ifdef ANT_HAVE_TEMPORAL
#include "temporal_internal.h"

uint32_t temporal_native_tag(temporal_kind_t kind) {
  static const uint32_t tags[] = {
    TEMPORAL_DURATION_TAG,
    TEMPORAL_INSTANT_TAG,
    TEMPORAL_PLAIN_DATE_TAG,
    TEMPORAL_PLAIN_DATETIME_TAG,
    TEMPORAL_PLAIN_MONTHDAY_TAG,
    TEMPORAL_PLAIN_TIME_TAG,
    TEMPORAL_PLAIN_YEARMONTH_TAG,
    TEMPORAL_ZONED_DATETIME_TAG,
  };
  return tags[kind];
}

static ant_value_t temporal_proto(ant_t *js, temporal_kind_t kind) {
  switch (kind) {
    case TEMPORAL_DURATION: return js->builtins.temporal_duration_proto;
    case TEMPORAL_INSTANT: return js->builtins.temporal_instant_proto;
    case TEMPORAL_PLAIN_DATE: return js->builtins.temporal_plain_date_proto;
    case TEMPORAL_PLAIN_DATETIME: return js->builtins.temporal_plain_datetime_proto;
    case TEMPORAL_PLAIN_MONTHDAY: return js->builtins.temporal_plain_monthday_proto;
    case TEMPORAL_PLAIN_TIME: return js->builtins.temporal_plain_time_proto;
    case TEMPORAL_PLAIN_YEARMONTH: return js->builtins.temporal_plain_yearmonth_proto;
    case TEMPORAL_ZONED_DATETIME: return js->builtins.temporal_zoned_datetime_proto;
  }
  return js_mkundef();
}

static void temporal_destroy_value(temporal_kind_t kind, void *ptr) {
  if (!ptr) return;
  switch (kind) {
    case TEMPORAL_DURATION: temporal_rs_Duration_destroy(ptr); break;
    case TEMPORAL_INSTANT: temporal_rs_Instant_destroy(ptr); break;
    case TEMPORAL_PLAIN_DATE: temporal_rs_PlainDate_destroy(ptr); break;
    case TEMPORAL_PLAIN_DATETIME: temporal_rs_PlainDateTime_destroy(ptr); break;
    case TEMPORAL_PLAIN_MONTHDAY: temporal_rs_PlainMonthDay_destroy(ptr); break;
    case TEMPORAL_PLAIN_TIME: temporal_rs_PlainTime_destroy(ptr); break;
    case TEMPORAL_PLAIN_YEARMONTH: temporal_rs_PlainYearMonth_destroy(ptr); break;
    case TEMPORAL_ZONED_DATETIME: temporal_rs_ZonedDateTime_destroy(ptr); break;
  }
}

static void temporal_value_finalize(ant_t *js, ant_object_t *obj) {
  (void)js;
  ant_value_t value = js_obj_from_ptr(obj);
  for (int kind = TEMPORAL_DURATION; kind <= TEMPORAL_ZONED_DATETIME; kind++) {
    uint32_t tag = temporal_native_tag((temporal_kind_t)kind);
    void *ptr = js_get_native(value, tag);
    if (!ptr) continue;
    js_clear_native(value, tag);
    temporal_destroy_value((temporal_kind_t)kind, ptr);
    return;
  }
}

static void temporal_namespace_finalize(ant_t *js, ant_object_t *obj) {
  (void)js;
  ant_value_t value = js_obj_from_ptr(obj);
  Provider *provider = js_get_native(value, TEMPORAL_PROVIDER_TAG);
  if (!provider) return;
  js_clear_native(value, TEMPORAL_PROVIDER_TAG);
  temporal_rs_Provider_destroy(provider);
}

Provider *temporal_provider(ant_t *js) {
  return js_get_native(js->builtins.temporal_namespace, TEMPORAL_PROVIDER_TAG);
}

ant_value_t temporal_error(ant_t *js, TemporalError err) {
  js_err_type_t type = JS_ERR_GENERIC;
  switch (err.kind) {
    case ErrorKind_Type: type = JS_ERR_TYPE; break;
    case ErrorKind_Range: type = JS_ERR_RANGE; break;
    case ErrorKind_Syntax: type = JS_ERR_SYNTAX; break;
    case ErrorKind_Assert: type = JS_ERR_INTERNAL; break;
    case ErrorKind_Generic: break;
  }
  if (err.msg.is_ok)
    return js_create_error(js, type, js_mkundef(), "%.*s", (int)err.msg.ok.len, err.msg.ok.data);
  return js_create_error(js, type, js_mkundef(), "Temporal operation failed");
}

static bool temporal_normalize_duration(Duration **duration, TemporalError *err) {
  Duration *value = *duration;
  temporal_rs_Duration_create_result normalized = temporal_rs_Duration_create(
    temporal_rs_Duration_years(value), temporal_rs_Duration_months(value),
    temporal_rs_Duration_weeks(value), temporal_rs_Duration_days(value),
    temporal_rs_Duration_hours(value), temporal_rs_Duration_minutes(value),
    temporal_rs_Duration_seconds(value), temporal_rs_Duration_milliseconds(value),
    temporal_rs_Duration_microseconds(value), temporal_rs_Duration_nanoseconds(value));
  temporal_rs_Duration_destroy(value);
  if (!normalized.is_ok) { *err = normalized.err; *duration = NULL; return false; }
  *duration = normalized.ok;
  return true;
}

static ant_value_t temporal_wrap_on(
  ant_t *js, ant_value_t obj, temporal_kind_t kind, void *ptr, ant_value_t proto
) {
  if (!ptr) return js_mkerr_typed(js, JS_ERR_INTERNAL, "Temporal allocation failed");
  if (kind == TEMPORAL_DURATION) {
    TemporalError err = {0}; Duration *duration = ptr;
    if (!temporal_normalize_duration(&duration, &err)) return temporal_error(js, err);
    ptr = duration;
  }
  if (!is_object_type(obj)) obj = js_mkobj(js);
  if (is_err(obj)) {
    temporal_destroy_value(kind, ptr);
    return obj;
  }
  if (is_special_object(proto)) js_set_proto_init(obj, proto);
  js_set_native(obj, ptr, temporal_native_tag(kind));
  if (js_get_native(obj, temporal_native_tag(kind)) != ptr) {
    temporal_destroy_value(kind, ptr);
    return js_mkerr_typed(js, JS_ERR_INTERNAL, "Temporal allocation failed");
  }
  js_set_finalizer(obj, temporal_value_finalize);
  return obj;
}

ant_value_t temporal_wrap(ant_t *js, temporal_kind_t kind, void *ptr) {
  return temporal_wrap_on(js, js_mkobj(js), kind, ptr, temporal_proto(js, kind));
}

ant_value_t temporal_wrap_constructed(ant_t *js, temporal_kind_t kind, void *ptr) {
  ant_value_t proto = js_instance_proto_from_new_target(js, temporal_proto(js, kind));
  return temporal_wrap_on(js, js->this_val, kind, ptr, proto);
}

void *temporal_unwrap(
  ant_t *js, ant_value_t value, temporal_kind_t kind, const char *method, ant_value_t *err
) {
  void *ptr = is_object_type(value)
    ? js_get_native(value, temporal_native_tag(kind))
    : NULL;
  if (!ptr && err)
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "%s called on an incompatible receiver", method);
  return ptr;
}

ant_value_t temporal_string_from_write(ant_t *js, DiplomatWrite *write) {
  if (!write) return js_mkerr_typed(js, JS_ERR_INTERNAL, "Temporal string allocation failed");
  ant_value_t result = js_mkstr(
    js, diplomat_buffer_write_get_bytes(write), diplomat_buffer_write_len(write));
  diplomat_buffer_write_destroy(write);
  return result;
}

bool temporal_to_string_view(
  ant_t *js, ant_value_t value, DiplomatStringView *out, ant_value_t *root, ant_value_t *err
) {
  if (vtype(value) == kTypeSymbol) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a Symbol value to a string");
    return false;
  }
  *root = vtype(value) == kTypeString ? value : coerce_to_str(js, value);
  if (is_err(*root)) {
    *err = *root;
    return false;
  }
  size_t len = 0;
  const char *data = js_getstr(js, *root, &len);
  if (!data) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert value to a string");
    return false;
  }
  *out = (DiplomatStringView){data, len};
  return true;
}

bool temporal_to_number(
  ant_t *js, ant_value_t value, double *out, ant_value_t *err
) {
  ant_value_t primitive = is_object_type(value) ? js_to_primitive(js, value, 2) : value;
  if (is_err(primitive)) { *err = primitive; return false; }
  if (vtype(primitive) == kTypeSymbol || vtype(primitive) == kTypeBigInt) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert value to a number");
    return false;
  }
  *out = js_to_number(js, primitive);
  return true;
}

bool temporal_integer(
  ant_t *js, ant_value_t value, int64_t default_value, int64_t *out, ant_value_t *err
) {
  if (vtype(value) == kTypeUndefined) {
    *out = default_value;
    return true;
  }
  double number;
  if (!temporal_to_number(js, value, &number, err)) return false;
  if (!isfinite(number) || number < (double)INT64_MIN ||
      number > (double)INT64_MAX) {
    *err = js_mkerr_typed(js, JS_ERR_RANGE, "Temporal field must be a finite integer");
    return false;
  }
  *out = (int64_t)trunc(number);
  return true;
}

bool temporal_integral(
  ant_t *js, ant_value_t value, int64_t default_value, int64_t *out, ant_value_t *err
) {
  if (vtype(value) == kTypeUndefined) { *out = default_value; return true; }
  double number;
  if (!temporal_to_number(js, value, &number, err)) return false;
  if (!isfinite(number) || trunc(number) != number || number < (double)INT64_MIN ||
      number > (double)INT64_MAX) {
    *err = js_mkerr_typed(js, JS_ERR_RANGE, "Temporal field must be an integral number");
    return false;
  }
  *out = (int64_t)number;
  return true;
}

bool temporal_i128_from_value(
  ant_t *js, ant_value_t value, I128Nanoseconds *out, ant_value_t *err
) {
  if (vtype(value) == kTypeNumber) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a Number to a BigInt");
    return false;
  }
  ant_value_t bigint = bigint_from_value(js, value);
  if (is_err(bigint)) {
    *err = vtype(value) == kTypeString
      ? js_mkerr_typed(js, JS_ERR_SYNTAX, "Invalid BigInt syntax")
      : bigint;
    return false;
  }
  size_t len = bigint_digits_len(js, bigint);
  if (len == 0 || len > 39) {
    *err = js_mkerr_typed(js, JS_ERR_RANGE, "epoch nanoseconds are outside the supported range");
    return false;
  }
  char stack[48];
  if (strbigint(js, bigint, stack, sizeof(stack)) >= sizeof(stack)) {
    *err = js_mkerr_typed(js, JS_ERR_RANGE, "epoch nanoseconds are outside the supported range");
    return false;
  }
  const char *digits = stack;
  bool negative = *digits == '-';
  if (negative) digits++;
  unsigned __int128 magnitude = 0;
  const unsigned __int128 max = (((unsigned __int128)UINT64_MAX) << 63) | UINT64_MAX;
  for (; *digits; digits++) {
    if (*digits < '0' || *digits > '9') {
      *err = js_mkerr_typed(js, JS_ERR_TYPE, "Invalid BigInt");
      return false;
    }
    unsigned digit = (unsigned)(*digits - '0');
    if (magnitude > (max - digit) / 10) {
      *err = js_mkerr_typed(js, JS_ERR_RANGE, "epoch nanoseconds are outside the supported range");
      return false;
    }
    magnitude = magnitude * 10 + digit;
  }
  uint64_t high = (uint64_t)(magnitude >> 64);
  out->high = high | (negative ? UINT64_C(1) << 63 : 0);
  out->low = (uint64_t)magnitude;
  return true;
}

ant_value_t temporal_i128_to_bigint(ant_t *js, I128Nanoseconds value) {
  bool negative = (value.high & (UINT64_C(1) << 63)) != 0;
  unsigned __int128 magnitude =
    ((unsigned __int128)(value.high & ~(UINT64_C(1) << 63)) << 64) | value.low;
  char digits[48];
  char *end = digits + sizeof(digits);
  char *start = end;
  do {
    *--start = (char)('0' + magnitude % 10);
    magnitude /= 10;
  } while (magnitude != 0);
  return js_mkbigint(js, start, (size_t)(end - start), negative);
}

bool temporal_time_zone_from_value(
  ant_t *js, ant_value_t value, TimeZone *out, ant_value_t *err
) {
  if (is_object_type(value)) {
    ZonedDateTime *zdt = js_get_native(value, TEMPORAL_ZONED_DATETIME_TAG);
    if (zdt) { *out = temporal_rs_ZonedDateTime_timezone(zdt); return true; }
  }
  if (vtype(value) != kTypeString) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "time zone must be a string");
    return false;
  }
  DiplomatStringView view;
  ant_value_t root;
  if (!temporal_to_string_view(js, value, &view, &root, err)) return false;
  temporal_rs_TimeZone_try_from_str_with_provider_result result =
    temporal_rs_TimeZone_try_from_str_with_provider(view, temporal_provider(js));
  if (!result.is_ok) {
    *err = temporal_error(js, result.err);
    return false;
  }
  *out = result.ok;
  return true;
}

ant_value_t temporal_time_zone_identifier(ant_t *js, TimeZone zone) {
  DiplomatWrite *write = diplomat_buffer_write_create(32);
  if (!write) return js_mkerr_typed(js, JS_ERR_INTERNAL, "Temporal string allocation failed");
  temporal_rs_TimeZone_identifier_with_provider_result result =
    temporal_rs_TimeZone_identifier_with_provider(zone, temporal_provider(js), write);
  if (!result.is_ok) {
    diplomat_buffer_write_destroy(write);
    return temporal_error(js, result.err);
  }
  return temporal_string_from_write(js, write);
}

bool temporal_partial_time(
  ant_t *js, ant_value_t value, PartialTime *out, bool require_any, ant_value_t *err
) {
  if (!is_object_type(value)) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "Time-like value must be an object");
    return false;
  }
  memset(out, 0, sizeof(*out));
  struct {
    const char *name;
    size_t offset;
    bool wide;
  } fields[] = {
    {"hour", offsetof(PartialTime, hour), false},
    {"microsecond", offsetof(PartialTime, microsecond), true},
    {"millisecond", offsetof(PartialTime, millisecond), true},
    {"minute", offsetof(PartialTime, minute), false},
    {"nanosecond", offsetof(PartialTime, nanosecond), true},
    {"second", offsetof(PartialTime, second), false},
  };
  bool any = false;
  for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
    ant_value_t field = js_get(js, value, fields[i].name);
    if (is_err(field)) { *err = field; return false; }
    if (vtype(field) == kTypeUndefined) continue;
    int64_t integer;
    if (!temporal_integer(js, field, 0, &integer, err)) return false;
    any = true;
    if (fields[i].wide) {
      if (integer < 0 || integer > UINT16_MAX) {
        *err = js_mkerr_typed(js, JS_ERR_RANGE, "Temporal time field is outside the supported range");
        return false;
      }
      OptionU16 *slot = (OptionU16 *)((char *)out + fields[i].offset);
      *slot = (OptionU16){.ok = (uint16_t)integer, .is_ok = true};
    } else {
      if (integer < 0 || integer > UINT8_MAX) {
        *err = js_mkerr_typed(js, JS_ERR_RANGE, "Temporal time field is outside the supported range");
        return false;
      }
      OptionU8 *slot = (OptionU8 *)((char *)out + fields[i].offset);
      *slot = (OptionU8){.ok = (uint8_t)integer, .is_ok = true};
    }
  }
  if (require_any && !any) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "Time-like object must have at least one time field");
    return false;
  }
  return true;
}

static const Calendar *temporal_calendar_from_object(ant_value_t value) {
  void *ptr;
  if ((ptr = js_get_native(value, TEMPORAL_PLAIN_DATE_TAG)))
    return temporal_rs_PlainDate_calendar(ptr);
  if ((ptr = js_get_native(value, TEMPORAL_PLAIN_DATETIME_TAG)))
    return temporal_rs_PlainDateTime_calendar(ptr);
  if ((ptr = js_get_native(value, TEMPORAL_PLAIN_MONTHDAY_TAG)))
    return temporal_rs_PlainMonthDay_calendar(ptr);
  if ((ptr = js_get_native(value, TEMPORAL_PLAIN_YEARMONTH_TAG)))
    return temporal_rs_PlainYearMonth_calendar(ptr);
  if ((ptr = js_get_native(value, TEMPORAL_ZONED_DATETIME_TAG)))
    return temporal_rs_ZonedDateTime_calendar(ptr);
  return NULL;
}

bool temporal_calendar_kind(
  ant_t *js, ant_value_t value, AnyCalendarKind default_kind,
  AnyCalendarKind *out, ant_value_t *err
) {
  if (vtype(value) == kTypeUndefined) { *out = default_kind; return true; }
  if (is_object_type(value)) {
    const Calendar *calendar = temporal_calendar_from_object(value);
    if (!calendar) {
      *err = js_mkerr_typed(js, JS_ERR_TYPE, "calendar must be a string or a Temporal object");
      return false;
    }
    *out = temporal_rs_Calendar_kind(calendar);
    return true;
  }
  if (vtype(value) != kTypeString) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "calendar must be a string or a Temporal object");
    return false;
  }
  DiplomatStringView view;
  ant_value_t root;
  if (!temporal_to_string_view(js, value, &view, &root, err)) return false;
  temporal_rs_AnyCalendarKind_parse_temporal_calendar_string_result result =
    temporal_rs_AnyCalendarKind_parse_temporal_calendar_string(view);
  if (!result.is_ok) {
    *err = js_mkerr_typed(js, JS_ERR_RANGE, "invalid Temporal calendar string");
    return false;
  }
  *out = result.ok;
  return true;
}

bool temporal_calendar_identifier_kind(
  ant_t *js, ant_value_t value, AnyCalendarKind default_kind,
  AnyCalendarKind *out, ant_value_t *err
) {
  if (vtype(value) == kTypeUndefined) { *out = default_kind; return true; }
  if (is_object_type(value)) {
    const Calendar *calendar = temporal_calendar_from_object(value);
    if (!calendar) {
      *err = js_mkerr_typed(js, JS_ERR_TYPE, "calendar must be a string or a Temporal object");
      return false;
    }
    *out = temporal_rs_Calendar_kind(calendar);
    return true;
  }
  if (vtype(value) != kTypeString) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "calendar must be a string or a Temporal object");
    return false;
  }
  DiplomatStringView view; ant_value_t root;
  if (!temporal_to_string_view(js, value, &view, &root, err)) return false;
  temporal_rs_Calendar_from_utf8_result result = temporal_rs_Calendar_from_utf8(view);
  if (!result.is_ok) { *err = temporal_error(js, result.err); return false; }
  *out = temporal_rs_Calendar_kind(result.ok);
  temporal_rs_Calendar_destroy(result.ok);
  return true;
}

bool temporal_calendar_kind_from_property(
  ant_t *js, ant_value_t value, AnyCalendarKind default_kind,
  AnyCalendarKind *out, ant_value_t *err
) {
  if (vtype(value) == kTypeUndefined || is_object_type(value))
    return temporal_calendar_kind(js, value, default_kind, out, err);
  if (vtype(value) != kTypeString) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "calendar must be a string or a Temporal object");
    return false;
  }
  return temporal_calendar_kind(js, value, default_kind, out, err);
}

bool temporal_month_code_syntax(
  ant_t *js, ant_value_t value, DiplomatStringView *view, ant_value_t *root,
  ant_value_t *err
) {
  ant_value_t string = value;
  if (is_object_type(string)) {
    string = js_to_primitive(js, string, 1);
    if (is_err(string)) { *err = string; return false; }
  }
  if (vtype(string) != kTypeString) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "monthCode must be a string");
    return false;
  }
  if (!temporal_to_string_view(js, string, view, root, err)) return false;
  bool valid = (view->len == 3 || view->len == 4) && view->data[0] == 'M' &&
    view->data[1] >= '0' && view->data[1] <= '9' &&
    view->data[2] >= '0' && view->data[2] <= '9' &&
    (view->len == 3 || view->data[3] == 'L');
  if (!valid) {
    *err = js_mkerr_typed(js, JS_ERR_RANGE, "invalid Temporal monthCode");
    return false;
  }
  return true;
}

ant_value_t temporal_calendar_identifier(ant_t *js, const Calendar *calendar) {
  DiplomatStringView id = temporal_rs_Calendar_identifier(calendar);
  return js_mkstr(js, id.data, id.len);
}

bool temporal_partial_date_impl(
  ant_t *js, ant_value_t value, AnyCalendarKind default_calendar,
  temporal_partial_date_t *out, bool include_day, bool require_any,
  bool read_calendar, ant_value_t *err
) {
  if (!is_object_type(value)) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "Date-like value must be an object");
    return false;
  }
  memset(out, 0, sizeof(*out));
  out->month_code_root = js_mkundef();
  out->era_root = js_mkundef();
  out->partial.calendar = default_calendar;
  if (read_calendar) {
    ant_value_t calendar = js_get(js, value, "calendar");
    if (is_err(calendar)) { *err = calendar; return false; }
    if (!temporal_calendar_kind_from_property(
        js, calendar, default_calendar, &out->partial.calendar, err))
      return false;
  }
  bool any = false;
  struct {
    const char *name;
    size_t offset;
    int bits;
  } numbers[] = {
    {"day", offsetof(PartialDate, day), 8},
    {"eraYear", offsetof(PartialDate, era_year), 32},
    {"month", offsetof(PartialDate, month), 8},
    {"year", offsetof(PartialDate, year), 32},
  };
  if (include_day) {
    ant_value_t day = js_get(js, value, "day");
    if (is_err(day)) { *err = day; return false; }
    if (vtype(day) != kTypeUndefined) {
      int64_t integer;
      if (!temporal_integer(js, day, 0, &integer, err)) return false;
      if (integer < 1) {
        *err = js_mkerr_typed(js, JS_ERR_RANGE, "day is outside the supported range"); return false;
      }
      if (integer > UINT8_MAX) integer = UINT8_MAX;
      out->partial.day = (OptionU8){.ok = (uint8_t)integer, .is_ok = true};
      any = true;
    }
  }
  bool iso_calendar = out->partial.calendar == AnyCalendarKind_Iso;
  if (!iso_calendar) {
    ant_value_t era = js_get(js, value, "era");
    if (is_err(era)) { *err = era; return false; }
    if (vtype(era) != kTypeUndefined) {
      if (!temporal_to_string_view(js, era, &out->partial.era, &out->era_root, err)) return false;
      any = true;
    }
  }
  for (size_t i = 1; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
    if (i == 1 && iso_calendar) continue;
    ant_value_t field = js_get(js, value, numbers[i].name);
    if (is_err(field)) { *err = field; return false; }
    if (vtype(field) != kTypeUndefined) {
      int64_t integer;
      if (!temporal_integer(js, field, 0, &integer, err)) return false;
      if (numbers[i].bits == 8) {
        if (integer < 1) {
          *err = js_mkerr_typed(js, JS_ERR_RANGE, "%s is outside the supported range", numbers[i].name);
          return false;
        }
        if (integer > UINT8_MAX) integer = UINT8_MAX;
        OptionU8 *slot = (OptionU8 *)((char *)&out->partial + numbers[i].offset);
        *slot = (OptionU8){.ok = (uint8_t)integer, .is_ok = true};
      } else {
        if (integer < INT32_MIN || integer > INT32_MAX) {
          *err = js_mkerr_typed(js, JS_ERR_RANGE, "%s is outside the supported range", numbers[i].name);
          return false;
        }
        OptionI32 *slot = (OptionI32 *)((char *)&out->partial + numbers[i].offset);
        *slot = (OptionI32){.ok = (int32_t)integer, .is_ok = true};
      }
      any = true;
    }
    if (i == 2) {
      ant_value_t month_code = js_get(js, value, "monthCode");
      if (is_err(month_code)) { *err = month_code; return false; }
      if (vtype(month_code) != kTypeUndefined) {
        if (!temporal_month_code_syntax(js, month_code, &out->partial.month_code,
                                        &out->month_code_root, err)) return false;
        any = true;
      }
    }
  }
  if (require_any && !any) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "Date-like object must have at least one date field");
    return false;
  }
  return true;
}

bool temporal_partial_date(
  ant_t *js, ant_value_t value, AnyCalendarKind default_calendar,
  temporal_partial_date_t *out, bool include_day, bool require_any, ant_value_t *err
) {
  return temporal_partial_date_impl(
    js, value, default_calendar, out, include_day, require_any, true, err);
}

bool temporal_integer_property(
  ant_t *js, ant_value_t object, const char *name, int64_t minimum, int64_t maximum,
  bool *present, int64_t *integer, ant_value_t *err
) {
  ant_value_t value = js_get(js, object, name);
  if (is_err(value)) { *err = value; return false; }
  *present = vtype(value) != kTypeUndefined;
  if (!*present) return true;
  if (!temporal_integer(js, value, 0, integer, err)) return false;
  if (*integer < minimum || *integer > maximum) {
    *err = js_mkerr_typed(js, JS_ERR_RANGE, "%s is outside the supported range", name);
    return false;
  }
  return true;
}

bool temporal_validate_partial_object(
  ant_t *js, ant_value_t value, ant_value_t *err
) {
  if (!is_object_type(value)) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "partial Temporal value must be an object");
    return false;
  }
  for (int kind = TEMPORAL_DURATION; kind <= TEMPORAL_ZONED_DATETIME; kind++) {
    if (js_get_native(value, temporal_native_tag((temporal_kind_t)kind))) {
      *err = js_mkerr_typed(js, JS_ERR_TYPE, "Temporal objects are not valid partial values");
      return false;
    }
  }
  ant_value_t disallowed = js_get(js, value, "calendar");
  if (is_err(disallowed)) { *err = disallowed; return false; }
  if (vtype(disallowed) != kTypeUndefined) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "calendar is not allowed in a partial value");
    return false;
  }
  disallowed = js_get(js, value, "timeZone");
  if (is_err(disallowed)) { *err = disallowed; return false; }
  if (vtype(disallowed) != kTypeUndefined) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "timeZone is not allowed in a partial value");
    return false;
  }
  return true;
}

bool temporal_options_object(
  ant_t *js, ant_value_t value, bool string_allowed, ant_value_t *out, ant_value_t *err
) {
  if (vtype(value) == kTypeUndefined) { *out = js_mkundef(); return true; }
  if (string_allowed && vtype(value) == kTypeString) { *out = value; return true; }
  if (!is_object_type(value)) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "options must be an object");
    return false;
  }
  *out = value;
  return true;
}

bool temporal_string_equals(DiplomatStringView view, const char *literal) {
  size_t len = strlen(literal);
  return view.len == len && memcmp(view.data, literal, len) == 0;
}

bool temporal_unit_from_value(
  ant_t *js, ant_value_t value, bool allow_auto, Unit *out, ant_value_t *err
) {
  DiplomatStringView view; ant_value_t root;
  if (!temporal_to_string_view(js, value, &view, &root, err)) return false;
  size_t len = view.len;
  if (len > 1 && view.data[len - 1] == 's') len--;
#define UNIT_CASE(text, unit) \
  if (len == sizeof(text) - 1 && memcmp(view.data, text, len) == 0) { *out = (unit); return true; }
  if (allow_auto && temporal_string_equals(view, "auto")) { *out = Unit_Auto; return true; }
  UNIT_CASE("nanosecond", Unit_Nanosecond)
  UNIT_CASE("microsecond", Unit_Microsecond)
  UNIT_CASE("millisecond", Unit_Millisecond)
  UNIT_CASE("second", Unit_Second)
  UNIT_CASE("minute", Unit_Minute)
  UNIT_CASE("hour", Unit_Hour)
  UNIT_CASE("day", Unit_Day)
  UNIT_CASE("week", Unit_Week)
  UNIT_CASE("month", Unit_Month)
  UNIT_CASE("year", Unit_Year)
#undef UNIT_CASE
  *err = js_mkerr_typed(js, JS_ERR_RANGE, "invalid Temporal unit");
  return false;
}

bool temporal_rounding_mode_from_value(
  ant_t *js, ant_value_t value, RoundingMode *out, ant_value_t *err
) {
  DiplomatStringView view; ant_value_t root;
  if (!temporal_to_string_view(js, value, &view, &root, err)) return false;
#define ROUND_MODE_CASE(text, mode) \
  if (temporal_string_equals(view, text)) { *out = (mode); return true; }
  ROUND_MODE_CASE("ceil", RoundingMode_Ceil)
  ROUND_MODE_CASE("floor", RoundingMode_Floor)
  ROUND_MODE_CASE("expand", RoundingMode_Expand)
  ROUND_MODE_CASE("trunc", RoundingMode_Trunc)
  ROUND_MODE_CASE("halfCeil", RoundingMode_HalfCeil)
  ROUND_MODE_CASE("halfFloor", RoundingMode_HalfFloor)
  ROUND_MODE_CASE("halfExpand", RoundingMode_HalfExpand)
  ROUND_MODE_CASE("halfTrunc", RoundingMode_HalfTrunc)
  ROUND_MODE_CASE("halfEven", RoundingMode_HalfEven)
#undef ROUND_MODE_CASE
  *err = js_mkerr_typed(js, JS_ERR_RANGE, "invalid Temporal rounding mode");
  return false;
}

bool temporal_rounding_increment(
  ant_t *js, ant_value_t value, OptionU32 *out, ant_value_t *err
) {
  if (vtype(value) == kTypeUndefined) return true;
  double number;
  if (!temporal_to_number(js, value, &number, err)) return false;
  if (!isfinite(number)) {
    *err = js_mkerr_typed(js, JS_ERR_RANGE, "invalid Temporal rounding increment");
    return false;
  }
  number = trunc(number);
  if (number < 1 || number > 1000000000.0) {
    *err = js_mkerr_typed(js, JS_ERR_RANGE, "invalid Temporal rounding increment");
    return false;
  }
  *out = (OptionU32){.ok = (uint32_t)number, .is_ok = true};
  return true;
}

bool temporal_rounding_options(
  ant_t *js, ant_value_t input, bool require_smallest, bool allow_largest,
  RoundingOptions *out, ant_value_t *err
) {
  memset(out, 0, sizeof(*out));
  ant_value_t options;
  if (!temporal_options_object(js, input, true, &options, err)) return false;
  if (vtype(options) == kTypeUndefined && require_smallest) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "round options are required");
    return false;
  }
  if (vtype(options) == kTypeString) {
    Unit unit;
    if (!temporal_unit_from_value(js, options, false, &unit, err)) return false;
    out->smallest_unit = (Unit_option){.ok = unit, .is_ok = true};
    return true;
  }
  ant_value_t value;
  if (is_object_type(options)) {
    if (allow_largest) {
      value = js_get(js, options, "largestUnit"); if (is_err(value)) { *err = value; return false; }
      if (vtype(value) != kTypeUndefined) {
        Unit unit; if (!temporal_unit_from_value(js, value, true, &unit, err)) return false;
        out->largest_unit = (Unit_option){.ok = unit, .is_ok = true};
      }
    }
    value = js_get(js, options, "roundingIncrement"); if (is_err(value)) { *err = value; return false; }
    if (!temporal_rounding_increment(js, value, &out->increment, err)) return false;
    value = js_get(js, options, "roundingMode"); if (is_err(value)) { *err = value; return false; }
    if (vtype(value) != kTypeUndefined) {
      RoundingMode parsed; if (!temporal_rounding_mode_from_value(js, value, &parsed, err)) return false;
      out->rounding_mode = (RoundingMode_option){.ok = parsed, .is_ok = true};
    }
    value = js_get(js, options, "smallestUnit"); if (is_err(value)) { *err = value; return false; }
    if (vtype(value) != kTypeUndefined) {
      Unit unit; if (!temporal_unit_from_value(js, value, false, &unit, err)) return false;
      out->smallest_unit = (Unit_option){.ok = unit, .is_ok = true};
    } else if (require_smallest) {
      *err = js_mkerr_typed(js, JS_ERR_RANGE, "smallestUnit is required");
      return false;
    }
  } else if (require_smallest) {
    *err = js_mkerr_typed(js, JS_ERR_RANGE, "smallestUnit is required");
    return false;
  }
  return true;
}

bool temporal_difference_settings(
  ant_t *js, ant_value_t input, DifferenceSettings *out, ant_value_t *err
) {
  if (vtype(input) != kTypeUndefined && !is_object_type(input)) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "options must be an object");
    return false;
  }
  RoundingOptions options;
  if (!temporal_rounding_options(js, input, false, true, &options, err)) return false;
  *out = (DifferenceSettings){
    .largest_unit = options.largest_unit,
    .smallest_unit = options.smallest_unit,
    .rounding_mode = options.rounding_mode,
    .increment = options.increment,
  };
  return true;
}

bool temporal_overflow_option(
  ant_t *js, ant_value_t input, ArithmeticOverflow_option *out, ant_value_t *err
) {
  ant_value_t options;
  if (!temporal_options_object(js, input, false, &options, err)) return false;
  if (!is_object_type(options)) return true;
  ant_value_t value = js_get(js, options, "overflow");
  if (is_err(value)) { *err = value; return false; }
  if (vtype(value) == kTypeUndefined) return true;
  DiplomatStringView view; ant_value_t root;
  if (!temporal_to_string_view(js, value, &view, &root, err)) return false;
  if (temporal_string_equals(view, "constrain"))
    *out = (ArithmeticOverflow_option){.ok = ArithmeticOverflow_Constrain, .is_ok = true};
  else if (temporal_string_equals(view, "reject"))
    *out = (ArithmeticOverflow_option){.ok = ArithmeticOverflow_Reject, .is_ok = true};
  else { *err = js_mkerr_typed(js, JS_ERR_RANGE, "invalid overflow option"); return false; }
  return true;
}

bool temporal_disambiguation_value(
  ant_t *js, ant_value_t value, Disambiguation *out, ant_value_t *err
) {
  DiplomatStringView view; ant_value_t root;
  if (!temporal_to_string_view(js, value, &view, &root, err)) return false;
  if (temporal_string_equals(view, "compatible")) *out = Disambiguation_Compatible;
  else if (temporal_string_equals(view, "earlier")) *out = Disambiguation_Earlier;
  else if (temporal_string_equals(view, "later")) *out = Disambiguation_Later;
  else if (temporal_string_equals(view, "reject")) *out = Disambiguation_Reject;
  else { *err = js_mkerr_typed(js, JS_ERR_RANGE, "invalid disambiguation option"); return false; }
  return true;
}

bool temporal_offset_disambiguation_value(
  ant_t *js, ant_value_t value, OffsetDisambiguation *out, ant_value_t *err
) {
  DiplomatStringView view; ant_value_t root;
  if (!temporal_to_string_view(js, value, &view, &root, err)) return false;
  if (temporal_string_equals(view, "use")) *out = OffsetDisambiguation_Use;
  else if (temporal_string_equals(view, "prefer")) *out = OffsetDisambiguation_Prefer;
  else if (temporal_string_equals(view, "ignore")) *out = OffsetDisambiguation_Ignore;
  else if (temporal_string_equals(view, "reject")) *out = OffsetDisambiguation_Reject;
  else { *err = js_mkerr_typed(js, JS_ERR_RANGE, "invalid offset option"); return false; }
  return true;
}

bool temporal_zdt_options(
  ant_t *js, ant_value_t input, Disambiguation_option *disambiguation,
  OffsetDisambiguation_option *offset, ArithmeticOverflow_option *overflow,
  ant_value_t *err
) {
  ant_value_t options;
  if (!temporal_options_object(js, input, false, &options, err)) return false;
  if (!is_object_type(options)) return true;
  ant_value_t value = js_get(js, options, "disambiguation");
  if (is_err(value)) { *err = value; return false; }
  if (vtype(value) != kTypeUndefined) {
    Disambiguation parsed;
    if (!temporal_disambiguation_value(js, value, &parsed, err)) return false;
    *disambiguation = (Disambiguation_option){.ok = parsed, .is_ok = true};
  }
  value = js_get(js, options, "offset");
  if (is_err(value)) { *err = value; return false; }
  if (vtype(value) != kTypeUndefined) {
    OffsetDisambiguation parsed;
    if (!temporal_offset_disambiguation_value(js, value, &parsed, err)) return false;
    *offset = (OffsetDisambiguation_option){.ok = parsed, .is_ok = true};
  }
  return temporal_overflow_option(js, options, overflow, err);
}

bool temporal_to_string_options(
  ant_t *js, ant_value_t input, unsigned fields,
  temporal_to_string_options_t *out, ant_value_t *err
) {
  memset(out, 0, sizeof(*out));
  out->calendar = DisplayCalendar_Auto;
  out->offset = DisplayOffset_Auto;
  out->time_zone_name = DisplayTimeZone_Auto;
  ant_value_t options;
  if (!temporal_options_object(js, input, false, &options, err)) return false;
  if (!is_object_type(options)) return true;
  ant_value_t value;
  if (fields & TEMPORAL_TOSTRING_CALENDAR) {
    value = js_get(js, options, "calendarName"); if (is_err(value)) { *err = value; return false; }
  if (vtype(value) != kTypeUndefined) {
    DiplomatStringView view; ant_value_t root; if (!temporal_to_string_view(js, value, &view, &root, err)) return false;
    if (temporal_string_equals(view, "auto")) out->calendar = DisplayCalendar_Auto;
    else if (temporal_string_equals(view, "always")) out->calendar = DisplayCalendar_Always;
    else if (temporal_string_equals(view, "never")) out->calendar = DisplayCalendar_Never;
    else if (temporal_string_equals(view, "critical")) out->calendar = DisplayCalendar_Critical;
    else { *err = js_mkerr_typed(js, JS_ERR_RANGE, "invalid calendarName option"); return false; }
  }
  }
  if (fields & TEMPORAL_TOSTRING_DIGITS) {
    value = js_get(js, options, "fractionalSecondDigits"); if (is_err(value)) { *err = value; return false; }
  if (vtype(value) != kTypeUndefined) {
    if (vtype(value) == kTypeNumber) {
      double number = floor(tod(value));
      if (!isfinite(number) || number < 0 || number > 9) {
        *err = js_mkerr_typed(js, JS_ERR_RANGE, "invalid fractionalSecondDigits"); return false;
      }
      out->rounding.precision.precision = (OptionU8){.ok = (uint8_t)number, .is_ok = true};
    } else {
      DiplomatStringView view; ant_value_t root; if (!temporal_to_string_view(js, value, &view, &root, err)) return false;
      if (!temporal_string_equals(view, "auto")) { *err = js_mkerr_typed(js, JS_ERR_RANGE, "invalid fractionalSecondDigits"); return false; }
    }
  }
  }
  if (fields & TEMPORAL_TOSTRING_OFFSET) {
    value = js_get(js, options, "offset"); if (is_err(value)) { *err = value; return false; }
  if (vtype(value) != kTypeUndefined) {
    DiplomatStringView view; ant_value_t root; if (!temporal_to_string_view(js, value, &view, &root, err)) return false;
    if (temporal_string_equals(view, "auto")) out->offset = DisplayOffset_Auto;
    else if (temporal_string_equals(view, "never")) out->offset = DisplayOffset_Never;
    else { *err = js_mkerr_typed(js, JS_ERR_RANGE, "invalid offset option"); return false; }
  }
  }
  if (fields & TEMPORAL_TOSTRING_ROUNDING_MODE) {
    value = js_get(js, options, "roundingMode"); if (is_err(value)) { *err = value; return false; }
  if (vtype(value) != kTypeUndefined) {
    RoundingMode parsed; if (!temporal_rounding_mode_from_value(js, value, &parsed, err)) return false;
    out->rounding.rounding_mode = (RoundingMode_option){.ok = parsed, .is_ok = true};
  }
  }
  if (fields & TEMPORAL_TOSTRING_SMALLEST_UNIT) {
    value = js_get(js, options, "smallestUnit"); if (is_err(value)) { *err = value; return false; }
  if (vtype(value) != kTypeUndefined) {
    Unit unit; if (!temporal_unit_from_value(js, value, false, &unit, err)) return false;
    out->rounding.smallest_unit = (Unit_option){.ok = unit, .is_ok = true};
  }
  }
  if (fields & TEMPORAL_TOSTRING_TIME_ZONE) {
    value = js_get(js, options, "timeZone"); if (is_err(value)) { *err = value; return false; }
  if (vtype(value) != kTypeUndefined) {
    TimeZone parsed; if (!temporal_time_zone_from_value(js, value, &parsed, err)) return false;
    out->time_zone = (TimeZone_option){.ok = parsed, .is_ok = true};
  }
  }
  if (fields & TEMPORAL_TOSTRING_TIME_ZONE_NAME) {
    value = js_get(js, options, "timeZoneName"); if (is_err(value)) { *err = value; return false; }
  if (vtype(value) != kTypeUndefined) {
    DiplomatStringView view; ant_value_t root; if (!temporal_to_string_view(js, value, &view, &root, err)) return false;
    if (temporal_string_equals(view, "auto")) out->time_zone_name = DisplayTimeZone_Auto;
    else if (temporal_string_equals(view, "never")) out->time_zone_name = DisplayTimeZone_Never;
    else if (temporal_string_equals(view, "critical")) out->time_zone_name = DisplayTimeZone_Critical;
    else { *err = js_mkerr_typed(js, JS_ERR_RANGE, "invalid timeZoneName option"); return false; }
  }
  }
  return true;
}

void temporal_relative_to_destroy(temporal_relative_to_t *relative) {
  if (relative->date) temporal_rs_PlainDate_destroy(relative->date);
  if (relative->zoned) temporal_rs_ZonedDateTime_destroy(relative->zoned);
  memset(relative, 0, sizeof(*relative));
}

bool temporal_relative_to(
  ant_t *js, ant_value_t input, temporal_relative_to_t *out, ant_value_t *err
) {
  memset(out, 0, sizeof(*out));
  if (vtype(input) == kTypeUndefined) return true;
  if (is_object_type(input)) {
    PlainDate *date = js_get_native(input, TEMPORAL_PLAIN_DATE_TAG);
    ZonedDateTime *zoned = js_get_native(input, TEMPORAL_ZONED_DATETIME_TAG);
    if (date) out->date = temporal_rs_PlainDate_clone(date);
    else if (zoned) out->zoned = temporal_rs_ZonedDateTime_clone(zoned);
    else {
      temporal_partial_zdt_t partial; bool has_zone = false;
      if (!temporal_partial_zdt(
          js, input, AnyCalendarKind_Iso, &partial, true, false,
          true, true, &has_zone, err)) return false;
      if (has_zone) {
        temporal_rs_ZonedDateTime_from_partial_with_provider_result result =
          temporal_rs_ZonedDateTime_from_partial_with_provider(
            partial.partial, (ArithmeticOverflow_option){0},
            (Disambiguation_option){0}, (OffsetDisambiguation_option){0},
            temporal_provider(js));
        if (!result.is_ok) { *err = temporal_error(js, result.err); return false; }
        out->zoned = result.ok;
      } else {
        temporal_rs_PlainDate_from_partial_result result =
          temporal_rs_PlainDate_from_partial(
            partial.partial.date, (ArithmeticOverflow_option){0});
        if (!result.is_ok) { *err = temporal_error(js, result.err); return false; }
        out->date = result.ok;
      }
    }
  } else {
    if (vtype(input) != kTypeString) {
      *err = js_mkerr_typed(js, JS_ERR_TYPE, "relativeTo must be a string or object");
      return false;
    }
    DiplomatStringView view; ant_value_t root;
    if (!temporal_to_string_view(js, input, &view, &root, err)) return false;
    bool has_time_zone = false;
    for (size_t i = 0; i < view.len; i++) {
      if (view.data[i] != '[') continue;
      if (i + 6 < view.len && memcmp(view.data + i + 1, "u-ca=", 5) == 0) continue;
      has_time_zone = true;
      break;
    }
    if (has_time_zone) {
      temporal_rs_ZonedDateTime_from_utf8_with_provider_result zdt =
        temporal_rs_ZonedDateTime_from_utf8_with_provider(view, Disambiguation_Compatible,
          OffsetDisambiguation_Reject, temporal_provider(js));
      if (!zdt.is_ok) { *err = temporal_error(js, zdt.err); return false; }
      out->zoned = zdt.ok;
    } else {
      temporal_rs_PlainDate_from_utf8_result date = temporal_rs_PlainDate_from_utf8(view);
      if (!date.is_ok) { *err = temporal_error(js, date.err); return false; }
      out->date = date.ok;
    }
  }
  out->value.date = out->date;
  out->value.zoned = out->zoned;
  return true;
}

bool temporal_relative_to_from_options(
  ant_t *js, ant_value_t options, temporal_relative_to_t *out, ant_value_t *err
) {
  if (vtype(options) == kTypeUndefined) { memset(out, 0, sizeof(*out)); return true; }
  if (!is_object_type(options)) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "options must be an object"); return false;
  }
  ant_value_t relative = js_get(js, options, "relativeTo");
  if (is_err(relative)) { *err = relative; return false; }
  return temporal_relative_to(js, relative, out, err);
}

ant_value_t temporal_require_new(ant_t *js, const char *name) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "%s constructor requires 'new'", name);
}

void temporal_set_length(ant_t *js, ant_value_t fn, int length) {
  ant_value_t obj = js_obj_from_ptr(js_obj_ptr(fn));
  js_mkprop_fast(js, obj, "length", 6, js_mknum(length));
  js_set_descriptor(js, obj, "length", 6, JS_DESC_C);
  js_set_descriptor(js, obj, "name", 4, JS_DESC_C);
  js_set_descriptor(js, obj, "prototype", 9, 0);
}

void temporal_set_namespace_property(
  ant_t *js, ant_value_t temporal, const char *name, ant_value_t value
) {
  js_set(js, temporal, name, value);
  js_set_descriptor(js, temporal, name, strlen(name), JS_DESC_W | JS_DESC_C);
}

ant_value_t temporal_make_function(
  ant_t *js, ant_cfunc_t callback, const char *name, size_t name_len, int length
) {
  ant_value_t obj = js_mkobj(js);
  js_set_proto_init(obj, js->sym.function_proto);
  js_set_slot(obj, SLOT_CFUNC, js_mkfun_dyn(callback));
  js_mkprop_fast(js, obj, "name", 4, js_mkstr(js, name, name_len));
  js_set_descriptor(js, obj, "name", 4, JS_DESC_C);
  js_mkprop_fast(js, obj, "length", 6, js_mknum(length));
  js_set_descriptor(js, obj, "length", 6, JS_DESC_C);
  return js_obj_to_func(js, obj);
}

void temporal_set_to_string_tag(ant_t *js, ant_value_t obj, const char *tag) {
  ant_value_t symbol = get_toStringTag_sym();
  js_set_sym(js, obj, symbol, js_mkstr(js, tag, strlen(tag)));
  js_set_sym_descriptor(js, obj, symbol, JS_DESC_C);
}

void init_temporal_module(ant_t *js) {
  ant_value_t temporal = js_mkobj(js);
  js_set_proto_init(temporal, js->sym.object_proto);
  js->builtins.temporal_namespace = temporal;

#ifdef _WIN32
  Provider *provider = temporal_rs_Provider_new_compiled();
#else
  Provider *provider = temporal_rs_Provider_new_fs();
#endif
  if (!provider) provider = temporal_rs_Provider_empty();
  js_set_native(temporal, provider, TEMPORAL_PROVIDER_TAG);
  js_set_finalizer(temporal, temporal_namespace_finalize);
  temporal_set_to_string_tag(js, temporal, "Temporal");

  temporal_init_duration(js, temporal);
  temporal_init_instant(js, temporal);
  temporal_init_plain_date(js, temporal);
  temporal_init_plain_datetime(js, temporal);
  temporal_init_plain_monthday(js, temporal);
  temporal_init_plain_time(js, temporal);
  temporal_init_plain_yearmonth(js, temporal);
  temporal_init_zdt(js, temporal);
  temporal_init_now(js, temporal);

  js_set_global_builtin(js, "Temporal", temporal);
}

#endif
