#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "types.h"

ant_value_t sv_ast_export_public(
  ant_t *js,
  const sv_ast_t *program,
  const char *source,
  size_t source_len,
  const char *source_type,
  bool locations
);
