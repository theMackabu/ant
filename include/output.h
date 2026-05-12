#ifndef ANT_OUTPUT_H
#define ANT_OUTPUT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
  char *data;
  size_t len;
  size_t cap;
  bool failed;
} ant_output_buffer_t;

typedef struct {
  FILE *stream;
  ant_output_buffer_t buffer;
} ant_output_stream_t;

ant_output_stream_t *ant_output_stream(FILE *stream);

void ant_output_init_console(void);
void ant_output_stream_begin(ant_output_stream_t *out);

bool ant_output_stream_reserve(ant_output_stream_t *out, size_t extra);
bool ant_output_stream_append(ant_output_stream_t *out, const void *data, size_t len);
bool ant_output_stream_append_cstr(ant_output_stream_t *out, const char *str);
bool ant_output_stream_putc(ant_output_stream_t *out, char ch);
bool ant_output_stream_appendfv(ant_output_stream_t *out, const char *fmt, va_list ap);

__attribute__((format(printf, 2, 3)))
bool ant_output_stream_appendf(ant_output_stream_t *out, const char *fmt, ...);
bool ant_output_stream_flush(ant_output_stream_t *out);

#endif
