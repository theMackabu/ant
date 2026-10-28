#include "pass_facts.h"

typedef struct {
  uint64_t key[3];
  uint64_t value;
} pass_fact_entry_t;

struct pass_fact_table {
  uint32_t owner;
  uint32_t len;
  uint32_t cap;
  pass_fact_entry_t *items;
};

typedef struct {
  uint32_t len;
  ir_shape_check_t items[];
} pass_shape_list_t;

typedef uint8_t pass_kinds_t;

static constexpr pass_kinds_t PASS_KINDS_INT32 = 1u << 0;
static constexpr pass_kinds_t PASS_KINDS_DOUBLE = 1u << 1;
static constexpr pass_kinds_t PASS_KINDS_STRING = 1u << 2;
static constexpr pass_kinds_t PASS_KINDS_OBJECT = 1u << 3;
static constexpr pass_kinds_t PASS_KINDS_OTHER = 1u << 4;
static constexpr pass_kinds_t PASS_KINDS_ALL = (1u << 5) - 1;
static constexpr pass_kinds_t PASS_KINDS_NUMBER = PASS_KINDS_INT32 | PASS_KINDS_DOUBLE;

static pass_kinds_t pass_kinds_of(pass_value_kind_t kind) {
  switch (kind) {
    case PASS_KIND_INT32: return PASS_KINDS_INT32;
    case PASS_KIND_DOUBLE: return PASS_KINDS_DOUBLE;
    case PASS_KIND_NUMBER: return PASS_KINDS_NUMBER;
    case PASS_KIND_STRING: return PASS_KINDS_STRING;
    case PASS_KIND_OBJECT: return PASS_KINDS_OBJECT;
    case PASS_KIND_UNKNOWN: break;
  }
  return PASS_KINDS_ALL;
}

static pass_value_kind_t pass_kinds_kind(pass_kinds_t kinds) {
  switch (kinds) {
    case PASS_KINDS_INT32: return PASS_KIND_INT32;
    case PASS_KINDS_DOUBLE: return PASS_KIND_DOUBLE;
    case PASS_KINDS_NUMBER: return PASS_KIND_NUMBER;
    case PASS_KINDS_STRING: return PASS_KIND_STRING;
    case PASS_KINDS_OBJECT: return PASS_KIND_OBJECT;
    default: return PASS_KIND_UNKNOWN;
  }
}

static bool pass_kinds_of_condition(ir_condition_t condition, pass_kinds_t *passing, pass_kinds_t *failing) {
  pass_kinds_t exactly = 0;
  switch (condition.kind) {
    case IR_COND_OBJECT: exactly = PASS_KINDS_OBJECT; break;
    case IR_COND_STRING: exactly = PASS_KINDS_STRING; break;
    case IR_COND_INT32_VALUE: exactly = PASS_KINDS_INT32; break;
    case IR_COND_DOUBLE: exactly = PASS_KINDS_DOUBLE; break;
    case IR_COND_NON_NEGATIVE_INT32:
      *passing = PASS_KINDS_INT32;
      *failing = PASS_KINDS_ALL;
      return true;
    case IR_COND_ELEMENTS_KIND:
      *passing = PASS_KINDS_OBJECT;
      *failing = PASS_KINDS_ALL;
      return true;
    case IR_COND_NULLISH:
    case IR_COND_UNDEFINED:
      *passing = PASS_KINDS_OTHER | PASS_KINDS_OBJECT;
      *failing = PASS_KINDS_ALL;
      return true;
    default: return false;
  }
  *passing = exactly;
  *failing = PASS_KINDS_ALL & (pass_kinds_t)~exactly;
  return true;
}

static int pass_fact_key_compare(const uint64_t *a, const uint64_t *b) {
  for (uint32_t i = 0; i < 3; i++)
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}

static uint32_t pass_fact_find(const pass_fact_table_t *table, const uint64_t *key, bool *found) {
  uint32_t low = 0, high = table ? table->len : 0;
  *found = false;
  while (low < high) {
    uint32_t mid = low + (high - low) / 2;
    int order = pass_fact_key_compare(table->items[mid].key, key);
    if (order == 0) {
      *found = true;
      return mid;
    }
    if (order < 0) low = mid + 1;
    else high = mid;
  }
  return low;
}

