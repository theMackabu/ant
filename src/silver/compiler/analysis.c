#include "compiler_internal.h"

typedef struct sv_write_scan sv_write_scan_t;

struct sv_write_scan {
  void (*mark)(sv_write_scan_t *scan, const char *name, uint32_t len);
  void *data;
  uint32_t used;
  bool repeated;
  bool arguments_poisons;
  bool poisoned;
};

static void write_scan_mark_pattern(sv_write_scan_t *scan, const sv_ast_t *node) {
  if (!node) return;
  
  if (node->type == N_IDENT) {
    scan->mark(scan, node->str, node->len);
    return;
  }
  
  const sv_ast_t *child;
  SV_AST_FOR_EACH_CHILD(node, child) write_scan_mark_pattern(scan, child);
}

static void write_scan_mark_target(sv_write_scan_t *scan, const sv_ast_t *node) {
  if (!node) return;
  switch (node->type) {
    case N_MEMBER: case N_OPTIONAL: case N_CALL: return;
    case N_VAR:
      for (int i = 0; i < node->args.count; i++)
        if (node->args.items[i]) write_scan_mark_pattern(scan, node->args.items[i]->left);
      return;
    default: write_scan_mark_pattern(scan, node); return;
  }
}

static bool write_scan_repeats(const sv_ast_t *node, const sv_ast_t *child) {
  switch (node->type) {
    case N_FUNC: case N_ARROW: case N_CLASS: case N_METHOD: case N_STATIC_BLOCK: return true;
    case N_WHILE: case N_DO_WHILE: return true;
    case N_FOR: return child != node->init;
    case N_FOR_IN: case N_FOR_OF: case N_FOR_AWAIT_OF: return child != node->right;
    default: return false;
  }
}

static void write_scan(sv_write_scan_t *scan, const sv_ast_t *node) {
  if (!node || scan->poisoned) return;
  bool repeated = scan->repeated;
  switch (node->type) {
    case N_NUMBER: case N_STRING: case N_BIGINT: case N_BOOL:
    case N_NULL: case N_UNDEF: case N_THIS: case N_GLOBAL_THIS:
      return;
    case N_IDENT:
      if (scan->arguments_poisons && is_ident_str(node->str, node->len, "arguments", 9)) scan->poisoned = true;
      return;
    case N_WITH:
      scan->poisoned = true;
      return;
    case N_CALL:
      if (node->left && node->left->type == N_IDENT && is_ident_str(node->left->str, node->left->len, "eval", 4))
        scan->poisoned = true;
      break;
    case N_ASSIGN: write_scan_mark_target(scan, node->left); break;
    case N_UPDATE: write_scan_mark_target(scan, node->right); break;
    case N_VARDECL: if (node->right) write_scan_mark_pattern(scan, node->left); break;
    case N_FOR_IN: case N_FOR_OF: case N_FOR_AWAIT_OF:
      scan->repeated = true;
      write_scan_mark_target(scan, node->left);
      scan->repeated = repeated;
      break;
    case N_FUNC: case N_CLASS:
      if (node->str && node->len) scan->mark(scan, node->str, node->len);
      break;
    default: break;
  }
  
  sv_ast_children_t it = sv_ast_children(node);
  for (const sv_ast_t *child; (child = sv_ast_next_child(&it));) {
    scan->repeated = repeated || write_scan_repeats(node, sv_ast_child_is_arg(&it) ? NULL : child);
    write_scan(scan, child);
  }
  
  scan->repeated = repeated;
}

typedef struct {
  const sv_ast_t *params;
  uint64_t written;
} sv_value_params_t;

static void value_params_mark(sv_write_scan_t *scan, const char *name, uint32_t len) {
  sv_value_params_t *params = scan->data;
  for (int i = 0; i < params->params->args.count && i < 64; i++) {
    const sv_ast_t *param = params->params->args.items[i];
    if (is_ident_str(name, len, param->str, param->len)) params->written |= 1ull << i;
  }
}

