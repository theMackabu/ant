#ifndef SILVER_COMPILER_H
#define SILVER_COMPILER_H

#include "silver/ast.h"
#include "silver/vm.h"
#include "silver/engine.h"

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

typedef struct {
  const char *name;
  uint32_t name_len;
  uint32_t name_hash;
  int lookup_next;
  int depth;
  bool is_const;
  bool captured;
  bool is_tdz;
  uint8_t inferred_type;
} sv_local_t;

typedef struct {
  int *offsets;
  int count;
  int cap;
} sv_patch_list_t;

typedef struct {
  const char *name;
  uint32_t len;
} sv_deferred_export_t;

typedef struct {
  int loop_start;
  sv_patch_list_t breaks;
  sv_patch_list_t continues;
  int scope_depth;
  const char *label;
  uint32_t label_len;
  bool is_switch;
} sv_loop_t;

typedef struct const_dedup_entry {
  const char *str;
  size_t len;
  int index;
  UT_hash_handle hh;
} const_dedup_entry_t;

typedef struct sv_line_table {
  uint32_t *offsets;
  int count;
} sv_line_table_t;

typedef struct sv_compiler {
  ant_t *js;
  const char *filename;
  const char *source;
  ant_offset_t source_len;

  uint8_t *code;
  int code_len;
  int code_cap;

  ant_value_t *constants;
  int const_count;
  int const_cap;

  sv_atom_t *atoms;
  int atom_count;
  int atom_cap;
  int ic_count;

  sv_local_t *locals;
  int local_count;
  int local_cap;
  int *local_lookup_heads;
  int local_lookup_cap;
  int max_local_count;
  int scope_depth;
  int param_locals;

  sv_upval_desc_t *upval_descs;
  int upvalue_count;
  int upvalue_cap;

  sv_loop_t *loops;
  int loop_count;
  int loop_cap;

  struct sv_compiler *enclosing;
  sv_ast_t **field_inits;

  int field_init_count;
  int *computed_key_locals;
  int param_count;

  bool is_arrow;
  bool is_async;
  bool is_strict;
  sv_compile_mode_t mode;

  bool is_tla;
  int try_depth;
  int with_depth;
  int strict_args_local;
  int new_target_local;
  int super_local;

  const char *pending_label;
  uint32_t pending_label_len;

  const char *inferred_name;
  uint32_t inferred_name_len;

  sv_srcpos_t *srcpos;
  int srcpos_count;
  int srcpos_cap;
  uint32_t last_srcpos_off;
  uint32_t last_srcpos_end;

  sv_type_info_t *slot_types;
  int slot_type_cap;

  sv_deferred_export_t *deferred_exports;
  int deferred_export_count;
  int deferred_export_cap;

  const_dedup_entry_t *const_dedup;
  sv_line_table_t *line_table;
} sv_compiler_t;


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

sv_func_t *compile_function_body(
  sv_compiler_t *enclosing,
  sv_ast_t *node,
  sv_compile_mode_t mode
);

void compile_array(sv_compiler_t *c, sv_ast_t *node);
void compile_array_destructure(sv_compiler_t *c, sv_ast_t *pat, bool keep);
void compile_assign(sv_compiler_t *c, sv_ast_t *node);
void compile_binary(sv_compiler_t *c, sv_ast_t *node);
void compile_break(sv_compiler_t *c, sv_ast_t *node);
void compile_call(sv_compiler_t *c, sv_ast_t *node);
void compile_class(sv_compiler_t *c, sv_ast_t *node);
void compile_continue(sv_compiler_t *c, sv_ast_t *node);
void compile_delete(sv_compiler_t *c, sv_ast_t *node);
void compile_destructure_binding(sv_compiler_t *c, sv_ast_t *pat, sv_var_kind_t kind);
void compile_do_while(sv_compiler_t *c, sv_ast_t *node);
void compile_export_decl(sv_compiler_t *c, sv_ast_t *node);
void compile_expr(sv_compiler_t *c, sv_ast_t *node);
void compile_for_in(sv_compiler_t *c, sv_ast_t *node);
void compile_for_of(sv_compiler_t *c, sv_ast_t *node);
void compile_for(sv_compiler_t *c, sv_ast_t *node);
void compile_func_expr(sv_compiler_t *c, sv_ast_t *node);
void compile_if(sv_compiler_t *c, sv_ast_t *node);
void compile_import_decl(sv_compiler_t *c, sv_ast_t *node);
void compile_label(sv_compiler_t *c, sv_ast_t *node);
void compile_lhs_set(sv_compiler_t *c, sv_ast_t *target, bool keep);
void compile_member(sv_compiler_t *c, sv_ast_t *node);
void compile_new(sv_compiler_t *c, sv_ast_t *node);
void compile_object_destructure(sv_compiler_t *c, sv_ast_t *pat, bool keep);
void compile_object(sv_compiler_t *c, sv_ast_t *node);
void compile_optional_get(sv_compiler_t *c, sv_ast_t *node);
void compile_optional(sv_compiler_t *c, sv_ast_t *node);
void compile_stmt(sv_compiler_t *c, sv_ast_t *node);
void compile_stmts(sv_compiler_t *c, sv_ast_list_t *list);
void compile_switch(sv_compiler_t *c, sv_ast_t *node);
void compile_tail_return_expr(sv_compiler_t *c, sv_ast_t *expr);
void compile_template(sv_compiler_t *c, sv_ast_t *node);
void compile_ternary(sv_compiler_t *c, sv_ast_t *node);
void compile_try(sv_compiler_t *c, sv_ast_t *node);
void compile_typeof(sv_compiler_t *c, sv_ast_t *node);
void compile_unary(sv_compiler_t *c, sv_ast_t *node);
void compile_update(sv_compiler_t *c, sv_ast_t *node);
void compile_var_decl(sv_compiler_t *c, sv_ast_t *node);
void compile_while(sv_compiler_t *c, sv_ast_t *node);

#endif
