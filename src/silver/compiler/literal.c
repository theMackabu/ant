#include "compiler_internal.h"
#include "arrays.h"

size_t literal_num_key(double num, char *buf, size_t cap) {
  if (isnan(num)) return (size_t)snprintf(buf, cap, "NaN");
  if (isinf(num)) return (size_t)snprintf(buf, cap, num > 0 ? "Infinity" : "-Infinity");
  return ant_number_to_shortest(num, buf, cap);
}

size_t literal_num_name(const sv_ast_t *key, char *buf, size_t cap) {
  if (!key->str || key->len >= cap) return literal_num_key(key->num, buf, cap);
  memcpy(buf, key->str, key->len);
  buf[key->len] = '\0';
  return key->len;
}

static constexpr int SV_SHAPED_LITERAL_MAX_KEYS = 32;
static constexpr int SV_LITERAL_DYNAMIC_RATIO = 6;
static constexpr int SV_LITERAL_CLASS_STACK = 64;
static constexpr int SV_LITERAL_CLASS_STRIDE = 64;

static bool literal_is_leaf(const sv_ast_t *node) {
  if (!node) return false;
  switch (node->type) {
    case N_NUMBER: case N_STRING: case N_BOOL: case N_NULL: return true;
    case N_UNARY: return node->op == TOK_UMINUS && node->right && node->right->type == N_NUMBER;
    default: return false;
  }
}

static ant_value_t literal_leaf_value(sv_compiler_t *c, const sv_ast_t *node) {
  switch (node->type) {
    case N_NUMBER: return Ant_Value_FromNumber(node->num);
    case N_STRING: return ast_string_const(c, node);
    case N_BOOL:   return Ant_Value_FromBool(node->num != 0.0);
    case N_NULL:   return Ant_Value_Null;
    default:       return Ant_Value_FromNumber(-node->right->num);
  }
}

static bool literal_string_key(const sv_ast_t *k, const char **key, uint32_t *len) {
  if (!k) return false;
  if (is_quoted_ident_key(k)) {
    *key = k->str + 1;
    *len = k->len - 2;
    return true;
  }
  if (k->type != N_IDENT && k->type != N_STRING) return false;
  *key = k->str ? k->str : "";
  *len = k->len;
  return true;
}

static bool literal_object_key(const sv_ast_t *prop, const char **key, uint32_t *len) {
  if (!prop || prop->type != N_PROPERTY || !(prop->flags & FN_COLON)) return false;
  if (prop->flags & (FN_GETTER | FN_SETTER | FN_COMPUTED)) return false;
  if (!literal_string_key(prop->left, key, len)) return false;
  if (*len == 9 && memcmp(*key, "__proto__", 9) == 0) return false;
  return !is_array_index(*key, *len);
}

typedef enum : uint8_t {
  LITERAL_TREE_OK,
  LITERAL_TREE_DYNAMIC,
  LITERAL_TREE_TOO_DEEP,
} literal_tree_t;

static literal_tree_t literal_tree_check(sv_ast_t *node, int depth);

static bool literal_is_container(const sv_ast_t *node) {
  return node && (node->type == N_ARRAY || node->type == N_OBJECT);
}

static bool literal_child_is_dynamic(const sv_ast_t *child) {
  if (!child) return true;
  if (literal_is_container(child)) return (child->flags & FN_LITERAL_DYNAMIC) != 0;
  return child->type != N_EMPTY && !literal_is_leaf(child);
}

static literal_tree_t literal_tree_scan(sv_ast_t *node, int depth) {
  if (node->type == N_ARRAY) {
    for (int i = 0; i < node->args.count; i++)
      if (literal_child_is_dynamic(node->args.items[i])) return LITERAL_TREE_DYNAMIC;
    
    for (int i = 0; i < node->args.count; i++) {
      sv_ast_t *elem = node->args.items[i];
      if (!literal_is_container(elem)) continue;
      literal_tree_t r = literal_tree_check(elem, depth + 1);
      if (r != LITERAL_TREE_OK) return r;
    }
    return LITERAL_TREE_OK;
  }
  
  if (node->flags & FN_PATTERN_ONLY) return LITERAL_TREE_DYNAMIC;
  if (node->args.count > SV_SHAPED_LITERAL_MAX_KEYS) return LITERAL_TREE_DYNAMIC;
  
  for (int i = 0; i < node->args.count; i++) {
    const sv_ast_t *prop = node->args.items[i];
    if (!prop || prop->type != N_PROPERTY || literal_child_is_dynamic(prop->right)) return LITERAL_TREE_DYNAMIC;
  }
  
  const char *keys[SV_SHAPED_LITERAL_MAX_KEYS];
  uint32_t lens[SV_SHAPED_LITERAL_MAX_KEYS];
  
  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *prop = node->args.items[i];
    if (!literal_object_key(prop, &keys[i], &lens[i])) return LITERAL_TREE_DYNAMIC;
    for (int j = 0; j < i; j++) if (lens[j] == lens[i] && memcmp(keys[j], keys[i], lens[i]) == 0) return LITERAL_TREE_DYNAMIC;
    if (!literal_is_container(prop->right)) continue;
    literal_tree_t r = literal_tree_check(prop->right, depth + 1);
    if (r != LITERAL_TREE_OK) return r;
  }
  
  return LITERAL_TREE_OK;
}

