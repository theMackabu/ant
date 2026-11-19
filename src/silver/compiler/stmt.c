#include "compiler_internal.h"

static bool is_tail_callable(sv_compiler_t *c, sv_ast_t *node) {
  if (c->try_depth > 0) return false;
  if (c->is_class_ctor) return false;
  if (c->using_cleanup_count > 0) return false;
  if (node->type != N_CALL) return false;
  if (call_has_spread_arg(node)) return false;
  
  sv_ast_t *callee = node->left;
  if (callee->type == N_OPTIONAL || sv_node_has_optional_base(callee)) return false;
  if (callee->type == N_MEMBER && sv_node_has_optional_base(callee->left)) return false;
  if (
    callee->type == N_IDENT && 
    ((callee->len == 5 && memcmp(callee->str, "super", 5) == 0) ||
     (callee->len == 4 && memcmp(callee->str, "eval", 4) == 0))
  ) return false;
  
  return true;
}

void emit_using_dispose_call(
  sv_compiler_t *c,
  int stack_local,
  int completion_local,
  bool is_async,
  bool suppressed_completion
) {
  emit_get_local(c, stack_local);
  if (suppressed_completion) {
    emit_get_local(c, completion_local);
  }
  if (is_async && suppressed_completion) emit_op(c, OP_USING_DISPOSE_ASYNC_SUPPRESSED);
  else if (is_async) emit_op(c, OP_USING_DISPOSE_ASYNC);
  else if (suppressed_completion) emit_op(c, OP_USING_DISPOSE_SUPPRESSED);
  else emit_op(c, OP_USING_DISPOSE);
  if (is_async) emit_op(c, OP_AWAIT);
}

void emit_using_cleanups_to_depth(sv_compiler_t *c, int target_depth) {
  for (int i = c->using_cleanup_count - 1; i >= 0; i--) {
    sv_using_cleanup_t *cleanup = &c->using_cleanups[i];
    if (cleanup->scope_depth <= target_depth) break;
    emit_using_dispose_call(c, cleanup->stack_local, -1, cleanup->is_async, false);
    emit_op(c, OP_POP);
  }
}

typedef struct {
  int locals;
  int usings;
} exit_cursor_t;

static void emit_exit_cleanups(sv_compiler_t *c, exit_cursor_t *cur, int local_floor, int using_floor) {
  int close_local = -1;
  for (int i = cur->locals - 1; i >= local_floor; i--)
    if (c->locals[i].captured) close_local = i;
  if (close_local >= 0)
    emit_slot_op(c, OP_CLOSE_UPVAL, (uint16_t)local_to_frame_slot(c, close_local));
  if (local_floor < cur->locals) cur->locals = local_floor;

  for (int i = cur->usings - 1; i >= using_floor; i--) {
    sv_using_cleanup_t *cleanup = &c->using_cleanups[i];
    emit_using_dispose_call(c, cleanup->stack_local, -1, cleanup->is_async, false);
    emit_op(c, OP_POP);
  }
  if (using_floor < cur->usings) cur->usings = using_floor;
}

static void emit_handler_close(sv_compiler_t *c) {
  if (c->handler_close_floor)
    emit_slot_op(c, OP_CLOSE_UPVAL, (uint16_t)(c->handler_close_floor - 1));
}

static void emit_iter_close_seq(sv_compiler_t *c, bool is_async) {
  if (is_async) {
    emit_op(c, OP_ITER_CLOSE_ASYNC);
    emit_op(c, OP_AWAIT);
    emit_op(c, OP_ITER_CLOSE_CHECK);
  } else {
    emit_op(c, OP_ITER_CLOSE);
    emit(c, 0);
  }
}

static void emit_loop_exit_unwind(
  sv_compiler_t *c, int unwind_depth, int target_loop,
  exit_cursor_t *cur, exit_cursor_t floor
) {
  for (int i = c->unwind_count - 1; i >= unwind_depth; i--) {
    sv_unwind_mark_t mark = c->unwind_marks[i];
    emit_exit_cleanups(
      c, cur,
      mark.local_count > floor.locals ? mark.local_count : floor.locals,
      mark.using_count > floor.usings ? mark.using_count : floor.usings
    );
    if (c->unwind_kinds[i] == UNW_TRY_FINALLY) {
      emit_op(c, OP_UNWIND_JMP);
      emit_i32(c, 0);
      emit(c, 1);
      emit(c, 1);
      continue;
    }
    emit_op(c, c->unwind_kinds[i] == UNW_FINALLY_BODY ? OP_FINALLY_DISCARD : OP_TRY_POP);
    for (int j = c->loop_count - 1; j > target_loop; j--)
      if (c->loops[j].iter_unwind_index == i) emit_iter_close_seq(c, c->loops[j].iter_async);
  }
}

