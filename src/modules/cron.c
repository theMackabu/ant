#include <compat.h> // IWYU pragma: keep

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uv.h>

#ifndef _WIN32
#include <fcntl.h>
#include <libgen.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#else
#define COBJMACROS
#include <oleauto.h>
#include <sddl.h>
#include <taskschd.h>
#include <windows.h>
#endif

#include "ant.h"
#include "errors.h"
#include "internal.h"
#include "ptr.h"
#include "reactor.h"
#include "utils.h"
#include "gc/roots.h"
#include "modules/assert.h"
#include "modules/cron.h"
#include "modules/date.h"
#include "modules/process.h"
#include "modules/symbol.h"
#include "silver/engine.h"

#ifdef ANT_HAVE_TEMPORAL
#include "temporal/temporal_internal.h"
#endif

typedef struct {
  uint64_t minutes;
  uint32_t hours;
  uint32_t days;
  uint16_t months;
  uint8_t weekdays;
  bool minute_wildcard;
  bool hour_wildcard;
  bool day_wildcard;
  bool weekday_wildcard;
} cron_expression_t;

typedef struct {
  int year;
  int month;
  int day;
  int hour;
  int minute;
  int weekday;
} cron_fields_t;

typedef struct {
#ifdef ANT_HAVE_TEMPORAL
  TimeZone value;
#else
  bool utc;
#endif
} cron_zone_t;

#define CRON_DATE_LIMIT_MS INT64_C(8640000000000000)

typedef struct cron_job {
  uv_timer_t timer;
  ant_t *js;
  ant_value_t object;
  ant_value_t handler;
  cron_expression_t expression;
  char *source;
  char *time_zone;
  bool stopped;
  bool closing;
  bool timer_closed;
  bool finalized;
  bool listed;
  bool busy;
  struct cron_job *next;
  struct cron_job *prev;
} cron_job_t;

typedef struct cron_os_request {
  uv_work_t work;
  ant_t *js;
  ant_value_t promise;
  char *path;
  char *schedule;
  char *title;
  char *executable;
  char error[512];
  int rc;
  bool remove;
  struct cron_os_request *queue_next;
  struct cron_os_request *next;
  struct cron_os_request *prev;
} cron_os_request_t;

static struct {
  cron_job_t *jobs;
  cron_os_request_t *requests;
  cron_os_request_t *request_queue_head;
  cron_os_request_t *request_queue_tail;
  cron_os_request_t *active_request;
} cron_state = {
  .jobs = NULL,
  .requests = NULL,
  .request_queue_head = NULL,
  .request_queue_tail = NULL,
  .active_request = NULL,
};

enum { CRON_JOB_NATIVE_TAG = 0x43524f4eu }; // CRON

static const char *const month_names[] = {
  "january", "february", "march", "april", "may", "june",
  "july", "august", "september", "october", "november", "december",
};

static const char *const weekday_names[] = {
  "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday",
};

static int cron_ascii_equal(const char *value, size_t len, const char *name) {
  size_t name_len = strlen(name);
  if (len != name_len) return 0;
  for (size_t i = 0; i < len; i++) if (
    tolower((unsigned char)value[i]) != tolower((unsigned char)name[i])
  ) return 0;
  return 1;
}

static int cron_parse_named_value(
  const char *text, size_t len, const char *const *names, int count
) {
  for (int i = 0; i < count; i++) {
    size_t full_len = strlen(names[i]);
    if (len == full_len && cron_ascii_equal(text, len, names[i])) return i;
    if (len == 3) {
      size_t j = 0;
      for (; j < 3; j++) if (
        tolower((unsigned char)text[j]) != tolower((unsigned char)names[i][j])
      ) break;
      if (j == 3) return i;
    }
  }
  return -1;
}

static int cron_parse_value(
  const char *text,
  size_t len,
  int minimum,
  int maximum,
  int name_kind,
  int *out
) {
  if (len == 0) return -1;
  if (text[0] == '+') {
    text++;
    len--;
    if (len == 0) return -1;
  }
  if (name_kind != 0 && isalpha((unsigned char)text[0])) {
    int value = name_kind == 1
      ? cron_parse_named_value(text, len, month_names, 12) + 1
      : cron_parse_named_value(text, len, weekday_names, 7);
    if ((name_kind == 1 && value == 0) || (name_kind == 2 && value < 0)) return -1;
    *out = value;
    return 0;
  }

  int value = 0;
  for (size_t i = 0; i < len; i++) {
    if (!isdigit((unsigned char)text[i])) return -1;
    if (value > (maximum - (text[i] - '0')) / 10) return -1;
    value = value * 10 + (text[i] - '0');
  }
  if (value < minimum || value > maximum) return -1;
  *out = value;
  return 0;
}

static void cron_set_bit(uint64_t *bits, int value, int weekday) {
  if (weekday && value == 7) value = 0;
  *bits |= UINT64_C(1) << value;
}

static int cron_parse_field(
  const char *text,
  size_t len,
  int minimum,
  int maximum,
  int name_kind,
  uint64_t *bits,
  bool *wildcard,
  char *error,
  size_t error_len
) {
  *bits = 0;
  if (wildcard) *wildcard = len == 1 && text[0] == '*';

  size_t item_start = 0;
  while (item_start < len) {
    size_t item_end = item_start;
    while (item_end < len && text[item_end] != ',') item_end++;
    if (item_end == item_start) goto invalid;

    size_t slash = item_start;
    while (slash < item_end && text[slash] != '/') slash++;
    int step = 1;
    if (slash < item_end) {
      if (slash + 1 == item_end || cron_parse_value(
        text + slash + 1, item_end - slash - 1, 1, 127, 0, &step
      ) != 0) goto invalid;
    }

    size_t base_end = slash < item_end ? slash : item_end;
    int first = minimum;
    int last = maximum;
    if (!(base_end == item_start + 1 && text[item_start] == '*')) {
      size_t dash = item_start;
      while (dash < base_end && text[dash] != '-') dash++;
      if (dash < base_end) {
        if (cron_parse_value(
          text + item_start, dash - item_start, minimum, maximum, name_kind, &first
        ) != 0 || cron_parse_value(
          text + dash + 1, base_end - dash - 1, minimum, maximum, name_kind, &last
        ) != 0 || first > last) goto invalid;
      } else {
        if (cron_parse_value(
          text + item_start, base_end - item_start, minimum, maximum, name_kind, &first
        ) != 0) goto invalid;
        last = slash < item_end ? maximum : first;
      }
    }

    for (int value = first; value <= last;) {
      cron_set_bit(bits, value, name_kind == 2);
      if (step > last - value) break;
      value += step;
    }

    if (item_end < len && item_end + 1 == len) goto invalid;
    item_start = item_end + 1;
  }

  if (*bits == 0) goto invalid;
  return 0;

invalid:
  snprintf(error, error_len, "invalid cron field '%.*s'", (int)len, text);
  return -1;
}

static const char *cron_expand_nickname(const char *source, size_t len) {
  if (cron_ascii_equal(source, len, "@yearly") || cron_ascii_equal(source, len, "@annually"))
    return "0 0 1 1 *";
  if (cron_ascii_equal(source, len, "@monthly")) return "0 0 1 * *";
  if (cron_ascii_equal(source, len, "@weekly")) return "0 0 * * 0";
  if (cron_ascii_equal(source, len, "@daily") || cron_ascii_equal(source, len, "@midnight"))
    return "0 0 * * *";
  if (cron_ascii_equal(source, len, "@hourly")) return "0 * * * *";
  return NULL;
}

static int cron_parse_expression(
  const char *source,
  size_t source_len,
  cron_expression_t *out,
  char *error,
  size_t error_len
) {
  while (source_len > 0 && isspace((unsigned char)*source)) {
    source++;
    source_len--;
  }
  while (source_len > 0 && isspace((unsigned char)source[source_len - 1])) source_len--;
  const char *nickname = cron_expand_nickname(source, source_len);
  if (nickname) {
    source = nickname;
    source_len = strlen(nickname);
  }

  const char *fields[5] = {0};
  size_t lengths[5] = {0};
  size_t cursor = 0;
  int count = 0;
  while (cursor < source_len) {
    while (cursor < source_len && isspace((unsigned char)source[cursor])) cursor++;
    if (cursor == source_len) break;
    if (count == 5) goto field_count;
    size_t start = cursor;
    while (cursor < source_len && !isspace((unsigned char)source[cursor])) cursor++;
    fields[count] = source + start;
    lengths[count] = cursor - start;
    count++;
  }
  if (count != 5) goto field_count;

  memset(out, 0, sizeof(*out));
  uint64_t bits = 0;
  if (cron_parse_field(
    fields[0], lengths[0], 0, 59, 0, &out->minutes, &out->minute_wildcard,
    error, error_len
  ) != 0)
    return -1;
  out->minute_wildcard = out->minutes == ((UINT64_C(1) << 60) - 1);
  if (cron_parse_field(
    fields[1], lengths[1], 0, 23, 0, &bits, &out->hour_wildcard,
    error, error_len
  ) != 0)
    return -1;
  out->hours = (uint32_t)bits;
  out->hour_wildcard = out->hours == ((UINT32_C(1) << 24) - 1);
  if (cron_parse_field(fields[2], lengths[2], 1, 31, 0, &bits, &out->day_wildcard, error, error_len) != 0)
    return -1;
  out->days = (uint32_t)bits;
  if (cron_parse_field(fields[3], lengths[3], 1, 12, 1, &bits, NULL, error, error_len) != 0)
    return -1;
  out->months = (uint16_t)bits;
  if (cron_parse_field(fields[4], lengths[4], 0, 7, 2, &bits, &out->weekday_wildcard, error, error_len) != 0)
    return -1;
  out->weekdays = (uint8_t)bits;
  return 0;

field_count:
  snprintf(error, error_len, "expected 5 space-separated fields (minute hour day month weekday)");
  return -1;
}