static literal_tree_t literal_tree_check(sv_ast_t *node, int depth) {
  if (!node || (node->type != N_ARRAY && node->type != N_OBJECT)) return LITERAL_TREE_DYNAMIC;
  if (node->flags & FN_LITERAL_DYNAMIC) return LITERAL_TREE_DYNAMIC;
  if (depth >= SV_LITERAL_MAX_DEPTH) return LITERAL_TREE_TOO_DEEP;
  
  literal_tree_t r = literal_tree_scan(node, depth);
  if (r == LITERAL_TREE_DYNAMIC) node->flags |= FN_LITERAL_DYNAMIC;
  return r;
}

static bool literal_site_uses_block(sv_literal_kind_t kind, uint8_t flags, uint32_t count) {
  return kind == SV_LITERAL_ARRAY && count && !(flags & SV_LITERAL_HAS_CHILDREN);
}

static int literal_site_add(
  sv_compiler_t *c, sv_literal_kind_t kind, uint8_t flags,
  const ant_value_t *vals, uint32_t count, const uint32_t *keys
) {
  if (c->literal_site_count >= c->literal_site_cap) {
    int cap = c->literal_site_cap ? c->literal_site_cap * 2 : 8;
    void *next = realloc(c->literal_sites, (size_t)cap * sizeof(*c->literal_sites));
    if (!next) return -1;
    c->literal_sites = next;
    c->literal_site_cap = cap;
  }
  
  uint32_t first_key = (uint32_t)c->literal_key_count;
  if (keys && count) {
    if (c->literal_key_count + (int)count > c->literal_key_cap) {
      int cap = c->literal_key_cap ? c->literal_key_cap * 2 : 32;
      while (cap < c->literal_key_count + (int)count) cap *= 2;
      void *next = realloc(c->literal_keys, (size_t)cap * sizeof(uint32_t));
      if (!next) return -1;
      c->literal_keys = next;
      c->literal_key_cap = cap;
    }
    memcpy(c->literal_keys + c->literal_key_count, keys, (size_t)count * sizeof(uint32_t));
    c->literal_key_count += (int)count;
  }
  
  gc_shared_elements_t *block = NULL;
  int start = 0;
  
  if (literal_site_uses_block(kind, flags, count)) {
    if (!(block = gc_shared_elements_alloc(c->js, vals, count))) return -1;
    sv_code_unit_charge(c->js, sizeof(*block) + (size_t)count * sizeof(*vals));
  } else if (count && (start = add_constant_run(c, vals, (int)count)) < 0) return -1;
  
  c->literal_sites[c->literal_site_count] = (struct sv_literal_site_desc){
    .block = block, .start = (uint32_t)start, .count = count, .first_key = first_key, .kind = kind, .flags = flags,
  };
  
  return c->literal_site_count++;
}

typedef enum : uint8_t {
  LITERAL_DYNAMIC,
  LITERAL_FIXED,
  LITERAL_NESTED,
} literal_class_t;

static bool literal_tree_has_array(const sv_ast_t *node) {
  if (node->type == N_ARRAY) return node->args.count > 0;
  for (int i = 0; i < node->args.count; i++) {
    const sv_ast_t *value = node->args.items[i]->right;
    if (value && (value->type == N_ARRAY || value->type == N_OBJECT) && literal_tree_has_array(value)) return true;
  }
  return false;
}

static literal_class_t literal_classify(sv_ast_t *node) {
  if (node && node->type == N_NUMBER) return LITERAL_FIXED;
  if ((node && node->type == N_EMPTY) || literal_is_leaf(node)) return LITERAL_FIXED;
  if (literal_tree_check(node, 1) != LITERAL_TREE_OK) return LITERAL_DYNAMIC;
  return literal_tree_has_array(node) ? LITERAL_NESTED : LITERAL_FIXED;
}

static int literal_build_site(sv_compiler_t *c, sv_ast_t *node);

