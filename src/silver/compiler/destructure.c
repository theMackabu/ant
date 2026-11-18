#include "compiler_internal.h"

static void emit_build_object_rest(sv_compiler_t *c, sv_ast_t *pat, int rest_idx, const int *key_locals) {
  emit_op(c, OP_DUP);
  emit_op(c, OP_OBJECT);
  emit_op(c, OP_SWAP);
  
  int named = 0;
  for (int j = 0; j < rest_idx; j++) {
    sv_ast_t *prev = pat->args.items[j];
    if (prev && prev->type == N_PROPERTY && prev->left) named++;
  }
  
  if (named > UINT16_MAX) {
    js_mkerr_typed(c->js, JS_ERR_SYNTAX, "too many properties before the rest element");
    rest_idx = 0;
  }
  
  uint16_t excluded = 0;
  for (int j = 0; j < rest_idx; j++) {
    sv_ast_t *prev = pat->args.items[j];
    if (!prev || prev->type != N_PROPERTY || !prev->left) continue;
    
    if (prev->flags & FN_COMPUTED) emit_get_local(c, key_locals[j]);
    else compile_static_property_key(c, prev->left);
    excluded++;
  }
  
  emit_op(c, OP_COPY_DATA_PROPS);
  emit_u16(c, excluded);
  emit_op(c, OP_POP);
}

static void emit_require_destructurable(sv_compiler_t *c) {
  static const char undef_msg[] = "Cannot destructure 'undefined' as it is undefined.";
  static const char null_msg[] = "Cannot destructure 'null' as it is null.";
  
  int ok = emit_jump(c, OP_JMP_NOT_NULLISH);
  emit_op(c, OP_DUP);
  emit_op(c, OP_IS_UNDEF);
  
  int is_null = emit_jump(c, OP_JMP_FALSE);
  emit_throw_type_error(c, undef_msg, sizeof(undef_msg) - 1);
  
  patch_jump(c, is_null);
  emit_throw_type_error(c, null_msg, sizeof(null_msg) - 1);
  patch_jump(c, ok);
}

bool is_destructure_pattern_node(sv_ast_t *node) {
  return node && (node->type == N_ARRAY_PAT || node->type == N_ARRAY ||
                  node->type == N_OBJECT_PAT || node->type == N_OBJECT);
}

static void compile_destructure_store(sv_compiler_t *c, sv_ast_t *target,
                                      sv_destructure_mode_t mode,
                                      sv_var_kind_t kind) {
  if (!target) return;

  if (mode == DESTRUCTURE_ASSIGN) {
    compile_lhs_set(c, target, false);
    return;
  }

  if (target->type == N_IDENT) {
    if ((kind == SV_VAR_LET || kind == SV_VAR_CONST) && is_repl_top_level(c)) {
      emit_atom_op(c, OP_INIT_GLOBAL_LEX, target->str, target->len);
      return;
    }
    bool is_const = (
      kind == SV_VAR_CONST ||
      kind == SV_VAR_USING ||
      kind == SV_VAR_AWAIT_USING
    );
    int spill = resolve_spill(c, target->str, target->len);
    if (spill >= 0 && (kind == SV_VAR_VAR ? resolve_local(c, target->str, target->len) == -1 : c->spills[spill].depth == c->scope_depth)) {
      emit_spill_define(c, spill);
      return;
    }
    int idx = ensure_local_at_depth(c, target->str, target->len, is_const, c->scope_depth);
    if (kind != SV_VAR_VAR) c->locals[idx].eval_flags |= SV_EVAL_BIND_LEXICAL;
    emit_put_local(c, idx);
    c->locals[idx].is_tdz = false;
    set_local_inferred_type(c, idx, SV_TI_UNKNOWN);
    emit_local_export_writes(c, idx);
    return;
  }

  if (is_destructure_pattern_node(target)) {
    compile_destructure_pattern(c, target, false, true, mode, kind);
    return;
  }

  compile_lhs_set(c, target, false);
}

