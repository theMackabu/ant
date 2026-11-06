#include "compiler_internal.h"

static sv_private_name_t *private_scope_find_current(
  sv_private_scope_t *scope, const char *name, uint32_t len
) {
  if (!scope || !name) return NULL;
  for (int i = 0; i < scope->count; i++) {
    sv_private_name_t *p = &scope->names[i];
    if (p->len == len && memcmp(p->name, name, len) == 0) return p;
  }
  return NULL;
}

static sv_private_name_t *private_scope_resolve(
  sv_compiler_t *c, const char *name, uint32_t len
) {
  for (sv_private_scope_t *scope = c->private_scope; scope; scope = scope->parent) {
    sv_private_name_t *p = private_scope_find_current(scope, name, len);
    if (p) return p;
  }
  return NULL;
}

static bool private_scope_add(
  sv_compiler_t *c, sv_private_scope_t *scope,
  sv_ast_t *name, uint8_t kind, bool is_static
) {
  if (!is_private_name_node(name)) return true;

  if (name->len == 12 && memcmp(name->str, "#constructor", 12) == 0) {
    js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Classes may not declare private constructor names");
    return false;
  }

  sv_private_name_t *existing = private_scope_find_current(scope, name->str, name->len);
  bool is_accessor = kind == SV_COMP_PRIVATE_GETTER || kind == SV_COMP_PRIVATE_SETTER;
  if (existing) {
    bool existing_accessor = existing->kind == SV_COMP_PRIVATE_GETTER ||
      existing->kind == SV_COMP_PRIVATE_SETTER;
    if (!is_accessor || !existing_accessor || existing->is_static != is_static) {
      js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Duplicate private name '%.*s'", (int)name->len, name->str);
      return false;
    }
    if ((kind == SV_COMP_PRIVATE_GETTER && existing->has_getter) ||
        (kind == SV_COMP_PRIVATE_SETTER && existing->has_setter)) {
      js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Duplicate private accessor '%.*s'", (int)name->len, name->str);
      return false;
    }
    if (kind == SV_COMP_PRIVATE_GETTER) existing->has_getter = true;
    if (kind == SV_COMP_PRIVATE_SETTER) existing->has_setter = true;
    return true;
  }

  if (scope->count >= scope->cap) {
    int new_cap = scope->cap ? scope->cap * 2 : 8;
    sv_private_name_t *new_names = realloc(scope->names, (size_t)new_cap * sizeof(*scope->names));
    if (!new_names) return false;
    scope->names = new_names;
    scope->cap = new_cap;
  }

  sv_private_name_t *p = &scope->names[scope->count++];
  
  uint64_t hash64 = hash_key(name->str, (size_t)name->len);
  uint32_t hash = (uint32_t)(hash64 ^ (hash64 >> 32));
  
  *p = (sv_private_name_t){
    .name = name->str,
    .len = name->len,
    .kind = kind,
    .hash = hash,
    .is_static = is_static,
    .has_getter = kind == SV_COMP_PRIVATE_GETTER,
    .has_setter = kind == SV_COMP_PRIVATE_SETTER,
    .owner = NULL,
    .local = -1
  };
  
  return true;
}

static int resolve_private_upvalue(sv_compiler_t *c, sv_private_name_t *p) {
  if (!c->enclosing || !p || !p->owner || p->local < 0) return -1;

  if (c->enclosing == p->owner) {
    p->owner->locals[p->local].captured = true;
    uint16_t slot = (uint16_t)local_to_frame_slot(p->owner, p->local);
    sv_binding_meta_t binding = {.may_tdz = p->owner->locals[p->local].is_tdz};
    return add_upvalue_binding(c, slot, true, false, false, true, &binding);
  }

  int upvalue = resolve_private_upvalue(c->enclosing, p);
  if (upvalue == -1) return -1;
  
  const sv_upval_desc_t *outer = &c->enclosing->upval_descs[upvalue];
  sv_binding_meta_t binding = {.may_tdz = outer->may_tdz};
  
  return add_upvalue_binding(c, (uint16_t)upvalue, false, false, false, outer->write_once, &binding);
}

