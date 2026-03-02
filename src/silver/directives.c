#include <string.h>
#include "silver/ast.h"

bool sv_ast_is_directive(
  ant_t *js, const sv_ast_t *node,
  const char *directive, size_t directive_len
) {
  if (!node || node->type != N_STRING) return false;

  if (node->str && node->len == directive_len + 2 
    && ((node->str[0] == '\'' && node->str[node->len - 1] == '\'') 
    || (node->str[0] == '"' && node->str[node->len - 1] == '"')) 
    && memcmp(node->str + 1, directive, directive_len) == 0
  ) return true;

  return (
    node->str && node->len == directive_len 
    && memcmp(node->str, directive, directive_len) == 0
  );
}

bool sv_ast_is_use_strict(ant_t *js, const sv_ast_t *node) {
  return sv_ast_is_directive(js, node, "use strict", 10);
}
