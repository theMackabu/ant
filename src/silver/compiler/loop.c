#include "compiler_internal.h"

void compile_do_while(sv_compiler_t *c, sv_ast_t *node) {
  emit_set_completion_undefined(c);

  int loop_start = c->code_len;
  push_loop(c, loop_start, NULL, 0, SV_LOOP_ITERATION);
  compile_stmt(c, node->body);

  sv_loop_t *loop = &c->loops[c->loop_count - 1];
  for (int i = 0; i < loop->continues.count; i++)
    patch_jump(c, loop->continues.offsets[i]);

  compile_truthy_test_expr(c, node->cond);
  int exit_jump = emit_jump(c, OP_JMP_FALSE);
  emit_loop(c, loop_start);
  patch_jump(c, exit_jump);
  pop_loop(c);
}

static void for_add_slot_unique(int **slots, int *count, int *cap, int slot) {
  if (slot < 0) return;
  for (int i = 0; i < *count; i++) {
    if ((*slots)[i] == slot) return;
  }
  if (*count >= *cap) {
    int new_cap = (*cap > 0) ? (*cap * 2) : 8;
    int *new_slots = realloc(*slots, (size_t)new_cap * sizeof(int));
    if (!new_slots) return;
    *slots = new_slots;
    *cap = new_cap;
  }
  (*slots)[(*count)++] = slot;
}

static void for_collect_pattern_slots(sv_compiler_t *c, sv_ast_t *pat, int **slots, int *count, int *cap) {
  if (!pat) return;
  switch (pat->type) {
    case N_IDENT: {
      int slot = resolve_local(c, pat->str, pat->len);
      for_add_slot_unique(slots, count, cap, slot);
      break;
    }
    case N_ASSIGN_PAT:
    case N_ASSIGN:
      for_collect_pattern_slots(c, pat->left, slots, count, cap);
      break;
    case N_REST:
    case N_SPREAD:
      for_collect_pattern_slots(c, pat->right, slots, count, cap);
      break;
    case N_ARRAY:
    case N_ARRAY_PAT:
      for (int i = 0; i < pat->args.count; i++)
        for_collect_pattern_slots(c, pat->args.items[i], slots, count, cap);
      break;
    case N_OBJECT:
    case N_OBJECT_PAT:
      for (int i = 0; i < pat->args.count; i++) {
        sv_ast_t *prop = pat->args.items[i];
        if (!prop) continue;
        if (prop->type == N_PROPERTY)
          for_collect_pattern_slots(c, prop->right, slots, count, cap);
        else
          for_collect_pattern_slots(c, prop, slots, count, cap);
      }
      break;
    default:
      break;
  }
}

static void for_collect_var_decl_slots(sv_compiler_t *c, sv_ast_t *init_var, int **slots, int *count, int *cap) {
  if (!init_var || init_var->type != N_VAR) return;
  for (int i = 0; i < init_var->args.count; i++) {
    sv_ast_t *decl = init_var->args.items[i];
    if (!decl || decl->type != N_VARDECL || !decl->left) continue;
    for_collect_pattern_slots(c, decl->left, slots, count, cap);
  }
}

static bool loop_head_reads_own_bindings(sv_compiler_t *c, sv_ast_t *var_node, int lb) {
  for (int i = 0; i < var_node->args.count; i++) {
    sv_ast_t *decl = var_node->args.items[i];
    if (!decl || decl->type != N_VARDECL) continue;
    if (decl->left && decl->left->type != N_IDENT) return true;
    for (int l = lb; l < c->local_count; l++)
      if (ast_mentions_name(decl->right, c->locals[l].name, c->locals[l].name_len)) return true;
  }
  return false;
}

static void hoist_loop_head_lexicals(sv_compiler_t *c, sv_ast_t *var_node, bool always_tdz) {
  bool is_const =
    var_node->var_kind == SV_VAR_CONST ||
    var_node->var_kind == SV_VAR_USING ||
    var_node->var_kind == SV_VAR_AWAIT_USING;
  int lb = c->local_count;
  for (int i = 0; i < var_node->args.count; i++) {
    sv_ast_t *decl = var_node->args.items[i];
    if (!decl || decl->type != N_VARDECL || !decl->left) continue;
    hoist_lexical_pattern(c, decl->left, is_const);
  }
  if (!always_tdz && !loop_head_reads_own_bindings(c, var_node, lb)) return;
  for (int i = lb; i < c->local_count; i++) {
    c->locals[i].is_tdz = true;
    set_local_inferred_type(c, i, SV_TI_UNKNOWN);
    emit_op(c, OP_SET_LOCAL_UNDEF);
    emit_u16(c, (uint16_t)(i - c->param_locals));
  }
}

