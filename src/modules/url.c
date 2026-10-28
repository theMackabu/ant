#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <ada_c.h>
#include <uv.h>

#include "ant.h"
#include "utf8.h"
#include "cfunc.h"
#include "ptr.h"
#include "errors.h"
#include "internal.h"
#include "descriptors.h"
#include "gc/roots.h"

#include "modules/blob.h"
#include "modules/crypto.h"
#include "silver/call.h"
#include "modules/url.h"
#include "modules/symbol.h"
#include "modules/iterator.h"
#include "modules/uri.h"
#include "url/url_internal.h"

static constexpr uint32_t URL_NATIVE_TAG = Ant_NativeTag("URLS");

enum {
  USP_ITER_ENTRIES = 0,
  USP_ITER_KEYS = 1,
  USP_ITER_VALUES = 2
};

url_state_t *url_get_state(ant_value_t obj) {
  return (url_state_t *)js_get_native(obj, URL_NATIVE_TAG);
}

bool usp_is_urlsearchparams(ant_t *js, ant_value_t obj) {
  return js_check_brand(obj, BRAND_URLSEARCHPARAMS);
}

void url_state_clear(url_state_t *s) {
  if (!s) return;
  free(s->href);     free(s->protocol); free(s->username);
  free(s->password);
  free(s->hostname); free(s->port);    free(s->pathname);
  free(s->search);   free(s->hash);
  memset(s, 0, sizeof(*s));
}

void url_free_state(url_state_t *s) {
  if (!s) return;
  url_state_clear(s);
  free(s);
}

static void url_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  url_free_state((url_state_t *)js_get_native(value, URL_NATIVE_TAG));
  js_clear_native(value, URL_NATIVE_TAG);
}

static bool is_special_scheme(const char *proto) {
  if (!proto) return false;
  return 
    strcmp(proto, "http:") == 0 || strcmp(proto, "https:") == 0 ||
    strcmp(proto, "ftp:") == 0  || strcmp(proto, "ws:") == 0    ||
    strcmp(proto, "wss:") == 0;
}

static bool uses_authority_syntax(const char *proto) {
  if (!proto) return false;
  return is_special_scheme(proto) || strcmp(proto, "file:") == 0;
}

static bool url_has_opaque_path(const url_state_t *s) {
  if (!s || uses_authority_syntax(s->protocol) || (s->hostname && *s->hostname)) return false;
  return !s->pathname || s->pathname[0] != '/';
}

char *form_urlencode_n(const char *str, size_t len) {
  if (!str) return strdup("");
  char *out = malloc(len * 3 + 1);
  
  if (!out) return strdup("");
  size_t j = 0;
  
  for (size_t i = 0; i < len; i++) {
  unsigned char c = (unsigned char)str[i];
  if (isalnum(c) || c == '*' || c == '-' || c == '.' || c == '_') out[j++] = (char)c;
  else if (c == ' ') out[j++] = '+';
  else {
    snprintf(out + j, 4, "%%%02X", c);
    j += 3;
  }}

  out[j] = '\0';
  return out;
}

static char *url_decoded_utf8(char *out, size_t len, size_t *out_len) {
  size_t valid = utf8_valid_prefix(out, len, NULL);
  if (valid < len) {
    char *clean = utf8_replace_invalid(out, len, valid, &len);
    free(out);
    if (!clean) return strdup("");
    out = clean;
  }
  if (out_len) *out_len = len;
  return out;
}

char *form_urldecode_len(const char *str, size_t *out_len) {
  if (out_len) *out_len = 0;
  if (!str) return strdup("");
  size_t len = strlen(str);
  char *out = malloc(len + 1);

  if (!out) return strdup("");
  size_t j = 0;

  for (size_t i = 0; i < len; i++) {
    if (str[i] == '+') out[j++] = ' ';
    else if (
      str[i] == '%' && i + 2 < len &&
      isxdigit((unsigned char)str[i+1]) &&
      isxdigit((unsigned char)str[i+2])
    ) {
      int hi = isdigit((unsigned char)str[i+1]) ? str[i+1]-'0' : tolower((unsigned char)str[i+1])-'a'+10;
      int lo = isdigit((unsigned char)str[i+2]) ? str[i+2]-'0' : tolower((unsigned char)str[i+2])-'a'+10;
      out[j++] = (char)((hi << 4) | lo);
      i += 2;
    } else out[j++] = str[i];
  }
  
  out[j] = '\0';
  return url_decoded_utf8(out, j, out_len);
}

char *url_decode_component_len(const char *str, size_t *out_len) {
  if (out_len) *out_len = 0;
  if (!str) return strdup("");
  size_t len = strlen(str);
  char *out = malloc(len + 1);

  if (!out) return strdup("");
  size_t j = 0;

  for (size_t i = 0; i < len; i++) {
    if (
      str[i] == '%' && i + 2 < len &&
      isxdigit((unsigned char)str[i+1]) &&
      isxdigit((unsigned char)str[i+2])
    ) {
      int hi = isdigit((unsigned char)str[i+1]) ? str[i+1]-'0' : tolower((unsigned char)str[i+1])-'a'+10;
      int lo = isdigit((unsigned char)str[i+2]) ? str[i+2]-'0' : tolower((unsigned char)str[i+2])-'a'+10;
      out[j++] = (char)((hi << 4) | lo);
      i += 2;
    } else out[j++] = str[i];
  }

  out[j] = '\0';
  return url_decoded_utf8(out, j, out_len);
}

char *form_urldecode(const char *str) {
  return form_urldecode_len(str, NULL);
}

char *url_decode_component(const char *str) {
  return url_decode_component_len(str, NULL);
}

static char *ada_string_dup(ada_string value) {
  return value.data ? strndup(value.data, value.length) : strdup("");
}

static int ada_url_to_state(ada_url url, url_state_t *s) {
  s->href = ada_string_dup(ada_get_href(url));
  s->protocol = ada_string_dup(ada_get_protocol(url));
  s->username = ada_string_dup(ada_get_username(url));
  s->password = ada_string_dup(ada_get_password(url));
  s->hostname = ada_string_dup(ada_get_hostname(url));
  s->port = ada_string_dup(ada_get_port(url));
  s->pathname = ada_string_dup(ada_get_pathname(url));
  s->search = ada_string_dup(ada_get_search(url));
  s->hash = ada_string_dup(ada_get_hash(url));

  if (s->search && !*s->search && ada_has_search(url)) {
    free(s->search);
    s->search = strdup("?");
  }
  if (s->hash && !*s->hash && ada_has_hash(url)) {
    free(s->hash);
    s->hash = strdup("#");
  }

  if (
    !s->href || !s->protocol || !s->username || !s->password || !s->hostname ||
    !s->port || !s->pathname || !s->search || !s->hash
  ) {
    url_state_clear(s);
    return -1;
  }
  return 0;
}

static int parse_url_to_state_n(
  const char *url_str,
  size_t url_len,
  const char *base_str,
  size_t base_len,
  url_state_t *s
) {
  if (!url_str || !s) return -1;
  memset(s, 0, sizeof(*s));

  char *url_utf8 = wtf8_to_utf8_dup(url_str, &url_len);
  char *base_utf8 = base_str ? wtf8_to_utf8_dup(base_str, &base_len) : NULL;
  if (url_utf8) url_str = url_utf8;
  if (base_utf8) base_str = base_utf8;

  ada_url url = base_str
    ? ada_parse_with_base(url_str, url_len, base_str, base_len)
    : ada_parse(url_str, url_len);
  free(url_utf8);
  free(base_utf8);
  if (!url || !ada_is_valid(url)) {
    if (url) ada_free(url);
    return -1;
  }

  int result = ada_url_to_state(url, s);
  ada_free(url);
  return result;
}

int parse_url_to_state(const char *url_str, const char *base_str, url_state_t *s) {
  return parse_url_to_state_n(
    url_str,
    url_str ? strlen(url_str) : 0,
    base_str,
    base_str ? strlen(base_str) : 0,
    s
  );
}

int Ant_URL_ParseDocumentRelative(ant_t *js, const char *url_str, url_state_t *s) {
  ant_value_t href = js->builtins.location_href;
  return parse_url_to_state(url_str, vtype(href) == kTypeString ? js_getstr(js, href, NULL) : NULL, s);
}

