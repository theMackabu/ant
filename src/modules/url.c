#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <uriparser/Uri.h>

#include "ant.h"
#include "ptr.h"
#include "errors.h"
#include "internal.h"
#include "runtime.h"
#include "descriptors.h"

#include "silver/engine.h"
#include "modules/url.h"
#include "modules/symbol.h"

static ant_value_t g_url_proto = 0;
static ant_value_t g_usp_proto = 0;
static ant_value_t g_usp_iter_proto = 0;

enum { URL_NATIVE_TAG = 0x55524c53u }; // URLS

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
  free(s->protocol); free(s->username); free(s->password);
  free(s->hostname); free(s->port);    free(s->pathname);
  free(s->search);   free(s->hash);
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

static int default_port_for(const char *proto) {
  if (!proto) return -1;
  if (strcmp(proto, "http:") == 0  || strcmp(proto, "ws:")  == 0) return 80;
  if (strcmp(proto, "https:") == 0 || strcmp(proto, "wss:") == 0) return 443;
  if (strcmp(proto, "ftp:") == 0) return 21;
  return -1;
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

static bool url_base_is_opaque(const char *base_str, const char *proto) {
  const char *after_colon = NULL;

  if (!base_str || is_special_scheme(proto)) return false;
  after_colon = strchr(base_str, ':');
  if (!after_colon) return false;
  after_colon++;
  
  return *after_colon != '/' && *after_colon != '\0';
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

char *form_urlencode(const char *str) {
  if (!str) return strdup("");
  return form_urlencode_n(str, strlen(str));
}

char *form_urldecode(const char *str) {
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
  return out;
}

char *url_decode_component(const char *str) {
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
  return out;
}

static char *userinfo_encode(const char *str) {
  if (!str) return strdup("");
  size_t len = strlen(str);
  char *out = malloc(len * 3 + 1);
  
  if (!out) return strdup("");
  size_t j = 0;
  
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)str[i];
    if (
      isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' ||
      c == '!' || c == '$' || c == '&' || c == '\'' || c == '(' ||
      c == ')' || c == '*' || c == '+' || c == ',' || c == ';' ||
      c == '=' || c == ':'
    ) out[j++] = (char)c; else { snprintf(out + j, 4, "%%%02X", c); j += 3; }
  }
  
  out[j] = '\0';
  return out;
}

static char *uri_range_dup(const UriTextRangeA *r) {
  if (!r->first || !r->afterLast) return strdup("");
  return strndup(r->first, (size_t)(r->afterLast - r->first));
}

static bool url_has_brackets_in_query_or_fragment(const char *url_str) {
  size_t len = strlen(url_str);
  bool in_query = false;
  bool in_fragment = false;

  for (size_t i = 0; i < len; i++) {
    char c = url_str[i];
    if (c == '#' && !in_fragment) {
      in_query = false;
      in_fragment = true;
      continue;
    }
    if (c == '?' && !in_query && !in_fragment) {
      in_query = true;
      continue;
    }
    if ((in_query || in_fragment) && (c == '[' || c == ']')) return true;
  }

  return false;
}

static char *url_escape_brackets_in_query_or_fragment(const char *url_str) {
  size_t len = strlen(url_str);
  size_t extra = 0;
  bool in_query = false;
  bool in_fragment = false;

  for (size_t i = 0; i < len; i++) {
    char c = url_str[i];
    if (c == '#' && !in_fragment) {
      in_query = false;
      in_fragment = true;
      continue;
    }
    if (c == '?' && !in_query && !in_fragment) {
      in_query = true;
      continue;
    }
    if ((in_query || in_fragment) && (c == '[' || c == ']')) extra += 2;
  }

  char *escaped = malloc(len + extra + 1);
  size_t pos = 0;
  in_query = false;
  in_fragment = false;

  if (!escaped) return NULL;

  for (size_t i = 0; i < len; i++) {
    char c = url_str[i];
    if (c == '#' && !in_fragment) {
      in_query = false;
      in_fragment = true;
      escaped[pos++] = c;
      continue;
    }
    if (c == '?' && !in_query && !in_fragment) {
      in_query = true;
      escaped[pos++] = c;
      continue;
    }
    if ((in_query || in_fragment) && c == '[') {
      memcpy(escaped + pos, "%5B", 3);
      pos += 3;
      continue;
    }
    if ((in_query || in_fragment) && c == ']') {
      memcpy(escaped + pos, "%5D", 3);
      pos += 3;
      continue;
    }
    escaped[pos++] = c;
  }

  escaped[pos] = '\0';
  return escaped;
}

static int url_parse_single_uri_relaxed(
  UriUriA *uri,
  const char *url_str,
  const char **errpos,
  char **owned_input_out,
  bool *used_relaxed_out
) {
  char *escaped = NULL;

  if (owned_input_out) *owned_input_out = NULL;
  if (used_relaxed_out) *used_relaxed_out = false;
  if (uriParseSingleUriA(uri, url_str, errpos) == URI_SUCCESS) return 0;

  if (!url_has_brackets_in_query_or_fragment(url_str)) return -1;
  escaped = url_escape_brackets_in_query_or_fragment(url_str);
  
  if (!escaped) return -1;
  if (owned_input_out) *owned_input_out = escaped;
  if (used_relaxed_out) *used_relaxed_out = true;

  return uriParseSingleUriA(uri, escaped, errpos) == URI_SUCCESS ? 0 : -1;
}

static void url_override_search_hash_from_input(url_state_t *s, const char *url_str) {
  const char *hash = strchr(url_str, '#');
  const char *query = strchr(url_str, '?');
  size_t search_len = 0;
  size_t hash_len = 0;

  if (query && hash && hash < query) query = NULL;

  if (query) {
    const char *search_end = hash && hash > query ? hash : url_str + strlen(url_str);
    search_len = (size_t)(search_end - query);
  }
  if (hash) hash_len = strlen(hash);

  free(s->search);
  s->search = search_len > 0 ? strndup(query, search_len) : strdup("");

  free(s->hash);
  s->hash = hash_len > 0 ? strndup(hash, hash_len) : strdup("");
}

static void url_override_search_hash_from_reference(url_state_t *s, const char *url_str) {
  const char *hash = strchr(url_str, '#');
  const char *query = strchr(url_str, '?');
  size_t search_len = 0;
  size_t hash_len = 0;

  if (query && hash && hash < query) query = NULL;

  if (query) {
    const char *search_end = hash && hash > query ? hash : url_str + strlen(url_str);
    search_len = (size_t)(search_end - query);
    free(s->search);
    s->search = strndup(query, search_len);
  }

  if (hash) {
    hash_len = strlen(hash);
    free(s->hash);
    s->hash = strndup(hash, hash_len);
  }
}