void emit_loop_exit_jump(sv_compiler_t *c, sv_loop_t *loop, sv_patch_list_t *pl) {
  exit_cursor_t cur = { c->local_count, c->using_cleanup_count };
  exit_cursor_t floor = cur;
  while (floor.locals > 0 && c->locals[floor.locals - 1].depth > loop->scope_depth) floor.locals--;
  while (floor.usings > 0 && c->using_cleanups[floor.usings - 1].scope_depth > loop->scope_depth) floor.usings--;

  emit_loop_exit_unwind(c, loop->unwind_depth, (int)(loop - c->loops), &cur, floor);
  emit_exit_cleanups(c, &cur, floor.locals, floor.usings);
  patch_list_add(pl, emit_jump(c, OP_JMP));
}

static bool unwinding_runs_finally(const sv_compiler_t *c) {
  for (int i = 0; i < c->unwind_count; i++)
    if (c->unwind_kinds[i] == UNW_TRY_FINALLY) return true;
  return false;
}

void emit_return_from_stack(sv_compiler_t *c) {
  exit_cursor_t cur = { c->local_count, c->using_cleanup_count };
  int active_unwinds = c->unwind_count;
  int stash_local = -1;
  bool needs_stash = false;

  for (int i = 0; i < c->loop_count; i++) {
    sv_loop_t *loop = &c->loops[i];
    if (loop->iter_unwind_index < 0) continue;
    active_unwinds = loop->iter_unwind_index;
    stash_local = loop->iter_completion_local;
    needs_stash = true;
    break;
  }

  if (c->using_cleanup_count > 0 && c->using_cleanups[0].stash_local >= 0) {
    int first = 0;
    while (first < active_unwinds && c->unwind_marks[first].using_count == 0) first++;
    if (first < active_unwinds) {
      active_unwinds = first;
      stash_local = c->using_cleanups[0].stash_local;
      needs_stash = false;
      for (int i = first; i < c->unwind_count && !needs_stash; i++)
        needs_stash = c->unwind_kinds[i] == UNW_TRY_FINALLY;
      for (int i = 0; i < c->loop_count && !needs_stash; i++)
        needs_stash = c->loops[i].iter_unwind_index >= first;
    }
  }

  if (needs_stash) emit_put_local(c, stash_local);
  emit_loop_exit_unwind(c, active_unwinds, -1, &cur, (exit_cursor_t){ 0, 0 });
  emit_exit_cleanups(c, &cur, cur.locals, 0);
  if (needs_stash) emit_get_local(c, stash_local);

  bool runs_finally = false;
  for (int i = 0; i < active_unwinds && !runs_finally; i++)
    runs_finally = c->unwind_kinds[i] == UNW_TRY_FINALLY;

  if (!runs_finally) emit_close_upvals(c);
  emit_op(c, OP_RETURN);
}

static void compile_tail_call(sv_compiler_t *c, sv_ast_t *node) {
  sv_ast_t *callee = node->left;

  sv_call_kind_t kind = compile_call_setup_non_optional(c, callee);
  int argc = node->args.count;
  for (int i = 0; i < argc; i++)
    compile_expr(c, node->args.items[i]);

  emit_close_upvals(c);
  emit_call_srcpos(c, node);
  emit_op(c, kind == SV_CALL_METHOD ? OP_TAIL_CALL_METHOD : OP_TAIL_CALL);
  emit_u16(c, (uint16_t)argc);
}

static bool can_tail_await(sv_compiler_t *c) {
  if (!c->is_async || c->is_async_generator || c->is_tla) return false;
  if (c->try_depth != 0 || c->using_cleanup_count != 0) return false;
  for (int i = 0; i < c->loop_count; i++)
    if (c->loops[i].iter_unwind_index >= 0) return false;
  return true;
}

void compile_tail_return_expr(sv_compiler_t *c, sv_ast_t *expr) {
  if (expr->type == N_TERNARY) {
    compile_truthy_test_expr(c, expr->cond);
    int else_jump = emit_jump(c, OP_JMP_FALSE);
    compile_tail_return_expr(c, expr->left);
    patch_jump(c, else_jump);
    compile_tail_return_expr(c, expr->right);
    return;
  }

  if (expr->type == N_AWAIT && can_tail_await(c)) {
    compile_expr(c, expr->right);
    emit_close_upvals(c);
    emit_op(c, OP_AWAIT_RETURN);
    return;
  }

  if (expr->type == N_CALL &&
      compile_call_map_template_intrinsic(c, expr, false, true)) {
    return;
  }

  if (is_tail_callable(c, expr)) {
    compile_tail_call(c, expr);
    return;
  }

  compile_expr(c, expr);
  emit_return_from_stack(c);
}

