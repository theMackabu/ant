#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "ant.h"
#include "errors.h"
#include "internal.h"
#include "runtime.h"
#include "utils.h"

#include "modules/url.h"
#include "modules/symbol.h"

typedef struct {
  char *protocol;
  char *username;
  char *password;
  char *hostname;
  char *port;
  char *pathname;
  char *search;
  char *hash;
} parsed_url_t;

static void free_parsed_url(parsed_url_t *p) {
  if (p->protocol) free(p->protocol);
  if (p->username) free(p->username);
  if (p->password) free(p->password);
  if (p->hostname) free(p->hostname);
  if (p->port) free(p->port);
  if (p->pathname) free(p->pathname);
  if (p->search) free(p->search);
  if (p->hash) free(p->hash);
}

static char *url_encode_component(const char *str) {
  if (!str) return strdup("");
  size_t len = strlen(str);
  char *out = malloc(len * 3 + 1);
  if (!out) return NULL;
  size_t j = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)str[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out[j++] = c;
    } else {
      snprintf(out + j, 4, "%%%02X", c);
      j += 3;
    }
  }
  out[j] = '\0';
  return out;
}

static char *url_decode_component(const char *str) {
  if (!str) return strdup("");
  size_t len = strlen(str);
  char *out = malloc(len + 1);
  if (!out) return NULL;
  size_t j = 0;
  for (size_t i = 0; i < len; i++) {
    if (str[i] == '%' && i + 2 < len && isxdigit(str[i+1]) && isxdigit(str[i+2])) {
      int hi = isdigit(str[i+1]) ? str[i+1] - '0' : tolower(str[i+1]) - 'a' + 10;
      int lo = isdigit(str[i+2]) ? str[i+2] - '0' : tolower(str[i+2]) - 'a' + 10;
      out[j++] = (char)((hi << 4) | lo);
      i += 2;
    } else if (str[i] == '+') {
      out[j++] = ' ';
    } else {
      out[j++] = str[i];
    }
  }
  out[j] = '\0';
  return out;
}

static int parse_url(const char *url_str, const char *base_str, parsed_url_t *out) {
  memset(out, 0, sizeof(*out));

  const char *p = url_str;
  const char *end = url_str + strlen(url_str);

  if (base_str && *p == '/') {
    parsed_url_t base = {0};
    if (parse_url(base_str, NULL, &base) < 0) return -1;
    out->protocol = base.protocol ? strdup(base.protocol) : NULL;
    out->username = base.username ? strdup(base.username) : strdup("");
    out->password = base.password ? strdup(base.password) : strdup("");
    out->hostname = base.hostname ? strdup(base.hostname) : NULL;
    out->port = base.port ? strdup(base.port) : strdup("");

    char *path_copy = strdup(p);
    char *q = strchr(path_copy, '?');
    char *h = strchr(path_copy, '#');

    if (h) {
      out->hash = strdup(h);
      *h = '\0';
    } else {
      out->hash = strdup("");
    }

    if (q) {
      out->search = strdup(q);
      *q = '\0';
    } else {
      out->search = strdup("");
    }

    out->pathname = strdup(path_copy);
    free(path_copy);

    free(base.protocol); free(base.username); free(base.password);
    free(base.hostname); free(base.port); free(base.pathname);
    free(base.search); free(base.hash);
    return 0;
  }

  const char *scheme_end = strstr(p, "://");
  if (!scheme_end) return -1;

  size_t scheme_len = scheme_end - p;
  out->protocol = malloc(scheme_len + 2);
  memcpy(out->protocol, p, scheme_len);
  out->protocol[scheme_len] = ':';
  out->protocol[scheme_len + 1] = '\0';
  p = scheme_end + 3;

  out->username = strdup("");
  out->password = strdup("");

  const char *at = strchr(p, '@');
  const char *slash = strchr(p, '/');
  if (at && (!slash || at < slash)) {
    const char *colon = strchr(p, ':');
    if (colon && colon < at) {
      free(out->username);
      out->username = strndup(p, colon - p);
      free(out->password);
      out->password = strndup(colon + 1, at - colon - 1);
    } else {
      free(out->username);
      out->username = strndup(p, at - p);
    }
    p = at + 1;
  }

  slash = strchr(p, '/');
  const char *qmark = strchr(p, '?');
  const char *hash = strchr(p, '#');

  const char *host_end = slash ? slash : (qmark ? qmark : (hash ? hash : end));
  char *host_part = strndup(p, host_end - p);

  char *port_sep = strrchr(host_part, ':');
  if (port_sep && strchr(port_sep, ']') == NULL) {
    *port_sep = '\0';
    out->port = strdup(port_sep + 1);
    out->hostname = strdup(host_part);
  } else {
    out->hostname = strdup(host_part);
    out->port = strdup("");
  }
  free(host_part);
  p = host_end;

  if (slash) {
    const char *path_end = qmark ? qmark : (hash ? hash : end);
    out->pathname = strndup(slash, path_end - slash);
    p = path_end;
  } else {
    out->pathname = strdup("/");
  }

  if (qmark && (!hash || qmark < hash)) {
    const char *search_end = hash ? hash : end;
    out->search = strndup(qmark, search_end - qmark);
    p = search_end;
  } else {
    out->search = strdup("");
  }

  if (hash) {
    out->hash = strdup(hash);
  } else {
    out->hash = strdup("");
  }

  return 0;
}

