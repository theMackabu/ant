#ifndef ANT_ERROR_RENDER_H
#define ANT_ERROR_RENDER_H

#include "errors.h"

static constexpr ant_offset_t ERROR_CONTEXT_MAX_SOURCE_BYTES = 256 * 1024;

typedef struct { char *buf; size_t size; } errbuf_t;

typedef enum {
  ERROR_FRAME_PRETTY,
  ERROR_FRAME_RAW,
  ERROR_FRAME_PLAIN,
} error_frame_style_t;

typedef struct {
  const char *src;
  ant_offset_t src_len;
  ant_offset_t src_pos;
  ant_offset_t src_span_len;
  int error_col;
  int error_span_cols;
  int line;
  int col;
  char error_line[256];
} js_error_render_site_t;

bool errbuf_init(errbuf_t *eb, size_t size);
ant_value_t errbuf_take_string(ant_t *js, errbuf_t *eb, size_t len);

__attribute__((format(printf, 3, 4)))
size_t errbuf_appendf(errbuf_t *eb, size_t used, const char *fmt, ...);

void error_line_col(const char *code, ant_offset_t code_len, ant_offset_t pos, int *out_line, int *out_col);
size_t error_render_value(errbuf_t *eb, ant_t *js, size_t used, ant_value_t value);

bool error_render_frame(
  errbuf_t *eb, size_t *n, const char *name, const char *file,
  int line, int col, error_frame_style_t style
);

void error_render_site_init(const js_error_site_t *es, js_error_render_site_t *site);
void error_render_site_finish(js_error_render_site_t *site);
void error_render_site_prefix(errbuf_t *eb, size_t *n, const char *file, const js_error_render_site_t *site);
const char *error_site_file(ant_t *js, const js_error_site_t *es);

#endif
