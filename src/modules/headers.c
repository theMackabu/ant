#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#include "ant.h"
#include "ptr.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "descriptors.h"
#include "utf8.h"
#include "silver/engine.h"

#include "modules/headers.h"
#include "modules/symbol.h"

typedef struct hdr_entry {
  char *name;
  char *value;
  struct hdr_entry *next;
  char storage[];
} hdr_entry_t;

enum { HEADERS_INLINE_ENTRY_BYTES = 96 };

struct headers_data {
  hdr_entry_t  *head;
  hdr_entry_t **tail;
  size_t count;
  bool inline_used;
  union {
    max_align_t align;
    unsigned char bytes[HEADERS_INLINE_ENTRY_BYTES];
  } inline_entry;
};

typedef struct headers_data hdr_list_t;

_Static_assert(sizeof(headers_data_t) <= 256, "headers data exceeds native arena slot");

typedef struct {
  char *name;
  char *value;
} sorted_pair_t;

typedef struct {
  hdr_list_t *list;
  size_t index;
  int kind;
} hdr_iter_t;

enum { 
  ITER_ENTRIES = 0,
  ITER_KEYS = 1,
  ITER_VALUES = 2
};

ant_value_t g_headers_proto      = 0;
ant_value_t g_headers_iter_proto = 0;

enum {
  HEADERS_NATIVE_TAG = 0x48445253u, // HDRS
  HEADERS_ITER_NATIVE_TAG = 0x48444954u // HDIT
};

static hdr_list_t *list_new(void) {
  hdr_list_t *l = js_native_data_alloc(rt->js, sizeof(hdr_list_t));
  if (!l) return NULL;
  l->head = NULL;
  l->tail = &l->head;
  l->count = 0;
  l->inline_used = false;
  return l;
}

headers_data_t *headers_data_create(void) {
  return list_new();
}

static void list_free(hdr_list_t *l) {
  if (!l) return;
  for (hdr_entry_t *e = l->head; e; ) {
    hdr_entry_t *n = e->next;
    if ((void *)e != (void *)l->inline_entry.bytes) free(e);
    e = n;
  }
  js_native_data_free(rt->js, l);
}

void headers_data_destroy(headers_data_t *data) {
  list_free(data);
}

static hdr_list_t *get_list(ant_value_t obj) {
  return (hdr_list_t *)js_get_native(obj, HEADERS_NATIVE_TAG);
}

static void headers_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  hdr_list_t *list = get_list(value);
  list_free(list);
  js_clear_native(value, HEADERS_NATIVE_TAG);
}

static void headers_iter_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  free(js_get_native(value, HEADERS_ITER_NATIVE_TAG));
  js_clear_native(value, HEADERS_ITER_NATIVE_TAG);
}

bool headers_is_immutable(ant_value_t obj) {
  return js_get_slot(obj, SLOT_HEADERS_GUARD) == js_true;
}

bool headers_is_headers(ant_value_t obj) {
  return get_list(obj) != NULL;
}

static bool is_token_char(unsigned char c) {
  if ((c >= '0' && c <= '9') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= 'a' && c <= 'z')) return true;
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '*': case '+': case '-': case '.': case '^': case '_':
    case '`': case '|': case '~':
      return true;
    default:
      return false;
  }
}

static inline char ascii_lower(unsigned char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : (char)c;
}

static bool ascii_case_equal(const char *a, const char *b) {
  while (*a && *b) {
    if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b)) return false;
    a++;
    b++;
  }
  return *a == *b;
}

static bool is_valid_name_n(const char *s, size_t len) {
  if (!s || len == 0) return false;
  for (size_t i = 0; i < len; i++)
    if (!is_token_char((unsigned char)s[i])) return false;
  return true;
}

static bool is_valid_name(const char *s) {
  return s && is_valid_name_n(s, strlen(s));
}

static bool is_valid_value(const char *s) {
  if (!s) return false;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    unsigned char c = *p;
    if (c == 0 || c == '\r' || c == '\n') return false;
  }
  return true;
}

static bool is_valid_value_n(const char *s, size_t len) {
  if (!s) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == 0 || c == '\r' || c == '\n') return false;
  }
  return true;
}

static bool bytes_are_ascii(const char *s, size_t len) {
  for (size_t i = 0; i < len; i++)
    if ((unsigned char)s[i] >= 0x80) return false;
  return true;
}

static ant_value_t header_value_from_js(
  ant_t *js,
  const char *utf8, size_t utf8_len,
  const char **out, size_t *out_len,
  uint8_t **owned
) {
  *out = utf8 ? utf8 : "";
  *out_len = utf8 ? utf8_len : 0;
  *owned = NULL;

  bool has_non_ascii = false;
  for (size_t i = 0; i < utf8_len; i++) {
    unsigned char c = (unsigned char)utf8[i];
    if (c >= 0x80) has_non_ascii = true;
    else if (c == 0 || c == '\r' || c == '\n')
      return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid header value");
  }
  if (!utf8 || !has_non_ascii) return js_mkundef();

  bool is_latin1 = true;
  uint8_t *bytes = utf8_to_latin1(utf8, utf8_len, out_len, &is_latin1);
  if (!bytes) {
    if (!is_latin1)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid header value");
    return js_mkerr(js, "out of memory");
  }

  *out = (const char *)bytes;
  *owned = bytes;
  return js_mkundef();
}

