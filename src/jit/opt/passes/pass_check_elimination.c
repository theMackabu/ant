#include "pass_facts.h"
#include "../ir/ir_value.h"
#include <assert.h>

typedef struct {
  ir_node_id_t node;
  ir_op_t op;
} pass_control_decision_t;

typedef struct {
  pass_replacements_t replacements;
  opt_bitset_t removed;
  OPT_VEC(pass_control_decision_t) controls;
} pass_decisions_t;

typedef struct {
  ir_block_id_t target;
  pass_facts_t facts;
} pass_edge_facts_t;

typedef OPT_VEC(pass_edge_facts_t) pass_edge_facts_vec_t;

typedef struct {
  ir_graph_t *graph;
  opt_arena_t *arena;
  pass_facts_context_t *context;
  pass_constants_t *constants;
  pass_decisions_t *decisions;
} pass_check_elimination_t;

static ir_node_id_t pass_decided_value(const pass_check_elimination_t *pass, ir_node_id_t value) {
  value = pass_resolve(&pass->decisions->replacements, value);
  for (;;) {
    ir_node_id_t replaced = pass_resolve(&pass->decisions->replacements, ir_unrefined(pass->graph, value));
    if (replaced == value) return value;
    value = replaced;
  }
}

static bool pass_facts_at_entry(pass_check_elimination_t *pass, ir_block_id_t block, pass_edge_facts_vec_t *edge_facts, const bool *visited, pass_facts_t *facts) {
  const ir_block_vec_t *predecessors = &ir_block(pass->graph, block)->predecessors;
  if (!predecessors->len) {
    pass_facts_init(facts, pass->context);
    pass_facts_assume_code_valid(facts);
    return true;
  }
  OPT_VEC(const pass_facts_t *) incoming = {0};
  OPT_VEC(pass_facts_t) taken = {0};
  for (uint32_t i = 0; i < predecessors->len; i++) {
    ir_block_id_t predecessor = predecessors->items[i];
    bool is_back_edge = predecessor >= block;
    if (!is_back_edge && !visited[predecessor]) continue;
    pass_edge_facts_vec_t *edges = &edge_facts[predecessor];
    uint32_t kept = 0;
    for (uint32_t j = 0; j < edges->len; j++) {
      if (edges->items[j].target == block) opt_vec_push(pass->arena, &taken, edges->items[j].facts);
      else edges->items[kept++] = edges->items[j];
    }
    edges->len = kept;
  }
  if (!taken.len) return false;
  if (taken.len == 1) {
    *facts = taken.items[0];
    return true;
  }
  for (uint32_t i = 0; i < taken.len; i++) opt_vec_push(pass->arena, &incoming, &taken.items[i]);
  pass_facts_merge(facts, incoming.items, incoming.len, pass->context);
  return true;
}

static void pass_push_edge(pass_check_elimination_t *pass, pass_edge_facts_vec_t *edges, ir_block_id_t target, const pass_facts_t *facts) {
  pass_edge_facts_t edge = {.target = target, .facts = *facts};
  opt_vec_push(pass->arena, edges, edge);
}

static void pass_visit_node(pass_check_elimination_t *pass, ir_node_id_t node_id, pass_facts_t *facts);