void compile_stmts(sv_compiler_t *c, sv_ast_list_t *list) {
  for (int i = 0; i < list->count; i++) compile_stmt(c, list->items[i]);
}

void compile_body_block(
  sv_compiler_t *c,
  sv_ast_t *body,
  bool skip_static_declarations
) {
  for (int i = 0; i < body->args.count; i++) {
    sv_ast_t *stmt = body->args.items[i];
    if (skip_static_declarations && module_stmt_is_static_declaration(stmt)) continue;
    compile_stmt(c, stmt);
    if (stmt == c->this_bound_stmt) c->this_bound = true;
  }
}

bool stmt_list_has_using_decl(sv_ast_list_t *list, bool *has_await_using) {
  bool found = false;
  for (int i = 0; i < list->count; i++) {
    sv_ast_t *node = list->items[i];
    if (!node) continue;
    
    sv_ast_t *decl = (node->type == N_EXPORT) ? node->left : node;
    if (!decl || decl->type != N_VAR) continue;
    
    if (decl->var_kind == SV_VAR_USING || decl->var_kind == SV_VAR_AWAIT_USING) {
      if (decl->var_kind == SV_VAR_AWAIT_USING && has_await_using) *has_await_using = true;
      found = true;
    }
  }
  
  return found;
}

void emit_empty_disposal_stack(sv_compiler_t *c) {
  emit_op(c, OP_ARRAY);
  emit_u16(c, 0);
}

void emit_using_push(sv_compiler_t *c, bool is_async) {
  emit_op(c, is_async ? OP_USING_PUSH_ASYNC : OP_USING_PUSH);
}

void pop_using_cleanup(sv_compiler_t *c) {
  if (c->using_cleanup_count > 0) c->using_cleanup_count--;
}

void emit_dispose_resource(sv_compiler_t *c, bool is_async) {
  emit_op(c, is_async ? OP_DISPOSE_RESOURCE_ASYNC : OP_DISPOSE_RESOURCE);
  if (is_async) emit_op(c, OP_AWAIT);
}

void push_using_cleanup(
  sv_compiler_t *c,
  int stack_local,
  int stash_local,
  int scope_depth,
  bool is_async
) {
  if (c->using_cleanup_count >= c->using_cleanup_cap) {
    int cap = c->using_cleanup_cap ? c->using_cleanup_cap * 2 : 4;
    c->using_cleanups = realloc(c->using_cleanups, (size_t)cap * sizeof(sv_using_cleanup_t));
    c->using_cleanup_cap = cap;
  }

  c->using_cleanups[c->using_cleanup_count++] = (sv_using_cleanup_t){
    .stack_local = stack_local,
    .stash_local = stash_local,
    .scope_depth = scope_depth,
    .is_async = is_async,
  };
}

static void compile_block_with_using(sv_compiler_t *c, sv_ast_t *node) {
  bool has_await_using = false;
  bool has_using = stmt_list_has_using_decl(&node->args, &has_await_using);

  begin_scope(c);
  check_statement_list_declarations(c, &node->args, true, NULL, false);
  hoist_lexical_decls(c, &node->args);
  hoist_func_decls(c, &node->args);

  if (!has_using) {
    compile_stmts(c, &node->args);
    end_scope(c);
    return;
  }

  emit_empty_disposal_stack(c);
  int stack_local = add_local(c, "", 0, false, c->scope_depth);
  
  emit_put_local(c, stack_local);
  int err_local = add_local(c, "", 0, false, c->scope_depth);

  int old_using_stack = c->using_stack_local;
  bool old_using_async = c->using_stack_async;
  
  c->using_stack_local = stack_local;
  c->using_stack_async = has_await_using;
  push_using_cleanup(c, stack_local, err_local, c->scope_depth, has_await_using);

  c->try_depth++;
  int try_jump = emit_jump(c, OP_TRY_PUSH);
  unwind_push(c, UNW_TRY_CATCH);
  
  compile_stmts(c, &node->args);
  emit_op(c, OP_TRY_POP);
  unwind_pop(c);
  c->try_depth--;

  emit_using_dispose_call(c, stack_local, -1, has_await_using, false);
  emit_op(c, OP_POP);
  int end_jump = emit_jump(c, OP_JMP);

  patch_jump(c, try_jump);
  int catch_tag = emit_jump(c, OP_CATCH);
  
  emit_put_local(c, err_local);
  emit_using_dispose_call(c, stack_local, err_local, has_await_using, true);
  
  if (!has_await_using) emit_op(c, OP_THROW);
  patch_jump(c, catch_tag);
  patch_jump(c, end_jump);

  c->using_stack_local = old_using_stack;
  c->using_stack_async = old_using_async;
  
  pop_using_cleanup(c);
  end_scope(c);
}