static ant_value_t header_value_to_js(
  ant_t *js, const char *bytes, size_t len
) {
  if (!bytes || bytes_are_ascii(bytes, len))
    return js_mkstr(js, bytes ? bytes : "", bytes ? len : 0);

  size_t utf8_len = 0;
  char *utf8 = latin1_to_utf8((const uint8_t *)bytes, len, &utf8_len);
  if (!utf8) return js_mkerr(js, "out of memory");

  ant_value_t out = js_mkstr(js, utf8, utf8_len);
  free(utf8);
  return out;
}

static char *normalize_value(const char *s) {
  if (!s) return strdup("");
  while (*s == ' ' || *s == '\t') s++;
  
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
  
  char *out = malloc(len + 1);
  if (!out) return NULL;
  
  memcpy(out, s, len);
  out[len] = '\0';
  
  return out;
}

static char *lowercase_dup(const char *s) {
  if (!s) return strdup("");
  size_t len = strlen(s);
  char *out = malloc(len + 1);
  if (!out) return NULL;
  for (size_t i = 0; i <= len; i++)
    out[i] = (char)tolower((unsigned char)s[i]);
  return out;
}

static ant_value_t headers_require_mutable(ant_t *js, ant_value_t headers) {
  if (!headers_is_immutable(headers)) return js_mkundef();
  return js_mkerr_typed(js, JS_ERR_TYPE, "Headers are immutable");
}

static bool list_append_parts(
  hdr_list_t *l,
  const char *name, size_t name_len,
  const char *value, size_t value_len,
  bool lowercase_name
) {
  size_t required = 0;
  hdr_entry_t *e = NULL;

  if (name_len > SIZE_MAX - value_len - sizeof(hdr_entry_t) - 2) return false;
  required = sizeof(hdr_entry_t) + name_len + value_len + 2;
  if (!l->inline_used && required <= sizeof(l->inline_entry.bytes)) {
    e = (hdr_entry_t *)l->inline_entry.bytes;
    l->inline_used = true;
  } else e = malloc(required);
  if (!e) return false;

  e->name = e->storage;
  e->value = e->storage + name_len + 1;
  e->next = NULL;
  for (size_t i = 0; i < name_len; i++) {
    unsigned char c = (unsigned char)name[i];
    e->name[i] = lowercase_name ? ascii_lower(c) : (char)c;
  }
  e->name[name_len] = '\0';
  memcpy(e->value, value, value_len);
  e->value[value_len] = '\0';

  *l->tail = e;
  l->tail  = &e->next;
  l->count++;
  return true;
}

static void list_append_raw(hdr_list_t *l, const char *lower_name, const char *value) {
  if (!lower_name || !value) return;
  (void)list_append_parts(
    l,
    lower_name, strlen(lower_name),
    value, strlen(value),
    false);
}

headers_data_t *headers_data_copy(const headers_data_t *src) {
  if (!src) return NULL;
  headers_data_t *dst = list_new();
  if (!dst) return NULL;

  for (const hdr_entry_t *e = src->head; e; e = e->next)
    list_append_raw(dst, e->name, e->value);
  return dst;
}

static void list_delete_name(hdr_list_t *l, const char *lower_name) {
  hdr_entry_t **pp = &l->head;
  l->tail = &l->head;
  while (*pp) {
  if (strcmp((*pp)->name, lower_name) == 0) {
    hdr_entry_t *dead = *pp;
    *pp = dead->next;
    if ((void *)dead == (void *)l->inline_entry.bytes) l->inline_used = false;
    else free(dead);
    l->count--;
  } else {
    l->tail = &(*pp)->next;
    pp = &(*pp)->next;
  }}
}

static int cmp_pairs(const void *a, const void *b) {
  return strcmp(((const sorted_pair_t *)a)->name, ((const sorted_pair_t *)b)->name);
}

static sorted_pair_t *build_sorted_view(hdr_list_t *l, size_t *out) {
  *out = 0;
  if (!l || l->count == 0) return NULL;

  sorted_pair_t *raw = malloc(l->count * sizeof(sorted_pair_t));
  if (!raw) return NULL;

  size_t n = 0;
  for (hdr_entry_t *e = l->head; e; e = e->next) {
    raw[n].name  = e->name;
    raw[n].value = e->value;
    n++;
  }
  
  qsort(raw, n, sizeof(sorted_pair_t), cmp_pairs);
  sorted_pair_t *res = malloc(n * sizeof(sorted_pair_t));
  if (!res) { free(raw); return NULL; }

  size_t ri = 0;
  for (size_t i = 0; i < n; ) {
  if (strcmp(raw[i].name, "set-cookie") == 0) {
    res[ri].name  = strdup(raw[i].name);
    res[ri].value = strdup(raw[i].value);
    ri++; i++;
  } else {
    size_t j = i + 1;
    size_t total = strlen(raw[i].value);
    while (j < n && strcmp(raw[j].name, raw[i].name) == 0) {
      total += 2 + strlen(raw[j].value);
      j++;
    }
    char *combined = malloc(total + 1);
    if (!combined) combined = strdup("");
    size_t pos = 0;
    for (size_t k = i; k < j; k++) {
      if (k > i) { combined[pos++] = ','; combined[pos++] = ' '; }
      size_t vl = strlen(raw[k].value);
      memcpy(combined + pos, raw[k].value, vl);
      pos += vl;
    }
    combined[pos] = '\0';
    res[ri].name  = strdup(raw[i].name);
    res[ri].value = combined;
    ri++; i = j;
  }}

  free(raw);
  *out = ri;
  
  return res;
}

static void free_sorted_view(sorted_pair_t *v, size_t n) {
  if (!v) return;
  for (size_t i = 0; i < n; i++) { free(v[i].name); free(v[i].value); }
  free(v);
}

