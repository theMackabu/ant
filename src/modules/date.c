#include <compat.h> // IWYU pragma: keep

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <sys/timeb.h>
#else
#include <sys/time.h>
#endif

#include "ant.h"
#include "internal.h"
#include "errors.h"
#include "descriptors.h"
#include "runtime.h"
#include "silver/engine.h"
#include "modules/date.h"
#include "modules/symbol.h"

static const int month_days[] = {
  31, 28, 31, 30, 31, 30,
  31, 31, 30, 31, 30, 31
};

static const char month_names[] =
  "Jan" "Feb" "Mar" "Apr" "May" "Jun"
  "Jul" "Aug" "Sep" "Oct" "Nov" "Dec";

static const char day_names[] =
  "Sun" "Mon" "Tue" "Wed" "Thu" "Fri" "Sat";

static inline double *date_fields_slot(date_fields_t *f, date_field_index_t index) {
switch (index) {
  case DATE_FIELD_YEAR: return &f->year;
  case DATE_FIELD_MONTH: return &f->month;
  case DATE_FIELD_DAY_OF_MONTH: return &f->day;
  case DATE_FIELD_HOUR: return &f->hour;
  case DATE_FIELD_MINUTE: return &f->minute;
  case DATE_FIELD_SECOND: return &f->second;
  case DATE_FIELD_MILLISECOND: return &f->millisecond;
  case DATE_FIELD_DAY_OF_WEEK: return &f->weekday;
  case DATE_FIELD_TZ_MINUTES: return &f->tz_minutes;
  default: return NULL;
}}

static inline double date_fields_get(const date_fields_t *f, date_field_index_t index) {
  double *slot = date_fields_slot((date_fields_t *)f, index);
  return slot ? *slot : 0.0;
}

static inline void date_fields_set(date_fields_t *f, date_field_index_t index, double value) {
  double *slot = date_fields_slot(f, index);
  if (slot) *slot = value;
}

static inline int date_min_int(int a, int b) {
  return (a < b) ? a : b;
}

static inline int to_upper_ascii(int c) {
  if (c >= 'a' && c <= 'z') return c - ('a' - 'A');
  return c;
}

static inline bool date_is_primitive(jsval_t v) {
  uint8_t t = vtype(v);
  return 
    t == T_STR 
    || t == T_NUM
    || t == T_BOOL
    || t == T_NULL
    || t == T_UNDEF
    || t == T_SYMBOL
    || t == T_BIGINT;
}

static bool is_date_instance(ant_t *js, jsval_t value) {
  if (!is_object_type(value)) return false;
  jsval_t date_proto = js_get_ctor_proto(js, "Date", 4);
  
  if (!is_object_type(date_proto)) return false;
  if (value == date_proto) return true;
  
  return proto_chain_contains(js, value, date_proto);
}

static bool date_this_time_value(ant_t *js, jsval_t this_val, double *out, jsval_t *err) {
  if (!is_date_instance(js, this_val)) {
    if (err) *err = js_mkerr_typed(js, JS_ERR_TYPE, "not a Date object");
    return false;
  }
  
  jsval_t t = js_get_slot(js, js_as_obj(this_val), SLOT_DATA);
  *out = (vtype(t) == T_NUM) ? tod(t) : JS_NAN;
  return true;
}

static jsval_t date_set_this_time_value(ant_t *js, jsval_t this_val, double v) {
  if (!is_date_instance(js, this_val)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "not a Date object");
  }
  
  js_set_slot(js, js_as_obj(this_val), SLOT_DATA, tov(v));
  return tov(v);
}

static int64_t math_mod(int64_t a, int64_t b) {
  int64_t m = a % b;
  return m + (m < 0) * b;
}

static int64_t floor_div_int64(int64_t a, int64_t b) {
  int64_t m = a % b;
  return (a - (m + (m < 0) * b)) / b;
}

static double date_timeclip(double t) {
  if (t >= -8.64e15 && t <= 8.64e15) return trunc(t) + 0.0;
  return JS_NAN;
}

static int64_t days_in_year(int64_t y) {
  return 365 + !(y % 4) - !(y % 100) + !(y % 400);
}

static int64_t days_from_year(int64_t y) {
  return 365 * (y - 1970)
    + floor_div_int64(y - 1969, 4)
    - floor_div_int64(y - 1901, 100)
    + floor_div_int64(y - 1601, 400);
}

static int64_t year_from_days(int64_t *days) {
  int64_t y, d1, nd, d = *days;
  y = floor_div_int64(d * 10000, 3652425) + 1970;
  
  while (true) {
    d1 = d - days_from_year(y);
    
    if (d1 < 0) {
      y--;
      d1 += days_in_year(y);
      continue;
    }
    
    nd = days_in_year(y);
    if (d1 < nd) break;
    
    d1 -= nd;
    y++;
  }
  
  *days = d1;
  return y;
}

static int get_timezone_offset(int64_t time_ms) {
#ifdef _WIN32
  DWORD r;
  TIME_ZONE_INFORMATION tzi;
  r = GetTimeZoneInformation(&tzi);
  if (r == TIME_ZONE_ID_INVALID) return 0;
  if (r == TIME_ZONE_ID_DAYLIGHT) return (int)(tzi.Bias + tzi.DaylightBias);
  return (int)tzi.Bias;
#else
  time_t ti;
  struct tm tm_local;

  int64_t time_s = time_ms / 1000;
  if (sizeof(time_t) == 4) {
    if ((time_t)-1 < 0) {
      if (time_s < INT32_MIN) time_s = INT32_MIN;
      else if (time_s > INT32_MAX) time_s = INT32_MAX;
    } else {
      if (time_s < 0) time_s = 0;
      else if (time_s > UINT32_MAX) time_s = UINT32_MAX;
    }
  }

  ti = (time_t)time_s;
  localtime_r(&ti, &tm_local);

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__) || defined(__GLIBC__)
  return (int)(-tm_local.tm_gmtoff / 60);
#else
  struct tm tm_gmt;
  gmtime_r(&ti, &tm_gmt);
  tm_local.tm_isdst = 0;
  return (int)(difftime(mktime(&tm_gmt), mktime(&tm_local)) / 60);
#endif
#endif
}