bool emit_private_token(sv_compiler_t *c, sv_ast_t *name) {
  sv_private_name_t *p = private_scope_resolve(c, name->str, name->len);
  if (!p) {
    js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Private name '%.*s' is not declared", (int)name->len, name->str);
    emit_op(c, OP_UNDEF);
    return false;
  }
  if (!p->owner || p->local < 0) {
    js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Private name '%.*s' is not initialized", (int)name->len, name->str);
    emit_op(c, OP_UNDEF);
    return false;
  }
  if (p->owner == c) {
    emit_get_local(c, p->local);
    return true;
  }
  int upvalue = resolve_private_upvalue(c, p);
  if (upvalue == -1) {
    js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Private name '%.*s' is not in scope", (int)name->len, name->str);
    emit_op(c, OP_UNDEF);
    return false;
  }
  emit_op(c, OP_GET_UPVAL);
  emit_u16(c, (uint16_t)upvalue);
  return true;
}

static void compile_static_initializer_value(sv_compiler_t *c, sv_ast_t *expr, int ctor_local);

void compile_method_function(sv_compiler_t *c, sv_ast_t *m) {
  const char *saved_name = c->inferred_name;
  uint32_t saved_len = c->inferred_name_len;
  
  bool named = 
    !(m->flags & (FN_COMPUTED | FN_GETTER | FN_SETTER)) 
    && m->left && m->left->str && m->left->len;
  
  if (named) {
    c->inferred_name = m->left->str;
    c->inferred_name_len = m->left->len;
  }
  
  compile_func_expr(c, m->right);
  c->inferred_name = saved_name;
  c->inferred_name_len = saved_len;
}

static void compile_class_method(
  sv_compiler_t *c, sv_ast_t *m,
  int ctor_local, int proto_local,
  int preeval_key
) {
  bool is_static = !!(m->flags & FN_STATIC);
  int home_local = is_static ? ctor_local : proto_local;
  bool is_fn = is_class_method_def(m);

  if (is_fn) {
    if (is_static) m->right->flags |= FN_STATIC;
    compile_method_function(c, m);
    emit_get_local(c, home_local);    
    emit_op(c, OP_SET_HOME_OBJ);      
    emit_op(c, OP_SWAP);              
  } else {
    emit_get_local(c, home_local);    
    if (is_static) compile_static_initializer_value(c, m->right, ctor_local);
    else if (m->right) compile_expr(c, m->right);
    else emit_op(c, OP_UNDEF);        
  }

  uint8_t method_flags = 0;
  if (m->flags & FN_GETTER) method_flags |= SV_DEFINE_METHOD_GETTER;
  if (m->flags & FN_SETTER) method_flags |= SV_DEFINE_METHOD_SETTER;
  if (is_fn) method_flags |= SV_DEFINE_METHOD_SET_NAME;
  if (is_fn || (m->flags & (FN_GETTER | FN_SETTER))) method_flags |= SV_DEFINE_METHOD_NON_ENUM;

  if (m->flags & FN_COMPUTED) {
    if (preeval_key >= 0) emit_get_local(c, preeval_key);
    else compile_expr(c, m->left);
  } else compile_static_property_key(c, m->left); 
  
  emit_op(c, OP_SWAP);                          
  emit_op(c, OP_DEFINE_METHOD_COMP);
  emit(c, method_flags);                        
  emit_op(c, OP_POP);
}

static void compile_private_static_element(sv_compiler_t *c, sv_ast_t *m, int ctor_local) {
  bool is_fn = is_class_method_def(m);
  emit_get_local(c, ctor_local);
  emit_private_token(c, m->left);
  if (is_fn) {
    if (m->flags & FN_STATIC) m->right->flags |= FN_STATIC;
    compile_method_function(c, m);
  } else if ((m->flags & FN_STATIC)) compile_static_initializer_value(c, m->right, ctor_local);
  else if (m->right) compile_expr(c, m->right);
  else emit_op(c, OP_UNDEF);

  emit_op(c, OP_DEF_PRIVATE);
  if (m->flags & FN_GETTER) emit(c, SV_COMP_PRIVATE_GETTER);
  else if (m->flags & FN_SETTER) emit(c, SV_COMP_PRIVATE_SETTER);
  else emit(c, is_fn ? SV_COMP_PRIVATE_METHOD : SV_COMP_PRIVATE_FIELD);
  emit_op(c, OP_POP);
}