static int cron_is_leap_year(int year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int cron_days_in_month(int year, int month) {
  static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && cron_is_leap_year(year) ? 29 : days[month - 1];
}

static bool cron_matches(const cron_expression_t *expression, const cron_fields_t *fields) {
  if ((expression->minutes & (UINT64_C(1) << fields->minute)) == 0 ||
      (expression->hours & (UINT32_C(1) << fields->hour)) == 0 ||
      (expression->months & (UINT16_C(1) << fields->month)) == 0) return false;

  bool day_match = (expression->days & (UINT32_C(1) << fields->day)) != 0;
  bool weekday_match = (expression->weekdays & (UINT8_C(1) << fields->weekday)) != 0;
  if (!expression->day_wildcard && !expression->weekday_wildcard)
    return day_match || weekday_match;
  if (!expression->day_wildcard) return day_match;
  if (!expression->weekday_wildcard) return weekday_match;
  return true;
}

static bool cron_date_matches(const cron_expression_t *expression, const cron_fields_t *fields) {
  if ((expression->months & (UINT16_C(1) << fields->month)) == 0) return false;
  bool day_match = (expression->days & (UINT32_C(1) << fields->day)) != 0;
  bool weekday_match = (expression->weekdays & (UINT8_C(1) << fields->weekday)) != 0;
  if (!expression->day_wildcard && !expression->weekday_wildcard)
    return day_match || weekday_match;
  if (!expression->day_wildcard) return day_match;
  if (!expression->weekday_wildcard) return weekday_match;
  return true;
}

static void cron_advance_day(cron_fields_t *fields) {
  fields->day++;
  fields->weekday = (fields->weekday + 1) % 7;
  if (fields->day <= cron_days_in_month(fields->year, fields->month)) return;
  fields->day = 1;
  fields->month++;
  if (fields->month <= 12) return;
  fields->month = 1;
  fields->year++;
}

static int64_t cron_now_ms(void) {
  uv_timeval64_t now = {0};
  uv_gettimeofday(&now);
  return now.tv_sec * INT64_C(1000) + now.tv_usec / 1000;
}

#ifdef ANT_HAVE_TEMPORAL
static bool cron_resolve_zone(
  ant_t *js,
  ant_value_t options,
  cron_zone_t *out,
  char **name_out,
  ant_value_t *error
) {
  ant_value_t zone_value = js_mkundef();
  char system_zone[256];
  if (vtype(options) != kTypeUndefined && vtype(options) != kTypeNull) {
    if (!is_object_type(options)) {
      *error = js_mkerr_typed(js, JS_ERR_TYPE, "Ant.cron options must be an object");
      return false;
    }
    zone_value = js_get(js, options, "tz");
    if (js->thrown_exists) {
      *error = js_throw(js, js_take_thrown(js, zone_value));
      return false;
    }
  }
  if (vtype(zone_value) == kTypeUndefined || vtype(zone_value) == kTypeNull) {
    size_t len = temporal_system_time_zone(system_zone, sizeof(system_zone));
    zone_value = js_mkstr(js, system_zone, len);
  } else if (vtype(zone_value) != kTypeString) {
    *error = js_mkerr_typed(js, JS_ERR_TYPE, "Ant.cron options.tz must be a string");
    return false;
  }

  size_t zone_len = 0;
  const char *zone_name = js_getstr(js, zone_value, &zone_len);
  if ((zone_len > 0 && (zone_name[0] == '+' || zone_name[0] == '-')) ||
      !temporal_time_zone_from_value(js, zone_value, &out->value, error)) {
    *error = js_mkerr_typed(
      js, JS_ERR_TYPE, "Ant.cron: unknown time zone '%.*s'", (int)zone_len, zone_name
    );
    return false;
  }
  if (name_out) {
    size_t len = 0;
    const char *name = js_getstr(js, zone_value, &len);
    *name_out = strndup(name, len);
    if (!*name_out) {
      *error = js_mkerr(js, "out of memory");
      return false;
    }
  }
  return true;
}

static bool cron_fields_from_epoch(
  ant_t *js,
  int64_t epoch_ms,
  cron_zone_t zone,
  cron_fields_t *out,
  ant_value_t *error
) {
  temporal_rs_ZonedDateTime_from_epoch_milliseconds_with_provider_result result =
    temporal_rs_ZonedDateTime_from_epoch_milliseconds_with_provider(
      epoch_ms, zone.value, temporal_provider(js)
    );
  if (!result.is_ok) {
    *error = temporal_error(js, result.err);
    return false;
  }
  ZonedDateTime *value = result.ok;
  out->year = temporal_rs_ZonedDateTime_year(value);
  out->month = temporal_rs_ZonedDateTime_month(value);
  out->day = temporal_rs_ZonedDateTime_day(value);
  out->hour = temporal_rs_ZonedDateTime_hour(value);
  out->minute = temporal_rs_ZonedDateTime_minute(value);
  out->weekday = temporal_rs_ZonedDateTime_day_of_week(value) % 7;
  temporal_rs_ZonedDateTime_destroy(value);
  return true;
}

static bool cron_epoch_from_fields(
  ant_t *js,
  const cron_fields_t *fields,
  cron_zone_t zone,
  int64_t *out,
  ant_value_t *error
) {
  temporal_rs_PlainDateTime_try_new_result plain = temporal_rs_PlainDateTime_try_new(
    fields->year, (uint8_t)fields->month, (uint8_t)fields->day,
    (uint8_t)fields->hour, (uint8_t)fields->minute, 0, 0, 0, 0,
    AnyCalendarKind_Iso
  );
  if (!plain.is_ok) {
    *error = temporal_error(js, plain.err);
    return false;
  }
  temporal_rs_PlainDateTime_to_zoned_date_time_with_provider_result zoned =
    temporal_rs_PlainDateTime_to_zoned_date_time_with_provider(
      plain.ok, zone.value, Disambiguation_Compatible, temporal_provider(js)
    );
  temporal_rs_PlainDateTime_destroy(plain.ok);
  if (!zoned.is_ok) {
    *error = temporal_error(js, zoned.err);
    return false;
  }
  *out = temporal_rs_ZonedDateTime_epoch_milliseconds(zoned.ok);
  temporal_rs_ZonedDateTime_destroy(zoned.ok);
  return true;
}

static bool cron_offset_from_epoch(
  ant_t *js, int64_t epoch_ms, cron_zone_t zone, int64_t *out, ant_value_t *error
) {
  temporal_rs_ZonedDateTime_from_epoch_milliseconds_with_provider_result result =
    temporal_rs_ZonedDateTime_from_epoch_milliseconds_with_provider(
      epoch_ms, zone.value, temporal_provider(js)
    );
  if (!result.is_ok) {
    *error = temporal_error(js, result.err);
    return false;
  }
  *out = temporal_rs_ZonedDateTime_offset_nanoseconds(result.ok);
  temporal_rs_ZonedDateTime_destroy(result.ok);
  return true;
}
#else
static bool cron_resolve_zone(
  ant_t *js,
  ant_value_t options,
  cron_zone_t *out,
  char **name_out,
  ant_value_t *error
) {
  out->utc = false;
  if (vtype(options) != kTypeUndefined && vtype(options) != kTypeNull) {
    if (!is_object_type(options)) {
      *error = js_mkerr_typed(js, JS_ERR_TYPE, "Ant.cron options must be an object");
      return false;
    }
    ant_value_t tz = js_get(js, options, "tz");
    if (js->thrown_exists) {
      *error = js_throw(js, js_take_thrown(js, tz));
      return false;
    }
    if (vtype(tz) != kTypeUndefined && vtype(tz) != kTypeNull) {
      if (vtype(tz) != kTypeString) {
        *error = js_mkerr_typed(js, JS_ERR_TYPE, "Ant.cron options.tz must be a string");
        return false;
      }
      size_t len = 0;
      const char *name = js_getstr(js, tz, &len);
      if (!cron_ascii_equal(name, len, "UTC")) {
        *error = js_mkerr_typed(js, JS_ERR_TYPE, "Ant.cron: IANA time zones require Temporal support");
        return false;
      }
      out->utc = true;
    }
  }
  if (name_out) *name_out = strdup(out->utc ? "UTC" : "local");
  return true;
}

static bool cron_fields_from_epoch(
  ant_t *js,
  int64_t epoch_ms,
  cron_zone_t zone,
  cron_fields_t *out,
  ant_value_t *error
) {
  time_t seconds = (time_t)(epoch_ms / 1000);
  struct tm value = {0};
#ifdef _WIN32
  errno_t result = zone.utc
    ? gmtime_s(&value, &seconds)
    : localtime_s(&value, &seconds);
  if (result != 0) {
    *error = js_mkerr(js, "failed to convert cron time");
    return false;
  }
#else
  struct tm *result = zone.utc
    ? gmtime_r(&seconds, &value)
    : localtime_r(&seconds, &value);
  if (!result) {
    *error = js_mkerr(js, "failed to convert cron time");
    return false;
  }
#endif
  out->year = value.tm_year + 1900;
  out->month = value.tm_mon + 1;
  out->day = value.tm_mday;
  out->hour = value.tm_hour;
  out->minute = value.tm_min;
  out->weekday = value.tm_wday;
  return true;
}

static bool cron_epoch_from_fields(
  ant_t *js,
  const cron_fields_t *fields,
  cron_zone_t zone,
  int64_t *out,
  ant_value_t *error
) {
  (void)js;
  (void)error;
  struct tm value = {
    .tm_year = fields->year - 1900,
    .tm_mon = fields->month - 1,
    .tm_mday = fields->day,
    .tm_hour = fields->hour,
    .tm_min = fields->minute,
    .tm_isdst = -1,
  };
  time_t seconds = zone.utc ? timegm(&value) : mktime(&value);
  *out = (int64_t)seconds * INT64_C(1000);
  return true;
}

#endif

static bool cron_next_epoch(
  ant_t *js,
  const cron_expression_t *expression,
  cron_zone_t zone,
  int64_t relative_ms,
  int64_t *next_ms,
  bool *found,
  ant_value_t *error
) {
  *found = false;
  int64_t minute = relative_ms >= 0
    ? relative_ms / 60000
    : -((-relative_ms + 59999) / 60000);
  const int64_t first_candidate = (minute + 1) * INT64_C(60000);
  const int64_t direct_horizon = INT64_C(26) * 60 * 60000;
  const int64_t search_horizon = INT64_C(8) * 366 * 24 * 60 * 60000;
  const int64_t direct_end = first_candidate > CRON_DATE_LIMIT_MS - direct_horizon
    ? CRON_DATE_LIMIT_MS
    : first_candidate + direct_horizon;
  const int64_t search_end = relative_ms > CRON_DATE_LIMIT_MS - search_horizon
    ? CRON_DATE_LIMIT_MS
    : relative_ms + search_horizon;
  if (first_candidate > search_end) {
    return true;
  }

  cron_fields_t relative_fields;
  if (!cron_fields_from_epoch(js, relative_ms, zone, &relative_fields, error)) return false;
  cron_fields_t end_fields;
  if (!cron_fields_from_epoch(js, search_end, zone, &end_fields, error)) return false;

  cron_fields_t fields = relative_fields;
  fields.hour = 0;
  fields.minute = 0;
  int64_t civil_match = 0;
  bool civil_found = false;

  while (fields.year < end_fields.year ||
         (fields.year == end_fields.year && fields.month < end_fields.month) ||
         (fields.year == end_fields.year && fields.month == end_fields.month &&
          fields.day <= end_fields.day)) {
    bool relative_date = fields.year == relative_fields.year &&
      fields.month == relative_fields.month && fields.day == relative_fields.day;
    bool scan_full_relative_day = false;
#ifdef ANT_HAVE_TEMPORAL
    if (relative_date) {
      cron_fields_t next_day = fields;
      cron_advance_day(&next_day);
      int64_t day_start = 0, day_end = search_end;
      int64_t start_offset = 0, end_offset = 0;
      bool has_full_day = fields.year < end_fields.year || fields.month < end_fields.month ||
        fields.day < end_fields.day;
      if (!cron_epoch_from_fields(js, &fields, zone, &day_start, error) ||
          (has_full_day && !cron_epoch_from_fields(js, &next_day, zone, &day_end, error)) ||
          !cron_offset_from_epoch(js, day_start, zone, &start_offset, error) ||
          !cron_offset_from_epoch(js, day_end, zone, &end_offset, error)) return false;
      scan_full_relative_day = start_offset != end_offset;
    }
#endif
    bool saw_shifted_match = false;
    if (cron_date_matches(expression, &fields)) {
      int first_hour = relative_date && !scan_full_relative_day ? relative_fields.hour : 0;
      for (int hour = first_hour; hour < 24; hour++) {
        if ((expression->hours & (UINT32_C(1) << hour)) == 0) continue;
        int first_minute = relative_date && !scan_full_relative_day &&
          hour == relative_fields.hour ? relative_fields.minute : 0;
        for (int minute_value = first_minute; minute_value < 60; minute_value++) {
          if ((expression->minutes & (UINT64_C(1) << minute_value)) == 0) continue;
          cron_fields_t candidate_fields = fields;
          candidate_fields.hour = hour;
          candidate_fields.minute = minute_value;
          if (candidate_fields.year == end_fields.year &&
              candidate_fields.month == end_fields.month &&
              candidate_fields.day == end_fields.day &&
              (candidate_fields.hour > end_fields.hour ||
               (candidate_fields.hour == end_fields.hour &&
                candidate_fields.minute > end_fields.minute))) goto civil_done;
          int64_t epoch_ms = 0;
          if (!cron_epoch_from_fields(js, &candidate_fields, zone, &epoch_ms, error)) return false;
          cron_fields_t roundtrip;
          if (!cron_fields_from_epoch(js, epoch_ms, zone, &roundtrip, error)) return false;
          bool shifted = roundtrip.year != candidate_fields.year ||
            roundtrip.month != candidate_fields.month || roundtrip.day != candidate_fields.day ||
            roundtrip.hour != candidate_fields.hour || roundtrip.minute != candidate_fields.minute;
          if (shifted) {
            if (saw_shifted_match) continue;
            saw_shifted_match = true;
          }
          if (epoch_ms > relative_ms && epoch_ms <= search_end) {
            civil_match = epoch_ms;
            civil_found = true;
            goto civil_done;
          }
        }
      }
    }
    cron_advance_day(&fields);
  }

civil_done:
  int64_t direct_match = 0;
  bool direct_found = false;
  bool scan_absolute = true;
#ifdef ANT_HAVE_TEMPORAL
  int64_t relative_minute = relative_ms >= 0
    ? (relative_ms / 60000) * INT64_C(60000)
    : -(((-relative_ms + 59999) / 60000) * INT64_C(60000));
  int64_t compatible_relative = 0;
  int64_t start_offset = 0;
  int64_t end_offset = 0;
  if (!cron_epoch_from_fields(js, &relative_fields, zone, &compatible_relative, error)) return false;
  if (!cron_offset_from_epoch(js, relative_minute, zone, &start_offset, error) ||
      !cron_offset_from_epoch(js, direct_end, zone, &end_offset, error)) return false;
  scan_absolute = compatible_relative != relative_minute || start_offset != end_offset;
#endif
  if (scan_absolute) for (int64_t candidate = first_candidate;
       candidate <= direct_end && candidate <= search_end;
       candidate += 60000) {
    cron_fields_t candidate_fields;
    if (!cron_fields_from_epoch(js, candidate, zone, &candidate_fields, error)) return false;
    if (!cron_matches(expression, &candidate_fields)) continue;

    bool repeated_fixed_time = false;
    if (!expression->minute_wildcard && !expression->hour_wildcard) {
      int64_t compatible = 0;
      if (!cron_epoch_from_fields(js, &candidate_fields, zone, &compatible, error)) return false;
      repeated_fixed_time = compatible != candidate;
    }
    if (!repeated_fixed_time) {
      direct_match = candidate;
      direct_found = true;
      break;
    }
  }

  if (direct_found && (!civil_found || direct_match < civil_match)) *next_ms = direct_match;
  else if (civil_found) *next_ms = civil_match;
  else return true;
  *found = true;
  return true;
}

static bool cron_relative_ms(
  ant_t *js,
  ant_value_t value,
  int64_t *out,
  ant_value_t *error
) {
  if (vtype(value) == kTypeUndefined || vtype(value) == kTypeNull) {
    *out = cron_now_ms();
    return true;
  }
  ant_value_t number = value;
  if (is_date_instance(value)) number = js_get_slot(value, SLOT_DATA);
  if (vtype(number) != kTypeNumber || !isfinite(js_getnum(number))) {
    *error = js_mkerr_typed(js, JS_ERR_TYPE, "Ant.cron.parse() relativeDate must be a Date or number");
    return false;
  }
  double milliseconds = js_getnum(number);
  if (milliseconds < -(double)CRON_DATE_LIMIT_MS || milliseconds > (double)CRON_DATE_LIMIT_MS) {
    *error = js_mkerr_typed(js, JS_ERR_TYPE, "Invalid date value");
    return false;
  }
  *out = (int64_t)milliseconds;
  return true;
}

static ant_value_t cron_make_date(ant_t *js, int64_t epoch_ms) {
  ant_value_t object = js_mkobj(js);
  ant_value_t prototype = js_get_slot(js->builtins.cron_proto, SLOT_DATA);
  if (is_object_type(prototype)) js_set_proto_init(object, prototype);
  js_set_slot(object, SLOT_DATA, js_mknum((double)epoch_ms));
  js_set_slot(object, SLOT_BRAND, js_mknum(BRAND_DATE));
  return object;
}

static void cron_add_job(cron_job_t *job) {
  job->prev = NULL;
  job->next = cron_state.jobs;
  if (job->next) job->next->prev = job;
  cron_state.jobs = job;
  job->listed = true;
}

static void cron_remove_job(cron_job_t *job) {
  if (!job || !job->listed) return;
  if (job->prev) job->prev->next = job->next;
  else cron_state.jobs = job->next;
  if (job->next) job->next->prev = job->prev;
  job->next = NULL;
  job->prev = NULL;
  job->listed = false;
}

static void cron_free_job(cron_job_t *job) {
  free(job->source);
  free(job->time_zone);
  free(job);
}

static void cron_close_callback(uv_handle_t *handle) {
  cron_job_t *job = handle->data;
  if (!job) return;
  job->timer_closed = true;
  if (job->finalized) cron_free_job(job);
}

static void cron_close_job(cron_job_t *job) {
  if (!job) return;
  job->stopped = true;
  job->busy = false;
  job->handler = js_mkundef();
  cron_remove_job(job);
  uv_timer_stop(&job->timer);
  if (!job->closing && !uv_is_closing((uv_handle_t *)&job->timer)) {
    job->closing = true;
    uv_close((uv_handle_t *)&job->timer, cron_close_callback);
  }
}

static void cron_object_finalize(ant_t *js, ant_object_t *object) {
  ant_value_t value = js_obj_from_ptr(object);
  cron_job_t *job = js_get_native(value, CRON_JOB_NATIVE_TAG);
  if (!job) return;
  js_clear_native(value, CRON_JOB_NATIVE_TAG);
  job->finalized = true;
  cron_close_job(job);
  if (job->timer_closed) cron_free_job(job);
}

static bool cron_schedule_job(cron_job_t *job, ant_value_t *error);

static uint64_t cron_timer_delay(int64_t next, int64_t now) {
  const char *test_delay = getenv("ANT_TEST_CRON_TIMER_MS");
  if (test_delay && *test_delay) {
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(test_delay, &end, 10);
    if (errno == 0 && end && *end == '\0' && value > 0 && value <= 60000)
      return (uint64_t)value;
  }
  return next > now ? (uint64_t)(next - now) : 0;
}

static cron_job_t *cron_job_from_this(ant_t *js, const char *member, ant_value_t *error) {
  ant_value_t object = js_getthis(js);
  if (is_object_type(object)) {
    cron_job_t *job = js_get_native(object, CRON_JOB_NATIVE_TAG);
    if (job && job->js == js && job->object == object && !job->finalized) return job;
  }
  *error = js_mkerr_typed(
    js, JS_ERR_TYPE, "CronJob.%s can only be used on instances of CronJob", member
  );
  return NULL;
}

static ant_value_t cron_handler_fulfilled(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t object = js_get_slot(state, SLOT_DATA);
  cron_job_t *job = js_get_native(object, CRON_JOB_NATIVE_TAG);
  if (!job || job->stopped || job->closing) return js_mkundef();
  job->busy = false;
  ant_value_t error = js_mkundef();
  if (!cron_schedule_job(job, &error) && is_err(error)) return error;
  return js_mkundef();
}

static ant_value_t cron_handler_rejected(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t reason = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t promise = js_get_slot(state, SLOT_AUX);
  if (process_has_event_listeners(js, "unhandledRejection")) {
    ant_value_t event_args[2] = {reason, promise};
    emit_process_event(js, "unhandledRejection", event_args, 2);
  } else {
    ant_value_t report = js_mkpromise(js);
    js_reject_promise(js, report, reason);
    js->fatal_error = true;
  }
  return cron_handler_fulfilled(js, NULL, 0);
}

static void cron_timer_callback(uv_timer_t *timer) {
  cron_job_t *job = timer->data;
  if (!job || job->stopped || job->closing || job->busy) return;
  uv_timer_stop(timer);
  job->busy = true;

  ant_t *js = job->js;
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, job->object);
  GC_ROOT_PIN(js, job->handler);
  ant_value_t result = sv_vm_call(
    js->vm, js, job->handler, job->object, NULL, 0, NULL, false
  );
  GC_ROOT_PIN(js, result);

  if (is_err(result) || js->thrown_exists) {
    ant_value_t reason = js_take_thrown(js, result);
    if (process_has_event_listeners(js, "uncaughtException")) {
      emit_process_event(js, "uncaughtException", &reason, 1);
    } else {
      print_error_value(js, reason, js_mkundef(), NULL);
      GC_ROOT_RESTORE(js, root_mark);
      exit(EXIT_FAILURE);
    }
    job->busy = false;
    ant_value_t error = js_mkundef();
    cron_schedule_job(job, &error);
    GC_ROOT_RESTORE(js, root_mark);
    process_microtasks(js);
    return;
  }

  if (vtype(result) == kTypePromise) {
    if (!job->stopped && !job->closing) {
      const uint64_t keepalive_interval = UINT64_C(24) * 60 * 60 * 1000;
      uv_timer_start(
        &job->timer, cron_timer_callback, keepalive_interval, keepalive_interval
      );
    }
    ant_value_t state = js_mkobj(js);
    js_set_slot(state, SLOT_DATA, job->object);
    js_set_slot(state, SLOT_AUX, result);
    ant_value_t fulfilled = js_heavy_mkfun(js, cron_handler_fulfilled, state);
    ant_value_t rejected = js_heavy_mkfun(js, cron_handler_rejected, state);
    ant_value_t continuation = js_promise_then(js, result, fulfilled, rejected);
    promise_mark_handled(continuation);
  } else {
    job->busy = false;
    ant_value_t error = js_mkundef();
    cron_schedule_job(job, &error);
  }

  GC_ROOT_RESTORE(js, root_mark);
  process_microtasks(js);
}

