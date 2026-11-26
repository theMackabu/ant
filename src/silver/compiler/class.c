#include "private/codegen.h"

static sv_private_name_t *private_scope_find_current(
  sv_private_scope_t *scope, const char *name, uint32_t len
) {
  if (!scope || !name) return NULL;
  if (scope->index) {
    uint32_t mask = (uint32_t)scope->index_cap - 1;
    for (uint32_t i = sv_compile_ctx_hash_local_name(name, len) & mask;; i = (i + 1) & mask) {
      int entry = scope->index[i];
      if (entry < 0) return NULL;
      sv_private_name_t *p = &scope->names[entry];
      if (p->len == len && memcmp(p->name, name, len) == 0) return p;
    }
  }
  for (int i = 0; i < scope->count; i++) {
    sv_private_name_t *p = &scope->names[i];
    if (p->len == len && memcmp(p->name, name, len) == 0) return p;
  }
  return NULL;
}

static void private_scope_index_insert(sv_private_scope_t *scope, int entry) {
  uint32_t mask = (uint32_t)scope->index_cap - 1;
  const sv_private_name_t *p = &scope->names[entry];
  uint32_t i = sv_compile_ctx_hash_local_name(p->name, p->len) & mask;
  while (scope->index[i] >= 0) i = (i + 1) & mask;
  scope->index[i] = entry;
}

static void private_scope_index_add(sv_private_scope_t *scope) {
  if (scope->count < 16) return;
  if (scope->index && scope->count * 2 < scope->index_cap) {
    private_scope_index_insert(scope, scope->count - 1);
    return;
  }

  int cap = scope->index_cap ? scope->index_cap * 2 : 64;
  int *index = realloc(scope->index, (size_t)cap * sizeof(*index));
  if (!index) return;
  memset(index, 0xff, (size_t)cap * sizeof(*index));
  scope->index = index;
  scope->index_cap = cap;
  for (int i = 0; i < scope->count; i++) private_scope_index_insert(scope, i);
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
    sv_private_name_t *new_names = Silver_Compiler_Grow(c, scope->names, &scope->cap, scope->count + 1, 8, sizeof(*scope->names));
    if (!new_names) return false;
    scope->names = new_names;
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
    .local = -1,
    .method_local = -1,
    .slot = -1,
    .method_slot = -1,
    .scope = scope
  };
  
  private_scope_index_add(scope);
  return true;
}

static inline bool private_name_is_instance_method(const sv_private_name_t *p) {
  return p->kind == SV_COMP_PRIVATE_METHOD && !p->is_static;
}

static uint32_t private_brand_hash(uint32_t method_hash, uint32_t class_off) {
  return (method_hash * 0x9E3779B1u) ^ class_off;
}

static bool private_token_is_solo(const sv_private_scope_t *scope, bool is_static) {
  int entries = 0;
  bool branded = false;
  for (int i = 0; i < scope->count; i++) {
    const sv_private_name_t *p = &scope->names[i];
    if (p->is_static != is_static || (private_name_is_instance_method(p) && branded)) continue;
    branded |= private_name_is_instance_method(p);
    entries++;
  }
  return entries == 1;
}

static int resolve_private_upvalue(sv_compiler_t *c, sv_compiler_t *owner, int local) {
  if (!c->enclosing || !owner || local < 0) return -1;

  if (c->enclosing == owner) {
    owner->locals[local].captured = true;
    uint16_t slot = (uint16_t)local_to_frame_slot(owner, local);
    sv_binding_meta_t binding = {.may_tdz = owner->locals[local].is_tdz};
    return add_upvalue_binding(c, slot, true, false, false, true, &binding);
  }

  int upvalue = resolve_private_upvalue(c->enclosing, owner, local);
  if (upvalue == -1) return -1;
  
  const sv_upval_desc_t *outer = &c->enclosing->upval_descs[upvalue];
  sv_binding_meta_t binding = {.may_tdz = outer->may_tdz};
  
  return add_upvalue_binding(c, (uint16_t)upvalue, false, false, false, outer->write_once, &binding);
}

