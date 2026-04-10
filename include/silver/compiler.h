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

typedef struct {
  const char *name;
  size_t len;
} sv_param_t;

#define SV_PARAM(name_literal) \
  ((sv_param_t){ (name_literal), sizeof(name_literal) - 1 })

sv_func_t *sv_compile(
  ant_t *js, sv_ast_t *program,
  sv_compile_mode_t mode,
  const char *source, ant_offset_t source_len
);

sv_func_t *sv_compile_function(
  ant_t *js, const char *source,
  size_t len, bool is_async, bool is_generator
);

sv_func_t *sv_compile_function_with_params(
  ant_t *js, const sv_param_t *params,
  int param_count, const char *body,
  size_t body_len, bool is_async
);

#endif