static const pass_fact_entry_t *pass_fact_get(const pass_fact_table_t *table, uint64_t a, uint64_t b, uint64_t c) {
  bool found;
  uint32_t index = pass_fact_find(table, (uint64_t[]){a, b, c}, &found);
  return found ? &table->items[index] : NULL;
}

static inline uint32_t pass_fact_len(const pass_fact_table_t *table) {
  return table ? table->len : 0;
}

static pass_fact_table_t *pass_fact_mut(pass_facts_t *facts, pass_fact_table_t **slot) {
  pass_fact_table_t *table = *slot;
  if (table && table->owner == facts->id) return table;
  opt_arena_t *arena = facts->context->arena;
  pass_fact_table_t *copy = opt_arena_new(arena, pass_fact_table_t, 1);
  copy->owner = facts->id;
  if (table && table->len) {
    copy->cap = table->len + table->len / 8 + 4;
    copy->len = table->len;
    copy->items = opt_arena_alloc(arena, copy->cap * sizeof *table->items);
    memcpy(copy->items, table->items, table->len * sizeof *table->items);
  }
  *slot = copy;
  return copy;
}

static void pass_fact_insert(pass_facts_t *facts, pass_fact_table_t **slot, uint64_t a, uint64_t b, uint64_t c, uint64_t value) {
  pass_fact_table_t *table = pass_fact_mut(facts, slot);
  uint64_t key[3] = {a, b, c};
  bool found;
  uint32_t index = pass_fact_find(table, key, &found);
  if (found) {
    table->items[index].value = value;
    return;
  }
  pass_fact_entry_t entry = {.key = {a, b, c}, .value = value};
  opt_vec_insert(facts->context->arena, table, index, entry);
}

static void pass_fact_remove(pass_facts_t *facts, pass_fact_table_t **slot, uint64_t a, uint64_t b, uint64_t c) {
  pass_fact_table_t *table = pass_fact_mut(facts, slot);
  bool found;
  uint32_t index = pass_fact_find(table, (uint64_t[]){a, b, c}, &found);
  if (found) opt_vec_remove(table, index);
}

static void pass_fact_clear(pass_facts_t *facts, pass_fact_table_t **slot) {
  pass_fact_mut(facts, slot)->len = 0;
}

static bool pass_fact_tables_equal(const pass_fact_table_t *a, const pass_fact_table_t *b, bool shape_values) {
  uint32_t len = pass_fact_len(a);
  if (len != pass_fact_len(b)) return false;
  for (uint32_t i = 0; i < len; i++) {
    const pass_fact_entry_t *x = &a->items[i], *y = &b->items[i];
    if (pass_fact_key_compare(x->key, y->key)) return false;
    if (!shape_values) {
      if (x->value != y->value) return false;
      continue;
    }
    const pass_shape_list_t *xs = (const pass_shape_list_t *)(uintptr_t)x->value;
    const pass_shape_list_t *ys = (const pass_shape_list_t *)(uintptr_t)y->value;
    if (xs->len != ys->len) return false;
    for (uint32_t j = 0; j < xs->len; j++)
      if (!ir_shape_check_equal(xs->items[j], ys->items[j])) return false;
  }
  return true;
}

static pass_shape_list_t *pass_shape_list(opt_arena_t *arena, uint32_t len) {
  pass_shape_list_t *list = opt_arena_alloc(arena, sizeof *list + len * sizeof(ir_shape_check_t));
  list->len = len;
  return list;
}

static bool pass_shape_list_contains(const pass_shape_list_t *list, ir_shape_check_t shape) {
  for (uint32_t i = 0; i < list->len; i++)
    if (ir_shape_check_equal(list->items[i], shape)) return true;
  return false;
}

static inline const pass_shape_list_t *pass_shapes_of(const pass_fact_entry_t *entry) {
  return (const pass_shape_list_t *)(uintptr_t)entry->value;
}

void pass_facts_init(pass_facts_t *facts, pass_facts_context_t *context) {
  *facts = (pass_facts_t){.context = context, .id = ++context->next_id};
}