bool param_captured_by_value(sv_compiler_t *c, int local) {
  if (!c->value_params_node || local < 0 || local >= c->param_locals || local >= 64) return false;
  if (!c->value_params_known) {
    const sv_ast_t *fn = c->value_params_node;
    sv_value_params_t params = {.params = fn};
    sv_write_scan_t scan = {.mark = value_params_mark, .data = &params, .arguments_poisons = !c->is_strict || c->args_mapped};
    for (int i = 0; i < fn->args.count && i < 64; i++)
      for (int j = 0; j < i; j++)
        if (is_ident_str(fn->args.items[i]->str, fn->args.items[i]->len, fn->args.items[j]->str, fn->args.items[j]->len))
          params.written |= (1ull << i) | (1ull << j);
    write_scan(&scan, fn->body);
    c->value_params = scan.poisoned ? 0 : ~params.written;
    c->value_params_known = true;
  }
  return (c->value_params >> local) & 1;
}

static sv_write_count_t *write_count_slot(sv_write_count_t *table, uint32_t cap, const char *name, uint32_t len) {
  uint32_t slot = sv_compile_ctx_hash_local_name(name, len) & (cap - 1);
  while (table[slot].name && (table[slot].len != len || memcmp(table[slot].name, name, len) != 0))
    slot = (slot + 1) & (cap - 1);
  return &table[slot];
}

static void write_counts_mark(sv_write_scan_t *scan, const char *name, uint32_t len) {
  sv_compiler_t *c = scan->data;
  if (!name || !len) return;
  if ((scan->used + 1) * 2 > c->write_count_cap) {
    uint32_t cap = c->write_count_cap ? c->write_count_cap * 2 : 64;
    sv_write_count_t *table = calloc(cap, sizeof(*table));
    if (!table) {
      scan->poisoned = true;
      return;
    }
    for (uint32_t i = 0; i < c->write_count_cap; i++)
      if (c->write_counts[i].name) *write_count_slot(table, cap, c->write_counts[i].name, c->write_counts[i].len) = c->write_counts[i];
    free(c->write_counts);
    c->write_counts = table;
    c->write_count_cap = cap;
  }
  sv_write_count_t *entry = write_count_slot(c->write_counts, c->write_count_cap, name, len);
  if (!entry->name) {
    *entry = (sv_write_count_t){.name = name, .len = len};
    scan->used++;
  }
  entry->writes = scan->repeated || entry->writes ? 2 : 1;
}

bool local_written_once(sv_compiler_t *c, int local) {
  if (!c->write_scan_node || local < c->param_locals || local >= c->local_count) return false;
  const sv_local_t *loc = &c->locals[local];
  if (!loc->name || !loc->name_len || loc->binding.import_kind || loc->binding.exports) return false;
  
  if (!c->write_counts_known) {
    c->write_counts_known = true;
    sv_write_scan_t scan = {.mark = write_counts_mark, .data = c};
    write_scan(&scan, c->write_scan_node->body);
    if (scan.poisoned) { c->write_scan_node = NULL; return false; }
  }
  
  if (!c->write_count_cap) return false;
  const sv_write_count_t *entry = write_count_slot(c->write_counts, c->write_count_cap, loc->name, loc->name_len);
  
  return entry->name && entry->writes == 1;
}

uint32_t class_public_field_count(sv_ast_t **fields, int count) {
  uint32_t found = 0;
  for (int i = 0; i < count; i++) found += !is_private_name_node(fields[i]->left);
  return found;
}

uint32_t ctor_prop_count(const sv_ast_t *node, uint32_t fields) {
  if (node && (node->flags & (FN_ARROW | FN_GENERATOR | FN_ASYNC))) return 0;
  return (node ? node->this_props : 0) + fields;
}

typedef struct {
  uint32_t fact;
  int start;
  int local;
} sv_type_kill_t;

static int type_region_start(const sv_compiler_t *c, uint32_t fact) {
  for (int i = 0; i < c->loop_count; i++)
    if (c->loops[i].iterates && c->loops[i].type_fact_mark >= fact) return c->loops[i].type_use_start;
  return -1;
}

