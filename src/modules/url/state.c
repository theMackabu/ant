#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "ant.h"
#include "utf8.h"
#include "ptr.h"
#include "errors.h"
#include "internal.h"
#include "modules/url.h"
#include "url_internal.h"

url_state_t *url_get_state(ant_value_t obj) {
  return (url_state_t *)js_get_native(obj, URL_NATIVE_TAG);
}

void url_state_clear(url_state_t *s) {
  if (!s) return;
  if (s->storage) free(s->storage);
  else {
    free(s->href);
    free(s->protocol);
    free(s->username);
    free(s->password);
    free(s->hostname);
    free(s->port);
    free(s->pathname);
    free(s->search);
    free(s->hash);
  }
  memset(s, 0, sizeof(*s));
}

void url_free_state(url_state_t *s) {
  if (!s) return;
  url_state_clear(s);
  free(s);
}

void url_finalize(ant_t *js, ant_object_t *obj) {
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
  return form_urldecode_n(str, str ? strlen(str) : 0, out_len);
}

char *form_urldecode_n(const char *str, size_t len, size_t *out_len) {
  if (out_len) *out_len = 0;
  if (!str) return strdup("");
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

int ada_url_to_state(ada_url url, url_state_t *s) {
  ada_string parts[9] = {
    ada_get_href(url), ada_get_protocol(url), ada_get_username(url), ada_get_password(url),
    ada_get_hostname(url), ada_get_port(url), ada_get_pathname(url), ada_get_search(url), ada_get_hash(url),
  };
  
  if (parts[7].length == 0 && ada_has_search(url)) parts[7] = (ada_string){.data = "?", .length = 1};
  if (parts[8].length == 0 && ada_has_hash(url)) parts[8] = (ada_string){.data = "#", .length = 1};
  
  size_t total = 0;
  for (int i = 0; i < 9; i++) total += parts[i].length + 1;
  
  char *storage = malloc(total);
  if (!storage) {
    memset(s, 0, sizeof(*s));
    return -1;
  }
  
  char *fields[9];
  char *at = storage;
  for (int i = 0; i < 9; i++) {
    fields[i] = at;
    if (parts[i].length) memcpy(at, parts[i].data, parts[i].length);
    at[parts[i].length] = '\0';
    at += parts[i].length + 1;
  }
  
  *s = (url_state_t){
    .href = fields[0], .protocol = fields[1], .username = fields[2], .password = fields[3],
    .hostname = fields[4], .port = fields[5], .pathname = fields[6], .search = fields[7], .hash = fields[8],
    .storage = storage,
  };
  
  return 0;
}

int parse_url_to_state_n(
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

bool url_can_parse_n(const char *url_str, size_t url_len, const char *base_str, size_t base_len) {
  if (!url_str) return false;
  
  char *url_utf8 = wtf8_to_utf8_dup(url_str, &url_len);
  char *base_utf8 = base_str ? wtf8_to_utf8_dup(base_str, &base_len) : NULL;
  
  if (url_utf8) url_str = url_utf8;
  if (base_utf8) base_str = base_utf8;
  
  bool ok = base_str
    ? ada_can_parse_with_base(url_str, url_len, base_str, base_len)
    : ada_can_parse(url_str, url_len);
  
  free(url_utf8);
  free(base_utf8);
  
  return ok;
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

// WebIDL USVString: each lone surrogate becomes U+FFFD
ant_value_t url_string_value(ant_t *js, ant_value_t value) {
  ant_value_t str = vtype(value) == kTypeString ? value : js_template_to_string(js, value);
  if (vtype(str) != kTypeString) return str;

  size_t len = 0;
  const char *bytes = js_getstr(js, str, &len);
  if (!bytes || !wtf8_has_surrogate(bytes, len)) return str;

  char *usv = wtf8_to_utf8_dup(bytes, &len);
  if (!usv) return Ant_Error_CreateOutOfMemory(js);
  ant_value_t result = js_mkstr(js, usv, len);
  free(usv);
  return result;
}

ant_value_t url_invalid_url_error(ant_t *js, ant_value_t input, ant_value_t base) {
  ant_value_t props = Ant_Object_CreateBare(js);
  js_set(js, props, "code", js_mkstr(js, "ERR_INVALID_URL", 15));
  js_set(js, props, "input", input);
  if (!is_undefined(base)) js_set(js, props, "base", base);
  return js_mkerr_props(js, JS_ERR_TYPE, props, "Invalid URL");
}

ant_value_t url_missing_args_error(ant_t *js, const char *message) {
  return js_mkerr_code(js, JS_ERR_TYPE, "ERR_MISSING_ARGS", message);
}

bool ada_set_search_bool(ada_url url, const char *value, size_t len) {
  ada_set_search(url, value, len);
  return true;
}

bool ada_set_hash_bool(ada_url url, const char *value, size_t len) {
  ada_set_hash(url, value, len);
  return true;
}

ant_value_t url_apply_setter(
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
    return Ant_Value_Undefined;
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
  return Ant_Value_Undefined;
}
