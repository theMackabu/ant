#include <compat.h> // IWYU pragma: keep

#include <stdio.h>
#include <string.h>

#include "ant.h"
#include "modules/http_metadata.h"

static const char *const http_methods[] = {
  "ACL",
  "BIND",
  "CHECKOUT",
  "CONNECT",
  "COPY",
  "DELETE",
  "GET",
  "HEAD",
  "LINK",
  "LOCK",
  "M-SEARCH",
  "MERGE",
  "MKACTIVITY",
  "MKCALENDAR",
  "MKCOL",
  "MOVE",
  "NOTIFY",
  "OPTIONS",
  "PATCH",
  "POST",
  "PROPFIND",
  "PROPPATCH",
  "PURGE",
  "PUT",
  "QUERY",
  "REBIND",
  "REPORT",
  "SEARCH",
  "SOURCE",
  "SUBSCRIBE",
  "TRACE",
  "UNBIND",
  "UNLINK",
  "UNLOCK",
  "UNSUBSCRIBE"
};

typedef struct {
  int code;
  const char *text;
} http_status_entry_t;

static const http_status_entry_t http_status_codes[] = {
  {200, "OK"},
  {201, "Created"},
  {202, "Accepted"},
  {204, "No Content"},
  {301, "Moved Permanently"},
  {302, "Found"},
  {303, "See Other"},
  {304, "Not Modified"},
  {307, "Temporary Redirect"},
  {308, "Permanent Redirect"},
  {400, "Bad Request"},
  {401, "Unauthorized"},
  {403, "Forbidden"},
  {404, "Not Found"},
  {405, "Method Not Allowed"},
  {408, "Request Timeout"},
  {409, "Conflict"},
  {410, "Gone"},
  {413, "Payload Too Large"},
  {415, "Unsupported Media Type"},
  {421, "Misdirected Request"},
  {429, "Too Many Requests"},
  {500, "Internal Server Error"},
  {501, "Not Implemented"},
  {502, "Bad Gateway"},
  {503, "Service Unavailable"},
  {504, "Gateway Timeout"}
};

static ant_value_t http_metadata_make_methods(ant_t *js) {
  ant_value_t methods = js_mkarr(js);
  size_t count = sizeof(http_methods) / sizeof(http_methods[0]);

  for (size_t i = 0; i < count; i++) {
    js_arr_push(js, methods, js_mkstr(js, http_methods[i], strlen(http_methods[i])));
  }

  return methods;
}

static ant_value_t http_metadata_make_status_codes(ant_t *js) {
  ant_value_t status_codes = js_mkobj(js);
  
  size_t count = sizeof(http_status_codes) / sizeof(http_status_codes[0]);
  char code_buf[16];

  for (size_t i = 0; i < count; i++) {
    int code = http_status_codes[i].code;
    size_t code_len = (size_t)snprintf(code_buf, sizeof(code_buf), "%d", code);
    
    js_setprop(
      js, status_codes,
      js_mkstr(js, code_buf, code_len),
      js_mkstr(js, http_status_codes[i].text, strlen(http_status_codes[i].text))
    );
  }

  return status_codes;
}

ant_value_t internal_http_metadata_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);

  js_set(js, lib, "METHODS", http_metadata_make_methods(js));
  js_set(js, lib, "STATUS_CODES", http_metadata_make_status_codes(js));
  
  return lib;
}