static bool literal_value(sv_compiler_t *c, sv_ast_t *node, ant_value_t *out, uint8_t *flags) {
  if (node && node->type == N_NUMBER) {
    *out = Ant_Value_FromNumber(node->num);
    return true;
  }
  
  if (node && node->type == N_EMPTY) {
    *out = T_EMPTY;
    return true;
  }
  
  if (literal_is_leaf(node)) {
    *out = literal_leaf_value(c, node);
    return !is_err(*out);
  }
  
  int site = literal_build_site(c, node);
  if (site < 0) return false;
  
  *out = SV_LITERAL_CHILD_TAG | (uint32_t)site;
  *flags |= SV_LITERAL_HAS_CHILDREN;
  
  return true;
}

static int literal_build_site(sv_compiler_t *c, sv_ast_t *node) {
  bool object = node->type == N_OBJECT;
  uint32_t count = (uint32_t)node->args.count;
  uint32_t keys[SV_SHAPED_LITERAL_MAX_KEYS];
  
  ant_value_t stack_vals[SV_LITERAL_CLASS_STACK];
  ant_value_t *vals = count > SV_LITERAL_CLASS_STACK ? malloc((size_t)count * sizeof(*vals)) : stack_vals;
  if (!vals) return -1;
  
  uint8_t flags = 0;
  for (uint32_t i = 0; i < count; i++) {
    sv_ast_t *item = node->args.items[i];
    const char *key;
    uint32_t len;
    
    if (object && literal_string_key(item->left, &key, &len)) keys[i] = (uint32_t)add_atom(c, key, len);
    if (!literal_value(c, object ? item->right : item, &vals[i], &flags)) {
      if (vals != stack_vals) free(vals);
      return -1;
    }
  }
  
  int site = literal_site_add(c, object ? SV_LITERAL_OBJECT : SV_LITERAL_ARRAY, flags, vals, count, object ? keys : NULL);
  if (vals != stack_vals) free(vals);
  
  return site;
}

static int literal_build_root(sv_compiler_t *c, sv_ast_t *node, int prefix, const literal_class_t *classes) {
  ant_value_t stack_vals[SV_LITERAL_CLASS_STACK];
  ant_value_t *vals = prefix > SV_LITERAL_CLASS_STACK ? malloc((size_t)prefix * sizeof(*vals)) : stack_vals;
  if (!vals) return -1;
  
  uint8_t flags = prefix < node->args.count ? SV_LITERAL_HAS_DYNAMIC : 0;
  for (int i = 0; i < prefix; i++) {
    sv_ast_t *item = node->args.items[i];
    if (item && item->type == N_NUMBER) {
      vals[i] = Ant_Value_FromNumber(item->num);
    } else if (classes[i] == LITERAL_DYNAMIC) {
      vals[i] = Ant_Value_Undefined;
      flags |= SV_LITERAL_HAS_DYNAMIC;
    } else if (!literal_value(c, item, &vals[i], &flags)) {
      if (vals != stack_vals) free(vals);
      return -1;
    }
  }
  
  int site = literal_site_add(c, SV_LITERAL_ARRAY, flags, vals, (uint32_t)prefix, NULL);
  if (vals != stack_vals) free(vals);
  
  return site;
}

static bool literal_root_pays(int prefix, int fixed, int nested) {
  int dynamic = prefix - fixed;
  if (prefix <= SV_LITERAL_INLINE_MAX) return dynamic == 0 && nested > 0;
  return fixed > 0 && dynamic * SV_LITERAL_DYNAMIC_RATIO <= fixed;
}

static bool literal_small_root_viable(const sv_ast_t *node, int prefix) {
  bool container = false;
  for (int i = 0; i < prefix; i++) {
    const sv_ast_t *item = node->args.items[i];
    if (literal_child_is_dynamic(item)) return false;
    container |= literal_is_container(item);
  }
  return container;
}

