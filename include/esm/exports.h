#ifndef ESM_EXPORTS_H
#define ESM_EXPORTS_H

#include "types.h"
#include "silver/ast.h"

void esm_predeclare_exports(ant_t *js, sv_ast_t *program, ant_value_t ns);

#endif
