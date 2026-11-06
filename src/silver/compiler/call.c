#include "compiler_internal.h"

void mark_char_code_at_binding(sv_compiler_t *c, sv_ast_t *prop) {
  if (!prop || prop->type != N_PROPERTY || (prop->flags & FN_COMPUTED)) return;
  sv_ast_t *key = prop->left;
  sv_ast_t *value = prop->right;
  
  if (!key || !key->str || !is_ident_str(key->str, key->len, "StringPrototypeCharCodeAt", 25)) return;
  if (value && (value->type == N_ASSIGN_PAT || value->type == N_ASSIGN)) value = value->left;
  if (!value || value->type != N_IDENT) return;
  
  int local = resolve_local(c, value->str, value->len);
  if (local >= 0) c->locals[local].char_code_at_hint = true;
}

static bool call_spread_is_last_only(const sv_ast_t *node) {
  int argc = node->args.count;
  if (argc <= 0 || argc > UINT16_MAX) return false;
  
  for (int i = 0; i < argc - 1; i++) {
    sv_ast_t *arg = node->args.items[i];
    if (arg && arg->type == N_SPREAD) return false;
  }
  
  sv_ast_t *last = node->args.items[argc - 1];
  return last && last->type == N_SPREAD;
}

bool call_has_spread_arg(const sv_ast_t *node) {
  if (!node) return false;
  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *arg = node->args.items[i];
    if (arg && arg->type == N_SPREAD) return true;
  }
  return false;
}

void compile_push_arg_to_array(sv_compiler_t *c, sv_ast_t *arg) {
  if (arg && arg->type == N_EMPTY) emit_op(c, OP_EMPTY);
  else compile_expr(c, arg);
  emit_op(c, OP_APPEND);
}

void compile_concat_spread_to_array(sv_compiler_t *c, sv_ast_t *spread_arg) {
  compile_expr(c, spread_arg->right);
  emit_op(c, OP_SPREAD);
}

static void compile_call_args_array(sv_compiler_t *c, sv_ast_t *call_node) {
  emit_op(c, OP_ARRAY);
  emit_u16(c, 0);
  for (int i = 0; i < call_node->args.count; i++) {
    sv_ast_t *arg = call_node->args.items[i];
    if (arg && arg->type == N_SPREAD) compile_concat_spread_to_array(c, arg);
    else compile_push_arg_to_array(c, arg);
  }
}

static inline bool sv_call_kind_has_receiver(sv_call_kind_t kind) {
  return kind == SV_CALL_METHOD || kind == SV_CALL_SUPER;
}

static bool compile_with_identifier_call(
  sv_compiler_t *c, const char *name, uint32_t len
) {
  sv_with_refs_t shadows = {0};
  with_shadows(c, name, len, &shadows);
  if (shadows.count == 0) return false;

  sv_patch_list_t found = {0};
  emit_with_tries(c, &shadows, OP_WITH_TRY_CALL, name, len, 2, &found);
  emit_op(c, OP_UNDEF);
  emit_get_var_plain(c, name, len);
  patch_list_resolve(c, &found);
  
  return true;
}

static void compile_receiver_property_get(sv_compiler_t *c, sv_ast_t *node) {
  emit_op(c, OP_DUP);
  if (node->flags & 1) {
    compile_expr(c, node->right);
    emit_op(c, OP_GET_ELEM);
  } else if (is_private_name_node(node->right)) {
    emit_private_token(c, node->right);
    emit_op(c, OP_GET_PRIVATE);
  } else {
    emit_srcpos(c, node->right);
    emit_atom_op(c, OP_GET_FIELD, node->right->str, node->right->len);
  }
}

static int emit_optional_chain_short_circuit(sv_compiler_t *c) {
  int present = emit_jump(c, OP_JMP_NOT_NULLISH);
  emit_op(c, OP_POP);
  emit_op(c, OP_UNDEF);
  int end = emit_jump(c, OP_JMP);
  patch_jump(c, present);
  return end;
}

static bool member_call_needs_optional_base_guard(const sv_ast_t *member) {
  if (!member || !member->right) return false;
  if (member->type == N_OPTIONAL) return true;
  return member->type == N_MEMBER &&
    sv_node_has_optional_base(member->left);
}