static bool for_captures_name(sv_ast_t *node, const char *name, uint32_t len) {
  return
    ast_closure_captures_name(node->init, name, len)   || ast_closure_captures_name(node->cond, name, len) ||
    ast_closure_captures_name(node->update, name, len) || ast_closure_captures_name(node->body, name, len);
}

static int for_iteration_close_slot(sv_compiler_t *c, sv_ast_t *node) {
  sv_var_kind_t kind = node->init->var_kind;
  if (kind != SV_VAR_LET && kind != SV_VAR_CONST) return -1;

  int *iter_slots = NULL;
  int iter_count = 0, iter_cap = 0;
  for_collect_var_decl_slots(c, node->init, &iter_slots, &iter_count, &iter_cap);

  int close_slot = -1;
  for (int i = 0; i < iter_count; i++) {
    sv_local_t *local = &c->locals[iter_slots[i]];
    if (!for_captures_name(node, local->name, local->name_len)) continue;
    int slot = local_to_frame_slot(c, iter_slots[i]);
    if (close_slot < 0 || slot < close_slot) close_slot = slot;
  }
  free(iter_slots);
  return close_slot;
}

static int compile_for_init(sv_compiler_t *c, sv_ast_t *node) {
  if (!node->init) return -1;
  if (node->init->type != N_VAR) {
    compile_expr(c, node->init);
    emit_op(c, OP_POP);
    return -1;
  }
  if (node->init->var_kind != SV_VAR_VAR) hoist_loop_head_lexicals(c, node->init, false);
  compile_var_decl(c, node->init);
  return for_iteration_close_slot(c, node);
}

static void compile_for_update(sv_compiler_t *c, sv_ast_t *upd) {
  if (!upd) return;
  int slot = -1;
  bool local_step =
    upd->type == N_UPDATE && upd->right && upd->right->type == N_IDENT &&
    (slot = resolve_local_slot(c, upd->right->str, upd->right->len)) >= 0 &&
    !c->locals[c->param_locals + slot].captured &&
    !local_with_shadowed(c, upd->right->str, upd->right->len);
  if (!local_step) {
    compile_expr(c, upd);
    emit_op(c, OP_POP);
    return;
  }
  emit_op(c, upd->op == TOK_POSTINC ? OP_INC_LOCAL : OP_DEC_LOCAL);
  emit(c, (uint8_t)slot);
  set_local_inferred_type(c, c->param_locals + slot, SV_TI_UNKNOWN);
}

void compile_for(sv_compiler_t *c, sv_ast_t *node) {
  emit_set_completion_undefined(c);
  check_for_head_declarations(c, node->init, node->body);
  begin_scope(c);

  int close_slot = compile_for_init(c, node);
  if (close_slot >= 0) emit_slot_op(c, OP_CLOSE_UPVAL, (uint16_t)close_slot);

  int loop_start = c->code_len;
  push_loop(c, loop_start, NULL, 0, SV_LOOP_ITERATION);

  int exit_jump = -1;
  if (node->cond) {
    compile_truthy_test_expr(c, node->cond);
    exit_jump = emit_jump(c, OP_JMP_FALSE);
  }

  compile_stmt(c, node->body);
  sv_loop_t *loop = &c->loops[c->loop_count - 1];
  for (int i = 0; i < loop->continues.count; i++)
    patch_jump(c, loop->continues.offsets[i]);

  if (close_slot >= 0) emit_slot_op(c, OP_CLOSE_UPVAL, (uint16_t)close_slot);
  compile_for_update(c, node->update);

  emit_loop(c, loop_start);
  if (exit_jump >= 0) patch_jump(c, exit_jump);
  pop_loop(c);
  end_scope(c);
}

static void compile_for_each(sv_compiler_t *c, sv_ast_t *node, bool is_for_of);

void compile_for_in(sv_compiler_t *c, sv_ast_t *node) {
  compile_for_each(c, node, false);
}

void compile_for_of(sv_compiler_t *c, sv_ast_t *node) {
  compile_for_each(c, node, true);
}