static void pass_visit_block(pass_check_elimination_t *pass, ir_block_id_t block, pass_facts_t *facts, pass_edge_facts_vec_t *edges) {
  ir_graph_t *graph = pass->graph;
  pass_decisions_t *decisions = pass->decisions;
  edges->len = 0;
  const ir_block_t *data = ir_block(graph, block);
  for (uint32_t p = 0; p < data->phis.len; p++) {
    const ir_node_t *phi = ir_node(graph, data->phis.items[p]);
    for (uint32_t i = 0; i < phi->inputs.len; i++) pass_facts_expose(facts, pass_decided_value(pass, phi->inputs.items[i]));
  }
  ir_node_vec_t body = {0};
  opt_vec_clone(pass->arena, &body, &ir_block(graph, block)->body);
  for (uint32_t i = 0; i < body.len; i++) pass_visit_node(pass, body.items[i], facts);

  ir_node_id_t control = ir_control_id(graph, block);
  ir_op_t op = ir_node(graph, control)->op;
  if (op.code == IR_OP_BRANCH && pass_facts_is_about_kinds(op.branch.condition)) {
    ir_node_id_t value = pass_decided_value(pass, ir_node(graph, control)->inputs.items[0]);
    int holds = pass_facts_decides(facts, graph, value, op.branch.condition);
    if (holds >= 0) {
      ir_block_id_t target = holds ? op.branch.if_true : op.branch.if_false;
      pass_control_decision_t decision = {.node = control, .op = ir_op(IR_OP_JUMP)};
      decision.op.target = target;
      opt_vec_push(pass->arena, &decisions->controls, decision);
      pass_push_edge(pass, edges, target, facts);
      return;
    }
    pass_facts_t passed;
    pass_facts_clone(&passed, facts);
    pass_facts_forget_refinement(&passed, value);
    pass_facts_set_passed(&passed, value, op.branch.condition);
    pass_facts_set_failed(facts, value, op.branch.condition);
    pass_push_edge(pass, edges, op.branch.if_true, &passed);
    pass_push_edge(pass, edges, op.branch.if_false, facts);
    return;
  }
  if (op.code == IR_OP_BRANCH) {
    const ir_node_t *branch = ir_node(graph, control);
    ir_node_id_t inputs[2];
    uint32_t count = branch->inputs.len < 2 ? branch->inputs.len : 2;
    for (uint32_t i = 0; i < count; i++) inputs[i] = pass_decided_value(pass, branch->inputs.items[i]);
    pass_branch_key_t key;
    if (branch->inputs.len <= 2 && pass_branch_key_of(graph, op.branch.condition, inputs, count, &key)) {
      int holds = pass_facts_branch_outcome(facts, &key);
      if (holds >= 0) {
        ir_block_id_t target = holds ? op.branch.if_true : op.branch.if_false;
        pass_control_decision_t decision = {.node = control, .op = ir_op(IR_OP_JUMP)};
        decision.op.target = target;
        opt_vec_push(pass->arena, &decisions->controls, decision);
        pass_push_edge(pass, edges, target, facts);
        return;
      }
      pass_facts_t passed;
      pass_facts_clone(&passed, facts);
      pass_facts_set_branch_outcome(&passed, &key, true);
      pass_facts_set_branch_outcome(facts, &key, false);
      pass_push_edge(pass, edges, op.branch.if_true, &passed);
      pass_push_edge(pass, edges, op.branch.if_false, facts);
      return;
    }
  }
  if (op.code != IR_OP_SHAPE_SWITCH) {
    ir_successors_t successors = ir_op_successors(&op);
    for (uint32_t i = 0; i < successors.len; i++) {
      pass_facts_t copy;
      if (i + 1 < successors.len) pass_facts_clone(&copy, facts);
      else copy = *facts;
      pass_push_edge(pass, edges, ir_successor_at(&successors, i), &copy);
    }
    return;
  }
  ir_node_id_t object = pass_decided_value(pass, ir_node(graph, control)->inputs.items[0]);
  uint32_t known_count = 0;
  const ir_shape_check_t *known = pass_facts_shapes(facts, object, &known_count);
  ir_shape_case_t *possible = opt_arena_alloc(pass->arena, (op.cases.len + 1) * sizeof *possible);
  uint32_t possible_count = 0;
  for (uint32_t i = 0; i < op.cases.len; i++) {
    bool keep = !known || !op.cases.items[i].check.shape;
    for (uint32_t j = 0; known && j < known_count && !keep; j++) keep = ir_shape_check_equal(known[j], op.cases.items[i].check);
    if (keep) possible[possible_count++] = op.cases.items[i];
  }
  bool covers_known = known != NULL;
  for (uint32_t j = 0; known && j < known_count && covers_known; j++) {
    bool covered = false;
    for (uint32_t i = 0; i < possible_count && !covered; i++) covered = ir_shape_check_equal(possible[i].check, known[j]);
    covers_known = covered;
  }
  if (possible_count == 1 && covers_known) {
    pass_control_decision_t decision = {.node = control, .op = ir_op(IR_OP_JUMP)};
    decision.op.target = possible[0].block;
    opt_vec_push(pass->arena, &decisions->controls, decision);
  } else if (possible_count < op.cases.len && possible_count) {
    pass_control_decision_t decision = {.node = control, .op = ir_op(IR_OP_SHAPE_SWITCH)};
    decision.op.cases.items = possible;
    decision.op.cases.len = possible_count;
    opt_vec_push(pass->arena, &decisions->controls, decision);
  }
  for (uint32_t i = 0; i < possible_count; i++) {
    pass_facts_t case_facts;
    pass_facts_clone(&case_facts, facts);
    if (possible[i].check.shape) pass_facts_set_shapes(&case_facts, object, &possible[i].check, 1);
    pass_push_edge(pass, edges, possible[i].block, &case_facts);
  }
}