static bool compile_array_site(sv_compiler_t *c, sv_ast_t *node, int prefix) {
  if (prefix == 0 || (prefix <= SV_LITERAL_INLINE_MAX && !literal_small_root_viable(node, prefix))) return false;
  literal_class_t stack_classes[SV_LITERAL_CLASS_STACK];
  literal_class_t *classes = prefix <= SV_LITERAL_CLASS_STACK ? stack_classes : malloc((size_t)prefix * sizeof(*classes));
  if (!classes) return false;
  
  int fixed = 0, nested = 0;
  bool small = prefix <= SV_LITERAL_INLINE_MAX;
  for (int i = 0; i < prefix;) {
    int end = prefix - i > SV_LITERAL_CLASS_STRIDE ? i + SV_LITERAL_CLASS_STRIDE : prefix;
    for (; i < end; i++) {
      classes[i] = literal_classify(node->args.items[i]);
      if (small && classes[i] == LITERAL_DYNAMIC) break;
      fixed += classes[i] != LITERAL_DYNAMIC;
      nested += classes[i] == LITERAL_NESTED;
    }
    if (i < end || (i - fixed) * (SV_LITERAL_DYNAMIC_RATIO + 1) > prefix) break;
  }
  
  int site = literal_root_pays(prefix, fixed, nested) ? literal_build_root(c, node, prefix, classes) : -1;
  if (site < 0) {
    if (classes != stack_classes) free(classes);
    return false;
  }
  
  emit_op(c, OP_ARRAY_LITERAL);
  emit_u32(c, (uint32_t)site);
  
  for (int i = 0; i < prefix; i++) {
    if (classes[i] != LITERAL_DYNAMIC) continue;
    compile_expr(c, node->args.items[i]);
    emit_op(c, OP_ARRAY_INIT);
    emit_u32(c, (uint32_t)i);
  }
  if (classes != stack_classes) free(classes);
  
  for (int i = prefix; i < node->args.count; i++) {
    sv_ast_t *elem = node->args.items[i];
    if (elem && elem->type == N_SPREAD) compile_concat_spread_to_array(c, elem);
    else compile_push_arg_to_array(c, elem);
  }
  
  return true;
}

void compile_array(sv_compiler_t *c, sv_ast_t *node) {
  int count = node->args.count;
  int prefix = count;
  
  for (int i = 0; i < count; i++) {
    sv_ast_t *elem = node->args.items[i];
    if (elem && elem->type == N_SPREAD) {
      prefix = i;
      break;
    }
  }

  if (compile_array_site(c, node, prefix)) return;

  if (prefix == count && count <= UINT16_MAX) {
    for (int i = 0; i < count; i++) {
      sv_ast_t *elem = node->args.items[i];
      if (elem && elem->type == N_EMPTY) emit_op(c, OP_EMPTY);
      else compile_expr(c, elem);
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
    if (k && k->type == N_NUMBER) {
      size_t nn = literal_num_key(k->num, numbuf[i], sizeof(numbuf[i]));
      keys[i] = numbuf[i]; lens[i] = (uint32_t)nn;
    } else if (!literal_string_key(k, &keys[i], &lens[i])) return false;
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

static void compile_property_value(sv_compiler_t *c, sv_ast_t *prop) {
  const sv_ast_t *key = prop->left;
  char buf[SV_LITERAL_NUM_KEY_BUF_SIZE];

  if (!key || !node_needs_inferred_function_name(prop->right)) compile_expr(c, prop->right);
  else if (is_quoted_ident_key(key)) compile_expr_with_inferred_name(c, prop->right, key->str + 1, key->len - 2);
  else if (key->type == N_IDENT || key->type == N_STRING)
    compile_expr_with_inferred_name(c, prop->right, key->str ? key->str : "", key->len);
  else if (key->type == N_NUMBER)
    compile_expr_with_inferred_name(c, prop->right, buf, (uint32_t)literal_num_name(key, buf, sizeof(buf)));
  else compile_expr(c, prop->right);
}

static bool computed_key_needs_early_coercion(sv_compiler_t *c, const sv_ast_t *prop) {
  const sv_ast_t *value = prop->right;
  if (!value || value->type == N_FUNC || value->type == N_NUMBER || value->type == N_STRING) return false;
  
  switch (infer_expr_type(c, prop->left)) {
    case SV_TI_NUM:
    case SV_TI_STR:
    case SV_TI_BOOL:
    case SV_TI_NULL:
    case SV_TI_UNDEF: return false;
    default:          return true;
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
      compile_property_value(c, prop);
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
    } else if ((prop->flags & FN_COMPUTED) && prop->right && prop->right->type == N_CLASS && !prop->right->str) {
      compile_expr(c, prop->left);
      emit_op(c, OP_TO_PROPKEY);
      int key_local = add_hidden_local(c);
      emit_op(c, OP_DUP);
      emit_put_local(c, key_local);
      c->inferred_key_upval = -(key_local + 1);
      compile_expr(c, prop->right);
      c->inferred_key_upval = 0;
      emit_op(c, OP_DEFINE_METHOD_COMP);
      emit(c, 0);
    } else if (prop->flags & FN_COMPUTED) {
      compile_expr(c, prop->left);
      if (computed_key_needs_early_coercion(c, prop)) emit_op(c, OP_TO_PROPKEY);
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
      compile_property_value(c, prop);
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
