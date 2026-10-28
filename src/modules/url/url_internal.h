#ifndef ANT_URL_INTERNAL_H
#define ANT_URL_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <ada_c.h>
#include "types.h"
#include "native_tag.h"
#include "modules/url.h"

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} url_fmt_buf_t;

static constexpr uint32_t URL_NATIVE_TAG = Ant_NativeTag("URLS");
typedef bool (*ada_url_setter_t)(ada_url, const char *, size_t);

ant_value_t url_invalid_url_error(ant_t *js, ant_value_t input, ant_value_t base);
ant_value_t url_missing_args_error(ant_t *js, const char *message);
ant_value_t url_string_value(ant_t *js, ant_value_t value);
ant_value_t make_usp_for_url(ant_t *js, ant_value_t url_obj, const char *query);

static inline bool url_fmt_reserve(url_fmt_buf_t *b, size_t extra) {
  size_t needed = b->len + extra + 1;
  if (needed <= b->cap) return true;

  size_t next = b->cap ? b->cap : 128;
  while (next < needed) next *= 2;

  char *buf = realloc(b->buf, next);
  if (!buf) return false;

  b->buf = buf;
  b->cap = next;
  
  return true;
}

static inline bool url_fmt_append_n(url_fmt_buf_t *b, const char *s, size_t n) {
  if (!s || n == 0) return true;
  if (!url_fmt_reserve(b, n)) return false;
  
  memcpy(b->buf + b->len, s, n);
  b->len += n;
  b->buf[b->len] = '\0';
  
  return true;
}

static inline bool url_fmt_append(url_fmt_buf_t *b, const char *s) {
  return s ? url_fmt_append_n(b, s, strlen(s)) : true;
}

static inline bool url_fmt_append_c(url_fmt_buf_t *b, char c) {
  if (!url_fmt_reserve(b, 1)) return false;
  b->buf[b->len++] = c;
  b->buf[b->len] = '\0';
  return true;
}

int ada_url_to_state(ada_url url, url_state_t *s);

void url_init_search_params(ant_t *js);
void url_finalize(ant_t *js, ant_object_t *obj);
void legacy_url_register(ant_t *js, ant_value_t lib);
void url_sync_usp(ant_t *js, ant_value_t url_obj, const char *query);

bool ada_set_hash_bool(ada_url url, const char *value, size_t len);
bool ada_set_search_bool(ada_url url, const char *value, size_t len);

int parse_url_to_state_n(
  const char *url_str,
  size_t url_len,
  const char *base_str,
  size_t base_len,
  url_state_t *s
);

ant_value_t url_apply_setter(
  ant_t *js,
  url_state_t *state,
  ant_value_t input,
  ada_url_setter_t setter
);


#endif