void compile_destructure_pattern(
  sv_compiler_t *c, sv_ast_t *pat,
  bool keep, bool consume_source,
  sv_destructure_mode_t mode,
  sv_var_kind_t kind
) {
  if (!pat) return;
  if (keep) emit_op(c, OP_DUP);

  if (pat->type == N_ARRAY_PAT || pat->type == N_ARRAY) {
    if (!consume_source && !keep) emit_op(c, OP_DUP);
    emit_op(c, OP_DESTRUCTURE_INIT);
    
    int err_local = add_local(c, "", 0, false, c->scope_depth);
    int try_jump = emit_jump(c, OP_TRY_PUSH);
    c->try_depth++;
    
    for (int i = 0; i < pat->args.count; i++) {
      sv_ast_t *elem = pat->args.items[i];
      if (!elem || elem->type == N_EMPTY) {
        emit_op(c, OP_DESTRUCTURE_NEXT);
        emit_op(c, OP_POP);
        continue;
      }
      
      if (elem->type == N_REST || elem->type == N_SPREAD) {
        emit_op(c, OP_DESTRUCTURE_REST);
        compile_destructure_store(c, elem->right, mode, kind);
        continue;
      }
      
      sv_ast_t *target = elem;
      sv_ast_t *default_val = NULL;
      if (elem->type == N_ASSIGN_PAT ||
          (elem->type == N_ASSIGN && elem->op == TOK_ASSIGN)) {
        target = elem->left;
        default_val = elem->right;
      }
      
      emit_op(c, OP_DESTRUCTURE_NEXT);
      
      if (default_val) {
        emit_op(c, OP_DUP);
        emit_op(c, OP_IS_UNDEF);
        int skip = emit_jump(c, OP_JMP_FALSE);
        emit_op(c, OP_POP);
        compile_expr(c, default_val);
        patch_jump(c, skip);
      }
      
      compile_destructure_store(c, target, mode, kind);
    }

    emit_op(c, OP_TRY_POP);
    emit_op(c, OP_DESTRUCTURE_CLOSE);
    emit(c, 0);
    int end_jump = emit_jump(c, OP_JMP);

    patch_jump(c, try_jump);
    int catch_tag = emit_jump(c, OP_CATCH);
    
    emit_put_local(c, err_local);
    emit_op(c, OP_DESTRUCTURE_CLOSE);
    emit(c, 1);
    emit_get_local(c, err_local);
    emit_op(c, OP_THROW);
    patch_jump(c, catch_tag);
    patch_jump(c, end_jump);
    c->try_depth--;
    
    return;
  } else if (pat->type == N_OBJECT_PAT || pat->type == N_OBJECT) {
    sv_ast_t *last = pat->args.count ? pat->args.items[pat->args.count - 1] : NULL;
    bool has_rest = last && (last->type == N_REST || last->type == N_SPREAD);
    
    bool reads_property = false;
    for (int i = 0; i < pat->args.count && !reads_property; i++)
      reads_property = pat->args.items[i] && pat->args.items[i]->type == N_PROPERTY;
    
    if (!reads_property) emit_require_destructurable(c);
    int key_locals[has_rest ? pat->args.count : 1];
    
    for (int i = 0; i < pat->args.count; i++) {
      sv_ast_t *prop = pat->args.items[i];
      if (!prop) continue;

      if (prop->type == N_REST || prop->type == N_SPREAD) {
        emit_build_object_rest(c, pat, i, key_locals);
        compile_destructure_store(c, prop->right, mode, kind);
        continue;
      }
      if (prop->type != N_PROPERTY) continue;

      sv_ast_t *key = prop->left;
      sv_ast_t *value = prop->right;
      sv_ast_t *default_val = NULL;
      if (value && (value->type == N_ASSIGN_PAT ||
                    (value->type == N_ASSIGN && value->op == TOK_ASSIGN))) {
        default_val = value->right;
        value = value->left;
      }

      emit_op(c, OP_DUP);
      if ((prop->flags & FN_COMPUTED)) {
        compile_expr(c, key);
        if (has_rest) {
          key_locals[i] = add_local(c, "", 0, false, c->scope_depth);
          emit_op(c, OP_TO_PROPKEY);
          emit_op(c, OP_DUP);
          emit_put_local(c, key_locals[i]);
        }
        emit_op(c, OP_GET_ELEM);
      } else if (key->type == N_IDENT) emit_atom_op(c, OP_GET_FIELD, key->str, key->len);
      else {
        compile_expr(c, key);
        emit_op(c, OP_GET_ELEM);
      }

      if (default_val) {
        emit_op(c, OP_DUP);
        emit_op(c, OP_IS_UNDEF);
        int skip = emit_jump(c, OP_JMP_FALSE);
        emit_op(c, OP_POP);
        compile_expr(c, default_val);
        patch_jump(c, skip);
      }

      compile_destructure_store(c, value, mode, kind);
    }
  }

  if (consume_source) emit_op(c, OP_POP);
}

void compile_array_destructure(sv_compiler_t *c, sv_ast_t *pat, bool keep) {
  compile_destructure_pattern(c, pat, keep, true, DESTRUCTURE_ASSIGN, SV_VAR_LET);
}

void compile_object_destructure(sv_compiler_t *c, sv_ast_t *pat, bool keep) {
  compile_destructure_pattern(c, pat, keep, true, DESTRUCTURE_ASSIGN, SV_VAR_LET);
}
