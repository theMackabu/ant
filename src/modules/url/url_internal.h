#ifndef ANT_URL_INTERNAL_H
#define ANT_URL_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} url_fmt_buf_t;

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

ant_value_t legacy_url_parse_impl(
  ant_t *js,
  const char *input, size_t input_len,
  bool parse_query, bool slashes_denote_host
);

#endif
