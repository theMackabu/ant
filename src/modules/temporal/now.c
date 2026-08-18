#ifdef ANT_HAVE_TEMPORAL
#ifdef _WIN32
#include <windows.h>
#endif
#include "temporal_internal.h"

#ifdef _WIN32
typedef struct {
  const char *windows;
  const char *iana;
} temporal_windows_zone_t;

// CLDR windowsZones.xml territory=001 mappings.
static const temporal_windows_zone_t temporal_windows_zones[] = {
  {"Dateline Standard Time", "Etc/GMT+12"},
  {"UTC-11", "Etc/GMT+11"},
  {"Aleutian Standard Time", "America/Adak"},
  {"Hawaiian Standard Time", "Pacific/Honolulu"},
  {"Marquesas Standard Time", "Pacific/Marquesas"},
  {"Alaskan Standard Time", "America/Anchorage"},
  {"UTC-09", "Etc/GMT+9"},
  {"Pacific Standard Time (Mexico)", "America/Tijuana"},
  {"UTC-08", "Etc/GMT+8"},
  {"Pacific Standard Time", "America/Los_Angeles"},
  {"US Mountain Standard Time", "America/Phoenix"},
  {"Mountain Standard Time (Mexico)", "America/Mazatlan"},
  {"Mountain Standard Time", "America/Denver"},
  {"Yukon Standard Time", "America/Whitehorse"},
  {"Central America Standard Time", "America/Guatemala"},
  {"Central Standard Time", "America/Chicago"},
  {"Easter Island Standard Time", "Pacific/Easter"},
  {"Central Standard Time (Mexico)", "America/Mexico_City"},
  {"Canada Central Standard Time", "America/Regina"},
  {"SA Pacific Standard Time", "America/Bogota"},
  {"Eastern Standard Time (Mexico)", "America/Cancun"},
  {"Eastern Standard Time", "America/New_York"},
  {"Haiti Standard Time", "America/Port-au-Prince"},
  {"Cuba Standard Time", "America/Havana"},
  {"US Eastern Standard Time", "America/Indianapolis"},
  {"Turks And Caicos Standard Time", "America/Grand_Turk"},
  {"Paraguay Standard Time", "America/Asuncion"},
  {"Atlantic Standard Time", "America/Halifax"},
  {"Venezuela Standard Time", "America/Caracas"},
  {"Central Brazilian Standard Time", "America/Cuiaba"},
  {"SA Western Standard Time", "America/La_Paz"},
  {"Pacific SA Standard Time", "America/Santiago"},
  {"Newfoundland Standard Time", "America/St_Johns"},
  {"Tocantins Standard Time", "America/Araguaina"},
  {"E. South America Standard Time", "America/Sao_Paulo"},
  {"SA Eastern Standard Time", "America/Cayenne"},
  {"Argentina Standard Time", "America/Buenos_Aires"},
  {"Greenland Standard Time", "America/Godthab"},
  {"Montevideo Standard Time", "America/Montevideo"},
  {"Magallanes Standard Time", "America/Punta_Arenas"},
  {"Saint Pierre Standard Time", "America/Miquelon"},
  {"Bahia Standard Time", "America/Bahia"},
  {"UTC-02", "Etc/GMT+2"},
  {"Azores Standard Time", "Atlantic/Azores"},
  {"Cape Verde Standard Time", "Atlantic/Cape_Verde"},
  {"UTC", "Etc/UTC"},
  {"GMT Standard Time", "Europe/London"},
  {"Greenwich Standard Time", "Atlantic/Reykjavik"},
  {"Sao Tome Standard Time", "Africa/Sao_Tome"},
  {"Morocco Standard Time", "Africa/Casablanca"},
  {"W. Europe Standard Time", "Europe/Berlin"},
  {"Central Europe Standard Time", "Europe/Budapest"},
  {"Romance Standard Time", "Europe/Paris"},
  {"Central European Standard Time", "Europe/Warsaw"},
  {"W. Central Africa Standard Time", "Africa/Lagos"},
  {"Jordan Standard Time", "Asia/Amman"},
  {"GTB Standard Time", "Europe/Bucharest"},
  {"Middle East Standard Time", "Asia/Beirut"},
  {"Egypt Standard Time", "Africa/Cairo"},
  {"E. Europe Standard Time", "Europe/Chisinau"},
  {"Syria Standard Time", "Asia/Damascus"},
  {"West Bank Standard Time", "Asia/Hebron"},
  {"South Africa Standard Time", "Africa/Johannesburg"},
  {"FLE Standard Time", "Europe/Kiev"},
  {"Israel Standard Time", "Asia/Jerusalem"},
  {"South Sudan Standard Time", "Africa/Juba"},
  {"Kaliningrad Standard Time", "Europe/Kaliningrad"},
  {"Sudan Standard Time", "Africa/Khartoum"},
  {"Libya Standard Time", "Africa/Tripoli"},
  {"Namibia Standard Time", "Africa/Windhoek"},
  {"Arabic Standard Time", "Asia/Baghdad"},
  {"Turkey Standard Time", "Europe/Istanbul"},
  {"Arab Standard Time", "Asia/Riyadh"},
  {"Belarus Standard Time", "Europe/Minsk"},
  {"Russian Standard Time", "Europe/Moscow"},
  {"E. Africa Standard Time", "Africa/Nairobi"},
  {"Iran Standard Time", "Asia/Tehran"},
  {"Arabian Standard Time", "Asia/Dubai"},
  {"Astrakhan Standard Time", "Europe/Astrakhan"},
  {"Azerbaijan Standard Time", "Asia/Baku"},
  {"Russia Time Zone 3", "Europe/Samara"},
  {"Mauritius Standard Time", "Indian/Mauritius"},
  {"Saratov Standard Time", "Europe/Saratov"},
  {"Georgian Standard Time", "Asia/Tbilisi"},
  {"Volgograd Standard Time", "Europe/Volgograd"},
  {"Caucasus Standard Time", "Asia/Yerevan"},
  {"Afghanistan Standard Time", "Asia/Kabul"},
  {"West Asia Standard Time", "Asia/Tashkent"},
  {"Ekaterinburg Standard Time", "Asia/Yekaterinburg"},
  {"Pakistan Standard Time", "Asia/Karachi"},
  {"Qyzylorda Standard Time", "Asia/Qyzylorda"},
  {"India Standard Time", "Asia/Calcutta"},
  {"Sri Lanka Standard Time", "Asia/Colombo"},
  {"Nepal Standard Time", "Asia/Katmandu"},
  {"Central Asia Standard Time", "Asia/Bishkek"},
  {"Bangladesh Standard Time", "Asia/Dhaka"},
  {"Omsk Standard Time", "Asia/Omsk"},
  {"Myanmar Standard Time", "Asia/Rangoon"},
  {"SE Asia Standard Time", "Asia/Bangkok"},
  {"Altai Standard Time", "Asia/Barnaul"},
  {"W. Mongolia Standard Time", "Asia/Hovd"},
  {"North Asia Standard Time", "Asia/Krasnoyarsk"},
  {"N. Central Asia Standard Time", "Asia/Novosibirsk"},
  {"Tomsk Standard Time", "Asia/Tomsk"},
  {"China Standard Time", "Asia/Shanghai"},
  {"North Asia East Standard Time", "Asia/Irkutsk"},
  {"Singapore Standard Time", "Asia/Singapore"},
  {"W. Australia Standard Time", "Australia/Perth"},
  {"Taipei Standard Time", "Asia/Taipei"},
  {"Ulaanbaatar Standard Time", "Asia/Ulaanbaatar"},
  {"Aus Central W. Standard Time", "Australia/Eucla"},
  {"Transbaikal Standard Time", "Asia/Chita"},
  {"Tokyo Standard Time", "Asia/Tokyo"},
  {"North Korea Standard Time", "Asia/Pyongyang"},
  {"Korea Standard Time", "Asia/Seoul"},
  {"Yakutsk Standard Time", "Asia/Yakutsk"},
  {"Cen. Australia Standard Time", "Australia/Adelaide"},
  {"AUS Central Standard Time", "Australia/Darwin"},
  {"E. Australia Standard Time", "Australia/Brisbane"},
  {"AUS Eastern Standard Time", "Australia/Sydney"},
  {"West Pacific Standard Time", "Pacific/Port_Moresby"},
  {"Tasmania Standard Time", "Australia/Hobart"},
  {"Vladivostok Standard Time", "Asia/Vladivostok"},
  {"Lord Howe Standard Time", "Australia/Lord_Howe"},
  {"Bougainville Standard Time", "Pacific/Bougainville"},
  {"Russia Time Zone 10", "Asia/Srednekolymsk"},
  {"Magadan Standard Time", "Asia/Magadan"},
  {"Norfolk Standard Time", "Pacific/Norfolk"},
  {"Sakhalin Standard Time", "Asia/Sakhalin"},
  {"Central Pacific Standard Time", "Pacific/Guadalcanal"},
  {"Russia Time Zone 11", "Asia/Kamchatka"},
  {"New Zealand Standard Time", "Pacific/Auckland"},
  {"UTC+12", "Etc/GMT-12"},
  {"Fiji Standard Time", "Pacific/Fiji"},
  {"Chatham Islands Standard Time", "Pacific/Chatham"},
  {"UTC+13", "Etc/GMT-13"},
  {"Tonga Standard Time", "Pacific/Tongatapu"},
  {"Samoa Standard Time", "Pacific/Apia"},
  {"Line Islands Standard Time", "Pacific/Kiritimati"},
};