int Ant_URL_ParseLocation(const char *href, url_state_t *s) {
  if (parse_url_to_state(href, NULL, s) != 0) return -1;
  if (strcmp(s->protocol, "http:") == 0 || strcmp(s->protocol, "https:") == 0) return 0;
  url_state_clear(s);
  return -1;
}

char *build_href(const url_state_t *s) {
  if (!s) return NULL;
  if (s->href) return strdup(s->href);
  if (!s->protocol) return NULL;

  bool has_authority =
    (s->hostname && *s->hostname) ||
    (s->username && *s->username) ||
    (s->password && *s->password) ||
    (s->port && *s->port);
    
  bool opaque_like = url_has_opaque_path(s);
  bool serialize_authority = has_authority || uses_authority_syntax(s->protocol);
  const char *pathname = s->pathname ? s->pathname : "";
  size_t len = strlen(s->protocol) + strlen(pathname) + strlen(s->search) + strlen(s->hash) + 32;

  if (has_authority) len += strlen(s->hostname) + 2;
  if (s->username && *s->username) len += strlen(s->username) + 1;
  if (s->password && *s->password) len += strlen(s->password) + 1;
  if (s->port && *s->port) len += strlen(s->port) + 1;

  char *href = malloc(len);
  if (!href) return strdup("");
  
  size_t pos = 0;
  pos += (size_t)sprintf(href + pos, "%s", s->protocol);

  if (opaque_like) {
    pos += (size_t)sprintf(href + pos, "%s%s%s", pathname, s->search, s->hash);
    href[pos] = '\0';
    return href;
  }

  if (!serialize_authority) {
    pos += (size_t)sprintf(href + pos, "%s%s%s", pathname, s->search, s->hash);
    href[pos] = '\0';
    return href;
  }

  pos += (size_t)sprintf(href + pos, "//");
  if (s->username && *s->username) {
    pos += (size_t)sprintf(href + pos, "%s", s->username);
    if (s->password && *s->password)
      pos += (size_t)sprintf(href + pos, ":%s", s->password);
    href[pos++] = '@';
  }

  pos += (size_t)sprintf(href + pos, "%s", s->hostname);
  if (s->port && *s->port)
    pos += (size_t)sprintf(href + pos, ":%s", s->port);

  pos += (size_t)sprintf(href + pos, "%s%s%s", pathname, s->search, s->hash);
  href[pos] = '\0';
  return href;
}

static ant_value_t url_string_value(ant_t *js, ant_value_t value) {
  return vtype(value) == kTypeString ? value : js_template_to_string(js, value);
}

static ant_value_t url_invalid_url_error(ant_t *js, ant_value_t input, ant_value_t base) {
  ant_value_t props = Ant_Object_CreateBare(js);
  js_set(js, props, "code", js_mkstr(js, "ERR_INVALID_URL", 15));
  js_set(js, props, "input", input);
  if (!is_undefined(base)) js_set(js, props, "base", base);
  return js_mkerr_props(js, JS_ERR_TYPE, props, "Invalid URL");
}

static ant_value_t url_missing_args_error(ant_t *js, const char *message) {
  return js_mkerr_code(js, JS_ERR_TYPE, "ERR_MISSING_ARGS", message);
}

typedef bool (*ada_url_setter_t)(ada_url, const char *, size_t);

static bool ada_set_search_bool(ada_url url, const char *value, size_t len) {
  ada_set_search(url, value, len);
  return true;
}

static bool ada_set_hash_bool(ada_url url, const char *value, size_t len) {
  ada_set_hash(url, value, len);
  return true;
}

static ant_value_t url_apply_setter(
  ant_t *js,
  url_state_t *state,
  ant_value_t input,
  ada_url_setter_t setter
) {
  ant_value_t value = url_string_value(js, input);
  if (is_err(value)) return value;

  size_t value_len = 0;
  const char *value_str = js_getstr(js, value, &value_len);
  char *href = build_href(state);
  if (!value_str || !href) { free(href); return Ant_Error_CreateOutOfMemory(js); }

  ada_url url = ada_parse(href, strlen(href));
  free(href);
  if (!url || !ada_is_valid(url)) {
    if (url) ada_free(url);
    return js_mkundef();
  }

  char *value_utf8 = wtf8_to_utf8_dup(value_str, &value_len);
  setter(url, value_utf8 ? value_utf8 : value_str, value_len);
  free(value_utf8);
  
  if (ada_is_valid(url)) {
    url_state_t next = {0};
    if (ada_url_to_state(url, &next) == 0) {
      url_state_clear(state);
      *state = next;
    }
  }
  
  ada_free(url);
  return js_mkundef();
}

static ant_value_t parse_query_to_arr(ant_t *js, const char *query) {
  ant_value_t arr = js_mkarr(js);
  if (!query || !*query) return arr;
  const char *p = query;
  
  while (*p) {
    const char *amp = strchr(p, '&');
    size_t plen = amp ? (size_t)(amp - p) : strlen(p);
    if (plen == 0) { p = amp ? amp + 1 : p + 1; continue; }
    char *pair = strndup(p, plen);
    if (!pair) { p = amp ? amp + 1 : p + plen; continue; }
    
    char *eq = strchr(pair, '=');
    char *raw_v = eq ? ((*eq = '\0'), eq + 1) : "";
    char *k = form_urldecode(pair);
    char *v = form_urldecode(raw_v);
    
    ant_value_t entry = js_mkarr(js);
    js_arr_push(js, entry, js_mkstr(js, k, strlen(k)));
    js_arr_push(js, entry, js_mkstr(js, v, strlen(v)));
    js_arr_push(js, arr, entry);
    free(pair); free(k); free(v);
    p = amp ? amp + 1 : p + plen;
  }
  
  return arr;
}

char *usp_serialize(ant_t *js, ant_value_t usp) {
  ant_value_t entries = js_get_slot(usp, SLOT_ENTRIES);
  ant_offset_t len = is_special_object(entries) ? js_arr_len(js, entries) : 0;

  size_t cap = 256;
  char *buf = malloc(cap);
  if (!buf) return strdup("");
  size_t pos = 0;

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, entries, i);
    size_t klen = 0, vlen = 0;
    
    char *k = js_getstr(js, js_arr_get(js, entry, 0), &klen);
    char *v = js_getstr(js, js_arr_get(js, entry, 1), &vlen);
    char *ek = k ? form_urlencode_n(k, klen) : strdup("");
    char *ev = v ? form_urlencode_n(v, vlen) : strdup("");
    
    size_t needed = strlen(ek) + strlen(ev) + 3;
    if (pos + needed >= cap) {
      cap = cap * 2 + needed;
      buf = realloc(buf, cap);
      if (!buf) { free(ek); free(ev); return strdup(""); }
    }
    
    if (pos > 0) buf[pos++] = '&';
    pos += (size_t)sprintf(buf + pos, "%s=%s", ek, ev);
    free(ek); free(ev);
  }
  
  buf[pos] = '\0';
  return buf;
}

static void usp_push_to_url(ant_t *js, ant_value_t usp) {
  ant_value_t url_obj = js_get_slot(usp, SLOT_DATA);
  if (!is_special_object(url_obj)) return;
  url_state_t *s = url_get_state(url_obj);
  
  if (!s) return;
  char *qs = usp_serialize(js, usp);
  ada_url url = s->href ? ada_parse(s->href, strlen(s->href)) : NULL;
  if (url && ada_is_valid(url)) {
    if (*qs) ada_set_search(url, qs, strlen(qs));
    else ada_clear_search(url);

    url_state_t next = {0};
    if (ada_url_to_state(url, &next) == 0) {
      url_state_clear(s);
      *s = next;
    }
  }
  if (url) ada_free(url);
  free(qs);
}

static void url_sync_usp(ant_t *js, ant_value_t url_obj, const char *query) {
  ant_value_t usp = js_get_slot(url_obj, SLOT_ENTRIES);
  if (!is_special_object(usp)) return;
  ant_value_t new_entries = parse_query_to_arr(js, query);
  js_set_slot_wb(js, usp, SLOT_ENTRIES, new_entries);
}

static ant_value_t make_usp_for_url(ant_t *js, ant_value_t url_obj, const char *query) {
  ant_value_t usp = Ant_Object_CreateBare(js);
  js_set_proto_init(usp, js->builtins.usp_proto);
  js_set_slot(usp, SLOT_BRAND, js_mknum(BRAND_URLSEARCHPARAMS));
  js_set_slot_wb(js, usp, SLOT_DATA, url_obj);
  js_set_slot(usp, SLOT_ENTRIES, parse_query_to_arr(js, query));
  return usp;
}