static int type_capture_start(const sv_compiler_t *c, const sv_local_t *local) {
  if (!local->captured || local->is_const) return -1;
  for (int i = 0; i < c->loop_count; i++)
    if (c->loops[i].iterates) return c->loops[i].type_use_start;
  return -1;
}

static inline bool type_dep_live(const sv_compiler_t *c, const sv_type_dep_t *dep) {
  return dep->local < c->local_count && c->locals[dep->local].type_fact == dep->fact;
}

void type_dep_add(sv_compiler_t *c, int local) {
  if (c->types->dep_count >= c->types->dep_cap) {
    int cap = c->types->dep_cap ? c->types->dep_cap * 2 : 16;
    sv_type_dep_t *next = sv_compile_ctx_grow_types(
      c->types, c->types->deps, (size_t)c->types->dep_count * sizeof(*next), (size_t)cap * sizeof(*next));
    if (!next) return;
    c->types->deps = next;
    c->types->dep_cap = cap;
  }
  c->types->deps[c->types->dep_count++] = (sv_type_dep_t){.local = local, .fact = c->locals[local].type_fact};
  c->types->dep_seq++;
}

bool type_deps_live(const sv_compiler_t *c, int base) {
  for (int i = base; i < c->types->dep_count; i++)
    if (!type_dep_live(c, &c->types->deps[i])) return false;
  return true;
}

static sv_type_use_t *type_use_push(sv_compiler_t *c, const sv_type_dep_t *dep) {
  if (c->types->use_count >= c->types->use_cap) {
    int cap = c->types->use_cap ? c->types->use_cap * 2 : 32;
    sv_type_use_t *next = sv_compile_ctx_grow_types(
      c->types, c->types->uses, (size_t)c->types->use_count * sizeof(*next), (size_t)cap * sizeof(*next));
    if (!next) return NULL;
    c->types->uses = next;
    c->types->use_cap = cap;
  }
  sv_local_t *local = &c->locals[dep->local];
  sv_type_use_t *use = &c->types->uses[c->types->use_count++];
  *use = (sv_type_use_t){.fact = dep->fact, .prev = local->type_use_head};
  local->type_use_head = c->types->use_count;
  return use;
}

bool type_use_op(sv_compiler_t *c, int base, int pc) {
  for (int i = base; i < c->types->dep_count; i++) {
    sv_type_use_t *use = type_use_push(c, &c->types->deps[i]);
    if (!use) return false;
    use->at = pc;
  }
  return true;
}

static bool type_use_derived(sv_compiler_t *c, int base, int local) {
  uint32_t fact = c->locals[local].type_fact;
  int start = type_region_start(c, fact);
  if (start < 0) start = c->types->use_count;
  bool derived = false;
  for (int i = base; i < c->types->dep_count; i++) {
    if (c->types->deps[i].fact == fact) continue;
    sv_type_use_t *use = type_use_push(c, &c->types->deps[i]);
    if (!use) return false;
    use->derived = fact;
    use->at = start;
    use->local = local;
    derived = true;
  }
  return derived;
}

static void type_use_patch(sv_compiler_t *c, int pc) {
  switch (c->code[pc]) {
    case OP_ADD_NUM: c->code[pc] = OP_ADD; break;
    case OP_SUB_NUM: c->code[pc] = OP_SUB; break;
    case OP_MUL_NUM: c->code[pc] = OP_MUL; break;
    case OP_DIV_NUM: c->code[pc] = OP_DIV; break;
    default: break;
  }
}

static inline void type_local_clear(sv_local_t *local) {
  local->inferred_type = SV_TI_UNKNOWN;
  local->type_fact = 0;
  local->type_use_head = 0;
  local->type_derived = false;
}