static const char *private_method_binding_name(sv_compiler_t *c, const char *name, uint32_t len) {
  char *buf = malloc((size_t)len + 2);
  if (!buf) {
    Silver_Compiler_RaiseOutOfMemory(c);
    return NULL;
  }
  memcpy(buf, name, len);
  memcpy(buf + len, "()", 2);
  const char *interned = intern_string(c->js, buf, (size_t)len + 2);
  free(buf);
  return interned;
}

typedef enum {
  EVAL_PRIVATE_NONE,
  EVAL_PRIVATE_TOKEN,
  EVAL_PRIVATE_METHOD,
} eval_private_kind_t;

static eval_private_kind_t eval_private_kind(sv_compiler_t *c, const char *name, uint32_t len) {
  sv_compiler_t *root = c;
  while (root->enclosing) root = root->enclosing;
  if (!sv_compile_mode_is_eval(root->mode) || !is_object_type(root->eval_env)) return EVAL_PRIVATE_NONE;
  if (!sv_eval_env_shadows_global(c->js, root->eval_env, name, len)) return EVAL_PRIVATE_NONE;

  const char *method = private_method_binding_name(c, name, len);
  return method && sv_eval_env_shadows_global(c->js, root->eval_env, method, len + 2)
    ? EVAL_PRIVATE_METHOD : EVAL_PRIVATE_TOKEN;
}

bool private_name_resolvable(sv_compiler_t *c, sv_ast_t *name) {
  return
    private_scope_resolve(c, name->str, name->len) ||
    eval_private_kind(c, name->str, name->len) != EVAL_PRIVATE_NONE;
}

static bool private_binding_location(
  sv_compiler_t *c, sv_compiler_t *owner, int local, uint8_t *kind, uint16_t *index
) {
  if (!owner || local < 0) return false;
  if (owner == c) {
    owner->locals[local].captured = true;
    *kind = SV_EVAL_BIND_LOCAL;
    *index = (uint16_t)(local - c->param_locals);
    return true;
  }

  int upvalue = resolve_private_upvalue(c, owner, local);
  if (upvalue < 0) return false;
  *kind = SV_EVAL_BIND_UPVALUE;
  *index = (uint16_t)upvalue;
  return true;
}

int private_eval_binding_count(const sv_compiler_t *c) {
  int count = 0;
  for (sv_private_scope_t *scope = c->private_scope; scope; scope = scope->parent)
    count += scope->count * 2;
  return count;
}

bool private_eval_bindings_each(sv_compiler_t *c, private_eval_binding_fn fn, void *ctx) {
  for (sv_private_scope_t *scope = c->private_scope; scope; scope = scope->parent) {
  for (int i = 0; i < scope->count; i++) {
    sv_private_name_t *p = &scope->names[i];
    sv_private_name_t *token = private_name_is_instance_method(p) ? &scope->brand : p;
    uint8_t kind;
    uint16_t index;

    if (token->slot >= 0 || !private_binding_location(c, token->owner, token->local, &kind, &index)) continue;
    if (!fn(ctx, p->name, p->len, kind, index)) return false;
    if (token == p || p->method_local < 0 || p->method_slot >= 0) continue;

    const char *method = private_method_binding_name(c, p->name, p->len);
    if (!method || !private_binding_location(c, p->owner, p->method_local, &kind, &index)) continue;
    if (!fn(ctx, method, p->len + 2, kind, index)) return false;
  }}
  return true;
}

static void emit_private_table_load(sv_compiler_t *c, int slot) {
  if (slot < 0) return;
  emit_number(c, (double)slot);
  emit_op(c, OP_GET_ELEM);
}

static void emit_private_table_store(sv_compiler_t *c, int table_local, int slot) {
  emit_get_local(c, table_local);
  emit_op(c, OP_SWAP);
  emit_number(c, (double)slot);
  emit_op(c, OP_SWAP);
  emit_op(c, OP_PUT_ELEM);
}

