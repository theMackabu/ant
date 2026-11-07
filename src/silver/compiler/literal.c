#include "compiler_internal.h"

size_t literal_num_key(double num, char *buf, size_t cap) {
  if (isnan(num)) return (size_t)snprintf(buf, cap, "NaN");
  if (isinf(num)) return (size_t)snprintf(buf, cap, num > 0 ? "Infinity" : "-Infinity");
  return ant_number_to_shortest(num, buf, cap);
}

static constexpr int SV_SHAPED_LITERAL_MAX_KEYS = 32;

void compile_array(sv_compiler_t *c, sv_ast_t *node) {
  int count = node->args.count;
  bool has_spread = false;
  for (int i = 0; i < count; i++) {
    sv_ast_t *elem = node->args.items[i];
    if (elem && elem->type == N_SPREAD) {
      has_spread = true;
      break;
    }
  }

  if (!has_spread) {
    for (int i = 0; i < count; i++) {
      sv_ast_t *elem = node->args.items[i];
      if (elem && elem->type == N_EMPTY)
        emit_op(c, OP_EMPTY);
      else
        compile_expr(c, elem);
    }
    emit_op(c, OP_ARRAY);
    emit_u16(c, (uint16_t)count);
    return;
  }

  emit_op(c, OP_ARRAY);
  emit_u16(c, 0);
  for (int i = 0; i < count; i++) {
    sv_ast_t *elem = node->args.items[i];
    if (elem && elem->type == N_SPREAD) compile_concat_spread_to_array(c, elem);
    else compile_push_arg_to_array(c, elem);
  }
}

static bool object_literal_proto_entry(const sv_ast_t *prop) {
  return prop->type == N_PROPERTY && (prop->flags & FN_COLON) &&
    !(prop->flags & (FN_GETTER | FN_SETTER | FN_COMPUTED)) && sv_ast_is_proto_key(prop->left);
}

static bool object_literal_static_keys(
  sv_ast_t *node,
  const char **keys, uint32_t *lens,
  char (*numbuf)[SV_LITERAL_NUM_KEY_BUF_SIZE],
  int *count, int *proto_at
) {
  *count = 0;
  *proto_at = -1;
  for (int p = 0; p < node->args.count; p++) {
    sv_ast_t *prop = node->args.items[p];
    if (object_literal_proto_entry(prop)) {
      if (*proto_at >= 0) return false;
      *proto_at = p;
      continue;
    }
    if (*count >= SV_SHAPED_LITERAL_MAX_KEYS) return false;
    int i = (*count)++;
    if (prop->type != N_PROPERTY) return false;
    if (prop->flags & (FN_GETTER | FN_SETTER | FN_COMPUTED)) return false;
    sv_ast_t *k = prop->left;
    if (!k) return false;
    if (k->type == N_IDENT && !is_quoted_ident_key(k)) {
      keys[i] = k->str; lens[i] = k->len;
    } else if (is_quoted_ident_key(k)) {
      keys[i] = k->str + 1; lens[i] = k->len - 2;
    } else if (k->type == N_STRING) {
      keys[i] = k->str ? k->str : ""; lens[i] = k->len;
    } else if (k->type == N_NUMBER) {
      size_t nn = literal_num_key(k->num, numbuf[i], sizeof(numbuf[i]));
      keys[i] = numbuf[i]; lens[i] = (uint32_t)nn;
    } else return false;
    for (int j = 0; j < i; j++)
      if (lens[j] == lens[i] && memcmp(keys[j], keys[i], lens[i]) == 0)
        return false;
  }
  return *count > 0;
}