void compile_stmt(sv_compiler_t *c, sv_ast_t *node) {
  if (!node || compile_stack_overflow(c)) return;
  emit_srcpos(c, node);

  switch (node->type) {
    case N_EMPTY:
    case N_DEBUGGER:
      break;

    case N_BLOCK:
      compile_block_with_using(c, node);
      break;

    case N_VAR:
      compile_var_decl(c, node);
      break;

    case N_IMPORT_DECL:
      compile_import_decl(c, node);
      break;

    case N_EXPORT:
      compile_export_decl(c, node);
      break;

    case N_IF:
      compile_if(c, node);
      break;

    case N_WHILE:
      compile_while(c, node);
      break;

    case N_DO_WHILE:
      compile_do_while(c, node);
      break;

    case N_FOR:
      compile_for(c, node);
      break;

    case N_FOR_IN:
      compile_for_in(c, node);
      break;

    case N_FOR_OF:
      compile_for_of(c, node);
      break;

    case N_FOR_AWAIT_OF:
      if (c->enclosing && !c->enclosing->enclosing) c->is_tla = true;
      compile_for_of(c, node);
      break;

    case N_RETURN:
      if (derived_return_needs_check(c)) {
        if (node->right) compile_expr(c, node->right);
        else emit_op(c, OP_UNDEF);
        if (c->derived_return_local >= 0 && unwinding_runs_finally(c)) {
          emit_put_local(c, c->derived_return_local);
          emit_break_to_loop(c, 0);
        } else {
          emit_derived_return_check(c);
          emit_return_from_stack(c);
        }
      } else if (node->right) {
        compile_tail_return_expr(c, node->right);
      } else {
        emit_op(c, OP_UNDEF);
        emit_return_from_stack(c);
      }
      break;

    case N_THROW:
      compile_expr(c, node->right);
      emit_op(c, OP_THROW);
      break;

    case N_BREAK:
      compile_break(c, node);
      break;

    case N_CONTINUE:
      compile_continue(c, node);
      break;

    case N_TRY:
      compile_try(c, node);
      break;

    case N_SWITCH:
      compile_switch(c, node);
      break;

    case N_LABEL:
      compile_label(c, node);
      break;

    case N_ASSIGN:
      if (!has_completion_accumulator(c) && compile_self_append_stmt(c, node)) break;
      compile_expr(c, node);
      emit_set_completion_from_stack(c);
      break;

    case N_FUNC:
      if (node->str && !(node->flags & (FN_ARROW | FN_PAREN))) break;
      compile_expr(c, node);
      emit_set_completion_from_stack(c);
      break;

    case N_CLASS:
      compile_class(c, node);
      if (node->flags & FN_PAREN) emit_set_completion_from_stack(c);
      else emit_op(c, OP_POP);
      break;

    case N_WITH: {
      emit_set_completion_undefined(c);
      compile_expr(c, node->left);
      emit_op(c, OP_TO_OBJECT);
      
      begin_scope(c);
      int local = add_hidden_local(c);
      emit_put_local(c, local);
      
      if (c->with_depth >= c->with_cap) {
        c->with_cap = c->with_cap ? c->with_cap * 2 : 4;
        c->with_scopes = realloc(c->with_scopes, (size_t)c->with_cap * sizeof(*c->with_scopes));
      }
      c->with_scopes[c->with_depth++] = (sv_with_scope_t){ .local = local, .depth = c->scope_depth, .block_depth = c->scope_depth };
      
      compile_stmt(c, node->body);
      c->with_depth--;
      c->eval_scope_shareable = false;
      end_scope(c);
      break;
    }

    default:
      if (!has_completion_accumulator(c) &&
          compile_regexp_exec_discard_intrinsic(c, node))
        break;
      compile_expr(c, node);
      emit_set_completion_from_stack(c);
      break;
  }
}