static ant_value_t headers_append_name_value_bytes_n(
  ant_t *js, hdr_list_t *l,
  const char *name, size_t name_len,
  const char *value, size_t value_len
) {
  const char *value_start = value;

  if (!is_valid_name_n(name, name_len))
    return js_mkerr_typed(
      js, JS_ERR_TYPE, "Invalid header name: %.*s",
      (int)name_len, name ? name : "");

  if (!value_start) {
    value_start = "";
    value_len = 0;
  }
  while (value_len > 0 && (*value_start == ' ' || *value_start == '\t')) {
    value_start++;
    value_len--;
  }
  while (value_len > 0 &&
         (value_start[value_len - 1] == ' ' || value_start[value_len - 1] == '\t'))
    value_len--;
  if (!is_valid_value_n(value_start, value_len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid header value");

  if (!list_append_parts(l, name, name_len, value_start, value_len, true))
    return js_mkerr(js, "out of memory");
  return js_mkundef();
}

static ant_value_t headers_append_name_value_js_n(
  ant_t *js, hdr_list_t *l,
  const char *name, size_t name_len,
  const char *value, size_t value_len
) {
  const char *bytes = NULL;
  size_t bytes_len = 0;
  uint8_t *owned = NULL;
  ant_value_t converted = header_value_from_js(
    js, value, value_len, &bytes, &bytes_len, &owned);
  if (is_err(converted)) return converted;

  if (!is_valid_name_n(name, name_len)) {
    free(owned);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid header name");
  }
  while (bytes_len > 0 && (*bytes == ' ' || *bytes == '\t')) {
    bytes++;
    bytes_len--;
  }
  while (bytes_len > 0 &&
         (bytes[bytes_len - 1] == ' ' || bytes[bytes_len - 1] == '\t'))
    bytes_len--;

  ant_value_t out = js_mkundef();
  if (!list_append_parts(l, name, name_len, bytes, bytes_len, true))
    out = js_mkerr(js, "out of memory");
  free(owned);
  return out;
}

static const char *g_content_type_interned;

static ant_value_t headers_append_lower_name_value_bytes_n(
  ant_t *js, hdr_list_t *l,
  const char *name, size_t name_len,
  const char *value, size_t value_len
) {
  const char *value_start = value ? value : "";
  while (value_len > 0 && (*value_start == ' ' || *value_start == '\t')) {
    value_start++;
    value_len--;
  }
  while (value_len > 0 &&
         (value_start[value_len - 1] == ' ' || value_start[value_len - 1] == '\t'))
    value_len--;
  if (!is_valid_value_n(value_start, value_len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid header value");
  if (!list_append_parts(l, name, name_len, value_start, value_len, false))
    return js_mkerr(js, "out of memory");
  return js_mkundef();
}

static ant_value_t headers_append_lower_name_value_js_n(
  ant_t *js, hdr_list_t *l,
  const char *name, size_t name_len,
  const char *value, size_t value_len
) {
  const char *bytes = NULL;
  size_t bytes_len = 0;
  uint8_t *owned = NULL;
  ant_value_t converted = header_value_from_js(
    js, value, value_len, &bytes, &bytes_len, &owned);
  if (is_err(converted)) return converted;

  while (bytes_len > 0 && (*bytes == ' ' || *bytes == '\t')) {
    bytes++;
    bytes_len--;
  }
  while (bytes_len > 0 &&
         (bytes[bytes_len - 1] == ' ' || bytes[bytes_len - 1] == '\t'))
    bytes_len--;

  ant_value_t out = js_mkundef();
  if (!list_append_parts(l, name, name_len, bytes, bytes_len, false))
    out = js_mkerr(js, "out of memory");
  free(owned);
  return out;
}

static ant_value_t headers_append_name_value(ant_t *js, hdr_list_t *l, const char *name, const char *value) {
  return headers_append_name_value_bytes_n(
    js, l,
    name, name ? strlen(name) : 0,
    value, value ? strlen(value) : 0);
}

static ant_value_t headers_append_pair(ant_t *js, hdr_list_t *l, ant_value_t name_v, ant_value_t value_v) {
  const char *name = NULL;
  const char *value = NULL;
  size_t name_len = 0;
  size_t value_len = 0;

  if (vtype(name_v) != T_STR) {
    name_v = js_tostring_val(js, name_v);
    if (is_err(name_v)) return name_v;
  }
  
  if (vtype(value_v) != T_STR) {
    value_v = js_tostring_val(js, value_v);
    if (is_err(value_v)) return value_v;
  }

  name = js_getstr(js, name_v, &name_len);
  value = js_getstr(js, value_v, &value_len);
  return headers_append_name_value_js_n(js, l, name, name_len, value, value_len);
}

static ant_value_t headers_append_record_value(
  ant_t *js, hdr_list_t *l,
  const char *name, size_t name_len, ant_value_t value_v
) {
  size_t value_len = 0;
  if (vtype(value_v) != T_STR) {
    value_v = js_tostring_val(js, value_v);
    if (is_err(value_v)) return value_v;
  }
  const char *value = js_getstr(js, value_v, &value_len);
  return headers_append_name_value_js_n(js, l, name, name_len, value, value_len);
}

ant_value_t headers_append_value(ant_t *js, ant_value_t hdrs, ant_value_t name_v, ant_value_t value_v) {
  hdr_list_t *l = get_list(hdrs);
  ant_value_t r = 0;

  if (!l) return js_mkerr(js, "Invalid Headers object");
  r = headers_append_pair(js, l, name_v, value_v);
  
  if (is_err(r)) return r;
  return js_mkundef();
}

ant_value_t headers_append_literal(ant_t *js, ant_value_t hdrs, const char *name, const char *value) {
  hdr_list_t *l = get_list(hdrs);
  ant_value_t r = 0;

  if (!l) return js_mkerr(js, "Invalid Headers object");
  r = headers_append_name_value(js, l, name, value);
  
  if (is_err(r)) return r;
  return js_mkundef();
}

static ant_value_t init_from_sequence(ant_t *js, hdr_list_t *l, ant_value_t seq) {
  js_iter_t it;
  if (!js_iter_open(js, seq, &it)) return js_mkerr_typed(js, JS_ERR_TYPE, "Headers init is not iterable");

  ant_value_t pair;
  while (js_iter_next(js, &it, &pair)) {
    uint8_t pt = vtype(pair);
    if (pt != T_ARR && pt != T_OBJ) {
      js_iter_close(js, &it);
      return js_mkerr_typed(js, JS_ERR_TYPE, "Each header init pair must be a sequence");
    }
    
    if (js_arr_len(js, pair) != 2) {
      js_iter_close(js, &it);
      return js_mkerr_typed(js, JS_ERR_TYPE, "Each header init pair must have exactly 2 elements");
    }
    
    ant_value_t r = headers_append_pair(js, l, js_arr_get(js, pair, 0), js_arr_get(js, pair, 1));
    if (is_err(r)) { js_iter_close(js, &it); return r; }
  }
  
  return js_mkundef();
}

static ant_value_t init_from_record(ant_t *js, hdr_list_t *l, ant_value_t obj) {
  ant_iter_t it = js_prop_iter_begin(js, obj);
  const char *key;
  size_t key_len;
  ant_value_t val;

  while (js_prop_iter_next(&it, &key, &key_len, &val)) {
    ant_value_t r = headers_append_record_value(js, l, key, key_len, val);
    if (is_err(r)) { js_prop_iter_end(&it); return r; }
  }
  
  js_prop_iter_end(&it);
  return js_mkundef();
}

bool advance_headers(ant_t *js, js_iter_t *it, ant_value_t *out) {
  hdr_iter_t *st = (hdr_iter_t *)js_get_native(it->iterator, HEADERS_ITER_NATIVE_TAG);
  if (!st) return false;

  size_t count = 0;
  sorted_pair_t *view = build_sorted_view(st->list, &count);

  if (st->index >= count) {
    free_sorted_view(view, count);
    return false;
  }

  sorted_pair_t *e = &view[st->index];
  switch (st->kind) {
  case ITER_KEYS:
    *out = js_mkstr(js, e->name, strlen(e->name));
    break;
  case ITER_VALUES:
    *out = header_value_to_js(js, e->value, strlen(e->value));
    break;
  default: {
    *out = js_mkarr(js);
    js_arr_push(js, *out, js_mkstr(js, e->name,  strlen(e->name)));
    js_arr_push(js, *out, header_value_to_js(js, e->value, strlen(e->value)));
    break;
  }}

  free_sorted_view(view, count);
  st->index++;
  return true;
}

static ant_value_t headers_iter_next(ant_t *js, ant_value_t *args, int nargs) {
  return js_iter_next_result(js, advance_headers);
}

static ant_value_t make_headers_iter(ant_t *js, ant_value_t headers_obj, int kind) {
  hdr_list_t *l = get_list(headers_obj);
  if (!l) return js_mkerr(js, "Invalid Headers object");

  hdr_iter_t *st = ant_calloc(sizeof(hdr_iter_t));
  if (!st) return js_mkerr(js, "out of memory");
  
  st->list  = l;
  st->kind  = kind;

  ant_value_t iter = js_mkobj(js);
  js_set_proto_init(iter, g_headers_iter_proto);
  js_set_native(iter, st, HEADERS_ITER_NATIVE_TAG);
  js_set_finalizer(iter, headers_iter_finalize);
  js_set_slot_wb(js, iter, SLOT_AUX, headers_obj);
  
  return iter;
}

static ant_value_t js_headers_append(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "Headers.append requires 2 arguments");
  hdr_list_t *l = get_list(js->this_val);
  
  if (!l) return js_mkerr(js, "Invalid Headers object");
  ant_value_t guard_err = headers_require_mutable(js, js->this_val);
  
  if (is_err(guard_err)) return guard_err;
  ant_value_t r = headers_append_pair(js, l, args[0], args[1]);
  
  if (is_err(r)) return r;
  return js_mkundef();
}

static ant_value_t js_headers_set(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "Headers.set requires 2 arguments");
  hdr_list_t *l = get_list(js->this_val);
  
  if (!l) return js_mkerr(js, "Invalid Headers object");
  ant_value_t guard_err = headers_require_mutable(js, js->this_val);
  if (is_err(guard_err)) return guard_err;

  ant_value_t name_v  = args[0];
  ant_value_t value_v = args[1];
  if (vtype(name_v)  != T_STR) { name_v  = js_tostring_val(js, name_v);  if (is_err(name_v))  return name_v;  }
  if (vtype(value_v) != T_STR) { value_v = js_tostring_val(js, value_v); if (is_err(value_v)) return value_v; }

  size_t name_len = 0;
  size_t value_len = 0;
  const char *name = js_getstr(js, name_v, &name_len);
  const char *value = js_getstr(js, value_v, &value_len);

  const char *value_bytes = NULL;
  size_t value_bytes_len = 0;
  uint8_t *owned_value = NULL;
  ant_value_t converted = header_value_from_js(
    js, value, value_len,
    &value_bytes, &value_bytes_len, &owned_value);
  if (is_err(converted)) return converted;

  if (!is_valid_name_n(name, name_len)) {
    free(owned_value);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid header name");
  }

  while (value_bytes_len > 0 &&
         (*value_bytes == ' ' || *value_bytes == '\t')) {
    value_bytes++;
    value_bytes_len--;
  }
  while (value_bytes_len > 0 &&
         (value_bytes[value_bytes_len - 1] == ' ' ||
          value_bytes[value_bytes_len - 1] == '\t'))
    value_bytes_len--;
  char *lower = malloc(name_len + 1);
  if (!lower) {
    free(owned_value);
    return js_mkerr(js, "out of memory");
  }
  for (size_t i = 0; i < name_len; i++)
    lower[i] = ascii_lower((unsigned char)name[i]);
  lower[name_len] = '\0';

  list_delete_name(l, lower);
  bool appended = list_append_parts(
    l, lower, name_len, value_bytes, value_bytes_len, false);

  free(lower);
  free(owned_value);
  if (!appended) return js_mkerr(js, "out of memory");
  
  return js_mkundef();
}

static ant_value_t js_headers_get(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Headers.get requires 1 argument");
  hdr_list_t *l = get_list(js->this_val);
  if (!l) return js_mknull();

  ant_value_t name_v = args[0];
  if (vtype(name_v) != T_STR) { name_v = js_tostring_val(js, name_v); if (is_err(name_v)) return name_v; }
  const char *name = js_getstr(js, name_v, NULL);
  if (!is_valid_name(name)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid header name");

  char *lower = lowercase_dup(name);
  if (!lower) return js_mkerr(js, "out of memory");

  // set-cookie is never combined per Fetch spec
  if (strcmp(lower, "set-cookie") == 0) {
    for (hdr_entry_t *e = l->head; e; e = e->next) {
      if (strcmp(e->name, lower) == 0) {
        ant_value_t ret = header_value_to_js(js, e->value, strlen(e->value));
        free(lower);
        return ret;
      }
    }
    free(lower);
    return js_mknull();
  }

  size_t total = 0;
  int count = 0;
  for (hdr_entry_t *e = l->head; e; e = e->next) {
    if (strcmp(e->name, lower) == 0) {
      if (count > 0) total += 2;
      total += strlen(e->value);
      count++;
    }
  }

  if (count == 0) { free(lower); return js_mknull(); }

  char *combined = malloc(total + 1);
  if (!combined) { free(lower); return js_mkerr(js, "out of memory"); }

  size_t pos = 0;
  int seen = 0;
  for (hdr_entry_t *e = l->head; e; e = e->next) {
    if (strcmp(e->name, lower) == 0) {
      if (seen > 0) { combined[pos++] = ','; combined[pos++] = ' '; }
      size_t vl = strlen(e->value);
      memcpy(combined + pos, e->value, vl);
      pos += vl;
      seen++;
    }
  }
  combined[pos] = '\0';
  free(lower);

  ant_value_t ret = header_value_to_js(js, combined, pos);
  free(combined);
  return ret;
}

static ant_value_t js_headers_has(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Headers.has requires 1 argument");
  hdr_list_t *l = get_list(js->this_val);
  if (!l) return js_false;

  ant_value_t name_v = args[0];
  if (vtype(name_v) != T_STR) { name_v = js_tostring_val(js, name_v); if (is_err(name_v)) return name_v; }
  const char *name = js_getstr(js, name_v, NULL);
  if (!is_valid_name(name)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid header name");

  char *lower = lowercase_dup(name);
  if (!lower) return js_mkerr(js, "out of memory");

  bool found = false;
  for (hdr_entry_t *e = l->head; e; e = e->next) {
    if (strcmp(e->name, lower) == 0) { found = true; break; }
  }
  free(lower);
  return js_bool(found);
}

static ant_value_t js_headers_delete(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Headers.delete requires 1 argument");
  hdr_list_t *l = get_list(js->this_val);
  
  if (!l) return js_mkundef();
  ant_value_t guard_err = headers_require_mutable(js, js->this_val);
  if (is_err(guard_err)) return guard_err;

  ant_value_t name_v = args[0];
  if (vtype(name_v) != T_STR) { name_v = js_tostring_val(js, name_v); if (is_err(name_v)) return name_v; }
  const char *name = js_getstr(js, name_v, NULL);
  if (!is_valid_name(name)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid header name");

  char *lower = lowercase_dup(name);
  if (!lower) return js_mkerr(js, "out of memory");
  list_delete_name(l, lower);
  free(lower);
  
  return js_mkundef();
}

static ant_value_t js_headers_get_set_cookie(ant_t *js, ant_value_t *args, int nargs) {
  hdr_list_t *l = get_list(js->this_val);
  ant_value_t arr = js_mkarr(js);
  if (!l) return arr;
  for (hdr_entry_t *e = l->head; e; e = e->next) {
    if (strcmp(e->name, "set-cookie") == 0)
      js_arr_push(js, arr, header_value_to_js(js, e->value, strlen(e->value)));
  }
  return arr;
}

static ant_value_t js_headers_for_each(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Headers.forEach requires 1 argument");

  ant_value_t cb = args[0];
  uint8_t cbt = vtype(cb);
  if (cbt != T_FUNC && cbt != T_CFUNC)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Headers.forEach callback must be callable");

  ant_value_t this_obj = js->this_val;
  hdr_list_t *l = get_list(this_obj);
  if (!l) return js_mkundef();

  ant_value_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();

  size_t count = 0;
  sorted_pair_t *view = build_sorted_view(l, &count);

  for (size_t i = 0; i < count; i++) {
    ant_value_t call_args[3] = {
      header_value_to_js(js, view[i].value, strlen(view[i].value)),
      js_mkstr(js, view[i].name,  strlen(view[i].name)),
      this_obj
    };
    ant_value_t r = sv_vm_call(js->vm, js, cb, this_arg, call_args, 3, NULL, false);
    if (is_err(r)) { free_sorted_view(view, count); return r; }
  }

  free_sorted_view(view, count);
  return js_mkundef();
}

static ant_value_t js_headers_keys(ant_t *js, ant_value_t *args, int nargs) {
  return make_headers_iter(js, js->this_val, ITER_KEYS);
}

static ant_value_t js_headers_values(ant_t *js, ant_value_t *args, int nargs) {
  return make_headers_iter(js, js->this_val, ITER_VALUES);
}

static ant_value_t js_headers_entries(ant_t *js, ant_value_t *args, int nargs) {
  return make_headers_iter(js, js->this_val, ITER_ENTRIES);
}

static ant_value_t headers_inspect_finish(ant_t *js, ant_value_t this_obj, ant_value_t body_obj) {
  ant_value_t tag_val = js_get_sym(js, this_obj, get_toStringTag_sym());
  const char *tag = vtype(tag_val) == T_STR ? js_getstr(js, tag_val, NULL) : "Headers";

  js_inspect_builder_t builder;
  if (!js_inspect_builder_init_dynamic(&builder, js, 128)) {
    return js_mkerr(js, "out of memory");
  }

  bool ok = js_inspect_header_for(&builder, body_obj, "%s", tag);
  if (ok) ok = js_inspect_object_body(&builder, body_obj);
  if (ok) ok = js_inspect_close(&builder);

  if (!ok) {
    js_inspect_builder_dispose(&builder);
    return js_mkerr(js, "out of memory");
  }

  return js_inspect_builder_result(&builder);
}

static ant_value_t headers_inspect(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  hdr_list_t *list = get_list(this_obj);
  ant_value_t out = js_mkobj(js);

  if (!list) return js_mkerr(js, "Invalid Headers object");

  for (hdr_entry_t *e = list->head; e; e = e->next) {
    ant_value_t existing = js_get(js, out, e->name);
    ant_value_t value_js = header_value_to_js(js, e->value, strlen(e->value));
    if (is_err(value_js)) return value_js;
    if (vtype(existing) == T_UNDEF) {
      js_set(js, out, e->name, value_js);
      continue;
    }
    
    size_t existing_len = 0;
    size_t value_len = 0;
    const char *existing_str = js_getstr(js, existing, &existing_len);
    const char *value_str = js_getstr(js, value_js, &value_len);
    size_t combined_len = existing_len + 2 + value_len;
    char *combined = malloc(combined_len + 1);
    if (!combined) return js_mkerr(js, "out of memory");
    
    memcpy(combined, existing_str, existing_len);
    combined[existing_len] = ',';
    combined[existing_len + 1] = ' ';
    memcpy(combined + existing_len + 2, value_str, value_len);
    combined[combined_len] = '\0';
    
    js_set(js, out, e->name, js_mkstr(js, combined, combined_len));
    free(combined);
  }

  return headers_inspect_finish(js, this_obj, out);
}

static ant_value_t js_headers_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Headers constructor requires 'new'");

  hdr_list_t *l = list_new();
  if (!l) return js_mkerr(js, "out of memory");

  ant_value_t init = (nargs >= 1) ? args[0] : js_mkundef();

  if (vtype(init) != T_UNDEF) {
    uint8_t t = vtype(init);

    if (t == T_NULL || (t != T_OBJ && t != T_ARR && t != T_FUNC && t != T_CFUNC)) {
      list_free(l);
      return js_mkerr_typed(js, JS_ERR_TYPE,
        "Failed to construct 'Headers': The provided value is not of type 'HeadersInit'");
    }

    ant_value_t iter_fn = js_get_sym(js, init, get_iterator_sym());
    bool has_iter = (vtype(iter_fn) == T_FUNC || vtype(iter_fn) == T_CFUNC);

    ant_value_t r;
    if (t == T_ARR || has_iter) r = init_from_sequence(js, l, init);
    else                        r = init_from_record(js, l, init);
    if (is_err(r)) { list_free(l); return r; }
  }

  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_headers_proto);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);

  js_set_native(obj, l, HEADERS_NATIVE_TAG);
  js_set_finalizer(obj, headers_finalize);
  
  return obj;
}

ant_value_t headers_create_empty(ant_t *js) {
  hdr_list_t *l = list_new();
  if (!l) return js_mkerr(js, "out of memory");

  return headers_create_from_data(js, l);
}

ant_value_t headers_create_from_data(ant_t *js, headers_data_t *data) {
  if (!data) return js_mkerr(js, "out of memory");
  ant_value_t obj = js_mkobj(js);
  js_set_proto_init(obj, g_headers_proto);
  js_set_native(obj, data, HEADERS_NATIVE_TAG);
  js_set_finalizer(obj, headers_finalize);

  return obj;
}

bool headers_copy_from(ant_t *js, ant_value_t dst, ant_value_t src) {
  hdr_list_t *src_list = get_list(src);
  hdr_list_t *dst_list = get_list(dst);
  
  if (!dst_list) return false;
  if (!src_list) return true;
  
  for (hdr_entry_t *e = src_list->head; e; e = e->next)
    list_append_raw(dst_list, e->name, e->value);
  return true;
}

size_t headers_find_literal(ant_value_t hdrs, const char *lower_name, const char **first_value) {
  hdr_list_t *l = get_list(hdrs);
  size_t count = 0;

  if (first_value) *first_value = NULL;
  if (!l || !lower_name) return 0;

  for (hdr_entry_t *e = l->head; e; e = e->next) {
    if (strcmp(e->name, lower_name) != 0) continue;
    if (count == 0 && first_value) *first_value = e->value;
    count++;
  }

  return count;
}

void headers_set_immutable(ant_value_t hdrs, bool immutable) {
  if (!immutable && js_get_slot(hdrs, SLOT_HEADERS_GUARD) != js_true) return;
  js_set_slot(hdrs, SLOT_HEADERS_GUARD, js_bool(immutable));
}

void headers_append_if_missing(ant_value_t hdrs, const char *name, const char *value) {
  hdr_list_t *l = get_list(hdrs);
  headers_data_append_if_missing(l, name, value);
}

void headers_data_append_if_missing(headers_data_t *data, const char *name, const char *value) {
  if (!data || !name || !value) return;
  size_t name_len = strlen(name);
  for (hdr_entry_t *e = data->head; e; e = e->next) {
    if (ascii_case_equal(e->name, name)) return;
  }
  (void)list_append_parts(
    data,
    name, name_len,
    value, strlen(value),
    true);
}

void headers_for_each(ant_value_t hdrs, headers_foreach_cb cb, void *ctx) {
  hdr_list_t *l = get_list(hdrs);
  if (!l || !cb) return;
  for (hdr_entry_t *e = l->head; e; e = e->next) cb(e->name, e->value, ctx);
}

bool headers_set_literal(ant_t *js, ant_value_t hdrs, const char *name, const char *value) {
  hdr_list_t *l = get_list(hdrs);
  char *norm = NULL;
  char *lower = NULL;

  if (!l || !name || !value) return false;
  if (!is_valid_name(name)) return false;

  norm = normalize_value(value);
  if (!norm) return false;
  if (!is_valid_value(norm)) {
    free(norm);
    return false;
  }

  lower = lowercase_dup(name);
  if (!lower) {
    free(norm);
    return false;
  }

  if (headers_is_immutable(hdrs)) {
    free(lower);
    free(norm);
    return false;
  }

  list_delete_name(l, lower);
  list_append_raw(l, lower, norm);
  free(lower);
  free(norm);
  
  return true;
}

ant_value_t headers_data_init_from(ant_t *js, headers_data_t *data, ant_value_t init) {
  uint8_t ht = vtype(init);

  if (!data) return js_mkerr(js, "Invalid Headers object");
  if (ht == T_UNDEF) return js_mkundef();

  if (headers_is_headers(init)) {
    hdr_list_t *src = get_list(init);
    if (src) for (hdr_entry_t *e = src->head; e; e = e->next)
      list_append_raw(data, e->name, e->value);
    return js_mkundef();
  }

  if (ht == T_ARR) {
    ant_offset_t len = js_arr_len(js, init);
    for (ant_offset_t i = 0; i < len; i++) {
      ant_value_t pair = js_arr_get(js, init, i);
      ant_value_t r = 0;
      if (js_arr_len(js, pair) < 2) continue;
      r = headers_append_pair(js, data, js_arr_get(js, pair, 0), js_arr_get(js, pair, 1));
      if (is_err(r)) return r;
    }
    return js_mkundef();
  }

  if (ht == T_OBJ) {
    ant_object_t *obj = js_obj_ptr(init);
    if (obj && !obj->flags.is_exotic && obj->shape && obj->prop_count == 1) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(obj->shape, 0);
      if (prop && prop->type == ANT_SHAPE_KEY_STRING &&
          !prop->has_getter && !prop->has_setter &&
          prop->key.interned == g_content_type_interned) {
        ant_value_t value = ant_object_prop_get_unchecked(obj, 0);
        if (vtype(value) == T_STR) {
          size_t value_len = 0;
          const char *value_str = js_getstr(js, value, &value_len);
          if (str_is_ascii(value_str))
            return headers_append_lower_name_value_bytes_n(
              js, data, "content-type", 12, value_str, value_len);
          return headers_append_lower_name_value_js_n(
            js, data, "content-type", 12, value_str, value_len);
        }
      }
    }

    ant_iter_t it = js_prop_iter_begin(js, init);
    const char *key = NULL;
    size_t key_len = 0;
    ant_value_t val = 0;

    while (js_prop_iter_next(&it, &key, &key_len, &val)) {
      ant_value_t r = headers_append_record_value(js, data, key, key_len, val);
      if (is_err(r)) {
        js_prop_iter_end(&it);
        return r;
      }
    }

    js_prop_iter_end(&it);
  }

  return js_mkundef();
}

ant_value_t headers_init_from(ant_t *js, ant_value_t hdrs, ant_value_t init) {
  return headers_data_init_from(js, get_list(hdrs), init);
}

ant_value_t headers_create_from_init(ant_t *js, ant_value_t init) {
  ant_value_t new_hdrs = headers_create_empty(js);
  ant_value_t step = 0;

  if (is_err(new_hdrs)) return new_hdrs;
  step = headers_init_from(js, new_hdrs, init);
  if (is_err(step)) return step;
  return new_hdrs;
}

bool headers_init_has_name(ant_t *js, ant_value_t init, const char *name) {
  uint8_t ht = vtype(init);

  if (ht == T_UNDEF) return false;
  if (headers_is_headers(init)) {
    ant_value_t value = headers_get_value(js, init, name);
    return !is_err(value) && vtype(value) != T_NULL;
  }

  if (ht == T_ARR) {
    ant_offset_t len = js_arr_len(js, init);
    for (ant_offset_t i = 0; i < len; i++) {
      ant_value_t pair = js_arr_get(js, init, i);
      ant_value_t key_v = 0;
      const char *key = NULL;
      if (js_arr_len(js, pair) < 1) continue;
      key_v = js_arr_get(js, pair, 0);
      if (vtype(key_v) != T_STR) {
        key_v = js_tostring_val(js, key_v);
        if (is_err(key_v)) continue;
      }
      key = js_getstr(js, key_v, NULL);
      if (key && strcasecmp(key, name) == 0) return true;
    }
    return false;
  }

  if (ht == T_OBJ) {
    ant_iter_t it = js_prop_iter_begin(js, init);
    const char *key = NULL;
    size_t key_len = 0;
    ant_value_t value = 0;
    bool found = false;

    while (js_prop_iter_next(&it, &key, &key_len, &value)) {
      (void)value;
      if (key && strcasecmp(key, name) == 0) {
        found = true;
        break;
      }
    }

    js_prop_iter_end(&it);
    return found;
  }

  return false;
}

ant_value_t headers_get_value(ant_t *js, ant_value_t hdrs, const char *name) {
  hdr_list_t *l = get_list(hdrs);
  
  if (!l) return js_mknull();
  if (!is_valid_name(name)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid header name");

  char *lower = lowercase_dup(name);
  if (!lower) return js_mkerr(js, "out of memory");

  if (strcmp(lower, "set-cookie") == 0) {
    for (hdr_entry_t *e = l->head; e; e = e->next) {
    if (strcmp(e->name, lower) == 0) {
      ant_value_t ret = header_value_to_js(js, e->value, strlen(e->value));
      free(lower);
      return ret;
    }}
    free(lower);
    return js_mknull();
  }

  size_t total = 0;
  int count = 0;
  
  for (hdr_entry_t *e = l->head; e; e = e->next) {
  if (strcmp(e->name, lower) == 0) {
    if (count > 0) total += 2;
    total += strlen(e->value);
    count++;
  }}

  if (count == 0) {
    free(lower);
    return js_mknull();
  }

  char *combined = malloc(total + 1);
  if (!combined) {
    free(lower);
    return js_mkerr(js, "out of memory");
  }

  size_t pos = 0;
  int seen = 0;
  
  for (hdr_entry_t *e = l->head; e; e = e->next) {
  if (strcmp(e->name, lower) == 0) {
    if (seen > 0) { combined[pos++] = ','; combined[pos++] = ' '; }
    size_t vl = strlen(e->value);
    memcpy(combined + pos, e->value, vl);
    pos += vl;
    seen++;
  }}
  
  combined[pos] = '\0';
  free(lower);

  ant_value_t ret = header_value_to_js(js, combined, pos);
  free(combined);
  
  return ret;
}

void init_headers_module(void) {
  ant_t *js     = rt->js;
  ant_value_t g = js_glob(js);

  g_content_type_interned = intern_string("content-type", 12);

  g_headers_iter_proto = js_mkobj(js);
  js_set_proto_init(g_headers_iter_proto, js->sym.iterator_proto);
  js_set(js, g_headers_iter_proto, "next", js_mkfun(headers_iter_next));
  js_set_descriptor(js, g_headers_iter_proto, "next", 4, JS_DESC_W | JS_DESC_E | JS_DESC_C);
  js_set_sym(js, g_headers_iter_proto, get_iterator_sym(), js_mkfun(sym_this_cb));
  js_iter_register_advance(g_headers_iter_proto, advance_headers);

  g_headers_proto = js_mkobj(js);

  js_set(js, g_headers_proto, "append",       js_mkfun(js_headers_append));
  js_set(js, g_headers_proto, "set",          js_mkfun(js_headers_set));
  js_set(js, g_headers_proto, "get",          js_mkfun(js_headers_get));
  js_set(js, g_headers_proto, "has",          js_mkfun(js_headers_has));
  js_set(js, g_headers_proto, "delete",       js_mkfun(js_headers_delete));
  js_set(js, g_headers_proto, "forEach",      js_mkfun(js_headers_for_each));
  js_set(js, g_headers_proto, "keys",         js_mkfun(js_headers_keys));
  js_set(js, g_headers_proto, "values",       js_mkfun(js_headers_values));
  js_set(js, g_headers_proto, "entries",      js_mkfun(js_headers_entries));
  js_set(js, g_headers_proto, "getSetCookie", js_mkfun(js_headers_get_set_cookie));
  
  js_set_sym(js, g_headers_proto, get_iterator_sym(),    js_get(js, g_headers_proto, "entries"));
  js_set_sym(js, g_headers_proto, get_inspect_sym(),     js_mkfun(headers_inspect));
  js_set_sym(js, g_headers_proto, get_toStringTag_sym(), js_mkstr(js, "Headers", 7));

  ant_value_t ctor_obj = js_mkobj(js);
  js_set_slot(ctor_obj, SLOT_CFUNC, js_mkfun(js_headers_ctor));
  js_mkprop_fast(js, ctor_obj, "prototype", 9, g_headers_proto);
  js_mkprop_fast(js, ctor_obj, "name", 4, js_mkstr(js, "Headers", 7));
  js_set_descriptor(js, ctor_obj, "name", 4, 0);
  
  ant_value_t ctor = js_obj_to_func(ctor_obj);
  js_set(js, g_headers_proto, "constructor", ctor);
  js_set_descriptor(js, g_headers_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);

  js_set(js, g, "Headers", ctor);
  js_set_descriptor(js, g, "Headers", 7, JS_DESC_W | JS_DESC_C);
}
