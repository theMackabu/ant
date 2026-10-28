#include <compat.h> // IWYU pragma: keep
#include <ada_c.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "ascii.h"
#include "cfunc.h"
#include "internal.h"
#include "utf8.h"
#include "silver/call.h"
#include "url_internal.h"

enum {
  LP_PROTOCOL,
  LP_SLASHES,
  LP_AUTH,
  LP_HOST,
  LP_PORT,
  LP_HOSTNAME,
  LP_HASH,
  LP_SEARCH,
  LP_QUERY,
  LP_PATHNAME,
  LP_PATH,
  LP_HREF,
  LP_FIELD_COUNT,
};

enum {
  LP_FLAG_SIMPLE = 1 << 12,
  LP_FLAG_PARSE_QUERY = 1 << 13,
  LP_FLAG_HOST_BRANCH = 1 << 14,
  LP_FLAG_IPV6 = 1 << 15,
  LP_STATUS_SHIFT = 16,
  LP_STATUS_INVALID_PORT = 1,
  LP_STATUS_INVALID_URL = 2,
};

typedef struct {
  const char *ptr;
  size_t len;
} lp_slice_t;

typedef struct {
  ant_t *js;
  ant_value_t target;
  ant_value_t values[LP_FIELD_COUNT];
  uint32_t assigned;
  uint32_t flags;
  int hostname_truthy;
  url_fmt_buf_t bufs[6];
} lp_state_t;

static lp_slice_t lp_slice(const char *ptr, size_t len) {
  return (lp_slice_t){ ptr, len };
}

static lp_slice_t lp_sub(lp_slice_t s, size_t from, size_t to) {
  return lp_slice(s.ptr + from, to - from);
}

static ssize_t lp_find(lp_slice_t s, char c) {
  const char *hit = s.len ? memchr(s.ptr, c, s.len) : NULL;
  return hit ? hit - s.ptr : -1;
}

static ssize_t lp_find_last(lp_slice_t s, char c) {
  for (size_t i = s.len; i > 0; i--) if (s.ptr[i - 1] == c) return (ssize_t)(i - 1);
  return -1;
}

static bool lp_eq(lp_slice_t s, const char *lit) {
  size_t n = strlen(lit);
  return s.len == n && memcmp(s.ptr, lit, n) == 0;
}

static lp_slice_t lp_build(url_fmt_buf_t *b, lp_slice_t first, lp_slice_t second, lp_slice_t third) {
  b->len = 0;
  url_fmt_append_n(b, first.ptr, first.len);
  url_fmt_append_n(b, second.ptr, second.len);
  url_fmt_append_n(b, third.ptr, third.len);
  return lp_slice(b->buf ? b->buf : "", b->len);
}

static size_t lp_ws_prefix(const uint8_t *p, size_t n) {
  if (p[0] < 33) return 1;
  if (n >= 2 && p[0] == 0xc2 && p[1] == 0xa0) return 2;
  if (n >= 3 && p[0] == 0xef && p[1] == 0xbb && p[2] == 0xbf) return 3;
  return 0;
}

static size_t lp_ws_suffix(const uint8_t *p, size_t n) {
  if (p[n - 1] < 33) return 1;
  if (n >= 2 && p[n - 2] == 0xc2 && p[n - 1] == 0xa0) return 2;
  if (n >= 3 && p[n - 3] == 0xef && p[n - 2] == 0xbb && p[n - 1] == 0xbf) return 3;
  return 0;
}

static bool lp_js_space_at(const uint8_t *p, size_t n) {
  uint8_t c = p[0];
  if (c < 0x80) return c == ' ' || (c >= '\t' && c <= '\r');
  if (c == 0xc2) return n >= 2 && p[1] == 0xa0;
  if (n < 3) return false;
  if (c == 0xe1) return p[1] == 0x9a && p[2] == 0x80;
  if (c == 0xe2 && p[1] == 0x80) return p[2] <= 0x8a || p[2] == 0xa8 || p[2] == 0xa9 || p[2] == 0xaf;
  if (c == 0xe2) return p[1] == 0x81 && p[2] == 0x9f;
  if (c == 0xe3) return p[1] == 0x80 && p[2] == 0x80;
  if (c == 0xef) return p[1] == 0xbb && p[2] == 0xbf;
  return false;
}