void compile_var_decl(sv_compiler_t *c, sv_ast_t *node) {
  sv_var_kind_t kind = node->var_kind;
  
  bool is_using = (kind == SV_VAR_USING || kind == SV_VAR_AWAIT_USING);
  bool is_await_using = (kind == SV_VAR_AWAIT_USING);
  bool is_const = (kind == SV_VAR_CONST || is_using);
  bool global_lexical = !is_using && kind != SV_VAR_VAR && is_repl_top_level(c);

  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *decl = node->args.items[i];
    if (decl->type != N_VARDECL) continue;
    sv_ast_t *target = decl->left;

    if (global_lexical && target->type == N_IDENT) {
      if (decl->right)
        compile_expr_with_inferred_name(c, decl->right, target->str, target->len);
      else emit_op(c, OP_UNDEF);
      emit_atom_op(c, OP_INIT_GLOBAL_LEX, target->str, target->len);
    } else if (kind == SV_VAR_VAR) {
      if (decl->right) {
        if (target->type == N_IDENT)
          compile_expr_with_inferred_name(c, decl->right, target->str, target->len);
        else compile_expr(c, decl->right);
        if (target->type == N_IDENT) {
          int idx = resolve_local(c, target->str, target->len);
          if (idx >= 0 && c->locals[idx].depth != -1 && !local_with_shadowed(c, target->str, target->len)) {
            int dep_base = c->types->dep_count;
            emit_put_local_typed(c, idx, infer_expr_type(c, decl->right), dep_base);
            c->types->dep_count = dep_base;
          } else compile_lhs_set(c, target, false);
        } else compile_lhs_set(c, target, false);
      }
    } else {
      int spill = target->type == N_IDENT ? resolve_spill(c, target->str, target->len) : -1;
      if (spill >= 0 && c->spills[spill].depth == c->scope_depth) {
        if (decl->right) compile_expr_with_inferred_name(c, decl->right, target->str, target->len);
        else emit_op(c, OP_UNDEF);
        emit_spill_define(c, spill);
      } else if (target->type == N_IDENT) {
        int idx = ensure_local_at_depth(c, target->str, target->len, is_const, c->scope_depth);
        if (!(c->locals[idx].eval_flags & SV_EVAL_BIND_LEXICAL)) c->eval_scope_shareable = false;
        c->locals[idx].eval_flags |= SV_EVAL_BIND_LEXICAL;
        uint8_t init_type = SV_TI_UNKNOWN;
        int dep_base = c->types->dep_count;
        if (decl->right) {
          compile_expr_with_inferred_name(c, decl->right, target->str, target->len);
          init_type = infer_expr_type(c, decl->right);
        } else if (!is_const) {
          emit_op(c, OP_UNDEF);
          init_type = SV_TI_UNDEF;
        }
        if (decl->right || !is_const) {
          emit_put_local_typed(c, idx, init_type, dep_base);
          c->types->dep_count = dep_base;
          c->locals[idx].is_tdz = false;
          emit_local_export_writes(c, idx);
          if (is_using) {
            if (c->using_stack_local >= 0) {
              emit_get_local(c, c->using_stack_local);
              emit_get_local(c, idx);
              emit_using_push(c, is_await_using);
            } else {
              emit_get_local(c, idx);
              emit_dispose_resource(c, is_await_using);
            }
            emit_op(c, OP_POP);
          }
        }
      } else {
        if (decl->right) {
          compile_expr(c, decl->right);
          compile_destructure_binding(c, target, kind);
          emit_op(c, OP_POP);
        }
      }
    }
  }
}

void compile_destructure_binding(sv_compiler_t *c, sv_ast_t *pat, sv_var_kind_t kind) {
  compile_destructure_pattern(c, pat, false, false, DESTRUCTURE_BIND, kind);
}

static bool fold_static_typeof_compare(
  sv_compiler_t *c, sv_ast_t *cond, bool *out_truth
) {
  if (!cond || cond->type != N_BINARY) return false;
  if (!(cond->op == TOK_SEQ || cond->op == TOK_SNE ||
        cond->op == TOK_EQ  || cond->op == TOK_NE))
    return false;

  sv_ast_t *typeof_node = NULL;
  sv_ast_t *str_node = NULL;
  if (cond->left && cond->left->type == N_TYPEOF &&
      cond->right && cond->right->type == N_STRING) {
    typeof_node = cond->left;
    str_node = cond->right;
  } else if (cond->right && cond->right->type == N_TYPEOF &&
             cond->left && cond->left->type == N_STRING) {
    typeof_node = cond->right;
    str_node = cond->left;
  } else return false;

  if (!typeof_node->right || typeof_node->right->type != N_IDENT) return false;
  sv_ast_t *ident = typeof_node->right;
  int local = resolve_local(c, ident->str, ident->len);
  if (local < 0) return false;
  if (c->with_depth > 0 || !c->locals[local].is_const || c->locals[local].type_derived) return false;
  const char *known = typeof_name_for_type(get_local_inferred_type(c, local));
  if (!known) return false;

  bool is_equal = (strlen(known) == str_node->len &&
                   memcmp(known, str_node->str, str_node->len) == 0);
  bool truth = (cond->op == TOK_SEQ || cond->op == TOK_EQ) ? is_equal : !is_equal;
  *out_truth = truth;
  return true;
}

