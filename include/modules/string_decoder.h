#pragma once

#include <stdbool.h>
#include "types.h"

__attribute__((cold)) 
ant_value_t string_decoder_library(ant_t *js);
ant_value_t string_decoder_create(ant_t *js, ant_value_t encoding, ant_value_t new_target);

ant_value_t string_decoder_decode_value(
  ant_t *js, ant_value_t decoder, 
  ant_value_t chunk, bool flush
);