static void compile_class_element_strict(
  sv_compiler_t *c, sv_ast_t *m,
  int ctor_local, int proto_local,
  int preeval_key
) {
  bool saved_strict = c->is_strict;
  c->is_strict = true;
  if (is_private_name_node(m->left)) {
    compile_private_static_element(c, m, ctor_local);
  } else {
    compile_class_method(c, m, ctor_local, proto_local, preeval_key);
  }
  c->is_strict = saved_strict;
}

typedef struct {
  sv_ast_t *node;
  int preeval_key;
} sv_class_emit_item_t;

static inline int compile_class_precompute_key(sv_compiler_t *c, sv_ast_t *key_expr) {
  compile_expr(c, key_expr);
  int loc = add_local(c, "", 0, false, c->scope_depth);
  emit_put_local(c, loc);
  return loc;
}

static int compile_static_child_function(sv_compiler_t *c, sv_ast_t *node, bool returns_expr) {
  sv_compiler_t comp;
  sv_compile_ctx_init_child(&comp, c, NULL, SV_COMPILE_SCRIPT);
  comp.is_strict = true;

  comp.super_local = add_hidden_local(&comp);
  emit_op(&comp, OP_SPECIAL_OBJ);
  emit(&comp, SV_SPECIAL_SUPER);
  emit_put_local(&comp, comp.super_local);

  if (returns_expr) {
    if (node) compile_expr(&comp, node);
    else emit_op(&comp, OP_UNDEF);
    emit_return_from_stack(&comp);
  } else {
    check_statement_list_declarations(&comp, &node->args, false, NULL, true);
    compile_stmts(&comp, &node->args);
    emit_close_upvals(&comp);
    emit_op(&comp, OP_RETURN_UNDEF);
  }

  sv_func_t *fn = sv_func_new(c->js);
  sv_func_init_code_and_map_templates(&comp, fn);
  sv_func_init_obj_sites(&comp, fn);

  if (comp.const_count > 0) {
    fn->constants = sv_code_bump(c->js, (size_t)comp.const_count * sizeof(ant_value_t));
    memcpy(fn->constants, comp.constants, (size_t)comp.const_count * sizeof(ant_value_t));
    fn->const_count = comp.const_count;
    if (!build_gc_const_tables(comp.js, fn)) js_mkerr(comp.js, "out of memory building constant tables");
  }
  if (comp.atom_count > 0) {
    fn->atoms = sv_code_bump(c->js, (size_t)comp.atom_count * sizeof(sv_atom_t));
    memcpy(fn->atoms, comp.atoms, (size_t)comp.atom_count * sizeof(sv_atom_t));
    fn->atom_count = comp.atom_count;
  }
  fn->ic_count = (uint16_t)comp.ic_count;
  fn->ic_slots = alloc_ic_slots(c->js, fn->ic_count);
  if (comp.upvalue_count > 0) {
    fn->upval_descs = sv_code_bump(c->js, (size_t)comp.upvalue_count * sizeof(sv_upval_desc_t));
    memcpy(fn->upval_descs, comp.upval_descs, (size_t)comp.upvalue_count * sizeof(sv_upval_desc_t));
    fn->upvalue_count = comp.upvalue_count;
  }
  if (comp.srcpos_count > 0) {
    fn->debug->srcpos = sv_code_bump(c->js, (size_t)comp.srcpos_count * sizeof(sv_srcpos_t));
    memcpy(fn->debug->srcpos, comp.srcpos, (size_t)comp.srcpos_count * sizeof(sv_srcpos_t));
    fn->debug->srcpos_count = comp.srcpos_count;
  }

  fn->max_locals = comp.max_local_count;
  sv_func_finalize_type_data(fn, &comp, fn->max_locals);
  fn->param_count = 0;
  fn->function_length = 0;
  fn->allows_new_target = true;
  fn->is_strict = true;
  fn->is_static = true;
  fn->debug->filename = c->filename ? c->filename : c->js->filename;
  fn->debug->source_line = node ? (int)node->line : 0;

  sv_compile_ctx_cleanup(&comp);

  return add_constant(c, mkref(kTypeFunctionInfo, fn));
}