static void pass_fact_share(pass_fact_table_t *table) {
  if (table) table->owner = 0;
}

void pass_facts_clone(pass_facts_t *dst, const pass_facts_t *src) {
  pass_fact_share(src->kinds);
  pass_fact_share(src->checked_values);
  pass_fact_share(src->elements);
  pass_fact_share(src->shapes);
  pass_fact_share(src->valid_prototype_chains);
  pass_fact_share(src->contents);
  pass_fact_share(src->unexposed);
  pass_fact_share(src->refinements);
  pass_fact_share(src->branch_outcomes);
  *dst = *src;
  dst->id = ++src->context->next_id;
}

bool pass_facts_equal(const pass_facts_t *a, const pass_facts_t *b) {
  return a->code_valid == b->code_valid
    && pass_fact_tables_equal(a->kinds, b->kinds, false)
    && pass_fact_tables_equal(a->checked_values, b->checked_values, false)
    && pass_fact_tables_equal(a->elements, b->elements, false)
    && pass_fact_tables_equal(a->shapes, b->shapes, true)
    && pass_fact_tables_equal(a->valid_prototype_chains, b->valid_prototype_chains, false)
    && pass_fact_tables_equal(a->contents, b->contents, false)
    && pass_fact_tables_equal(a->unexposed, b->unexposed, false)
    && pass_fact_tables_equal(a->refinements, b->refinements, false)
    && pass_fact_tables_equal(a->branch_outcomes, b->branch_outcomes, false);
}

static pass_kinds_t pass_facts_kinds(const pass_facts_t *facts, ir_node_id_t value) {
  const pass_fact_entry_t *entry = pass_fact_get(facts->kinds, value, 0, 0);
  return entry ? (pass_kinds_t)entry->value : PASS_KINDS_ALL;
}

static pass_kinds_t pass_facts_kinds_in(const pass_facts_t *facts, const ir_graph_t *graph, ir_node_id_t value) {
  const ir_node_t *node = ir_node(graph, value);
  if (node->repr == OPT_REPR_INT32) return PASS_KINDS_INT32;
  if (node->repr == OPT_REPR_FLOAT64) return PASS_KINDS_NUMBER;
  if (node->op.code == IR_OP_BOX_INT32) return pass_facts_kinds(facts, value) & PASS_KINDS_INT32;
  if (node->op.code == IR_OP_BOX_FLOAT64) return pass_facts_kinds(facts, value) & PASS_KINDS_NUMBER;
  return pass_facts_kinds(facts, value);
}

static void pass_facts_narrow_kinds(pass_facts_t *facts, ir_node_id_t value, pass_kinds_t kinds) {
  pass_kinds_t narrowed = pass_facts_kinds(facts, value) & kinds;
  if (narrowed == PASS_KINDS_ALL) pass_fact_remove(facts, &facts->kinds, value, 0, 0);
  else pass_fact_insert(facts, &facts->kinds, value, 0, 0, narrowed);
}

pass_value_kind_t pass_facts_value_kind(const pass_facts_t *facts, ir_node_id_t value) {
  const pass_fact_entry_t *entry = pass_fact_get(facts->kinds, value, 0, 0);
  return entry ? pass_kinds_kind((pass_kinds_t)entry->value) : PASS_KIND_UNKNOWN;
}

void pass_facts_set_value_kind(pass_facts_t *facts, ir_node_id_t value, pass_value_kind_t kind) {
  pass_facts_narrow_kinds(facts, value, pass_kinds_of(kind));
}

bool pass_facts_is_object(const pass_facts_t *facts, ir_node_id_t value) {
  return pass_facts_value_kind(facts, value) == PASS_KIND_OBJECT;
}

void pass_facts_set_object(pass_facts_t *facts, ir_node_id_t value) {
  pass_facts_set_value_kind(facts, value, PASS_KIND_OBJECT);
}

bool pass_facts_is_about_kinds(ir_condition_t condition) {
  pass_kinds_t passing, failing;
  return pass_kinds_of_condition(condition, &passing, &failing);
}

