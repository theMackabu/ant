#ifndef SILVER_AST_EXPORT_H
#define SILVER_AST_EXPORT_H

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

#endif