static bool cron_schedule_job(cron_job_t *job, ant_value_t *error) {
  if (!job || job->stopped || job->closing) return true;
  ant_value_t options = js_mkobj(job->js);
  js_set(job->js, options, "tz", js_mkstr(job->js, job->time_zone, strlen(job->time_zone)));
  cron_zone_t zone;
  if (!cron_resolve_zone(job->js, options, &zone, NULL, error)) return false;

  int64_t now = cron_now_ms();
  int64_t next = 0;
  bool found = false;
  if (!cron_next_epoch(job->js, &job->expression, zone, now, &next, &found, error)) return false;
  if (!found) {
    *error = js_mkerr_typed(
      job->js, JS_ERR_TYPE, "Cron expression '%s' has no future occurrences", job->source
    );
    return false;
  }
  uint64_t delay = cron_timer_delay(next, now);
  int rc = uv_timer_start(&job->timer, cron_timer_callback, delay, 0);
  if (rc != 0) {
    *error = js_mkerr(job->js, "failed to schedule cron job: %s", uv_strerror(rc));
    return false;
  }
  return true;
}

static ant_value_t cron_job_get_cron(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t error = js_mkundef();
  cron_job_t *job = cron_job_from_this(js, "cron getter", &error);
  return job ? js_mkstr(js, job->source, strlen(job->source)) : error;
}

static ant_value_t cron_job_stop(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t object = js_getthis(js);
  ant_value_t error = js_mkundef();
  cron_job_t *job = cron_job_from_this(js, "stop()", &error);
  if (!job) return error;
  if (!job->stopped) cron_close_job(job);
  return object;
}

static ant_value_t cron_job_ref(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t object = js_getthis(js);
  ant_value_t error = js_mkundef();
  cron_job_t *job = cron_job_from_this(js, "ref()", &error);
  if (!job) return error;
  if (job && !job->closing) uv_ref((uv_handle_t *)&job->timer);
  return object;
}

static ant_value_t cron_job_unref(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t object = js_getthis(js);
  ant_value_t error = js_mkundef();
  cron_job_t *job = cron_job_from_this(js, "unref()", &error);
  if (!job) return error;
  if (job && !job->closing) uv_unref((uv_handle_t *)&job->timer);
  return object;
}