static void type_kill(sv_compiler_t *c, sv_type_kill_t first) {
  sv_type_kill_t inline_work[16];
  sv_type_kill_t *work = inline_work;
  int count = 0, cap = 16;
  work[count++] = first;

  while (count > 0) {
    sv_type_kill_t kill = work[--count];
    bool held = kill.local < c->local_count && c->locals[kill.local].type_fact == kill.fact;
    if (held) {
      int captured = type_capture_start(c, &c->locals[kill.local]);
      if (captured >= 0 && captured < kill.start) kill.start = captured;
    }
    int i = held ? c->locals[kill.local].type_use_head - 1 : kill.start;
    
    while (held ? i >= kill.start : i < c->types->use_count) {
      sv_type_use_t *use = &c->types->uses[i];
      i = held ? use->prev - 1 : i + 1;
      if (use->fact != kill.fact) continue;
      use->fact = 0;
      if (!use->derived) {
        type_use_patch(c, use->at);
        continue;
      }
      if (count >= cap) {
        sv_type_kill_t *next = malloc((size_t)cap * 2 * sizeof(*next));
        if (!next) continue;
        memcpy(next, work, (size_t)count * sizeof(*next));
        if (work != inline_work) free(work);
        work = next;
        cap *= 2;
      }
      work[count++] = (sv_type_kill_t){.fact = use->derived, .start = use->at, .local = use->local};
    }
    
    if (held && kill.fact != first.fact) type_local_clear(&c->locals[kill.local]);
  }

  if (work != inline_work) free(work);
}

void type_assign_local_slow(sv_compiler_t *c, int local_idx, uint8_t type, int dep_base) {
  sv_local_t *local = &c->locals[local_idx];
  if (local->depth == -1) {
    local->inferred_type = type;
    return;
  }
  
  uint8_t old = local->inferred_type;
  if (sv_type_is_known(type) && !type_deps_live(c, dep_base)) type = SV_TI_UNKNOWN;
  if (!sv_type_is_known(type) && !sv_type_is_known(old)) return;
  
  if (type == old) {
    if (c->type_loop_depth && type_use_derived(c, dep_base, local_idx)) local->type_derived = true;
    return;
  }

  if (local->type_fact && c->type_loop_depth) {
    int start = type_region_start(c, local->type_fact);
    if (start < 0) start = type_capture_start(c, local);
    if (start >= 0) type_kill(c, (sv_type_kill_t){.fact = local->type_fact, .start = start, .local = local_idx});
  }
  
  type_local_clear(local);
  if (!sv_type_is_known(type)) return;
  
  local->inferred_type = type;
  local->type_fact = ++c->types->fact_next;
  c->type_any = true;
  if (c->type_loop_depth) local->type_derived = type_use_derived(c, dep_base, local_idx);
  if (!c->type_region_depth) return;
  
  if (c->types->promo_count >= c->types->promo_cap) {
    int cap = c->types->promo_cap ? c->types->promo_cap * 2 : 16;
    sv_type_dep_t *next = sv_compile_ctx_grow_types(
      c->types, c->types->promos, (size_t)c->types->promo_count * sizeof(*next), (size_t)cap * sizeof(*next));
    if (!next) {
      type_local_clear(local);
      return;
    }
    c->types->promos = next;
    c->types->promo_cap = cap;
  }
  c->types->promos[c->types->promo_count++] = (sv_type_dep_t){.local = local_idx, .fact = local->type_fact};
}

void type_region_leave(sv_compiler_t *c, int mark) {
  for (int i = mark; i < c->types->promo_count; i++) {
    const sv_type_dep_t *promo = &c->types->promos[i];
    if (!type_dep_live(c, promo)) continue;
    sv_local_t *local = &c->locals[promo->local];
    int start = c->type_loop_depth ? type_capture_start(c, local) : -1;
    if (start >= 0) type_kill(c, (sv_type_kill_t){.fact = promo->fact, .start = start, .local = promo->local});
    type_local_clear(local);
  }
  c->types->promo_count = mark;
  c->type_region_depth--;
}

void type_loop_leave(sv_compiler_t *c) {
  for (int i = c->param_locals; i < c->local_count; i++) {
    sv_local_t *local = &c->locals[i];
    if (!local->type_fact || !local->captured || local->is_const) continue;
    type_kill(c, (sv_type_kill_t){.fact = local->type_fact, .start = type_capture_start(c, local), .local = i});
    type_local_clear(local);
  }
}
