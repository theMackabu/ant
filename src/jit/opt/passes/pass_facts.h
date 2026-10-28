#ifndef ANT_JIT_OPT_PASS_FACTS_H
#define ANT_JIT_OPT_PASS_FACTS_H

#include "passes.h"

typedef enum : uint8_t {
  PASS_KIND_UNKNOWN,
  PASS_KIND_INT32,
  PASS_KIND_DOUBLE,
  PASS_KIND_NUMBER,
  PASS_KIND_STRING,
  PASS_KIND_OBJECT,
} pass_value_kind_t;

typedef enum : uint8_t {
  PASS_LOC_NAMED,
  PASS_LOC_GLOBAL_BINDING,
  PASS_LOC_BINDING,
  PASS_LOC_ELEMENT_COUNT,
  PASS_LOC_TYPED_ARRAY_DATA_OFFSET,
  PASS_LOC_ELEMENT,
  PASS_LOC_NAMED_STORAGE,
} pass_location_kind_t;

typedef enum : uint8_t {
  PASS_COUNT_LENGTH,
  PASS_COUNT_CAPACITY,
  PASS_COUNT_TYPED_ARRAY_LENGTH,
} pass_element_count_t;

typedef struct {
  pass_location_kind_t kind;
  uint64_t a;
  uint64_t b;
} pass_location_t;

bool pass_element_count_loaded_by(const ir_op_t *op, pass_element_count_t *count);

typedef struct pass_fact_table pass_fact_table_t;

typedef struct {
  opt_arena_t *arena;
  uint32_t next_id;
} pass_facts_context_t;

typedef struct {
  pass_facts_context_t *context;
  uint32_t id;
  pass_fact_table_t *kinds;
  pass_fact_table_t *checked_values;
  pass_fact_table_t *elements;
  pass_fact_table_t *shapes;
  pass_fact_table_t *valid_prototype_chains;
  pass_fact_table_t *contents;
  pass_fact_table_t *unexposed;
  pass_fact_table_t *refinements;
  pass_fact_table_t *branch_outcomes;
  bool code_valid;
} pass_facts_t;

typedef struct {
  uint64_t key[3];
} pass_branch_key_t;

void pass_facts_init(pass_facts_t *facts, pass_facts_context_t *context);
void pass_facts_clone(pass_facts_t *dst, const pass_facts_t *src);
bool pass_facts_equal(const pass_facts_t *a, const pass_facts_t *b);

static inline bool pass_facts_is_code_valid(const pass_facts_t *facts) {
  return facts->code_valid;
}

static inline void pass_facts_assume_code_valid(pass_facts_t *facts) {
  facts->code_valid = true;
}

pass_value_kind_t pass_facts_value_kind(const pass_facts_t *facts, ir_node_id_t value);
void pass_facts_set_value_kind(pass_facts_t *facts, ir_node_id_t value, pass_value_kind_t kind);
bool pass_facts_is_object(const pass_facts_t *facts, ir_node_id_t value);
void pass_facts_set_object(pass_facts_t *facts, ir_node_id_t value);

bool pass_facts_is_about_kinds(ir_condition_t condition);
int pass_facts_decides(const pass_facts_t *facts, const ir_graph_t *graph, ir_node_id_t value, ir_condition_t condition);
void pass_facts_set_passed(pass_facts_t *facts, ir_node_id_t value, ir_condition_t condition);
void pass_facts_set_failed(pass_facts_t *facts, ir_node_id_t value, ir_condition_t condition);

bool pass_branch_key_of(const ir_graph_t *graph, ir_condition_t condition, const ir_node_id_t *inputs, uint32_t count, pass_branch_key_t *key);
int pass_facts_branch_outcome(const pass_facts_t *facts, const pass_branch_key_t *key);
void pass_facts_set_branch_outcome(pass_facts_t *facts, const pass_branch_key_t *key, bool holds);

bool pass_facts_is_checked_value(const pass_facts_t *facts, ir_node_id_t value, uint64_t expected);
void pass_facts_set_checked_value(pass_facts_t *facts, ir_node_id_t value, uint64_t expected);

bool pass_facts_elements_kind(const pass_facts_t *facts, ir_node_id_t value, ir_elements_kind_t *kind);
void pass_facts_set_elements_kind(pass_facts_t *facts, ir_node_id_t value, ir_elements_kind_t kind);

const ir_shape_check_t *pass_facts_shapes(const pass_facts_t *facts, ir_node_id_t value, uint32_t *count);
void pass_facts_set_shapes(pass_facts_t *facts, ir_node_id_t value, const ir_shape_check_t *shapes, uint32_t count);

bool pass_facts_is_prototype_chain_valid(const pass_facts_t *facts, ir_cell_t validity);
void pass_facts_set_prototype_chain_valid(pass_facts_t *facts, ir_cell_t validity);

bool pass_facts_contents(const pass_facts_t *facts, pass_location_t location, ir_node_id_t *value);
void pass_facts_set_contents(pass_facts_t *facts, pass_location_t location, ir_node_id_t value);
void pass_facts_forget_shapes_of(pass_facts_t *facts, ir_node_id_t object);
void pass_facts_forget_named_properties_at(pass_facts_t *facts, uint32_t offset);
void pass_facts_forget_bindings_at(pass_facts_t *facts, uint32_t index);

bool pass_facts_is_unexposed(const pass_facts_t *facts, ir_node_id_t object);
void pass_facts_set_unexposed(pass_facts_t *facts, ir_node_id_t object);
void pass_facts_expose(pass_facts_t *facts, ir_node_id_t object);

ir_node_id_t pass_facts_refinement(const pass_facts_t *facts, ir_node_id_t value);
void pass_facts_set_refinement(pass_facts_t *facts, ir_node_id_t value, ir_node_id_t refinement);
void pass_facts_forget_refinement(pass_facts_t *facts, ir_node_id_t value);

void pass_facts_apply(pass_facts_t *facts, ir_node_properties_t properties);
void pass_facts_merge(pass_facts_t *merged, const pass_facts_t *const *facts, uint32_t count, pass_facts_context_t *context);
int pass_elements_kind_implies(ir_elements_kind_t known, ir_elements_kind_t kind);

#endif
