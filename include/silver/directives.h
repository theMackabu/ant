#ifndef SILVER_STRICT_H
#define SILVER_STRICT_H

#include <stdbool.h>
#include <stddef.h>

#include "ast.h"
#include "types.h"

bool sv_ast_is_directive(
  ant_t *js, const sv_ast_t *node,
  const char *directive, size_t directive_len
);

bool sv_ast_is_use_strict(ant_t *js, const sv_ast_t *node);

#endif