static ant_value_t cron_job_dispose(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t result = cron_job_stop(js, args, nargs);
  return is_err(result) ? result : js_mkundef();
}

static ant_value_t cron_parse(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != kTypeString)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Ant.cron.parse() expects a string cron expression as the first argument");

  size_t source_len = 0;
  const char *source = js_getstr(js, args[0], &source_len);
  cron_expression_t expression;
  char parse_error[192];
  if (cron_parse_expression(source, source_len, &expression, parse_error, sizeof(parse_error)) != 0)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid cron expression: %s", parse_error);

  ant_value_t error = js_mkundef();
  int64_t relative = 0;
  if (!cron_relative_ms(js, nargs > 1 ? args[1] : js_mkundef(), &relative, &error)) return error;
  cron_zone_t zone;
  if (!cron_resolve_zone(js, nargs > 2 ? args[2] : js_mkundef(), &zone, NULL, &error)) return error;

  int64_t next = 0;
  bool found = false;
  if (!cron_next_epoch(js, &expression, zone, relative, &next, &found, &error)) return error;
  return found ? cron_make_date(js, next) : js_mknull();
}

static ant_value_t cron_create_job(
  ant_t *js,
  ant_value_t schedule,
  ant_value_t handler,
  ant_value_t options
) {
  if (vtype(schedule) != kTypeString)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Ant.cron() expects a string cron expression");
  if (!is_callable(handler))
    return js_mkerr_typed(
      js, JS_ERR_TYPE, "Ant.cron(schedule, handler) expects a function handler as the second argument"
    );

  size_t source_len = 0;
  const char *source = js_getstr(js, schedule, &source_len);
  cron_expression_t expression;
  char parse_error[192];
  if (cron_parse_expression(source, source_len, &expression, parse_error, sizeof(parse_error)) != 0)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid cron expression: %s", parse_error);

  ant_value_t error = js_mkundef();
  cron_zone_t zone;
  char *zone_name = NULL;
  if (!cron_resolve_zone(js, options, &zone, &zone_name, &error)) return error;
  int64_t next = 0;
  bool found = false;
  if (!cron_next_epoch(js, &expression, zone, cron_now_ms(), &next, &found, &error)) {
    free(zone_name);
    return error;
  }
  if (!found) {
    free(zone_name);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cron expression '%.*s' has no future occurrences", (int)source_len, source);
  }

  cron_job_t *job = calloc(1, sizeof(*job));
  if (!job) {
    free(zone_name);
    return js_mkerr(js, "out of memory");
  }
  job->source = strndup(source, source_len);
  if (!job->source) {
    free(zone_name);
    free(job);
    return js_mkerr(js, "out of memory");
  }
  job->time_zone = zone_name;
  job->js = js;
  job->handler = handler;
  job->expression = expression;
  int rc = uv_timer_init(uv_default_loop(), &job->timer);
  if (rc != 0) {
    free(job->source);
    free(job->time_zone);
    free(job);
    return js_mkerr(js, "failed to initialize cron job: %s", uv_strerror(rc));
  }
  job->timer.data = job;
  cron_add_job(job);

  ant_value_t object = js_mkobj(js);
  js_set_proto_init(object, js->builtins.cron_proto);
  js_set_native(object, job, CRON_JOB_NATIVE_TAG);
  if (js_get_native(object, CRON_JOB_NATIVE_TAG) != job) {
    cron_close_job(job);
    return js_mkerr(js, "out of memory");
  }
  js_set_finalizer(object, cron_object_finalize);
  job->object = object;

  int64_t delay_now = cron_now_ms();
  uint64_t delay = cron_timer_delay(next, delay_now);
  rc = uv_timer_start(&job->timer, cron_timer_callback, delay, 0);
  if (rc != 0) {
    cron_close_job(job);
    return js_mkerr(js, "failed to schedule cron job: %s", uv_strerror(rc));
  }
  return object;
}

static bool cron_valid_title(const char *title, size_t len) {
  if (len == 0) return false;
  for (size_t i = 0; i < len; i++) if (
    !isalnum((unsigned char)title[i]) && title[i] != '-' && title[i] != '_'
  ) return false;
  return true;
}

#if defined(__APPLE__) || defined(_WIN32)
typedef struct {
  char *data;
  size_t length;
  size_t capacity;
} cron_text_t;

static bool cron_text_appendf(cron_text_t *text, const char *format, ...)
  __attribute__((format(printf, 2, 3)));
static bool cron_text_appendf(cron_text_t *text, const char *format, ...) {
  va_list args;
  va_start(args, format);
  va_list copy;
  va_copy(copy, args);
  int needed = vsnprintf(NULL, 0, format, copy);
  va_end(copy);
  if (needed < 0) {
    va_end(args);
    return false;
  }
  size_t required = text->length + (size_t)needed + 1;
  if (required > text->capacity) {
    size_t capacity = text->capacity ? text->capacity : 1024;
    while (capacity < required) {
      if (capacity > SIZE_MAX / 2) {
        va_end(args);
        return false;
      }
      capacity *= 2;
    }
    char *grown = realloc(text->data, capacity);
    if (!grown) {
      va_end(args);
      return false;
    }
    text->data = grown;
    text->capacity = capacity;
  }
  vsnprintf(text->data + text->length, text->capacity - text->length, format, args);
  va_end(args);
  text->length += (size_t)needed;
  return true;
}

static char *cron_xml_escape(const char *value) {
  cron_text_t text = {0};
  for (const char *p = value; *p; p++) {
    const char *replacement = NULL;
    switch (*p) {
      case '&': replacement = "&amp;"; break;
      case '<': replacement = "&lt;"; break;
      case '>': replacement = "&gt;"; break;
      case '\'': replacement = "&apos;"; break;
      case '"': replacement = "&quot;"; break;
      default:
        if (!cron_text_appendf(&text, "%c", *p)) goto oom;
        continue;
    }
    if (!cron_text_appendf(&text, "%s", replacement)) goto oom;
  }
  if (!text.data && !cron_text_appendf(&text, "%s", "")) goto oom;
  return text.data;
oom:
  free(text.data);
  return NULL;
}
#endif

#ifndef _WIN32
static int cron_run_command(char *const argv[], bool ignore_failure, char *error, size_t error_len) {
  pid_t pid = 0;
  int rc = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
  if (rc != 0) {
    snprintf(error, error_len, "%s: %s", argv[0], strerror(rc));
    return -1;
  }
  int status = 0;
  pid_t waited = 0;
  do waited = waitpid(pid, &status, 0); while (waited < 0 && errno == EINTR);
  if (waited < 0) {
    snprintf(error, error_len, "cannot wait for %s: %s", argv[0], strerror(errno));
    return -1;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    if (ignore_failure) return 0;
    snprintf(error, error_len, "%s exited unsuccessfully", argv[0]);
    return -1;
  }
  return 0;
}

#ifndef __APPLE__
static char *cron_shell_quote(const char *value) {
  size_t len = 2;
  for (const char *p = value; *p; p++) len += *p == '\'' ? 4 : 1;
  char *out = malloc(len + 1);
  if (!out) return NULL;
  char *write = out;
  *write++ = '\'';
  for (const char *p = value; *p; p++) {
    if (*p == '\'') {
      memcpy(write, "'\\''", 4);
      write += 4;
    } else *write++ = *p;
  }
  *write++ = '\'';
  *write = '\0';
  return out;
}

static char *cron_read_crontab(char *error, size_t error_len) {
  FILE *pipe = popen("LC_ALL=C crontab -l 2>&1", "r");
  if (!pipe) {
    snprintf(error, error_len, "cannot read crontab: %s", strerror(errno));
    return NULL;
  }
  size_t length = 0;
  size_t capacity = 1024;
  char *out = malloc(capacity);
  if (!out) {
    pclose(pipe);
    snprintf(error, error_len, "out of memory");
    return NULL;
  }
  char chunk[512];
  while (fgets(chunk, sizeof(chunk), pipe)) {
    size_t chunk_len = strlen(chunk);
    if (length + chunk_len + 1 > capacity) {
      while (length + chunk_len + 1 > capacity) capacity *= 2;
      char *grown = realloc(out, capacity);
      if (!grown) {
        free(out);
        pclose(pipe);
        snprintf(error, error_len, "out of memory");
        return NULL;
      }
      out = grown;
    }
    memcpy(out + length, chunk, chunk_len);
    length += chunk_len;
  }
  int status = pclose(pipe);
  out[length] = '\0';
  if (status != 0) {
    if (strstr(out, "no crontab for") != NULL) {
      out[0] = '\0';
      return out;
    }
    snprintf(
      error, error_len, "crontab -l failed%s%s",
      length > 0 ? ": " : "", length > 0 ? out : ""
    );
    free(out);
    return NULL;
  }
  return out;
}

