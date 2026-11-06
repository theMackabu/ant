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
    sv_write_scan_t scan = {.mark = value_params_mark, .data = &params, .arguments_poisons = !c->is_strict};
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

uint8_t ctor_slot_class(const sv_ast_t *node) {
  if (!node || (node->flags & (FN_ARROW | FN_GENERATOR | FN_ASYNC))) return 0;
  if (node->this_props <= ANT_INOBJ_BASE_SLOTS) return 0;
  return node->this_props <= ANT_INOBJ_WIDE_SLOTS ? 1 : 2;
}