static int capture_field_key(sv_compiler_t *c, sv_compiler_t *owner, int local) {
  if (c->enclosing == owner) {
    owner->locals[local].captured = true;
    return add_upvalue(c, (uint16_t)local_to_frame_slot(owner, local), true, false);
  }
  int uv = capture_field_key(c->enclosing, owner, local);
  return add_upvalue(c, (uint16_t)uv, false, false);
}

void emit_field_inits(
  sv_compiler_t *c, sv_compiler_t *owner, sv_ast_t **fields, int count
) {
  for (int i = 0; i < count; i++) {
    sv_ast_t *m = fields[i];
    bool is_fn = is_class_method_def(m);
    if (is_private_name_node(m->left)) {
      emit_op(c, OP_THIS);
      emit_private_token(c, m->left);
      if (is_fn) compile_method_function(c, m);
      else if (m->right) compile_expr(c, m->right);
      else emit_op(c, OP_UNDEF);
      emit_op(c, OP_DEF_PRIVATE);
      if (m->flags & FN_GETTER) emit(c, SV_COMP_PRIVATE_GETTER);
      else if (m->flags & FN_SETTER) emit(c, SV_COMP_PRIVATE_SETTER);
      else emit(c, is_fn ? SV_COMP_PRIVATE_METHOD : SV_COMP_PRIVATE_FIELD);
      emit_op(c, OP_POP);
      continue;
    }

    emit_op(c, OP_THIS);
    if (m->right) compile_expr(c, m->right);
    else emit_op(c, OP_UNDEF);
    if (m->flags & FN_COMPUTED) {
      int uv = capture_field_key(c, owner, owner->computed_key_locals[i]);
      emit_op(c, OP_GET_UPVAL);
      emit_u16(c, (uint16_t)uv);
    } else {
      compile_static_property_key(c, m->left);
    }
    emit_op(c, OP_SWAP);
    emit_op(c, OP_DEFINE_METHOD_COMP);
    emit(c, 0);
    emit_op(c, OP_POP);
  }
}

static void emit_derived_field_inits(sv_compiler_t *c) {
  sv_compiler_t *owner = c->derived_field_owner;
  if (owner && owner->field_init_count > 0) 
    emit_field_inits(c, owner, owner->field_inits, owner->field_init_count);
}

void emit_call_srcpos(sv_compiler_t *c, sv_ast_t *node) {
  sv_ast_t *callee = node->left;
  if (callee && (callee->type == N_MEMBER || callee->type == N_OPTIONAL) && callee->right) callee = callee->right;
  emit_srcpos(c, callee ? callee : node);
}

static void compile_call_emit_invoke(
  sv_compiler_t *c, sv_ast_t *node,
  sv_call_kind_t kind, bool has_spread
) {
  int argc = node->args.count;
  if (has_spread) {
    if (kind == SV_CALL_METHOD) emit_op(c, OP_SWAP);
    else if (kind == SV_CALL_DIRECT) emit_op(c, OP_UNDEF);
    
    if (kind != SV_CALL_SUPER && call_spread_is_last_only(node)) {
      for (int i = 0; i < argc - 1; i++) compile_expr(c, node->args.items[i]);
      compile_expr(c, node->args.items[argc - 1]->right);
      
      emit_call_srcpos(c, node);
      emit_op(c, OP_CALL_SPREAD);
      emit_u16(c, (uint16_t)argc);
      
      return;
    }
    
    compile_call_args_array(c, node);
    emit_call_srcpos(c, node);
    emit_op(c, kind == SV_CALL_SUPER ? OP_SUPER_APPLY : OP_APPLY);
    emit_u16(c, 1);
    
    if (kind == SV_CALL_SUPER) emit_derived_field_inits(c);
    
    return;
  }

  for (int i = 0; i < argc; i++)
    compile_expr(c, node->args.items[i]);
  emit_call_srcpos(c, node);
  emit_op(c,
    kind == SV_CALL_SUPER ? OP_CALL_SUPER :
    kind == SV_CALL_METHOD ? OP_CALL_METHOD : OP_CALL);
  emit_u16(c, (uint16_t)argc);
  if (kind == SV_CALL_SUPER) emit_derived_field_inits(c);
}