static bool lp_has_js_space(lp_slice_t s, size_t from, size_t to) {
  for (size_t i = from; i < to; i++) if (lp_js_space_at((const uint8_t *)s.ptr + i, to - i)) return true;
  return false;
}

static bool lp_is_slashed(lp_slice_t proto) {
  static const char *const names[] = { "http:", "https:", "ftp:", "gopher:", "file:", "ws:", "wss:" };
  if (!proto.ptr) return false;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) if (lp_eq(proto, names[i])) return true;
  return false;
}

static bool lp_is_hostless(lp_slice_t proto) {
  return proto.ptr && lp_eq(proto, "javascript:");
}

static bool lp_is_host_ending(uint8_t c) {
  return c == '#' || c == '/' || c == '?';
}

static bool lp_is_non_host(uint8_t c) {
  switch (c) {
    case ' ': case '"': case '%': case '\'': case ';': case '<': case '>':
    case '\\': case '^': case '`': case '{': case '|': case '}':
      return true;
    default:
      return false;
  }
}

static bool lp_is_forbidden_host(uint8_t c, bool ipv6) {
  switch (c) {
    case 0: case '\t': case '\n': case '\r': case ' ': case '#': case '%': case '/':
    case '<': case '>': case '?': case '@': case '\\': case '^': case '|':
      return true;
    case ':': case '[': case ']':
      return !ipv6;
    default:
      return false;
  }
}

static const char *lp_auto_escape(uint8_t c) {
  switch (c) {
    case '\t': return "%09";
    case '\n': return "%0A";
    case '\r': return "%0D";
    case ' ': return "%20";
    case '"': return "%22";
    case '\'': return "%27";
    case '<': return "%3C";
    case '>': return "%3E";
    case '\\': return "%5C";
    case '^': return "%5E";
    case '`': return "%60";
    case '{': return "%7B";
    case '|': return "%7C";
    case '}': return "%7D";
    default: return NULL;
  }
}

static bool lp_host_pattern(lp_slice_t s) {
  if (s.len < 2 || s.ptr[0] != '/' || s.ptr[1] != '/') return false;
  size_t i = 2;
  while (i < s.len && s.ptr[i] != '@' && s.ptr[i] != '/') i++;
  if (i == 2 || i >= s.len || s.ptr[i] != '@') return false;
  return i + 1 < s.len && s.ptr[i + 1] != '@' && s.ptr[i + 1] != '/';
}

static size_t lp_utf16_len(lp_slice_t s) {
  size_t units = 0;
  for (size_t i = 0; i < s.len; i++) {
    uint8_t c = (uint8_t)s.ptr[i];
    if ((c & 0xc0) != 0x80) units += c >= 0xf0 ? 2 : 1;
  }
  return units;
}

static bool lp_is_ascii(lp_slice_t s) {
  for (size_t i = 0; i < s.len; i++) if ((uint8_t)s.ptr[i] >= 0x80) return false;
  return true;
}

static void lp_assign(lp_state_t *st, int field, ant_value_t value) {
  st->values[field] = value;
  st->assigned |= 1u << field;
}

static void lp_assign_slice(lp_state_t *st, int field, lp_slice_t s) {
  lp_assign(st, field, js_mkstr(st->js, s.ptr ? s.ptr : "", s.len));
}

static ant_value_t lp_current(lp_state_t *st, int field, const char *name) {
  if (st->assigned & (1u << field)) return st->values[field];
  if (!is_object_type(st->target)) return js_mkundef();
  return js_get(st->js, st->target, name);
}

static ant_value_t lp_result(lp_state_t *st, uint32_t flags) {
  ant_value_t items[LP_FIELD_COUNT + 1];
  items[0] = js_mknum((double)(flags | st->flags | st->assigned));
  for (int i = 0; i < LP_FIELD_COUNT; i++) items[i + 1] = st->values[i];
  return js_mkarr_dense_literal(st->js, items, LP_FIELD_COUNT + 1);
}

