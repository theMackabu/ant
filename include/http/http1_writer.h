#ifndef ANT_HTTP1_WRITER_H
#define ANT_HTTP1_WRITER_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "types.h"

typedef struct {
  char *data;
  size_t len;
  size_t cap;
  bool failed;
} ant_http1_buffer_t;

void ant_http1_buffer_init(ant_http1_buffer_t *buf);
void ant_http1_buffer_free(ant_http1_buffer_t *buf);

__attribute__((format(printf, 2, 3)))
bool ant_http1_buffer_appendf(ant_http1_buffer_t *buf, const char *fmt, ...);
bool ant_http1_buffer_reserve(ant_http1_buffer_t *buf, size_t extra);
bool ant_http1_buffer_append(ant_http1_buffer_t *buf, const void *data, size_t len);
bool ant_http1_buffer_append_cstr(ant_http1_buffer_t *buf, const char *str);
bool ant_http1_buffer_appendfv(ant_http1_buffer_t *buf, const char *fmt, va_list ap);

const char *ant_http1_default_status_text(int status);
char *ant_http1_buffer_take(ant_http1_buffer_t *buf, size_t *len_out);

bool ant_http1_write_basic_response(
  ant_http1_buffer_t *buf,
  int status,
  const char *status_text,
  const char *content_type,
  const uint8_t *body,
  size_t body_len
);

bool ant_http1_write_response_head(
  ant_http1_buffer_t *buf,
  int status,
  const char *status_text,
  ant_value_t headers,
  bool body_is_stream,
  size_t body_size
);

bool ant_http1_write_final_chunk(ant_http1_buffer_t *buf);
bool ant_http1_write_chunk(ant_http1_buffer_t *buf, const uint8_t *chunk, size_t len);

#endif
