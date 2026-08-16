#include <compat.h> // IWYU pragma: keep
#include <ada_c.h>
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "ant.h"
#include "internal.h"
#include "modules/url.h"
#include "url_internal.h"

typedef struct {
  const char *ptr;
  size_t len;
} url_slice_t;

typedef struct {
  url_slice_t protocol, auth, host, port, hostname;
  url_slice_t hash, search, query, pathname, path, href;
  bool slashes;
} legacy_url_t;

static url_slice_t url_slice(const char *ptr, size_t len) {
  return (url_slice_t){ ptr, len };
}

static url_slice_t url_slice_buf(const url_fmt_buf_t *b) {
  return b->len ? url_slice(b->buf, b->len) : (url_slice_t){0};
}

static void legacy_url_set(ant_t *js, ant_value_t obj, const char *name, url_slice_t s) {
  js_set(js, obj, name, s.ptr ? js_mkstr(js, s.ptr, s.len) : js_mknull());
}

static ant_value_t legacy_url_object(ant_t *js, const legacy_url_t *u) {
  ant_value_t obj = js_mkobj(js);

  legacy_url_set(js, obj, "protocol", u->protocol);
  js_set(js, obj, "slashes", u->slashes ? js_true : js_mknull());
  legacy_url_set(js, obj, "auth", u->auth);
  legacy_url_set(js, obj, "host", u->host);
  legacy_url_set(js, obj, "port", u->port);
  legacy_url_set(js, obj, "hostname", u->hostname);
  legacy_url_set(js, obj, "hash", u->hash);
  legacy_url_set(js, obj, "search", u->search);
  legacy_url_set(js, obj, "query", u->query);
  legacy_url_set(js, obj, "pathname", u->pathname);
  legacy_url_set(js, obj, "path", u->path);
  legacy_url_set(js, obj, "href", u->href);

  return obj;
}

static bool legacy_proto_is(const char *proto, size_t len, const char *name) {
  size_t name_len = strlen(name);
  if (len && proto[len - 1] == ':') len--;
  return len == name_len && strncmp(proto, name, name_len) == 0;
}

static bool legacy_proto_is_slashed(const char *proto, size_t len) {
  return
    legacy_proto_is(proto, len, "http")  || legacy_proto_is(proto, len, "https") ||
    legacy_proto_is(proto, len, "ftp")   || legacy_proto_is(proto, len, "gopher") ||
    legacy_proto_is(proto, len, "file")  || legacy_proto_is(proto, len, "ws") ||
    legacy_proto_is(proto, len, "wss");
}

static bool legacy_proto_is_hostless(const char *proto, size_t len) {
  return legacy_proto_is(proto, len, "javascript");
}

static const char *legacy_auto_escape(unsigned char c) {
  switch (c) {
    case '\t': return "%09";
    case '\n': return "%0A";
    case '\r': return "%0D";
    case ' ':  return "%20";
    case '"':  return "%22";
    case '\'': return "%27";
    case '<':  return "%3C";
    case '>':  return "%3E";
    case '\\': return "%5C";
    case '^':  return "%5E";
    case '`':  return "%60";
    case '{':  return "%7B";
    case '|':  return "%7C";
    case '}':  return "%7D";
    default:   return NULL;
  }
}

static bool legacy_auth_is_safe(unsigned char c) {
  return isalnum(c) || (c && strchr("!'()*-._~:", (char)c) != NULL);
}

static bool legacy_host_char_is_stripped(unsigned char c) {
  return c == '\t' || c == '\n' || c == '\r';
}

static bool legacy_host_char_is_invalid(unsigned char c) {
  switch (c) {
    case ' ': case '"': case '%': case '\'':
    case ';': case '<': case '>': case '\\': case '^': case '`':
    case '{': case '|': case '}': return true;
    default: return false;
  }
}

static size_t legacy_scheme_len(const char *value, size_t len) {
  size_t i = 0;
  for (; i < len; i++) {
    unsigned char c = (unsigned char)value[i];
    if (isalnum(c) || c == '.' || c == '+' || c == '-') continue;
    return c == ':' && i > 0 ? i + 1 : 0;
  }
  return 0;
}

