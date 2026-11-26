#pragma once

#include "scope.h"

void mark_export_pattern(sv_compiler_t *c, sv_ast_t *pat);
void mark_import_binding(sv_compiler_t *c, int idx, sv_ast_t *spec);
void mark_export_binding(sv_compiler_t *c, const char *name, uint32_t len);

void check_switch_declarations(sv_compiler_t *c, sv_ast_t *node);
void check_catch_declarations(sv_compiler_t *c, sv_ast_t *param, sv_ast_t *body);
void check_for_head_declarations(sv_compiler_t *c, sv_ast_t *head, sv_ast_t *body);

void check_statement_list_declarations(
  sv_compiler_t *c, sv_ast_list_t *stmts, bool functions_lexical,
  const sv_decl_names_t *params, bool function_body
);

bool check_function_param_early_errors(
  ant_t *js, sv_ast_t *fn,
  bool inherited_strict,
  bool *out_has_own_use_strict,
  bool *out_has_non_simple_params,
  bool emit_error
);

void hoist_var_decls(sv_compiler_t *c, sv_ast_t *node);
void hoist_func_decls(sv_compiler_t *c, sv_ast_list_t *stmts);
void hoist_lexical_decls(sv_compiler_t *c, sv_ast_list_t *stmts);
void hoist_lexical_pattern(sv_compiler_t *c, sv_ast_t *pat, bool is_const);
void hoist_one_func(sv_compiler_t *c, sv_ast_t *node, bool annex_b_update_var);

void compile_export_emit(sv_compiler_t *c, const char *name, uint32_t len);
void compile_module_static_declaration_prologue(sv_compiler_t *c, sv_ast_list_t *list);

void emit_closure(sv_compiler_t *c, int index);
bool module_stmt_is_static_declaration(sv_ast_t *node);
int split_param_binding(sv_compiler_t *c, int param_local);
void collect_pattern_names(sv_ast_t *pat, sv_decl_names_t *out);
void annex_b_collect_funcs(ant_t *js, sv_ast_t *node, sv_ast_list_t *out);
bool should_emit_module_static_declarations(sv_compiler_t *c, sv_ast_t *body);

static inline void decl_names_init(sv_decl_names_t *names) {
  names->items = names->inline_items;
  names->count = 0;
  names->cap = 16;
}

static inline void decl_names_free(sv_decl_names_t *names) {
  if (names->items != names->inline_items) free(names->items);
}