static void emit_static_child_call(sv_compiler_t *c, int func_idx, int ctor_local) {
  emit_closure(c, func_idx);
  emit_get_local(c, ctor_local);
  emit_op(c, OP_SET_HOME_OBJ);
  emit_op(c, OP_SWAP);
  emit_op(c, OP_CALL_METHOD);
  emit_u16(c, 0);
}

static void compile_static_initializer_value(sv_compiler_t *c, sv_ast_t *expr, int ctor_local) {
  int idx = compile_static_child_function(c, expr, true);
  emit_static_child_call(c, idx, ctor_local);
}

static void compile_static_block(sv_compiler_t *c, sv_ast_t *block, int ctor_local) {
  int idx = compile_static_child_function(c, block, false);
  emit_static_child_call(c, idx, ctor_local);
  emit_op(c, OP_POP);
}

void compile_class(sv_compiler_t *c, sv_ast_t *node) {
  int outer_name_local = -1;
  bool binds_outer_name = node->str &&
    (node->flags & FN_CLASS_DECL);
  bool class_global_lexical = binds_outer_name && is_repl_top_level(c);

  sv_ast_t *ctor_method = NULL;
  bool has_static_name = false;

  int field_count = 0;
  int method_emit_count = 0;
  int static_init_count = 0;

  if (binds_outer_name)
    outer_name_local = resolve_local(c, node->str, node->len);
  if (node->left) compile_expr(c, node->left);
  else emit_op(c, OP_UNDEF);

  sv_private_scope_t private_scope = { .parent = c->private_scope };
  sv_private_scope_t *saved_private_scope = c->private_scope;
  c->private_scope = &private_scope;

  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *m = node->args.items[i];
    if (m->type == N_STATIC_BLOCK) {
      static_init_count++;
      continue;
    }
    if (m->type != N_METHOD) continue;
    bool is_fn = is_class_method_def(m);
    bool is_private = is_private_name_node(m->left);

    if (is_private) {
      uint8_t private_kind = (m->flags & FN_GETTER) ? SV_COMP_PRIVATE_GETTER :
        (m->flags & FN_SETTER) ? SV_COMP_PRIVATE_SETTER :
        is_fn ? SV_COMP_PRIVATE_METHOD : SV_COMP_PRIVATE_FIELD;
      if (!private_scope_add(c, &private_scope, m->left, private_kind, !!(m->flags & FN_STATIC))) {
        c->private_scope = saved_private_scope;
        free(private_scope.names);
        emit_op(c, OP_UNDEF);
        return;
      }
    }
    
    if (
      !(m->flags & FN_STATIC) &&
      !(m->flags & FN_COMPUTED) &&
      m->left && m->left->type == N_IDENT &&
      m->left->len == 11 &&
      memcmp(m->left->str, "constructor", 11) == 0
    ) { ctor_method = m; continue; }
    
    if (
      (m->flags & FN_STATIC) &&
      !(m->flags & FN_COMPUTED) &&
      m->left && m->left->str &&
      m->left->len == 4 &&
      memcmp(m->left->str, "name", 4) == 0
    ) has_static_name = true;
    
    if (!(m->flags & FN_STATIC) && (is_private || !is_fn)) field_count++;
    else if (is_fn && (!is_private || (m->flags & FN_STATIC))) method_emit_count++;
    else if (m->flags & FN_STATIC) static_init_count++;
  }

  sv_ast_t **field_inits = NULL;
  int *computed_key_locals = NULL;
  
  sv_class_emit_item_t *method_emit_items = NULL;
  sv_class_emit_item_t *static_init_items = NULL;
  
  if (field_count > 0) {
    field_inits = malloc(sizeof(sv_ast_t *) * field_count);
    computed_key_locals = malloc(sizeof(int) * field_count);
  }
  if (method_emit_count > 0)
    method_emit_items = malloc(sizeof(sv_class_emit_item_t) * method_emit_count);
  if (static_init_count > 0)
    static_init_items = malloc(sizeof(sv_class_emit_item_t) * static_init_count);

  int fi = 0, mi = 0, si = 0;
  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *m = node->args.items[i];
    if (m->type == N_STATIC_BLOCK) {
      static_init_items[si++] = (sv_class_emit_item_t){ m, -1 };
      continue;
    }
    if (m->type != N_METHOD || m == ctor_method) continue;
    
    bool is_fn = is_class_method_def(m);
    bool is_private = is_private_name_node(m->left);
    bool is_static = !!(m->flags & FN_STATIC);
    bool needs_instance_init = !is_static && (is_private || !is_fn);
    
    if (needs_instance_init) {
      field_inits[fi] = m;
      computed_key_locals[fi++] = (!is_private && (m->flags & FN_COMPUTED))
        ? compile_class_precompute_key(c, m->left) : -1;
      continue;
    }

    bool needs_preeval_key = !is_private && node->str && (m->flags & FN_COMPUTED) &&
      (is_fn || is_static);
    int preeval_key = needs_preeval_key ? compile_class_precompute_key(c, m->left) : -1;

    if (is_fn && (!is_private || is_static)) {
      method_emit_items[mi++] = (sv_class_emit_item_t){ m, preeval_key };
      continue;
    }
    if (is_static) static_init_items[si++] = (sv_class_emit_item_t){ m, preeval_key };
  }

  int inner_name_local = -1;
  bool has_class_scope = node->str || private_scope.count > 0;
  if (has_class_scope) {
    begin_scope(c);
    if (node->str)
      inner_name_local = add_local(c, node->str, node->len, true, c->scope_depth);
    for (int i = 0; i < private_scope.count; i++) {
      sv_private_name_t *p = &private_scope.names[i];
      p->owner = c;
      p->local = add_local(c, "", 0, true, c->scope_depth);
      emit_op(c, OP_PRIVATE_TOKEN);
      emit_u32(c, p->hash);
      emit_put_local(c, p->local);
    }
  }

  if (ctor_method && ctor_method->right) {
    ctor_method->right->flags |= FN_CLASS_CTOR;
    if (node->left) ctor_method->right->flags |= FN_DERIVED_CTOR;
    c->field_inits = field_inits;
    c->field_init_count = field_count;
    c->computed_key_locals = computed_key_locals;
    const char *saved_name = c->inferred_name;
    uint32_t saved_len = c->inferred_name_len;
    if (node->str && node->len) {
      c->inferred_name = node->str;
      c->inferred_name_len = node->len;
    }
    compile_func_expr(c, ctor_method->right);
    c->inferred_name = saved_name;
    c->inferred_name_len = saved_len;
    c->field_inits = NULL;
    c->field_init_count = 0;
    c->computed_key_locals = NULL;
  } else if (field_count > 0) {
    c->computed_key_locals = computed_key_locals;
    sv_compiler_t comp;
    sv_compile_ctx_init_child(&comp, c, NULL, SV_COMPILE_SCRIPT);
    comp.is_strict = true;

    emit_op(&comp, OP_CHECK_CTOR);

    if (node->left) {
      emit_op(&comp, OP_THIS);
      emit_op(&comp, OP_SPECIAL_OBJ);
      emit(&comp, SV_SPECIAL_SUPER);
      emit_op(&comp, OP_SPECIAL_OBJ);
      emit(&comp, SV_SPECIAL_NEW_TARGET);
      emit_op(&comp, OP_SUPER_FORWARD);
      emit_op(&comp, OP_POP);
    }

    emit_field_inits(&comp, c, field_inits, field_count);
    emit_op(&comp, OP_RETURN_UNDEF);

    sv_func_t *fn = sv_func_new(c->js);
    fn->is_derived_ctor = node->left != NULL;
    fn->is_class_ctor = true;
    sv_func_init_code_and_map_templates(&comp, fn);
    sv_func_init_obj_sites(&comp, fn);
    if (comp.const_count > 0) {
      fn->constants = sv_code_bump(c->js, (size_t)comp.const_count * sizeof(ant_value_t));
      memcpy(fn->constants, comp.constants, (size_t)comp.const_count * sizeof(ant_value_t));
      fn->const_count = comp.const_count;
      if (!build_gc_const_tables(comp.js, fn)) js_mkerr(comp.js, "out of memory building constant tables");
    }
    if (comp.atom_count > 0) {
      fn->atoms = sv_code_bump(c->js, (size_t)comp.atom_count * sizeof(sv_atom_t));
      memcpy(fn->atoms, comp.atoms, (size_t)comp.atom_count * sizeof(sv_atom_t));
      fn->atom_count = comp.atom_count;
    }
    fn->ic_count = (uint16_t)comp.ic_count;
    fn->ic_slots = alloc_ic_slots(c->js, fn->ic_count);
    if (comp.upvalue_count > 0) {
      fn->upval_descs = sv_code_bump(c->js, (size_t)comp.upvalue_count * sizeof(sv_upval_desc_t));
      memcpy(fn->upval_descs, comp.upval_descs, (size_t)comp.upvalue_count * sizeof(sv_upval_desc_t));
      fn->upvalue_count = comp.upvalue_count;
    }
    fn->max_locals = comp.max_local_count;
    sv_func_finalize_type_data(fn, &comp, fn->max_locals);
    
    fn->param_count = (uint16_t)comp.param_count;
    fn->function_length = (uint16_t)comp.param_count;
    fn->is_strict = comp.is_strict;
    fn->allows_new_target = comp.allows_new_target;
    fn->debug->filename = c->js->filename;
    fn->debug->source_line = (int)node->line;
    
    if (node->str && node->len > 0) {
      char *name = sv_code_bump(c->js, node->len + 1);
      memcpy(name, node->str, node->len);
      name[node->len] = '\0';
      fn->debug->name = name;
    }
    
    sv_compile_ctx_cleanup(&comp);
    int idx = add_constant(c, mkref(kTypeFunctionInfo, fn));
    emit_closure(c, idx);
  } else emit_op(c, OP_UNDEF);
  
  free(field_inits);
  free(computed_key_locals);
  c->computed_key_locals = NULL;

  const char *class_name = node->str;
  uint32_t class_name_len = node->len;
  bool consumed_inferred_name = false;
  
  if (!class_name && c->inferred_name) {
    class_name = c->inferred_name;
    class_name_len = c->inferred_name_len;
    consumed_inferred_name = true;
  }

  if (!has_static_name) {
    int atom = class_name
      ? add_atom(c, class_name, class_name_len)
      : add_atom(c, "", 0);
    emit_op(c, OP_DEFINE_CLASS);
    emit_u32(c, (uint32_t)atom);
    emit(c, SV_CLASS_FLAG_HAS_NAME |
      (node->left ? SV_CLASS_FLAG_HAS_HERITAGE : 0));
  } else {
    emit_op(c, OP_DEFINE_CLASS);
    emit_u32(c, 0);
    emit(c, node->left ? SV_CLASS_FLAG_HAS_HERITAGE : 0);
  }
  
  if (consumed_inferred_name) {
    c->inferred_name = NULL;
    c->inferred_name_len = 0;
  }
  
  emit_u32(c, node->src_off);
  emit_u32(c, node->src_end);

  int proto_local = add_local(c, "", 0, false, c->scope_depth);
  int ctor_local = add_local(c, "", 0, false, c->scope_depth);
  emit_put_local(c, proto_local);
  emit_put_local(c, ctor_local);

  if (inner_name_local >= 0) {
    emit_get_local(c, ctor_local);
    emit_put_local(c, inner_name_local);
  }

  for (int i = 0; i < method_emit_count; i++) {
    sv_class_emit_item_t *item = &method_emit_items[i];
    compile_class_element_strict(
      c, item->node, ctor_local, proto_local, item->preeval_key
    );
  }

  for (int i = 0; i < static_init_count; i++) {
    sv_ast_t *m = static_init_items[i].node;
    if (m->type == N_STATIC_BLOCK) {
      compile_static_block(c, m, ctor_local);
      continue;
    }

    compile_class_element_strict(
      c, m, ctor_local, proto_local, static_init_items[i].preeval_key
    );
  }

  free(method_emit_items);
  free(static_init_items);
  emit_get_local(c, ctor_local);

  if (class_global_lexical) {
    emit_op(c, OP_DUP);
    emit_atom_op(c, OP_INIT_GLOBAL_LEX, node->str, node->len);
  } else if (outer_name_local >= 0) {
    emit_op(c, OP_DUP);
    emit_put_local(c, outer_name_local);
    c->locals[outer_name_local].is_tdz = false;
    emit_local_export_writes(c, outer_name_local);
  }

  if (has_class_scope) end_scope(c);
  c->private_scope = saved_private_scope;
  free(private_scope.names);
}