static ant_value_t url_get_href(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  char *href = build_href(s);
  if (!href) return js_mkstr(js, "", 0);
  ant_value_t ret = js_mkstr(js, href, strlen(href));
  free(href);
  return ret;
}

static ant_value_t url_get_protocol(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  return js_mkstr(js, s->protocol, strlen(s->protocol));
}

static ant_value_t url_get_username(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  return js_mkstr(js, s->username, strlen(s->username));
}

static ant_value_t url_get_password(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  return js_mkstr(js, s->password, strlen(s->password));
}

static ant_value_t url_get_host(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  if (s->port && *s->port) {
    size_t len = strlen(s->hostname) + strlen(s->port) + 2;
    char *host = malloc(len);
    snprintf(host, len, "%s:%s", s->hostname, s->port);
    ant_value_t ret = js_mkstr(js, host, strlen(host));
    free(host);
    return ret;
  }
  return js_mkstr(js, s->hostname, strlen(s->hostname));
}

static ant_value_t url_get_hostname(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  return js_mkstr(js, s->hostname, strlen(s->hostname));
}

static ant_value_t url_get_port(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  return js_mkstr(js, s->port, strlen(s->port));
}

static ant_value_t url_get_pathname(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  return js_mkstr(js, s->pathname, strlen(s->pathname));
}

static ant_value_t url_get_search(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  if (s->search[0] == '?' && s->search[1] == '\0') return js_mkstr(js, "", 0);
  return js_mkstr(js, s->search, strlen(s->search));
}

static ant_value_t url_get_hash(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  if (s->hash[0] == '#' && s->hash[1] == '\0') return js_mkstr(js, "", 0);
  return js_mkstr(js, s->hash, strlen(s->hash));
}

static ant_value_t url_get_origin(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");

  char *href = build_href(s);
  if (!href) return js_mkstr(js, "null", 4);
  ada_url url = ada_parse(href, strlen(href));
  free(href);
  if (!url || !ada_is_valid(url)) {
    if (url) ada_free(url);
    return js_mkstr(js, "null", 4);
  }

  ada_owned_string origin = ada_get_origin(url);
  ant_value_t result = origin.data
    ? js_mkstr(js, origin.data, origin.length)
    : js_mkstr(js, "null", 4);
  ada_free_owned_string(origin);
  ada_free(url);
  return result;
}

static ant_value_t url_get_searchParams(ant_params_t) {
  if (!url_get_state(js->this_val))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  ant_value_t usp = js_get_slot(js->this_val, SLOT_ENTRIES);
  if (vtype(usp) == kTypeObject) return usp;
  return js_mkundef();
}

static ant_value_t url_set_href(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");

  ant_value_t value = url_string_value(js, nargs ? args[0] : js_mkundef());
  if (is_err(value)) return value;
  size_t value_len = 0;
  const char *val = js_getstr(js, value, &value_len);
  if (!val) return url_invalid_url_error(js, value, js_mkundef());
  
  url_state_t tmp;
  if (parse_url_to_state_n(val, value_len, NULL, 0, &tmp) != 0)
    return url_invalid_url_error(js, value, js_mkundef());
    
  url_state_clear(s);
  *s = tmp;
  
  const char *q = (s->search[0] == '?') ? s->search + 1 : "";
  url_sync_usp(js, js->this_val, q);
  
  return js_mkundef();
}

static ant_value_t url_set_protocol(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  return url_apply_setter(js, s, nargs ? args[0] : js_mkundef(), ada_set_protocol);
}

static ant_value_t url_set_username(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  return url_apply_setter(js, s, nargs ? args[0] : js_mkundef(), ada_set_username);
}

static ant_value_t url_set_password(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  return url_apply_setter(js, s, nargs ? args[0] : js_mkundef(), ada_set_password);
}

static ant_value_t url_set_host(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  return url_apply_setter(js, s, nargs ? args[0] : js_mkundef(), ada_set_host);
}

static ant_value_t url_set_hostname(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  return url_apply_setter(js, s, nargs ? args[0] : js_mkundef(), ada_set_hostname);
}

static ant_value_t url_set_port(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  return url_apply_setter(js, s, nargs ? args[0] : js_mkundef(), ada_set_port);
}

static ant_value_t url_set_pathname(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  return url_apply_setter(js, s, nargs ? args[0] : js_mkundef(), ada_set_pathname);
}

static ant_value_t url_set_search(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  ant_value_t result = url_apply_setter(js, s, nargs ? args[0] : js_mkundef(), ada_set_search_bool);
  if (is_err(result)) return result;
  const char *query = s->search && s->search[0] == '?' ? s->search + 1 : "";
  url_sync_usp(js, js->this_val, query);
  return result;
}

static ant_value_t url_set_hash(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  return url_apply_setter(js, s, nargs ? args[0] : js_mkundef(), ada_set_hash_bool);
}

static ant_value_t url_toString(ant_params_t) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  char *href = build_href(s);
  if (!href) return js_mkstr(js, "", 0);
  ant_value_t ret = js_mkstr(js, href, strlen(href));
  free(href);
  return ret;
}

static ant_value_t js_URL(ant_params_t) {
  if (is_undefined(call_new_target))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Class constructor URL cannot be invoked without 'new'");
  if (nargs < 1) return url_missing_args_error(js, "The \"url\" argument must be specified");

  ant_value_t url_sv = url_string_value(js, args[0]);
  if (is_err(url_sv)) return url_sv;
  size_t url_len = 0;
  const char *url_str = js_getstr(js, url_sv, &url_len);

  const char *base_str = NULL;
  size_t base_len = 0;
  ant_value_t base_sv = js_mkundef();
  if (nargs > 1 && !is_undefined(args[1])) {
    base_sv = url_string_value(js, args[1]);
    if (is_err(base_sv)) return base_sv;
    base_str = js_getstr(js, base_sv, &base_len);
  }

  url_state_t *s = calloc(1, sizeof(url_state_t));
  if (!s) return Ant_Error_CreateOutOfMemory(js);

  if (!url_str || parse_url_to_state_n(url_str, url_len, base_str, base_len, s) != 0) {
    free(s);
    return url_invalid_url_error(js, url_sv, base_sv);
  }

  ant_value_t obj = Ant_Object_CreateBare(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.url_proto, call_new_target);
  js_set_proto_init(obj, is_object_type(proto) ? proto : js->builtins.url_proto);
  js_set_native(obj, s, URL_NATIVE_TAG);
  js_set_finalizer(obj, url_finalize);

  const char *query = (s->search && s->search[0] == '?') ? s->search + 1 : "";
  ant_value_t usp = make_usp_for_url(js, obj, query);
  js_set_slot_wb(js, obj, SLOT_ENTRIES, usp);

  return obj;
}

ant_value_t make_url_obj(ant_t *js, url_state_t *s) {
  ant_value_t obj = Ant_Object_CreateBare(js);
  js_set_proto_init(obj, js->builtins.url_proto);
  js_set_native(obj, s, URL_NATIVE_TAG);
  js_set_finalizer(obj, url_finalize);
  const char *query = (s->search && s->search[0] == '?') ? s->search + 1 : "";
  ant_value_t usp = make_usp_for_url(js, obj, query);
  js_set_slot_wb(js, obj, SLOT_ENTRIES, usp);
  return obj;
}

static ant_value_t url_canParse(ant_params_t) {
  if (nargs < 1) return url_missing_args_error(js, "The \"url\" argument must be specified");
  ant_value_t url_sv = url_string_value(js, args[0]);
  if (is_err(url_sv)) return url_sv;
  size_t url_len = 0;
  const char *url_str = js_getstr(js, url_sv, &url_len);
  if (!url_str) return js_false;
  const char *base_str = NULL;
  size_t base_len = 0;
  ant_value_t base_sv = js_mkundef();
  if (nargs > 1 && !is_undefined(args[1])) {
    base_sv = url_string_value(js, args[1]);
    if (is_err(base_sv)) return base_sv;
    base_str = js_getstr(js, base_sv, &base_len);
  }
  url_state_t s;
  if (parse_url_to_state_n(url_str, url_len, base_str, base_len, &s) != 0) return js_false;
  url_state_clear(&s);
  return js_true;
}