static void record_shaped_site(sv_compiler_t *c, uint32_t bc_off,
                               const uint32_t *atom_idx, uint16_t count) {
  if (c->shaped_site_count >= c->shaped_site_cap) {
    int cap = c->shaped_site_cap ? c->shaped_site_cap * 2 : 8;
    void *next = realloc(c->shaped_sites, (size_t)cap * sizeof(*c->shaped_sites));
    if (!next) return;
    c->shaped_sites = next;
    c->shaped_site_cap = cap;
  }
  if (c->shaped_key_count + count > c->shaped_key_cap) {
    int cap = c->shaped_key_cap ? c->shaped_key_cap * 2 : 32;
    while (cap < c->shaped_key_count + count) cap *= 2;
    void *next = realloc(c->shaped_keys, (size_t)cap * sizeof(uint32_t));
    if (!next) return;
    c->shaped_keys = next;
    c->shaped_key_cap = cap;
  }
  c->shaped_sites[c->shaped_site_count].bc_off = bc_off;
  c->shaped_sites[c->shaped_site_count].first_key = (uint32_t)c->shaped_key_count;
  c->shaped_sites[c->shaped_site_count].key_count = count;
  c->shaped_site_count++;
  memcpy(c->shaped_keys + c->shaped_key_count, atom_idx, (size_t)count * sizeof(uint32_t));
  c->shaped_key_count += count;
}

static bool object_method_needs_home(const sv_ast_t *value) {
  return value && value->type == N_FUNC &&
    (value->flags & (FN_METHOD | FN_GETTER | FN_SETTER)) &&
    (value->flags & (FN_USES_SUPER | FN_DIRECT_EVAL));
}

static void emit_object_method_home(sv_compiler_t *c, const sv_ast_t *value, bool key_on_stack) {
  if (!object_method_needs_home(value)) return;
  if (key_on_stack) {
    emit_op(c, OP_ROT3L);
    emit_op(c, OP_SET_HOME_OBJ);
    emit_op(c, OP_ROT3R);
  } else {
    emit_op(c, OP_SWAP);
    emit_op(c, OP_SET_HOME_OBJ);
    emit_op(c, OP_SWAP);
  }
}

