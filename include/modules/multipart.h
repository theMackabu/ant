#ifndef MULTIPART_H
#define MULTIPART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "types.h"

uint8_t *formdata_serialize_multipart(
  ant_t *js, ant_value_t fd,
  size_t *out_size, char **out_boundary
);

ant_value_t formdata_parse_body(
  ant_t *js, const uint8_t *data, size_t size,
  const char *body_type, bool has_body
);

#endif