bool emit_private_token(sv_compiler_t *c, sv_ast_t *name) {
  sv_private_name_t *p = private_scope_resolve(c, name->str, name->len);
  if (!p && eval_private_kind(c, name->str, name->len) != EVAL_PRIVATE_NONE) {
    emit_atom_op(c, OP_GET_EVAL_GLOBAL, name->str, name->len);
    return true;
  }
  if (!p) {
    js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Private name '%.*s' is not declared", (int)name->len, name->str);
    emit_op(c, OP_UNDEF);
    return false;
  }
  if (private_name_is_instance_method(p)) p = &p->scope->brand;
  if (!p->owner || p->local < 0) {
    js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Private name '%.*s' is not initialized", (int)name->len, name->str);
    emit_op(c, OP_UNDEF);
    return false;
  }
  if (p->owner == c) {
    emit_get_local(c, p->local);
    emit_private_table_load(c, p->slot);
    return true;
  }
  int upvalue = resolve_private_upvalue(c, p->owner, p->local);
  if (upvalue == -1) {
    js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Private name '%.*s' is not in scope", (int)name->len, name->str);
    emit_op(c, OP_UNDEF);
    return false;
  }
  emit_op(c, OP_GET_UPVAL);
  emit_u16(c, (uint16_t)upvalue);
  emit_private_table_load(c, p->slot);
  return true;
}

static void compile_static_initializer_value(sv_compiler_t *c, sv_ast_t *expr, const sv_ast_t *key, int key_local, int ctor_local);

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
    if (is_static) compile_static_initializer_value(c, m->right, field_static_key(m), preeval_key, ctor_local);
    else if (m->right) compile_expr(c, m->right);
    else emit_op(c, OP_UNDEF);        
  }

  uint8_t method_flags = 0;
  if (m->flags & FN_GETTER) method_flags |= SV_DEFINE_METHOD_GETTER;
  if (m->flags & FN_SETTER) method_flags |= SV_DEFINE_METHOD_SETTER;
  if (is_fn) method_flags |= SV_DEFINE_METHOD_SET_NAME;
  else method_flags |= field_define_flags(m);
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
    if (class_method_needs_home(m)) {
      emit_get_local(c, ctor_local);
      emit_op(c, OP_SET_HOME_OBJ);
      emit_op(c, OP_POP);
    }
  } else if ((m->flags & FN_STATIC)) compile_static_initializer_value(c, m->right, m->left, -1, ctor_local);
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
  emit_op(c, OP_TO_PROPKEY);
  int loc = add_local(c, "", 0, false, c->scope_depth);
  emit_put_local(c, loc);
  return loc;
}

static int private_brand_method_local(sv_compiler_t *c, sv_ast_t *m) {
  sv_private_name_t *p = private_scope_find_current(c->private_scope, m->left->str, m->left->len);
  return p && private_name_is_instance_method(p) ? p->method_local : -1;
}

static int reserve_private_method_local(sv_compiler_t *c) {
  int loc = add_local(c, "", 0, false, c->scope_depth);
  emit_op(c, OP_UNDEF);
  emit_put_local(c, loc);
  return loc;
}

static void emit_private_method_value(sv_compiler_t *c, const sv_private_name_t *p) {
  if (p->owner == c) {
    emit_get_local(c, p->method_local);
    emit_private_table_load(c, p->method_slot);
    return;
  }
  
  int upvalue = resolve_private_upvalue(c, p->owner, p->method_local);
  if (upvalue == -1) {
    js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Private name '%.*s' is not in scope", (int)p->len, p->name);
    emit_op(c, OP_UNDEF);
    return;
  }
  
  emit_op(c, OP_GET_UPVAL);
  emit_u16(c, (uint16_t)upvalue);
  emit_private_table_load(c, p->method_slot);
}

