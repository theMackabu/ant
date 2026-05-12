#include <compat.h> // IWYU pragma: keep

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "output.h"

#define ANT_OUTPUT_INITIAL_CAP 65536

static ant_output_stream_t g_stdout_writer = { .stream = NULL };
static ant_output_stream_t g_stderr_writer = { .stream = NULL };

#ifdef _WIN32
void ant_output_init_console(void) {
  DWORD mode = 0;
  bool has_console_output =
    GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) ||
    GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode);
  
  if (has_console_output) SetConsoleOutputCP(CP_UTF8);
  if (GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode)) SetConsoleCP(CP_UTF8);
}
#endif

ant_output_stream_t *ant_output_stream(FILE *stream) {
  ant_output_stream_t *out = (stream == stderr) ? &g_stderr_writer : &g_stdout_writer;
  out->stream = stream;
  return out;
}

void ant_output_stream_begin(ant_output_stream_t *out) {
  if (!out) return;
  out->buffer.len = 0;
  out->buffer.failed = false;
}

bool ant_output_stream_reserve(ant_output_stream_t *out, size_t extra) {
  size_t needed = 0;
  size_t next_cap = 0;
  char *next = NULL;

  if (!out || out->buffer.failed) return false;

  needed = out->buffer.len + extra;
  if (needed <= out->buffer.cap) return true;

  next_cap = out->buffer.cap ? out->buffer.cap * 2 : ANT_OUTPUT_INITIAL_CAP;
  while (next_cap < needed) next_cap *= 2;

  next = realloc(out->buffer.data, next_cap);
  if (!next) {
    out->buffer.failed = true;
    return false;
  }

  out->buffer.data = next;
  out->buffer.cap = next_cap;
  return true;
}

bool ant_output_stream_append(ant_output_stream_t *out, const void *data, size_t len) {
  if (!ant_output_stream_reserve(out, len)) return false;
  if (len > 0) memcpy(out->buffer.data + out->buffer.len, data, len);
  out->buffer.len += len;
  return true;
}

bool ant_output_stream_append_cstr(ant_output_stream_t *out, const char *str) {
  return ant_output_stream_append(out, str, strlen(str));
}

bool ant_output_stream_putc(ant_output_stream_t *out, char ch) {
  return ant_output_stream_append(out, &ch, 1);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"

bool ant_output_stream_appendfv(ant_output_stream_t *out, const char *fmt, va_list ap) {
  char stack[256];
  va_list ap_copy;
  int written = 0;

  if (!out || out->buffer.failed) return false;

  va_copy(ap_copy, ap);
  written = vsnprintf(stack, sizeof(stack), fmt, ap_copy);
  va_end(ap_copy);

  if (written < 0) {
    out->buffer.failed = true;
    return false;
  }

  if ((size_t)written < sizeof(stack))
    return ant_output_stream_append(out, stack, (size_t)written);

  if (!ant_output_stream_reserve(out, (size_t)written + 1)) return false;
  vsnprintf(out->buffer.data + out->buffer.len, out->buffer.cap - out->buffer.len, fmt, ap);
  out->buffer.len += (size_t)written;
  
  return true;
}

bool ant_output_stream_appendf(ant_output_stream_t *out, const char *fmt, ...) {
  va_list ap;
  bool ok = false;

  va_start(ap, fmt);
  ok = ant_output_stream_appendfv(out, fmt, ap);
  va_end(ap);
  return ok;
}

#pragma GCC diagnostic pop

bool ant_output_stream_flush(ant_output_stream_t *out) {
  size_t len = 0;
  size_t wrote = 0;

  if (!out || !out->stream) return false;
  if (out->buffer.failed) {
    out->buffer.len = 0;
    out->buffer.failed = false;
    return false;
  }
  if (out->buffer.len == 0) return fflush(out->stream) == 0;

  len = out->buffer.len;
  wrote = fwrite(out->buffer.data, 1, len, out->stream);
  out->buffer.len = 0;
  
  return wrote == len && fflush(out->stream) == 0;
}