static int cron_linux_lock(char *error, size_t error_len) {
  char path[128];
  snprintf(path, sizeof(path), "/tmp/ant-cron-%lu.lock", (unsigned long)getuid());
  int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    snprintf(error, error_len, "cannot open cron lock: %s", strerror(errno));
    return -1;
  }
  struct stat info;
  if (fstat(fd, &info) != 0 || info.st_uid != getuid()) {
    snprintf(error, error_len, "cron lock has an unexpected owner");
    close(fd);
    return -1;
  }
  struct flock lock = {
    .l_type = F_WRLCK,
    .l_whence = SEEK_SET,
    .l_start = 0,
    .l_len = 0,
  };
  while (fcntl(fd, F_SETLKW, &lock) != 0) {
    if (errno == EINTR) continue;
    snprintf(error, error_len, "cannot acquire cron lock: %s", strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

static char *cron_filter_crontab(const char *input, const char *title) {
  size_t marker_len = strlen(title) + 14;
  char *marker = malloc(marker_len);
  if (!marker) return NULL;
  snprintf(marker, marker_len, "# ant-cron: %s", title);

  size_t input_len = strlen(input);
  char *output = malloc(input_len + 2);
  if (!output) {
    free(marker);
    return NULL;
  }
  size_t written = 0;
  bool skip_next = false;
  const char *cursor = input;
  while (*cursor) {
    const char *end = strchr(cursor, '\n');
    size_t line_len = end ? (size_t)(end - cursor) : strlen(cursor);
    if (skip_next) {
      skip_next = false;
    } else if (line_len == strlen(marker) && memcmp(cursor, marker, line_len) == 0) {
      skip_next = true;
    } else {
      memcpy(output + written, cursor, line_len);
      written += line_len;
      output[written++] = '\n';
    }
    cursor = end ? end + 1 : cursor + line_len;
  }
  output[written] = '\0';
  free(marker);
  return output;
}

static int cron_install_crontab(const char *contents, char *error, size_t error_len) {
  char path[] = "/tmp/ant-cron-XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) {
    snprintf(error, error_len, "cannot create temporary crontab: %s", strerror(errno));
    return -1;
  }
  size_t len = strlen(contents);
  size_t offset = 0;
  while (offset < len) {
    ssize_t written = write(fd, contents + offset, len - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) break;
    offset += (size_t)written;
  }
  close(fd);
  if (offset != len) {
    unlink(path);
    snprintf(error, error_len, "cannot write temporary crontab");
    return -1;
  }
  char *argv[] = {"crontab", path, NULL};
  int rc = cron_run_command(argv, false, error, error_len);
  unlink(path);
  return rc;
}

static int cron_linux_register(
  const char *script,
  const char *schedule,
  const char *title,
  const char *executable,
  char *error,
  size_t error_len
) {
  int lock_fd = cron_linux_lock(error, error_len);
  if (lock_fd < 0) return -1;
  char *current = cron_read_crontab(error, error_len);
  if (!current) {
    close(lock_fd);
    return -1;
  }
  char *filtered = cron_filter_crontab(current, title);
  free(current);
  if (!filtered) goto oom;
  char *quoted_exe = cron_shell_quote(executable);
  char *quoted_schedule = cron_shell_quote(schedule);
  char *quoted_script = cron_shell_quote(script);
  if (!quoted_exe || !quoted_schedule || !quoted_script) {
    free(filtered); free(quoted_exe); free(quoted_schedule); free(quoted_script);
    goto oom;
  }

  size_t needed = strlen(filtered) + strlen(title) + strlen(schedule) +
    strlen(quoted_exe) + strlen(quoted_schedule) + strlen(quoted_script) + 96;
  char *updated = malloc(needed);
  if (!updated) {
    free(filtered); free(quoted_exe); free(quoted_schedule); free(quoted_script);
    goto oom;
  }
  snprintf(
    updated, needed,
    "%s# ant-cron: %s\n%s %s --cron-title=%s --cron-period=%s %s\n",
    filtered, title, schedule, quoted_exe, title, quoted_schedule, quoted_script
  );
  int rc = cron_install_crontab(updated, error, error_len);
  free(updated); free(filtered); free(quoted_exe); free(quoted_schedule); free(quoted_script);
  close(lock_fd);
  return rc;

oom:
  close(lock_fd);
  snprintf(error, error_len, "out of memory");
  return -1;
}

static int cron_linux_remove(const char *title, char *error, size_t error_len) {
  int lock_fd = cron_linux_lock(error, error_len);
  if (lock_fd < 0) return -1;
  char *current = cron_read_crontab(error, error_len);
  if (!current) {
    close(lock_fd);
    return -1;
  }
  char *filtered = cron_filter_crontab(current, title);
  if (!filtered) {
    free(current);
    close(lock_fd);
    snprintf(error, error_len, "out of memory");
    return -1;
  }
  if (strcmp(current, filtered) == 0) {
    free(current);
    free(filtered);
    close(lock_fd);
    return 0;
  }
  free(current);
  int rc = cron_install_crontab(filtered, error, error_len);
  free(filtered);
  close(lock_fd);
  return rc;
}
#endif

#ifdef __APPLE__
static bool cron_macos_append_interval(
  cron_text_t *text, int minute, int hour, int day, int month, int weekday
) {
  if (!cron_text_appendf(text, "<dict>")) return false;
  if (minute >= 0 && !cron_text_appendf(
    text, "<key>Minute</key><integer>%d</integer>", minute
  )) return false;
  if (hour >= 0 && !cron_text_appendf(
    text, "<key>Hour</key><integer>%d</integer>", hour
  )) return false;
  if (day >= 0 && !cron_text_appendf(
    text, "<key>Day</key><integer>%d</integer>", day
  )) return false;
  if (month >= 0 && !cron_text_appendf(
    text, "<key>Month</key><integer>%d</integer>", month
  )) return false;
  if (weekday >= 0 && !cron_text_appendf(
    text, "<key>Weekday</key><integer>%d</integer>", weekday
  )) return false;
  return cron_text_appendf(text, "</dict>");
}

static char *cron_macos_calendar(
  const cron_expression_t *expression, char *error, size_t error_len
) {
  int minutes[60], hours[24], months[12];
  int minute_count = 0, hour_count = 0, month_count = 0;
  if (expression->minute_wildcard) minutes[minute_count++] = -1;
  else for (int value = 0; value < 60; value++)
    if (expression->minutes & (UINT64_C(1) << value)) minutes[minute_count++] = value;
  if (expression->hour_wildcard) hours[hour_count++] = -1;
  else for (int value = 0; value < 24; value++)
    if (expression->hours & (UINT32_C(1) << value)) hours[hour_count++] = value;
  const uint16_t all_months = (uint16_t)(((UINT16_C(1) << 13) - 1) & ~UINT16_C(1));
  if (expression->months == all_months) months[month_count++] = -1;
  else for (int value = 1; value <= 12; value++)
    if (expression->months & (UINT16_C(1) << value)) months[month_count++] = value;

  int days[38], weekdays[38], branch_count = 0;
  if (expression->day_wildcard && expression->weekday_wildcard) {
    days[branch_count] = weekdays[branch_count] = -1;
    branch_count++;
  } else if (!expression->day_wildcard && !expression->weekday_wildcard) {
    for (int value = 1; value <= 31; value++) if (
      expression->days & (UINT32_C(1) << value)
    ) {
      days[branch_count] = value;
      weekdays[branch_count++] = -1;
    }
    for (int value = 0; value < 7; value++) if (
      expression->weekdays & (UINT8_C(1) << value)
    ) {
      days[branch_count] = -1;
      weekdays[branch_count++] = value;
    }
  } else if (!expression->day_wildcard) {
    for (int value = 1; value <= 31; value++) if (
      expression->days & (UINT32_C(1) << value)
    ) {
      days[branch_count] = value;
      weekdays[branch_count++] = -1;
    }
  } else {
    for (int value = 0; value < 7; value++) if (
      expression->weekdays & (UINT8_C(1) << value)
    ) {
      days[branch_count] = -1;
      weekdays[branch_count++] = value;
    }
  }

  size_t interval_count = (size_t)minute_count * (size_t)hour_count *
    (size_t)month_count * (size_t)branch_count;
  if (interval_count > 10000) {
    snprintf(
      error, error_len,
      "Cron expression requires %zu macOS calendar intervals (maximum 10000)",
      interval_count
    );
    return NULL;
  }

  cron_text_t text = {0};
  if (!cron_text_appendf(&text, "<array>")) goto oom;
  for (int mi = 0; mi < minute_count; mi++)
    for (int hi = 0; hi < hour_count; hi++)
      for (int moi = 0; moi < month_count; moi++)
        for (int bi = 0; bi < branch_count; bi++) if (!cron_macos_append_interval(
          &text, minutes[mi], hours[hi], days[bi], months[moi], weekdays[bi]
        )) goto oom;
  if (!cron_text_appendf(&text, "</array>")) goto oom;
  return text.data;

oom:
  free(text.data);
  snprintf(error, error_len, "out of memory");
  return NULL;
}

static int cron_macos_path(const char *title, char *path, size_t path_len) {
  const char *home = getenv("HOME");
  if (!home || !*home) return -1;
  char directory[PATH_MAX];
  if ((size_t)snprintf(directory, sizeof(directory), "%s/Library/LaunchAgents", home) >= sizeof(directory))
    return -1;
  if (ant_mkdir_p(directory) != 0) return -1;
  return (size_t)snprintf(path, path_len, "%s/ant.cron.%s.plist", directory, title) < path_len ? 0 : -1;
}

static int cron_macos_remove(const char *title, char *error, size_t error_len) {
  char path[PATH_MAX];
  if (cron_macos_path(title, path, sizeof(path)) != 0) {
    snprintf(error, error_len, "cannot resolve LaunchAgents directory");
    return -1;
  }
  char domain[64];
  snprintf(domain, sizeof(domain), "gui/%lu", (unsigned long)getuid());
  if (access(path, F_OK) != 0) {
    if (errno == ENOENT) return 0;
    snprintf(error, error_len, "cannot inspect %s: %s", path, strerror(errno));
    return -1;
  }
  char *argv[] = {"launchctl", "bootout", domain, path, NULL};
  if (cron_run_command(argv, false, error, error_len) != 0) return -1;
  if (unlink(path) != 0 && errno != ENOENT) {
    snprintf(error, error_len, "cannot remove %s: %s", path, strerror(errno));
    return -1;
  }
  return 0;
}

static bool cron_macos_restore(
  const char *path,
  const char *backup,
  bool old_moved,
  char *domain,
  char *error,
  size_t error_len
) {
  if (old_moved && rename(backup, path) != 0) {
    snprintf(error, error_len, "cannot restore LaunchAgent file: %s", strerror(errno));
    return false;
  }
  char *restore[] = {"launchctl", "bootstrap", domain, (char *)path, NULL};
  return cron_run_command(restore, false, error, error_len) == 0;
}

static int cron_macos_register(
  const char *script,
  const char *schedule,
  const char *title,
  const char *executable,
  char *error,
  size_t error_len
) {
  char path[PATH_MAX];
  if (cron_macos_path(title, path, sizeof(path)) != 0) {
    snprintf(error, error_len, "cannot resolve LaunchAgents directory");
    return -1;
  }
  size_t period_len = strlen(schedule) + 15;
  size_t title_len = strlen(title) + 14;
  char *period_arg = malloc(period_len);
  char *title_arg = malloc(title_len);
  if (!period_arg || !title_arg) {
    free(period_arg); free(title_arg);
    snprintf(error, error_len, "out of memory");
    return -1;
  }
  snprintf(period_arg, period_len, "--cron-period=%s", schedule);
  snprintf(title_arg, title_len, "--cron-title=%s", title);
  char *values[] = {
    cron_xml_escape(title), cron_xml_escape(executable), cron_xml_escape(title_arg),
    cron_xml_escape(period_arg), cron_xml_escape(script),
  };
  free(period_arg); free(title_arg);
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) if (!values[i]) {
    for (size_t j = 0; j < sizeof(values) / sizeof(values[0]); j++) free(values[j]);
    snprintf(error, error_len, "out of memory");
    return -1;
  }
  cron_expression_t expression;
  char parse_error[192];
  if (cron_parse_expression(
    schedule, strlen(schedule), &expression, parse_error, sizeof(parse_error)
  ) != 0) {
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) free(values[i]);
    snprintf(error, error_len, "Invalid cron expression: %s", parse_error);
    return -1;
  }
  char *calendar = cron_macos_calendar(&expression, error, error_len);
  if (!calendar) {
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) free(values[i]);
    return -1;
  }
  size_t needed = strlen(values[0]) + strlen(values[1]) + strlen(values[2]) +
    strlen(values[3]) + strlen(values[4]) + strlen(calendar) + 1000;
  char *plist = malloc(needed);
  if (!plist) {
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) free(values[i]);
    free(calendar);
    snprintf(error, error_len, "out of memory");
    return -1;
  }
  snprintf(
    plist, needed,
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\"><dict>"
    "<key>Label</key><string>ant.cron.%s</string>"
    "<key>ProgramArguments</key><array><string>%s</string><string>%s</string>"
    "<string>%s</string><string>%s</string></array>"
    "<key>StartCalendarInterval</key>%s"
    "</dict></plist>\n",
    values[0], values[1], values[2], values[3], values[4], calendar
  );
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) free(values[i]);
  free(calendar);

  char temporary[PATH_MAX];
  if ((size_t)snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path) >=
      sizeof(temporary)) {
    free(plist);
    snprintf(error, error_len, "temporary LaunchAgent path is too long");
    return -1;
  }
  int temporary_fd = mkstemp(temporary);
  FILE *file = temporary_fd >= 0 ? fdopen(temporary_fd, "wb") : NULL;
  int write_failed = !file;
  if (temporary_fd >= 0 && !file) close(temporary_fd);
  if (file) {
    write_failed = fwrite(plist, 1, strlen(plist), file) != strlen(plist);
    if (fclose(file) != 0) write_failed = 1;
  }
  free(plist);
  if (write_failed) {
    unlink(temporary);
    snprintf(error, error_len, "cannot write %s: %s", path, strerror(errno));
    return -1;
  }

  char domain[64];
  snprintf(domain, sizeof(domain), "gui/%lu", (unsigned long)getuid());
  bool had_existing = access(path, F_OK) == 0;
  char backup[PATH_MAX] = {0};
  if (had_existing) {
    if ((size_t)snprintf(backup, sizeof(backup), "%s.backup.XXXXXX", path) >=
        sizeof(backup)) {
      unlink(temporary);
      snprintf(error, error_len, "backup LaunchAgent path is too long");
      return -1;
    }
    int backup_fd = mkstemp(backup);
    if (backup_fd < 0) {
      unlink(temporary);
      snprintf(error, error_len, "cannot reserve LaunchAgent backup: %s", strerror(errno));
      return -1;
    }
    close(backup_fd);
    unlink(backup);
  } else if (errno != ENOENT) {
    unlink(temporary);
    snprintf(error, error_len, "cannot inspect %s: %s", path, strerror(errno));
    return -1;
  }

  char *bootout[] = {"launchctl", "bootout", domain, path, NULL};
  if (had_existing && cron_run_command(bootout, false, error, error_len) != 0) {
    unlink(temporary);
    return -1;
  }
  bool old_moved = false;
  if (had_existing && rename(path, backup) == 0) old_moved = true;
  if ((had_existing && !old_moved) || rename(temporary, path) != 0) {
    int saved_errno = errno;
    unlink(temporary);
    if (had_existing) {
      char rollback_error[512];
      if (!cron_macos_restore(
        path, backup, old_moved, domain, rollback_error, sizeof(rollback_error)
      )) {
        snprintf(
          error, error_len, "cannot replace %s: %s; rollback failed: %s",
          path, strerror(saved_errno), rollback_error
        );
        return -1;
      }
    }
    snprintf(error, error_len, "cannot replace %s: %s", path, strerror(saved_errno));
    return -1;
  }
  char *bootstrap[] = {"launchctl", "bootstrap", domain, path, NULL};
  if (cron_run_command(bootstrap, false, error, error_len) != 0) {
    char primary_error[512];
    snprintf(primary_error, sizeof(primary_error), "%s", error);
    cron_run_command(bootout, true, error, error_len);
    unlink(path);
    if (had_existing) {
      char rollback_error[512];
      if (!cron_macos_restore(
        path, backup, true, domain, rollback_error, sizeof(rollback_error)
      )) {
        snprintf(
          error, error_len, "%s; rollback failed: %s", primary_error, rollback_error
        );
        return -1;
      }
    }
    snprintf(error, error_len, "%s", primary_error);
    return -1;
  }
  if (had_existing) unlink(backup);
  return 0;
}
#endif
#else
static int cron_popcount64(uint64_t bits) {
  int count = 0;
  while (bits) { bits &= bits - 1; count++; }
  return count;
}