void compile_if(sv_compiler_t *c, sv_ast_t *node) {
  emit_set_completion_undefined(c);

  bool folded_truth = false;
  if (fold_static_typeof_compare(c, node->cond, &folded_truth)) {
    if (folded_truth) compile_stmt(c, node->left);
    else if (node->right) compile_stmt(c, node->right);
    return;
  }

  compile_truthy_test_expr(c, node->cond);
  int else_jump = emit_jump(c, OP_JMP_FALSE);
  compile_stmt_region(c, node->left);
  if (node->right) {
    int end_jump = emit_jump(c, OP_JMP);
    patch_jump(c, else_jump);
    compile_stmt_region(c, node->right);
    patch_jump(c, end_jump);
  } else {
    patch_jump(c, else_jump);
  }
}

void compile_while(sv_compiler_t *c, sv_ast_t *node) {
  emit_set_completion_undefined(c);

  int loop_start = c->code_len;
  push_loop(c, loop_start, NULL, 0, SV_LOOP_ITERATION);

  compile_truthy_test_expr(c, node->cond);
  int exit_jump = emit_jump(c, OP_JMP_FALSE);
  compile_stmt_region(c, node->body);

  sv_loop_t *loop = &c->loops[c->loop_count - 1];
  for (int i = 0; i < loop->continues.count; i++)
    patch_jump(c, loop->continues.offsets[i]);

  emit_loop(c, loop_start);
  patch_jump(c, exit_jump);
  pop_loop(c);
}

static void compile_finally_block(sv_compiler_t *c, sv_ast_t *finally_body) {
  int finally_jump = emit_jump(c, OP_FINALLY);
  unwind_push(c, UNW_FINALLY_BODY);
  compile_stmt(c, finally_body);
  unwind_pop(c);
  emit_op(c, OP_FINALLY_RET);
  patch_jump(c, finally_jump);
}

static void compile_catch_body(sv_compiler_t *c, sv_ast_t *node) {
  check_catch_declarations(c, node->catch_param, node->catch_body);
  begin_scope(c);
  if (node->catch_param && node->catch_param->type == N_IDENT) {
    int loc = add_local(c, node->catch_param->str, node->catch_param->len, false, c->scope_depth);
    if (!(c->locals[loc].eval_flags & SV_EVAL_BIND_CATCH)) c->eval_scope_shareable = false;
    c->locals[loc].eval_flags |= SV_EVAL_BIND_CATCH;
    emit_put_local(c, loc);
  } else if (node->catch_param && is_destructure_pattern_node(node->catch_param)) {
    compile_destructure_binding(c, node->catch_param, SV_VAR_LET);
    emit_op(c, OP_POP);
  } else emit_op(c, OP_POP);  
  
  compile_stmt_region(c, node->catch_body);
  end_scope(c);
}

static void merge_handler_close_floor(sv_compiler_t *c, int outer) {
  if (outer && (!c->handler_close_floor || outer < c->handler_close_floor))
    c->handler_close_floor = outer;
}