static bool pass_accesses_properties(const ir_op_t *op, uint32_t index) {
  switch (op->code) {
    case IR_OP_CHECK_OBJECT:
    case IR_OP_CELL_ADDRESS:
    case IR_OP_INITIALIZE_NAMED: return index == 0;
    case IR_OP_CHECK_SHAPE:
    case IR_OP_SHAPE_SWITCH:
    case IR_OP_LOAD_NAMED:
    case IR_OP_LOAD_NAMED_STORAGE:
    case IR_OP_CHECK_ACCESSOR_FUNCTION:
    case IR_OP_LOAD_ACCESSOR_FUNCTION: return true;
    case IR_OP_STORE_NAMED:
    case IR_OP_ADD_NAMED: return index != 1;
    default: return false;
  }
}

static bool pass_loaded_location(const pass_check_elimination_t *pass, ir_node_id_t load, pass_location_t *location) {
  const ir_graph_t *graph = pass->graph;
  const ir_node_t *node = ir_node(graph, load);
  switch (node->op.code) {
    case IR_OP_LOAD_NAMED:
      *location = (pass_location_t){PASS_LOC_NAMED, pass_decided_value(pass, node->inputs.items[0]), node->op.offset};
      return true;
    case IR_OP_LOAD_GLOBAL_BINDING:
      *location = (pass_location_t){PASS_LOC_GLOBAL_BINDING, node->op.global_binding.environment, node->op.global_binding.index};
      return true;
    case IR_OP_LOAD_ENVIRONMENT_BINDING:
      *location = (pass_location_t){PASS_LOC_BINDING, pass_decided_value(pass, node->inputs.items[0]), node->op.index};
      return true;
    default: break;
  }
  pass_element_count_t count;
  bool is_count = pass_element_count_loaded_by(&node->op, &count);
  bool is_element = node->op.code == IR_OP_LOAD_ELEMENT_AT;
  if (!is_count && !is_element && node->op.code != IR_OP_LOAD_TYPED_ARRAY_DATA_OFFSET) return false;
  ir_node_id_t input = pass_decided_value(pass, node->inputs.items[0]);
  const ir_node_t *address = ir_node(graph, input);
  ir_node_id_t object = address->op.code == IR_OP_CELL_ADDRESS ? pass_decided_value(pass, address->inputs.items[0]) : input;
  if (is_element) {
    ir_node_id_t index = ir_unrefined(graph, pass_decided_value(pass, node->inputs.items[1]));
    while (ir_node(graph, index)->op.code == IR_OP_CHECK_INT32) index = ir_unrefined(graph, ir_node(graph, index)->inputs.items[0]);
    *location = (pass_location_t){PASS_LOC_ELEMENT, object, (uint64_t)node->op.elements << 32 | index};
  } else if (is_count) *location = (pass_location_t){PASS_LOC_ELEMENT_COUNT, count, object};
  else *location = (pass_location_t){PASS_LOC_TYPED_ARRAY_DATA_OFFSET, object, 0};
  return true;
}

static void pass_new_object(pass_facts_t *facts, ir_node_id_t object) {
  pass_facts_set_object(facts, object);
  pass_facts_set_refinement(facts, object, object);
}

static bool pass_reuse_contents(pass_check_elimination_t *pass, pass_facts_t *facts, pass_location_t location, ir_node_id_t node) {
  ir_node_id_t value;
  if (pass_facts_contents(facts, location, &value)) {
    pass_replace(&pass->decisions->replacements, node, value);
    return true;
  }
  pass_facts_set_contents(facts, location, node);
  return false;
}