sv_call_kind_t compile_call_setup_non_optional(sv_compiler_t *c, sv_ast_t *callee) {
  if (is_ident_name(callee, "super")) {
    emit_op(c, OP_THIS);
    emit_get_var(c, "super", 5);
    if (c->is_arrow) emit_lexical_new_target(c);
    else {
      emit_op(c, OP_SPECIAL_OBJ);
      emit(c, SV_SPECIAL_NEW_TARGET);
    }
    return SV_CALL_SUPER;
  }

  if (callee->type == N_MEMBER && is_ident_name(callee->left, "super")) {
    if (!(callee->flags & 1) && is_private_name_node(callee->right)) {
      js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Cannot access private member through super");
      emit_op(c, OP_UNDEF);
      return SV_CALL_DIRECT;
    }
    emit_op(c, OP_THIS);
    emit_op(c, OP_THIS);
    emit_get_var(c, "super", 5);
    if (callee->flags & 1)
      compile_expr(c, callee->right);
    else
      emit_constant(c, sv_const_str(c, callee->right->str, callee->right->len));
    emit_op(c, OP_GET_SUPER_VAL);
    return SV_CALL_METHOD;
  }

  if (callee->type == N_MEMBER) {
    compile_expr(c, callee->left);
    compile_receiver_property_get(c, callee);
    return SV_CALL_METHOD;
  }

  if (
    callee->type == N_IDENT &&
    compile_with_identifier_call(c, callee->str, callee->len)
  ) return SV_CALL_METHOD;

  compile_expr(c, callee);
  return SV_CALL_DIRECT;
}

static bool compile_call_is_proto_intrinsic(
  sv_compiler_t *c, sv_ast_t *node, bool has_spread
) {
  if (!node || has_spread || node->args.count != 1) return false;
  sv_ast_t *callee = node->left;
  if (!callee || callee->type != N_MEMBER) return false;
  if ((callee->flags & 1) || !callee->right || !callee->right->str) return false;
  if (is_ident_name(callee->left, "super")) return false;
  if (!is_ident_str(callee->right->str, callee->right->len, "isPrototypeOf", 13))
    return false;

  compile_expr(c, callee->left);
  compile_receiver_property_get(c, callee);
  compile_expr(c, node->args.items[0]);
  emit_op(c, OP_CALL_IS_PROTO);
  emit_u16(c, alloc_ic_idx(c));
  return true;
}

static bool compile_call_array_includes_intrinsic(
  sv_compiler_t *c, sv_ast_t *node, bool has_spread
) {
  if (!node || has_spread || node->args.count > UINT16_MAX) return false;
  sv_ast_t *callee = node->left;
  
  if (!callee || callee->type != N_MEMBER) return false;
  if ((callee->flags & 1) || !callee->right || !callee->right->str) return false;
  if (is_ident_name(callee->left, "super")) return false;
  if (!is_ident_str(callee->right->str, callee->right->len, "includes", 8))
    return false;

  compile_expr(c, callee->left);
  compile_receiver_property_get(c, callee);
  for (int i = 0; i < node->args.count; i++)
    compile_expr(c, node->args.items[i]);
  emit_op(c, OP_CALL_ARRAY_INCLUDES);
  emit_u16(c, (uint16_t)node->args.count);
  
  return true;
}

static bool is_char_code_at_binding(sv_compiler_t *c, sv_ast_t *callee) {
  if (!callee || callee->type != N_IDENT) return false;
  for (sv_compiler_t *scope = c; scope; scope = scope->enclosing) {
    int local = resolve_local(scope, callee->str, callee->len);
    if (local >= 0) return scope->locals[local].char_code_at_hint;
  }
  return false;
}

static bool compile_call_char_code_at_intrinsic(
  sv_compiler_t *c, sv_ast_t *node, bool has_spread
) {
  if (!node || has_spread || node->args.count > UINT16_MAX) return false;
  sv_ast_t *callee = node->left;
  
  if (!callee) return false;
  if (callee->type == N_MEMBER) {
    if (member_call_needs_optional_base_guard(callee)) return false;
    if ((callee->flags & 1) || !callee->right || !callee->right->str) return false;
    if (is_ident_name(callee->left, "super")) return false;
    if (!is_ident_str(callee->right->str, callee->right->len, "charCodeAt", 10)) return false;
  } else if (!is_char_code_at_binding(c, callee)) return false;

  sv_call_kind_t kind = compile_call_setup_non_optional(c, callee);
  if (kind == SV_CALL_DIRECT) {
    emit_op(c, OP_UNDEF);
    emit_op(c, OP_SWAP);
  }
  
  for (int i = 0; i < node->args.count; i++) compile_expr(c, node->args.items[i]);
  emit_op(c, OP_CALL_CHAR_CODE_AT);
  emit_u16(c, (uint16_t)node->args.count);
  
  return true;
}

