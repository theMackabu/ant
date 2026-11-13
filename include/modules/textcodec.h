#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "types.h"

typedef enum {
#define TD_ENCODING(id, name) TD_ENC_##id,
#include "textcodec/encodings.h"
  TD_ENC_COUNT
} td_encoding_t;

typedef struct td_rs_decoder td_rs_decoder_t;

typedef struct {
  td_encoding_t encoding;
  td_rs_decoder_t *rs;
  uint8_t pending[4];
  int pending_len;
  bool fatal;
  bool ignore_bom;
  bool bom_seen;
} td_state_t;

td_state_t *td_state_new(td_encoding_t enc, bool fatal, bool ignore_bom);

__attribute__((cold)) 
void init_textcodec_module(ant_t *js);
void td_state_free(td_state_t *st);

const char *td_encoding_name(td_encoding_t enc);
ant_value_t td_encoding_from_label(ant_t *js, ant_value_t label, const char *ctor, td_encoding_t *out);

ant_value_t td_decode(ant_t *js, td_state_t *st, const uint8_t *input, size_t input_len, bool stream);
ant_value_t te_encode(ant_t *js, const char *str, size_t str_len);