static void pass_visit_node(pass_check_elimination_t *pass, ir_node_id_t node_id, pass_facts_t *facts) {
  ir_graph_t *graph = pass->graph;
  pass_decisions_t *decisions = pass->decisions;
  ir_op_t op = ir_node(graph, node_id)->op;
  opt_repr_t repr = ir_node(graph, node_id)->repr;
  ir_node_vec_t inputs = {0};
  for (uint32_t i = 0; i < ir_node(graph, node_id)->inputs.len; i++)
    opt_vec_push(pass->arena, &inputs, pass_decided_value(pass, ir_node(graph, node_id)->inputs.items[i]));
  for (uint32_t i = 0; i < inputs.len; i++)
    if (pass_facts_is_unexposed(facts, inputs.items[i]) && !pass_accesses_properties(&op, i)) pass_facts_expose(facts, inputs.items[i]);
  bool refines = ir_op_refined_input(&op) >= 0 && repr != OPT_REPR_NONE;
  ir_node_id_t anchor = IR_NONE;
  if (refines) {
    anchor = pass_facts_refinement(facts, inputs.items[0]);
    if (anchor == IR_NONE && ir_is_constant(graph, inputs.items[0])) anchor = inputs.items[0];
  }
  bool removable = !refines || anchor != IR_NONE;
  bool remove = false;
  switch (op.code) {
    case IR_OP_CHECK_OBJECT:
      remove = removable && pass_facts_is_object(facts, inputs.items[0]);
      pass_facts_set_object(facts, inputs.items[0]);
      break;
    case IR_OP_CHECK_SHAPE: {
      ir_node_id_t object = inputs.items[0];
      uint32_t known_count;
      const ir_shape_check_t *known = pass_facts_shapes(facts, object, &known_count);
      if (removable && known) {
        remove = true;
        for (uint32_t i = 0; i < known_count && remove; i++) {
          bool listed = false;
          for (uint32_t j = 0; j < op.shapes.len && !listed; j++) listed = ir_shape_check_equal(op.shapes.items[j], known[i]);
          remove = listed;
        }
      }
      if (!remove) pass_facts_set_shapes(facts, object, op.shapes.items, op.shapes.len);
      break;
    }
    case IR_OP_CHECK_INT32: pass_facts_set_value_kind(facts, inputs.items[0], PASS_KIND_INT32); break;
    case IR_OP_CHECK_STRING:
      remove = removable && pass_facts_value_kind(facts, inputs.items[0]) == PASS_KIND_STRING;
      pass_facts_set_value_kind(facts, inputs.items[0], PASS_KIND_STRING);
      break;
    case IR_OP_REFINE:
      remove = anchor != IR_NONE && pass_facts_decides(facts, graph, inputs.items[0], op.refine.condition) == (int)op.refine.holds;
      break;
    case IR_OP_ASSUME_VALID:
      remove = pass_facts_is_code_valid(facts);
      pass_facts_assume_code_valid(facts);
      break;
    case IR_OP_CHECK_PROTOTYPE_CHAIN_VALID:
      remove = pass_facts_is_prototype_chain_valid(facts, op.validity);
      pass_facts_set_prototype_chain_valid(facts, op.validity);
      break;
    case IR_OP_CHECK_VALUE: {
      ir_node_id_t value = inputs.items[0];
      uint64_t bits;
      bool constant = ir_constant_value(graph, value, &bits) && bits == op.check_value.expected;
      remove = removable && (constant || pass_facts_is_checked_value(facts, value, op.check_value.expected));
      if (!remove) {
        pass_facts_set_checked_value(facts, value, op.check_value.expected);
        pass_location_t location;
        ir_node_id_t contents;
        if (pass_loaded_location(pass, value, &location)
          && (location.kind == PASS_LOC_NAMED || location.kind == PASS_LOC_GLOBAL_BINDING)
          && pass_facts_contents(facts, location, &contents) && contents == value) {
          ir_node_id_t constant_node = pass_constant(pass->constants, graph, op.check_value.expected);
          pass_facts_set_contents(facts, location, constant_node);
        }
      }
      break;
    }
    case IR_OP_LOAD_NAMED:
    case IR_OP_LOAD_GLOBAL_BINDING:
    case IR_OP_LOAD_ENVIRONMENT_BINDING:
    case IR_OP_LOAD_ELEMENTS_LENGTH:
    case IR_OP_LOAD_ELEMENTS_CAPACITY:
    case IR_OP_LOAD_TYPED_ARRAY_LENGTH:
    case IR_OP_LOAD_TYPED_ARRAY_DATA_OFFSET:
    case IR_OP_LOAD_ELEMENT_AT: {
      pass_location_t location;
      [[maybe_unused]] bool loads = pass_loaded_location(pass, node_id, &location);
      assert(loads);
      remove = pass_reuse_contents(pass, facts, location, node_id);
      break;
    }
    case IR_OP_LOAD_NAMED_STORAGE: {
      pass_location_t location = {PASS_LOC_NAMED_STORAGE, inputs.items[0], 0};
      ir_node_id_t known;
      if (pass_facts_contents(facts, location, &known) && ir_node(graph, known)->op.count >= op.count) {
        pass_replace(&decisions->replacements, node_id, known);
        remove = true;
      } else pass_facts_set_contents(facts, location, node_id);
      break;
    }
    case IR_OP_STORE_GLOBAL_BINDING:
      pass_facts_forget_bindings_at(facts, op.global_binding.index);
      pass_facts_set_contents(facts,
        (pass_location_t){PASS_LOC_GLOBAL_BINDING, op.global_binding.environment, op.global_binding.index}, inputs.items[0]);
      break;
    case IR_OP_STORE_ENVIRONMENT_BINDING:
      pass_facts_forget_bindings_at(facts, op.index);
      pass_facts_set_contents(facts, (pass_location_t){PASS_LOC_BINDING, inputs.items[0], op.index}, inputs.items[1]);
      break;
    case IR_OP_STORE_NAMED:
      pass_facts_forget_named_properties_at(facts, op.offset);
      pass_facts_set_contents(facts, (pass_location_t){PASS_LOC_NAMED, inputs.items[0], op.offset}, inputs.items[1]);
      break;
    case IR_OP_ADD_NAMED: {
      ir_node_id_t object = inputs.items[0];
      pass_facts_forget_shapes_of(facts, object);
      pass_facts_set_object(facts, object);
      ir_shape_check_t shape = {.shape = op.add_named.shape};
      pass_facts_set_shapes(facts, object, &shape, 1);
      pass_facts_set_contents(facts, (pass_location_t){PASS_LOC_NAMED, object, op.add_named.offset}, inputs.items[1]);
      break;
    }
    case IR_OP_CHECK_ELEMENTS: {
      ir_node_id_t object = inputs.items[0];
      ir_elements_kind_t known = 0;
      bool is_known = false;
      uint64_t bits;
      if (ir_constant_value(graph, object, &bits)) {
        for (uint32_t i = 0; i < graph->typed_array_constants.len && !is_known; i++) {
          if (graph->typed_array_constants.items[i].bits != bits) continue;
          known = ir_elements_typed(graph->typed_array_constants.items[i].element);
          is_known = true;
        }
      }
      if (!is_known) is_known = pass_facts_elements_kind(facts, object, &known);
      remove = removable && is_known && pass_elements_kind_implies(known, op.elements) == 1;
      pass_facts_set_object(facts, object);
      if (!remove) pass_facts_set_elements_kind(facts, object, op.elements);
      break;
    }
    case IR_OP_ALLOCATE_OBJECT: {
      pass_new_object(facts, node_id);
      pass_facts_set_unexposed(facts, node_id);
      ir_shape_check_t shape = {.shape = op.allocate_object.shape};
      pass_facts_set_shapes(facts, node_id, &shape, 1);
      ir_node_id_t undefined = pass_constant(pass->constants, graph, IR_VALUE_UNDEFINED);
      for (uint32_t index = 0; index < op.allocate_object.property_count; index++)
        pass_facts_set_contents(facts, (pass_location_t){PASS_LOC_NAMED, node_id, op.allocate_object.reserve << 16 | index}, undefined);
      break;
    }
    case IR_OP_INITIALIZE_NAMED:
      pass_facts_set_contents(facts, (pass_location_t){PASS_LOC_NAMED, inputs.items[0], op.offset}, inputs.items[1]);
      break;
    case IR_OP_ALLOCATE_FUNCTION:
    case IR_OP_SLICE_ARGUMENTS: pass_new_object(facts, node_id); break;
    case IR_OP_ALLOCATE_ARRAY:
      pass_new_object(facts, node_id);
      if (op.count) pass_facts_set_elements_kind(facts, node_id, IR_ELEMENTS_PACKED);
      break;
    case IR_OP_CHECK_APPENDABLE_ARRAY: pass_facts_set_object(facts, inputs.items[0]); break;
    default: pass_facts_apply(facts, ir_op_properties(&op)); break;
  }
  if (remove) {
    if (anchor != IR_NONE) pass_replace(&decisions->replacements, node_id, anchor);
    opt_bitset_insert(&decisions->removed, node_id);
  } else if (refines) {
    pass_facts_set_refinement(facts, inputs.items[0], node_id);
  }
}