static void compile_for_each_assign_target(sv_compiler_t *c, sv_ast_t *lhs) {
  if (!lhs) return;
  if (lhs->type == N_VAR && lhs->args.count > 0) {
    sv_ast_t *decl = lhs->args.items[0];
    sv_ast_t *target = decl->left ? decl->left : decl;
    if (target->type == N_IDENT) {
      if (lhs->var_kind == SV_VAR_VAR && (is_sloppy_eval(c) || local_with_shadowed(c, target->str, target->len))) {
        emit_set_var(c, target->str, target->len, false);
        return;
      }
      int loc = resolve_local(c, target->str, target->len);
      if (loc == -1) {
        bool is_const = (lhs->var_kind == SV_VAR_CONST ||
                         lhs->var_kind == SV_VAR_USING ||
                         lhs->var_kind == SV_VAR_AWAIT_USING);
        loc = add_local(c, target->str, target->len, is_const, c->scope_depth);
      }
      emit_put_local(c, loc);
    } else {
      if (lhs->var_kind == SV_VAR_VAR) {
        compile_lhs_set(c, target, false);
      } else {
        compile_destructure_binding(c, target, lhs->var_kind);
        emit_op(c, OP_POP);
      }
    }
    return;
  }
  if (lhs->type == N_IDENT) {
    emit_set_var(c, lhs->str, lhs->len, false);
    return;
  }
  compile_lhs_set(c, lhs, false);
}

static void compile_using_dispose_target(sv_compiler_t *c, sv_ast_t *lhs) {
  if (!lhs || lhs->type != N_VAR) return;
  bool is_await_using = lhs->var_kind == SV_VAR_AWAIT_USING;
  if (lhs->var_kind != SV_VAR_USING && !is_await_using) return;
  if (lhs->args.count == 0) return;

  sv_ast_t *decl = lhs->args.items[0];
  if (!decl || decl->type != N_VARDECL || !decl->left || decl->left->type != N_IDENT) return;

  emit_get_var(c, decl->left->str, decl->left->len);
  emit_dispose_resource(c, is_await_using);
  emit_op(c, OP_POP);
}

static bool compile_using_push_target(sv_compiler_t *c, sv_ast_t *lhs, int stack_local) {
  if (!lhs || lhs->type != N_VAR) return false;
  bool is_await_using = lhs->var_kind == SV_VAR_AWAIT_USING;
  if (lhs->var_kind != SV_VAR_USING && !is_await_using) return false;
  if (lhs->args.count == 0) return false;

  sv_ast_t *decl = lhs->args.items[0];
  if (!decl || decl->type != N_VARDECL || !decl->left || decl->left->type != N_IDENT) return false;

  emit_get_local(c, stack_local);
  emit_get_var(c, decl->left->str, decl->left->len);
  emit_using_push(c, is_await_using);
  emit_op(c, OP_POP);
  
  return true;
}