static char *build_href(
  const char *protocol, const char *username, const char *password,
  const char *hostname, const char *port, const char *pathname,
  const char *search, const char *hash
) {
  size_t len = strlen(protocol) + 2 + strlen(hostname) + strlen(pathname) + strlen(search) + strlen(hash) + 32;
  if (username && *username) len += strlen(username) + strlen(password) + 2;
  if (port && *port) len += strlen(port) + 1;

  char *href = malloc(len);
  if (!href) return NULL;

  size_t used = 0;
  size_t remaining = len;

  int written = snprintf(href + used, remaining, "%s//", protocol);
  if (written < 0) { href[0] = '\0'; return href; }
  if ((size_t)written >= remaining) { href[len - 1] = '\0'; return href; }
  used += (size_t)written;
  remaining -= (size_t)written;

  if (username && *username) {
    written = snprintf(href + used, remaining, "%s", username);
    if (written < 0) { href[0] = '\0'; return href; }
    if ((size_t)written >= remaining) { href[len - 1] = '\0'; return href; }
    used += (size_t)written;
    remaining -= (size_t)written;

    if (password && *password) {
      written = snprintf(href + used, remaining, ":%s", password);
      if (written < 0) { href[0] = '\0'; return href; }
      if ((size_t)written >= remaining) { href[len - 1] = '\0'; return href; }
      used += (size_t)written;
      remaining -= (size_t)written;
    }

    written = snprintf(href + used, remaining, "@");
    if (written < 0) { href[0] = '\0'; return href; }
    if ((size_t)written >= remaining) { href[len - 1] = '\0'; return href; }
    used += (size_t)written;
    remaining -= (size_t)written;
  }

  written = snprintf(href + used, remaining, "%s", hostname);
  if (written < 0) { href[0] = '\0'; return href; }
  if ((size_t)written >= remaining) { href[len - 1] = '\0'; return href; }
  used += (size_t)written;
  remaining -= (size_t)written;

  if (port && *port) {
    written = snprintf(href + used, remaining, ":%s", port);
    if (written < 0) { href[0] = '\0'; return href; }
    if ((size_t)written >= remaining) { href[len - 1] = '\0'; return href; }
    used += (size_t)written;
    remaining -= (size_t)written;
  }

  written = snprintf(href + used, remaining, "%s%s%s", pathname, search, hash);
  if (written < 0) { href[0] = '\0'; return href; }
  if ((size_t)written >= remaining) { href[len - 1] = '\0'; return href; }

  return href;
}