int pass_facts_decides(const pass_facts_t *facts, const ir_graph_t *graph, ir_node_id_t value, ir_condition_t condition) {
  ir_elements_kind_t known;
  if (condition.kind == IR_COND_ELEMENTS_KIND && pass_facts_elements_kind(facts, value, &known))
    return pass_elements_kind_implies(known, condition.elements);
  pass_kinds_t passing, failing;
  if (!pass_kinds_of_condition(condition, &passing, &failing)) return -1;
  pass_kinds_t kinds = pass_facts_kinds_in(facts, graph, value);
  if (!(kinds & passing)) return 0;
  if (!(kinds & failing)) return 1;
  return -1;
}

void pass_facts_set_passed(pass_facts_t *facts, ir_node_id_t value, ir_condition_t condition) {
  pass_kinds_t passing, failing;
  if (pass_kinds_of_condition(condition, &passing, &failing)) pass_facts_narrow_kinds(facts, value, passing);
  if (condition.kind == IR_COND_ELEMENTS_KIND) pass_facts_set_elements_kind(facts, value, condition.elements);
}

void pass_facts_set_failed(pass_facts_t *facts, ir_node_id_t value, ir_condition_t condition) {
  pass_kinds_t passing, failing;
  if (pass_kinds_of_condition(condition, &passing, &failing)) pass_facts_narrow_kinds(facts, value, failing);
}

bool pass_branch_key_of(const ir_graph_t *graph, ir_condition_t condition, const ir_node_id_t *inputs, uint32_t count, pass_branch_key_t *key) {
  uint64_t parameter;
  switch (condition.kind) {
    case IR_COND_BOOL:
    case IR_COND_INDEX_IN_BOUNDS: parameter = 0; break;
    case IR_COND_INT32:
    case IR_COND_FLOAT64: parameter = condition.comparison; break;
    case IR_COND_TAGGED_EQUALS: parameter = condition.equal; break;
    default: return false;
  }
  if (count > 2) return false;
  uint64_t constants = 0;
  for (uint32_t i = 0; i < 2; i++) {
    uint64_t bits = 0;
    if (i >= count) key->key[1 + i] = UINT64_MAX;
    else if (ir_constant_value(graph, inputs[i], &bits)) {
      key->key[1 + i] = bits;
      constants |= 1u << i;
    } else key->key[1 + i] = inputs[i];
  }
  key->key[0] = (uint64_t)condition.kind << 40 | parameter << 8 | constants;
  return true;
}

int pass_facts_branch_outcome(const pass_facts_t *facts, const pass_branch_key_t *key) {
  const pass_fact_entry_t *entry = pass_fact_get(facts->branch_outcomes, key->key[0], key->key[1], key->key[2]);
  return entry ? (int)entry->value : -1;
}

void pass_facts_set_branch_outcome(pass_facts_t *facts, const pass_branch_key_t *key, bool holds) {
  if (pass_facts_branch_outcome(facts, key) == (int)holds) return;
  pass_fact_insert(facts, &facts->branch_outcomes, key->key[0], key->key[1], key->key[2], holds);
}

bool pass_facts_is_checked_value(const pass_facts_t *facts, ir_node_id_t value, uint64_t expected) {
  return pass_fact_get(facts->checked_values, value, expected, 0) != NULL;
}

void pass_facts_set_checked_value(pass_facts_t *facts, ir_node_id_t value, uint64_t expected) {
  pass_fact_insert(facts, &facts->checked_values, value, expected, 0, 0);
}

bool pass_facts_elements_kind(const pass_facts_t *facts, ir_node_id_t value, ir_elements_kind_t *kind) {
  const pass_fact_entry_t *entry = pass_fact_get(facts->elements, value, 0, 0);
  if (!entry) return false;
  *kind = (ir_elements_kind_t)entry->value;
  return true;
}

void pass_facts_set_elements_kind(pass_facts_t *facts, ir_node_id_t value, ir_elements_kind_t kind) {
  pass_fact_insert(facts, &facts->elements, value, 0, 0, kind);
}

const ir_shape_check_t *pass_facts_shapes(const pass_facts_t *facts, ir_node_id_t value, uint32_t *count) {
  const pass_fact_entry_t *entry = pass_fact_get(facts->shapes, value, 0, 0);
  if (!entry) return NULL;
  const pass_shape_list_t *list = pass_shapes_of(entry);
  *count = list->len;
  return list->items;
}