static bool compile_call_string_intrinsic(
  sv_compiler_t *c, sv_ast_t *node, bool has_spread
) {
  if (!node || has_spread || node->args.count > UINT16_MAX) return false;
  sv_ast_t *callee = node->left;

  if (!callee || callee->type != N_MEMBER) return false;
  if (member_call_needs_optional_base_guard(callee)) return false;
  if ((callee->flags & 1) || !callee->right || !callee->right->str) return false;
  if (is_ident_name(callee->left, "super")) return false;

  ant_string_intrinsic_kind_t kind;
  if (is_ident_str(callee->right->str, callee->right->len, "indexOf", 7))
    kind = ANT_STRING_INTRINSIC_INDEX_OF;
  else if (is_ident_str(callee->right->str, callee->right->len, "substring", 9))
    kind = ANT_STRING_INTRINSIC_SUBSTRING;
  else return false;

  compile_expr(c, callee->left);
  compile_receiver_property_get(c, callee);
  for (int i = 0; i < node->args.count; i++)
    compile_expr(c, node->args.items[i]);

  emit_op(c, OP_CALL_STRING_INTRINSIC);
  emit(c, (uint8_t)kind);
  emit_u16(c, (uint16_t)node->args.count);

  return true;
}

bool compile_call_map_template_intrinsic(
  sv_compiler_t *c, sv_ast_t *node, bool has_spread, bool is_tail
) {
  if (!node || has_spread || node->args.count != 1) return false;
  if (is_tail && (c->try_depth > 0 || c->using_cleanup_count > 0))
    return false;
  sv_ast_t *callee = node->left;
  
  if (!callee || callee->type != N_MEMBER) return false;
  if (member_call_needs_optional_base_guard(callee)) return false;
  if ((callee->flags & 1) || !callee->right || !callee->right->str) return false;
  if (is_ident_name(callee->left, "super")) return false;
  
  sv_map_template_operation_t operation;
  if (is_ident_str(callee->right->str, callee->right->len, "get", 3))
    operation = SV_MAP_TEMPLATE_GET;
  else if (is_ident_str(callee->right->str, callee->right->len, "has", 3))
    operation = SV_MAP_TEMPLATE_HAS;
  else return false;

  sv_ast_t *tpl = node->args.items[0];
  if (!tpl || tpl->type != N_TEMPLATE || tpl->args.count < 3 ||
      tpl->args.count > 2 * SV_MAP_TEMPLATE_MAX_SUBSTITUTIONS + 1 ||
      (tpl->args.count & 1) == 0 ||
      !template_has_valid_cooked_segments(tpl))
    return false;

  uint8_t substitution_count = (uint8_t)((tpl->args.count - 1) / 2);
  for (uint8_t i = 0; i <= substitution_count; i++)
    if (!is_template_segment(tpl->args.items[i * 2])) return false;

  uint32_t desc_idx = add_map_template_desc(
    c, operation, tpl, substitution_count);

  compile_expr(c, callee->left);
  compile_receiver_property_get(c, callee);
  for (uint8_t i = 0; i < substitution_count; i++) {
    compile_expr(c, tpl->args.items[i * 2 + 1]);
    if (i + 1 < substitution_count)
      emit_op(c, OP_TO_STRING_DEFER_NUMBER);
  }
  
  if (is_tail) emit_close_upvals(c);
  emit_op(c, is_tail ? OP_TAIL_MAP_TEMPLATE : OP_CALL_MAP_TEMPLATE);
  emit_u32(c, desc_idx);
  
  return true;
}