static int cron_step_interval(uint64_t bits) {
  if (!bits) return 0;
  uint64_t remaining = bits;
  int first = __builtin_ctzll(remaining);
  remaining &= remaining - 1;
  if (!remaining) return 64;
  int previous = __builtin_ctzll(remaining);
  int step = previous - first;
  remaining &= remaining - 1;
  while (remaining) {
    int next = __builtin_ctzll(remaining);
    if (next - previous != step) return 0;
    previous = next;
    remaining &= remaining - 1;
  }
  return step;
}

static const char *const cron_windows_months[] = {
  NULL, "January", "February", "March", "April", "May", "June",
  "July", "August", "September", "October", "November", "December",
};

static const char *const cron_windows_weekdays[] = {
  "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};

static bool cron_windows_append_months(cron_text_t *text, uint16_t months) {
  if (!cron_text_appendf(text, "<Months>")) return false;
  for (int value = 1; value <= 12; value++) if (months & (UINT16_C(1) << value))
    if (!cron_text_appendf(text, "<%s/>", cron_windows_months[value])) return false;
  return cron_text_appendf(text, "</Months>");
}

static bool cron_windows_append_days(cron_text_t *text, uint32_t days) {
  if (!cron_text_appendf(text, "<DaysOfMonth>")) return false;
  for (int value = 1; value <= 31; value++) if (days & (UINT32_C(1) << value))
    if (!cron_text_appendf(text, "<Day>%d</Day>", value)) return false;
  return cron_text_appendf(text, "</DaysOfMonth>");
}

static bool cron_windows_append_weekdays(cron_text_t *text, uint8_t weekdays) {
  if (!cron_text_appendf(text, "<DaysOfWeek>")) return false;
  for (int value = 0; value < 7; value++) if (weekdays & (UINT8_C(1) << value))
    if (!cron_text_appendf(text, "<%s/>", cron_windows_weekdays[value])) return false;
  return cron_text_appendf(text, "</DaysOfWeek>");
}

enum {
  CRON_WINDOWS_BY_DAY,
  CRON_WINDOWS_BY_WEEK,
  CRON_WINDOWS_BY_MONTH,
  CRON_WINDOWS_BY_MONTH_WEEKDAY,
  CRON_WINDOWS_BY_MONTH_ALL_DAYS,
};

static bool cron_windows_append_trigger(
  cron_text_t *text,
  int minute,
  int hour,
  const cron_expression_t *expression,
  int schedule_type
) {
  if (!cron_text_appendf(
    text,
    "<CalendarTrigger><StartBoundary>2000-01-01T%02d:%02d:00</StartBoundary>"
    "<Enabled>true</Enabled>", hour, minute
  )) return false;
  if (schedule_type == CRON_WINDOWS_BY_DAY) {
    if (!cron_text_appendf(
      text, "<ScheduleByDay><DaysInterval>1</DaysInterval></ScheduleByDay>"
    )) return false;
  } else if (schedule_type == CRON_WINDOWS_BY_MONTH ||
             schedule_type == CRON_WINDOWS_BY_MONTH_ALL_DAYS) {
    if (!cron_text_appendf(text, "<ScheduleByMonth>")) return false;
    uint32_t days = schedule_type == CRON_WINDOWS_BY_MONTH
      ? expression->days : UINT32_MAX & ~UINT32_C(1);
    if (!cron_windows_append_days(text, days) ||
        !cron_windows_append_months(text, expression->months)) return false;
    if (!cron_text_appendf(text, "</ScheduleByMonth>")) return false;
  } else if (schedule_type == CRON_WINDOWS_BY_WEEK) {
    if (!cron_text_appendf(text, "<ScheduleByWeek><WeeksInterval>1</WeeksInterval>"))
      return false;
    if (!cron_windows_append_weekdays(text, expression->weekdays) ||
        !cron_text_appendf(text, "</ScheduleByWeek>")) return false;
  } else {
    if (!cron_text_appendf(
      text,
      "<ScheduleByMonthDayOfWeek><Weeks><Week>1</Week><Week>2</Week>"
      "<Week>3</Week><Week>4</Week><Week>Last</Week></Weeks>"
    )) return false;
    if (!cron_windows_append_weekdays(text, expression->weekdays) ||
        !cron_windows_append_months(text, expression->months)) return false;
    if (!cron_text_appendf(text, "</ScheduleByMonthDayOfWeek>")) return false;
  }
  return cron_text_appendf(text, "</CalendarTrigger>");
}

static char *cron_windows_triggers(
  const cron_expression_t *expression, char *error, size_t error_len
) {
  const uint16_t all_months = (uint16_t)(((UINT16_C(1) << 13) - 1) & ~UINT16_C(1));
  const uint32_t all_days = UINT32_MAX & ~UINT32_C(1);
  const uint8_t all_weekdays = UINT8_C(0x7f);
  const uint64_t all_minutes = (UINT64_C(1) << 60) - 1;
  const uint32_t all_hours = (UINT32_C(1) << 24) - 1;
  bool days_wild = expression->days == all_days;
  bool weekdays_wild = expression->weekdays == all_weekdays;
  bool months_wild = expression->months == all_months;
  int minute_count = cron_popcount64(expression->minutes);
  int hour_count = cron_popcount64(expression->hours);
  int minute_interval = cron_step_interval(expression->minutes);
  int hour_interval = cron_step_interval(expression->hours);
  if (minute_count == 1) minute_interval = 60;
  if (hour_count == 1) hour_interval = 24;
  bool minute_repetition = expression->hours == all_hours && minute_interval > 0 &&
    minute_interval <= 60 && 60 % minute_interval == 0 &&
    minute_count == 60 / minute_interval;
  bool hour_repetition = minute_count == 1 && hour_interval > 0 &&
    hour_interval <= 24 && 24 % hour_interval == 0 &&
    hour_count == 24 / hour_interval;
  if (days_wild && weekdays_wild && months_wild &&
      (minute_repetition || hour_repetition)) {
    cron_text_t repeat = {0};
    int first_minute = __builtin_ctzll(expression->minutes);
    int first_hour = __builtin_ctz(expression->hours);
    if (!cron_text_appendf(
      &repeat,
      "<CalendarTrigger><StartBoundary>2000-01-01T%02d:%02d:00</StartBoundary>"
      "<Repetition><Interval>PT%d%c</Interval></Repetition><Enabled>true</Enabled>"
      "<ScheduleByDay><DaysInterval>1</DaysInterval></ScheduleByDay></CalendarTrigger>",
      first_hour, first_minute,
      minute_repetition ? minute_interval : hour_interval,
      minute_repetition ? 'M' : 'H'
    )) goto oom;
    return repeat.data;
  }

  bool needs_or_split = !days_wild && !weekdays_wild;
  int trigger_count = minute_count * hour_count * (needs_or_split ? 2 : 1);
  if (trigger_count > 48) {
    snprintf(
      error, error_len,
      "Cron expression requires %d Windows Task Scheduler triggers (maximum 48)",
      trigger_count
    );
    return NULL;
  }

  cron_text_t text = {0};
  for (int minute = 0; minute < 60; minute++) if (
    expression->minutes & (UINT64_C(1) << minute)
  ) for (int hour = 0; hour < 24; hour++) if (
    expression->hours & (UINT32_C(1) << hour)
  ) {
    if (!days_wild && !cron_windows_append_trigger(
      &text, minute, hour, expression, CRON_WINDOWS_BY_MONTH
    )) goto oom;
    if (!weekdays_wild && !cron_windows_append_trigger(
      &text, minute, hour, expression,
      months_wild ? CRON_WINDOWS_BY_WEEK : CRON_WINDOWS_BY_MONTH_WEEKDAY
    )) goto oom;
    if (days_wild && weekdays_wild && !cron_windows_append_trigger(
      &text, minute, hour, expression,
      months_wild ? CRON_WINDOWS_BY_DAY : CRON_WINDOWS_BY_MONTH_ALL_DAYS
    )) goto oom;
  }
  return text.data;

oom:
  free(text.data);
  snprintf(error, error_len, "out of memory");
  return NULL;
}

static char *cron_windows_user_sid(char *error, size_t error_len) {
  HANDLE token = NULL;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    snprintf(error, error_len, "cannot open current user token: %lu", (unsigned long)GetLastError());
    return NULL;
  }
  DWORD needed = 0;
  GetTokenInformation(token, TokenUser, NULL, 0, &needed);
  TOKEN_USER *user = malloc(needed);
  if (!user || !GetTokenInformation(token, TokenUser, user, needed, &needed)) {
    snprintf(error, error_len, "cannot resolve current user SID: %lu", (unsigned long)GetLastError());
    free(user); CloseHandle(token); return NULL;
  }
  LPSTR sid_value = NULL;
  if (!ConvertSidToStringSidA(user->User.Sid, &sid_value)) {
    snprintf(error, error_len, "cannot format current user SID: %lu", (unsigned long)GetLastError());
    free(user); CloseHandle(token); return NULL;
  }
  char *sid = strdup(sid_value);
  LocalFree(sid_value); free(user); CloseHandle(token);
  if (!sid) snprintf(error, error_len, "out of memory");
  return sid;
}