void pass_facts_set_shapes(pass_facts_t *facts, ir_node_id_t value, const ir_shape_check_t *shapes, uint32_t count) {
  const pass_fact_entry_t *entry = pass_fact_get(facts->shapes, value, 0, 0);
  pass_shape_list_t *narrowed = pass_shape_list(facts->context->arena, count);
  narrowed->len = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (entry && !pass_shape_list_contains(pass_shapes_of(entry), shapes[i])) continue;
    narrowed->items[narrowed->len++] = shapes[i];
  }
  pass_fact_insert(facts, &facts->shapes, value, 0, 0, (uint64_t)(uintptr_t)narrowed);
}

bool pass_facts_is_prototype_chain_valid(const pass_facts_t *facts, ir_cell_t validity) {
  return pass_fact_get(facts->valid_prototype_chains, validity, 0, 0) != NULL;
}

void pass_facts_set_prototype_chain_valid(pass_facts_t *facts, ir_cell_t validity) {
  pass_fact_insert(facts, &facts->valid_prototype_chains, validity, 0, 0, 0);
}

bool pass_facts_contents(const pass_facts_t *facts, pass_location_t location, ir_node_id_t *value) {
  const pass_fact_entry_t *entry = pass_fact_get(facts->contents, location.kind, location.a, location.b);
  if (!entry) return false;
  *value = (ir_node_id_t)entry->value;
  return true;
}

void pass_facts_set_contents(pass_facts_t *facts, pass_location_t location, ir_node_id_t value) {
  pass_fact_insert(facts, &facts->contents, location.kind, location.a, location.b, value);
}

static bool pass_shape_lists_intersect(const pass_shape_list_t *a, const pass_shape_list_t *b) {
  for (uint32_t i = 0; i < a->len; i++)
    if (pass_shape_list_contains(b, a->items[i])) return true;
  return false;
}

void pass_facts_forget_shapes_of(pass_facts_t *facts, ir_node_id_t object) {
  const pass_fact_entry_t *entry = pass_fact_get(facts->shapes, object, 0, 0);
  if (!entry) {
    pass_fact_clear(facts, &facts->shapes);
    return;
  }
  const pass_shape_list_t *known = pass_shapes_of(entry);
  pass_fact_table_t *table = pass_fact_mut(facts, &facts->shapes);
  uint32_t kept = 0;
  for (uint32_t i = 0; i < table->len; i++)
    if (!pass_shape_lists_intersect(pass_shapes_of(&table->items[i]), known)) table->items[kept++] = table->items[i];
  table->len = kept;
}

void pass_facts_forget_named_properties_at(pass_facts_t *facts, uint32_t offset) {
  pass_fact_table_t *table = pass_fact_mut(facts, &facts->contents);
  uint32_t kept = 0;
  for (uint32_t i = 0; i < table->len; i++) {
    const uint64_t *key = table->items[i].key;
    if (key[0] == PASS_LOC_NAMED && key[2] == offset) continue;
    table->items[kept++] = table->items[i];
  }
  table->len = kept;
}

void pass_facts_forget_bindings_at(pass_facts_t *facts, uint32_t index) {
  pass_fact_table_t *table = pass_fact_mut(facts, &facts->contents);
  uint32_t kept = 0;
  for (uint32_t i = 0; i < table->len; i++) {
    const uint64_t *key = table->items[i].key;
    if ((key[0] == PASS_LOC_GLOBAL_BINDING || key[0] == PASS_LOC_BINDING) && key[2] == index) continue;
    table->items[kept++] = table->items[i];
  }
  table->len = kept;
}

bool pass_facts_is_unexposed(const pass_facts_t *facts, ir_node_id_t object) {
  return pass_fact_get(facts->unexposed, object, 0, 0) != NULL;
}

void pass_facts_set_unexposed(pass_facts_t *facts, ir_node_id_t object) {
  pass_fact_insert(facts, &facts->unexposed, object, 0, 0, 0);
}

void pass_facts_expose(pass_facts_t *facts, ir_node_id_t object) {
  if (pass_facts_is_unexposed(facts, object)) pass_fact_remove(facts, &facts->unexposed, object, 0, 0);
}

