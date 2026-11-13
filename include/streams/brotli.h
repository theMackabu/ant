#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "types.h"

static constexpr int BROTLI_MAX_PARAM_ID = 9;
static constexpr size_t BROTLI_CHUNK_SIZE = 32 * 1024;
static constexpr uint32_t ANT_BROTLI_DEFAULT_QUALITY = 6;

typedef struct {
  bool set[BROTLI_MAX_PARAM_ID + 1];
  uint32_t value[BROTLI_MAX_PARAM_ID + 1];
} brotli_params_t;

typedef int (*brotli_stream_chunk_cb)(void *ctx, const uint8_t *chunk, size_t len);
typedef struct brotli_stream_state brotli_stream_state_t;

brotli_stream_state_t *brotli_stream_state_new_params(
  bool decompress, const brotli_params_t *params,
  const uint8_t *dictionary, size_t dictionary_len, int *bad_id
);

void brotli_decoder_tables_init(void);
void brotli_stream_state_destroy(brotli_stream_state_t *st);
brotli_stream_state_t *brotli_stream_state_new(bool decompress);

ant_value_t brotli_stream_transform(
  ant_t *js, brotli_stream_state_t *st,
  ant_value_t ctrl_obj, const uint8_t *input, size_t input_len
);

int brotli_stream_process(
  brotli_stream_state_t *st, const uint8_t *input,
  size_t input_len, brotli_stream_chunk_cb cb, void *ctx
);

int brotli_stream_finish(brotli_stream_state_t *st, brotli_stream_chunk_cb cb, void *ctx);
int brotli_stream_decoder_error(brotli_stream_state_t *st, const char **name);
ant_value_t brotli_stream_flush(ant_t *js, brotli_stream_state_t *st, ant_value_t ctrl_obj);