void compile_try(sv_compiler_t *c, sv_ast_t *node) {
  emit_set_completion_undefined(c);
  int outer_close_floor = c->handler_close_floor;
  c->handler_close_floor = 0;

  bool has_catch = (node->catch_body != NULL);
  bool has_finally = (node->finally_body != NULL);

  c->try_depth++;
  int try_jump = emit_jump(c, has_finally ? OP_TRY_PUSH_FINALLY : OP_TRY_PUSH);
  unwind_push(c, has_finally ? UNW_TRY_FINALLY : UNW_TRY_CATCH);

  if (has_catch && has_finally) {
    c->try_depth++;
    int inner_jump = emit_jump(c, OP_TRY_PUSH);
    unwind_push(c, UNW_TRY_CATCH);
    compile_stmt_region(c, node->body);

    emit_op(c, OP_TRY_POP);
    unwind_pop(c);
    int inner_end = emit_jump(c, OP_JMP);
    patch_jump(c, inner_jump);

    int catch_tag = emit_jump(c, OP_CATCH);
    emit_handler_close(c);
    compile_catch_body(c, node);

    patch_jump(c, catch_tag);
    patch_jump(c, inner_end);
    c->try_depth--;
  } else compile_stmt_region(c, node->body);

  emit_op(c, OP_TRY_POP);
  unwind_pop(c);

  if (!has_finally) {
    int end_jump = emit_jump(c, OP_JMP);
    patch_jump(c, try_jump);
    int catch_tag = emit_jump(c, OP_CATCH);
    emit_handler_close(c);
    if (has_catch) compile_catch_body(c, node);
    else emit_op(c, OP_POP);
    patch_jump(c, catch_tag);
    patch_jump(c, end_jump);
    c->try_depth--;
    merge_handler_close_floor(c, outer_close_floor);
    return;
  }

  int to_finally_from_try = emit_jump(c, OP_JMP);
  patch_jump(c, try_jump);
  emit_op(c, OP_POP);
  emit_handler_close(c);
  
  int to_finally_from_throw = emit_jump(c, OP_JMP);
  patch_jump(c, to_finally_from_try);
  patch_jump(c, to_finally_from_throw);
  
  compile_finally_block(c, node->finally_body);
  c->try_depth--;
  merge_handler_close_floor(c, outer_close_floor);
}

// TODO: cleanup, reduce nesting
static void hoist_switch_func_decls(sv_compiler_t *c, sv_ast_t *node) {
  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *cas = node->args.items[i];
    for (int j = 0; cas && j < cas->args.count; j++) {
      sv_ast_t *stmt = cas->args.items[j];
      if (!stmt) continue;
      if (stmt->type == N_FUNC && stmt->str && !(stmt->flags & (FN_ARROW | FN_PAREN))) {
        int local = ensure_local_at_depth(c, stmt->str, stmt->len, false, c->scope_depth);
        if (!(c->locals[local].eval_flags & SV_EVAL_BIND_LEXICAL)) c->eval_scope_shareable = false;
        c->locals[local].eval_flags |= SV_EVAL_BIND_LEXICAL;
      } else if (!c->is_strict && (stmt->type == N_IF || stmt->type == N_LABEL)) {
        sv_ast_list_t funcs = {0};
        annex_b_collect_funcs(c->js, stmt, &funcs);
        for (int k = 0; k < funcs.count; k++)
          if (resolve_local(c, funcs.items[k]->str, funcs.items[k]->len) == -1)
            add_local(c, funcs.items[k]->str, funcs.items[k]->len, false, c->scope_depth);
      }
    }
  }
  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *cas = node->args.items[i];
    for (int j = 0; cas && j < cas->args.count; j++) {
      sv_ast_t *stmt = cas->args.items[j];
      if (!stmt) continue;
      if (stmt->type == N_FUNC && stmt->str && !(stmt->flags & (FN_ARROW | FN_PAREN))) hoist_one_func(c, stmt, false);
      else if (!c->is_strict && (stmt->type == N_IF || stmt->type == N_LABEL)) {
        sv_ast_list_t one = { .items = &cas->args.items[j], .count = 1, .cap = 1 };
        hoist_func_decls(c, &one);
      }
    }
  }
}

static void emit_switch_func_annex_b(sv_compiler_t *c, sv_ast_t *node) {
  if (c->is_strict || !node || node->type != N_FUNC || !node->str || (node->flags & (FN_ARROW | FN_PAREN))) return;
  int annex_var = !is_sloppy_eval(c) ? resolve_local_at_depth(c, node->str, node->len, 0) : -1;
  if (annex_var >= 0 && (c->locals[annex_var].eval_flags & SV_EVAL_BIND_LEXICAL)) annex_var = -1;
  bool eval_annex_var = is_sloppy_eval(c) && find_eval_var(c, node->str, node->len) >= 0;
  if (annex_var < 0 && !eval_annex_var) return;
  emit_get_var(c, node->str, node->len);
  if (annex_var >= 0 && eval_annex_var) emit_op(c, OP_DUP);
  if (annex_var >= 0) emit_put_local(c, annex_var);
  if (eval_annex_var) emit_atom_op(c, OP_PUT_EVAL_FUNCTION, node->str, node->len);
}

