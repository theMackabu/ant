#include <stdlib.h>
#include <string.h>

#include <brotli/encode.h>
#include <brotli/decode.h>

#include "ant.h"
#include "errors.h"
#include "internal.h"
#include "modules/buffer.h"
#include "streams/brotli.h"
#include "streams/transform.h"

#define BROTLI_CHUNK_SIZE 16384

struct brotli_stream_state {
  bool decompress;
  union {
    BrotliEncoderState *enc;
    BrotliDecoderState *dec;
  } u;
};

static ant_value_t brotli_enqueue_buffer(
  ant_t *js, ant_value_t ctrl_obj, const uint8_t *data, size_t len
) {
  ArrayBufferData *ab = create_array_buffer_data(len);
  if (!ab) return js_mkerr(js, "out of memory");
  memcpy(ab->data, data, len);
  ant_value_t arr = create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, len, "Uint8Array");
  if (!ts_is_controller(ctrl_obj))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TransformStreamDefaultController");
  return ts_ctrl_enqueue(js, ctrl_obj, arr);
}

static ant_value_t brotli_encoder_step(
  ant_t *js, brotli_stream_state_t *st, ant_value_t ctrl_obj,
  const uint8_t *input, size_t input_len, BrotliEncoderOperation op
) {
  size_t avail_in = input_len;
  const uint8_t *next_in = input;

  for (;;) {
    uint8_t out_buf[BROTLI_CHUNK_SIZE];
    uint8_t *next_out = out_buf;
    size_t avail_out = sizeof(out_buf);

    if (!BrotliEncoderCompressStream(
      st->u.enc, op, &avail_in, &next_in, &avail_out, &next_out, NULL)) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "Compression failed");
    }

    size_t have = sizeof(out_buf) - avail_out;
    if (have > 0) {
      ant_value_t r = brotli_enqueue_buffer(js, ctrl_obj, out_buf, have);
      if (is_err(r)) return r;
    }

    if (op == BROTLI_OPERATION_FINISH) {
      if (BrotliEncoderIsFinished(st->u.enc) &&
          !BrotliEncoderHasMoreOutput(st->u.enc))
        break;
    } else if (avail_in == 0 && !BrotliEncoderHasMoreOutput(st->u.enc)) break;
  }

  return js_mkundef();
}

static ant_value_t brotli_decoder_step(
  ant_t *js, brotli_stream_state_t *st, ant_value_t ctrl_obj,
  const uint8_t *input, size_t input_len
) {
  size_t avail_in = input_len;
  const uint8_t *next_in = input;

  for (;;) {
    uint8_t out_buf[BROTLI_CHUNK_SIZE];
    uint8_t *next_out = out_buf;
    size_t avail_out = sizeof(out_buf);

    BrotliDecoderResult ret = BrotliDecoderDecompressStream(
      st->u.dec, &avail_in, &next_in, &avail_out, &next_out, NULL);

    size_t have = sizeof(out_buf) - avail_out;
    if (have > 0) {
      ant_value_t r = brotli_enqueue_buffer(js, ctrl_obj, out_buf, have);
      if (is_err(r)) return r;
    }

    if (ret == BROTLI_DECODER_RESULT_ERROR)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Decompression failed");
    if (ret == BROTLI_DECODER_RESULT_SUCCESS) break;
    if (ret == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT ||
        BrotliDecoderHasMoreOutput(st->u.dec)) continue;
    if (ret == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) break;
  }

  return js_mkundef();
}

brotli_stream_state_t *brotli_stream_state_new(bool decompress) {
  brotli_stream_state_t *st = calloc(1, sizeof(*st));
  if (!st) return NULL;

  st->decompress = decompress;
  if (decompress) {
  st->u.dec = BrotliDecoderCreateInstance(NULL, NULL, NULL);
  if (!st->u.dec) {
    free(st);
    return NULL;
  }} else {
  st->u.enc = BrotliEncoderCreateInstance(NULL, NULL, NULL);
  if (!st->u.enc) {
    free(st);
    return NULL;
  }}

  return st;
}

void brotli_stream_state_destroy(brotli_stream_state_t *st) {
  if (!st) return;
  if (st->decompress) BrotliDecoderDestroyInstance(st->u.dec);
  else BrotliEncoderDestroyInstance(st->u.enc);
  free(st);
}

ant_value_t brotli_stream_transform(
  ant_t *js, brotli_stream_state_t *st,
  ant_value_t ctrl_obj, const uint8_t *input, size_t input_len
) {
  if (st->decompress)
    return brotli_decoder_step(js, st, ctrl_obj, input, input_len);
  return brotli_encoder_step(js, st, ctrl_obj, input, input_len, BROTLI_OPERATION_PROCESS);
}

ant_value_t brotli_stream_flush(
  ant_t *js, brotli_stream_state_t *st,
  ant_value_t ctrl_obj
) {
  if (st->decompress) {
    ant_value_t result = brotli_decoder_step(js, st, ctrl_obj, NULL, 0);
    if (is_err(result)) return result;
    if (!BrotliDecoderIsFinished(st->u.dec))
      return js_mkerr_typed(js, JS_ERR_TYPE, "Decompression failed");
    return js_mkundef();
  }

  return brotli_encoder_step(js, st, ctrl_obj, NULL, 0, BROTLI_OPERATION_FINISH);
}