void pass_check_elimination(ir_graph_t *graph) {
  opt_arena_t scratch;
  opt_arena_init(&scratch);
  pass_facts_context_t context = {.arena = &scratch};
  pass_constants_t constants;
  pass_constants_init(&constants, graph, &scratch);
  uint32_t block_count = ir_block_count(graph);
  pass_edge_facts_vec_t *edge_facts = opt_arena_new(&scratch, pass_edge_facts_vec_t, block_count + 1);
  pass_facts_t *entry_facts = opt_arena_new(&scratch, pass_facts_t, block_count + 1);
  bool *has_entry_facts = opt_arena_new(&scratch, bool, block_count + 1);
  bool *revisited = opt_arena_new(&scratch, bool, block_count + 1);
  for (ir_block_id_t block = 0; block < block_count; block++) {
    const ir_block_vec_t *predecessors = &ir_block(graph, block)->predecessors;
    for (uint32_t i = 0; i < predecessors->len; i++) revisited[block] |= predecessors->items[i] >= block;
  }
  bool *visited = opt_arena_new(&scratch, bool, block_count + 1);
  pass_decisions_t decisions;
  for (;;) {
    decisions = (pass_decisions_t){0};
    pass_replacements_init(&decisions.replacements, graph, &scratch);
    decisions.removed = opt_bitset_new(&scratch, ir_node_count(graph));
    pass_check_elimination_t pass = {
      .graph = graph, .arena = &scratch, .context = &context, .constants = &constants, .decisions = &decisions,
    };
    bool changed = false;
    memset(visited, 0, block_count * sizeof *visited);
    ir_block_vec_t skipped = {0};
    for (ir_block_id_t block = 0; block < block_count; block++) {
      pass_facts_t facts;
      if (!pass_facts_at_entry(&pass, block, edge_facts, visited, &facts)) {
        edge_facts[block].len = 0;
        opt_vec_push(&scratch, &skipped, block);
        continue;
      }
      visited[block] = true;
      if (revisited[block] && (!has_entry_facts[block] || !pass_facts_equal(&entry_facts[block], &facts))) {
        changed = true;
        pass_facts_clone(&entry_facts[block], &facts);
        has_entry_facts[block] = true;
      }
      pass_visit_block(&pass, block, &facts, &edge_facts[block]);
    }
    for (uint32_t s = 0; s < skipped.len && !changed; s++) {
      ir_block_id_t block = skipped.items[s];
      const ir_block_vec_t *predecessors = &ir_block(graph, block)->predecessors;
      for (uint32_t i = 0; i < predecessors->len && !changed; i++) {
        const pass_edge_facts_vec_t *edges = &edge_facts[predecessors->items[i]];
        for (uint32_t j = 0; j < edges->len && !changed; j++) changed = edges->items[j].target == block;
      }
    }
    if (!changed) break;
  }

  pass_replacements_apply(&decisions.replacements, graph);
  pass_remove_nodes(graph, &decisions.removed);
  bool changed_control_flow = decisions.controls.len != 0;
  for (uint32_t i = 0; i < decisions.controls.len; i++) {
    pass_control_decision_t decision = decisions.controls.items[i];
    if (decision.op.code == IR_OP_JUMP) {
      pass_make_jump(graph, decision.node, decision.op.target);
      continue;
    }
    decision.op.cases.items = ir_dup_cases(graph, decision.op.cases.items, decision.op.cases.len);
    ir_node(graph, decision.node)->op = decision.op;
  }
  opt_arena_free(&scratch);
  if (changed_control_flow) pass_clean_up_control_flow(graph);
}
