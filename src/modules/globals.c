#include <compat.h> // IWYU pragma: keep

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "descriptors.h"
#include "silver/engine.h"

#include "modules/events.h"
#include "modules/globals.h"

ant_value_t js_report_error(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "reportError requires 1 argument");
  
  ant_value_t error  = args[0];
  ant_value_t global = js_glob(js);

  const char *msg = "";
  uint8_t et = vtype(error);
  
  if (et == T_STR) {
    msg = js_str(js, error);
  } else if (et == T_OBJ) {
    const char *m = get_str_prop(js, error, "message", 7, NULL);
    if (m && *m) msg = m;
  }
  
  if (!*msg) {
    ant_value_t str = js_tostring_val(js, error);
    if (vtype(str) == T_STR) { const char *s = js_str(js, str); if (s) msg = s; }
  }

  const char *filename = "";
  int lineno = 1, colno = 1;
  js_get_call_location(js, &filename, &lineno, &colno);
  if (!filename) filename = "";

  ant_value_t loc = js_get(js, global, "location");
  if (vtype(loc) == T_OBJ) {
    const char *href = get_str_prop(js, loc, "href", 4, NULL);
    if (href && *href) filename = href;
  }

  ant_value_t event = js_mkobj(js);
  js_set(js, event, "type",     js_mkstr(js, "error", 5));
  js_set(js, event, "message",  js_mkstr(js, msg, strlen(msg)));
  js_set(js, event, "filename", js_mkstr(js, filename, strlen(filename)));
  js_set(js, event, "lineno",   js_mknum(lineno));
  js_set(js, event, "colno",    js_mknum(colno));
  js_set(js, event, "error",    error);

  js_dispatch_global_event(js, event);
  ant_value_t handler = js_get(js, global, "onerror");
  
  uint8_t ht = vtype(handler);
  if (ht == T_FUNC || ht == T_CFUNC) {
    ant_value_t msg_str  = js_mkstr(js, msg, strlen(msg));
    ant_value_t file_str = js_mkstr(js, filename, strlen(filename));
    ant_value_t call_args[5] = { msg_str, file_str, js_mknum(lineno), js_mknum(colno), error };
    sv_vm_call(js->vm, js, handler, global, call_args, 5, NULL, false);
  }

  return js_mkundef();
}

bool js_fire_unhandled_rejection(ant_t *js, ant_value_t promise_val, ant_value_t reason) {
  ant_value_t global  = js_glob(js);
  ant_value_t handler = js_get(js, global, "onunhandledrejection");
  
  uint8_t ht = vtype(handler);
  if (ht != T_FUNC && ht != T_CFUNC) return false;

  ant_value_t event = js_mkobj(js);
  js_set(js, event, "type",    js_mkstr(js, "unhandledrejection", 18));
  js_set(js, event, "reason",  reason);
  js_set(js, event, "promise", promise_val);
  
  ant_value_t call_args[1] = { event };
  sv_vm_call(js->vm, js, handler, global, call_args, 1, NULL, false);
  
  return true;
}

void js_fire_rejection_handled(ant_t *js, ant_value_t promise_val, ant_value_t reason) {
  ant_value_t global  = js_glob(js);
  ant_value_t handler = js_get(js, global, "onrejectionhandled");
  
  uint8_t ht = vtype(handler);
  if (ht != T_FUNC && ht != T_CFUNC) return;

  ant_value_t event = js_mkobj(js);
  js_set(js, event, "type",    js_mkstr(js, "rejectionhandled", 16));
  js_set(js, event, "reason",  reason);
  js_set(js, event, "promise", promise_val);
  
  ant_value_t call_args[1] = { event };
  sv_vm_call(js->vm, js, handler, global, call_args, 1, NULL, false);
}

// stub: minimal Intl
static ant_value_t intl_dtf_format(ant_t *js, ant_value_t *args, int nargs) {
  time_t t;
  
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    t = (time_t)(js_getnum(args[0]) / 1000.0);
  } else t = time(NULL);

  struct tm local;
#ifdef _WIN32
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif

  char buf[64];
  int hour12 = local.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;
  
  const char *ampm = local.tm_hour < 12 ? "AM" : "PM";
  snprintf(buf, sizeof(buf), "%d:%02d:%02d %s", hour12, local.tm_min, local.tm_sec, ampm);

  return js_mkstr(js, buf, strlen(buf));
}

static ant_value_t intl_dtf_resolvedOptions(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t obj = js_mkobj(js);
  js_set(js, obj, "locale", js_mkstr(js, "en-US", 5));
  js_set(js, obj, "timeZone", js_mkstr(js, "UTC", 3));
  return obj;
}

static ant_value_t intl_dtf_formatToParts(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkarr(js);
}