static void uri_to_state(const UriUriA *uri, url_state_t *s) {
  char *scheme = uri_range_dup(&uri->scheme);
  size_t slen = strlen(scheme);
  for (size_t i = 0; i < slen; i++) scheme[i] = (char)tolower((unsigned char)scheme[i]);
  
  s->protocol = malloc(slen + 2);
  memcpy(s->protocol, scheme, slen);
  s->protocol[slen] = ':';
  s->protocol[slen + 1] = '\0';
  free(scheme);

  char *userinfo = uri_range_dup(&uri->userInfo);
  char *colon = strchr(userinfo, ':');
  
  if (colon) {
    *colon = '\0';
    s->username = strdup(userinfo);
    s->password = strdup(colon + 1);
  } else {
    s->username = strdup(userinfo);
    s->password = strdup("");
  }
  
  free(userinfo);
  s->hostname = uri_range_dup(&uri->hostText);

  char *port = uri_range_dup(&uri->portText);
  int def = default_port_for(s->protocol);
  if (def > 0 && *port && atoi(port) == def) {
    free(port);
    port = strdup("");
  } s->port = port;

  size_t path_cap = 2;
  for (UriPathSegmentA *seg = uri->pathHead; seg; seg = seg->next)
    path_cap += (size_t)(seg->text.afterLast - seg->text.first) + 1;
    
  char *path = malloc(path_cap + 1);
  size_t pos = 0;
  
  for (UriPathSegmentA *seg = uri->pathHead; seg; seg = seg->next) {
    path[pos++] = '/';
    size_t seglen = (size_t)(seg->text.afterLast - seg->text.first);
    memcpy(path + pos, seg->text.first, seglen);
    pos += seglen;
  }
  
  if (pos == 0) path[pos++] = '/';
  path[pos] = '\0';
  s->pathname = path;

  char *query = uri_range_dup(&uri->query);
  if (*query) {
    size_t qlen = strlen(query);
    s->search = malloc(qlen + 2);
    s->search[0] = '?';
    memcpy(s->search + 1, query, qlen + 1);
  } else s->search = strdup("");
  free(query);

  char *frag = uri_range_dup(&uri->fragment);
  if (*frag) {
    size_t flen = strlen(frag);
    s->hash = malloc(flen + 2);
    s->hash[0] = '#';
    memcpy(s->hash + 1, frag, flen + 1);
  } else s->hash = strdup("");
  free(frag);
}

int parse_url_to_state(const char *url_str, const char *base_str, url_state_t *s) {
  memset(s, 0, sizeof(*s));
  const char *errpos;

  if (base_str) {
    UriUriA base_uri, ref_uri, resolved;
    char *escaped_base = NULL;
    char *escaped_ref = NULL;
    bool used_relaxed_ref_parse = false;
    
    if (url_parse_single_uri_relaxed(&base_uri, base_str, &errpos, &escaped_base, NULL) != 0) {
      free(escaped_base);
      return -1;
    }
    
    char *base_scheme = uri_range_dup(&base_uri.scheme);
    size_t bslen = strlen(base_scheme);
    for (size_t i = 0; i < bslen; i++) base_scheme[i] = (char)tolower((unsigned char)base_scheme[i]);
    
    char proto_buf[bslen + 2];
    memcpy(proto_buf, base_scheme, bslen);
    proto_buf[bslen] = ':';
    proto_buf[bslen + 1] = '\0';
    free(base_scheme);
    
    if (url_base_is_opaque(base_str, proto_buf)) {
      uriFreeUriMembersA(&base_uri);
      free(escaped_base);
      return -1;
    }
    
    if (url_parse_single_uri_relaxed(&ref_uri, url_str, &errpos, &escaped_ref, &used_relaxed_ref_parse) != 0) {
      uriFreeUriMembersA(&base_uri);
      free(escaped_base);
      free(escaped_ref);
      return -1;
    }
    
    if (uriAddBaseUriA(&resolved, &ref_uri, &base_uri) != URI_SUCCESS) {
      uriFreeUriMembersA(&base_uri);
      uriFreeUriMembersA(&ref_uri);
      free(escaped_base);
      free(escaped_ref);
      return -1;
    }
    
    uriNormalizeSyntaxA(&resolved);
    if (!resolved.scheme.first || resolved.scheme.first == resolved.scheme.afterLast) {
      uriFreeUriMembersA(&resolved);
      uriFreeUriMembersA(&ref_uri);
      uriFreeUriMembersA(&base_uri);
      free(escaped_base);
      free(escaped_ref);
      return -1;
    }
    
    uri_to_state(&resolved, s);
    if (used_relaxed_ref_parse) url_override_search_hash_from_reference(s, url_str);
    uriFreeUriMembersA(&resolved);
    uriFreeUriMembersA(&ref_uri);
    uriFreeUriMembersA(&base_uri);
    free(escaped_ref);
    free(escaped_base);
    
    return 0;
  }

  UriUriA uri;
  char *escaped_url = NULL;
  bool used_relaxed_query_parse = false;

  if (url_parse_single_uri_relaxed(&uri, url_str, &errpos, &escaped_url, &used_relaxed_query_parse) != 0) {
    free(escaped_url);
    return -1;
  }
  
  if (!uri.scheme.first || uri.scheme.first == uri.scheme.afterLast) {
    uriFreeUriMembersA(&uri);
    free(escaped_url);
    return -1;
  }
  
  uriNormalizeSyntaxA(&uri);
  uri_to_state(&uri, s);
  if (used_relaxed_query_parse) url_override_search_hash_from_input(s, url_str);
  
  uriFreeUriMembersA(&uri);
  free(escaped_url);
  
  return 0;
}