static void compile_for_each(sv_compiler_t *c, sv_ast_t *node, bool is_for_of) {
  emit_set_completion_undefined(c);
  check_for_head_declarations(c, node->left, node->body);
  begin_scope(c);

  int *iter_slots = NULL;
  int iter_count = 0;
  int iter_cap = 0;

  if (node->left && node->left->type == N_VAR &&
      node->left->var_kind != SV_VAR_VAR) {
    hoist_loop_head_lexicals(c, node->left, true);
    for_collect_var_decl_slots(c, node->left, &iter_slots, &iter_count, &iter_cap);
  }

  if (!is_for_of && node->left && node->left->type == N_VAR &&
      node->left->var_kind == SV_VAR_VAR && node->left->args.count > 0) {
    sv_ast_t *decl = node->left->args.items[0];
    if (decl && decl->right) {
      compile_expr(c, decl->right);
      sv_ast_t *target = decl->left ? decl->left : decl;
      if (target->type == N_IDENT && !is_sloppy_eval(c) && !local_with_shadowed(c, target->str, target->len)) {
        int loc = resolve_local(c, target->str, target->len);
        if (loc == -1)
          loc = add_local(c, target->str, target->len, false, c->scope_depth);
        emit_put_local(c, loc);
      } else {
        compile_lhs_set(c, target, false);
      }
    }
  }

  int iter_local = -1;
  int idx_local = -1;
  int exit_jump = -1;
  int try_jump_for_of = -1;
  int iter_err_local = -1;
  int break_close_slot = -1;
  int iter_inner_start = -1;
  int using_stack_local = -1;

  bool is_for_await = (node->type == N_FOR_AWAIT_OF);
  if (is_for_await && !await_allowed(c))
    js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Unexpected reserved word");
  bool is_using_loop = node->left && node->left->type == N_VAR &&
    (node->left->var_kind == SV_VAR_USING || node->left->var_kind == SV_VAR_AWAIT_USING);
  bool is_await_using_loop = is_using_loop && node->left->var_kind == SV_VAR_AWAIT_USING;
  int old_using_stack = c->using_stack_local;
  bool old_using_async = c->using_stack_async;
  uint8_t iter_hint = 0;
  if (is_for_of && !is_for_await)
    iter_hint = iter_hint_for_type(infer_expr_type(c, node->right));

  if (is_using_loop) {
    emit_empty_disposal_stack(c);
    using_stack_local = add_local(c, "", 0, false, c->scope_depth);
    emit_put_local(c, using_stack_local);
  }

  compile_expr(c, node->right);
  if (is_for_of) {
    emit_op(c, is_for_await ? OP_FOR_AWAIT_OF : OP_FOR_OF);
    iter_err_local = add_local(c, "", 0, false, c->scope_depth);
    try_jump_for_of = emit_jump(c, OP_TRY_PUSH);
    unwind_push(c, UNW_TRY_CATCH);
    c->try_depth++;
  } else {
    emit_op(c, OP_FOR_IN);  
    iter_local = add_local(c, "", 0, false, c->scope_depth);
    emit_put_local(c, iter_local);

    emit_number(c, 0);
    idx_local = add_local(c, "", 0, false, c->scope_depth);
    emit_put_local(c, idx_local);
  }

  int loop_start = c->code_len;
  push_loop(c, loop_start, NULL, 0, SV_LOOP_ITERATION);
  
  if (is_for_of) {
    c->loops[c->loop_count - 1].iter_unwind_index = c->unwind_count - 1;
    c->loops[c->loop_count - 1].iter_completion_local = iter_err_local;
    c->loops[c->loop_count - 1].iter_async = is_for_await;
  }

  if (is_using_loop) {
    emit_empty_disposal_stack(c);
    emit_put_local(c, using_stack_local);
  }

  if (is_for_of) {
    if (is_for_await) {
      emit_op(c, OP_AWAIT_ITER_NEXT);
    } else {
      emit_op(c, OP_ITER_NEXT);
      emit(c, iter_hint);
    }
    exit_jump = emit_jump(c, OP_JMP_TRUE);  
  } else {
    emit_get_local(c, idx_local);
    emit_get_local(c, iter_local);
    emit_get_length(c);
    emit_op(c, OP_LT);
    exit_jump = emit_jump(c, OP_JMP_FALSE);

    emit_get_local(c, iter_local);
    emit_get_local(c, idx_local);
    emit_op(c, OP_GET_ELEM);
  }

  compile_for_each_assign_target(c, node->left);

  if (iter_count > 0) {
    begin_scope(c);
    iter_inner_start = c->local_count;
    for (int i = 0; i < iter_count; i++) {
      int outer_idx = iter_slots[i];
      sv_local_t outer = c->locals[outer_idx];
      emit_get_local(c, outer_idx);
      int inner_idx = add_local(c, outer.name, outer.name_len,
                                outer.is_const, c->scope_depth);
      c->locals[inner_idx].eval_flags = outer.eval_flags;
      emit_put_local(c, inner_idx);
    }
  }

  if (is_using_loop) {
    c->using_stack_local = using_stack_local;
    c->using_stack_async = is_await_using_loop;
    push_using_cleanup(c, using_stack_local, c->scope_depth, is_await_using_loop);
    compile_using_push_target(c, node->left, using_stack_local);
  }

  compile_stmt(c, node->body);
  sv_loop_t *loop = &c->loops[c->loop_count - 1];
  for (int i = 0; i < loop->continues.count; i++)
    patch_jump(c, loop->continues.offsets[i]);

  if (is_using_loop) {
    emit_using_dispose_call(c, using_stack_local, -1, is_await_using_loop, false);
    emit_op(c, OP_POP);

    c->using_stack_local = old_using_stack;
    c->using_stack_async = old_using_async;
    pop_using_cleanup(c);
  } else compile_using_dispose_target(c, node->left);

  if (iter_count > 0) {
    for (int i = 0; i < iter_count; i++) {
      int inner_idx = iter_inner_start + i;
      if (!c->locals[inner_idx].captured) continue;
      int slot = local_to_frame_slot(c, inner_idx);
      if (break_close_slot < 0 || slot < break_close_slot)
        break_close_slot = slot;
    }
    end_scope(c);
  }

  if (!is_for_of) {
    emit_get_local(c, idx_local);
    emit_op(c, OP_INC);
    emit_put_local(c, idx_local);
  }

  emit_loop(c, loop_start);
  patch_jump(c, exit_jump);

  if (is_for_of) {
    emit_op(c, OP_POP);
    emit_op(c, OP_TRY_POP);
    unwind_pop(c);

    emit_op(c, OP_POP);
    emit_op(c, OP_POP);
    emit_op(c, OP_POP);
    int normal_end_jump = emit_jump(c, OP_JMP);

    pop_loop(c);
    if (break_close_slot >= 0) {
      emit_op(c, OP_CLOSE_UPVAL);
      emit_u16(c, (uint16_t)break_close_slot);
    }
    emit_op(c, OP_TRY_POP);
    if (is_for_await) {
      emit_op(c, OP_ITER_CLOSE_ASYNC);
      emit_op(c, OP_AWAIT);
      emit_op(c, OP_ITER_CLOSE_CHECK);
    } else { emit_op(c, OP_ITER_CLOSE); emit(c, 0); }
    int end_jump = emit_jump(c, OP_JMP);

    patch_jump(c, try_jump_for_of);
    int catch_tag = emit_jump(c, OP_CATCH);
    emit_put_local(c, iter_err_local);
    
    if (is_using_loop) {
      emit_using_dispose_call(c, using_stack_local, iter_err_local, is_await_using_loop, true);
      emit_put_local(c, iter_err_local);
    }
    
    if (is_for_await) {
      int close_try = emit_jump(c, OP_TRY_PUSH);
      emit_op(c, OP_ITER_CLOSE_ASYNC);
      emit_op(c, OP_AWAIT);
      emit_op(c, OP_ITER_CLOSE_CHECK);
      emit_op(c, OP_TRY_POP);
      int close_end = emit_jump(c, OP_JMP);

      patch_jump(c, close_try);
      int close_catch_tag = emit_jump(c, OP_CATCH);
      emit_op(c, OP_POP);
      patch_jump(c, close_catch_tag);
      patch_jump(c, close_end);
    } else { emit_op(c, OP_ITER_CLOSE); emit(c, 1); }
    emit_get_local(c, iter_err_local);  
    emit_op(c, OP_THROW);              
    patch_jump(c, catch_tag);

    patch_jump(c, end_jump);
    patch_jump(c, normal_end_jump);
    c->try_depth--;
  } else {
    int skip_break_cleanup = -1;
    if (break_close_slot >= 0)
      skip_break_cleanup = emit_jump(c, OP_JMP);

    pop_loop(c);
    if (break_close_slot >= 0) {
      emit_op(c, OP_CLOSE_UPVAL);
      emit_u16(c, (uint16_t)break_close_slot);
      patch_jump(c, skip_break_cleanup);
    }
  }

  free(iter_slots);
  end_scope(c);
}