static ant_value_t url_parse(ant_params_t) {
  if (nargs < 1) return url_missing_args_error(js, "The \"url\" argument must be specified");
  ant_value_t url_sv = url_string_value(js, args[0]);
  if (is_err(url_sv)) return url_sv;
  size_t url_len = 0;
  const char *url_str = js_getstr(js, url_sv, &url_len);
  if (!url_str) return js_mknull();
  const char *base_str = NULL;
  size_t base_len = 0;
  ant_value_t base_sv = js_mkundef();
  if (nargs > 1 && !is_undefined(args[1])) {
    base_sv = url_string_value(js, args[1]);
    if (is_err(base_sv)) return base_sv;
    base_str = js_getstr(js, base_sv, &base_len);
  }
  url_state_t *s = calloc(1, sizeof(url_state_t));
  if (!s) return js_mknull();
  if (parse_url_to_state_n(url_str, url_len, base_str, base_len, s) != 0) {
    free(s);
    return js_mknull();
  }
  return make_url_obj(js, s);
}

static size_t object_url_key_length(const char *url) {
  const char *hash = url ? strchr(url, '#') : NULL;
  return hash ? (size_t)(hash - url) : (url ? strlen(url) : 0);
}

ant_value_t url_resolve_object_url(ant_t *js, const char *url) {
  if (!url || strncmp(url, "blob:", 5) != 0 || !is_object_type(js->builtins.url_ctor))
    return js_mkundef();

  ant_value_t store = js_get_slot(js->builtins.url_ctor, SLOT_DATA);
  if (vtype(store) != kTypeArray) return js_mkundef();

  size_t url_len = object_url_key_length(url);
  ant_offset_t len = js_arr_len(js, store);
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, store, i);
    ant_value_t key = js_arr_get(js, entry, 0);
    size_t key_len = 0;
    const char *key_str = js_getstr(js, key, &key_len);
    if (key_str && key_len == url_len && memcmp(key_str, url, url_len) == 0)
      return js_arr_get(js, entry, 1);
  }

  return js_mkundef();
}

static ant_value_t url_create_object_url(ant_params_t) {
  if (nargs < 1)
    return js_mkerr_typed(js, JS_ERR_TYPE, "URL.createObjectURL requires at least 1 argument");
  if (!blob_is_blob(js, args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "URL.createObjectURL requires a Blob");

  char uuid[37];
  if (crypto_random_uuid(uuid) < 0) return js_mkerr(js, "failed to generate blob URL");

  char url[sizeof("blob:ant/") + 36];
  snprintf(url, sizeof(url), "blob:ant/%s", uuid);
  ant_value_t url_value = js_mkstr(js, url, strlen(url));

  ant_value_t store = js_get_slot(js->builtins.url_ctor, SLOT_DATA);
  if (vtype(store) != kTypeArray) {
    store = js_mkarr(js);
    js_set_slot_wb(js, js->builtins.url_ctor, SLOT_DATA, store);
  }

  ant_value_t entry = js_mkarr(js);
  js_arr_push(js, entry, url_value);
  js_arr_push(js, entry, args[0]);
  js_arr_push(js, store, entry);
  return url_value;
}

static ant_value_t url_revoke_object_url(ant_params_t) {
  if (nargs < 1)
    return js_mkerr_typed(js, JS_ERR_TYPE, "URL.revokeObjectURL requires at least 1 argument");

  ant_value_t value = url_string_value(js, args[0]);
  if (is_err(value)) return value;
  size_t url_len = 0;
  const char *url = js_getstr(js, value, &url_len);
  if (!url || url_len < 5 || strncmp(url, "blob:", 5) != 0) return js_mkundef();
  url_len = object_url_key_length(url);

  ant_value_t store = js_get_slot(js->builtins.url_ctor, SLOT_DATA);
  if (vtype(store) != kTypeArray) return js_mkundef();

  ant_value_t next = js_mkarr(js);
  ant_offset_t len = js_arr_len(js, store);
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, store, i);
    ant_value_t key = js_arr_get(js, entry, 0);
    size_t key_len = 0;
    const char *key_str = js_getstr(js, key, &key_len);
    if (key_str && key_len == url_len && memcmp(key_str, url, url_len) == 0) continue;
    js_arr_push(js, next, entry);
  }
  js_set_slot_wb(js, js->builtins.url_ctor, SLOT_DATA, next);
  return js_mkundef();
}

static ant_value_t usp_get(ant_params_t) {
  if (nargs < 1) return url_missing_args_error(js, "The \"name\" argument must be specified");
  ant_value_t key_sv = url_string_value(js, args[0]);
  if (is_err(key_sv)) return key_sv;
  const char *key = js_getstr(js, key_sv, NULL);
  if (!key) return js_mknull();
  ant_value_t entries = js_get_slot(js->this_val, SLOT_ENTRIES);
  if (!is_special_object(entries)) return js_mknull();
  ant_offset_t len = js_arr_len(js, entries);
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, entries, i);
    const char *ek = js_getstr(js, js_arr_get(js, entry, 0), NULL);
    if (ek && strcmp(ek, key) == 0) return js_arr_get(js, entry, 1);
  }
  return js_mknull();
}

static ant_value_t usp_getAll(ant_params_t) {
  if (nargs < 1) return url_missing_args_error(js, "The \"name\" argument must be specified");
  ant_value_t result = js_mkarr(js);
  ant_value_t key_sv = url_string_value(js, args[0]);
  if (is_err(key_sv)) return key_sv;
  const char *key = js_getstr(js, key_sv, NULL);
  if (!key) return result;
  ant_value_t entries = js_get_slot(js->this_val, SLOT_ENTRIES);
  if (!is_special_object(entries)) return result;
  ant_offset_t len = js_arr_len(js, entries);
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, entries, i);
    const char *ek = js_getstr(js, js_arr_get(js, entry, 0), NULL);
    if (ek && strcmp(ek, key) == 0)
      js_arr_push(js, result, js_arr_get(js, entry, 1));
  }
  return result;
}

static ant_value_t usp_has(ant_params_t) {
  if (nargs < 1) return url_missing_args_error(js, "The \"name\" argument must be specified");
  ant_value_t key_sv = url_string_value(js, args[0]);
  if (is_err(key_sv)) return key_sv;
  const char *key = js_getstr(js, key_sv, NULL);
  if (!key) return js_false;
  const char *match_val = NULL;
  if (nargs >= 2 && !is_undefined(args[1])) {
    ant_value_t mv_sv = url_string_value(js, args[1]);
    if (is_err(mv_sv)) return mv_sv;
    match_val = js_getstr(js, mv_sv, NULL);
  }
  ant_value_t entries = js_get_slot(js->this_val, SLOT_ENTRIES);
  if (!is_special_object(entries)) return js_false;
  ant_offset_t len = js_arr_len(js, entries);
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, entries, i);
    const char *ek = js_getstr(js, js_arr_get(js, entry, 0), NULL);
    if (!ek || strcmp(ek, key) != 0) continue;
    if (!match_val) return js_true;
    const char *ev = js_getstr(js, js_arr_get(js, entry, 1), NULL);
    if (ev && strcmp(ev, match_val) == 0) return js_true;
  }
  return js_false;
}

static ant_value_t usp_set(ant_params_t) {
  if (nargs < 2) return url_missing_args_error(js, "The \"name\" and \"value\" arguments must be specified");
  ant_value_t key_sv = url_string_value(js, args[0]);
  
  if (is_err(key_sv)) return key_sv;
  ant_value_t val_sv = url_string_value(js, args[1]);
  
  if (is_err(val_sv)) return val_sv;
  const char *key = js_getstr(js, key_sv, NULL);
  
  if (!key) return js_mkundef();
  ant_value_t entries = js_get_slot(js->this_val, SLOT_ENTRIES);
  ant_offset_t len = is_special_object(entries) ? js_arr_len(js, entries) : 0;
  ant_value_t new_entries = js_mkarr(js);
  
  int found = 0;
  for (ant_offset_t i = 0; i < len; i++) {
  ant_value_t entry = js_arr_get(js, entries, i);
  const char *ek = js_getstr(js, js_arr_get(js, entry, 0), NULL);
  
  if (ek && strcmp(ek, key) == 0) {
    if (!found) {
      ant_value_t ne = js_mkarr(js);
      js_arr_push(js, ne, key_sv);
      js_arr_push(js, ne, val_sv);
      js_arr_push(js, new_entries, ne);
      found = 1;
    }
  } else js_arr_push(js, new_entries, entry); }
  
  if (!found) {
    ant_value_t ne = js_mkarr(js);
    js_arr_push(js, ne, key_sv);
    js_arr_push(js, ne, val_sv);
    js_arr_push(js, new_entries, ne);
  }
  
  js_set_slot_wb(js, js->this_val, SLOT_ENTRIES, new_entries);
  usp_push_to_url(js, js->this_val);
  
  return js_mkundef();
}