char *build_href(const url_state_t *s) {
  bool has_authority =
    (s->hostname && *s->hostname) ||
    (s->username && *s->username) ||
    (s->password && *s->password) ||
    (s->port && *s->port);
    
  bool opaque_like = !has_authority && !uses_authority_syntax(s->protocol);
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
    if (pathname[0] == '/') pathname++;
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

static const char *coerce_to_string(ant_t *js, ant_value_t val, size_t *len) {
  if (vtype(val) == T_STR) return js_getstr(js, val, len);
  if (is_object_type(val)) {
    ant_value_t href = js_getprop_fallback(js, val, "href");
    if (vtype(href) == T_STR) return js_getstr(js, href, len);
  }
  return NULL;
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
  free(s->search);
  
  if (*qs) {
    size_t qlen = strlen(qs);
    s->search = malloc(qlen + 2);
    s->search[0] = '?';
    memcpy(s->search + 1, qs, qlen + 1);
  } else s->search = strdup("");
  
  free(qs);
}

static void url_sync_usp(ant_t *js, ant_value_t url_obj, const char *query) {
  ant_value_t usp = js_get_slot(url_obj, SLOT_ENTRIES);
  if (!is_special_object(usp)) return;
  ant_value_t new_entries = parse_query_to_arr(js, query);
  js_set_slot_wb(js, usp, SLOT_ENTRIES, new_entries);
}

static ant_value_t make_usp_for_url(ant_t *js, ant_value_t url_obj, const char *query) {
  ant_value_t usp = js_mkobj(js);
  js_set_proto_init(usp, g_usp_proto);
  js_set_slot(usp, SLOT_BRAND, js_mknum(BRAND_URLSEARCHPARAMS));
  js_set_slot_wb(js, usp, SLOT_DATA, url_obj);
  js_set_slot(usp, SLOT_ENTRIES, parse_query_to_arr(js, query));
  return usp;
}

static ant_value_t url_get_href(ant_t *js, ant_value_t *args, int nargs) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkstr(js, "", 0);
  char *href = build_href(s);
  ant_value_t ret = js_mkstr(js, href, strlen(href));
  free(href);
  return ret;
}

static ant_value_t url_get_protocol(ant_t *js, ant_value_t *args, int nargs) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkstr(js, "", 0);
  return js_mkstr(js, s->protocol, strlen(s->protocol));
}

static ant_value_t url_get_username(ant_t *js, ant_value_t *args, int nargs) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkstr(js, "", 0);
  return js_mkstr(js, s->username, strlen(s->username));
}

static ant_value_t url_get_password(ant_t *js, ant_value_t *args, int nargs) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkstr(js, "", 0);
  return js_mkstr(js, s->password, strlen(s->password));
}

static ant_value_t url_get_host(ant_t *js, ant_value_t *args, int nargs) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkstr(js, "", 0);
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

static ant_value_t url_get_hostname(ant_t *js, ant_value_t *args, int nargs) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkstr(js, "", 0);
  return js_mkstr(js, s->hostname, strlen(s->hostname));
}

static ant_value_t url_get_port(ant_t *js, ant_value_t *args, int nargs) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkstr(js, "", 0);
  return js_mkstr(js, s->port, strlen(s->port));
}

static ant_value_t url_get_pathname(ant_t *js, ant_value_t *args, int nargs) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkstr(js, "/", 1);
  return js_mkstr(js, s->pathname, strlen(s->pathname));
}

static ant_value_t url_get_search(ant_t *js, ant_value_t *args, int nargs) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkstr(js, "", 0);
  return js_mkstr(js, s->search, strlen(s->search));
}

static ant_value_t url_get_hash(ant_t *js, ant_value_t *args, int nargs) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkstr(js, "", 0);
  return js_mkstr(js, s->hash, strlen(s->hash));
}

static ant_value_t url_get_origin(ant_t *js, ant_value_t *args, int nargs) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s || !is_special_scheme(s->protocol)) return js_mkstr(js, "null", 4);
  
  size_t proto_len = strlen(s->protocol) - 1;
  size_t host_len = strlen(s->hostname);
  size_t port_len = (s->port && *s->port) ? strlen(s->port) + 1 : 0;
  size_t total = proto_len + 3 + host_len + port_len + 1;
  char *origin = malloc(total);
  
  size_t pos = 0;
  memcpy(origin + pos, s->protocol, proto_len); pos += proto_len;
  memcpy(origin + pos, "://", 3);               pos += 3;
  memcpy(origin + pos, s->hostname, host_len);  pos += host_len;
  
  if (s->port && *s->port) {
    origin[pos++] = ':';
    memcpy(origin + pos, s->port, strlen(s->port));
    pos += strlen(s->port);
  }
  
  origin[pos] = '\0';
  ant_value_t ret = js_mkstr(js, origin, pos);
  free(origin);
  
  return ret;
}

static ant_value_t url_get_searchParams(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t usp = js_get_slot(js->this_val, SLOT_ENTRIES);
  if (vtype(usp) == T_OBJ) return usp;
  return js_mkundef();
}

static ant_value_t url_set_href(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkundef();
  
  const char *val = js_getstr(js, args[0], NULL);
  if (!val) return js_mkerr(js, "TypeError: Invalid URL");
  
  url_state_t tmp;
  if (parse_url_to_state(val, NULL, &tmp) != 0)
    return js_mkerr(js, "TypeError: Invalid URL");
    
  free(s->protocol); s->protocol = tmp.protocol;
  free(s->username); s->username = tmp.username;
  free(s->password); s->password = tmp.password;
  free(s->hostname); s->hostname = tmp.hostname;
  free(s->port);     s->port     = tmp.port;
  free(s->pathname); s->pathname = tmp.pathname;
  free(s->search);   s->search   = tmp.search;
  free(s->hash);     s->hash     = tmp.hash;
  
  const char *q = (s->search[0] == '?') ? s->search + 1 : "";
  url_sync_usp(js, js->this_val, q);
  
  return js_mkundef();
}

static ant_value_t url_set_protocol(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  url_state_t *s = url_get_state(js->this_val);
  
  if (!s) return js_mkundef();
  const char *val = js_getstr(js, args[0], NULL);
  
  if (!val || !*val) return js_mkundef();
  const char *colon = strchr(val, ':');
  
  size_t slen = colon ? (size_t)(colon - val) : strlen(val);
  if (!slen || !isalpha((unsigned char)val[0])) return js_mkundef();
  
  for (size_t i = 1; i < slen; i++) {
    unsigned char c = (unsigned char)val[i];
    if (!isalnum(c) && c != '+' && c != '-' && c != '.') return js_mkundef();
  }
  
  free(s->protocol);
  s->protocol = malloc(slen + 2);
  for (size_t i = 0; i < slen; i++) s->protocol[i] = (char)tolower((unsigned char)val[i]);
  s->protocol[slen] = ':';
  s->protocol[slen + 1] = '\0';
  
  if (s->port && *s->port) {
    int def = default_port_for(s->protocol);
    if (def > 0 && atoi(s->port) == def) { free(s->port); s->port = strdup(""); }
  }
  
  return js_mkundef();
}

static ant_value_t url_set_username(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkundef();
  const char *val = js_getstr(js, args[0], NULL);
  if (!val) return js_mkundef();
  free(s->username);
  s->username = userinfo_encode(val);
  return js_mkundef();
}