void emit_break_to_loop(sv_compiler_t *c, int target) {
  emit_close_upvals_to_depth(c, c->loops[target].scope_depth); emit_using_cleanups_to_depth(c, c->loops[target].scope_depth); emit_loop_exit_jump(c, &c->loops[target], &c->loops[target].breaks);
}

static int find_label_target(sv_compiler_t *c, const sv_ast_t *node) {
  int target = find_loop_label(c, node->str, node->len);
  if (target < 0) js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Undefined label '%.*s'", (int)node->len, node->str);
  return target;
}

void compile_break(sv_compiler_t *c, sv_ast_t *node) {
  int target = -1;
  
  if (node->str) target = find_label_target(c, node);
  else {
    for (int i = c->loop_count - 1; i >= 0 && target < 0; i--)
      if (!c->loops[i].is_block) target = i;
    if (target < 0) js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Illegal break statement");
  } if (target < 0) return;
  emit_close_upvals_to_depth(c, c->loops[target].scope_depth);
  emit_using_cleanups_to_depth(c, c->loops[target].scope_depth);

  if (target >= 0) emit_break_to_loop(c, target);
}

void compile_continue(sv_compiler_t *c, sv_ast_t *node) {
  int target = -1;

  if (node->str) {
    target = find_label_target(c, node);
    if (target < 0) return;
    
    bool reaches_loop = !c->loops[target].is_block || c->loops[target].labels_loop;
    while (reaches_loop && target < c->loop_count && c->loops[target].is_block) target++;
    
    if (!reaches_loop || target >= c->loop_count || c->loops[target].is_switch) {
      js_mkerr_typed(
        c->js, JS_ERR_SYNTAX, "Illegal continue statement: '%.*s' does not denote an iteration statement",
        (int)node->len, node->str
      );
      return;
    }
  } else {
    for (int i = c->loop_count - 1; i >= 0 && target < 0; i--)
      if (!c->loops[i].is_switch && !c->loops[i].is_block) target = i;
    
    if (target < 0) {
      js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Illegal continue statement: no surrounding iteration statement");
      return;
    }
  }
  emit_close_upvals_to_depth(c, c->loops[target].scope_depth); emit_using_cleanups_to_depth(c, c->loops[target].scope_depth);
  emit_loop_exit_jump(c, &c->loops[target], &c->loops[target].continues);
}