static int date_extract_fields_from_time(double dval, date_fields_t *fields, int is_local, int force) {
  int64_t d, days, wd, y, i, md, h, m, s, ms, tz = 0;

  if (isnan(dval)) {
    if (!force) return 0;
    d = 0;
  } else {
    d = (int64_t)dval;
    if (is_local) {
      tz = -get_timezone_offset(d);
      d += tz * 60000;
    }
  }

  h = math_mod(d, 86400000);
  days = (d - h) / 86400000;
  ms = h % 1000;
  h = (h - ms) / 1000;
  s = h % 60;
  h = (h - s) / 60;
  m = h % 60;
  h = (h - m) / 60;
  wd = math_mod(days + 4, 7);
  y = year_from_days(&days);

  for (i = 0; i < 11; i++) {
    md = month_days[i];
    if (i == 1) md += (int)days_in_year(y) - 365;
    if (days < md) break;
    days -= md;
  }

  fields->year = (double)y;
  fields->month = (double)i;
  fields->day = (double)(days + 1);
  fields->hour = (double)h;
  fields->minute = (double)m;
  fields->second = (double)s;
  fields->millisecond = (double)ms;
  fields->weekday = (double)wd;
  fields->tz_minutes = (double)tz;
  
  return 1;
}

static int date_extract_fields(
  ant_t *js, jsval_t this_val,
  date_fields_t *fields,
  int is_local, int force, jsval_t *err
) {
  double dval;
  if (!date_this_time_value(js, this_val, &dval, err)) return -1;
  return date_extract_fields_from_time(dval, fields, is_local, force);
}

static double date_make_fields(const date_fields_t *fields, int is_local) {
  double y, m, dt, ym, mn, day, h, s, milli, time, tv;
  int yi, mi, i;
  int64_t days;
  volatile double temp;
  double d = JS_NAN;

  y = fields->year;
  m = fields->month;
  dt = fields->day;

  ym = y + floor(m / 12);
  mn = fmod(m, 12);
  if (mn < 0) mn += 12;

  if (ym < -271821 || ym > 275760) return JS_NAN;

  yi = (int)ym;
  mi = (int)mn;

  days = days_from_year(yi);
  for (i = 0; i < mi; i++) {
    days += month_days[i];
    if (i == 1) days += days_in_year(yi) - 365;
  }
  day = (double)days + dt - 1;

  h = fields->hour;
  m = fields->minute;
  s = fields->second;
  milli = fields->millisecond;

  time = h * 3600000;
  time += (temp = m * 60000);
  time += (temp = s * 1000);
  time += milli;

  tv = (temp = day * 86400000) + time;
  if (!isfinite(tv)) return JS_NAN;

  if (is_local) {
    int64_t ti;
    if (tv < (double)INT64_MIN) ti = INT64_MIN;
    else if (tv >= 0x1p63) ti = INT64_MAX;
    else ti = (int64_t)tv;
    tv += (double)get_timezone_offset(ti) * 60000.0;
  }

  d = date_timeclip(tv);
  return d;
}

jsval_t get_date_string(ant_t *js, jsval_t this_val, date_string_spec_t spec) {
  char buf[96];
  date_fields_t fields;
  
  int res, fmt, pos;
  int y, mon, d, h, m, s, ms, wd, tz;
  jsval_t err;

  fmt = (int)spec.fmt;
  res = date_extract_fields(
    js, this_val,
    &fields, spec.fmt == DATE_STRING_FMT_LOCAL,
    0, &err
  );
  
  if (res < 0) return err;
  if (!res) {
    if (spec.fmt == DATE_STRING_FMT_ISO) return js_mkerr_typed(js, JS_ERR_RANGE, "Date value is NaN");
    return js_mkstr(js, "Invalid Date", 12);
  }

  y = (int)fields.year;
  mon = (int)fields.month;
  d = (int)fields.day;
  h = (int)fields.hour;
  m = (int)fields.minute;
  s = (int)fields.second;
  ms = (int)fields.millisecond;
  wd = (int)fields.weekday;
  tz = (int)fields.tz_minutes;
  pos = 0;

  if (spec.part & DATE_STRING_PART_DATE) {
    switch (fmt) {
      case 0:
        pos += snprintf(
          buf + pos, sizeof(buf) - (size_t)pos,
          "%.3s, %02d %.3s %0*d ",
          day_names + wd * 3, d, month_names + mon * 3, 4 + (y < 0), y);
        break;
      case 1:
        pos += snprintf(
          buf + pos, sizeof(buf) - (size_t)pos,
          "%.3s %.3s %02d %0*d",
          day_names + wd * 3, month_names + mon * 3, d, 4 + (y < 0), y);
        if (spec.part == DATE_STRING_PART_ALL) buf[pos++] = ' ';
        break;
      case 2:
        if (y >= 0 && y <= 9999) {
          pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%04d", y);
        } else pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%+07d", y);
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "-%02d-%02dT", mon + 1, d);
        break;
      case 3:
        pos += snprintf(
          buf + pos, sizeof(buf) - (size_t)pos,
          "%02d/%02d/%0*d", mon + 1, d, 4 + (y < 0), y);
        if (spec.part == DATE_STRING_PART_ALL) {
          buf[pos++] = ',';
          buf[pos++] = ' ';
        }
        break;
      default: break;
    }
  }

  if (spec.part & DATE_STRING_PART_TIME) {
    switch (fmt) {
      case 0:
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%02d:%02d:%02d GMT", h, m, s);
        break;
      case 1:
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%02d:%02d:%02d GMT", h, m, s);
        if (tz < 0) {
          buf[pos++] = '-';
          tz = -tz;
        } else buf[pos++] = '+';
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%02d%02d", tz / 60, tz % 60);
        break;
      case 2:
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%02d:%02d:%02d.%03dZ", h, m, s, ms);
        break;
      case 3:
        pos += snprintf(
          buf + pos, sizeof(buf) - (size_t)pos,
          "%02d:%02d:%02d %cM", (h + 11) % 12 + 1, m, s, (h < 12) ? 'A' : 'P');
        break;
      default: break;
    }
  }

  if (pos <= 0) return js_mkstr(js, "", 0);
  return js_mkstr(js, buf, (size_t)pos);
}

static int64_t date_now(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + (int64_t)(tv.tv_usec / 1000);
}

static bool string_skip_char(const uint8_t *sp, int *pp, int c) {
  if (sp[*pp] == c) { *pp += 1; return true; }
  return false;
}

static int string_skip_spaces(const uint8_t *sp, int *pp) {
  int c;
  while ((c = sp[*pp]) == ' ') *pp += 1;
  return c;
}

static int string_skip_separators(const uint8_t *sp, int *pp) {
  int c;
  while ((c = sp[*pp]) == '-' || c == '/' || c == '.' || c == ',') *pp += 1;
  return c;
}