static ant_value_t url_set_password(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkundef();
  const char *val = js_getstr(js, args[0], NULL);
  if (!val) return js_mkundef();
  free(s->password);
  s->password = userinfo_encode(val);
  return js_mkundef();
}

static ant_value_t url_set_host(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkundef();
  const char *val = js_getstr(js, args[0], NULL);
  if (!val) return js_mkundef();
  const char *colon = strchr(val, ':');
  
  if (colon) {
    free(s->hostname);
    s->hostname = strndup(val, (size_t)(colon - val));
    const char *port_str = colon + 1;
    free(s->port);
    if (*port_str) {
      int p = atoi(port_str);
      int def = default_port_for(s->protocol);
      s->port = (def > 0 && p == def) ? strdup("") : strdup(port_str);
    } else s->port = strdup("");
  } else {
    free(s->hostname);
    s->hostname = strdup(val);
  }
  
  return js_mkundef();
}

static ant_value_t url_set_hostname(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkundef();
  const char *val = js_getstr(js, args[0], NULL);
  if (!val) return js_mkundef();
  free(s->hostname);
  const char *colon = strchr(val, ':');
  s->hostname = colon ? strndup(val, (size_t)(colon - val)) : strdup(val);
  return js_mkundef();
}

static ant_value_t url_set_port(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkundef();
  const char *val = js_getstr(js, args[0], NULL);
  if (!val) return js_mkundef();
  free(s->port);
  if (!*val) { s->port = strdup(""); return js_mkundef(); }
  int p = atoi(val);
  if (p < 0 || p > 65535) { s->port = strdup(""); return js_mkundef(); }
  int def = default_port_for(s->protocol);
  if (def > 0 && p == def) s->port = strdup(""); else {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", p);
    s->port = strdup(buf);
  }
  return js_mkundef();
}

static ant_value_t url_set_pathname(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkundef();
  const char *val = js_getstr(js, args[0], NULL);
  if (!val) return js_mkundef();
  free(s->pathname);
  if (is_special_scheme(s->protocol) && val[0] != '/') {
    size_t vlen = strlen(val);
    s->pathname = malloc(vlen + 2);
    s->pathname[0] = '/';
    memcpy(s->pathname + 1, val, vlen + 1);
  } else s->pathname = strdup(val);
  return js_mkundef();
}

static ant_value_t url_set_search(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkundef();
  const char *val = js_getstr(js, args[0], NULL);
  if (!val) return js_mkundef();
  const char *q = (val[0] == '?') ? val + 1 : val;
  free(s->search);
  if (*q) {
    size_t qlen = strlen(q);
    s->search = malloc(qlen + 2);
    s->search[0] = '?';
    memcpy(s->search + 1, q, qlen + 1);
  } else s->search = strdup("");
  url_sync_usp(js, js->this_val, *q ? q : "");
  return js_mkundef();
}

static ant_value_t url_set_hash(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkundef();
  const char *val = js_getstr(js, args[0], NULL);
  if (!val) return js_mkundef();
  const char *h = (val[0] == '#') ? val + 1 : val;
  free(s->hash);
  if (*h) {
    size_t hlen = strlen(h);
    s->hash = malloc(hlen + 2);
    s->hash[0] = '#';
    memcpy(s->hash + 1, h, hlen + 1);
  } else s->hash = strdup("");
  return js_mkundef();
}

static ant_value_t url_toString(ant_t *js, ant_value_t *args, int nargs) {
  url_state_t *s = url_get_state(js->this_val);
  if (!s) return js_mkstr(js, "", 0);
  char *href = build_href(s);
  ant_value_t ret = js_mkstr(js, href, strlen(href));
  free(href);
  return ret;
}

static ant_value_t js_URL(ant_t *js, ant_value_t *args, int nargs) {
  if (is_undefined(js->new_target))
    return js_mkerr_typed(js, JS_ERR_TYPE,
      "Failed to construct 'URL': Please use the 'new' operator.");
  if (nargs < 1)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct 'URL': 1 argument required.");

  ant_value_t url_sv = (vtype(args[0]) == T_STR) ? args[0] : js_tostring_val(js, args[0]);
  if (is_err(url_sv))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct 'URL': Invalid URL.");
  const char *url_str = js_getstr(js, url_sv, NULL);
  if (!url_str)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct 'URL': Invalid URL.");

  const char *base_str = NULL;
  if (nargs > 1 && !is_undefined(args[1]) && !is_null(args[1]))
    base_str = coerce_to_string(js, args[1], NULL);

  url_state_t *s = calloc(1, sizeof(url_state_t));
  if (!s) return js_mkerr(js, "out of memory");

  if (parse_url_to_state(url_str, base_str, s) != 0) {
    free(s);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct 'URL': Invalid URL.");
  }

  ant_value_t obj = js_mkobj(js);
  js_set_proto_init(obj, g_url_proto);
  js_set_native(obj, s, URL_NATIVE_TAG);
  js_set_finalizer(obj, url_finalize);

  const char *query = (s->search && s->search[0] == '?') ? s->search + 1 : "";
  ant_value_t usp = make_usp_for_url(js, obj, query);
  js_set_slot_wb(js, obj, SLOT_ENTRIES, usp);

  return obj;
}

ant_value_t make_url_obj(ant_t *js, url_state_t *s) {
  ant_value_t obj = js_mkobj(js);
  js_set_proto_init(obj, g_url_proto);
  js_set_native(obj, s, URL_NATIVE_TAG);
  js_set_finalizer(obj, url_finalize);
  const char *query = (s->search && s->search[0] == '?') ? s->search + 1 : "";
  ant_value_t usp = make_usp_for_url(js, obj, query);
  js_set_slot_wb(js, obj, SLOT_ENTRIES, usp);
  return obj;
}

static ant_value_t url_canParse(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_false;
  ant_value_t url_sv = (vtype(args[0]) == T_STR) ? args[0] : js_tostring_val(js, args[0]);
  if (is_err(url_sv)) return js_false;
  const char *url_str = js_getstr(js, url_sv, NULL);
  if (!url_str) return js_false;
  const char *base_str = NULL;
  if (nargs > 1 && !is_undefined(args[1]) && !is_null(args[1]))
    base_str = coerce_to_string(js, args[1], NULL);
  url_state_t s;
  if (parse_url_to_state(url_str, base_str, &s) != 0) return js_false;
  url_state_clear(&s);
  return js_true;
}