static ant_value_t intl_dtf_constructor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this = js_getthis(js);

  ant_value_t format_fn = js_heavy_mkfun(js, intl_dtf_format, js_mkundef());
  js_set(js, this, "format", format_fn);
  js_set(js, this, "resolvedOptions", js_mkfun(intl_dtf_resolvedOptions));
  js_set(js, this, "formatToParts", js_mkfun(intl_dtf_formatToParts));

  return this;
}

static size_t intl_utf8_segment_len(const char *input, size_t remaining) {
  if (remaining == 0) return 0;

  const unsigned char *s = (const unsigned char *)input;
  unsigned char c = s[0];
  size_t len = 1;

  if ((c & 0x80) == 0) return 1;
  if ((c & 0xe0) == 0xc0) len = 2;
  else if ((c & 0xf0) == 0xe0) len = 3;
  else if ((c & 0xf8) == 0xf0) len = 4;

  if (len > remaining) return 1;
  for (size_t i = 1; i < len; i++) if ((s[i] & 0xc0) != 0x80) return 1;

  return len;
}

static bool intl_ascii_is_word_byte(const char *segment, size_t len) {
  if (len != 1) return true;

  unsigned char c = (unsigned char)segment[0];
  return 
    (c >= '0' && c <= '9') ||
    (c >= 'A' && c <= 'Z') ||
    (c >= 'a' && c <= 'z') ||
    c == '_';
}

static const char *intl_segmenter_granularity(ant_t *js, ant_value_t segmenter, size_t *len) {
  ant_value_t granularity = js_get(js, segmenter, "granularity");
  if (vtype(granularity) != T_STR) {
    if (len) *len = 8;
    return "grapheme";
  }

  return js_getstr(js, granularity, len);
}

static ant_value_t intl_segmenter_segment(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t input = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "", 0);
  if (is_err(input)) return input;

  size_t input_len = 0;
  char *input_str = js_getstr(js, input, &input_len);
  ant_value_t segments = js_mkarr(js);

  ant_value_t this = js_getthis(js);
  size_t granularity_len = 0;
  const char *granularity = intl_segmenter_granularity(js, this, &granularity_len);
  bool word_granularity = granularity_len == 4 && memcmp(granularity, "word", 4) == 0;

  for (size_t offset = 0; offset < input_len;) {
    size_t segment_len = intl_utf8_segment_len(input_str + offset, input_len - offset);
    ant_value_t record = js_mkobj(js);
    
    js_set(js, record, "segment", js_mkstr(js, input_str + offset, segment_len));
    js_set(js, record, "index", js_mknum((double)offset));
    js_set(js, record, "input", input);
    
    if (word_granularity) js_set(
      js, record, "isWordLike",
      js_bool(intl_ascii_is_word_byte(input_str + offset, segment_len))
    );
    
    js_arr_push(js, segments, record);
    offset += segment_len;
  }

  return segments;
}

static ant_value_t intl_segmenter_resolvedOptions(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t obj = js_mkobj(js);
  ant_value_t this = js_getthis(js);
  
  size_t granularity_len = 0;
  const char *granularity = intl_segmenter_granularity(js, this, &granularity_len);

  js_set(js, obj, "locale", js_mkstr(js, "en-US", 5));
  js_set(js, obj, "granularity", js_mkstr(js, granularity, granularity_len));
  
  return obj;
}

static ant_value_t intl_segmenter_constructor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this = js_getthis(js);
  const char *granularity = "grapheme";
  size_t granularity_len = 8;

  if (nargs >= 2 && vtype(args[1]) == T_OBJ) {
    ant_value_t option = js_get(js, args[1], "granularity");
    if (vtype(option) == T_STR) granularity = js_getstr(js, option, &granularity_len);
  }

  js_set(js, this, "granularity", js_mkstr(js, granularity, granularity_len));
  js_set(js, this, "segment", js_mkfun(intl_segmenter_segment));
  js_set(js, this, "resolvedOptions", js_mkfun(intl_segmenter_resolvedOptions));

  return this;
}

void init_globals_module(void) {
  ant_t *js = rt->js;
  ant_value_t global = js_glob(js);

  js_set(js, global, "reportError", js_mkfun(js_report_error));
  js_set_descriptor(js, global, "reportError", 11, JS_DESC_W | JS_DESC_C);

  ant_value_t intl = js_mkobj(js);
  ant_value_t dtf_ctor = js_heavy_mkfun(js, intl_dtf_constructor, js_mkundef());
  ant_value_t segmenter_ctor = js_heavy_mkfun(js, intl_segmenter_constructor, js_mkundef());
  
  js_mark_constructor(dtf_ctor, true);
  js_mark_constructor(segmenter_ctor, true);
  
  js_set(js, intl, "DateTimeFormat", dtf_ctor);
  js_set(js, intl, "Segmenter", segmenter_ctor);
  js_set(js, global, "Intl", intl);
}