static void update_url_href(struct js *js, jsval_t url_obj) {
  char *protocol = js_getstr(js, js_get(js, url_obj, "protocol"), NULL);
  char *username = js_getstr(js, js_get(js, url_obj, "username"), NULL);
  char *password = js_getstr(js, js_get(js, url_obj, "password"), NULL);
  char *hostname = js_getstr(js, js_get(js, url_obj, "hostname"), NULL);
  char *port = js_getstr(js, js_get(js, url_obj, "port"), NULL);
  char *pathname = js_getstr(js, js_get(js, url_obj, "pathname"), NULL);
  char *search = js_getstr(js, js_get(js, url_obj, "search"), NULL);
  char *hash = js_getstr(js, js_get(js, url_obj, "hash"), NULL);

  char *host;
  if (port && *port) {
    size_t hlen = strlen(hostname) + strlen(port) + 2;
    host = malloc(hlen);
    snprintf(host, hlen, "%s:%s", hostname, port);
  } else {
    host = strdup(hostname ? hostname : "");
  }
  js_set(js, url_obj, "host", js_mkstr(js, host, strlen(host)));

  char *origin;
  if (port && *port) {
    size_t olen = strlen(protocol) + strlen(hostname) + strlen(port) + 8;
    origin = malloc(olen);
    snprintf(origin, olen, "%s//%s:%s", protocol ? protocol : "", hostname ? hostname : "", port);
  } else {
    size_t olen = strlen(protocol) + strlen(hostname) + 8;
    origin = malloc(olen);
    snprintf(origin, olen, "%s//%s", protocol ? protocol : "", hostname ? hostname : "");
  }
  js_set(js, url_obj, "origin", js_mkstr(js, origin, strlen(origin)));

  char *href = build_href(
    protocol ? protocol : "", username ? username : "",
    password ? password : "", hostname ? hostname : "",
    port ? port : "", pathname ? pathname : "/",
    search ? search : "", hash ? hash : ""
  );
  
  js_set(js, url_obj, "href", js_mkstr(js, href, strlen(href)));

  free(host);
  free(origin);
  free(href);
}

static jsval_t url_toString(struct js *js, jsval_t *args, int nargs) {
  return js_get(js, js_getthis(js), "href");
}

static jsval_t js_URLSearchParams(struct js *js, jsval_t *args, int nargs) {
  jsval_t usp = js_mkobj(js);
  jsval_t proto = js_get_ctor_proto(js, "URLSearchParams", 15);
  if (is_special_object(proto)) js_set_proto(js, usp, proto);
  
  jsval_t entries = js_mkarr(js);
  js_set_slot(js, usp, SLOT_ENTRIES, entries);

  if (nargs < 1 || vtype(args[0]) != T_STR) return usp;
  char *init = js_getstr(js, args[0], NULL);
  if (!init) return usp;

  const char *p = init;
  if (*p == '?') p++;

parse_pair:
  if (!*p) return usp;
  const char *amp = strchr(p, '&');
  size_t plen = amp ? (size_t)(amp - p) : strlen(p);
  char *pair = strndup(p, plen);
  char *eq = strchr(pair, '=');
  char *key = pair, *val = eq ? (eq[0] = '\0', eq + 1) : "";
  char *dk = url_decode_component(key);
  char *dv = url_decode_component(val);
  jsval_t entry = js_mkarr(js);
  js_arr_push(js, entry, js_mkstr(js, dk, strlen(dk)));
  js_arr_push(js, entry, js_mkstr(js, dv, strlen(dv)));
  js_arr_push(js, entries, entry);
  free(pair); free(dk); free(dv);
  if (!amp) return usp;
  p = amp + 1;
  goto parse_pair;
}