static void hoist_switch_lexical_decls(sv_compiler_t *c, sv_ast_t *node) {
  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *cas = node->args.items[i];
    for (int j = 0; cas && j < cas->args.count; j++) {
      sv_ast_t *stmt = cas->args.items[j];
      bool lexical = stmt && (
        (stmt->type == N_VAR && stmt->var_kind != SV_VAR_VAR) ||
        (stmt->type == N_CLASS && stmt->str && (stmt->flags & FN_CLASS_DECL)));
      if (!lexical) continue;
      sv_ast_list_t one = { .items = &cas->args.items[j], .count = 1, .cap = 1 };
      hoist_lexical_decls(c, &one);
    }
  }
}

void compile_switch(sv_compiler_t *c, sv_ast_t *node) {
  emit_set_completion_undefined(c);

  int case_count = node->args.count;
  int default_case = -1;
  int *match_to_stub = NULL;
  int *stub_to_body = NULL;

  if (case_count > 0) {
    match_to_stub = calloc((size_t)case_count, sizeof(int));
    stub_to_body = calloc((size_t)case_count, sizeof(int));
    for (int i = 0; i < case_count; i++) {
      match_to_stub[i] = -1;
      stub_to_body[i] = -1;
    }
  }

  compile_expr(c, node->cond);  
  begin_scope(c);
  check_switch_declarations(c, node);
  int lexical_lb = c->local_count;
  hoist_switch_lexical_decls(c, node);
  int lexical_ub = c->local_count;
  hoist_switch_func_decls(c, node);
  push_loop(c, c->code_len, NULL, 0, SV_LOOP_SWITCH);

  for (int i = 0; i < case_count; i++) {
    sv_ast_t *cas = node->args.items[i];
    if (!cas->left) {
      default_case = i;
      continue;
    }
    emit_op(c, OP_DUP);
    compile_expr(c, cas->left);
    emit_op(c, OP_SEQ);
    match_to_stub[i] = emit_jump(c, OP_JMP_TRUE);
  }

  int miss_jump = emit_jump(c, OP_JMP);
  int default_to_body = -1;
  int miss_to_end = -1;

  for (int i = 0; i < case_count; i++) {
    if (match_to_stub[i] < 0) continue;
    patch_jump(c, match_to_stub[i]);
    emit_op(c, OP_POP);
    stub_to_body[i] = emit_jump(c, OP_JMP);
  }

  patch_jump(c, miss_jump);
  emit_op(c, OP_POP);
  if (default_case >= 0)
    default_to_body = emit_jump(c, OP_JMP);
  else
    miss_to_end = emit_jump(c, OP_JMP);

  for (int i = 0; i < case_count; i++) {
    if (i == default_case && default_to_body >= 0)
      patch_jump(c, default_to_body);
    if (stub_to_body[i] >= 0)
      patch_jump(c, stub_to_body[i]);

    for (int j = lexical_lb; j < lexical_ub; j++) c->locals[j].is_tdz = true;
    sv_ast_t *cas = node->args.items[i];
    int region = type_region_enter(c);
    for (int j = 0; j < cas->args.count; j++) {
      emit_switch_func_annex_b(c, cas->args.items[j]);
      compile_stmt(c, cas->args.items[j]);
    }
    type_region_exit(c, region);
  }

  if (miss_to_end >= 0)
    patch_jump(c, miss_to_end);

  free(match_to_stub);
  free(stub_to_body);
  pop_loop(c);
  end_scope(c);
}

static inline bool is_loop_node(sv_ast_t *n) {
  return n && 
    (n->type == N_WHILE || n->type == N_DO_WHILE ||
    n->type == N_FOR    || n->type == N_FOR_IN   ||
    n->type == N_FOR_OF || n->type == N_FOR_AWAIT_OF);
}

void compile_label(sv_compiler_t *c, sv_ast_t *node) {
  if (find_loop_label(c, node->str, node->len) >= 0) {
    js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Label '%.*s' has already been declared", (int)node->len, node->str);
    return;
  }

  if (is_loop_node(node->body)) {
    c->pending_label = node->str;
    c->pending_label_len = node->len;
    compile_stmt(c, node->body);
    c->pending_label = NULL;
    c->pending_label_len = 0;
  } else {
    sv_ast_t *inner = node->body;
    while (inner && inner->type == N_LABEL) inner = inner->body;
    
    push_loop(c, c->code_len, node->str, node->len, is_loop_node(inner) ? SV_LOOP_LABEL_OF_LOOP : SV_LOOP_LABEL);
    
    compile_stmt_region(c, node->body);
    pop_loop(c);
  }
}