static int string_skip_until(const uint8_t *sp, int *pp, const char *stoplist) {
  int c;
  while (!strchr(stoplist, c = sp[*pp])) *pp += 1;
  return c;
}

static bool string_get_digits(const uint8_t *sp, int *pp, int *pval, int min_digits, int max_digits) {
  int v = 0; int c;
  int p = *pp;
  int p_start = p;

  while ((c = sp[p]) >= '0' && c <= '9') {
    if (v >= 100000000) return false;
    v = v * 10 + c - '0';
    p++;
    if (max_digits > 0 && p - p_start == max_digits) break;
  }

  if (p - p_start < min_digits) return false;
  *pval = v;
  *pp = p;
  
  return true;
}

static bool string_get_milliseconds(const uint8_t *sp, int *pp, int *pval) {
  int mul = 100; int ms = 0;
  int c; int p_start; int p = *pp;

  c = sp[p];
  if (c == '.' || c == ',') {
    p++;
    p_start = p;
    
    while ((c = sp[p]) >= '0' && c <= '9') {
      ms += (c - '0') * mul;
      mul /= 10;
      p++;
      if (p - p_start == 9) break;
    }
    
    if (p > p_start) {
      *pval = ms;
      *pp = p;
    }
  }
  
  return true;
}

static bool string_get_tzoffset(const uint8_t *sp, int *pp, int *tzp, bool strict) {
  int tz = 0; int p = *pp;
  int sgn; int hh; int mm;

  sgn = sp[p++];
  if (sgn == '+' || sgn == '-') {
    int n = p;
    if (!string_get_digits(sp, &p, &hh, 1, 0)) return false;
    n = p - n;
    if (strict && n != 2 && n != 4) return false;

    while (n > 4) {
      n -= 2;
      hh /= 100;
    }
    
    if (n > 2) {
      mm = hh % 100;
      hh = hh / 100;
    } else {
      mm = 0;
      if (string_skip_char(sp, &p, ':') && !string_get_digits(sp, &p, &mm, 2, 2)) return false;
    }
    
    if (hh > 23 || mm > 59) return false;
    
    tz = hh * 60 + mm;
    if (sgn != '+') tz = -tz;
  } else if (sgn != 'Z') return false;

  *pp = p;
  *tzp = tz;
  
  return true;
}

static bool string_match(const uint8_t *sp, int *pp, const char *s) {
  int p = *pp;
  
  while (*s != '\0') {
    if (to_upper_ascii(sp[p]) != to_upper_ascii(*s++)) return false;
    p++;
  }
  
  *pp = p;
  return true;
}

static int find_abbrev(const uint8_t *sp, int p, const char *list, int count) {
  int n; int i;
  for (n = 0; n < count; n++) {
    for (i = 0;; i++) {
      if (to_upper_ascii(sp[p + i]) != to_upper_ascii(list[n * 3 + i])) break;
      if (i == 2) return n;
    }
  }
  return -1;
}

static bool string_get_month(const uint8_t *sp, int *pp, int *pval) {
  int n = find_abbrev(sp, *pp, month_names, 12);
  if (n < 0) return false;
  *pval = n + 1;
  *pp += 3;
  return true;
}

static bool parse_isostring(const uint8_t *sp, int fields[9], bool *is_local) {
  int sgn;
  int i;
  int p = 0;

  for (i = 0; i < 9; i++) fields[i] = (i == 2);
  *is_local = false;

  sgn = sp[p];
  if (sgn == '-' || sgn == '+') {
    p++;
    if (!string_get_digits(sp, &p, &fields[0], 6, 6)) return false;
    if (sgn == '-') {
      if (fields[0] == 0) return false;
      fields[0] = -fields[0];
    }
  } else {
    if (!string_get_digits(sp, &p, &fields[0], 4, 4)) return false;
  }

  if (string_skip_char(sp, &p, '-')) {
    if (!string_get_digits(sp, &p, &fields[1], 2, 2)) return false;
    if (fields[1] < 1) return false;
    fields[1] -= 1;

    if (string_skip_char(sp, &p, '-')) {
      if (!string_get_digits(sp, &p, &fields[2], 2, 2)) return false;
      if (fields[2] < 1) return false;
    }
  }

  if (string_skip_char(sp, &p, 'T')) {
    *is_local = true;
    if (!string_get_digits(sp, &p, &fields[3], 2, 2)
        || !string_skip_char(sp, &p, ':')
        || !string_get_digits(sp, &p, &fields[4], 2, 2)) {
      fields[3] = 100;
      return true;
    }

    if (string_skip_char(sp, &p, ':')) {
      if (!string_get_digits(sp, &p, &fields[5], 2, 2)) return false;
      string_get_milliseconds(sp, &p, &fields[6]);
    }
  }

  if (sp[p]) {
    *is_local = false;
    if (!string_get_tzoffset(sp, &p, &fields[8], true)) return false;
  }

  return sp[p] == '\0';
}

typedef struct {
  char name[6];
  int16_t offset;
} tzabbr_t;

static const tzabbr_t js_tzabbr[] = {
  {"GMT", 0}, {"UTC", 0}, {"UT", 0}, {"Z", 0},
  {"EDT", -4 * 60}, {"EST", -5 * 60},
  {"CDT", -5 * 60}, {"CST", -6 * 60},
  {"MDT", -6 * 60}, {"MST", -7 * 60},
  {"PDT", -7 * 60}, {"PST", -8 * 60},
  {"WET", 0},       {"WEST", +1 * 60},
  {"CET", +1 * 60}, {"CEST", +2 * 60},
  {"EET", +2 * 60}, {"EEST", +3 * 60},
};

static bool string_get_tzabbr(const uint8_t *sp, int *pp, int *offset) {
  for (int i = 0; i < DATE_COUNT_OF(js_tzabbr); i++) {
    if (string_match(sp, pp, js_tzabbr[i].name)) {
      *offset = js_tzabbr[i].offset;
      return true;
    }
  }
  return false;
}

