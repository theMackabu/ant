#include "modules/temporal.h"

#ifdef ANT_HAVE_TEMPORAL
#include "temporal_internal.h"

static I128Nanoseconds temporal_now_nanoseconds(void) {
  struct timespec now = {0};
  timespec_get(&now, TIME_UTC);
  __int128 value = (__int128)now.tv_sec * 1000000000 + now.tv_nsec;
  bool negative = value < 0;
  unsigned __int128 magnitude = negative ? (unsigned __int128)-value : (unsigned __int128)value;
  return (I128Nanoseconds){
    .high = (uint64_t)(magnitude >> 64) | (negative ? UINT64_C(1) << 63 : 0),
    .low = (uint64_t)magnitude,
  };
}

static size_t temporal_system_time_zone(char *buffer, size_t capacity) {
  const char *tz = getenv("TZ");
  if (tz && *tz) {
    if (*tz == ':') tz++;
    size_t len = strlen(tz);
    if (len < capacity) { memcpy(buffer, tz, len + 1); return len; }
  }
#ifndef _WIN32
  char path[512];
  ssize_t len = readlink("/etc/localtime", path, sizeof(path) - 1);
  if (len > 0) {
    path[len] = '\0';
    const char *marker = strstr(path, "zoneinfo/");
    if (marker) {
      marker += sizeof("zoneinfo/") - 1;
      size_t zone_len = strlen(marker);
      if (zone_len < capacity) { memcpy(buffer, marker, zone_len + 1); return zone_len; }
    }
  }
#endif
  if (capacity >= 4) { memcpy(buffer, "UTC", 4); return 3; }
  return 0;
}

static bool temporal_now_zone(
  ant_t *js, ant_value_t value, TimeZone *out, ant_value_t *err
) {
  if (vtype(value) != T_UNDEF) return temporal_time_zone_from_value(js, value, out, err);
  char name[256]; size_t len = temporal_system_time_zone(name, sizeof(name));
  temporal_rs_TimeZone_try_from_str_with_provider_result result =
    temporal_rs_TimeZone_try_from_str_with_provider((DiplomatStringView){name, len}, temporal_provider(js));
  if (!result.is_ok) {
    temporal_rs_TimeZone_utc_with_provider_result utc = temporal_rs_TimeZone_utc_with_provider(temporal_provider(js));
    if (!utc.is_ok) { *err = temporal_error(js, utc.err); return false; }
    *out = utc.ok; return true;
  }
  *out = result.ok; return true;
}

static bool temporal_now_zdt(ant_t *js, ant_value_t zone_value, ZonedDateTime **out, ant_value_t *err) {
  TimeZone zone;
  if (!temporal_now_zone(js, zone_value, &zone, err)) return false;
  temporal_rs_ZonedDateTime_try_new_with_provider_result result =
    temporal_rs_ZonedDateTime_try_new_with_provider(
      temporal_now_nanoseconds(), AnyCalendarKind_Iso, zone, temporal_provider(js));
  if (!result.is_ok) { *err = temporal_error(js, result.err); return false; }
  *out = result.ok; return true;
}

static ant_value_t temporal_now_instant(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  temporal_rs_Instant_try_new_result result = temporal_rs_Instant_try_new(temporal_now_nanoseconds());
  return result.is_ok ? temporal_wrap(js, TEMPORAL_INSTANT, result.ok) : temporal_error(js, result.err);
}

static ant_value_t temporal_now_zoned_datetime_iso(ant_t *js, ant_value_t *args, int nargs) {
  ZonedDateTime *value; ant_value_t err = js_mkundef();
  if (!temporal_now_zdt(js, nargs > 0 ? args[0] : js_mkundef(), &value, &err)) return err;
  return temporal_wrap(js, TEMPORAL_ZONED_DATETIME, value);
}

static ant_value_t temporal_now_plain_datetime_iso(ant_t *js, ant_value_t *args, int nargs) {
  ZonedDateTime *zdt; ant_value_t err = js_mkundef();
  if (!temporal_now_zdt(js, nargs > 0 ? args[0] : js_mkundef(), &zdt, &err)) return err;
  PlainDateTime *value = temporal_rs_ZonedDateTime_to_plain_datetime(zdt);
  temporal_rs_ZonedDateTime_destroy(zdt);
  return temporal_wrap(js, TEMPORAL_PLAIN_DATETIME, value);
}

static ant_value_t temporal_now_plain_date_iso(ant_t *js, ant_value_t *args, int nargs) {
  ZonedDateTime *zdt; ant_value_t err = js_mkundef();
  if (!temporal_now_zdt(js, nargs > 0 ? args[0] : js_mkundef(), &zdt, &err)) return err;
  PlainDate *value = temporal_rs_ZonedDateTime_to_plain_date(zdt);
  temporal_rs_ZonedDateTime_destroy(zdt);
  return temporal_wrap(js, TEMPORAL_PLAIN_DATE, value);
}

static ant_value_t temporal_now_plain_time_iso(ant_t *js, ant_value_t *args, int nargs) {
  ZonedDateTime *zdt; ant_value_t err = js_mkundef();
  if (!temporal_now_zdt(js, nargs > 0 ? args[0] : js_mkundef(), &zdt, &err)) return err;
  PlainTime *value = temporal_rs_ZonedDateTime_to_plain_time(zdt);
  temporal_rs_ZonedDateTime_destroy(zdt);
  return temporal_wrap(js, TEMPORAL_PLAIN_TIME, value);
}

static ant_value_t temporal_now_time_zone_id(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs; TimeZone zone; ant_value_t err = js_mkundef();
  if (!temporal_now_zone(js, js_mkundef(), &zone, &err)) return err;
  return temporal_time_zone_identifier(js, zone);
}

void temporal_init_now(ant_t *js, ant_value_t temporal) {
  ant_value_t now = js_mkobj(js); js_set_proto_init(now, js->sym.object_proto);
  TEMPORAL_METHOD(js, now, "instant", temporal_now_instant, 0);
  TEMPORAL_METHOD(js, now, "plainDateISO", temporal_now_plain_date_iso, 0);
  TEMPORAL_METHOD(js, now, "plainDateTimeISO", temporal_now_plain_datetime_iso, 0);
  TEMPORAL_METHOD(js, now, "plainTimeISO", temporal_now_plain_time_iso, 0);
  TEMPORAL_METHOD(js, now, "timeZoneId", temporal_now_time_zone_id, 0);
  TEMPORAL_METHOD(js, now, "zonedDateTimeISO", temporal_now_zoned_datetime_iso, 0);
  temporal_set_to_string_tag(js, now, "Temporal.Now");
  temporal_set_namespace_property(js, temporal, "Now", now);
}


#endif