static int cron_windows_run(
  char *command, DWORD *exit_code, char *error, size_t error_len
) {
  char temp_dir[MAX_PATH], output_path[MAX_PATH];
  if (!GetTempPathA(sizeof(temp_dir), temp_dir) ||
      !GetTempFileNameA(temp_dir, "ant", 0, output_path)) {
    snprintf(error, error_len, "cannot create Task Scheduler output file: %lu", (unsigned long)GetLastError());
    return -1;
  }
  SECURITY_ATTRIBUTES attributes = {
    .nLength = sizeof(attributes), .lpSecurityDescriptor = NULL, .bInheritHandle = TRUE,
  };
  HANDLE output = CreateFileA(
    output_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
    CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL
  );
  if (output == INVALID_HANDLE_VALUE) {
    DeleteFileA(output_path);
    snprintf(error, error_len, "cannot capture Task Scheduler output: %lu", (unsigned long)GetLastError());
    return -1;
  }
  STARTUPINFOA startup = {.cb = sizeof(startup)};
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = output;
  startup.hStdError = output;
  PROCESS_INFORMATION process = {0};
  if (!CreateProcessA(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
    CloseHandle(output); DeleteFileA(output_path);
    snprintf(error, error_len, "Task Scheduler command failed: %lu", (unsigned long)GetLastError());
    return -1;
  }
  DWORD wait_result = WaitForSingleObject(process.hProcess, INFINITE);
  if (wait_result != WAIT_OBJECT_0) {
    DWORD wait_error = GetLastError();
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(output);
    DeleteFileA(output_path);
    snprintf(error, error_len, "cannot wait for Task Scheduler command: %lu", (unsigned long)wait_error);
    return -1;
  }
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  CloseHandle(output);
  if (exit_code) *exit_code = code;
  if (code != 0) {
    FILE *file = fopen(output_path, "rb");
    char details[320] = {0};
    if (file) {
      size_t read = fread(details, 1, sizeof(details) - 1, file);
      details[read] = '\0';
      fclose(file);
    }
    DeleteFileA(output_path);
    snprintf(
      error, error_len, "Task Scheduler command exited with code %lu%s%s",
      (unsigned long)code, details[0] ? ": " : "", details
    );
    return -1;
  }
  DeleteFileA(output_path);
  return 0;
}

static int cron_windows_remove_com(const char *title, char *error, size_t error_len) {
  HRESULT initialized = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  bool uninitialize = SUCCEEDED(initialized);
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
    snprintf(error, error_len, "cannot initialize Task Scheduler: 0x%08lx", (unsigned long)initialized);
    return -1;
  }

  ITaskService *service = NULL;
  ITaskFolder *folder = NULL;
  BSTR root_path = NULL;
  BSTR task_name = NULL;
  HRESULT result = CoCreateInstance(
    &CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER,
    &IID_ITaskService, (void **)&service
  );
  if (FAILED(result)) goto done;
  VARIANT empty;
  VariantInit(&empty);
  result = ITaskService_Connect(service, empty, empty, empty, empty);
  if (FAILED(result)) goto done;
  root_path = SysAllocString(L"\\");
  if (!root_path) { result = E_OUTOFMEMORY; goto done; }
  result = ITaskService_GetFolder(service, root_path, &folder);
  if (FAILED(result)) goto done;

  size_t name_len = strlen(title) + 10;
  char *name = malloc(name_len);
  if (!name) { result = E_OUTOFMEMORY; goto done; }
  snprintf(name, name_len, "ant-cron-%s", title);
  int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, NULL, 0);
  if (wide_len <= 0) {
    free(name);
    result = HRESULT_FROM_WIN32(GetLastError());
    goto done;
  }
  task_name = SysAllocStringLen(NULL, (UINT)(wide_len - 1));
  if (!task_name) {
    free(name);
    result = E_OUTOFMEMORY;
    goto done;
  }
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, task_name, wide_len);
  free(name);
  result = ITaskFolder_DeleteTask(folder, task_name, 0);

done:
  if (task_name) SysFreeString(task_name);
  if (root_path) SysFreeString(root_path);
  if (folder) ITaskFolder_Release(folder);
  if (service) ITaskService_Release(service);
  if (uninitialize) CoUninitialize();
  if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
      result == (HRESULT)0x8004130fL) return 0;
  if (FAILED(result)) {
    snprintf(error, error_len, "Task Scheduler removal failed: 0x%08lx", (unsigned long)result);
    return -1;
  }
  return 0;
}

static int cron_windows_remove(const char *title, char *error, size_t error_len) {
  const char *tool = getenv("ANT_CRON_SCHTASKS");
  if (!tool || !*tool) return cron_windows_remove_com(title, error, error_len);
  size_t command_len = strlen(tool) + strlen(title) + 48;
  char *command = malloc(command_len);
  if (!command) { snprintf(error, error_len, "out of memory"); return -1; }
  snprintf(command, command_len, "%s /Delete /F /TN \"ant-cron-%s\"", tool, title);
  DWORD code = 0;
  int rc = cron_windows_run(command, &code, error, error_len);
  free(command);
  if (rc != 0 && code == 1) return 0;
  return rc;
}

static int cron_windows_register(
  const char *script,
  const char *schedule,
  const char *title,
  const char *executable,
  char *error,
  size_t error_len
) {
  cron_expression_t expression;
  char parse_error[192];
  if (cron_parse_expression(
    schedule, strlen(schedule), &expression, parse_error, sizeof(parse_error)
  ) != 0) {
    snprintf(error, error_len, "Invalid cron expression: %s", parse_error);
    return -1;
  }
  char *triggers = cron_windows_triggers(&expression, error, error_len);
  if (!triggers) return -1;
  char *sid = cron_windows_user_sid(error, error_len);
  char *escaped_executable = cron_xml_escape(executable);
  size_t args_len = strlen(title) + strlen(schedule) + strlen(script) + 64;
  char *arguments = malloc(args_len);
  if (arguments) snprintf(
    arguments, args_len, "--cron-title=%s --cron-period=\"%s\" \"%s\"",
    title, schedule, script
  );
  char *escaped_arguments = arguments ? cron_xml_escape(arguments) : NULL;
  free(arguments);
  if (!sid || !escaped_executable || !escaped_arguments) {
    free(triggers); free(sid); free(escaped_executable); free(escaped_arguments);
    if (!error[0]) snprintf(error, error_len, "out of memory");
    return -1;
  }

  cron_text_t xml = {0};
  bool built = cron_text_appendf(
    &xml,
    "<?xml version=\"1.0\"?>"
    "<Task version=\"1.4\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
    "<Triggers>%s</Triggers><Principals><Principal id=\"Author\"><UserId>%s</UserId>"
    "<LogonType>S4U</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>"
    "<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
    "<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
    "<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><StartWhenAvailable>true</StartWhenAvailable>"
    "<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Enabled>true</Enabled></Settings>"
    "<Actions Context=\"Author\"><Exec><Command>%s</Command><Arguments>%s</Arguments>"
    "</Exec></Actions></Task>",
    triggers, sid, escaped_executable, escaped_arguments
  );
  free(triggers); free(sid); free(escaped_executable); free(escaped_arguments);
  if (!built) {
    free(xml.data); snprintf(error, error_len, "out of memory"); return -1;
  }

  char temp_dir[MAX_PATH], xml_path[MAX_PATH];
  if (!GetTempPathA(sizeof(temp_dir), temp_dir) ||
      !GetTempFileNameA(temp_dir, "ant", 0, xml_path)) {
    free(xml.data);
    snprintf(error, error_len, "cannot create temporary task XML: %lu", (unsigned long)GetLastError());
    return -1;
  }
  FILE *file = fopen(xml_path, "wb");
  bool write_failed = !file;
  if (file) {
    write_failed = fwrite(xml.data, 1, xml.length, file) != xml.length;
    if (fclose(file) != 0) write_failed = true;
  }
  free(xml.data);
  if (write_failed) {
    DeleteFileA(xml_path);
    snprintf(error, error_len, "cannot write temporary task XML");
    return -1;
  }
  const char *tool = getenv("ANT_CRON_SCHTASKS");
  if (!tool || !*tool) tool = "schtasks";
  size_t command_len = strlen(tool) + strlen(title) + strlen(xml_path) + 64;
  char *command = malloc(command_len);
  if (!command) {
    DeleteFileA(xml_path); snprintf(error, error_len, "out of memory"); return -1;
  }
  snprintf(
    command, command_len, "%s /Create /F /TN \"ant-cron-%s\" /XML \"%s\"",
    tool, title, xml_path
  );
  DWORD code = 0;
  int rc = cron_windows_run(command, &code, error, error_len);
  free(command); DeleteFileA(xml_path);
  return rc;
}
#endif

static int cron_platform_register(
  const char *script,
  const char *schedule,
  const char *title,
  const char *executable,
  char *error,
  size_t error_len
) {
#ifdef _WIN32
  return cron_windows_register(script, schedule, title, executable, error, error_len);
#elif defined(__APPLE__)
  return cron_macos_register(script, schedule, title, executable, error, error_len);
#else
  return cron_linux_register(script, schedule, title, executable, error, error_len);
#endif
}

static int cron_platform_remove(const char *title, char *error, size_t error_len) {
#ifdef _WIN32
  return cron_windows_remove(title, error, error_len);
#elif defined(__APPLE__)
  return cron_macos_remove(title, error, error_len);
#else
  return cron_linux_remove(title, error, error_len);
#endif
}

static ant_value_t cron_settle_promise(ant_t *js, int rc, const char *message) {
  ant_value_t promise = js_mkpromise(js);
  if (rc == 0) js_resolve_promise(js, promise, js_mkundef());
  else js_reject_promise(js, promise, js_mkerr(js, "%s", message));
  return promise;
}

static char *cron_script_candidate(ant_t *js, const char *path, char *error, size_t error_len) {
  bool absolute = path[0] == '/';
#ifdef _WIN32
  absolute = absolute || path[0] == '\\' ||
    (path[0] != '\0' && path[1] != '\0' && isalpha((unsigned char)path[0]) && path[1] == ':');
#endif
  if (absolute) return strdup(path);

  const char *caller = js_module_eval_active_filename(js);
  const char *slash = caller ? strrchr(caller, '/') : NULL;
#ifdef _WIN32
  const char *backslash = caller ? strrchr(caller, '\\') : NULL;
  if (backslash && (!slash || backslash > slash)) slash = backslash;
#endif
  size_t base_len = slash ? (size_t)(slash - caller) : 1;
  const char *base = slash ? caller : ".";
  size_t needed = base_len + 1 + strlen(path) + 1;
  if (needed > PATH_MAX) {
    snprintf(error, error_len, "cron script path is too long");
    return NULL;
  }
  char *candidate = malloc(needed);
  if (!candidate) {
    snprintf(error, error_len, "out of memory");
    return NULL;
  }
  snprintf(candidate, needed, "%.*s/%s", (int)base_len, base, path);
  return candidate;
}

static char *cron_resolve_script_path(const char *path, char *error, size_t error_len) {
#ifdef _WIN32
  char resolved[PATH_MAX];
  if (!_fullpath(resolved, path, sizeof(resolved))) {
    snprintf(error, error_len, "cannot resolve cron script '%s'", path);
    return NULL;
  }
  DWORD attributes = GetFileAttributesA(resolved);
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    snprintf(error, error_len, "cannot resolve cron script '%s'", path);
    return NULL;
  }
  return strdup(resolved);
#else
  char *resolved = realpath(path, NULL);
  if (!resolved)
    snprintf(error, error_len, "cannot resolve cron script '%s': %s", path, strerror(errno));
  return resolved;
#endif
}

static void cron_request_add(cron_os_request_t *request) {
  request->prev = NULL;
  request->next = cron_state.requests;
  if (request->next) request->next->prev = request;
  cron_state.requests = request;
}

static void cron_request_remove(cron_os_request_t *request) {
  if (request->prev) request->prev->next = request->next;
  else cron_state.requests = request->next;
  if (request->next) request->next->prev = request->prev;
}

static void cron_request_free(cron_os_request_t *request) {
  free(request->path);
  free(request->schedule);
  free(request->title);
  free(request->executable);
  free(request);
}

static void cron_request_enqueue(cron_os_request_t *request) {
  request->queue_next = NULL;
  if (cron_state.request_queue_tail)
    cron_state.request_queue_tail->queue_next = request;
  else cron_state.request_queue_head = request;
  cron_state.request_queue_tail = request;
}

