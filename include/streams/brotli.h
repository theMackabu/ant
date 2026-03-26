#ifndef STREAMS_BROTLI_H
#define STREAMS_BROTLI_H

#include <stddef.h>
#include <stdbool.h>
#include "types.h"

typedef struct brotli_stream_state brotli_stream_state_t;

brotli_stream_state_t *brotli_stream_state_new(bool decompress);
void brotli_stream_state_destroy(brotli_stream_state_t *st);

ant_value_t brotli_stream_transform(
  ant_t *js, brotli_stream_state_t *st,
  ant_value_t ctrl_obj, const uint8_t *input, size_t input_len
);

ant_value_t brotli_stream_flush(
  ant_t *js, brotli_stream_state_t *st,
  ant_value_t ctrl_obj
);

#endif