static ant_value_t usp_append(ant_params_t) {
  if (nargs < 2) return url_missing_args_error(js, "The \"name\" and \"value\" arguments must be specified");
  ant_value_t key_sv = url_string_value(js, args[0]);
  
  if (is_err(key_sv)) return key_sv;
  ant_value_t val_sv = url_string_value(js, args[1]);
  
  if (is_err(val_sv)) return val_sv;
  ant_value_t entries = js_get_slot(js->this_val, SLOT_ENTRIES);
  
  if (!is_special_object(entries)) return js_mkundef();
  ant_value_t entry = js_mkarr(js);
  
  js_arr_push(js, entry, key_sv);
  js_arr_push(js, entry, val_sv);
  js_arr_push(js, entries, entry);
  usp_push_to_url(js, js->this_val);
  
  return js_mkundef();
}

static ant_value_t usp_delete(ant_params_t) {
  if (nargs < 1) return url_missing_args_error(js, "The \"name\" argument must be specified");
  ant_value_t key_sv = url_string_value(js, args[0]);
  
  if (is_err(key_sv)) return key_sv;
  const char *key = js_getstr(js, key_sv, NULL);
  
  if (!key) return js_mkundef();
  const char *match_val = NULL;
  
  if (nargs >= 2 && !is_undefined(args[1])) {
    ant_value_t mv_sv = url_string_value(js, args[1]);
    if (is_err(mv_sv)) return mv_sv;
    match_val = js_getstr(js, mv_sv, NULL);
  }
  
  ant_value_t entries = js_get_slot(js->this_val, SLOT_ENTRIES);
  ant_offset_t len = is_special_object(entries) ? js_arr_len(js, entries) : 0;
  ant_value_t new_entries = js_mkarr(js);
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, entries, i);
    const char *ek = js_getstr(js, js_arr_get(js, entry, 0), NULL);
    if (ek && strcmp(ek, key) == 0) {
      if (!match_val) continue;
      const char *ev = js_getstr(js, js_arr_get(js, entry, 1), NULL);
      if (ev && strcmp(ev, match_val) == 0) continue;
    }
    js_arr_push(js, new_entries, entry);
  }
  
  js_set_slot_wb(js, js->this_val, SLOT_ENTRIES, new_entries);
  usp_push_to_url(js, js->this_val);
  
  return js_mkundef();
}

static ant_value_t usp_toString(ant_params_t) {
  char *s = usp_serialize(js, js->this_val);
  ant_value_t ret = js_mkstr(js, s, strlen(s));
  free(s);
  return ret;
}

static ant_value_t usp_forEach(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0])) {
    char received[160], message[256];
    js_describe_received(js, nargs > 0 ? args[0] : js_mkundef(), received, sizeof(received));
    snprintf(message, sizeof(message), "The \"callback\" argument must be of type function. %s", received);
    return js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_ARG_TYPE", message);
  }
  
  ant_value_t cb = args[0];
  ant_value_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  ant_value_t self = js->this_val;
  ant_value_t entries = js_get_slot(self, SLOT_ENTRIES);
  
  if (!is_special_object(entries)) return js_mkundef();
  ant_offset_t len = js_arr_len(js, entries);
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, entries, i);
    ant_value_t k = js_arr_get(js, entry, 0);
    ant_value_t v = js_arr_get(js, entry, 1);
    ant_value_t cb_args[3] = { v, k, self };
    ant_value_t r = sv_vm_call(js->vm, js, cb, this_arg, cb_args, 3, NULL, js_mkundef());
    if (is_err(r)) return r;
  }
  
  return js_mkundef();
}

static ant_value_t usp_size_get(ant_params_t) {
  ant_value_t entries = js_get_slot(js->this_val, SLOT_ENTRIES);
  if (!is_special_object(entries)) return js_mknum(0);
  return js_mknum((double)js_arr_len(js, entries));
}

static ant_value_t usp_sort(ant_params_t) {
  ant_value_t entries = js_get_slot(js->this_val, SLOT_ENTRIES);
  if (!is_special_object(entries)) return js_mkundef();
  ant_offset_t len = js_arr_len(js, entries);
  if (len <= 1) return js_mkundef();

  ant_value_t *arr = malloc(sizeof(ant_value_t) * (size_t)len);
  if (!arr) return js_mkundef();
  for (ant_offset_t i = 0; i < len; i++) arr[i] = js_arr_get(js, entries, i);

  for (ant_offset_t i = 1; i < len; i++) {
    ant_value_t cur = arr[i];
    const char *ck = js_getstr(js, js_arr_get(js, cur, 0), NULL);
    ant_offset_t j = i;
    
    while (j > 0) {
      const char *jk = js_getstr(js, js_arr_get(js, arr[j - 1], 0), NULL);
      if (strcmp(jk ? jk : "", ck ? ck : "") <= 0) break;
      arr[j] = arr[j - 1]; j--;
    }
    
    arr[j] = cur;
  }

  ant_value_t new_entries = js_mkarr(js);
  for (ant_offset_t i = 0; i < len; i++) js_arr_push(js, new_entries, arr[i]);
  free(arr);

  js_set_slot_wb(js, js->this_val, SLOT_ENTRIES, new_entries);
  usp_push_to_url(js, js->this_val);
  
  return js_mkundef();
}

static ant_value_t usp_iter_next(ant_params_t) {
  ant_value_t state_v = js_get_slot(js->this_val, SLOT_ITER_STATE);
  if (vtype(state_v) != kTypeNumber) return js_iter_result(js, false, js_mkundef());

  uint32_t state = (uint32_t)js_getnum(state_v);
  uint32_t kind = iter_state_kind(state);
  uint32_t idx = iter_state_index(state);

  ant_value_t usp = js_get_slot(js->this_val, SLOT_DATA);
  ant_value_t entries = js_get_slot(usp, SLOT_ENTRIES);
  
  if (!is_special_object(entries) || (ant_offset_t)idx >= js_arr_len(js, entries))
    return js_iter_result(js, false, js_mkundef());

  js_set_slot(js->this_val, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(kind, idx + 1)));

  ant_value_t entry = js_arr_get(js, entries, (ant_offset_t)idx);
  ant_value_t k = js_arr_get(js, entry, 0);
  ant_value_t v = js_arr_get(js, entry, 1);

  ant_value_t out;
  switch (kind) {
  case USP_ITER_KEYS:   out = k; break;
  case USP_ITER_VALUES: out = v; break;
  default: {
    out = js_mkarr(js);
    js_arr_push(js, out, k);
    js_arr_push(js, out, v);
    break;
  }}
  
  return js_iter_result(js, true, out);
}

static ant_value_t make_usp_iter(ant_t *js, ant_value_t usp, int kind) {
  ant_value_t iter = Ant_Object_CreateBare(js);
  js_set_proto_init(iter, js->builtins.usp_iter_proto);
  js_set_slot_wb(js, iter, SLOT_DATA, usp);
  js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(kind, 0)));
  return iter;
}

static ant_value_t usp_entries_fn(ant_params_t) {
  return make_usp_iter(js, js->this_val, USP_ITER_ENTRIES);
}

static ant_value_t usp_keys_fn(ant_params_t) {
  return make_usp_iter(js, js->this_val, USP_ITER_KEYS);
}

static ant_value_t usp_values_fn(ant_params_t) {
  return make_usp_iter(js, js->this_val, USP_ITER_VALUES);
}

static ant_value_t usp_invalid_tuple(ant_t *js) {
  return js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_TUPLE", "Each query pair must be an iterable [name, value] tuple");
}