static bool compile_call_stable_builtin(
  sv_compiler_t *c, sv_ast_t *node, bool has_spread
) {
  if (!node || has_spread || node->args.count != 1) return false;
  sv_ast_t *callee = node->left;
  if (!callee || callee->type != N_MEMBER) return false;
  if ((callee->flags & 1) || !callee->right || !callee->right->str) return false;
  
  // TODO: (feat) to be expanded into more builtins as needed
  if (!is_ident_name(callee->left, "Promise")) return false;
  if (!is_ident_str(callee->right->str, callee->right->len, "resolve", 7))
    return false;

  bool can_load_intrinsic =
    !has_active_with_scope(c) && !c->inherits_eval_env &&
    resolve_local(c, "Promise", 7) == -1 &&
    resolve_upvalue(c, "Promise", 7) == -1;

  if (can_load_intrinsic) {
    emit_op(c, OP_LOAD_STABLE_BUILTIN);
    emit(c, SV_STABLE_BUILTIN_PROMISE_RESOLVE);
  } else {
    compile_expr(c, callee->left);
    compile_receiver_property_get(c, callee);
  }

  compile_expr(c, node->args.items[0]);
  emit_op(c, OP_CALL_STABLE_BUILTIN);
  emit(c, SV_STABLE_BUILTIN_PROMISE_RESOLVE);
  emit_u16(c, 1);
  
  return true;
}

static bool regexp_literal_exec_arg_is_simple(sv_compiler_t *c, sv_ast_t *node) {
  if (!node) return false;
  switch (node->type) {
    case N_STRING:
    case N_NUMBER:
    case N_BOOL:
    case N_NULL:
    case N_UNDEF:
    case N_THIS:
      return true;
    case N_IDENT:
      return resolve_local(c, node->str, node->len) >= 0 ||
             resolve_upvalue(c, node->str, node->len) >= 0;
    default:
      return false;
  }
}

static bool compile_regexp_literal_exec_intrinsic(
  sv_compiler_t *c, sv_ast_t *node, bool has_spread
) {
  if (!node || has_spread || node->args.count != 1) return false;
  if (c->regexp_exec_write_seen) return false;

  sv_ast_t *callee = node->left;
  if (!callee || callee->type != N_MEMBER) return false;
  if ((callee->flags & 1) || !callee->right || !callee->right->str) return false;
  if (!is_ident_str(callee->right->str, callee->right->len, "exec", 4)) return false;
  if (!callee->left || callee->left->type != N_REGEXP) return false;
  if (!regexp_literal_exec_arg_is_simple(c, node->args.items[0])) return false;

  sv_ast_t *rx = callee->left;
  emit_constant(c, sv_const_str(c, rx->str ? rx->str : "", rx->len));
  emit_constant(c, sv_const_str(c, rx->aux ? rx->aux : "", rx->aux_len));
  compile_expr(c, node->args.items[0]);
  emit_op(c, OP_RE_LITERAL_EXEC);
  return true;
}

static bool compile_string_regexp_literal_replace_intrinsic(
  sv_compiler_t *c, sv_ast_t *node, bool has_spread
) {
  if (!node || has_spread || node->args.count != 2) return false;
  if (c->regexp_replace_write_seen || c->regexp_exec_write_seen) return false;

  sv_ast_t *callee = node->left;
  if (!callee || callee->type != N_MEMBER) return false;
  if ((callee->flags & 1) || !callee->right || !callee->right->str) return false;
  if (!is_ident_str(callee->right->str, callee->right->len, "replace", 7)) return false;
  if (!regexp_literal_exec_arg_is_simple(c, callee->left)) return false;

  sv_ast_t *rx = node->args.items[0];
  sv_ast_t *replacement = node->args.items[1];
  if (!rx || rx->type != N_REGEXP || !replacement || replacement->type != N_STRING) return false;
  if (replacement->str && memchr(replacement->str, '$', replacement->len)) return false;

  compile_expr(c, callee->left);
  emit_constant(c, sv_const_str(c, rx->str ? rx->str : "", rx->len));
  emit_constant(c, sv_const_str(c, rx->aux ? rx->aux : "", rx->aux_len));
  compile_expr(c, replacement);
  emit_op(c, OP_STR_RE_LITERAL_REPLACE);
  return true;
}

static bool compile_regexp_exec_truthy_intrinsic(
  sv_compiler_t *c, sv_ast_t *node
) {
  if (!node || node->type != N_CALL || call_has_spread_arg(node) || node->args.count != 1)
    return false;
    
  sv_ast_t *callee = node->left;
  if (!callee || callee->type != N_MEMBER) return false;
  if (member_call_needs_optional_base_guard(callee)) return false;
  if ((callee->flags & 1) || !callee->right || !callee->right->str) return false;
  if (is_ident_name(callee->left, "super")) return false;
  if (!is_ident_str(callee->right->str, callee->right->len, "exec", 4))
    return false;

  compile_expr(c, callee->left);
  compile_receiver_property_get(c, callee);
  compile_expr(c, node->args.items[0]);
  emit_op(c, OP_RE_EXEC_TRUTHY);
  
  return true;
}

