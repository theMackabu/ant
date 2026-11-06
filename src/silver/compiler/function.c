#include "compiler_internal.h"

static bool ast_contains_await_expr(const sv_ast_t *node) {
  if (!node) return false;

  if (node->type == N_AWAIT) return true;
  if (node->type == N_FUNC) return false;

  if (ast_contains_await_expr(node->left))         return true;
  if (ast_contains_await_expr(node->right))        return true;
  if (ast_contains_await_expr(node->cond))         return true;
  if (ast_contains_await_expr(node->body))         return true;
  if (ast_contains_await_expr(node->catch_body))   return true;
  if (ast_contains_await_expr(node->finally_body)) return true;
  if (ast_contains_await_expr(node->catch_param))  return true;
  if (ast_contains_await_expr(node->init))         return true;
  if (ast_contains_await_expr(node->update))       return true;

  for (int i = 0; i < node->args.count; i++)
    if (ast_contains_await_expr(node->args.items[i])) return true;

  return false;
}

static bool func_params_contain_await(const sv_ast_t *node) {
  if (!node) return false;
  for (int i = 0; i < node->args.count; i++)
    if (ast_contains_await_expr(node->args.items[i])) return true;
  return false;
}

static bool collect_param_bindings(ant_t *js, sv_ast_t *param, sv_ast_list_t *names) {
  static const void *dispatch[N__COUNT] = {
    [N_IDENT]      = &&l_ident,
    [N_ASSIGN]     = &&l_left,
    [N_ASSIGN_PAT] = &&l_left,
    [N_REST]       = &&l_right,
    [N_SPREAD]     = &&l_right,
    [N_PROPERTY]   = &&l_right,
    [N_ARRAY]      = &&l_items,
    [N_ARRAY_PAT]  = &&l_items,
    [N_OBJECT]     = &&l_items,
    [N_OBJECT_PAT] = &&l_items,
  };

  if (!param) return true;
  if ((unsigned)param->type >= N__COUNT || !dispatch[param->type]) return true;
  goto *dispatch[param->type];

l_ident:
  if (!param->len) return true;
  return sv_ast_list_push(js, names, param);

l_left:
  return collect_param_bindings(js, param->left, names);

l_right:
  return collect_param_bindings(js, param->right, names);

l_items:
  for (int i = 0; i < param->args.count; i++)
    if (!collect_param_bindings(js, param->args.items[i], names)) return false;
  return true;
}

static int compare_param_names(const void *left, const void *right) {
  const sv_ast_t *a = *(sv_ast_t *const *)left;
  const sv_ast_t *b = *(sv_ast_t *const *)right;
  if (a->len != b->len) return a->len < b->len ? -1 : 1;
  return memcmp(a->str, b->str, a->len);
}

static bool check_function_param_names(ant_t *js, sv_ast_t *fn, bool strict, bool emit_error) {
  sv_ast_t *items[16];
  sv_ast_list_t names = {.items = items, .cap = sizeof(items) / sizeof(items[0])};

  for (int i = 0; i < fn->args.count; i++) {
    if (collect_param_bindings(js, fn->args.items[i], &names)) continue;
    if (emit_error) Ant_Error_CreateOutOfMemory(js);
    return false;
  }

  if (names.count > 1)
    qsort(names.items, (size_t)names.count, sizeof(*names.items), compare_param_names);

  for (int i = 0; i < names.count; i++) {
    sv_ast_t *name = names.items[i];
    if (strict && is_strict_restricted_ident(name->str, name->len)) {
      if (emit_error) js_mkerr_typed(js, JS_ERR_SYNTAX,
        "strict mode forbids '%.*s' as a parameter name", (int)name->len, name->str);
      return false;
    }

    if (i == 0) continue;
    sv_ast_t *prev = names.items[i - 1];
    if (!is_ident_str(name->str, name->len, prev->str, prev->len)) continue;
    if (emit_error) js_mkerr_typed(js, JS_ERR_SYNTAX,
      "duplicate parameter name '%.*s'", (int)name->len, name->str);
    return false;
  }
  return true;
}

static bool function_has_own_use_strict(ant_t *js, sv_ast_t *fn) {
  if (fn->body && fn->body->type == N_BLOCK) for (int i = 0; i < fn->body->args.count; i++) {
    sv_ast_t *stmt = fn->body->args.items[i];
    if (!stmt || stmt->type == N_EMPTY) continue;
    if (stmt->type != N_STRING) break;
    if (sv_ast_is_use_strict(js, stmt)) return true;
  }
  return false;
}