static ant_value_t lp_fail(lp_state_t *st, int status) {
  return lp_result(st, (uint32_t)status << LP_STATUS_SHIFT);
}

static ant_value_t lp_oom(lp_state_t *st) {
  return js_mkerr(st->js, "out of memory");
}

static lp_slice_t lp_string_slice(ant_t *js, ant_value_t value) {
  size_t len = 0;
  const char *ptr = js_getstr(js, value, &len);
  return ptr ? lp_slice(ptr, len) : lp_slice("", 0);
}

static ant_value_t lp_null_proto_object(ant_t *js) {
  ant_value_t obj = Ant_Object_CreateBare(js);
  js_set_proto_init(obj, js_mknull());
  return obj;
}

static ant_value_t lp_to_lower(lp_state_t *st, ant_value_t hostname, lp_slice_t s) {
  if (lp_is_ascii(s)) {
    url_fmt_buf_t *b = &st->bufs[4];
    b->len = 0;
    if (!url_fmt_reserve(b, s.len)) return lp_oom(st);
    for (size_t i = 0; i < s.len; i++) b->buf[i] = (char)ant_ascii_to_lower((unsigned char)s.ptr[i]);
    b->len = s.len;
    return js_mkstr(st->js, b->buf, b->len);
  }
  ant_value_t fn = js_get(st->js, st->js->sym.string_proto, "toLowerCase");
  if (is_err(fn)) return fn;
  return sv_vm_call_explicit_this(st->js->vm, st->js, fn, hostname, NULL, 0);
}

static ant_value_t lp_to_ascii(ant_t *js, lp_slice_t s) {
  size_t len = s.len;
  char *utf8 = wtf8_to_utf8_dup(s.ptr, &len);
  ada_owned_string ascii = ada_idna_to_ascii(utf8 ? utf8 : s.ptr, len);
  free(utf8);
  ant_value_t out = js_mkstr(js, ascii.data ? ascii.data : "", ascii.data ? ascii.length : 0);
  ada_free_owned_string(ascii);
  return out;
}

static bool lp_match_simple(lp_slice_t rest, size_t *pathname_end) {
  if (!rest.len || rest.ptr[0] != '/') return false;

  size_t i = 1;
  if (rest.len > 1 && rest.ptr[1] == '/') {
    if (rest.len > 2 && rest.ptr[2] == '/') return false;
    i = 2;
  }

  for (; i < rest.len && rest.ptr[i] != '?'; i++)
    if (lp_js_space_at((const uint8_t *)rest.ptr + i, rest.len - i)) return false;
  if (i < rest.len && lp_has_js_space(rest, i, rest.len)) return false;

  *pathname_end = i;
  return true;
}

static ant_value_t lp_parse_simple(lp_state_t *st, lp_slice_t rest, size_t pathname_end, bool parse_query) {
  ant_value_t whole = js_mkstr(st->js, rest.ptr, rest.len);
  lp_assign(st, LP_PATH, whole);
  lp_assign(st, LP_HREF, whole);
  lp_assign_slice(st, LP_PATHNAME, lp_sub(rest, 0, pathname_end));

  uint32_t flags = LP_FLAG_SIMPLE;
  if (pathname_end < rest.len) {
    lp_assign_slice(st, LP_SEARCH, lp_sub(rest, pathname_end, rest.len));
    lp_assign_slice(st, LP_QUERY, lp_sub(rest, pathname_end + 1, rest.len));
    if (parse_query) flags |= LP_FLAG_PARSE_QUERY;
  } else if (parse_query) {
    lp_assign(st, LP_SEARCH, js_mknull());
    lp_assign(st, LP_QUERY, lp_null_proto_object(st->js));
  }

  return lp_result(st, flags);
}