bool compile_regexp_exec_discard_intrinsic(
  sv_compiler_t *c, sv_ast_t *node
) {
  if (!node || node->type != N_CALL || call_has_spread_arg(node) ||
      node->args.count != 1)
    return false;

  sv_ast_t *callee = node->left;
  if (!callee || callee->type != N_MEMBER) return false;
  if (member_call_needs_optional_base_guard(callee)) return false;
  if ((callee->flags & 1) || !callee->right || !callee->right->str) return false;
  if (is_ident_name(callee->left, "super")) return false;
  if (!is_ident_str(callee->right->str, callee->right->len, "exec", 4))
    return false;

  compile_expr(c, callee->left);
  compile_receiver_property_get(c, callee);
  compile_expr(c, node->args.items[0]);
  emit_op(c, OP_RE_EXEC_DISCARD);
  return true;
}

void compile_truthy_test_expr(sv_compiler_t *c, sv_ast_t *node) {
  if (compile_regexp_exec_truthy_intrinsic(c, node)) return;
  compile_expr(c, node);
}

static void compile_optional_call_after_setup(
  sv_compiler_t *c, sv_ast_t *call_node,
  sv_call_kind_t kind, bool has_spread
) {
  emit_op(c, OP_DUP);
  emit_op(c, OP_IS_UNDEF_OR_NULL);
  int j_do_call = emit_jump(c, OP_JMP_FALSE);
  emit_op(c, OP_POP);
  if (sv_call_kind_has_receiver(kind))
    emit_op(c, OP_POP);
  emit_op(c, OP_UNDEF);
  int j_end = emit_jump(c, OP_JMP);
  patch_jump(c, j_do_call);
  compile_call_emit_invoke(c, call_node, kind, has_spread);
  patch_jump(c, j_end);
}

static void compile_guarded_member_call(
  sv_compiler_t *c, sv_ast_t *call_node,
  sv_ast_t *member, bool has_spread, bool optional_callee
) {
  compile_expr(c, member->left);
  int end = emit_optional_chain_short_circuit(c);
  compile_receiver_property_get(c, member);
  if (optional_callee)
    compile_optional_call_after_setup(
      c, call_node, SV_CALL_METHOD, has_spread);
  else
    compile_call_emit_invoke(
      c, call_node, SV_CALL_METHOD, has_spread);
  patch_jump(c, end);
}

static void compile_call_optional(
  sv_compiler_t *c, sv_ast_t *node,
  sv_ast_t *opt_callee, bool has_spread
) {
  if (opt_callee->right) {
    compile_guarded_member_call(
      c, node, opt_callee, has_spread, false);
    return;
  }

  sv_ast_t *target = opt_callee->left;
  if (member_call_needs_optional_base_guard(target)) {
    compile_guarded_member_call(
      c, node, target, has_spread, true);
    return;
  }

  sv_call_kind_t kind = compile_call_setup_non_optional(c, target);
  compile_optional_call_after_setup(c, node, kind, has_spread);
}

static bool fusion_arg_is_effect_free(sv_compiler_t *c, sv_ast_t *a) {
  if (!a) return false;
  switch (a->type) {
    case N_NUMBER:
    case N_STRING:
    case N_BIGINT:
    case N_BOOL:
    case N_NULL:
    case N_UNDEF:
      return true;
    case N_IDENT: {
      int l = resolve_local(c, a->str, a->len);
      return l >= 0 && c->locals[l].is_const && !c->locals[l].is_tdz;
    }
    default:
      return false;
  }
}

static int fusion_arg_deferred_slot(sv_compiler_t *c, sv_ast_t *a) {
  if (!a || a->type != N_IDENT) return -1;
  int l = resolve_local(c, a->str, a->len);
  if (l < 0 || c->locals[l].is_tdz || c->locals[l].is_const) return -1;
  int slot = local_to_frame_slot(c, l);
  return slot <= UINT16_MAX ? slot : -1;
}