static jsval_t js_URL(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "TypeError: URL requires at least 1 argument");

  char *url_str = js_getstr(js, args[0], NULL);
  char *base_str = (nargs > 1) ? js_getstr(js, args[1], NULL) : NULL;
  if (!url_str) return js_mkerr(js, "TypeError: Invalid URL");

  parsed_url_t parsed;
  if (parse_url(url_str, base_str, &parsed) < 0) {
    return js_mkerr(js, "TypeError: Invalid URL");
  }

  jsval_t url_obj = js_mkobj(js);
  jsval_t proto = js_get_ctor_proto(js, "URL", 3);
  if (is_special_object(proto)) js_set_proto(js, url_obj, proto);

  js_set(js, url_obj, "protocol", js_mkstr(js, parsed.protocol, strlen(parsed.protocol)));
  js_set(js, url_obj, "username", js_mkstr(js, parsed.username, strlen(parsed.username)));
  js_set(js, url_obj, "password", js_mkstr(js, parsed.password, strlen(parsed.password)));
  js_set(js, url_obj, "hostname", js_mkstr(js, parsed.hostname, strlen(parsed.hostname)));
  js_set(js, url_obj, "port", js_mkstr(js, parsed.port, strlen(parsed.port)));
  js_set(js, url_obj, "pathname", js_mkstr(js, parsed.pathname, strlen(parsed.pathname)));
  js_set(js, url_obj, "search", js_mkstr(js, parsed.search, strlen(parsed.search)));
  js_set(js, url_obj, "hash", js_mkstr(js, parsed.hash, strlen(parsed.hash)));

  update_url_href(js, url_obj);

  jsval_t search_params;
  if (parsed.search && *parsed.search) {
    const char *qs = parsed.search[0] == '?' ? parsed.search + 1 : parsed.search;
    jsval_t arg = js_mkstr(js, qs, strlen(qs));
    search_params = js_URLSearchParams(js, &arg, 1);
  } else search_params = js_URLSearchParams(js, NULL, 0);
  
  js_set_slot(js, search_params, SLOT_DATA, url_obj);
  js_set(js, url_obj, "searchParams", search_params);

  free_parsed_url(&parsed);
  return url_obj;
}

static jsval_t usp_get(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mknull();
  jsval_t this_val = js_getthis(js);
  
  char *key = js_getstr(js, args[0], NULL);
  if (!key) return js_mknull();

  jsval_t entries = js_get_slot(js, this_val, SLOT_ENTRIES);
  if (!is_special_object(entries)) return js_mknull();

  jsoff_t len = js_arr_len(js, entries);
  for (jsoff_t i = 0; i < len; i++) {
    jsval_t entry = js_arr_get(js, entries, i);
    char *ks = js_getstr(js, js_arr_get(js, entry, 0), NULL);
    if (ks && strcmp(ks, key) == 0) return js_arr_get(js, entry, 1);
  }
  
  return js_mknull();
}

static jsval_t usp_getAll(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkarr(js);
  jsval_t this_val = js_getthis(js);
  
  char *key = js_getstr(js, args[0], NULL);
  if (!key) return js_mkarr(js);

  jsval_t result = js_mkarr(js);
  jsval_t entries = js_get_slot(js, this_val, SLOT_ENTRIES);
  if (!is_special_object(entries)) return result;

  jsoff_t len = js_arr_len(js, entries);
  for (jsoff_t i = 0; i < len; i++) {
    jsval_t entry = js_arr_get(js, entries, i);
    char *ks = js_getstr(js, js_arr_get(js, entry, 0), NULL);
    if (ks && strcmp(ks, key) == 0) js_arr_push(js, result, js_arr_get(js, entry, 1));
  }
  
  return result;
}

static jsval_t usp_has(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_false;
  jsval_t this_val = js_getthis(js);
  
  char *key = js_getstr(js, args[0], NULL);
  if (!key) return js_false;

  jsval_t entries = js_get_slot(js, this_val, SLOT_ENTRIES);
  if (!is_special_object(entries)) return js_false;

  jsoff_t len = js_arr_len(js, entries);
  for (jsoff_t i = 0; i < len; i++) {
    jsval_t entry = js_arr_get(js, entries, i);
    char *ks = js_getstr(js, js_arr_get(js, entry, 0), NULL);
    if (ks && strcmp(ks, key) == 0) return js_true;
  }
  
  return js_false;
}

