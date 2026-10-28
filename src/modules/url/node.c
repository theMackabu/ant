#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <uv.h>

#include "ant.h"
#include "utf8.h"
#include "cfunc.h"
#include "errors.h"
#include "internal.h"
#include "modules/url.h"
#include "modules/uri.h"
#include "url_internal.h"

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