static bool compile_call_try_fused_chain(
  sv_compiler_t *c, sv_ast_t *node, sv_ast_t *callee, bool has_spread
) {
  if (has_spread || c->with_depth != 0) return false;
  if (callee->type != N_CALL) return false;
  if (call_has_spread_arg(callee)) return false;
  if (callee->args.count != 1) return false;
  if (node->args.count > SV_JIT_ARGS_BUF_CAP - 2) return false;
  if (!callee->left) return false;
  if (callee->left->type == N_MEMBER || callee->left->type == N_OPTIONAL) return false;
  if (is_ident_name(callee->left, "super") || is_ident_name(callee->left, "eval")) return false;
  int deferred_slot = -1;
  if (node->args.count == 1)
    deferred_slot = fusion_arg_deferred_slot(c, node->args.items[0]);
  if (deferred_slot < 0) {
    for (int i = 0; i < node->args.count; i++)
      if (!fusion_arg_is_effect_free(c, node->args.items[i])) return false;
  }

  compile_expr(c, callee->left);
  compile_expr(c, callee->args.items[0]);
  if (deferred_slot >= 0) {
    emit_op(c, OP_CALL_CALL_SLOT);
    emit_u16(c, (uint16_t)deferred_slot);
    return true;
  }
  for (int i = 0; i < node->args.count; i++)
    compile_expr(c, node->args.items[i]);
  emit_op(c, OP_CALL_CALL);
  emit(c, 1);
  emit(c, (uint8_t)node->args.count);
  return true;
}

void compile_call(sv_compiler_t *c, sv_ast_t *node) {
  sv_ast_t *callee = node->left;
  bool has_spread = call_has_spread_arg(node);

  if (callee->type == N_OPTIONAL) {
    compile_call_optional(c, node, callee, has_spread);
    return;
  }

  if (member_call_needs_optional_base_guard(callee)) {
    compile_guarded_member_call(
      c, node, callee, has_spread, false);
    return;
  }

  if (compile_call_is_proto_intrinsic(c, node, has_spread))
    return;

  if (compile_call_array_includes_intrinsic(c, node, has_spread))
    return;

  if (compile_call_char_code_at_intrinsic(c, node, has_spread))
    return;

  if (compile_call_string_intrinsic(c, node, has_spread))
    return;

  if (compile_call_map_template_intrinsic(c, node, has_spread, false))
    return;

  if (compile_call_stable_builtin(c, node, has_spread))
    return;

  if (compile_regexp_literal_exec_intrinsic(c, node, has_spread))
    return;

  if (compile_string_regexp_literal_replace_intrinsic(c, node, has_spread))
    return;

  if (
    !has_spread && node->args.count >= 2 &&
    callee->type == N_MEMBER &&
    is_ident_name(callee->left, "Ant") &&
    resolve_local(c, "Ant", 3) == -1 &&
    callee->right && callee->right->type == N_IDENT &&
    callee->right->len == 5 && memcmp(callee->right->str, "match", 5) == 0 &&
    node->args.items[1]->type == N_OBJECT
  ) {
    sv_ast_t *obj = node->args.items[1];
    sv_ast_t *param = sv_ast_new(c->js, N_IDENT);
    
    param->str = "$"; param->len = 1;
    sv_ast_t *arrow = sv_ast_new(c->js, N_FUNC);
    
    arrow->flags = FN_ARROW;
    arrow->body = obj;
    arrow->line = obj->line; arrow->col = obj->col;
    arrow->src_off = obj->src_off; arrow->src_end = obj->src_end;
    
    sv_ast_list_push(c->js, &arrow->args, param);
    node->args.items[1] = arrow;
  }

  if (compile_direct_eval_call(c, node, has_spread)) return;
  if (compile_call_try_fused_chain(c, node, callee, has_spread)) return;

  sv_call_kind_t kind = compile_call_setup_non_optional(c, callee);
  compile_call_emit_invoke(c, node, kind, has_spread);
}

void compile_new(sv_compiler_t *c, sv_ast_t *node) {
  compile_expr(c, node->left);
  emit_op(c, OP_DUP);
  if (call_has_spread_arg(node)) {
    compile_call_args_array(c, node);
    emit_srcpos(c, node);
    emit_op(c, OP_NEW_APPLY);
    emit_u16(c, 1);
  } else {
    int argc = node->args.count;
    for (int i = 0; i < argc; i++)
      compile_expr(c, node->args.items[i]);
    emit_srcpos(c, node);
    emit_op(c, OP_NEW);
    emit_u16(c, (uint16_t)argc);
  }
}