static ant_value_t usp_add_pair(ant_t *js, ant_value_t entries, ant_value_t pair) {
  if (!is_object_type(pair) && !is_callable(pair)) return usp_invalid_tuple(js);

  ant_value_t items[2] = { js_mkundef(), js_mkundef() };
  size_t count = 0;

  if (vtype(pair) == kTypeArray && !is_proxy(pair)) {
    count = (size_t)js_arr_len(js, pair);
    if (count != 2) return usp_invalid_tuple(js);
    items[0] = js_arr_get(js, pair, 0);
    items[1] = js_arr_get(js, pair, 1);
  } else {
    iterator_t it;
    if (!js_iter_open(js, pair, &it)) {
      if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
      return usp_invalid_tuple(js);
    }
    GC_ROOT_SAVE(root_mark, js);
    GC_ROOT_PIN(js, it.iterator);
    GC_ROOT_PIN(js, it.next_fn);
    ant_value_t value = js_mkundef();
    while (js_iter_next(js, &it, &value)) {
      if (count < 2) {
        items[count] = value;
        GC_ROOT_PIN(js, items[count]);
      }
      count++;
    }
    GC_ROOT_RESTORE(js, root_mark);
    if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
    if (count != 2) return usp_invalid_tuple(js);
  }

  ant_value_t ksv = url_string_value(js, items[0]);
  if (is_err(ksv)) return ksv;
  ant_value_t vsv = url_string_value(js, items[1]);
  if (is_err(vsv)) return vsv;

  ant_value_t entry = js_mkarr(js);
  js_arr_push(js, entry, ksv);
  js_arr_push(js, entry, vsv);
  js_arr_push(js, entries, entry);
  return js_mkundef();
}

static ant_value_t js_URLSearchParams(ant_params_t) {
  if (is_undefined(call_new_target))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Class constructor URLSearchParams cannot be invoked without 'new'");

  ant_value_t obj = Ant_Object_CreateBare(js);
  js_set_proto_init(obj, js->builtins.usp_proto);
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_URLSEARCHPARAMS));
  js_set_slot(obj, SLOT_DATA, js_mkundef());
  
  ant_value_t entries = js_mkarr(js);
  js_set_slot(obj, SLOT_ENTRIES, entries);

  if (nargs < 1 || is_undefined(args[0]) || is_null(args[0])) return obj;

  ant_value_t init = args[0];
  uint8_t t = vtype(init);

  if (t == kTypeString) {
    const char *s = js_getstr(js, init, NULL);
    if (s) {
      const char *q = (s[0] == '?') ? s + 1 : s;
      js_set_slot(obj, SLOT_ENTRIES, parse_query_to_arr(js, q));
    }
    return obj;
  }

  if (!is_object_type(init) && !is_callable(init)) {
    ant_value_t init_sv = url_string_value(js, init);
    if (is_err(init_sv)) return init_sv;
    const char *s = js_getstr(js, init_sv, NULL);
    if (s) js_set_slot(obj, SLOT_ENTRIES, parse_query_to_arr(js, s[0] == '?' ? s + 1 : s));
    return obj;
  }

  if (usp_is_urlsearchparams(js, init)) {
    ant_value_t src = js_get_slot(init, SLOT_ENTRIES);
    ant_offset_t len = vtype(src) == kTypeArray ? js_arr_len(js, src) : 0;
    for (ant_offset_t i = 0; i < len; i++) {
      ant_value_t entry = js_arr_get(js, src, i);
      ant_value_t ne = js_mkarr(js);
      js_arr_push(js, ne, js_arr_get(js, entry, 0));
      js_arr_push(js, ne, js_arr_get(js, entry, 1));
      js_arr_push(js, entries, ne);
    }
    return obj;
  }

  ant_value_t iter_fn = js_get_sym(js, init, js->sym.iterator_sym);
  if (is_err(iter_fn)) return iter_fn;
  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);

  if (!is_undefined(iter_fn) && !is_null(iter_fn)) {
    if (!is_callable(iter_fn))
      return js_mkerr_code(js, JS_ERR_TYPE, "ERR_ARG_NOT_ITERABLE", "Query pairs must be iterable");

    GC_ROOT_SAVE(root_mark, js);
    GC_ROOT_PIN(js, obj);
    GC_ROOT_PIN(js, entries);
    ant_value_t result = obj;

    if (vtype(init) == kTypeArray && !is_proxy(init)) {
      ant_offset_t len = js_arr_len(js, init);
      for (ant_offset_t i = 0; i < len; i++) {
        result = usp_add_pair(js, entries, js_arr_get(js, init, i));
        if (is_err(result)) break;
        result = obj;
      }
    } else {
      iterator_t it;
      if (!js_iter_open(js, init, &it)) result = Ant_Exception_Pending(js)
        ? Ant_Exception_Current(js)
        : js_mkerr_code(js, JS_ERR_TYPE, "ERR_ARG_NOT_ITERABLE", "Query pairs must be iterable");
      else {
        GC_ROOT_PIN(js, it.iterator);
        GC_ROOT_PIN(js, it.next_fn);
        ant_value_t pair = js_mkundef();
        while (js_iter_next(js, &it, &pair)) {
          result = usp_add_pair(js, entries, pair);
          if (is_err(result)) { js_iter_close(js, &it); break; }
          result = obj;
        }
        if (!is_err(result) && Ant_Exception_Pending(js)) result = Ant_Exception_Current(js);
      }
    }

    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  ant_iter_t it = js_prop_iter_begin(js, init);
  const char *key;
  size_t key_len;
  ant_value_t val;

  while (js_prop_iter_next(&it, &key, &key_len, &val)) {
    val = js_getprop_fallback_len(js, init, key, key_len);
    if (is_err(val)) { js_prop_iter_end(&it); return val; }
    ant_value_t sv = url_string_value(js, val);
    if (is_err(sv)) { js_prop_iter_end(&it); return sv; }
    ant_value_t entry = js_mkarr(js);
    js_arr_push(js, entry, js_mkstr(js, key, key_len));
    js_arr_push(js, entry, sv);
    js_arr_push(js, entries, entry);
  }

  js_prop_iter_end(&it);
  return obj;
}