static bool parse_otherstring(const uint8_t *sp, int fields[9], bool *is_local) {
  int c; int i; int val;
  int p = 0;
  int p_start;
  int num[3];
  
  bool has_year = false;
  bool has_mon = false;
  bool has_time = false;
  int num_index = 0;

  fields[0] = 2001;
  fields[1] = 1;
  fields[2] = 1;
  
  for (i = 3; i < 9; i++) fields[i] = 0;
  *is_local = true;

  while (string_skip_spaces(sp, &p)) {
    p_start = p;

    if ((c = sp[p]) == '+' || c == '-') {
      if (has_time && string_get_tzoffset(sp, &p, &fields[8], false)) {
        *is_local = false;
      } else {
        p++;
        if (string_get_digits(sp, &p, &val, 1, 0)) {
          if (c == '-') {
            if (val == 0) return false;
            val = -val;
          }
          fields[0] = val;
          has_year = true;
        }
      }
    } else if (string_get_digits(sp, &p, &val, 1, 0)) {
      if (string_skip_char(sp, &p, ':')) {
        fields[3] = val;
        if (!string_get_digits(sp, &p, &fields[4], 1, 2)) return false;

        if (string_skip_char(sp, &p, ':')) {
          if (!string_get_digits(sp, &p, &fields[5], 1, 2)) return false;
          string_get_milliseconds(sp, &p, &fields[6]);
        } else if (sp[p] != '\0' && sp[p] != ' ') return false;

        has_time = true;
      } else {
        if (p - p_start > 2) {
          fields[0] = val;
          has_year = true;
        } else if (val < 1 || val > 31) {
          fields[0] = val + (val < 100) * 1900 + (val < 50) * 100;
          has_year = true;
        } else {
          if (num_index == 3) return false;
          num[num_index++] = val;
        }
      }
    } else if (string_get_month(sp, &p, &fields[1])) {
      has_mon = true;
      string_skip_until(sp, &p, "0123456789 -/(");
    } else if (has_time && string_match(sp, &p, "PM")) {
      if (fields[3] != 12) fields[3] += 12;
      continue;
    } else if (has_time && string_match(sp, &p, "AM")) {
      if (fields[3] > 12) return false;
      if (fields[3] == 12) fields[3] -= 12;
      continue;
    } else if (string_get_tzabbr(sp, &p, &fields[8])) {
      *is_local = false;
      continue;
    } else if (c == '(') {
      int level = 0;
      while ((c = sp[p]) != '\0') {
        p++;
        level += (c == '(');
        level -= (c == ')');
        if (!level) break;
      }
      if (level > 0) return false;
    } else if (c == ')') {
      return false;
    } else {
      if ((int)has_year + (int)has_mon + (int)has_time + num_index) return false;
      string_skip_until(sp, &p, " -/(");
    }

    string_skip_separators(sp, &p);
  }

  if (num_index + (int)has_year + (int)has_mon > 3) return false;

  switch (num_index) {
    case 0:
      if (!has_year) return false;
      break;
    case 1:
      if (has_mon) fields[2] = num[0];
      else fields[1] = num[0];
      break;
    case 2:
      if (has_year) {
        fields[1] = num[0];
        fields[2] = num[1];
      } else if (has_mon) {
        fields[0] = num[1] + (num[1] < 100) * 1900 + (num[1] < 50) * 100;
        fields[2] = num[0];
      } else {
        fields[1] = num[0];
        fields[2] = num[1];
      }
      break;
    case 3:
      fields[0] = num[2] + (num[2] < 100) * 1900 + (num[2] < 50) * 100;
      fields[1] = num[0];
      fields[2] = num[1];
      break;
    default: return false;
  }

  if (fields[1] < 1 || fields[2] < 1) return false;
  fields[1] -= 1;
  
  return true;
}

static bool date_parse_string_to_ms(ant_t *js, jsval_t arg, double *out_ms) {
  jsval_t s = coerce_to_str(js, arg);
  if (is_err(s)) return false;

  jsoff_t len = 0;
  jsoff_t off = vstr(js, s, &len);

  char *buf = (char *)malloc((size_t)len + 1);
  if (!buf) {
    *out_ms = JS_NAN;
    return true;
  }

  memcpy(buf, &js->mem[off], (size_t)len);
  buf[len] = '\0';

  int fields[9];
  date_fields_t fields1 = {0};
  bool is_local = false;
  bool ok = parse_isostring((const uint8_t *)buf, fields, &is_local)
         || parse_otherstring((const uint8_t *)buf, fields, &is_local);

  free(buf);

  if (!ok) {
    *out_ms = JS_NAN;
    return true;
  }

  static const int field_max[6] = {0, 11, 31, 24, 59, 59};
  bool valid = true;

  for (int i = 1; i < 6; i++) {
    if (fields[i] > field_max[i]) valid = false;
  }
  if (fields[3] == 24 && (fields[4] | fields[5] | fields[6])) valid = false;

  if (!valid) {
    *out_ms = JS_NAN;
    return true;
  }

  fields1.year = (double)fields[0];
  fields1.month = (double)fields[1];
  fields1.day = (double)fields[2];
  fields1.hour = (double)fields[3];
  fields1.minute = (double)fields[4];
  fields1.second = (double)fields[5];
  fields1.millisecond = (double)fields[6];
  *out_ms = date_make_fields(&fields1, is_local ? 1 : 0) - (double)fields[8] * 60000.0;
  return true;
}

static jsval_t builtin_Date(ant_t *js, jsval_t *args, int nargs) {
  double val;
  static const date_string_spec_t kDateToStringSpec = {
    DATE_STRING_FMT_LOCAL,
    DATE_STRING_PART_ALL
  };

  if (vtype(js->new_target) == T_UNDEF) {
    val = (double)date_now();
    jsval_t tmp = js_mkobj(js);
    jsval_t date_proto = js_get_ctor_proto(js, "Date", 4);
    if (is_object_type(date_proto)) js_set_proto(js, tmp, date_proto);
    js_set_slot(js, tmp, SLOT_DATA, tov(val));
    return get_date_string(js, tmp, kDateToStringSpec);
  }

  if (nargs == 0) {
    val = (double)date_now();
  } else if (nargs == 1) {
    jsval_t input = args[0];

    if (is_object_type(input) && is_date_instance(js, input)) {
      jsval_t tv = js_get_slot(js, js_as_obj(input), SLOT_DATA);
      val = (vtype(tv) == T_NUM) ? tod(tv) : JS_NAN;
      val = date_timeclip(val);
    } else {
      jsval_t prim = js_to_primitive(js, input, 0);
      if (is_err(prim)) return prim;
      if (vtype(prim) == T_STR) {
        if (!date_parse_string_to_ms(js, prim, &val)) return js_mkerr_typed(js, JS_ERR_INTERNAL, "Date parse failed");
      } else {
        val = js_to_number(js, prim);
      }
      val = date_timeclip(val);
    }
  } else {
    date_fields_t fields = {0};
    fields.day = 1;
    int n = nargs;
    if (n > 7) n = 7;

    int i;
    for (i = 0; i < n; i++) {
      double a = js_to_number(js, args[i]);
      if (!isfinite(a)) break;
      double t = trunc(a);
      date_fields_set(&fields, (date_field_index_t)i, t);
      if (i == 0 && fields.year >= 0 && fields.year < 100) fields.year += 1900;
    }

    val = (i == n) ? date_make_fields(&fields, 1) : JS_NAN;
  }

  js_set_slot(js, js_as_obj(js->this_val), SLOT_DATA, tov(val));
  return js->this_val;
}