static cron_os_request_t *cron_request_dequeue(void) {
  cron_os_request_t *request = cron_state.request_queue_head;
  if (!request) return NULL;
  cron_state.request_queue_head = request->queue_next;
  if (!cron_state.request_queue_head) cron_state.request_queue_tail = NULL;
  request->queue_next = NULL;
  return request;
}

static void cron_request_work(uv_work_t *work) {
  cron_os_request_t *request = work->data;
  if (request->remove) {
    request->rc = cron_platform_remove(
      request->title, request->error, sizeof(request->error)
    );
    return;
  }
  char *script = cron_resolve_script_path(
    request->path, request->error, sizeof(request->error)
  );
  if (!script) {
    request->rc = -1;
    return;
  }
  request->rc = cron_platform_register(
    script, request->schedule, request->title, request->executable,
    request->error, sizeof(request->error)
  );
  free(script);
}

static void cron_start_next_request(void);

static void cron_request_after(uv_work_t *work, int status) {
  cron_os_request_t *request = work->data;
  if (status == UV_ECANCELED) {
    request->rc = -1;
    snprintf(request->error, sizeof(request->error), "cron operation was cancelled");
  }
  if (request->js) {
    if (request->rc == 0)
      js_resolve_promise(request->js, request->promise, js_mkundef());
    else js_reject_promise(
      request->js, request->promise,
      js_mkerr(request->js, "%s", request->error[0] ? request->error : "cron operation failed")
    );
  }
  if (cron_state.active_request == request) cron_state.active_request = NULL;
  cron_request_remove(request);
  cron_request_free(request);
  cron_start_next_request();
}

static void cron_start_next_request(void) {
  while (!cron_state.active_request) {
    cron_os_request_t *request = cron_request_dequeue();
    if (!request) return;
    cron_state.active_request = request;
    int rc = uv_queue_work(
      uv_default_loop(), &request->work, cron_request_work, cron_request_after
    );
    if (rc == 0) return;

    cron_state.active_request = NULL;
    if (request->js) js_reject_promise(
      request->js, request->promise,
      js_mkerr(request->js, "cannot queue cron operation: %s", uv_strerror(rc))
    );
    cron_request_remove(request);
    cron_request_free(request);
  }
}

static ant_value_t cron_queue_request(ant_t *js, cron_os_request_t *request) {
  request->js = js;
  request->promise = js_mkpromise(js);
  request->work.data = request;
  cron_request_add(request);
  cron_request_enqueue(request);
  ant_value_t promise = request->promise;
  cron_start_next_request();
  return promise;
}

static ant_value_t cron_register_os(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3 || vtype(args[0]) != kTypeString || vtype(args[1]) != kTypeString || vtype(args[2]) != kTypeString) {
    if (nargs < 1 || vtype(args[0]) != kTypeString)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Ant.cron() expects a string path as the first argument");
    if (nargs < 2 || vtype(args[1]) != kTypeString)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Ant.cron() expects a string schedule as the second argument");
    return js_mkerr_typed(js, JS_ERR_TYPE, "Ant.cron() expects a string title as the third argument");
  }
  size_t path_len = 0, schedule_len = 0, title_len = 0;
  const char *path_value = js_getstr(js, args[0], &path_len);
  const char *schedule_value = js_getstr(js, args[1], &schedule_len);
  const char *title_value = js_getstr(js, args[2], &title_len);
  char *path_input = strndup(path_value, path_len);
  char *schedule = strndup(schedule_value, schedule_len);
  char *title = strndup(title_value, title_len);
  if (!path_input || !schedule || !title) {
    free(path_input); free(schedule); free(title);
    return js_mkerr(js, "out of memory");
  }
  if (!cron_valid_title(title, title_len)) {
    free(path_input); free(schedule); free(title);
    return js_mkerr_typed(
      js, JS_ERR_TYPE,
      "Cron title must contain only alphanumeric characters, hyphens, and underscores"
    );
  }
  cron_expression_t expression;
  char error[512];
  if (cron_parse_expression(schedule, schedule_len, &expression, error, sizeof(error)) != 0) {
    free(path_input); free(schedule); free(title);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid cron expression: %s", error);
  }
  char *path = cron_script_candidate(js, path_input, error, sizeof(error));
  free(path_input);
  if (!path) {
    free(schedule); free(title);
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s", error);
  }
  char executable[PATH_MAX];
  if (ant_get_exe_path(executable, sizeof(executable), js->runtime.argc, js->runtime.argv) != 0) {
    free(path); free(schedule); free(title);
    return cron_settle_promise(js, -1, "cannot determine the Ant executable path");
  }
  cron_os_request_t *request = calloc(1, sizeof(*request));
  if (!request) {
    free(path); free(schedule); free(title);
    return js_mkerr(js, "out of memory");
  }
  request->path = path;
  request->schedule = schedule;
  request->title = title;
  request->executable = strdup(executable);
  if (!request->executable) {
    cron_request_free(request);
    return js_mkerr(js, "out of memory");
  }
  return cron_queue_request(js, request);
}

static ant_value_t cron_remove_os(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != kTypeString)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Ant.cron.remove() expects a string title");
  size_t title_len = 0;
  const char *title_value = js_getstr(js, args[0], &title_len);
  char *title = strndup(title_value, title_len);
  if (!title) return js_mkerr(js, "out of memory");
  if (!cron_valid_title(title, title_len)) {
    free(title);
    return js_mkerr_typed(
      js, JS_ERR_TYPE,
      "Cron title must contain only alphanumeric characters, hyphens, and underscores"
    );
  }
  cron_os_request_t *request = calloc(1, sizeof(*request));
  if (!request) {
    free(title);
    return js_mkerr(js, "out of memory");
  }
  request->title = title;
  request->remove = true;
  return cron_queue_request(js, request);
}

static ant_value_t cron_call(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Ant.cron() expects a string path as the first argument");
  if (nargs < 3 || (nargs > 1 && is_callable(args[1])))
    return cron_create_job(
      js, args[0], nargs > 1 ? args[1] : js_mkundef(),
      nargs > 2 ? args[2] : js_mkundef()
    );
  return cron_register_os(js, args, nargs);
}

void init_cron_module(ant_t *js) {
  js->builtins.cron_proto = js_mkobj(js);
  js_set_proto_init(js->builtins.cron_proto, js->sym.object_proto);
  js_set_slot(js->builtins.cron_proto, SLOT_DATA, js_get_ctor_proto(js, "Date", 4));
  gc_register_root(&js->builtins.cron_proto);

  ant_value_t cron_getter = js_cfunc_promote(js, js_mkfun(cron_job_get_cron));
  js_set_function_name(js, cron_getter, "get cron", 8);
  js_set_getter_desc(
    js, js_as_obj(js->builtins.cron_proto), "cron", 4,
    cron_getter, JS_DESC_E
  );
  js_set(js, js->builtins.cron_proto, "ref", js_mkfun(cron_job_ref));
  js_set(js, js->builtins.cron_proto, "stop", js_mkfun(cron_job_stop));
  js_set(js, js->builtins.cron_proto, "unref", js_mkfun(cron_job_unref));
  js_set_descriptor(js, js->builtins.cron_proto, "ref", 3, JS_DESC_W | JS_DESC_E);
  js_set_descriptor(js, js->builtins.cron_proto, "stop", 4, JS_DESC_W | JS_DESC_E);
  js_set_descriptor(js, js->builtins.cron_proto, "unref", 5, JS_DESC_W | JS_DESC_E);
  ant_value_t dispose_symbol = get_dispose_sym();
  ant_value_t dispose = js_cfunc_promote(js, js_mkfun_arity(cron_job_dispose, 1));
  js_set_function_name(js, dispose, "dispose", 7);
  js_set_sym(js, js->builtins.cron_proto, dispose_symbol, dispose);
  js_set_sym_descriptor(js, js->builtins.cron_proto, dispose_symbol, JS_DESC_C);
  js_set_sym(
    js, js->builtins.cron_proto, get_toStringTag_sym(), js_mkstr(js, "CronJob", 7)
  );
  js_set_sym_descriptor(js, js->builtins.cron_proto, get_toStringTag_sym(), JS_DESC_C);

  js_set(js, js->Ant, "cron", js_mkfun_arity(cron_call, 3));
  ant_value_t function = js_cfunc_promote(js, js_get(js, js->Ant, "cron"));
  js_set(js, js->Ant, "cron", function);
  js_set_descriptor(js, js_as_obj(js->Ant), "cron", 4, JS_DESC_W | JS_DESC_E);
  js_set(js, function, "parse", js_mkfun_arity(cron_parse, 1));
  js_set(js, function, "remove", js_mkfun_arity(cron_remove_os, 1));
}

void gc_mark_cron(ant_t *js, gc_mark_fn mark) {
  for (cron_job_t *job = cron_state.jobs; job; job = job->next) if (
    job->js == js && !job->stopped
  ) {
    mark(js, job->object);
    mark(js, job->handler);
  }
  for (cron_os_request_t *request = cron_state.requests; request; request = request->next)
    if (request->js == js) mark(js, request->promise);
}

void cleanup_cron_module(ant_t *js) {
  for (cron_job_t *job = cron_state.jobs, *next; job; job = next) {
    next = job->next;
    if (job->js == js) cron_close_job(job);
  }

  cron_os_request_t *previous = NULL;
  cron_os_request_t *request = cron_state.request_queue_head;
  while (request) {
    cron_os_request_t *next = request->queue_next;
    if (request->js == js) {
      if (previous) previous->queue_next = next;
      else cron_state.request_queue_head = next;
      if (cron_state.request_queue_tail == request)
        cron_state.request_queue_tail = previous;
      cron_request_remove(request);
      cron_request_free(request);
    } else {
      previous = request;
    }
    request = next;
  }

  request = cron_state.active_request;
  if (request && request->js == js) {
    request->js = NULL;
    request->promise = js_mkundef();
    uv_cancel((uv_req_t *)&request->work);
  }
}

int cron_run_scheduled_export(
  ant_t *js,
  ant_value_t default_export,
  const char *expression
) {
  cron_expression_t parsed;
  char parse_error[192];
  if (cron_parse_expression(expression, strlen(expression), &parsed, parse_error, sizeof(parse_error)) != 0) {
    fprintf(stderr, "Invalid cron expression: %s\n", parse_error);
    return EXIT_FAILURE;
  }

  int64_t scheduled_time = cron_now_ms();
  if (!is_object_type(default_export)) {
    fprintf(stderr, "Cron module must export a default object with a scheduled() method\n");
    return EXIT_FAILURE;
  }
  ant_value_t handler = js_get(js, default_export, "scheduled");
  if (!is_callable(handler)) {
    fprintf(stderr, "Cron module must export a default object with a scheduled() method\n");
    return EXIT_FAILURE;
  }

  ant_value_t controller = js_mkobj(js);
  js_set(js, controller, "cron", js_mkstr(js, expression, strlen(expression)));
  js_set(js, controller, "type", js_mkstr(js, "scheduled", 9));
  js_set(js, controller, "scheduledTime", js_mknum((double)scheduled_time));
  ant_value_t result = sv_vm_call(
    js->vm, js, handler, default_export, &controller, 1, NULL, false
  );
  if (is_err(result) || js->thrown_exists) {
    if (js->thrown_exists) print_uncaught_throw(js);
    else print_error_value(js, result, js_mkundef(), NULL);
    return EXIT_FAILURE;
  }
  if (vtype(result) == kTypePromise) {
    ant_value_t settled = js_mkundef();
    js_reactor_await_status_t status = js_reactor_blocking_await_promise(
      js, result, &settled, NULL, NULL
    );
    if (status == JS_REACTOR_AWAIT_FULFILLED) return EXIT_SUCCESS;
    if (status == JS_REACTOR_AWAIT_REJECTED)
      print_error_value(js, settled, js_mkundef(), "Uncaught (in promise) ");
    else fprintf(stderr, "Cron scheduled() promise could not be awaited\n");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