void emit_private_get(sv_compiler_t *c, sv_ast_t *name, sv_op_t op) {
  sv_private_name_t *p = private_scope_resolve(c, name->str, name->len);
  bool eval_method = !p && eval_private_kind(c, name->str, name->len) == EVAL_PRIVATE_METHOD;
  bool method = eval_method || (p && private_name_is_instance_method(p) && p->method_local >= 0);
  int done = -1;

  if (method && op == OP_GET_PRIVATE_OPT) {
    int present = emit_jump(c, OP_JMP_NOT_NULLISH);
    emit_op(c, OP_POP);
    emit_op(c, OP_UNDEF);
    done = emit_jump(c, OP_JMP);
    patch_jump(c, present);
    op = OP_GET_PRIVATE;
  }

  if (!emit_private_token(c, name)) {
    emit_op(c, OP_POP);
    if (done >= 0) patch_jump(c, done);
    return;
  }
  
  emit_op(c, op);
  if (!method) return;
  
  emit_op(c, OP_POP);
  if (eval_method) {
    const char *binding = private_method_binding_name(c, name->str, name->len);
    if (binding) emit_atom_op(c, OP_GET_EVAL_GLOBAL, binding, name->len + 2);
    else emit_op(c, OP_UNDEF);
  } else emit_private_method_value(c, p);
  if (done >= 0) patch_jump(c, done);
}

bool private_name_uses_brand(sv_compiler_t *c, sv_ast_t *name) {
  sv_private_name_t *p = private_scope_resolve(c, name->str, name->len);
  if (!p) return eval_private_kind(c, name->str, name->len) == EVAL_PRIVATE_METHOD;
  return private_name_is_instance_method(p);
}

static void compile_private_instance_methods(
  sv_compiler_t *c, sv_ast_t **fields, int count, int methods_local, int method_count, int proto_local
) {
  for (int i = 0; i < count; i++) {
    sv_ast_t *m = fields[i];
    if (!is_class_method_def(m)) continue;
    
    sv_private_name_t *p = private_scope_find_current(c->private_scope, m->left->str, m->left->len);
    bool branded = p && private_name_is_instance_method(p);
    if (!branded && method_count > 1) emit_private_token(c, m->left);
    compile_method_function(c, m);
    if (class_method_needs_home(m)) {
      emit_get_local(c, proto_local);
      emit_op(c, OP_SET_HOME_OBJ);
      emit_op(c, OP_POP);
    }
    if (branded) {
      if (p->method_slot >= 0) emit_private_table_store(c, p->method_local, p->method_slot);
      else emit_put_local(c, p->method_local);
      continue;
    }
    if (method_count > 1) emit_number(c, private_method_kind(m));
  }
  
  if (method_count == 0) return;
  if (method_count > 1) {
    emit_op(c, OP_ARRAY);
    emit_u16(c, (uint16_t)(method_count * 3));
  }
  emit_put_local(c, methods_local);
}

static const char *invalid_constructor_member(const sv_ast_t *m, bool is_fn, bool has_constructor) {
  if (!is_fn) return "Classes may not have a field named 'constructor'";
  if (m->flags & FN_STATIC) return NULL;
  if (m->flags & (FN_GETTER | FN_SETTER)) return "Class constructor may not be an accessor";
  if (m->flags & FN_ASYNC) return "Class constructor may not be an async method";
  if (m->flags & FN_GENERATOR) return "Class constructor may not be a generator";
  return has_constructor ? "A class may only have one constructor" : NULL;
}

static bool is_literal_key(const sv_ast_t *key, const char *name, uint32_t len) {
  if (!key || !key->str || (key->type != N_IDENT && key->type != N_STRING)) return false;
  return key->len == len && memcmp(key->str, name, len) == 0;
}

static bool is_constructor_key(const sv_ast_t *key) {
  return is_literal_key(key, "constructor", 11);
}