static jsval_t builtin_Date_UTC(ant_t *js, jsval_t *args, int nargs) {
  if (nargs == 0) return tov(JS_NAN);

  date_fields_t fields = {0};
  fields.day = 1;
  int n = nargs;
  if (n > 7) n = 7;

  for (int i = 0; i < n; i++) {
    double a = js_to_number(js, args[i]);
    if (!isfinite(a)) return tov(JS_NAN);
    double t = trunc(a);
    date_fields_set(&fields, (date_field_index_t)i, t);
    if (i == 0 && fields.year >= 0 && fields.year < 100) fields.year += 1900;
  }

  return tov(date_make_fields(&fields, 0));
}

static jsval_t builtin_Date_parse(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return tov(JS_NAN);
  double v;
  if (!date_parse_string_to_ms(js, args[0], &v)) {
    return js_mkerr_typed(js, JS_ERR_INTERNAL, "Date parse failed");
  }
  return tov(v);
}

static jsval_t builtin_Date_now(ant_t *js, jsval_t *args, int nargs) {
  return tov((double)date_now());
}

static jsval_t date_get_field(ant_t *js, date_get_field_spec_t spec) {
  date_fields_t fields;
  jsval_t err;

  int res = date_extract_fields(js, js->this_val, &fields, spec.local_time, 0, &err);
  if (res < 0) return err;
  if (!res) return tov(JS_NAN);

  if (spec.legacy_get_year) fields.year -= 1900;
  return tov(date_fields_get(&fields, spec.field));
}

static jsval_t date_set_field(ant_t *js, jsval_t *args, int nargs, date_set_field_spec_t spec) {
  date_fields_t fields;
  jsval_t err;
  double d = JS_NAN;

  int res = date_extract_fields(
    js, js->this_val,
    &fields, spec.local_time,
    spec.first_field == DATE_FIELD_YEAR, &err
  );
  
  if (res < 0) return err;
  int n = date_min_int(nargs, spec.end_field_exclusive - spec.first_field);
  
  for (int i = 0; i < n; i++) {
    double a = js_to_number(js, args[i]);
    if (!isfinite(a)) return date_set_this_time_value(js, js->this_val, JS_NAN);
    date_fields_set(&fields, (date_field_index_t)(spec.first_field + i), trunc(a));
  }

  if (!res) return tov(JS_NAN);
  if (nargs > 0) d = date_make_fields(&fields, spec.local_time);

  return date_set_this_time_value(js, js->this_val, d);
}

static jsval_t builtin_Date_valueOf(ant_t *js, jsval_t *args, int nargs) {
  double v;
  jsval_t err;
  if (!date_this_time_value(js, js->this_val, &v, &err)) return err;
  return tov(v);
}

static jsval_t builtin_Date_getTime(ant_t *js, jsval_t *args, int nargs) {
  return builtin_Date_valueOf(js, args, nargs);
}

static jsval_t builtin_Date_toUTCString(ant_t *js, jsval_t *args, int nargs) {
  static const date_string_spec_t kSpec = {DATE_STRING_FMT_UTC, DATE_STRING_PART_ALL};
  return get_date_string(js, js->this_val, kSpec);
}

static jsval_t builtin_Date_toString(ant_t *js, jsval_t *args, int nargs) {
  static const date_string_spec_t kSpec = {DATE_STRING_FMT_LOCAL, DATE_STRING_PART_ALL};
  return get_date_string(js, js->this_val, kSpec);
}

static jsval_t builtin_Date_toISOString(ant_t *js, jsval_t *args, int nargs) {
  static const date_string_spec_t kSpec = {DATE_STRING_FMT_ISO, DATE_STRING_PART_ALL};
  return get_date_string(js, js->this_val, kSpec);
}

static jsval_t builtin_Date_toDateString(ant_t *js, jsval_t *args, int nargs) {
  static const date_string_spec_t kSpec = {DATE_STRING_FMT_LOCAL, DATE_STRING_PART_DATE};
  return get_date_string(js, js->this_val, kSpec);
}

static jsval_t builtin_Date_toTimeString(ant_t *js, jsval_t *args, int nargs) {
  static const date_string_spec_t kSpec = {DATE_STRING_FMT_LOCAL, DATE_STRING_PART_TIME};
  return get_date_string(js, js->this_val, kSpec);
}

static jsval_t builtin_Date_toLocaleString(ant_t *js, jsval_t *args, int nargs) {
  static const date_string_spec_t kSpec = {DATE_STRING_FMT_LOCALE, DATE_STRING_PART_ALL};
  return get_date_string(js, js->this_val, kSpec);
}

static jsval_t builtin_Date_toLocaleDateString(ant_t *js, jsval_t *args, int nargs) {
  static const date_string_spec_t kSpec = {DATE_STRING_FMT_LOCALE, DATE_STRING_PART_DATE};
  return get_date_string(js, js->this_val, kSpec);
}

static jsval_t builtin_Date_toLocaleTimeString(ant_t *js, jsval_t *args, int nargs) {
  static const date_string_spec_t kSpec = {DATE_STRING_FMT_LOCALE, DATE_STRING_PART_TIME};
  return get_date_string(js, js->this_val, kSpec);
}

static jsval_t builtin_Date_getTimezoneOffset(ant_t *js, jsval_t *args, int nargs) {
  double v;
  jsval_t err;
  if (!date_this_time_value(js, js->this_val, &v, &err)) return err;
  if (isnan(v)) return tov(JS_NAN);
  return tov((double)get_timezone_offset((int64_t)trunc(v)));
}

static jsval_t builtin_Date_setTime(ant_t *js, jsval_t *args, int nargs) {
  double cur;
  jsval_t err;
  if (!date_this_time_value(js, js->this_val, &cur, &err)) return err;

  double v = (nargs > 0) ? js_to_number(js, args[0]) : JS_NAN;
  return date_set_this_time_value(js, js->this_val, date_timeclip(v));
}