static void usp_sync_url(struct js *js, jsval_t this_val) {
  jsval_t url_obj = js_get_slot(js, this_val, SLOT_DATA);
  if (!is_special_object(url_obj)) return;

  jsval_t entries = js_get_slot(js, this_val, SLOT_ENTRIES);
  jsoff_t len = js_arr_len(js, entries);

  size_t buf_size = 1024;
  char *buf = try_oom(buf_size);
  size_t pos = 0;

  for (jsoff_t i = 0; i < len; i++) {
    jsval_t entry = js_arr_get(js, entries, i);
    char *k = js_getstr(js, js_arr_get(js, entry, 0), NULL);
    char *v = js_getstr(js, js_arr_get(js, entry, 1), NULL);
    if (!k) continue;
    
    char *ek = url_encode_component(k);
    char *ev = url_encode_component(v ? v : "");
    
    size_t needed = strlen(ek) + strlen(ev) + 3;
    if (pos + needed >= buf_size) {
      buf_size = buf_size * 2 + needed;
      buf = realloc(buf, buf_size);
      if (!buf) { free(buf); return; }
    }
    
    buf[pos] = pos == 0 ? '?' : '&'; pos++;
    pos += sprintf(buf + pos, "%s=%s", ek, ev);
    free(ek); free(ev);
  }
  
  buf[pos] = '\0';
  js_set(js, url_obj, "search", js_mkstr(js, buf, pos));
  update_url_href(js, url_obj);
  free(buf);
}

static jsval_t usp_set(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  jsval_t this_val = js_getthis(js);
  
  char *key = js_getstr(js, args[0], NULL);
  if (!key) return js_mkundef();

  jsval_t entries = js_get_slot(js, this_val, SLOT_ENTRIES);
  jsoff_t len = js_arr_len(js, entries);

  jsval_t new_entries = js_mkarr(js);
  int found = 0;

  for (jsoff_t i = 0; i < len; i++) {
    jsval_t entry = js_arr_get(js, entries, i);
    char *ks = js_getstr(js, js_arr_get(js, entry, 0), NULL);
    if (ks && strcmp(ks, key) == 0) {
      if (!found) {
        jsval_t new_entry = js_mkarr(js);
        js_arr_push(js, new_entry, args[0]);
        js_arr_push(js, new_entry, args[1]);
        js_arr_push(js, new_entries, new_entry);
        found = 1;
      }
    } else js_arr_push(js, new_entries, entry);
  }

  if (!found) {
    jsval_t new_entry = js_mkarr(js);
    js_arr_push(js, new_entry, args[0]);
    js_arr_push(js, new_entry, args[1]);
    js_arr_push(js, new_entries, new_entry);
  }

  js_set_slot(js, this_val, SLOT_ENTRIES, new_entries);
  usp_sync_url(js, this_val);
  
  return js_mkundef();
}

static jsval_t usp_append(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  jsval_t this_val = js_getthis(js);

  jsval_t entries = js_get_slot(js, this_val, SLOT_ENTRIES);
  jsval_t entry = js_mkarr(js);
  js_arr_push(js, entry, args[0]);
  js_arr_push(js, entry, args[1]);
  js_arr_push(js, entries, entry);

  usp_sync_url(js, this_val);
  return js_mkundef();
}