static bool function_has_non_simple_params(sv_ast_t *fn) {
  for (int i = 0; i < fn->args.count; i++) {
    sv_ast_t *param = fn->args.items[i];
    if (!param || param->type != N_IDENT) return true;
  }
  return false;
}

bool check_function_param_early_errors(
  ant_t *js,
  sv_ast_t *fn,
  bool inherited_strict,
  bool *out_has_own_use_strict,
  bool *out_has_non_simple_params,
  bool emit_error
) {
  if (fn->args.count > UINT16_MAX) {
    if (emit_error)
      js_mkerr_typed(js, JS_ERR_SYNTAX, "too many function parameters");
    return false;
  }

  if ((fn->flags & FN_ASYNC) && func_params_contain_await(fn)) {
    if (emit_error)
      js_mkerr_typed(js, JS_ERR_SYNTAX, "await is not allowed in async function parameters");
    return false;
  }

  bool has_own_use_strict = function_has_own_use_strict(js, fn);
  if (out_has_own_use_strict) *out_has_own_use_strict = has_own_use_strict;

  bool strict = inherited_strict || (fn->flags & FN_CLASS_BODY) || has_own_use_strict;
  bool has_non_simple_params = function_has_non_simple_params(fn);
  if (out_has_non_simple_params) *out_has_non_simple_params = has_non_simple_params;

  if (has_own_use_strict && has_non_simple_params) {
    if (emit_error) js_mkerr_typed(js, JS_ERR_SYNTAX,
      "Illegal 'use strict' directive in function with non-simple parameter list");
    return false;
  }

  if (!strict && !has_non_simple_params && !(fn->flags & (FN_ARROW | FN_METHOD))) return true;
  return check_function_param_names(js, fn, strict, emit_error);
}

static uint16_t function_length_from_params(const sv_ast_t *node) {
  if (!node) return 0;
  uint16_t length = 0;
  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *param = node->args.items[i];
    if (!param || param->type == N_REST || param->type == N_ASSIGN_PAT) break;
    length++;
  }
  return length;
}

static bool sv_func_compute_fusable_leaf(sv_func_t *func) {
  if (func->is_async || func->is_generator || func->has_dynamic_eval) return false;
  if (func->is_derived_ctor) return false;
  if (func->upvalue_count > SV_CLOSURE_INLINE_UPVALS) return false;

  for (int i = 0; i < func->upvalue_count; i++)
    if (func->upval_descs[i].is_local && func->upval_descs[i].index != 0) return false;

  const uint8_t *ip = func->code;
  const uint8_t *end = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    switch (op) {
      case OP_CLOSURE_EVAL:
      case OP_CLOSURE_ARGS:
      case OP_INIT_EVAL_ENV:
      case OP_CLOSURE:
      case OP_THIS:
      case OP_SPECIAL_OBJ:
      case OP_EVAL:
      case OP_EXPORT:
      case OP_EXPORT_ALL:
      case OP_GET_SUPER_VAL:
      case OP_TRY_PUSH:
      case OP_TRY_PUSH_FINALLY:
      case OP_CATCH:
      case OP_FINALLY:
      case OP_UNWIND_JMP:
      case OP_AWAIT:
      case OP_AWAIT_ITER_NEXT:
      case OP_FOR_AWAIT_OF:
      case OP_YIELD:
      case OP_ASYNC_YIELD:
        return false;
      case OP_JMP:
      case OP_JMP_FALSE:
      case OP_JMP_TRUE:
      case OP_JMP_FALSE_PEEK:
      case OP_JMP_TRUE_PEEK:
      case OP_JMP_NOT_NULLISH:
        if (sv_get_i32((uint8_t *)ip + 1) < 0) return false;
        break;
      case OP_JMP8:
      case OP_JMP_FALSE8:
      case OP_JMP_TRUE8:
        if ((int8_t)ip[1] < 0) return false;
        break;
      default:
        break;
    }
    int sz = sv_op_size[op];
    if (sz <= 0) return false;
    ip += sz;
  }
  return true;
}