static jsval_t builtin_Date_getYear(ant_t *js, jsval_t *args, int nargs) {
  static const date_get_field_spec_t kSpec = {DATE_FIELD_YEAR, true, true};
  return date_get_field(js, kSpec);
}

static jsval_t builtin_Date_getFullYear(ant_t *js, jsval_t *args, int nargs) {
  static const date_get_field_spec_t kSpec = {DATE_FIELD_YEAR, true, false};
  return date_get_field(js, kSpec);
}

static jsval_t builtin_Date_getUTCFullYear(ant_t *js, jsval_t *args, int nargs) {
  static const date_get_field_spec_t kSpec = {DATE_FIELD_YEAR, false, false};
  return date_get_field(js, kSpec);
}

static jsval_t builtin_Date_getMonth(ant_t *js, jsval_t *args, int nargs) {
  static const date_get_field_spec_t kSpec = {DATE_FIELD_MONTH, true, false};
  return date_get_field(js, kSpec);
}

static jsval_t builtin_Date_getUTCMonth(ant_t *js, jsval_t *args, int nargs) {
  static const date_get_field_spec_t kSpec = {DATE_FIELD_MONTH, false, false};
  return date_get_field(js, kSpec);
}

static jsval_t builtin_Date_getDate(ant_t *js, jsval_t *args, int nargs) {
  static const date_get_field_spec_t kSpec = {DATE_FIELD_DAY_OF_MONTH, true, false};
  return date_get_field(js, kSpec);
}

static jsval_t builtin_Date_getUTCDate(ant_t *js, jsval_t *args, int nargs) {
  static const date_get_field_spec_t kSpec = {DATE_FIELD_DAY_OF_MONTH, false, false};
  return date_get_field(js, kSpec);
}

static jsval_t builtin_Date_getHours(ant_t *js, jsval_t *args, int nargs) {
  static const date_get_field_spec_t kSpec = {DATE_FIELD_HOUR, true, false};
  return date_get_field(js, kSpec);
}

static jsval_t builtin_Date_getUTCHours(ant_t *js, jsval_t *args, int nargs) {
  static const date_get_field_spec_t kSpec = {DATE_FIELD_HOUR, false, false};
  return date_get_field(js, kSpec);
}

static jsval_t builtin_Date_getMinutes(ant_t *js, jsval_t *args, int nargs) {
  static const date_get_field_spec_t kSpec = {DATE_FIELD_MINUTE, true, false};
  return date_get_field(js, kSpec);
}

static jsval_t builtin_Date_getUTCMinutes(ant_t *js, jsval_t *args, int nargs) {
  static const date_get_field_spec_t kSpec = {DATE_FIELD_MINUTE, false, false};
  return date_get_field(js, kSpec);
}

static jsval_t builtin_Date_getSeconds(ant_t *js, jsval_t *args, int nargs) {
  static const date_get_field_spec_t kSpec = {DATE_FIELD_SECOND, true, false};
  return date_get_field(js, kSpec);
}

static jsval_t builtin_Date_getUTCSeconds(ant_t *js, jsval_t *args, int nargs) {
  static const date_get_field_spec_t kSpec = {DATE_FIELD_SECOND, false, false};
  return date_get_field(js, kSpec);
}

static jsval_t builtin_Date_getMilliseconds(ant_t *js, jsval_t *args, int nargs) {
  static const date_get_field_spec_t kSpec = {DATE_FIELD_MILLISECOND, true, false};
  return date_get_field(js, kSpec);
}

static jsval_t builtin_Date_getUTCMilliseconds(ant_t *js, jsval_t *args, int nargs) {
  static const date_get_field_spec_t kSpec = {DATE_FIELD_MILLISECOND, false, false};
  return date_get_field(js, kSpec);
}

static jsval_t builtin_Date_getDay(ant_t *js, jsval_t *args, int nargs) {
  static const date_get_field_spec_t kSpec = {DATE_FIELD_DAY_OF_WEEK, true, false};
  return date_get_field(js, kSpec);
}

static jsval_t builtin_Date_getUTCDay(ant_t *js, jsval_t *args, int nargs) {
  static const date_get_field_spec_t kSpec = {DATE_FIELD_DAY_OF_WEEK, false, false};
  return date_get_field(js, kSpec);
}

static jsval_t builtin_Date_setMilliseconds(ant_t *js, jsval_t *args, int nargs) {
  static const date_set_field_spec_t kSpec = {DATE_FIELD_MILLISECOND, DATE_FIELD_DAY_OF_WEEK, true};
  return date_set_field(js, args, nargs, kSpec);
}

static jsval_t builtin_Date_setUTCMilliseconds(ant_t *js, jsval_t *args, int nargs) {
  static const date_set_field_spec_t kSpec = {DATE_FIELD_MILLISECOND, DATE_FIELD_DAY_OF_WEEK, false};
  return date_set_field(js, args, nargs, kSpec);
}

static jsval_t builtin_Date_setSeconds(ant_t *js, jsval_t *args, int nargs) {
  static const date_set_field_spec_t kSpec = {DATE_FIELD_SECOND, DATE_FIELD_DAY_OF_WEEK, true};
  return date_set_field(js, args, nargs, kSpec);
}

static jsval_t builtin_Date_setUTCSeconds(ant_t *js, jsval_t *args, int nargs) {
  static const date_set_field_spec_t kSpec = {DATE_FIELD_SECOND, DATE_FIELD_DAY_OF_WEEK, false};
  return date_set_field(js, args, nargs, kSpec);
}

static jsval_t builtin_Date_setMinutes(ant_t *js, jsval_t *args, int nargs) {
  static const date_set_field_spec_t kSpec = {DATE_FIELD_MINUTE, DATE_FIELD_DAY_OF_WEEK, true};
  return date_set_field(js, args, nargs, kSpec);
}

static jsval_t builtin_Date_setUTCMinutes(ant_t *js, jsval_t *args, int nargs) {
  static const date_set_field_spec_t kSpec = {DATE_FIELD_MINUTE, DATE_FIELD_DAY_OF_WEEK, false};
  return date_set_field(js, args, nargs, kSpec);
}

static jsval_t builtin_Date_setHours(ant_t *js, jsval_t *args, int nargs) {
  static const date_set_field_spec_t kSpec = {DATE_FIELD_HOUR, DATE_FIELD_DAY_OF_WEEK, true};
  return date_set_field(js, args, nargs, kSpec);
}

