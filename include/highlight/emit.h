#ifndef HIGHLIGHT_EMIT_H
#define HIGHLIGHT_EMIT_H

#include <stdbool.h>
#include <stddef.h>

#include "highlight.h"

typedef struct {
  char  *buf;
  size_t size;
  size_t pos;
  bool   overflow;
} hl_outbuf;

void hl_outbuf_init(hl_outbuf *o, char *buf, size_t size);
void hl_outbuf_write_escaped(hl_outbuf *o, const char *s, size_t n);
void hl_outbuf_emit_span(hl_outbuf *o, hl_token_class cls, const char *s, size_t n);

#endif