static bool sv_func_compute_curried_step(sv_func_t *func) {
  if (func->is_async || func->is_generator || func->has_dynamic_eval) return false;
  if (func->param_count != 1) return false;

  const uint8_t *p = func->code;
  const uint8_t *end = func->code + func->code_len;
  
  if (end - p < 6 || p[0] != OP_CLOSURE) return false;
  p += 5;
  
  if (p < end && *p == OP_SET_NAME) p += sv_op_size[OP_SET_NAME];
  if (p < end && *p == OP_CLOSE_UPVAL) p += sv_op_size[OP_CLOSE_UPVAL];
  if (p >= end || *p != OP_RETURN) return false;

  uint32_t kidx = sv_get_u32(func->code + 1);
  if (kidx >= (uint32_t)func->const_count) return false;
  
  sv_func_t *child = (sv_func_t *)vptr(func->constants[kidx]);
  if (!child) return false;
  
  return child->is_fusable_leaf;
}

sv_func_t *compile_function_body(
  sv_compiler_t *enclosing,
  sv_ast_t *node,
  sv_compile_mode_t mode
) {
  sv_compiler_t comp;
  sv_compile_ctx_init_child(&comp, enclosing, node, mode);
  if (node->flags & FN_DERIVED_CTOR) comp.derived_field_owner = enclosing;
  else if (comp.is_arrow) comp.derived_field_owner = enclosing->derived_field_owner;

  bool has_own_use_strict = false;
  bool has_non_simple_params = false;

  if (!check_function_param_early_errors(
    comp.js, node, comp.is_strict,
    &has_own_use_strict, &has_non_simple_params, true
  )) return NULL;

  if (!(node->flags & FN_GENERATOR)) {
    bool found = node->flags & FN_SCOPE_FACTS
      ? !!(node->flags & FN_OWN_YIELD)
      : ast_contains_own_yield(node->body, NULL);
    for (int i = 0; !found && !(node->flags & FN_SCOPE_FACTS) && i < node->args.count; i++)
      found = ast_contains_own_yield(node->args.items[i], NULL);
    if (found) {
      js_mkerr_typed(comp.js, JS_ERR_SYNTAX, "yield is only valid in generator functions");
      return NULL;
    }
  }

  if (has_own_use_strict) comp.is_strict = true;
  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *p = node->args.items[i];
    if (p->type == N_IDENT) {
      add_local(&comp, p->str, p->len, false, -1);  
    } else if (p->type == N_REST && p->right && p->right->type == N_IDENT) {
      add_local(&comp, p->right->str, p->right->len, false, -1);
    } else if (p->type == N_ASSIGN_PAT && p->left && p->left->type == N_IDENT) {
      add_local(&comp, p->left->str, p->left->len, false, -1);
    } else add_local(&comp, "", 0, false, -1);
  }

  comp.param_locals = comp.local_count;
  bool params_bind_eval = false;
  for (int i = 0; i < node->args.count; i++)
    params_bind_eval |= ast_pattern_binds(node->args.items[i], "eval");
  bool may_eval = !(node->flags & FN_SCOPE_FACTS) || (node->flags & FN_DIRECT_EVAL);
  if (may_eval && !sv_compile_mode_is_eval(mode) && !is_repl_root(&comp) && !comp.is_strict && !params_bind_eval) {
    for (int i = 0; !comp.owns_eval_env && i < node->args.count; i++)
      comp.owns_eval_env = ast_has_own_eval(&comp, node->args.items[i]);
    if (!comp.owns_eval_env && !ast_has_eval_var(node->body))
      comp.owns_eval_env = ast_has_own_eval(&comp, node->body);
  }
  comp.inherits_eval_env |= comp.owns_eval_env;
  if (comp.owns_eval_env) emit_op(&comp, OP_INIT_EVAL_ENV);
  if (node->flags & FN_CLASS_CTOR) emit_op(&comp, OP_CHECK_CTOR);

  bool value_params = !has_non_simple_params && !comp.owns_eval_env && !is_repl_root(&comp);
  for (int i = 0; value_params && i < node->args.count; i++)
    value_params = node->args.items[i]->type == N_IDENT;
  if (value_params) comp.value_params_node = node;
  if (!comp.owns_eval_env && !is_repl_root(&comp) && !sv_compile_mode_is_eval(mode)) comp.write_scan_node = node;

  if (!comp.is_arrow && comp.enclosing && (node->flags & FN_USES_NEW_TARGET)) {
    comp.new_target_local = add_hidden_local(&comp);
    emit_op(&comp, OP_SPECIAL_OBJ);
    emit(&comp, SV_SPECIAL_NEW_TARGET);
    emit_put_local(&comp, comp.new_target_local);
  }
  
  if (!has_non_simple_params && node->body) {
    if (node->body->type == N_BLOCK) {
      for (int i = 0; i < node->body->args.count; i++)
        hoist_var_decls(&comp, node->body->args.items[i]);
      hoist_lexical_decls(&comp, &node->body->args);
      hoist_func_decls(&comp, &node->body->args);
    } else hoist_var_decls(&comp, node->body);
  }

  if (!has_non_simple_params) {
    for (int i = 0; i < node->args.count; i++) {
      sv_ast_t *p = node->args.items[i];
      if (p->type == N_ASSIGN_PAT) {
        emit_op(&comp, OP_GET_ARG);
        emit_u16(&comp, (uint16_t)i);
        emit_op(&comp, OP_DUP);
        emit_op(&comp, OP_IS_UNDEF);
        int skip = emit_jump(&comp, OP_JMP_FALSE);
        emit_op(&comp, OP_POP);
        compile_expr(&comp, p->right);
        patch_jump(&comp, skip);
        if (p->left && p->left->type == N_IDENT) {
          emit_op(&comp, OP_PUT_ARG);
          emit_u16(&comp, (uint16_t)i);
        } else if (p->left) {
          compile_destructure_binding(&comp, p->left, SV_VAR_VAR);
          emit_op(&comp, OP_POP);
        } else emit_op(&comp, OP_POP);
      } else if (
        p->type == N_ARRAY_PAT || p->type == N_ARRAY ||
        p->type == N_OBJECT_PAT || p->type == N_OBJECT) {
        emit_op(&comp, OP_GET_ARG);
        emit_u16(&comp, (uint16_t)i);
        compile_destructure_binding(&comp, p, SV_VAR_VAR);
        emit_op(&comp, OP_POP);
      } else if (p->type == N_REST && p->right && p->right->type == N_IDENT) {
        emit_op(&comp, OP_REST);
        emit_u16(&comp, (uint16_t)i);
        int loc = resolve_local(&comp, p->right->str, p->right->len);
        if (loc >= 0) {
          emit_op(&comp, OP_PUT_ARG);
          emit_u16(&comp, (uint16_t)loc);
        }
      } else if (p->type == N_REST && p->right) {
        emit_op(&comp, OP_REST);
        emit_u16(&comp, (uint16_t)i);
        compile_destructure_binding(&comp, p->right, SV_VAR_VAR);
        emit_op(&comp, OP_POP);
      }
    }
  } else {
    int *param_bind_locals = NULL;
    int  param_bind_count  = 0;
    int  param_bind_cap    = 0;

    for (int i = 0; i < node->args.count; i++) {
      sv_ast_t *p = node->args.items[i];
      const char *pname = NULL;
      uint32_t    plen  = 0;
      if (p->type == N_IDENT) {
        pname = p->str; plen = p->len;
      } else if ((
        p->type == N_ASSIGN_PAT) && p->left &&
        p->left->type == N_IDENT) {
        pname = p->left->str; plen = p->left->len;
      } else if (
        p->type == N_REST && p->right &&
        p->right->type == N_IDENT) {
        pname = p->right->str; plen = p->right->len;
      }

      int lb = comp.local_count;
      if (pname && plen) {
        int loc = add_local(&comp, pname, plen, false, 0);
        comp.locals[loc].is_tdz = true;
        set_local_inferred_type(&comp, loc, SV_TI_UNKNOWN);
        int slot = loc - comp.param_locals;
        emit_op(&comp, OP_SET_LOCAL_UNDEF);
        emit_u16(&comp, (uint16_t)slot);
      }

      if (param_bind_count >= param_bind_cap) {
        param_bind_cap = param_bind_cap ? param_bind_cap * 2 : 8;
        param_bind_locals = realloc(
          param_bind_locals,
          (size_t)param_bind_cap * sizeof(int));
      }
      param_bind_locals[param_bind_count++] = lb;
    }

    for (int i = 0; i < node->args.count; i++) {
      sv_ast_t *p = node->args.items[i];
      int bind_lb = param_bind_locals[i];

      if (p->type == N_IDENT) {
        if (bind_lb < comp.local_count) {
          emit_op(&comp, OP_GET_ARG);
          emit_u16(&comp, (uint16_t)i);
          int slot = bind_lb - comp.param_locals;
          comp.locals[bind_lb].is_tdz = false;
          if (slot <= 255) { emit_op(&comp, OP_PUT_LOCAL8); emit(&comp, (uint8_t)slot); }
          else { emit_op(&comp, OP_PUT_LOCAL); emit_u16(&comp, (uint16_t)slot); }
          set_local_inferred_type(&comp, bind_lb, SV_TI_UNKNOWN);
        }
      } else if (p->type == N_ASSIGN_PAT) {
        emit_op(&comp, OP_GET_ARG);
        emit_u16(&comp, (uint16_t)i);
        emit_op(&comp, OP_DUP);
        emit_op(&comp, OP_IS_UNDEF);
        int skip = emit_jump(&comp, OP_JMP_FALSE);
        emit_op(&comp, OP_POP);
        compile_expr(&comp, p->right);
        patch_jump(&comp, skip);
        if (p->left && p->left->type == N_IDENT && bind_lb < comp.local_count) {
          int slot = bind_lb - comp.param_locals;
          comp.locals[bind_lb].is_tdz = false;
          if (slot <= 255) { emit_op(&comp, OP_PUT_LOCAL8); emit(&comp, (uint8_t)slot); }
          else { emit_op(&comp, OP_PUT_LOCAL); emit_u16(&comp, (uint16_t)slot); }
          set_local_inferred_type(&comp, bind_lb, SV_TI_UNKNOWN);
        } else if (p->left) {
          compile_destructure_binding(&comp, p->left, SV_VAR_VAR);
          emit_op(&comp, OP_POP);
        } else {
          emit_op(&comp, OP_POP);
        }
      } else if (
        p->type == N_ARRAY_PAT || p->type == N_ARRAY ||
        p->type == N_OBJECT_PAT || p->type == N_OBJECT) {
        emit_op(&comp, OP_GET_ARG);
        emit_u16(&comp, (uint16_t)i);
        compile_destructure_binding(&comp, p, SV_VAR_VAR);
        emit_op(&comp, OP_POP);
      } else if (
        p->type == N_REST && p->right && p->right->type == N_IDENT &&
        bind_lb < comp.local_count) {
        emit_op(&comp, OP_REST);
        emit_u16(&comp, (uint16_t)i);
        int slot = bind_lb - comp.param_locals;
        comp.locals[bind_lb].is_tdz = false;
        if (slot <= 255) { emit_op(&comp, OP_PUT_LOCAL8); emit(&comp, (uint8_t)slot); }
        else { emit_op(&comp, OP_PUT_LOCAL); emit_u16(&comp, (uint16_t)slot); }
        set_local_inferred_type(&comp, bind_lb, SV_TI_UNKNOWN);
      } else if (p->type == N_REST && p->right) {
        emit_op(&comp, OP_REST);
        emit_u16(&comp, (uint16_t)i);
        compile_destructure_binding(&comp, p->right, SV_VAR_VAR);
        emit_op(&comp, OP_POP);
      }
    }

    free(param_bind_locals);
    sv_ast_t *body = node->body;

    if (body && body->type != N_BLOCK) {
      hoist_var_decls(&comp, body);
    } else if (body) {
      for (int i = 0; i < body->args.count; i++)
        hoist_var_decls(&comp, body->args.items[i]);
      hoist_lexical_decls(&comp, &body->args);
      hoist_func_decls(&comp, &body->args);
    }
  }

  if (
    !comp.is_arrow && !comp.args_length_only &&
    has_implicit_arguments_obj(&comp) && (node->flags & FN_USES_ARGS)
  ) {
    comp.strict_args_local = add_hidden_local(&comp);
    emit_op(&comp, OP_SPECIAL_OBJ);
    emit(&comp, SV_SPECIAL_ARGUMENTS);
    emit_put_local(&comp, comp.strict_args_local);
  }

  if (!comp.is_arrow && comp.enclosing && (node->flags & (FN_METHOD | FN_GETTER | FN_SETTER | FN_STATIC))) {
    comp.super_local = add_hidden_local(&comp);
    emit_op(&comp, OP_SPECIAL_OBJ);
    emit(&comp, SV_SPECIAL_SUPER);
    emit_put_local(&comp, comp.super_local);
  }

  if ((node->flags & FN_CLASS_CTOR) && !(node->flags & FN_DERIVED_CTOR) &&
      enclosing->field_init_count > 0) {
    emit_field_inits(&comp, enclosing, enclosing->field_inits, enclosing->field_init_count);
  }

  bool body_has_await_using = false;
  bool body_has_using = node->body && node->body->type == N_BLOCK &&
    stmt_list_has_using_decl(&node->body->args, &body_has_await_using);
  int body_using_try_jump = -1;
  int body_using_err_local = -1;
  int old_using_stack = comp.using_stack_local;
  bool old_using_async = comp.using_stack_async;

  if (body_has_using) {
    emit_empty_disposal_stack(&comp);
    int body_using_stack = add_local(&comp, "", 0, false, comp.scope_depth);
    emit_put_local(&comp, body_using_stack);
    body_using_err_local = add_local(&comp, "", 0, false, comp.scope_depth);

    comp.using_stack_local = body_using_stack;
    comp.using_stack_async = body_has_await_using;
    push_using_cleanup(&comp, body_using_stack, comp.scope_depth, body_has_await_using);

    comp.try_depth++;
    body_using_try_jump = emit_jump(&comp, OP_TRY_PUSH);
    unwind_push(&comp, UNW_TRY_CATCH);
  }

  bool completion_top = is_completion_top_level(&comp);
  if (completion_top) {
    emit_op(&comp, OP_UNDEF);
    comp.completion_local = add_local(&comp, "", 0, false, comp.scope_depth);
    emit_put_local(&comp, comp.completion_local);
  }

  bool emitted_static_declarations =
    should_emit_module_static_declarations(&comp, node->body);
  if (emitted_static_declarations)
    compile_module_static_declaration_prologue(&comp, &node->body->args);

  if (node->body && node->body->type == N_BLOCK) {
    sv_decl_names_t params;
    decl_names_init(&params);
    for (int i = 0; i < node->args.count; i++) collect_pattern_names(node->args.items[i], &params);
    bool module_top = comp.mode == SV_COMPILE_MODULE && comp.enclosing && !comp.enclosing->enclosing;
    check_statement_list_declarations(&comp, &node->body->args, module_top, &params, true);
    decl_names_free(&params);
  }

  if (node->body) {
    if (node->body->type == N_BLOCK)
      compile_body_block(&comp, node->body, emitted_static_declarations);
    else compile_tail_return_expr(&comp, node->body);
  }

  for (int i = 0; i < comp.deferred_export_count; i++) {
    sv_deferred_export_t *e = &comp.deferred_exports[i];
    compile_export_emit(&comp, e->name, e->len);
  }

  if (body_has_using) {
    emit_op(&comp, OP_TRY_POP);
    unwind_pop(&comp);
    comp.try_depth--;

    emit_using_dispose_call(&comp, comp.using_stack_local, -1, body_has_await_using, false);
    emit_op(&comp, OP_POP);
    int end_jump = emit_jump(&comp, OP_JMP);

    patch_jump(&comp, body_using_try_jump);
    int catch_tag = emit_jump(&comp, OP_CATCH);
    emit_put_local(&comp, body_using_err_local);
    emit_using_dispose_call(&comp, comp.using_stack_local, body_using_err_local, body_has_await_using, true);
    if (!body_has_await_using) emit_op(&comp, OP_THROW);
    patch_jump(&comp, catch_tag);
    patch_jump(&comp, end_jump);

    pop_using_cleanup(&comp);
    comp.using_stack_local = old_using_stack;
    comp.using_stack_async = old_using_async;
  }
  
  if (completion_top) {
    emit_get_local(&comp, comp.completion_local);
    emit_return_from_stack(&comp);
  }

  emit_using_cleanups_to_depth(&comp, -1);
  emit_close_upvals(&comp);
  emit_op(&comp, OP_RETURN_UNDEF);

  int max_locals = comp.max_local_count - comp.param_locals;
  sv_func_t *func = sv_func_new(comp.js);

  sv_func_init_code_and_map_templates(&comp, func);
  sv_func_init_obj_sites(&comp, func);

  if (comp.const_count > 0) {
    func->constants = sv_code_bump(comp.js, (size_t)comp.const_count * sizeof(ant_value_t));
    memcpy(func->constants, comp.constants, (size_t)comp.const_count * sizeof(ant_value_t));
    func->const_count = comp.const_count;
    if (!build_gc_const_tables(comp.js, func)) js_mkerr(comp.js, "out of memory building constant tables");
  }

  if (comp.atom_count > 0) {
    func->atoms = sv_code_bump(comp.js, (size_t)comp.atom_count * sizeof(sv_atom_t));
    memcpy(func->atoms, comp.atoms, (size_t)comp.atom_count * sizeof(sv_atom_t));
    func->atom_count = comp.atom_count;
  }
  func->ic_count = (uint16_t)comp.ic_count;
  func->ic_slots = alloc_ic_slots(comp.js, func->ic_count);

  if (comp.upvalue_count > 0) {
    func->upval_descs = sv_code_bump(comp.js, 
      (size_t)comp.upvalue_count * sizeof(sv_upval_desc_t));
    memcpy(func->upval_descs, comp.upval_descs,
      (size_t)comp.upvalue_count * sizeof(sv_upval_desc_t));
    func->upvalue_count = comp.upvalue_count;
  }

  if (comp.srcpos_count > 0) {
    func->debug->srcpos = sv_code_bump(comp.js, (size_t)comp.srcpos_count * sizeof(sv_srcpos_t));
    memcpy(func->debug->srcpos, comp.srcpos, (size_t)comp.srcpos_count * sizeof(sv_srcpos_t));
    func->debug->srcpos_count = comp.srcpos_count;
  }

  if (enclosing->source && enclosing->source_len > 0) {
    func->debug->source = enclosing->source;
    func->debug->source_len = (int)enclosing->source_len;
    func->debug->source_start = (int)node->src_off;
    func->debug->source_end   = (node->src_end > node->src_off)
      ? (int)node->src_end : func->debug->source_len;
  }

  func->max_locals = max_locals;
  sv_func_finalize_type_data(func, &comp, max_locals);
  
  func->param_count = (uint16_t)comp.param_count;
  func->function_length = function_length_from_params(node);
  func->is_strict = comp.is_strict;
  func->is_arrow = comp.is_arrow;
  func->allows_new_target = comp.allows_new_target;
  
  func->is_async = !!(node->flags & FN_ASYNC);
  func->has_await = false;
  func->is_generator = !!(node->flags & FN_GENERATOR);
  func->is_method = !!(node->flags & FN_METHOD);
  if (comp.super_referenced) node->flags |= FN_USES_SUPER;
  func->is_derived_ctor = !!(node->flags & FN_DERIVED_CTOR);
  func->is_class_ctor = !!(node->flags & FN_CLASS_CTOR);
  func->ctor_slot_class = (!func->is_method || func->is_class_ctor) ? ctor_slot_class(node) : 0;
  func->is_static = !!(node->flags & FN_STATIC);
  func->is_tla = comp.is_tla;
  func->debug->filename = enclosing->filename ? enclosing->filename : enclosing->js->filename;
  func->debug->source_line = (int)source_line_of(enclosing, node);
  
  if (node->str && node->len > 0) {
    char *name = sv_code_bump(comp.js, node->len + 1);
    memcpy(name, node->str, node->len);
    name[node->len] = '\0';
    func->debug->name = name;
  } else if (enclosing->inferred_name && enclosing->inferred_name_len > 0) {
    char *name = sv_code_bump(comp.js, enclosing->inferred_name_len + 1);
    memcpy(name, enclosing->inferred_name, enclosing->inferred_name_len);
    name[enclosing->inferred_name_len] = '\0';
    func->debug->name = name;
  }

  if (func->is_async || func->is_tla) {
    const uint8_t *ip = func->code;
    const uint8_t *end = func->code + func->code_len;
    
    while (ip < end) {
      sv_op_t op = (sv_op_t)*ip;
      
      if (
        op == OP_AWAIT        || 
        op == OP_AWAIT_RETURN || 
        op == OP_ASYNC_YIELD  || 
        op == OP_FOR_AWAIT_OF || 
        op == OP_AWAIT_ITER_NEXT
      ) {
        func->has_await = true;
        break;
      }
      
      int sz = sv_op_size[op];
      if (sz <= 0) break;
      ip += sz;
    }
  }

  func->is_fusable_leaf = sv_func_compute_fusable_leaf(func);
  func->is_curried_step = sv_func_compute_curried_step(func);

  sv_compile_ctx_cleanup(&comp);
  return func;
}