static jsval_t builtin_Date_setUTCHours(ant_t *js, jsval_t *args, int nargs) {
  static const date_set_field_spec_t kSpec = {DATE_FIELD_HOUR, DATE_FIELD_DAY_OF_WEEK, false};
  return date_set_field(js, args, nargs, kSpec);
}

static jsval_t builtin_Date_setDate(ant_t *js, jsval_t *args, int nargs) {
  static const date_set_field_spec_t kSpec = {DATE_FIELD_DAY_OF_MONTH, DATE_FIELD_HOUR, true};
  return date_set_field(js, args, nargs, kSpec);
}

static jsval_t builtin_Date_setUTCDate(ant_t *js, jsval_t *args, int nargs) {
  static const date_set_field_spec_t kSpec = {DATE_FIELD_DAY_OF_MONTH, DATE_FIELD_HOUR, false};
  return date_set_field(js, args, nargs, kSpec);
}

static jsval_t builtin_Date_setMonth(ant_t *js, jsval_t *args, int nargs) {
  static const date_set_field_spec_t kSpec = {DATE_FIELD_MONTH, DATE_FIELD_DAY_OF_MONTH, true};
  return date_set_field(js, args, nargs, kSpec);
}

static jsval_t builtin_Date_setUTCMonth(ant_t *js, jsval_t *args, int nargs) {
  static const date_set_field_spec_t kSpec = {DATE_FIELD_MONTH, DATE_FIELD_DAY_OF_MONTH, false};
  return date_set_field(js, args, nargs, kSpec);
}

static jsval_t builtin_Date_setFullYear(ant_t *js, jsval_t *args, int nargs) {
  static const date_set_field_spec_t kSpec = {DATE_FIELD_YEAR, DATE_FIELD_HOUR, true};
  return date_set_field(js, args, nargs, kSpec);
}

static jsval_t builtin_Date_setUTCFullYear(ant_t *js, jsval_t *args, int nargs) {
  static const date_set_field_spec_t kSpec = {DATE_FIELD_YEAR, DATE_FIELD_HOUR, false};
  return date_set_field(js, args, nargs, kSpec);
}

static jsval_t builtin_Date_setYear(ant_t *js, jsval_t *args, int nargs) {
  double y;
  jsval_t err;
  if (!date_this_time_value(js, js->this_val, &y, &err)) return err;

  y = (nargs > 0) ? js_to_number(js, args[0]) : JS_NAN;
  if (isnan(y)) return date_set_this_time_value(js, js->this_val, y);

  if (isfinite(y)) {
    y = trunc(y);
    if (y >= 0 && y < 100) y += 1900;
  }

  jsval_t darg = tov(y);
  static const date_set_field_spec_t kSetYearSpec = {
    DATE_FIELD_YEAR, DATE_FIELD_MONTH, true
  };
  return date_set_field(js, &darg, 1, kSetYearSpec);
}

static jsval_t date_to_primitive_number(ant_t *js, jsval_t obj) {
  jsval_t to_primitive_sym = get_toPrimitive_sym();
  if (vtype(to_primitive_sym) == T_SYMBOL) {
    jsval_t ex = js_get_sym(js, obj, to_primitive_sym);
    uint8_t et = vtype(ex);
    if (et == T_FUNC || et == T_CFUNC) {
      jsval_t hint = js_mkstr(js, "number", 6);
      jsval_t result = sv_vm_call(js->vm, js, ex, obj, &hint, 1, NULL, false);
      if (is_err(result)) return result;
      if (date_is_primitive(result)) return result;
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert object to primitive value");
    }
    if (et != T_UNDEF) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "Symbol.toPrimitive is not a function");
    }
  }

  const char *methods[2] = {"valueOf", "toString"};
  for (int i = 0; i < 2; i++) {
    jsval_t method = js_getprop_fallback(js, obj, methods[i]);
    uint8_t mt = vtype(method);
    if (mt == T_FUNC || mt == T_CFUNC) {
      jsval_t result = sv_vm_call(js->vm, js, method, obj, NULL, 0, NULL, false);
      if (is_err(result)) return result;
      if (date_is_primitive(result)) return result;
    }
  }

  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert object to primitive value");
}

static jsval_t builtin_Date_toJSON(ant_t *js, jsval_t *args, int nargs) {
  jsval_t obj = js->this_val;
  if (!is_object_type(obj)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Date.prototype.toJSON called on non-object");
  }

  jsval_t tv = date_to_primitive_number(js, obj);
  if (is_err(tv)) return tv;

  if (vtype(tv) == T_NUM && !isfinite(tod(tv))) {
    return js_mknull();
  }

  jsval_t method = js_getprop_fallback(js, obj, "toISOString");
  uint8_t mt = vtype(method);
  if (mt != T_FUNC && mt != T_CFUNC) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "object needs toISOString method");
  }

  return sv_vm_call(js->vm, js, method, obj, NULL, 0, NULL, false);
}

static jsval_t date_toPrimitive(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_val = js_getthis(js);
  if (!is_object_type(this_val)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Not an object");
  }

  int hint_num;
  if (nargs > 0 && vtype(args[0]) == T_STR) {
    size_t hint_len = 0;
    const char *hint = js_getstr(js, args[0], &hint_len);
    if (!hint) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid hint");

    if ((hint_len == 6 && memcmp(hint, "number", 6) == 0)
        || (hint_len == 7 && memcmp(hint, "integer", 7) == 0)) {
      hint_num = 1;
    } else if (
      (hint_len == 6 && memcmp(hint, "string", 6) == 0)
      || (hint_len == 7 && memcmp(hint, "default", 7) == 0)) {
      hint_num = 0;
    } else {
      return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid hint");
    }
  } else return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid hint");

  const char *methods0[2] = {"toString", "valueOf"};
  const char *methods1[2] = {"valueOf", "toString"};
  const char **methods = hint_num ? methods1 : methods0;

  for (int i = 0; i < 2; i++) {
    jsval_t method = js_getprop_fallback(js, this_val, methods[i]);
    uint8_t mt = vtype(method);
    if (mt == T_FUNC || mt == T_CFUNC) {
      jsval_t result = sv_vm_call(js->vm, js, method, this_val, NULL, 0, NULL, false);
      if (is_err(result)) return result;
      if (date_is_primitive(result)) return result;
    }
  }

  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert object to primitive value");
}