static bool legacy_has_auth_authority(const char *rest, size_t len) {
  size_t i = 2;
  size_t user_len = 0;

  if (len < 2 || rest[0] != '/' || rest[1] != '/') return false;
  for (; i < len && rest[i] != '@' && rest[i] != '/'; i++) user_len++;
  if (!user_len || i >= len || rest[i] != '@') return false;

  return i + 1 < len && rest[i + 1] != '@' && rest[i + 1] != '/';
}

static bool legacy_is_ws(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static bool legacy_simple_path(const char *s, size_t len, size_t *path_len, bool *has_search) {
  size_t i = 1;

  if (!len || s[0] != '/') return false;
  if (i < len && s[i] == '/') {
    if (i + 1 < len && s[i + 1] == '/') return false;
    i++;
  }

  while (i < len && s[i] != '?' && !legacy_is_ws((unsigned char)s[i])) i++;
  if (i < len && legacy_is_ws((unsigned char)s[i])) return false;

  *path_len = i;
  *has_search = i < len;
  if (i == len) return true;

  for (size_t j = i + 1; j < len; j++)
    if (legacy_is_ws((unsigned char)s[j])) return false;

  return true;
}

static void legacy_query_add(
  ant_t *js, ant_value_t obj, const char *k, size_t k_len, const char *v, size_t v_len
) {
  // TODO: k_len is unused for now: js_get/js_set take NUL-terminated keys, and property keys
  // with an embedded NUL are truncated by the intern layer regardless of what this site
  // does (docs/exec-plans/active/nul-keys-and-array-includes.md, part 1). The decoder
  // already produces the length, so it is threaded through to make this site correct the
  // moment length-carrying key lookups exist.
  (void)k_len;
  ant_value_t existing = js_get(js, obj, k);
  ant_value_t value = js_mkstr(js, v, v_len);

  if (is_undefined(existing)) {
    js_set(js, obj, k, value);
    return;
  }

  if (vtype(existing) == T_ARR) {
    js_arr_push(js, existing, value);
    return;
  }

  ant_value_t arr = js_mkarr(js);
  js_arr_push(js, arr, existing);
  js_arr_push(js, arr, value);
  js_set(js, obj, k, arr);
}

static ant_value_t legacy_query_object(ant_t *js, const char *query, size_t len) {
  ant_value_t obj = js_mkobj(js);
  size_t start = 0;

  for (size_t i = 0; i <= len; i++) {
    if (i != len && query[i] != '&') continue;
    if (i > start) {
      size_t pair_len = i - start;
      const char *pair = query + start;
      const char *eq = memchr(pair, '=', pair_len);
      char *raw_k = strndup(pair, eq ? (size_t)(eq - pair) : pair_len);
      char *raw_v = eq ? strndup(eq + 1, pair_len - (size_t)(eq - pair) - 1) : strdup("");

      if (raw_k && raw_v) {
        size_t k_len = 0, v_len = 0;
        char *k = form_urldecode_len(raw_k, &k_len);
        char *v = form_urldecode_len(raw_v, &v_len);
        if (k && v) legacy_query_add(js, obj, k, k_len, v, v_len);
        free(k); free(v);
      }

      free(raw_k); free(raw_v);
    }
    start = i + 1;
  }

  return obj;
}

typedef struct {
  url_fmt_buf_t protocol;
  url_fmt_buf_t hostname;
  url_fmt_buf_t host;
  url_fmt_buf_t path;
  url_fmt_buf_t tail;
  url_fmt_buf_t input;
  char *auth;
  char *idna;
} legacy_parse_scratch_t;

static void legacy_parse_scratch_free(legacy_parse_scratch_t *s) {
  free(s->protocol.buf);
  free(s->hostname.buf);
  free(s->host.buf);
  free(s->path.buf);
  free(s->tail.buf);
  free(s->input.buf);
  free(s->auth);
  free(s->idna);
}

ant_value_t legacy_url_parse_impl(
  ant_t *js,
  const char *input,
  size_t input_len,
  bool parse_query,
  bool slashes_denote_host
) {
  legacy_parse_scratch_t scratch = {0};
  legacy_url_t url = {0};
  ant_value_t obj = 0;

  const char *rest = input;
  size_t rest_len = input_len;

  url_slice_t proto = {0}, hostname = {0}, port = {0};
  bool slashes = false;
  bool has_query_object = false;
  bool host_parsed = false;
  bool ipv6_hostname = false;
  bool has_hash = false;
  bool has_at = false;
  bool strip_tail_ws = false;

  while (rest_len && (unsigned char)rest[0] <= ' ') { rest++; rest_len--; }
  while (rest_len && (unsigned char)rest[rest_len - 1] <= ' ') rest_len--;

  bool has_backslash = false;
  size_t head = 0;

  for (; head < rest_len; head++) {
    char c = rest[head];
    if (c == '?' || c == '#') break;
    if (c == '@') has_at = true;
    else if (c == '\\') has_backslash = true;
  }

  has_hash = (head < rest_len && rest[head] == '#') ||
    memchr(rest + head, '#', rest_len - head) != NULL;

  if (has_backslash) {
    for (size_t i = 0; i < rest_len; i++) if (
      !url_fmt_append_c(&scratch.input, i < head && rest[i] == '\\' ? '/' : rest[i])
    ) goto oom;

    rest = scratch.input.buf;
    rest_len = scratch.input.len;
  }

  if (!slashes_denote_host && !has_hash && !has_at) {
    size_t path_len = 0;
    bool has_search = false;

    if (legacy_simple_path(rest, rest_len, &path_len, &has_search)) {
      url.pathname = url_slice(rest, path_len);
      url.path = url_slice(rest, rest_len);
      url.href = url.path;
      
      if (has_search) url.search = url_slice(rest + path_len, rest_len - path_len);
      obj = legacy_url_object(js, &url);

      if (has_search && parse_query)
        js_set(js, obj, "query", legacy_query_object(js, url.search.ptr + 1, url.search.len - 1));
      else if (has_search)
        js_set(js, obj, "query", js_mkstr(js, url.search.ptr + 1, url.search.len - 1));
      else if (parse_query)
        js_set(js, obj, "query", js_mkobj(js));

      legacy_parse_scratch_free(&scratch);
      return obj;
    }
  }

  {
    size_t scheme_len = legacy_scheme_len(rest, rest_len);
    if (scheme_len) {
      for (size_t i = 0; i < scheme_len; i++)
        if (!url_fmt_append_c(&scratch.protocol, (char)tolower((unsigned char)rest[i]))) goto oom;
      proto = url_slice(scratch.protocol.buf, scratch.protocol.len);
      rest += scheme_len;
      rest_len -= scheme_len;
    }
  }

  if (slashes_denote_host || proto.ptr || legacy_has_auth_authority(rest, rest_len)) {
    bool leading = rest_len >= 2 && rest[0] == '/' && rest[1] == '/';
    if (leading && !(proto.ptr && legacy_proto_is_hostless(proto.ptr, proto.len))) {
      rest += 2;
      rest_len -= 2;
      slashes = true;
    }
  }

  if (
    !(proto.ptr && legacy_proto_is_hostless(proto.ptr, proto.len)) &&
    (slashes || (proto.ptr && !legacy_proto_is_slashed(proto.ptr, proto.len)))
  ) {
    ssize_t at_sign = -1, host_end = -1, non_host = -1;
    size_t start = 0;
    size_t host_len = 0;
    const char *host = NULL;

    host_parsed = true;

    for (size_t i = 0; i < rest_len; i++) {
      unsigned char c = (unsigned char)rest[i];

      if (c == '@') { at_sign = (ssize_t)i; non_host = -1; continue; }
      if (c == '#' || c == '/' || c == '?') {
        if (non_host == -1) non_host = (ssize_t)i;
        host_end = (ssize_t)i;
        break;
      }
      if (legacy_host_char_is_invalid(c) && non_host == -1) non_host = (ssize_t)i;
    }

    if (at_sign != -1) {
      char *raw = NULL;

      for (ssize_t i = 0; i < at_sign; i++) {
        if (rest[i] != '%') continue;
        if (
          i + 2 < at_sign &&
          isxdigit((unsigned char)rest[i + 1]) &&
          isxdigit((unsigned char)rest[i + 2])
        ) { i += 2; continue; }

        legacy_parse_scratch_free(&scratch);
        return js_mkerr_typed(js, JS_ERR_URI, "URI malformed");
      }

      raw = strndup(rest, (size_t)at_sign);
      if (!raw) goto oom;
      size_t auth_len = 0;
      scratch.auth = url_decode_component_len(raw, &auth_len);
      free(raw);
      if (!scratch.auth) goto oom;

      url.auth = url_slice(scratch.auth, auth_len);
      start = (size_t)at_sign + 1;
    }

    strip_tail_ws = non_host != -1 && (host_end == -1 || non_host < host_end);

    if (non_host == -1) {
      host = rest + start;
      host_len = rest_len - start;
      rest += rest_len;
      rest_len = 0;
    } else {
      host = rest + start;
      host_len = (size_t)non_host - start;
      rest += (size_t)non_host;
      rest_len -= (size_t)non_host;
    }

    for (size_t i = 0; i < host_len; i++) {
      unsigned char c = (unsigned char)host[i];
      if (legacy_host_char_is_stripped(c)) continue;
      if (!url_fmt_append_c(&scratch.hostname, (char)tolower(c))) goto oom;
    }

    hostname = url_slice(scratch.hostname.buf ? scratch.hostname.buf : "", scratch.hostname.len);

    size_t scan_from = 0;
    size_t colon = hostname.len;

    if (hostname.len && hostname.ptr[0] == '[') {
      const char *close = memchr(hostname.ptr, ']', hostname.len);
      if (close) scan_from = (size_t)(close - hostname.ptr) + 1;
    }

    for (size_t i = hostname.len; i > scan_from; i--)
      if (hostname.ptr[i - 1] == ':') { colon = i - 1; break; }

    if (colon < hostname.len) {
      size_t port_len = hostname.len - colon - 1;

      for (size_t i = 0; i < port_len; i++)
        if (!isdigit((unsigned char)hostname.ptr[colon + 1 + i])) {
          legacy_parse_scratch_free(&scratch);
          return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid port in url");
        }

      if (port_len) port = url_slice(hostname.ptr + colon + 1, port_len);
      hostname = url_slice(hostname.ptr, colon);
    }

    if (hostname.len >= 2 && hostname.ptr[0] == '[' && hostname.ptr[hostname.len - 1] == ']') {
      url.hostname = url_slice(hostname.ptr + 1, hostname.len - 2);
      ipv6_hostname = true;
    } else {
      bool non_ascii = false;
      for (size_t i = 0; i < hostname.len; i++)
        if ((unsigned char)hostname.ptr[i] >= 0x80) { non_ascii = true; break; }

      if (non_ascii) {
        ada_owned_string ascii = ada_idna_to_ascii(hostname.ptr, hostname.len);
        if (!ascii.data || !ascii.length) {
          ada_free_owned_string(ascii);
          legacy_parse_scratch_free(&scratch);
          ant_value_t props = js_mkobj(js);
          js_set(js, props, "code", js_mkstr(js, "ERR_INVALID_URL", 15));
          js_set(js, props, "input", js_mkstr(js, input, input_len));
          return js_mkerr_props(js, JS_ERR_TYPE, props, "Invalid URL");
        }
        scratch.idna = strndup(ascii.data, ascii.length);
        ada_free_owned_string(ascii);
        if (!scratch.idna) goto oom;
        hostname = url_slice(scratch.idna, strlen(scratch.idna));
      }

      url.hostname = hostname;
    }

    if (!ipv6_hostname) {
    for (size_t i = 0; i < hostname.len; i++) {
      char c = hostname.ptr[i];
      if (c != ':' && c != '[' && c != ']') continue;
      legacy_parse_scratch_free(&scratch);
      return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid port in url");
    }}

    url.port = port;
  }

  if (!(proto.ptr && legacy_proto_is_hostless(proto.ptr, proto.len))) {
    if (ipv6_hostname && (!rest_len || rest[0] != '/')) if (!url_fmt_append_c(&scratch.tail, '/')) goto oom;

    for (size_t i = 0; i < rest_len; i++) {
      const char *escaped = NULL;
      bool ok = true;

      if (strip_tail_ws && legacy_host_char_is_stripped((unsigned char)rest[i])) continue;
      escaped = legacy_auto_escape((unsigned char)rest[i]);
      
      ok = escaped
        ? url_fmt_append(&scratch.tail, escaped)
        : url_fmt_append_c(&scratch.tail, rest[i]);
      if (!ok) goto oom;
    }

    rest = scratch.tail.buf ? scratch.tail.buf : "";
    rest_len = scratch.tail.len;
  }

  const char *hash = memchr(rest, '#', rest_len);
  if (hash) {
    url.hash = url_slice(hash, rest_len - (size_t)(hash - rest));
    rest_len = (size_t)(hash - rest);
  }

  const char *qm = memchr(rest, '?', rest_len);
  if (qm) {
    size_t search_len = rest_len - (size_t)(qm - rest);
    url.search = url_slice(qm, search_len);
    url.query = url_slice(qm + 1, search_len - 1);
    rest_len = (size_t)(qm - rest);
    has_query_object = parse_query;
  } else if (parse_query) has_query_object = true;

  if (rest_len) url.pathname = url_slice(rest, rest_len);
  
  if (
    proto.ptr && legacy_proto_is_slashed(proto.ptr, proto.len) &&
    hostname.len && !url.pathname.ptr
  ) url.pathname = url_slice("/", 1);

  if (url.pathname.ptr || url.search.ptr) {
    if (!url_fmt_append_n(&scratch.path, url.pathname.ptr, url.pathname.len)) goto oom;
    if (!url_fmt_append_n(&scratch.path, url.search.ptr, url.search.len)) goto oom;
    url.path = url_slice_buf(&scratch.path);
  }

  url.protocol = proto;
  url.slashes = slashes;

  if (host_parsed) {
    if (
      !url_fmt_append_n(&scratch.host, hostname.ptr, hostname.len) ||
      (port.len && (!url_fmt_append_c(&scratch.host, ':') ||
                    !url_fmt_append_n(&scratch.host, port.ptr, port.len)))
    ) goto oom;

    url.host = url_slice(scratch.host.buf ? scratch.host.buf : "", scratch.host.len);
  }

  {
    url_fmt_buf_t out = {0};
    bool slashed_proto = proto.ptr && legacy_proto_is_slashed(proto.ptr, proto.len);
    bool authority = slashes || (slashed_proto && url.host.len);
    bool file_authority =
      !authority && slashed_proto && legacy_proto_is(proto.ptr, proto.len, "file");
    bool want_slashes = authority || file_authority;
    bool root_pathname =
      authority && url.pathname.len && url.pathname.ptr[0] != '/';

    if (
      !url_fmt_append_n(&out, proto.ptr, proto.len) ||
      (want_slashes && !url_fmt_append_n(&out, "//", 2))
    ) { free(out.buf); goto oom; }

    for (size_t i = 0; url.host.len && i < url.auth.len; i++) {
      unsigned char c = (unsigned char)url.auth.ptr[i];
      char hex[4];

      if (legacy_auth_is_safe(c)) {
        if (!url_fmt_append_c(&out, (char)c)) { free(out.buf); goto oom; }
        continue;
      }

      snprintf(hex, sizeof(hex), "%%%02X", c);
      if (!url_fmt_append_n(&out, hex, 3)) { free(out.buf); goto oom; }
    }

    if (
      (url.host.len && url.auth.len && !url_fmt_append_c(&out, '@')) ||
      !url_fmt_append_n(&out, url.host.ptr, url.host.len) ||
      (root_pathname && !url_fmt_append_c(&out, '/')) ||
      !url_fmt_append_n(&out, url.pathname.ptr, url.pathname.len) ||
      !url_fmt_append_n(&out, url.search.ptr, url.search.len) ||
      !url_fmt_append_n(&out, url.hash.ptr, url.hash.len)
    ) { free(out.buf); goto oom; }

    url.href = url_slice(out.buf ? out.buf : "", out.len);
    obj = legacy_url_object(js, &url);
    free(out.buf);
  }

  if (has_query_object) {
  if (url.search.ptr) js_set(js, obj, "query", legacy_query_object(js, url.query.ptr, url.query.len));
  else {
    js_set(js, obj, "search", js_mknull());
    js_set(js, obj, "query", js_mkobj(js));
  }}

  legacy_parse_scratch_free(&scratch);
  return obj;

oom:
  legacy_parse_scratch_free(&scratch);
  return js_mkerr(js, "allocation failure");
}