static jsval_t usp_delete(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  jsval_t this_val = js_getthis(js);
  char *key = js_getstr(js, args[0], NULL);
  if (!key) return js_mkundef();

  jsval_t entries = js_get_slot(js, this_val, SLOT_ENTRIES);
  jsoff_t len = js_arr_len(js, entries);

  jsval_t new_entries = js_mkarr(js);
  for (jsoff_t i = 0; i < len; i++) {
    jsval_t entry = js_arr_get(js, entries, i);
    char *ks = js_getstr(js, js_arr_get(js, entry, 0), NULL);
    if (!ks || strcmp(ks, key) != 0) js_arr_push(js, new_entries, entry);
  }

  js_set_slot(js, this_val, SLOT_ENTRIES, new_entries);
  usp_sync_url(js, this_val);
  js->needs_gc = true;
  
  return js_mkundef();
}

static jsval_t usp_toString(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_val = js_getthis(js);
  jsval_t entries = js_get_slot(js, this_val, SLOT_ENTRIES);
  jsoff_t len = js_arr_len(js, entries);

  size_t buf_size = 1024;
  char *buf = try_oom(buf_size);
  size_t pos = 0;

  for (jsoff_t i = 0; i < len; i++) {
    jsval_t entry = js_arr_get(js, entries, i);
    char *k = js_getstr(js, js_arr_get(js, entry, 0), NULL);
    char *v = js_getstr(js, js_arr_get(js, entry, 1), NULL);
    if (!k) continue;
    
    char *ek = url_encode_component(k);
    char *ev = url_encode_component(v ? v : "");
    size_t needed = strlen(ek) + strlen(ev) + 3;
    
    if (pos + needed >= buf_size) {
      buf_size = buf_size * 2 + needed;
      buf = realloc(buf, buf_size);
      if (!buf) { free(buf); return js_mkstr(js, "", 0); }
    }
    
    if (pos > 0) buf[pos++] = '&';
    pos += sprintf(buf + pos, "%s=%s", ek, ev);
    free(ek);
    free(ev);
  }
  
  buf[pos] = '\0';
  jsval_t ret = js_mkstr(js, buf, pos);
  free(buf);
  
  return ret;
}

void init_url_module(void) {
  ant_t *js = rt->js;
  jsval_t glob = js->global;

  jsval_t url_ctor = js_mkobj(js);
  jsval_t url_proto = js_mkobj(js);
  
  js_set(js, url_proto, "toString", js_mkfun(url_toString));
  js_set(js, url_proto, get_toStringTag_sym_key(), js_mkstr(js, "URL", 3));
  
  js_set_slot(js, url_ctor, SLOT_CFUNC, js_mkfun(js_URL));
  js_mkprop_fast(js, url_ctor, "prototype", 9, url_proto);
  js_mkprop_fast(js, url_ctor, "name", 4, ANT_STRING("URL"));
  js_set_descriptor(js, url_ctor, "name", 4, 0);
  
  js_set(js, glob, "URL", js_obj_to_func(url_ctor));
  
  jsval_t usp_ctor = js_mkobj(js);
  jsval_t usp_proto = js_mkobj(js);
  
  js_set(js, usp_proto, "get", js_mkfun(usp_get));
  js_set(js, usp_proto, "getAll", js_mkfun(usp_getAll));
  js_set(js, usp_proto, "has", js_mkfun(usp_has));
  js_set(js, usp_proto, "set", js_mkfun(usp_set));
  js_set(js, usp_proto, "append", js_mkfun(usp_append));
  js_set(js, usp_proto, "delete", js_mkfun(usp_delete));
  js_set(js, usp_proto, "toString", js_mkfun(usp_toString));
  js_set(js, usp_proto, get_toStringTag_sym_key(), js_mkstr(js, "URLSearchParams", 15));
  
  js_set_slot(js, usp_ctor, SLOT_CFUNC, js_mkfun(js_URLSearchParams));
  js_mkprop_fast(js, usp_ctor, "prototype", 9, usp_proto);
  js_mkprop_fast(js, usp_ctor, "name", 4, ANT_STRING("URLSearchParams"));
  js_set_descriptor(js, usp_ctor, "name", 4, 0);
  
  js_set(js, glob, "URLSearchParams", js_obj_to_func(usp_ctor));
}
