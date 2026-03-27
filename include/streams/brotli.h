#ifndef STREAMS_BROTLI_H
#define STREAMS_BROTLI_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "types.h"

typedef int (*brotli_stream_chunk_cb)(
  void *ctx,
  const uint8_t *chunk,
  size_t len
);

typedef struct brotli_stream_state brotli_stream_state_t;
brotli_stream_state_t *brotli_stream_state_new(bool decompress);

void brotli_stream_state_destroy(brotli_stream_state_t *st);
bool brotli_stream_is_finished(brotli_stream_state_t *st);

ant_value_t brotli_stream_transform(
  ant_t *js, brotli_stream_state_t *st,
  ant_value_t ctrl_obj, const uint8_t *input, size_t input_len
);

ant_value_t brotli_stream_flush(
  ant_t *js, brotli_stream_state_t *st,
  ant_value_t ctrl_obj
);

int brotli_stream_process(
  brotli_stream_state_t *st,
  const uint8_t *input,
  size_t input_len,
  brotli_stream_chunk_cb cb,
  void *ctx
);

int brotli_stream_finish(
  brotli_stream_state_t *st,
  brotli_stream_chunk_cb cb,
  void *ctx
);

#endif