static bool ast_may_reference_super(const sv_ast_t *node) {
  if (!node) return false;
  if (is_ident_name(node, "super")) return true;
  if (node->type == N_CALL && is_ident_name(node->left, "eval")) return true;
  if (node->type == N_FUNC && !(node->flags & FN_ARROW)) return false;
  
  const sv_ast_t *child;
  SV_AST_FOR_EACH_CHILD(node, child)
    if (ast_may_reference_super(child)) return true;
  
  return false;
}

bool class_method_needs_home(const sv_ast_t *method) {
  const sv_ast_t *fn = method ? method->right : NULL;
  return fn && fn->type == N_FUNC && (fn->flags & (FN_USES_SUPER | FN_DIRECT_EVAL));
}

static bool field_inits_use_super(sv_ast_t **fields, int count) {
  for (int i = 0; i < count; i++) {
    if (!is_class_method_def(fields[i]) && ast_may_reference_super(fields[i]->right)) return true;
  }
  return false;
}

static void emit_super_local(sv_compiler_t *c) {
  c->super_local = add_local(c, "super", 5, false, c->scope_depth);
  emit_op(c, OP_SPECIAL_OBJ);
  emit(c, SV_SPECIAL_SUPER);
  emit_put_local(c, c->super_local);
}

static int compile_static_child_function(
  sv_compiler_t *c, sv_ast_t *node, bool returns_expr, const sv_ast_t *key, int key_local
) {
  sv_compiler_t comp;
  sv_compile_ctx_init_child(&comp, c, NULL, SV_COMPILE_SCRIPT);
  comp.is_strict = true;
  comp.is_static = true;
  comp.no_arguments_scope = true;
  emit_super_local(&comp);

  if (returns_expr) {
    if (key_local >= 0 && node && node->type == N_CLASS)
      comp.inferred_key_upval = capture_field_local(&comp, c, key_local) + 1;
    if (node) compile_field_initializer(&comp, node, key);
    else emit_op(&comp, OP_UNDEF);
    emit_return_from_stack(&comp);
  } else {
    check_statement_list_declarations(&comp, &node->args, false, NULL, true);
    for (int i = 0; i < node->args.count; i++) hoist_var_decls(&comp, node->args.items[i]);
    hoist_lexical_decls(&comp, &node->args);
    hoist_func_decls(&comp, &node->args);
    compile_stmts(&comp, &node->args);
    emit_close_upvals(&comp);
    emit_op(&comp, OP_RETURN_UNDEF);
  }

  sv_func_t *fn = sv_func_new(c->js);
  sv_func_init_code_and_map_templates(&comp, fn);
  sv_func_init_obj_sites(&comp, fn);
  sv_func_init_literal_sites(&comp, fn);

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
  fn->debug->source_line = (int)source_line_of(c, node);

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

static void compile_static_initializer_value(sv_compiler_t *c, sv_ast_t *expr, const sv_ast_t *key, int key_local, int ctor_local) {
  int idx = compile_static_child_function(c, expr, true, key, key_local);
  emit_static_child_call(c, idx, ctor_local);
}

static void compile_static_block(sv_compiler_t *c, sv_ast_t *block, int ctor_local) {
  int idx = compile_static_child_function(c, block, false, NULL, -1);
  emit_static_child_call(c, idx, ctor_local);
  emit_op(c, OP_POP);
}

static void compile_class_strict(
  sv_compiler_t *c, sv_ast_t *node,
  const char *inferred_name, uint32_t inferred_name_len, int inferred_key_upval
) {
  int outer_name_local = -1;
  bool binds_outer_name = node->str &&
    (node->flags & FN_CLASS_DECL);
  bool class_global_lexical = binds_outer_name && is_repl_top_level(c);

  sv_ast_t *ctor_method = NULL;
  bool has_computed_key = false;

  int field_count = 0;
  int method_emit_count = 0;
  int static_init_count = 0;

  if (binds_outer_name)
    outer_name_local = resolve_local(c, node->str, node->len);

  sv_private_scope_t private_scope = { .parent = c->private_scope, .brand = { .local = -1, .method_local = -1, .slot = -1, .method_slot = -1 } };
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
    if (m->flags & FN_COMPUTED) has_computed_key = true;

    if (is_private) {
      uint8_t private_kind = (m->flags & FN_GETTER) ? SV_COMP_PRIVATE_GETTER :
        (m->flags & FN_SETTER) ? SV_COMP_PRIVATE_SETTER :
        is_fn ? SV_COMP_PRIVATE_METHOD : SV_COMP_PRIVATE_FIELD;
      if (!private_scope_add(c, &private_scope, m->left, private_kind, !!(m->flags & FN_STATIC))) {
        c->private_scope = saved_private_scope;
        free(private_scope.names);
        free(private_scope.index);
        emit_op(c, OP_UNDEF);
        return;
      }
    }
    
    if (!(m->flags & FN_COMPUTED) && is_constructor_key(m->left)) {
      const char *invalid = invalid_constructor_member(m, is_fn, ctor_method != NULL);
      if (invalid) {
        js_mkerr_typed(c->js, JS_ERR_SYNTAX, "%s", invalid);
        c->private_scope = saved_private_scope;
        free(private_scope.names);
        free(private_scope.index);
        emit_op(c, OP_UNDEF);
        return;
      }
      if (is_fn && !(m->flags & FN_STATIC)) { ctor_method = m; continue; }
    }

    if ((m->flags & FN_STATIC) && !(m->flags & FN_COMPUTED) && is_literal_key(m->left, "prototype", 9)) {
      js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Classes may not have a static property named 'prototype'");
      c->private_scope = saved_private_scope;
      free(private_scope.names);
      free(private_scope.index);
      emit_op(c, OP_UNDEF);
      return;
    }
    
    if (!(m->flags & FN_STATIC) && (is_private || !is_fn)) field_count++;
    else if (is_fn && (!is_private || (m->flags & FN_STATIC))) method_emit_count++;
    else if (m->flags & FN_STATIC) static_init_count++;
  }

  const char *class_name = node->str;
  uint32_t class_name_len = node->len;
  if (!class_name && inferred_name) {
    class_name = inferred_name;
    class_name_len = inferred_name_len;
  }

  int inner_name_local = -1;
  bool inner_name_tdz = false;
  bool has_class_scope = node->str || private_scope.count > 0 || field_count > 0 || has_computed_key;
  if (has_class_scope) {
    begin_scope(c);
    if (node->str) {
      inner_name_local = add_local(c, node->str, node->len, true, c->scope_depth);
      inner_name_tdz = has_computed_key || node->left;
    }
    if (inner_name_tdz) {
      c->locals[inner_name_local].is_tdz = true;
      emit_op(c, OP_SET_LOCAL_UNDEF);
      emit_u16(c, (uint16_t)(inner_name_local - c->param_locals));
    }
  }

  c->private_scope = saved_private_scope;
  if (node->left) compile_expr(c, node->left);
  else emit_op(c, OP_UNDEF);
  c->private_scope = &private_scope;

  if (has_class_scope) {
    bool solo[2] = { private_token_is_solo(&private_scope, false), private_token_is_solo(&private_scope, true) };
    bool tabled = private_scope.count * 2 + (c->local_count - c->param_locals) >= SV_SPILL_THRESHOLD;
    int table_local = -1;
    int next_slot = 0;
    if (tabled) {
      table_local = add_hidden_local(c);
      emit_op(c, OP_ARRAY);
      emit_u16(c, 0);
      emit_put_local(c, table_local);
    }

    for (int i = 0; i < private_scope.count; i++) {
      sv_private_name_t *p = &private_scope.names[i];
      p->owner = c;
      if (private_name_is_instance_method(p)) {
        if (tabled) {
          p->method_local = table_local;
          p->method_slot = next_slot++;
          emit_op(c, OP_UNDEF);
          emit_private_table_store(c, table_local, p->method_slot);
        } else p->method_local = add_local(c, "", 0, false, c->scope_depth);
        if (private_scope.brand.local >= 0) continue;
        private_scope.brand.owner = c;
        private_scope.brand.hash = private_brand_hash(p->hash, node->src_off);
        p = &private_scope.brand;
      }
      p->hash &= ~ANT_PRIVATE_HASH_SOLO;
      if (solo[p->is_static]) p->hash |= ANT_PRIVATE_HASH_SOLO;
      if (!tabled) {
        p->local = add_local(c, "", 0, true, c->scope_depth);
        emit_op(c, OP_PRIVATE_TOKEN);
        emit_u32(c, p->hash);
        emit_put_local(c, p->local);
        continue;
      }
      p->local = table_local;
      p->slot = next_slot++;
      emit_op(c, OP_PRIVATE_TOKEN);
      emit_u32(c, p->hash);
      emit_private_table_store(c, table_local, p->slot);
    }
  }

  sv_ast_t **field_inits = NULL;
  int *field_locals = NULL;
  int private_method_count = 0;
  int private_methods_local = -1;
  
  sv_class_emit_item_t *method_emit_items = NULL;
  sv_class_emit_item_t *static_init_items = NULL;
  
  if (field_count > 0) {
    field_inits = malloc(sizeof(sv_ast_t *) * field_count);
    field_locals = malloc(sizeof(int) * field_count);
  }
  if (method_emit_count > 0)
    method_emit_items = malloc(sizeof(sv_class_emit_item_t) * method_emit_count);
  if (static_init_count > 0)
    static_init_items = malloc(sizeof(sv_class_emit_item_t) * static_init_count);

  int member_count = node->args.count;
  if (
    (field_count > 0 && (!field_inits || !field_locals)) ||
    (method_emit_count > 0 && !method_emit_items) ||
    (static_init_count > 0 && !static_init_items)
  ) {
    Silver_Compiler_RaiseOutOfMemory(c);
    field_count = method_emit_count = static_init_count = member_count = 0;
  }

  int fi = 0, mi = 0, si = 0;
  for (int i = 0; i < member_count; i++) {
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
      if (is_private && is_fn) {
        int brand_local = private_brand_method_local(c, m);
        if (brand_local < 0 && private_method_count++ == 0) private_methods_local = reserve_private_method_local(c);
        field_locals[fi++] = brand_local;
      } else field_locals[fi++] = (!is_private && (m->flags & FN_COMPUTED))
        ? compile_class_precompute_key(c, m->left) : -1;
      continue;
    }

    bool needs_preeval_key = !is_private && (m->flags & FN_COMPUTED) &&
      (is_fn || is_static);
    int preeval_key = needs_preeval_key ? compile_class_precompute_key(c, m->left) : -1;

    if (is_fn && (!is_private || is_static)) {
      method_emit_items[mi++] = (sv_class_emit_item_t){ m, preeval_key };
      continue;
    }
    if (is_static) static_init_items[si++] = (sv_class_emit_item_t){ m, preeval_key };
  }

  if (inner_name_tdz) c->locals[inner_name_local].is_tdz = false;

  if (ctor_method && ctor_method->right) {
    ctor_method->right->flags |= FN_CLASS_CTOR;
    if (node->left) ctor_method->right->flags |= FN_DERIVED_CTOR;
    c->field_inits = field_inits;
    c->field_init_count = field_count;
    c->field_locals = field_locals;
    c->private_method_count = private_method_count;
    c->private_methods_local = private_methods_local;
    if (node->str && node->len) {
      c->inferred_name = node->str;
      c->inferred_name_len = node->len;
    } else {
      c->inferred_name = inferred_name;
      c->inferred_name_len = inferred_name_len;
    }
    compile_func_expr(c, ctor_method->right);
    c->inferred_name = NULL;
    c->inferred_name_len = 0;
    c->field_inits = NULL;
    c->field_init_count = 0;
    c->field_locals = NULL;
  } else if (field_count > 0) {
    c->field_locals = field_locals;
    c->private_method_count = private_method_count;
    c->private_methods_local = private_methods_local;
    sv_compiler_t comp;
    sv_compile_ctx_init_child(&comp, c, NULL, SV_COMPILE_SCRIPT);
    comp.is_strict = true;
    comp.is_derived_ctor = node->left != NULL;

    emit_op(&comp, OP_CHECK_CTOR);
    if (field_inits_use_super(field_inits, field_count)) emit_super_local(&comp);

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
    fn->uses_super = comp.super_referenced;
    sv_func_set_ctor_props(fn, ctor_prop_count(NULL, class_public_field_count(field_inits, field_count)));
    sv_func_init_code_and_map_templates(&comp, fn);
    sv_func_init_obj_sites(&comp, fn);
    sv_func_init_literal_sites(&comp, fn);
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
    fn->debug->source_line = (int)source_line_of(c, node);
    
    if (node->str && node->len > 0) {
      char *name = sv_code_bump(c->js, node->len + 1);
      memcpy(name, node->str, node->len);
      name[node->len] = '\0';
      fn->debug->name = name;
    }
    
    sv_compile_ctx_cleanup(&comp);
    int idx = add_constant(c, mkref(kTypeFunctionInfo, fn));
    emit_closure(c, idx);
    emit_set_function_name(c, class_name, class_name_len);
  } else emit_op(c, OP_UNDEF);
  
  c->field_locals = NULL;
  c->private_method_count = 0;
  if (inner_name_tdz) c->locals[inner_name_local].is_tdz = true;

  int atom = class_name
    ? add_atom(c, class_name, class_name_len)
    : add_atom(c, "", 0);
  emit_op(c, OP_DEFINE_CLASS);
  emit_u32(c, (uint32_t)atom);
  emit(c, SV_CLASS_FLAG_HAS_NAME |
    (node->left ? SV_CLASS_FLAG_HAS_HERITAGE : 0));
  
  emit_u32(c, node->src_off);
  emit_u32(c, node->src_end);

  int proto_local = add_local(c, "", 0, false, c->scope_depth);
  int ctor_local = add_local(c, "", 0, false, c->scope_depth);
  emit_put_local(c, proto_local);
  emit_put_local(c, ctor_local);

  if (inner_name_local >= 0) {
    emit_get_local(c, ctor_local);
    emit_put_local(c, inner_name_local);
    c->locals[inner_name_local].is_tdz = false;
  }

  for (int i = 0; i < method_emit_count; i++) {
    sv_class_emit_item_t *item = &method_emit_items[i];
    compile_class_element_strict(
      c, item->node, ctor_local, proto_local, item->preeval_key
    );
  }
  
  compile_private_instance_methods(c, field_inits, field_count, private_methods_local, private_method_count, proto_local);
  free(field_inits);
  free(field_locals);

  if (!node->str && inferred_key_upval != 0) {
    emit_get_local(c, ctor_local);
    if (inferred_key_upval > 0) {
      emit_op(c, OP_GET_UPVAL);
      emit_u16(c, (uint16_t)(inferred_key_upval - 1));
    } else emit_get_local(c, -inferred_key_upval - 1);
    emit_op(c, OP_SET_NAME_COMP);
    emit_op(c, OP_POP);
    emit_op(c, OP_POP);
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
  free(private_scope.index);
}

void compile_class(sv_compiler_t *c, sv_ast_t *node) {
  bool saved_strict = c->is_strict;
  c->is_strict = true;
  const char *inferred_name = c->inferred_name;
  uint32_t inferred_name_len = c->inferred_name_len;
  int inferred_key_upval = c->inferred_key_upval;
  c->inferred_name = NULL;
  c->inferred_name_len = 0;
  c->inferred_key_upval = 0;
  compile_class_strict(c, node, inferred_name, inferred_name_len, inferred_key_upval);
  c->is_strict = saved_strict;
}
