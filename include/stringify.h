#pragma once

#include "types.h"
#include "list.h"
#include "isolate.h"
#include "modules/blob.h"
#include "modules/buffer.h"

#include <stdbool.h>
#include <stddef.h>

ant_offset_t dense_iterable_length(ant_t *js, ant_value_t obj);

size_t stringify_typedarray(ant_t *js, ant_value_t obj, const TypedArrayData *ta, char *buf, size_t len);
size_t stringify_arraybuffer(const ArrayBufferData *ab, char *buf, size_t len);
size_t stringify_dataview(const DataViewData *dv, char *buf, size_t len);
size_t stringify_blob(const blob_data_t *bd, bool is_file, char *buf, size_t len);

static inline void ant_stringify_enter(ant_t *js, ant_stringify_ctx_t *ctx, int max_depth) {
  ctx->next = NULL;
  ctx->pprev = NULL;
  ctx->depth = 0;
  ctx->indent = 0;
  ctx->max_depth = max_depth;
  ctx->multiref_count = 0;
  ctx->multiref_next_id = 0;
  ctx->flags = 0;
  Ant_List_Insert(&js->stringify, ctx);
}

const char *func_code_text(ant_t *js, ant_value_t code_val);
const char *get_func_name(ant_t *js, ant_value_t func, ant_offset_t *out_len);

bool func_has_code(ant_value_t code_val);
bool js_cfunc_try_get_own(ant_t *js, ant_value_t cfunc, const char *key, size_t key_len, ant_value_t *out);