static ant_value_t lp_parse_host(lp_state_t *st, lp_slice_t *rest_io) {
  ant_t *js = st->js;
  lp_slice_t rest = *rest_io;
  ssize_t at_sign = -1, non_host = -1;
  size_t removed_at = rest.len;

  for (size_t i = 0; i < rest.len; i++) {
    uint8_t c = (uint8_t)rest.ptr[i];
    if (c == '\t' || c == '\n' || c == '\r') { removed_at = i; break; }
    if (lp_is_host_ending(c)) { if (non_host == -1) non_host = (ssize_t)i; break; }
    if (c == '@') { at_sign = (ssize_t)i; non_host = -1; }
    else if (lp_is_non_host(c) && non_host == -1) non_host = (ssize_t)i;
  }

  if (removed_at < rest.len) {
    url_fmt_buf_t *b = &st->bufs[2];
    b->len = 0;
    if (!url_fmt_reserve(b, rest.len)) return lp_oom(st);
    at_sign = -1;
    non_host = -1;
    size_t i = 0;
    for (; i < rest.len; i++) {
      uint8_t c = (uint8_t)rest.ptr[i];
      if (c == '\t' || c == '\n' || c == '\r') continue;
      ssize_t w = (ssize_t)b->len;
      b->buf[b->len++] = (char)c;
      if (lp_is_host_ending(c)) { if (non_host == -1) non_host = w; i++; break; }
      if (c == '@') { at_sign = w; non_host = -1; }
      else if (lp_is_non_host(c) && non_host == -1) non_host = w;
    }
    memcpy(b->buf + b->len, rest.ptr + i, rest.len - i);
    b->len += rest.len - i;
    rest = lp_slice(b->buf, b->len);
  }

  size_t host_start = 0;
  if (at_sign != -1) {
    lp_assign_slice(st, LP_AUTH, lp_sub(rest, 0, (size_t)at_sign));
    host_start = (size_t)at_sign + 1;
  }

  lp_slice_t host;
  if (non_host == -1) {
    host = lp_sub(rest, host_start, rest.len);
    rest = lp_slice("", 0);
  } else {
    host = lp_sub(rest, host_start, (size_t)non_host);
    rest = lp_sub(rest, (size_t)non_host, rest.len);
  }
  lp_assign_slice(st, LP_HOST, host);

  ssize_t colon = lp_find_last(host, ':');
  if (colon != -1) {
    bool digits = true;
    for (size_t i = (size_t)colon + 1; i < host.len && digits; i++) digits = ant_ascii_is_digit((unsigned char)host.ptr[i]);
    if (digits) {
      if ((size_t)colon + 1 < host.len) lp_assign_slice(st, LP_PORT, lp_sub(host, (size_t)colon + 1, host.len));
      host = lp_sub(host, 0, (size_t)colon);
    }
  }
  if (host.len) lp_assign_slice(st, LP_HOSTNAME, host);

  ant_value_t hostname = lp_current(st, LP_HOSTNAME, "hostname");
  if (is_err(hostname)) return hostname;
  if (vtype(hostname) != kTypeString) {
    hostname = js_mkstr(js, "", 0);
    lp_assign(st, LP_HOSTNAME, hostname);
  }

  lp_slice_t name = lp_string_slice(js, hostname);
  bool ipv6 = name.len > 0 && name.ptr[0] == '[' && name.ptr[name.len - 1] == ']';

  if (!ipv6) {
    for (size_t i = 0; i < name.len; i++) {
      uint8_t c = (uint8_t)name.ptr[i];
      if (c != '/' && c != '\\' && c != '#' && c != '?' && c != ':') continue;
      if (c == ':') return lp_fail(st, LP_STATUS_INVALID_PORT);
      rest = lp_build(&st->bufs[3], lp_slice("/", 1), lp_sub(name, i, name.len), rest);
      hostname = js_mkstr(js, name.ptr, i);
      lp_assign(st, LP_HOSTNAME, hostname);
      name = lp_string_slice(js, hostname);
      break;
    }
  }

  if (name.len > 255 && lp_utf16_len(name) > 255) hostname = js_mkstr(js, "", 0);
  else hostname = lp_to_lower(st, hostname, name);
  if (is_err(hostname)) return hostname;
  lp_assign(st, LP_HOSTNAME, hostname);
  name = lp_string_slice(js, hostname);

  if (name.len) {
    if (!ipv6) {
      hostname = lp_to_ascii(js, name);
      lp_assign(st, LP_HOSTNAME, hostname);
      name = lp_string_slice(js, hostname);
      if (!name.len) return lp_fail(st, LP_STATUS_INVALID_URL);
    }
    for (size_t i = 0; i < name.len; i++)
      if (lp_is_forbidden_host((uint8_t)name.ptr[i], ipv6)) return lp_fail(st, LP_STATUS_INVALID_URL);
  }

  st->flags |= LP_FLAG_HOST_BRANCH;
  st->hostname_truthy = name.len > (ipv6 ? 2u : 0u);
  if (ipv6) {
    st->flags |= LP_FLAG_IPV6;
    if (!rest.len || rest.ptr[0] != '/') rest = lp_build(&st->bufs[3], lp_slice("/", 1), rest, lp_slice("", 0));
  }

  *rest_io = rest;
  return js_mkundef();
}