ir_node_id_t pass_facts_refinement(const pass_facts_t *facts, ir_node_id_t value) {
  const pass_fact_entry_t *entry = pass_fact_get(facts->refinements, value, 0, 0);
  return entry ? (ir_node_id_t)entry->value : IR_NONE;
}

void pass_facts_set_refinement(pass_facts_t *facts, ir_node_id_t value, ir_node_id_t refinement) {
  pass_fact_insert(facts, &facts->refinements, value, 0, 0, refinement);
}

void pass_facts_forget_refinement(pass_facts_t *facts, ir_node_id_t value) {
  if (pass_fact_get(facts->refinements, value, 0, 0)) pass_fact_remove(facts, &facts->refinements, value, 0, 0);
}

static ir_locations_t pass_location_locations(pass_location_kind_t kind) {
  switch (kind) {
    case PASS_LOC_NAMED: return IR_LOC_NAMED_SLOTS;
    case PASS_LOC_GLOBAL_BINDING: return IR_LOC_GLOBAL_BINDINGS;
    case PASS_LOC_BINDING: return IR_LOC_BINDINGS | IR_LOC_GLOBAL_BINDINGS;
    case PASS_LOC_ELEMENT_COUNT:
    case PASS_LOC_TYPED_ARRAY_DATA_OFFSET: return IR_LOC_ELEMENT_COUNTS;
    case PASS_LOC_ELEMENT: return IR_LOC_ELEMENTS_AND_COUNTS;
    case PASS_LOC_NAMED_STORAGE: return IR_LOC_SHAPES;
  }
  return IR_LOC_ALL;
}

static ir_node_id_t pass_location_object(const uint64_t *key) {
  switch ((pass_location_kind_t)key[0]) {
    case PASS_LOC_NAMED:
    case PASS_LOC_NAMED_STORAGE:
    case PASS_LOC_TYPED_ARRAY_DATA_OFFSET: return (ir_node_id_t)key[1];
    case PASS_LOC_ELEMENT_COUNT: return (ir_node_id_t)key[2];
    default: return IR_NONE;
  }
}

static bool pass_location_killed(const pass_facts_t *facts, const uint64_t *key, ir_locations_t writes) {
  if (!ir_locations_intersect(pass_location_locations((pass_location_kind_t)key[0]), writes)) return false;
  ir_node_id_t object = pass_location_object(key);
  return object == IR_NONE || !pass_facts_is_unexposed(facts, object);
}

void pass_facts_apply(pass_facts_t *facts, ir_node_properties_t properties) {
  ir_locations_t writes = properties.writes;
  if (properties.runs_code) facts->code_valid = false;
  if (writes == IR_LOC_NONE) return;
  bool any_killed = false;
  for (uint32_t i = 0; i < pass_fact_len(facts->contents) && !any_killed; i++)
    any_killed = pass_location_killed(facts, facts->contents->items[i].key, writes);
  if (any_killed) {
    pass_fact_table_t *table = pass_fact_mut(facts, &facts->contents);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < table->len; i++)
      if (!pass_location_killed(facts, table->items[i].key, writes)) table->items[kept++] = table->items[i];
    table->len = kept;
  }
  if (ir_locations_intersect(writes, IR_LOC_ELEMENT_COUNTS)) {
    pass_fact_table_t *table = pass_fact_mut(facts, &facts->elements);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < table->len; i++)
      if (ir_elements_is_typed((ir_elements_kind_t)table->items[i].value)) table->items[kept++] = table->items[i];
    table->len = kept;
  }
  if (ir_locations_intersect(writes, IR_LOC_SHAPES)) {
    pass_fact_table_t *shapes = pass_fact_mut(facts, &facts->shapes);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < shapes->len; i++) {
      ir_node_id_t value = (ir_node_id_t)shapes->items[i].key[0];
      if (pass_facts_is_unexposed(facts, value)) shapes->items[kept++] = shapes->items[i];
    }
    shapes->len = kept;
    pass_fact_clear(facts, &facts->valid_prototype_chains);
  }
}