static const char *temporal_windows_time_zone(void) {
  DYNAMIC_TIME_ZONE_INFORMATION info = {0};
  if (GetDynamicTimeZoneInformation(&info) == TIME_ZONE_ID_INVALID || !info.TimeZoneKeyName[0])
    return NULL;
  char key[128];
  if (!WideCharToMultiByte(
    CP_UTF8, 0, info.TimeZoneKeyName, -1, key, sizeof(key), NULL, NULL
  )) return NULL;
  for (size_t i = 0; i < sizeof(temporal_windows_zones) / sizeof(temporal_windows_zones[0]); i++)
    if (strcmp(key, temporal_windows_zones[i].windows) == 0) return temporal_windows_zones[i].iana;
  return NULL;
}
#endif

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

size_t temporal_system_time_zone(char *buffer, size_t capacity) {
  const char *tz = getenv("TZ");
  if (tz && *tz) {
    if (*tz == ':') tz++;
    size_t len = strlen(tz);
    if (len < capacity) { memcpy(buffer, tz, len + 1); return len; }
  }
#ifdef _WIN32
  const char *windows_zone = temporal_windows_time_zone();
  if (windows_zone) {
    size_t len = strlen(windows_zone);
    if (len < capacity) { memcpy(buffer, windows_zone, len + 1); return len; }
  }
#else
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