void init_url_module(ant_t *js) {
  js->builtins.usp_iter_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.usp_iter_proto, js->sym.iterator_proto);
  js_set(js, js->builtins.usp_iter_proto, "next", Ant_Function_Create(usp_iter_next));
  js_set_descriptor(js, js->builtins.usp_iter_proto, "next", 4, JS_DESC_W | JS_DESC_E | JS_DESC_C);
  js_set_sym(js, js->builtins.usp_iter_proto, js->sym.iterator_sym, Ant_Function_Create(sym_this_cb));

  js->builtins.usp_proto = Ant_Object_CreateBare(js);
  js_set(js, js->builtins.usp_proto, "get",      Ant_Function_CreateWithArity(usp_get, 1));
  js_set(js, js->builtins.usp_proto, "getAll",   Ant_Function_CreateWithArity(usp_getAll, 1));
  js_set(js, js->builtins.usp_proto, "has",      Ant_Function_CreateWithArity(usp_has, 1));
  js_set(js, js->builtins.usp_proto, "set",      Ant_Function_CreateWithArity(usp_set, 2));
  js_set(js, js->builtins.usp_proto, "append",   Ant_Function_CreateWithArity(usp_append, 2));
  js_set(js, js->builtins.usp_proto, "delete",   Ant_Function_CreateWithArity(usp_delete, 1));
  js_set(js, js->builtins.usp_proto, "sort",     Ant_Function_Create(usp_sort));
  js_set(js, js->builtins.usp_proto, "toString", Ant_Function_Create(usp_toString));
  js_set(js, js->builtins.usp_proto, "forEach",  Ant_Function_CreateWithArity(usp_forEach, 1));
  js_set_getter_desc(js, js->builtins.usp_proto, "size", 4, Ant_Function_Create(usp_size_get), JS_DESC_C);

  js_set(js, js->builtins.usp_proto, "entries", Ant_Function_Create(usp_entries_fn));
  js_set(js, js->builtins.usp_proto, "keys",    Ant_Function_Create(usp_keys_fn));
  js_set(js, js->builtins.usp_proto, "values",  Ant_Function_Create(usp_values_fn));
  
  js_set_sym(js, js->builtins.usp_proto, js->sym.iterator_sym, js_get(js, js->builtins.usp_proto, "entries"));
  js_set_tostringtag(js, js->builtins.usp_proto, js_mkstr(js, "URLSearchParams", 15));

  ant_value_t usp_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_URLSearchParams, js->builtins.usp_proto, "URLSearchParams", 0);
  js_set_global_builtin(js, "URLSearchParams", usp_ctor);

  js->builtins.url_proto = Ant_Object_CreateBare(js);
  js_set_accessor_desc(js, js->builtins.url_proto, "href", 4, Ant_Function_Create(url_get_href), Ant_Function_Create(url_set_href), JS_DESC_E | JS_DESC_C);
  js_set_accessor_desc(js, js->builtins.url_proto, "protocol", 8, Ant_Function_Create(url_get_protocol), Ant_Function_Create(url_set_protocol), JS_DESC_E | JS_DESC_C);
  js_set_accessor_desc(js, js->builtins.url_proto, "username", 8,  Ant_Function_Create(url_get_username), Ant_Function_Create(url_set_username), JS_DESC_E | JS_DESC_C);
  js_set_accessor_desc(js, js->builtins.url_proto, "password", 8,  Ant_Function_Create(url_get_password), Ant_Function_Create(url_set_password), JS_DESC_E | JS_DESC_C);
  js_set_accessor_desc(js, js->builtins.url_proto, "host", 4,  Ant_Function_Create(url_get_host), Ant_Function_Create(url_set_host), JS_DESC_E | JS_DESC_C);
  js_set_accessor_desc(js, js->builtins.url_proto, "hostname", 8,  Ant_Function_Create(url_get_hostname), Ant_Function_Create(url_set_hostname), JS_DESC_E | JS_DESC_C);
  js_set_accessor_desc(js, js->builtins.url_proto, "port", 4,  Ant_Function_Create(url_get_port), Ant_Function_Create(url_set_port), JS_DESC_E | JS_DESC_C);
  js_set_accessor_desc(js, js->builtins.url_proto, "pathname", 8,  Ant_Function_Create(url_get_pathname), Ant_Function_Create(url_set_pathname), JS_DESC_E | JS_DESC_C);
  js_set_accessor_desc(js, js->builtins.url_proto, "search", 6,  Ant_Function_Create(url_get_search), Ant_Function_Create(url_set_search), JS_DESC_E | JS_DESC_C);
  js_set_accessor_desc(js, js->builtins.url_proto, "hash", 4,  Ant_Function_Create(url_get_hash), Ant_Function_Create(url_set_hash), JS_DESC_E | JS_DESC_C);
  
  js_set_getter_desc(js, js->builtins.url_proto, "origin", 6,  Ant_Function_Create(url_get_origin), JS_DESC_E | JS_DESC_C);
  js_set_getter_desc(js, js->builtins.url_proto, "searchParams", 12, Ant_Function_Create(url_get_searchParams), JS_DESC_E | JS_DESC_C);
  
  js_set(js, js->builtins.url_proto, "toString", Ant_Function_Create(url_toString));
  js_set(js, js->builtins.url_proto, "toJSON",   Ant_Function_Create(url_toString));
  js_set_tostringtag(js, js->builtins.url_proto, js_mkstr(js, "URL", 3));

  js->builtins.url_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_URL, js->builtins.url_proto, "URL", 1);
  js_set_slot(js->builtins.url_ctor, SLOT_DATA, js_mkarr(js));
  
  js_set(js, js->builtins.url_ctor, "canParse",        Ant_Function_CreateWithArity(url_canParse, 1));
  js_set(js, js->builtins.url_ctor, "createObjectURL", Ant_Function_CreateWithArity(url_create_object_url, 1));
  js_set(js, js->builtins.url_ctor, "parse",           Ant_Function_CreateWithArity(url_parse, 1));
  js_set(js, js->builtins.url_ctor, "revokeObjectURL", Ant_Function_CreateWithArity(url_revoke_object_url, 1));
  js_set_global_builtin(js, "URL", js->builtins.url_ctor);
}

#if defined(__APPLE__)
static const char url_platform_name[] = "darwin";
#elif defined(__linux__)
static const char url_platform_name[] = "linux";
#elif defined(__FreeBSD__)
static const char url_platform_name[] = "freebsd";
#else
static const char url_platform_name[] = "unknown";
#endif

static ant_value_t url_invalid_arg_type(ant_t *js, const char *expected, ant_value_t value) {
  char received[160], message[288];
  js_describe_received(js, value, received, sizeof(received));
  snprintf(message, sizeof(message), "The \"path\" argument must be of type %s. %s", expected, received);
  return js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_ARG_TYPE", message);
}

static bool url_prop_truthy(ant_t *js, ant_value_t obj, const char *name, ant_value_t *err) {
  ant_value_t value = js_get(js, obj, name);
  if (is_err(value)) { *err = value; return false; }
  return js_truthy(js, value);
}

static bool url_prop_undefined(ant_t *js, ant_value_t obj, const char *name, ant_value_t *err) {
  ant_value_t value = js_get(js, obj, name);
  if (is_err(value)) { *err = value; return false; }
  return is_undefined(value);
}

int Ant_URL_IsURLLike(ant_t *js, ant_value_t value, ant_value_t *err) {
  if (!is_object_type(value)) return 0;
  *err = js_mkundef();
  bool ok =
    url_prop_truthy(js, value, "href", err) &&
    url_prop_truthy(js, value, "protocol", err) &&
    url_prop_undefined(js, value, "auth", err) &&
    url_prop_undefined(js, value, "path", err);
  if (is_err(*err)) return -1;
  return ok ? 1 : 0;
}

static bool url_prop_equals(ant_t *js, ant_value_t obj, const char *name, const char *expected, ant_value_t *err) {
  ant_value_t value = js_get(js, obj, name);
  if (is_err(value)) { *err = value; return false; }
  if (vtype(value) != kTypeString) return false;
  size_t len = 0;
  const char *str = js_getstr(js, value, &len);
  return str && len == strlen(expected) && memcmp(str, expected, len) == 0;
}

ant_value_t Ant_URL_FileURLToPath(ant_t *js, ant_value_t input) {
  ant_value_t url = input;
  ant_value_t err = js_mkundef();

  if (vtype(input) == kTypeString) {
    size_t len = 0;
    const char *str = js_getstr(js, input, &len);
    url_state_t *s = calloc(1, sizeof(url_state_t));
    if (!s) return js_mkerr(js, "out of memory");
    if (!str || parse_url_to_state_n(str, len, NULL, 0, s) != 0) {
      free(s);
      return url_invalid_url_error(js, input, js_mkundef());
    }
    url = make_url_obj(js, s);
  } else {
    int like = Ant_URL_IsURLLike(js, input, &err);
    if (like < 0) return err;
    if (!like) return url_invalid_arg_type(js, "string or an instance of URL", input);
  }

  if (!url_prop_equals(js, url, "protocol", "file:", &err)) {
    if (is_err(err)) return err;
    return js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_URL_SCHEME", "The URL must be of scheme file");
  }

  if (!url_prop_equals(js, url, "hostname", "", &err)) {
    if (is_err(err)) return err;
    char message[96];
    snprintf(message, sizeof(message), "File URL host must be \"localhost\" or empty on %s", url_platform_name);
    return js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_FILE_URL_HOST", message);
  }

  ant_value_t pathname = js_get(js, url, "pathname");
  if (is_err(pathname)) return pathname;
  if (is_undefined(pathname) || is_null(pathname))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot read properties of %s (reading 'length')", is_null(pathname) ? "null" : "undefined");

  bool is_string = vtype(pathname) == kTypeString;
  ant_value_t text = is_string ? pathname : js_tostring_val(js, pathname);
  if (is_err(text)) return text;

  size_t len = 0;
  const char *path = js_getstr(js, text, &len);
  if (!path || !memchr(path, '%', len)) return pathname;

  for (size_t i = 0; is_string && i + 2 < len; i++) {
    if (path[i] == '%' && path[i + 1] == '2' && (path[i + 2] | 0x20) == 'f')
      return js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_FILE_URL_PATH", "File URL path must not include encoded / characters");
  }

  return js_decodeURIComponent(js, &text, 1, js_mkundef());
}

static ant_value_t builtin_fileURLToPath(ant_params_t) {
  return Ant_URL_FileURLToPath(js, nargs > 0 ? args[0] : js_mkundef());
}

static bool url_path_needs_escape(unsigned char c) {
  if (c <= 0x20 || c >= 0x7f) return true;
  switch (c) {
    case '"': case '#': case '%': case '<': case '>': case '?': case '[': case '\\':
    case ']': case '^': case '`': case '{': case '|': case '}': case '~':
      return true;
    default:
      return false;
  }
}