void compile_object(sv_compiler_t *c, sv_ast_t *node) {
  if (node->flags & FN_PATTERN_ONLY) {
    bool shorthand_init = false;
    for (int p = 0; p < node->args.count && !shorthand_init; p++) {
      sv_ast_t *prop = node->args.items[p];
      shorthand_init = prop && prop->type == N_PROPERTY && !(prop->flags & FN_COLON) && prop->right && prop->right->type == N_ASSIGN;
    }
    js_mkerr_typed(c->js, JS_ERR_SYNTAX, shorthand_init
      ? "Invalid shorthand property initializer"
      : "Duplicate __proto__ fields are not allowed in object literals");
    emit_op(c, OP_UNDEF);
    return;
  }
  const char *skeys[SV_SHAPED_LITERAL_MAX_KEYS];
  uint32_t slens[SV_SHAPED_LITERAL_MAX_KEYS];
  char snum[SV_SHAPED_LITERAL_MAX_KEYS][SV_LITERAL_NUM_KEY_BUF_SIZE];
  
  int key_count, proto_at;
  if (object_literal_static_keys(node, skeys, slens, snum, &key_count, &proto_at)) {
    uint32_t atom_idx[SV_SHAPED_LITERAL_MAX_KEYS];
    for (int i = 0; i < key_count; i++)
      atom_idx[i] = (uint32_t)add_atom(c, skeys[i], slens[i]);
    uint32_t obj_off = (uint32_t)c->code_len;
    emit_op(c, OP_OBJECT);
    record_shaped_site(c, obj_off, atom_idx, (uint16_t)key_count);
    for (int p = 0, i = 0; p < node->args.count; p++) {
      sv_ast_t *prop = node->args.items[p];
      if (p == proto_at) {
        compile_expr(c, prop->right);
        emit_op(c, OP_SET_PROTO);
        continue;
      }
      if (prop->left->type == N_IDENT && !is_quoted_ident_key(prop->left))
        compile_expr_with_inferred_name(c, prop->right, prop->left->str, prop->left->len);
      else compile_expr(c, prop->right);
      emit_object_method_home(c, prop->right, false);
      emit_op(c, OP_DEFINE_SLOT);
      emit_u32(c, atom_idx[i]);
      emit_u16(c, (uint16_t)i);
      i++;
    }
    return;
  }

  emit_op(c, OP_OBJECT);
  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *prop = node->args.items[i];
    if (prop->type == N_SPREAD) {
      compile_expr(c, prop->right);
      emit_op(c, OP_COPY_DATA_PROPS);
      emit_u16(c, 0);
      emit_op(c, OP_POP);
      continue;
    }
    
    if (prop->type != N_PROPERTY) continue;
    if (prop->flags & FN_GETTER || prop->flags & FN_SETTER) {
      compile_expr(c, prop->right);
      emit_object_method_home(c, prop->right, false);
      uint8_t flags = 0;
      if (prop->flags & FN_GETTER) flags |= SV_DEFINE_METHOD_GETTER;
      if (prop->flags & FN_SETTER) flags |= SV_DEFINE_METHOD_SETTER;
      flags |= SV_DEFINE_METHOD_SET_NAME;
      if (prop->flags & FN_COMPUTED) compile_expr(c, prop->left);
      else compile_static_property_key(c, prop->left);
      emit_op(c, OP_SWAP);
      emit_op(c, OP_DEFINE_METHOD_COMP);
      emit(c, flags);
    } else if (prop->flags & FN_COMPUTED) {
      compile_expr(c, prop->left);
      compile_expr(c, prop->right);
      emit_object_method_home(c, prop->right, true);
      emit_op(c, OP_DEFINE_METHOD_COMP);
      emit(c, node_needs_inferred_function_name(prop->right) ? SV_DEFINE_METHOD_SET_NAME : 0);
    } else {
      if (object_literal_proto_entry(prop)) {
        compile_expr(c, prop->right);
        emit_op(c, OP_SET_PROTO);
        continue;
      }
      if (prop->left && prop->left->type == N_IDENT && !is_quoted_ident_key(prop->left))
        compile_expr_with_inferred_name(c, prop->right, prop->left->str, prop->left->len);
      else compile_expr(c, prop->right);
      emit_object_method_home(c, prop->right, false);
      if (prop->left->type == N_IDENT && !is_quoted_ident_key(prop->left)) {
        emit_atom_op(c, OP_DEFINE_FIELD, prop->left->str, prop->left->len);
      } else if (is_quoted_ident_key(prop->left)) {
        emit_atom_op(c, OP_DEFINE_FIELD, prop->left->str + 1, prop->left->len - 2);
      } else if (prop->left->type == N_STRING) {
        emit_atom_op(c, OP_DEFINE_FIELD, prop->left->str ? prop->left->str : "", prop->left->len);
      } else if (prop->left->type == N_NUMBER) {
        char buf[SV_LITERAL_NUM_KEY_BUF_SIZE];
        size_t n = literal_num_key(prop->left->num, buf, sizeof(buf));
        emit_atom_op(c, OP_DEFINE_FIELD, buf, (uint32_t)n);
      } else emit_atom_op(c, OP_DEFINE_FIELD, prop->left->str, prop->left->len);
    }
  }
}

void compile_func_expr(sv_compiler_t *c, sv_ast_t *node) {
  bool has_name = node->str && node->len > 0 && !(node->flags & FN_ARROW);
  int name_local = -1;

  if (has_name) {
    begin_scope(c);
    name_local = add_local(c, node->str, node->len, true, c->scope_depth);
    emit_op(c, OP_UNDEF);
    emit_put_local(c, name_local);
  }

  sv_func_t *fn = compile_function_body(c, node, SV_COMPILE_SCRIPT);
  if (!fn) {
    emit_op(c, OP_UNDEF);
    if (has_name) end_scope(c);
    return;
  }
  
  int idx = add_constant(c, mkref(kTypeFunctionInfo, fn));
  emit_closure(c, idx);

  if (node->str && node->len > 0) {
    emit_set_function_name(c, node->str, node->len);
  } else if (c->inferred_name) {
    emit_set_function_name(c, c->inferred_name, c->inferred_name_len);
    c->inferred_name = NULL;
    c->inferred_name_len = 0;
  } else {
    emit_set_function_name(c, NULL, 0);
  }

  if (has_name) {
    emit_op(c, OP_DUP);
    emit_put_local(c, name_local);
    end_scope(c);
  }
}
