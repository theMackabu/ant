#ifndef STRING_DECODER_H
#define STRING_DECODER_H

#include <stdbool.h>
#include "types.h"

ant_value_t string_decoder_library(ant_t *js);
ant_value_t string_decoder_create(ant_t *js, ant_value_t encoding);

ant_value_t string_decoder_decode_value(
  ant_t *js, ant_value_t decoder, 
  ant_value_t chunk, bool flush
);

ant_value_t string_decoder_decode_bytes(
  ant_t *js, ant_value_t decoder,
  const uint8_t *src, size_t len, bool flush
);

#endif