static bool url_resolve_posix_path(url_fmt_buf_t *out, const char *path, size_t len) {
  url_fmt_buf_t joined = {0};
  bool ok = true;

  if (len == 0 || path[0] != '/') {
    char cwd[4096];
    size_t cwd_len = sizeof(cwd);
    if (uv_cwd(cwd, &cwd_len) != 0) return false;
    ok = url_fmt_append_n(&joined, cwd, cwd_len) && url_fmt_append_c(&joined, '/');
  }
  ok = ok && url_fmt_append_n(&joined, path, len);
  if (!ok) { free(joined.buf); return false; }

  const char *p = joined.buf ? joined.buf : "";
  size_t n = joined.len;
  size_t i = 0;

  while (i < n && ok) {
    while (i < n && p[i] == '/') i++;
    size_t start = i;
    while (i < n && p[i] != '/') i++;
    size_t seg = i - start;
    if (seg == 0 || (seg == 1 && p[start] == '.')) continue;
    if (seg == 2 && p[start] == '.' && p[start + 1] == '.') {
      while (out->len > 0 && out->buf[out->len - 1] != '/') out->len--;
      if (out->len > 0) out->len--;
      continue;
    }
    ok = url_fmt_append_c(out, '/') && url_fmt_append_n(out, p + start, seg);
  }

  free(joined.buf);
  if (ok && out->len == 0) ok = url_fmt_append_c(out, '/');
  if (ok) out->buf[out->len] = '\0';
  return ok;
}

static ant_value_t builtin_pathToFileURL(ant_params_t) {
  ant_value_t input = nargs > 0 ? args[0] : js_mkundef();
  if (vtype(input) != kTypeString) return url_invalid_arg_type(js, "string", input);

  size_t len = 0;
  const char *path = js_getstr(js, input, &len);
  char *utf8 = wtf8_str_to_utf8_dup(path, &len);
  if (utf8) path = utf8;

  url_fmt_buf_t resolved = {0};
  url_fmt_buf_t href = {0};
  bool ok = url_resolve_posix_path(&resolved, path, len);
  if (ok && len > 0 && path[len - 1] == '/' && resolved.buf[resolved.len - 1] != '/')
    ok = url_fmt_append_c(&resolved, '/');
  free(utf8);

  ok = ok && url_fmt_append_n(&href, "file://", 7);
  for (size_t i = 0; ok && i < resolved.len; i++) {
    static const char hex[] = "0123456789ABCDEF";
    unsigned char c = (unsigned char)resolved.buf[i];
    if (!url_path_needs_escape(c)) ok = url_fmt_append_c(&href, (char)c);
    else ok = url_fmt_append_c(&href, '%') && url_fmt_append_c(&href, hex[c >> 4]) && url_fmt_append_c(&href, hex[c & 15]);
  }
  
  free(resolved.buf);
  if (!ok) { free(href.buf); return js_mkerr(js, "out of memory"); }

  url_state_t *s = calloc(1, sizeof(url_state_t));
  if (!s) { free(href.buf); return js_mkerr(js, "out of memory"); }
  
  if (parse_url_to_state_n(href.buf, href.len, NULL, 0, s) != 0) {
    ant_value_t href_val = js_mkstr(js, href.buf, href.len);
    free(href.buf);
    free(s);
    return url_invalid_url_error(js, href_val, js_mkundef());
  }

  free(href.buf);
  return make_url_obj(js, s);
}

static ant_value_t builtin_domainToASCII(ant_params_t) {
  if (nargs < 1) {
    return url_missing_args_error(js, "The \"domain\" argument must be specified");
  }

  ant_value_t domain = js_template_to_string(js, args[0]);
  if (is_err(domain)) return domain;
  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);

  size_t len = 0;
  const char *value = js_getstr(js, domain, &len);
  if (!value || len == 0) return js_mkstr(js, "", 0);

  ada_url url = ada_parse("ws://x", 6);
  if (!ada_set_hostname(url, value, len)) {
    ada_free(url);
    return js_mkstr(js, "", 0);
  }

  ada_string hostname = ada_get_hostname(url);
  ant_value_t result = js_mkstr(js, hostname.data, hostname.length);
  ada_free(url);
  
  return result;
}

static ant_value_t builtin_domainToUnicode(ant_params_t) {
  if (nargs < 1) return url_missing_args_error(js, "The \"domain\" argument must be specified");

  ant_value_t domain = js_template_to_string(js, args[0]);
  if (is_err(domain)) return domain;
  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);

  size_t len = 0;
  const char *value = js_getstr(js, domain, &len);
  if (!value || len == 0) return js_mkstr(js, "", 0);

  ada_url url = ada_parse("ws://x", 6);
  if (!ada_set_hostname(url, value, len)) {
    ada_free(url);
    return js_mkstr(js, "", 0);
  }

  ada_string hostname = ada_get_hostname(url);
  ada_owned_string uni = ada_idna_to_unicode(hostname.data, hostname.length);
  ant_value_t result = js_mkstr(js, uni.data ? uni.data : "", uni.data ? uni.length : 0);
  ada_free_owned_string(uni);
  ada_free(url);

  return result;
}

static ant_value_t builtin_url_format_href(ant_params_t) {
  if (nargs < 1 || vtype(args[0]) != kTypeString) return js_mkstr(js, "", 0);

  bool fragment = nargs > 1 && js_truthy(js, args[1]);
  bool unicode = nargs > 2 && js_truthy(js, args[2]);
  bool search = nargs > 3 && js_truthy(js, args[3]);
  bool auth = nargs > 4 && js_truthy(js, args[4]);

  size_t href_len = 0;
  const char *href = js_getstr(js, args[0], &href_len);
  if (!href) return js_mkstr(js, "", 0);

  ada_url url = ada_parse(href, href_len);
  if (!url || !ada_is_valid(url)) {
    if (url) ada_free(url);
    return js_mkstr(js, "", 0);
  }

  if (!fragment) ada_clear_hash(url);
  if (!search) ada_clear_search(url);
  
  if (!auth) {
    ada_set_username(url, "", 0);
    ada_set_password(url, "", 0);
  }

  ada_string out = ada_get_href(url);
  ant_value_t ret = js_mkstr(js, out.data, out.length);

  if (unicode && ada_has_hostname(url)) {
    ada_string hostname = ada_get_hostname(url);
    ada_owned_string uni = ada_idna_to_unicode(hostname.data, hostname.length);
    const char *dslash = out.length > 2 ? strstr(out.data, "//") : NULL;
    size_t at = dslash ? (size_t)(dslash - out.data) + 2 : out.length;

    if (dslash && ada_has_credentials(url)) {
      const char *sep = memchr(out.data + at, '@', out.length - at);
      at = sep ? (size_t)(sep - out.data) + 1 : out.length;
    }

    if (
      uni.data && at + hostname.length <= out.length &&
      memcmp(out.data + at, hostname.data, hostname.length) == 0
    ) {
      url_fmt_buf_t b = {0};
      bool ok =
        url_fmt_append_n(&b, out.data, at) &&
        url_fmt_append_n(&b, uni.data, uni.length) &&
        url_fmt_append_n(&b, out.data + at + hostname.length, out.length - at - hostname.length);
      if (ok) ret = js_mkstr(js, b.buf ? b.buf : "", b.len);
      free(b.buf);
    }
    ada_free_owned_string(uni);
  }

  ada_free(url);
  return ret;
}

ant_value_t internal_url_library(ant_t *js) {
  ant_value_t lib = Ant_Object_CreateBare(js);
  ant_value_t glob = js_glob(js);

  js_set(js, lib, "URL", js_get(js, glob, "URL"));
  js_set(js, lib, "URLSearchParams", js_get(js, glob, "URLSearchParams"));
  js_set(js, lib, "fileURLToPath", Ant_Function_CreateWithArity(builtin_fileURLToPath, 1));
  js_set(js, lib, "pathToFileURL", Ant_Function_CreateWithArity(builtin_pathToFileURL, 2));
  js_set(js, lib, "domainToASCII", Ant_Function_CreateWithArity(builtin_domainToASCII, 1));
  js_set(js, lib, "domainToUnicode", Ant_Function_CreateWithArity(builtin_domainToUnicode, 1));
  js_set(js, lib, "formatHref", Ant_Function_CreateWithArity(builtin_url_format_href, 5));
  
  legacy_url_register(js, lib);
  js_set(js, lib, "default", lib);

  return lib;
}
