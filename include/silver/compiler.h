#ifndef SILVER_COMPILER_H
#define SILVER_COMPILER_H

#include "silver/ast.h"
#include "silver/vm.h"

typedef enum {
  SV_COMPILE_SCRIPT = 0,
  SV_COMPILE_EVAL   = 1,
  SV_COMPILE_MODULE = 2,
  SV_COMPILE_REPL   = 3,
} sv_compile_mode_t;

sv_func_t *sv_compile(
  ant_t *js, sv_ast_t *program,
  sv_compile_mode_t mode,
  const char *source, ant_offset_t source_len
);

sv_func_t *sv_compile_function(
  ant_t *js, const char *source,
  size_t len, bool is_async
);

sv_func_t *sv_compile_function_parts(
  ant_t *js, const char *params,
  size_t params_len, const char *body,
  size_t body_len, bool is_async
);

#endif
