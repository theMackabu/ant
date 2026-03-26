#ifndef TEXTCODEC_H
#define TEXTCODEC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "types.h"

typedef enum {
  TD_ENC_UTF8 = 0,
  TD_ENC_UTF16LE,
  TD_ENC_UTF16BE,
  TD_ENC_WINDOWS_1252,
  TD_ENC_ISO_8859_2,
} td_encoding_t;

typedef struct {
  td_encoding_t encoding;
  uint8_t pending[4];
  int pending_len;
  bool fatal;
  bool ignore_bom;
  bool bom_seen;
} td_state_t;

void init_textcodec_module(void);
td_state_t *td_state_new(td_encoding_t enc, bool fatal, bool ignore_bom);

ant_value_t td_decode(ant_t *js, td_state_t *st, const uint8_t *input, size_t input_len, bool stream);
ant_value_t te_encode(ant_t *js, const char *str, size_t str_len);


#endif