static ant_value_t url_parse(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mknull();
  ant_value_t url_sv = (vtype(args[0]) == T_STR) ? args[0] : js_tostring_val(js, args[0]);
  if (is_err(url_sv)) return js_mknull();
  const char *url_str = js_getstr(js, url_sv, NULL);
  if (!url_str) return js_mknull();
  const char *base_str = NULL;
  if (nargs > 1 && !is_undefined(args[1]) && !is_null(args[1]))
    base_str = coerce_to_string(js, args[1], NULL);
  url_state_t *s = calloc(1, sizeof(url_state_t));
  if (!s) return js_mknull();
  if (parse_url_to_state(url_str, base_str, s) != 0) {
    free(s);
    return js_mknull();
  }
  return make_url_obj(js, s);
}

static ant_value_t usp_get(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mknull();
  ant_value_t key_sv = (vtype(args[0]) == T_STR) ? args[0] : js_tostring_val(js, args[0]);
  if (is_err(key_sv)) return js_mknull();
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

static ant_value_t usp_getAll(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t result = js_mkarr(js);
  if (nargs < 1) return result;
  ant_value_t key_sv = (vtype(args[0]) == T_STR) ? args[0] : js_tostring_val(js, args[0]);
  if (is_err(key_sv)) return result;
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

static ant_value_t usp_has(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_false;
  ant_value_t key_sv = (vtype(args[0]) == T_STR) ? args[0] : js_tostring_val(js, args[0]);
  if (is_err(key_sv)) return js_false;
  const char *key = js_getstr(js, key_sv, NULL);
  if (!key) return js_false;
  const char *match_val = NULL;
  if (nargs >= 2 && !is_undefined(args[1])) {
    ant_value_t mv_sv = (vtype(args[1]) == T_STR) ? args[1] : js_tostring_val(js, args[1]);
    if (!is_err(mv_sv)) match_val = js_getstr(js, mv_sv, NULL);
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

static ant_value_t usp_set(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  ant_value_t key_sv = (vtype(args[0]) == T_STR) ? args[0] : js_tostring_val(js, args[0]);
  
  if (is_err(key_sv)) return js_mkundef();
  ant_value_t val_sv = (vtype(args[1]) == T_STR) ? args[1] : js_tostring_val(js, args[1]);
  
  if (is_err(val_sv)) return js_mkundef();
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

static ant_value_t usp_append(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  ant_value_t key_sv = (vtype(args[0]) == T_STR) ? args[0] : js_tostring_val(js, args[0]);
  
  if (is_err(key_sv)) return js_mkundef();
  ant_value_t val_sv = (vtype(args[1]) == T_STR) ? args[1] : js_tostring_val(js, args[1]);
  
  if (is_err(val_sv)) return js_mkundef();
  ant_value_t entries = js_get_slot(js->this_val, SLOT_ENTRIES);
  
  if (!is_special_object(entries)) return js_mkundef();
  ant_value_t entry = js_mkarr(js);
  
  js_arr_push(js, entry, key_sv);
  js_arr_push(js, entry, val_sv);
  js_arr_push(js, entries, entry);
  usp_push_to_url(js, js->this_val);
  
  return js_mkundef();
}

static ant_value_t usp_delete(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  ant_value_t key_sv = (vtype(args[0]) == T_STR) ? args[0] : js_tostring_val(js, args[0]);
  
  if (is_err(key_sv)) return js_mkundef();
  const char *key = js_getstr(js, key_sv, NULL);
  
  if (!key) return js_mkundef();
  const char *match_val = NULL;
  
  if (nargs >= 2 && !is_undefined(args[1])) {
    ant_value_t mv_sv = (vtype(args[1]) == T_STR) ? args[1] : js_tostring_val(js, args[1]);
    if (!is_err(mv_sv)) match_val = js_getstr(js, mv_sv, NULL);
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

static ant_value_t usp_toString(ant_t *js, ant_value_t *args, int nargs) {
  char *s = usp_serialize(js, js->this_val);
  ant_value_t ret = js_mkstr(js, s, strlen(s));
  free(s);
  return ret;
}

static ant_value_t usp_forEach(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0])) return js_mkundef();
  
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
    ant_value_t r = sv_vm_call(js->vm, js, cb, this_arg, cb_args, 3, NULL, false);
    if (is_err(r)) return r;
  }
  
  return js_mkundef();
}

static ant_value_t usp_size_get(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t entries = js_get_slot(js->this_val, SLOT_ENTRIES);
  if (!is_special_object(entries)) return js_mknum(0);
  return js_mknum((double)js_arr_len(js, entries));
}

static ant_value_t usp_sort(ant_t *js, ant_value_t *args, int nargs) {
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

static ant_value_t usp_iter_next(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_v = js_get_slot(js->this_val, SLOT_ITER_STATE);
  if (vtype(state_v) != T_NUM) return js_iter_result(js, false, js_mkundef());

  uint32_t state = (uint32_t)js_getnum(state_v);
  uint32_t kind  = ITER_STATE_KIND(state);
  uint32_t idx   = ITER_STATE_INDEX(state);

  ant_value_t usp = js_get_slot(js->this_val, SLOT_DATA);
  ant_value_t entries = js_get_slot(usp, SLOT_ENTRIES);
  if (!is_special_object(entries) || (ant_offset_t)idx >= js_arr_len(js, entries))
    return js_iter_result(js, false, js_mkundef());

  js_set_slot(js->this_val, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, idx + 1)));

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
  ant_value_t iter = js_mkobj(js);
  js_set_proto_init(iter, g_usp_iter_proto);
  js_set_slot_wb(js, iter, SLOT_DATA, usp);
  js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, 0)));
  return iter;
}

static ant_value_t usp_entries_fn(ant_t *js, ant_value_t *args, int nargs) {
  return make_usp_iter(js, js->this_val, USP_ITER_ENTRIES);
}

static ant_value_t usp_keys_fn(ant_t *js, ant_value_t *args, int nargs) {
  return make_usp_iter(js, js->this_val, USP_ITER_KEYS);
}

static ant_value_t usp_values_fn(ant_t *js, ant_value_t *args, int nargs) {
  return make_usp_iter(js, js->this_val, USP_ITER_VALUES);
}