static void date_define_methods(
  ant_t *js, jsval_t target,
  const date_method_entry_t *methods,
  size_t count
) {
  for (size_t i = 0; i < count; i++) js_setprop(js,target,
    js_mkstr(js, methods[i].name, methods[i].len),
    js_mkfun(methods[i].fn)
  );
}

void init_date_module(void) {
  ant_t *js = rt->js;
  jsval_t glob = js_glob(js);
  jsval_t object_proto = js_get_ctor_proto(js, "Object", 6);
  jsval_t function_proto = js_get_ctor_proto(js, "Function", 8);

  jsval_t date_proto = js_mkobj(js);
  js_set_proto(js, date_proto, object_proto);

  static const date_method_entry_t kDateProtoMethods[] = {
    DATE_METHOD_ENTRY("valueOf", builtin_Date_valueOf),
    DATE_METHOD_ENTRY("toString", builtin_Date_toString),
    DATE_METHOD_ENTRY("toUTCString", builtin_Date_toUTCString),
    DATE_METHOD_ENTRY("toGMTString", builtin_Date_toUTCString),
    DATE_METHOD_ENTRY("toISOString", builtin_Date_toISOString),
    DATE_METHOD_ENTRY("toDateString", builtin_Date_toDateString),
    DATE_METHOD_ENTRY("toTimeString", builtin_Date_toTimeString),
    DATE_METHOD_ENTRY("toLocaleString", builtin_Date_toLocaleString),
    DATE_METHOD_ENTRY("toLocaleDateString", builtin_Date_toLocaleDateString),
    DATE_METHOD_ENTRY("toLocaleTimeString", builtin_Date_toLocaleTimeString),
    DATE_METHOD_ENTRY("getTimezoneOffset", builtin_Date_getTimezoneOffset),
    DATE_METHOD_ENTRY("getTime", builtin_Date_getTime),
    DATE_METHOD_ENTRY("getYear", builtin_Date_getYear),
    DATE_METHOD_ENTRY("getFullYear", builtin_Date_getFullYear),
    DATE_METHOD_ENTRY("getUTCFullYear", builtin_Date_getUTCFullYear),
    DATE_METHOD_ENTRY("getMonth", builtin_Date_getMonth),
    DATE_METHOD_ENTRY("getUTCMonth", builtin_Date_getUTCMonth),
    DATE_METHOD_ENTRY("getDate", builtin_Date_getDate),
    DATE_METHOD_ENTRY("getUTCDate", builtin_Date_getUTCDate),
    DATE_METHOD_ENTRY("getHours", builtin_Date_getHours),
    DATE_METHOD_ENTRY("getUTCHours", builtin_Date_getUTCHours),
    DATE_METHOD_ENTRY("getMinutes", builtin_Date_getMinutes),
    DATE_METHOD_ENTRY("getUTCMinutes", builtin_Date_getUTCMinutes),
    DATE_METHOD_ENTRY("getSeconds", builtin_Date_getSeconds),
    DATE_METHOD_ENTRY("getUTCSeconds", builtin_Date_getUTCSeconds),
    DATE_METHOD_ENTRY("getMilliseconds", builtin_Date_getMilliseconds),
    DATE_METHOD_ENTRY("getUTCMilliseconds", builtin_Date_getUTCMilliseconds),
    DATE_METHOD_ENTRY("getDay", builtin_Date_getDay),
    DATE_METHOD_ENTRY("getUTCDay", builtin_Date_getUTCDay),
    DATE_METHOD_ENTRY("setTime", builtin_Date_setTime),
    DATE_METHOD_ENTRY("setMilliseconds", builtin_Date_setMilliseconds),
    DATE_METHOD_ENTRY("setUTCMilliseconds", builtin_Date_setUTCMilliseconds),
    DATE_METHOD_ENTRY("setSeconds", builtin_Date_setSeconds),
    DATE_METHOD_ENTRY("setUTCSeconds", builtin_Date_setUTCSeconds),
    DATE_METHOD_ENTRY("setMinutes", builtin_Date_setMinutes),
    DATE_METHOD_ENTRY("setUTCMinutes", builtin_Date_setUTCMinutes),
    DATE_METHOD_ENTRY("setHours", builtin_Date_setHours),
    DATE_METHOD_ENTRY("setUTCHours", builtin_Date_setUTCHours),
    DATE_METHOD_ENTRY("setDate", builtin_Date_setDate),
    DATE_METHOD_ENTRY("setUTCDate", builtin_Date_setUTCDate),
    DATE_METHOD_ENTRY("setMonth", builtin_Date_setMonth),
    DATE_METHOD_ENTRY("setUTCMonth", builtin_Date_setUTCMonth),
    DATE_METHOD_ENTRY("setYear", builtin_Date_setYear),
    DATE_METHOD_ENTRY("setFullYear", builtin_Date_setFullYear),
    DATE_METHOD_ENTRY("setUTCFullYear", builtin_Date_setUTCFullYear),
    DATE_METHOD_ENTRY("toJSON", builtin_Date_toJSON),
  };
  date_define_methods(js, date_proto, kDateProtoMethods, DATE_COUNT_OF(kDateProtoMethods));

  jsval_t to_primitive_sym = get_toPrimitive_sym();
  if (vtype(to_primitive_sym) == T_SYMBOL) {
    js_set_sym(js, date_proto, to_primitive_sym, js_mkfun(date_toPrimitive));
  }

  jsval_t date_ctor_obj = js_mkobj(js);
  js_set_proto(js, date_ctor_obj, function_proto);
  js_set_slot(js, date_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Date));
  static const date_method_entry_t kDateCtorMethods[] = {
    DATE_METHOD_ENTRY("now", builtin_Date_now),
    DATE_METHOD_ENTRY("parse", builtin_Date_parse),
    DATE_METHOD_ENTRY("UTC", builtin_Date_UTC),
  };
  date_define_methods(js, date_ctor_obj, kDateCtorMethods, DATE_COUNT_OF(kDateCtorMethods));
  js_setprop_nonconfigurable(js, date_ctor_obj, "prototype", 9, date_proto);
  js_setprop(js, date_ctor_obj, ANT_STRING("name"), ANT_STRING("Date"));

  jsval_t date_ctor_func = js_obj_to_func(date_ctor_obj);
  js_setprop(js, glob, js_mkstr(js, "Date", 4), date_ctor_func);

  js_setprop(js, date_proto, js_mkstr(js, "constructor", 11), date_ctor_func);
  js_set_descriptor(js, date_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
}
