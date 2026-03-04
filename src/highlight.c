#include <string.h>

#include "highlight.h"
#include "highlight/emit.h"

int ant_highlight_stateful(
  const char *input, size_t input_len,
  char *out, size_t out_size,
  highlight_state *state
) {
  hl_outbuf o;
  hl_outbuf_init(&o, out, out_size);

  hl_iter it;
  hl_iter_init(&it, input, input_len, state);

  hl_span span;
  while (hl_iter_next(&it, &span) && !o.overflow)
    hl_outbuf_emit_span(&o, span.cls, input + span.off, span.len);

  *state = hl_iter_state(&it);

  if (o.overflow) {
    o.pos = 0;
    o.overflow = false;
    hl_outbuf_write_escaped(&o, input, input_len);
    if (o.overflow) {
      size_t safe = out_size > 1 ? out_size - 1 : 0;
      if (safe > input_len) safe = input_len;
      memcpy(out, input, safe);
      out[safe] = '\0';
      return (int)safe;
    }
  }

  o.buf[o.pos] = '\0';
  return (int)o.pos;
}

int ant_highlight(const char *input, size_t input_len, char *out, size_t out_size) {
  highlight_state state = HL_STATE_INIT;
  return ant_highlight_stateful(input, input_len, out, out_size, &state);
}

int highlight_js_line_clipped(
  const char *line, size_t line_len,
  size_t max_cols,
  char *out, size_t out_size,
  highlight_state *state
) {
  hl_outbuf o;
  hl_outbuf_init(&o, out, out_size);

  hl_iter it;
  hl_iter_init(&it, line, line_len, state);

  size_t vis_cols = 0;
  hl_span span;

  while (hl_iter_next(&it, &span)) {
    if (vis_cols >= max_cols) continue;
    size_t span_remaining = max_cols - vis_cols;
    size_t emit_len = span.len < span_remaining ? span.len : span_remaining;
    if (!o.overflow) hl_outbuf_emit_span(&o, span.cls, line + span.off, emit_len);
    vis_cols += span.len;
  }

  *state = hl_iter_state(&it);

  if (o.overflow) {
    o.pos = 0;
    size_t emit = line_len < max_cols ? line_len : max_cols;
    hl_outbuf_write_escaped(&o, line, emit);
    if (o.overflow) {
      size_t safe = out_size > 1 ? out_size - 1 : 0;
      if (safe > emit) safe = emit;
      memcpy(out, line, safe);
      out[safe] = '\0';
      return (int)safe;
    }
  }

  o.buf[o.pos] = '\0';
  return (int)o.pos;
}