static ant_value_t js_URLSearchParams(ant_t *js, ant_value_t *args, int nargs) {
  if (is_undefined(js->new_target))
    return js_mkerr_typed(js, JS_ERR_TYPE,
    "Failed to construct 'URLSearchParams': Please use the 'new' operator.");

  ant_value_t obj = js_mkobj(js);
  js_set_proto_init(obj, g_usp_proto);
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_URLSEARCHPARAMS));
  js_set_slot(obj, SLOT_DATA, js_mkundef());
  
  ant_value_t entries = js_mkarr(js);
  js_set_slot(obj, SLOT_ENTRIES, entries);

  if (nargs < 1 || is_undefined(args[0]) || is_null(args[0])) return obj;

  ant_value_t init = args[0];
  uint8_t t = vtype(init);

  if (t == T_STR) {
    const char *s = js_getstr(js, init, NULL);
    if (s) {
      const char *q = (s[0] == '?') ? s + 1 : s;
      js_set_slot(obj, SLOT_ENTRIES, parse_query_to_arr(js, q));
    }
    return obj;
  }

  if (t == T_ARR) {
    ant_offset_t len = js_arr_len(js, init);
    for (ant_offset_t i = 0; i < len; i++) {
      ant_value_t pair = js_arr_get(js, init, i);
      if (vtype(pair) != T_ARR)
        return js_mkerr_typed(js, JS_ERR_TYPE,
        "Failed to construct 'URLSearchParams': Each element must be an array.");
        
      ant_offset_t plen = js_arr_len(js, pair);
      if (plen != 2)
        return js_mkerr_typed(js, JS_ERR_TYPE,
        "Failed to construct 'URLSearchParams': Each pair must have exactly 2 elements.");
        
      ant_value_t pk = js_arr_get(js, pair, 0);
      ant_value_t pv = js_arr_get(js, pair, 1);
      ant_value_t ksv = (vtype(pk) == T_STR) ? pk : js_tostring_val(js, pk);
      
      if (is_err(ksv)) return ksv;
      ant_value_t vsv = (vtype(pv) == T_STR) ? pv : js_tostring_val(js, pv);
      
      if (is_err(vsv)) return vsv;
      ant_value_t entry = js_mkarr(js);
      
      js_arr_push(js, entry, ksv);
      js_arr_push(js, entry, vsv);
      js_arr_push(js, entries, entry);
    }
    
    return obj;
  }

  if (is_special_object(init)) {
    ant_value_t src = js_get_slot(init, SLOT_ENTRIES);
    if (vtype(src) == T_ARR) {
      ant_offset_t len = js_arr_len(js, src);
      for (ant_offset_t i = 0; i < len; i++) {
        ant_value_t entry = js_arr_get(js, src, i);
        ant_value_t ne = js_mkarr(js);
        
        js_arr_push(js, ne, js_arr_get(js, entry, 0));
        js_arr_push(js, ne, js_arr_get(js, entry, 1));
        js_arr_push(js, entries, ne);
      }
      
      return obj;
    }
    
    ant_iter_t it = js_prop_iter_begin(js, init);
    const char *key;
    size_t key_len;
    ant_value_t val;
    
    while (js_prop_iter_next(&it, &key, &key_len, &val)) {
      ant_value_t sv = (vtype(val) == T_STR) ? val : js_tostring_val(js, val);
      if (is_err(sv)) { js_prop_iter_end(&it); return sv; }
      ant_value_t entry = js_mkarr(js);
      js_arr_push(js, entry, js_mkstr(js, key, key_len));
      js_arr_push(js, entry, sv);
      js_arr_push(js, entries, entry);
    }
    
    js_prop_iter_end(&it);
  }

  return obj;
}

void init_url_module(void) {
  ant_t *js = rt->js;
  ant_value_t glob = js->global;

  g_usp_iter_proto = js_mkobj(js);
  js_set_proto_init(g_usp_iter_proto, js->sym.iterator_proto);
  js_set(js, g_usp_iter_proto, "next", js_mkfun(usp_iter_next));
  js_set_descriptor(js, g_usp_iter_proto, "next", 4, JS_DESC_W | JS_DESC_E | JS_DESC_C);
  js_set_sym(js, g_usp_iter_proto, get_iterator_sym(), js_mkfun(sym_this_cb));

  g_usp_proto = js_mkobj(js);
  js_set(js, g_usp_proto, "get",      js_mkfun(usp_get));
  js_set(js, g_usp_proto, "getAll",   js_mkfun(usp_getAll));
  js_set(js, g_usp_proto, "has",      js_mkfun(usp_has));
  js_set(js, g_usp_proto, "set",      js_mkfun(usp_set));
  js_set(js, g_usp_proto, "append",   js_mkfun(usp_append));
  js_set(js, g_usp_proto, "delete",   js_mkfun(usp_delete));
  js_set(js, g_usp_proto, "sort",     js_mkfun(usp_sort));
  js_set(js, g_usp_proto, "toString", js_mkfun(usp_toString));
  js_set(js, g_usp_proto, "forEach",  js_mkfun(usp_forEach));
  js_set_getter_desc(js, g_usp_proto, "size", 4, js_mkfun(usp_size_get), JS_DESC_C);

  js_set(js, g_usp_proto, "entries", js_mkfun(usp_entries_fn));
  js_set(js, g_usp_proto, "keys",    js_mkfun(usp_keys_fn));
  js_set(js, g_usp_proto, "values",  js_mkfun(usp_values_fn));
  
  js_set_sym(js, g_usp_proto, get_iterator_sym(), js_get(js, g_usp_proto, "entries"));
  js_set_sym(js, g_usp_proto, get_toStringTag_sym(), js_mkstr(js, "URLSearchParams", 15));

  ant_value_t usp_ctor = js_make_ctor(js, js_URLSearchParams, g_usp_proto, "URLSearchParams", 15);
  js_set(js, glob, "URLSearchParams", usp_ctor);

  g_url_proto = js_mkobj(js);
  js_set_accessor_desc(js, g_url_proto, "href",         4,  js_mkfun(url_get_href),         js_mkfun(url_set_href),     JS_DESC_C);
  js_set_accessor_desc(js, g_url_proto, "protocol",     8,  js_mkfun(url_get_protocol),     js_mkfun(url_set_protocol), JS_DESC_C);
  js_set_accessor_desc(js, g_url_proto, "username",     8,  js_mkfun(url_get_username),     js_mkfun(url_set_username), JS_DESC_C);
  js_set_accessor_desc(js, g_url_proto, "password",     8,  js_mkfun(url_get_password),     js_mkfun(url_set_password), JS_DESC_C);
  js_set_accessor_desc(js, g_url_proto, "host",         4,  js_mkfun(url_get_host),         js_mkfun(url_set_host),     JS_DESC_C);
  js_set_accessor_desc(js, g_url_proto, "hostname",     8,  js_mkfun(url_get_hostname),     js_mkfun(url_set_hostname), JS_DESC_C);
  js_set_accessor_desc(js, g_url_proto, "port",         4,  js_mkfun(url_get_port),         js_mkfun(url_set_port),     JS_DESC_C);
  js_set_accessor_desc(js, g_url_proto, "pathname",     8,  js_mkfun(url_get_pathname),     js_mkfun(url_set_pathname), JS_DESC_C);
  js_set_accessor_desc(js, g_url_proto, "search",       6,  js_mkfun(url_get_search),       js_mkfun(url_set_search),   JS_DESC_C);
  js_set_accessor_desc(js, g_url_proto, "hash",         4,  js_mkfun(url_get_hash),         js_mkfun(url_set_hash),     JS_DESC_C);
  js_set_getter_desc(js,   g_url_proto, "origin",       6,  js_mkfun(url_get_origin),       JS_DESC_C);
  js_set_getter_desc(js,   g_url_proto, "searchParams", 12, js_mkfun(url_get_searchParams), JS_DESC_C);
  js_set(js, g_url_proto, "toString", js_mkfun(url_toString));
  js_set(js, g_url_proto, "toJSON",   js_mkfun(url_toString));
  js_set_sym(js, g_url_proto, get_toStringTag_sym(), js_mkstr(js, "URL", 3));

  ant_value_t url_ctor = js_make_ctor(js, js_URL, g_url_proto, "URL", 3);
  js_set(js, url_ctor, "canParse", js_mkfun(url_canParse));
  js_set(js, url_ctor, "parse",    js_mkfun(url_parse));
  js_set(js, glob, "URL", url_ctor);
}

