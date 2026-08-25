#include <compat.h> // IWYU pragma: keep

#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <direct.h>
#define ant_getcwd _getcwd
#else
#include <unistd.h>
#define ant_getcwd getcwd
#endif

#include "ant.h"
#include "errors.h"
#include "internal.h"
#include "descriptors.h"
#include "silver/engine.h"

#include "modules/events.h"
#include "modules/globals.h"

#ifdef _WIN32
static bool is_windows_drive_path(const char *path) {
  return path &&
    ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
    path[1] == ':';
}
#endif

static ant_value_t make_global_location(ant_t *js) {
  ant_value_t location = js_newobj(js);
  char *cwd = ant_getcwd(NULL, 0);

#ifdef _WIN32
  if (cwd) {
    for (char *ch = cwd; *ch; ch++) {
      if (*ch == '\\') *ch = '/';
    }
  }
#endif
  
  const char *pathname = (cwd && cwd[0]) ? cwd : "/";
  size_t pathname_len = strlen(pathname);
  
  bool needs_slash = pathname_len > 0 && pathname[pathname_len - 1] != '/';
  size_t scheme_len = 7;
#ifdef _WIN32
  if (is_windows_drive_path(pathname)) scheme_len = 8;
#endif
  size_t href_len = scheme_len + pathname_len + (needs_slash ? 1 : 0);
  char *href = malloc(href_len + 1);

  if (href) {
    const char *scheme = "file://";
#ifdef _WIN32
    if (is_windows_drive_path(pathname)) scheme = "file:///";
#endif
    snprintf(href, href_len + 1, "%s%s%s", scheme, pathname, needs_slash ? "/" : "");
    js_set(js, location, "href", js_mkstr(js, href, href_len));
    free(href);
  }
  else js_set(js, location, "href", js_mkstr(js, "file:///", 8));

  js_set(js, location, "origin", js_mkstr(js, "null", 4));
  js_set(js, location, "protocol", js_mkstr(js, "file:", 5));
  js_set(js, location, "pathname", js_mkstr(js, pathname, pathname_len));
  js_set(js, location, "search", js_mkstr(js, "", 0));
  js_set(js, location, "hash", js_mkstr(js, "", 0));
  free(cwd);

  return location;
}

ant_value_t js_report_error(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "reportError requires 1 argument");
  
  ant_value_t error  = args[0];
  ant_value_t global = js_glob(js);

  const char *msg = "";
  uint8_t et = vtype(error);
  
  if (et == kTypeString) {
    msg = js_str(js, error);
  } else if (et == kTypeObject) {
    const char *m = get_str_prop(js, error, "message", 7, NULL);
    if (m && *m) msg = m;
  }
  
  if (!*msg) {
    ant_value_t str = js_tostring_val(js, error);
    if (vtype(str) == kTypeString) { const char *s = js_str(js, str); if (s) msg = s; }
  }

  const char *filename = "";
  int lineno = 1, colno = 1;
  js_get_call_location(js, &filename, &lineno, &colno);
  if (!filename) filename = "";

  ant_value_t loc = js_get(js, global, "location");
  if (vtype(loc) == kTypeObject) {
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
  if (ht == kTypeFunction || ht == kTypeBuiltin) {
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
  if (ht != kTypeFunction && ht != kTypeBuiltin) return false;

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
  if (ht != kTypeFunction && ht != kTypeBuiltin) return;

  ant_value_t event = js_mkobj(js);
  js_set(js, event, "type",    js_mkstr(js, "rejectionhandled", 16));
  js_set(js, event, "reason",  reason);
  js_set(js, event, "promise", promise_val);
  
  ant_value_t call_args[1] = { event };
  sv_vm_call(js->vm, js, handler, global, call_args, 1, NULL, false);
}

void init_globals_module(ant_t *js) {
  ant_value_t global = js_glob(js);

  if (js->runtime.flags & ANT_RUNTIME_WEB) {
    js_set(js, global, "location", make_global_location(js));
    js_set_descriptor(js, global, "location", 8, JS_DESC_W | JS_DESC_C);
  }

  js_set(js, global, "reportError", js_mkfun(js_report_error));
  js_set_descriptor(js, global, "reportError", 11, JS_DESC_W | JS_DESC_C);
}