static ant_value_t lp_parse(lp_state_t *st, ant_value_t input, bool parse_query, bool slashes_denote_host) {
  ant_t *js = st->js;
  lp_slice_t full = lp_string_slice(js, input);

  size_t start = 0, end = full.len, n;
  while (start < end && (n = lp_ws_prefix((const uint8_t *)full.ptr + start, end - start))) start += n;
  while (end > start && (n = lp_ws_suffix((const uint8_t *)full.ptr + start, end - start))) end -= n;

  lp_slice_t rest = lp_sub(full, start, end);
  ssize_t hash_at = lp_find(rest, '#');
  ssize_t question_at = lp_find(rest, '?');
  ssize_t split_at = hash_at == -1 ? question_at : (question_at == -1 || hash_at < question_at) ? hash_at : question_at;
  size_t head = split_at == -1 ? rest.len : (size_t)split_at;
  bool has_hash = hash_at != -1;
  bool has_at = lp_find(lp_sub(rest, 0, head), '@') != -1;

  if (lp_find(lp_sub(rest, 0, head), '\\') != -1) {
    url_fmt_buf_t *b = &st->bufs[0];
    if (!url_fmt_reserve(b, rest.len)) return lp_oom(st);
    for (size_t i = 0; i < rest.len; i++) b->buf[i] = i < head && rest.ptr[i] == '\\' ? '/' : rest.ptr[i];
    b->len = rest.len;
    rest = lp_slice(b->buf, b->len);
  }

  size_t pathname_end = 0;
  if (!slashes_denote_host && !has_hash && !has_at && lp_match_simple(rest, &pathname_end))
    return lp_parse_simple(st, rest, pathname_end, parse_query);

  lp_slice_t proto = {0};
  lp_slice_t lower_proto = {0};
  size_t scheme = 0;
  while (scheme < rest.len) {
    unsigned char c = (unsigned char)rest.ptr[scheme];
    if (!ant_ascii_is_alnum(c) && c != '.' && c != '+' && c != '-') break;
    scheme++;
  }
  if (scheme > 0 && scheme < rest.len && rest.ptr[scheme] == ':') {
    proto = lp_sub(rest, 0, scheme + 1);
    url_fmt_buf_t *b = &st->bufs[1];
    if (!url_fmt_reserve(b, proto.len)) return lp_oom(st);
    for (size_t i = 0; i < proto.len; i++) b->buf[i] = (char)ant_ascii_to_lower((unsigned char)proto.ptr[i]);
    b->len = proto.len;
    lower_proto = lp_slice(b->buf, b->len);
    lp_assign_slice(st, LP_PROTOCOL, lower_proto);
    rest = lp_sub(rest, proto.len, rest.len);
  }

  bool slashes = false;
  if (slashes_denote_host || proto.ptr || lp_host_pattern(rest)) {
    slashes = rest.len >= 2 && rest.ptr[0] == '/' && rest.ptr[1] == '/';
    if (slashes && !lp_is_hostless(lower_proto)) {
      rest = lp_sub(rest, 2, rest.len);
      lp_assign(st, LP_SLASHES, js_true);
    }
  }

  if (!lp_is_hostless(lower_proto) && (slashes || (proto.ptr && !lp_is_slashed(proto)))) {
    ant_value_t res = lp_parse_host(st, &rest);
    if (is_err(res) || !is_undefined(res)) return res;
  }

  if (!lp_is_hostless(lower_proto)) {
    size_t i = 0;
    while (i < rest.len && !lp_auto_escape((uint8_t)rest.ptr[i])) i++;
    if (i < rest.len) {
      url_fmt_buf_t *b = &st->bufs[5];
      b->len = 0;
      if (!url_fmt_append_n(b, rest.ptr, i)) return lp_oom(st);
      for (; i < rest.len; i++) {
        const char *escaped = lp_auto_escape((uint8_t)rest.ptr[i]);
        if (!(escaped ? url_fmt_append_n(b, escaped, 3) : url_fmt_append_c(b, rest.ptr[i]))) return lp_oom(st);
      }
      rest = lp_slice(b->buf, b->len);
    }
  }

  ssize_t hash_idx = lp_find(rest, '#');
  ssize_t question_idx = lp_find(rest, '?');
  if (hash_idx != -1) {
    lp_assign_slice(st, LP_HASH, lp_sub(rest, (size_t)hash_idx, rest.len));
    if (question_idx > hash_idx) question_idx = -1;
  }

  uint32_t flags = 0;
  if (question_idx != -1) {
    size_t search_end = hash_idx == -1 ? rest.len : (size_t)hash_idx;
    lp_assign_slice(st, LP_SEARCH, lp_sub(rest, (size_t)question_idx, search_end));
    lp_assign_slice(st, LP_QUERY, lp_sub(rest, (size_t)question_idx + 1, search_end));
    if (parse_query) flags |= LP_FLAG_PARSE_QUERY;
  } else if (parse_query) {
    lp_assign(st, LP_SEARCH, js_mknull());
    lp_assign(st, LP_QUERY, lp_null_proto_object(js));
  }

  ssize_t first_idx = question_idx != -1 ? question_idx : hash_idx;
  if (first_idx == -1) {
    if (rest.len) lp_assign_slice(st, LP_PATHNAME, rest);
  } else if (first_idx > 0) {
    lp_assign_slice(st, LP_PATHNAME, lp_sub(rest, 0, (size_t)first_idx));
  }

  if (lp_is_slashed(lower_proto)) {
    bool hostname_truthy = st->hostname_truthy > 0;
    if (st->hostname_truthy < 0) {
      ant_value_t hostname = lp_current(st, LP_HOSTNAME, "hostname");
      if (is_err(hostname)) return hostname;
      hostname_truthy = js_truthy(js, hostname);
    }
    if (hostname_truthy) {
      ant_value_t pathname = lp_current(st, LP_PATHNAME, "pathname");
      if (is_err(pathname)) return pathname;
      if (!js_truthy(js, pathname)) lp_assign_slice(st, LP_PATHNAME, lp_slice("/", 1));
    }
  }

  return lp_result(st, flags);
}

static ant_value_t builtin_legacy_url_parse(ant_params_t) {
  ant_value_t input = nargs > 1 ? args[1] : js_mkundef();
  if (vtype(input) != kTypeString) return js_mkerr_typed(js, JS_ERR_TYPE, "url must be a string");

  lp_state_t st = { .js = js, .target = nargs > 0 ? args[0] : js_mkundef(), .hostname_truthy = -1 };
  for (int i = 0; i < LP_FIELD_COUNT; i++) st.values[i] = js_mkundef();

  ant_value_t result = lp_parse(&st, input, nargs > 2 && js_truthy(js, args[2]), nargs > 3 && js_truthy(js, args[3]));
  for (size_t i = 0; i < sizeof(st.bufs) / sizeof(st.bufs[0]); i++) free(st.bufs[i].buf);
  return result;
}

void legacy_url_register(ant_t *js, ant_value_t lib) {
  js_set(js, lib, "legacyParse", Ant_Function_CreateWithArity(builtin_legacy_url_parse, 4));
}