static ant_value_t builtin_fileURLToPath(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "fileURLToPath requires a string or URL argument");
  
  size_t len;
  const char *str = coerce_to_string(js, args[0], &len);
  if (!str) return js_mkerr(js, "fileURLToPath requires a string or URL argument");

  url_state_t s;
  if (parse_url_to_state(str, NULL, &s) != 0)
    return js_mkerr(js, "Invalid URL");
  if (strcmp(s.protocol, "file:") != 0) {
    url_state_clear(&s);
    return js_mkerr(js, "fileURLToPath requires a file: URL");
  }
  
  char *decoded = url_decode_component(s.pathname);
  url_state_clear(&s);
  if (!decoded) return js_mkerr(js, "allocation failure");
  ant_value_t ret = js_mkstr(js, decoded, strlen(decoded));
  
  free(decoded);
  return ret;
}

static ant_value_t builtin_pathToFileURL(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR)
    return js_mkerr(js, "pathToFileURL requires a string argument");
    
  size_t len;
  const char *path = js_getstr(js, args[0], &len);
  size_t total = 7 + len;
  char *buf = malloc(total + 1);
  if (!buf) return js_mkerr(js, "allocation failure");
  
  memcpy(buf, "file://", 7);
  memcpy(buf + 7, path, len);
  buf[total] = '\0';
  
  url_state_t *s = calloc(1, sizeof(url_state_t));
  if (!s) { free(buf); return js_mkerr(js, "allocation failure"); }
  if (parse_url_to_state(buf, NULL, s) != 0) {
    free(buf); free(s);
    return js_mkerr(js, "Invalid file URL");
  }
  
  free(buf);
  return make_url_obj(js, s);
}

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} url_fmt_buf_t;

static bool url_fmt_reserve(url_fmt_buf_t *b, size_t extra) {
  if (extra <= b->cap - b->len) return true;

  size_t needed = b->len + extra + 1;
  size_t next = b->cap ? b->cap : 128;
  while (next < needed) next *= 2;

  char *buf = realloc(b->buf, next);
  if (!buf) return false;
  b->buf = buf;
  b->cap = next;
  
  return true;
}

static bool url_fmt_append_n(url_fmt_buf_t *b, const char *s, size_t n) {
  if (!s || n == 0) return true;
  if (!url_fmt_reserve(b, n)) return false;
  memcpy(b->buf + b->len, s, n);
  b->len += n;
  b->buf[b->len] = '\0';
  return true;
}

static bool url_fmt_append(url_fmt_buf_t *b, const char *s) {
  return url_fmt_append_n(b, s, s ? strlen(s) : 0);
}

static bool url_fmt_append_c(url_fmt_buf_t *b, char c) {
  if (!url_fmt_reserve(b, 1)) return false;
  b->buf[b->len++] = c;
  b->buf[b->len] = '\0';
  return true;
}

static bool url_fmt_append_value_string(ant_t *js, url_fmt_buf_t *b, ant_value_t value) {
  ant_value_t str_val = vtype(value) == T_STR ? value : js_tostring_val(js, value);
  if (is_err(str_val)) return false;

  size_t len = 0;
  const char *str = js_getstr(js, str_val, &len);
  return str && url_fmt_append_n(b, str, len);
}

static bool url_fmt_get_string_prop(
  ant_t *js,
  ant_value_t obj,
  const char *name,
  ant_value_t *out,
  const char **str,
  size_t *len
) {
  *out = js_get(js, obj, name);
  if (is_undefined(*out) || is_null(*out)) return false;

  ant_value_t str_val = vtype(*out) == T_STR ? *out : js_tostring_val(js, *out);
  if (is_err(str_val)) return false;

  *out = str_val;
  *str = js_getstr(js, str_val, len);
  return *str != NULL;
}

