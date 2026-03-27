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

#define BROTLI_CHUNK_SIZE (1024 * 32)

struct brotli_stream_state {
  bool decompress;
  union {
    BrotliEncoderState *enc;
    BrotliDecoderState *dec;
  } u;
};

static int brotli_emit_chunk(
  brotli_stream_chunk_cb cb, void *ctx,
  const uint8_t *data, size_t len
) {
  if (!cb || len == 0) return 0;
  return cb(ctx, data, len);
}

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

static int brotli_encoder_step(
  brotli_stream_state_t *st,
  const uint8_t *input,
  size_t input_len,
  BrotliEncoderOperation op,
  brotli_stream_chunk_cb cb,
  void *ctx
) {
  size_t avail_in = input_len;
  const uint8_t *next_in = input;

  for (;;) {
    uint8_t out_buf[BROTLI_CHUNK_SIZE];
    uint8_t *next_out = out_buf;
    size_t avail_out = sizeof(out_buf);

    if (!BrotliEncoderCompressStream(
      st->u.enc, op, &avail_in, &next_in, &avail_out, &next_out, NULL)) {
      return -1;
    }

    size_t have = sizeof(out_buf) - avail_out;
    if (brotli_emit_chunk(cb, ctx, out_buf, have) != 0) return -1;

    if (op == BROTLI_OPERATION_FINISH) {
      if (BrotliEncoderIsFinished(st->u.enc) &&
          !BrotliEncoderHasMoreOutput(st->u.enc))
        break;
    } else if (avail_in == 0 && !BrotliEncoderHasMoreOutput(st->u.enc)) break;
  }

  return 0;
}

static int brotli_decoder_step(
  brotli_stream_state_t *st,
  const uint8_t *input,
  size_t input_len,
  brotli_stream_chunk_cb cb,
  void *ctx
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
    if (brotli_emit_chunk(cb, ctx, out_buf, have) != 0) return -1;

    if (ret == BROTLI_DECODER_RESULT_ERROR) return -1;
    if (ret == BROTLI_DECODER_RESULT_SUCCESS) break;
    if (ret == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT ||
        BrotliDecoderHasMoreOutput(st->u.dec)) continue;
    if (ret == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) break;
  }

  return 0;
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

int brotli_stream_process(
  brotli_stream_state_t *st,
  const uint8_t *input,
  size_t input_len,
  brotli_stream_chunk_cb cb,
  void *ctx
) {
  if (!st) return -1;
  if (st->decompress) return brotli_decoder_step(st, input, input_len, cb, ctx);
  return brotli_encoder_step(st, input, input_len, BROTLI_OPERATION_PROCESS, cb, ctx);
}

int brotli_stream_finish(
  brotli_stream_state_t *st,
  brotli_stream_chunk_cb cb,
  void *ctx
) {
  if (!st) return -1;

  if (st->decompress) {
    if (brotli_decoder_step(st, NULL, 0, cb, ctx) != 0) return -1;
    return BrotliDecoderIsFinished(st->u.dec) ? 0 : -1;
  }

  return brotli_encoder_step(st, NULL, 0, BROTLI_OPERATION_FINISH, cb, ctx);
}

bool brotli_stream_is_finished(brotli_stream_state_t *st) {
  if (!st) return false;
  if (st->decompress) return BrotliDecoderIsFinished(st->u.dec);
  return BrotliEncoderIsFinished(st->u.enc);
}

typedef struct {
  ant_t *js;
  ant_value_t ctrl_obj;
  ant_value_t error;
} brotli_js_emit_ctx_t;

static int brotli_enqueue_chunk_cb(void *ctx, const uint8_t *chunk, size_t len) {
  brotli_js_emit_ctx_t *emit = (brotli_js_emit_ctx_t *)ctx;
  ant_value_t result = brotli_enqueue_buffer(emit->js, emit->ctrl_obj, chunk, len);
  if (is_err(result)) {
    emit->error = result;
    return -1;
  }
  return 0;
}

ant_value_t brotli_stream_transform(
  ant_t *js, brotli_stream_state_t *st,
  ant_value_t ctrl_obj, const uint8_t *input, size_t input_len
) {
  brotli_js_emit_ctx_t emit = { .js = js, .ctrl_obj = ctrl_obj, .error = js_mkundef() };
  if (brotli_stream_process(st, input, input_len, brotli_enqueue_chunk_cb, &emit) != 0) {
    if (is_err(emit.error)) return emit.error;
    return js_mkerr_typed(js, JS_ERR_TYPE, st && st->decompress ? "Decompression failed" : "Compression failed");
  }
  return js_mkundef();
}

ant_value_t brotli_stream_flush(
  ant_t *js, brotli_stream_state_t *st,
  ant_value_t ctrl_obj
) {
  brotli_js_emit_ctx_t emit = { .js = js, .ctrl_obj = ctrl_obj, .error = js_mkundef() };
  if (brotli_stream_finish(st, brotli_enqueue_chunk_cb, &emit) != 0) {
    if (is_err(emit.error)) return emit.error;
    return js_mkerr_typed(js, JS_ERR_TYPE, st && st->decompress ? "Decompression failed" : "Compression failed");
  }
  return js_mkundef();
}
