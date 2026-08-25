#ifndef ANT_BUILDER_H
#define ANT_BUILDER_H

#include "types.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  ant_t *js;
  char *buf;
  size_t len;
  size_t n;
  bool growable;
  bool inline_mode;
  bool first;
  bool closed;
  bool did_indent;
} js_inspect_builder_t;

static inline char *fixed_buf_write_ptr(char *buf, size_t len, size_t n, size_t *avail) {
  if (!buf || len == 0) {
    if (avail) *avail = 0;
    return NULL;
  }

  size_t write_index = n < len ? n : len - 1;
  if (avail) *avail = len - write_index;
  return buf + write_index;
}

void js_inspect_builder_init_fixed(js_inspect_builder_t *builder, ant_t *js, char *buf, size_t len, size_t initial_n);
bool js_inspect_builder_init_dynamic(js_inspect_builder_t *builder, ant_t *js, size_t initial_cap);
void js_inspect_builder_dispose(js_inspect_builder_t *builder);
bool js_inspect_tagged_header(js_inspect_builder_t *builder, const char *tag, size_t tag_len);
bool js_inspect_object_body(js_inspect_builder_t *builder, ant_value_t obj);
bool js_inspect_close(js_inspect_builder_t *builder);

__attribute__((format(printf, 2, 3)))
bool js_inspect_header(js_inspect_builder_t *builder, const char *fmt, ...);

__attribute__((format(printf, 3, 4)))
bool js_inspect_header_for(js_inspect_builder_t *builder, ant_value_t obj, const char *fmt, ...);

ant_value_t js_inspect_builder_result(js_inspect_builder_t *builder);
bool js_inspect_plain_header(js_inspect_builder_t *builder, ant_value_t obj);

#endif