static bool url_fmt_is_query_unescaped(unsigned char c) {
  return isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

static bool url_fmt_append_query_component(url_fmt_buf_t *b, const char *s, size_t len) {
  static const char hex[] = "0123456789ABCDEF";

  for (size_t i = 0; i < len; i++) {
  unsigned char c = (unsigned char)s[i];
  if (url_fmt_is_query_unescaped(c)) {
    if (!url_fmt_append_c(b, (char)c)) return false;
  } else {
    char esc[3] = { '%', hex[c >> 4], hex[c & 0x0f] };
    if (!url_fmt_append_n(b, esc, sizeof(esc))) return false;
  }}
  return true;
}

static bool url_fmt_append_query_object(ant_t *js, url_fmt_buf_t *b, ant_value_t query) {
  if (!is_special_object(query)) return true;

  bool first = true;
  ant_iter_t it = js_prop_iter_begin(js, query);
  
  const char *key;
  size_t key_len;
  ant_value_t val;

  while (js_prop_iter_next(&it, &key, &key_len, &val)) {
    if (!first && !url_fmt_append_c(b, '&')) {
      js_prop_iter_end(&it);
      return false;
    }
    first = false;
    
    if (!url_fmt_append_query_component(b, key, key_len)) {
      js_prop_iter_end(&it);
      return false;
    }
    
    if (!url_fmt_append_c(b, '=')) {
      js_prop_iter_end(&it);
      return false;
    }
    
    ant_value_t str_val = vtype(val) == T_STR ? val : js_tostring_val(js, val);
    if (is_err(str_val)) {
      js_prop_iter_end(&it);
      return false;
    }
    
    size_t val_len = 0;
    const char *val_str = js_getstr(js, str_val, &val_len);
    if (!val_str || !url_fmt_append_query_component(b, val_str, val_len)) {
      js_prop_iter_end(&it);
      return false;
    }
  }

  js_prop_iter_end(&it);
  return true;
}

static bool url_fmt_protocol_needs_slashes(const char *protocol, size_t len) {
  if (len > 0 && protocol[len - 1] == ':') len--;
  return
    (len == 4 && memcmp(protocol, "http", 4) == 0) ||
    (len == 5 && memcmp(protocol, "https", 5) == 0) ||
    (len == 3 && memcmp(protocol, "ftp", 3) == 0) ||
    (len == 4 && memcmp(protocol, "file", 4) == 0) ||
    (len == 2 && memcmp(protocol, "ws", 2) == 0) ||
    (len == 3 && memcmp(protocol, "wss", 3) == 0);
}

static ant_value_t builtin_url_format(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_object_type(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "url.format() requires a URL or object argument");

  url_state_t *state = url_get_state(args[0]);
  if (state) {
    char *href = build_href(state);
    ant_value_t ret = js_mkstr(js, href, strlen(href));
    free(href);
    return ret;
  }

  ant_value_t obj = args[0];
  ant_value_t tmp;
  
  const char *protocol = NULL, *auth = NULL, *host = NULL, *hostname = NULL;
  const char *port = NULL, *pathname = NULL, *search = NULL, *hash = NULL;
  
  size_t protocol_len = 0, auth_len = 0, host_len = 0, hostname_len = 0;
  size_t port_len = 0, pathname_len = 0, search_len = 0, hash_len = 0;

  url_fmt_get_string_prop(js, obj, "protocol", &tmp, &protocol, &protocol_len);
  url_fmt_get_string_prop(js, obj, "auth",     &tmp, &auth,     &auth_len);
  url_fmt_get_string_prop(js, obj, "host",     &tmp, &host,     &host_len);
  url_fmt_get_string_prop(js, obj, "hostname", &tmp, &hostname, &hostname_len);
  url_fmt_get_string_prop(js, obj, "port",     &tmp, &port,     &port_len);
  url_fmt_get_string_prop(js, obj, "pathname", &tmp, &pathname, &pathname_len);
  url_fmt_get_string_prop(js, obj, "search",   &tmp, &search,   &search_len);
  url_fmt_get_string_prop(js, obj, "hash",     &tmp, &hash,     &hash_len);

  url_fmt_buf_t b = {0};

  if (protocol && protocol_len > 0) {
    if (!url_fmt_append_n(&b, protocol, protocol_len)) goto oom;
    if (protocol[protocol_len - 1] != ':' && !url_fmt_append_c(&b, ':')) goto oom;
  }

  bool has_host = (host && host_len > 0) || (hostname && hostname_len > 0);
  ant_value_t slashes_val = js_get(js, obj, "slashes");
  
  bool needs_slashes =
    js_truthy(js, slashes_val) ||
    (protocol && url_fmt_protocol_needs_slashes(protocol, protocol_len));
    
  if (needs_slashes && (has_host || (protocol && protocol_len >= 4 && memcmp(protocol, "file", 4) == 0))) {
    if (!url_fmt_append(&b, "//")) goto oom;
  }

  if (auth && auth_len > 0) {
    if (!url_fmt_append_n(&b, auth, auth_len)) goto oom;
    if (!url_fmt_append_c(&b, '@')) goto oom;
  }

  if (host && host_len > 0) {
    if (!url_fmt_append_n(&b, host, host_len)) goto oom;
  } else if (hostname && hostname_len > 0) {
  if (!url_fmt_append_n(&b, hostname, hostname_len)) goto oom;
  if (port && port_len > 0) {
    if (!url_fmt_append_c(&b, ':')) goto oom;
    if (!url_fmt_append_n(&b, port, port_len)) goto oom;
  }}

  if (pathname && pathname_len > 0) {
    if (has_host && pathname[0] != '/' && !url_fmt_append_c(&b, '/')) goto oom;
    if (!url_fmt_append_n(&b, pathname, pathname_len)) goto oom;
  }

  if (search && search_len > 0) {
    if (search[0] != '?' && !url_fmt_append_c(&b, '?')) goto oom;
    if (!url_fmt_append_n(&b, search, search_len)) goto oom;
  } else {
    ant_value_t query = js_get(js, obj, "query");
    if (vtype(query) == T_STR) {
      size_t qlen = 0;
      const char *q = js_getstr(js, query, &qlen);
      if (q && qlen > 0) {
        if (!url_fmt_append_c(&b, '?')) goto oom;
        if (!url_fmt_append_n(&b, q, qlen)) goto oom;
      }
    } else if (is_special_object(query)) {
      url_fmt_buf_t qb = {0};
      if (!url_fmt_append_query_object(js, &qb, query)) {
        free(qb.buf);
        goto oom;
      }
      if (qb.len > 0) {
      if (!url_fmt_append_c(&b, '?')) {
        free(qb.buf);
        goto oom;
      }
      if (!url_fmt_append_n(&b, qb.buf, qb.len)) {
        free(qb.buf);
        goto oom;
      }}
      free(qb.buf);
    }
  }

  if (hash && hash_len > 0) {
    if (hash[0] != '#' && !url_fmt_append_c(&b, '#')) goto oom;
    if (!url_fmt_append_n(&b, hash, hash_len)) goto oom;
  }

  ant_value_t ret = js_mkstr(js, b.buf ? b.buf : "", b.len);
  free(b.buf);
  return ret;

oom:
  free(b.buf);
  return js_mkerr(js, "allocation failure");
}

ant_value_t url_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);
  ant_value_t glob = js_glob(js);
  
  js_set(js, lib, "URL",            js_get(js, glob, "URL"));
  js_set(js, lib, "URLSearchParams",js_get(js, glob, "URLSearchParams"));
  js_set(js, lib, "fileURLToPath",  js_mkfun(builtin_fileURLToPath));
  js_set(js, lib, "pathToFileURL",  js_mkfun(builtin_pathToFileURL));
  js_set(js, lib, "parse",          js_mkfun(url_parse));
  js_set(js, lib, "format",         js_mkfun(builtin_url_format));
  js_set(js, lib, "default", lib);
  
  return lib;
}