typedef enum {
  PASS_MERGE_INTERSECT,
  PASS_MERGE_SAME_VALUE,
  PASS_MERGE_UNION_KINDS,
  PASS_MERGE_UNION_SHAPES,
} pass_merge_mode_t;

static void pass_merge_table(pass_facts_t *merged, pass_fact_table_t **slot, const pass_fact_table_t *other, pass_merge_mode_t mode) {
  if (*slot == other) return;
  pass_fact_table_t *table = pass_fact_mut(merged, slot);
  uint32_t kept = 0;
  for (uint32_t i = 0; i < table->len; i++) {
    pass_fact_entry_t entry = table->items[i];
    const pass_fact_entry_t *match = pass_fact_get(other, entry.key[0], entry.key[1], entry.key[2]);
    if (!match) continue;
    switch (mode) {
      case PASS_MERGE_INTERSECT: break;
      case PASS_MERGE_SAME_VALUE:
        if (match->value != entry.value) continue;
        break;
      case PASS_MERGE_UNION_KINDS: entry.value |= match->value; break;
      case PASS_MERGE_UNION_SHAPES: {
        const pass_shape_list_t *mine = pass_shapes_of(&entry), *theirs = pass_shapes_of(match);
        pass_shape_list_t *united = pass_shape_list(merged->context->arena, mine->len + theirs->len);
        memcpy(united->items, mine->items, mine->len * sizeof *mine->items);
        united->len = mine->len;
        for (uint32_t j = 0; j < theirs->len; j++)
          if (!pass_shape_list_contains(united, theirs->items[j])) united->items[united->len++] = theirs->items[j];
        entry.value = (uint64_t)(uintptr_t)united;
        break;
      }
    }
    table->items[kept++] = entry;
  }
  table->len = kept;
}

void pass_facts_merge(pass_facts_t *merged, const pass_facts_t *const *facts, uint32_t count, pass_facts_context_t *context) {
  if (!count) {
    pass_facts_init(merged, context);
    return;
  }
  pass_facts_clone(merged, facts[0]);
  for (uint32_t f = 1; f < count; f++) {
    const pass_facts_t *other = facts[f];
    pass_merge_table(merged, &merged->kinds, other->kinds, PASS_MERGE_UNION_KINDS);
    pass_merge_table(merged, &merged->checked_values, other->checked_values, PASS_MERGE_INTERSECT);
    pass_merge_table(merged, &merged->elements, other->elements, PASS_MERGE_SAME_VALUE);
    pass_merge_table(merged, &merged->valid_prototype_chains, other->valid_prototype_chains, PASS_MERGE_INTERSECT);
    merged->code_valid &= other->code_valid;
    pass_merge_table(merged, &merged->contents, other->contents, PASS_MERGE_SAME_VALUE);
    pass_merge_table(merged, &merged->unexposed, other->unexposed, PASS_MERGE_INTERSECT);
    pass_merge_table(merged, &merged->refinements, other->refinements, PASS_MERGE_SAME_VALUE);
    pass_merge_table(merged, &merged->branch_outcomes, other->branch_outcomes, PASS_MERGE_SAME_VALUE);
    pass_merge_table(merged, &merged->shapes, other->shapes, PASS_MERGE_UNION_SHAPES);
  }
}

int pass_elements_kind_implies(ir_elements_kind_t known, ir_elements_kind_t kind) {
  if (known == kind) return 1;
  if (known == IR_ELEMENTS_PACKED && kind == IR_ELEMENTS_HOLEY) return 1;
  if (known == IR_ELEMENTS_HOLEY && kind == IR_ELEMENTS_PACKED) return -1;
  return 0;
}

bool pass_element_count_loaded_by(const ir_op_t *op, pass_element_count_t *count) {
  switch (op->code) {
    case IR_OP_LOAD_ELEMENTS_LENGTH: *count = PASS_COUNT_LENGTH; return true;
    case IR_OP_LOAD_ELEMENTS_CAPACITY: *count = PASS_COUNT_CAPACITY; return true;
    case IR_OP_LOAD_TYPED_ARRAY_LENGTH: *count = PASS_COUNT_TYPED_ARRAY_LENGTH; return true;
    default: return false;
  }
}
