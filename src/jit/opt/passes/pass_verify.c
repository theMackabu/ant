#include "passes.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
  ir_block_id_t block;
  uint32_t index;
} pass_place_t;

typedef struct {
  opt_text_t *out;
  uint32_t count;
} pass_errors_t;

[[gnu::format(printf, 2, 3)]] static void pass_error(pass_errors_t *errors, const char *format, ...) {
  char buffer[512];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof buffer, format, args);
  va_end(args);
  if (errors->out) {
    if (errors->count) opt_text_append(errors->out, "\n");
    opt_text_append(errors->out, buffer);
  }
  errors->count++;
}

static bool pass_reads_or_writes(const ir_op_t *op) {
  ir_node_properties_t properties = ir_op_properties(op);
  return properties.is_call || properties.reads != IR_LOC_NONE || properties.writes != IR_LOC_NONE;
}

bool pass_verify_frame_writes(const ir_graph_t *graph, const ir_frame_layout_t *layout, opt_text_t *out) {
  pass_errors_t errors = {.out = out};
  for (ir_block_id_t index = 0; index < ir_block_count(graph); index++) {
    const ir_block_t *block = ir_block(graph, index);
    uint32_t count = block->body.len + (block->control != IR_NONE);
    for (uint32_t position = 0; position < count; position++) {
      ir_node_id_t node = position < block->body.len ? block->body.items[position] : block->control;
      const ir_op_t *op = &ir_node(graph, node)->op;
      if (op->code != IR_OP_STORE_SLOT) continue;
      uint32_t slot = op->slot;
      bool is_constant = slot >= layout->registers_and_locals_count && slot < ir_layout_arguments_base(layout);
      if (slot < layout->reserved_count || is_constant || slot >= layout->number_of_registers) continue;
      bool reads_frame = false;
      for (uint32_t next = position + 1; next < count; next++) {
        ir_node_id_t reader = next < block->body.len ? block->body.items[next] : block->control;
        const ir_op_t *reader_op = &ir_node(graph, reader)->op;
        if (reader_op->code == IR_OP_STORE_SLOT || !pass_reads_or_writes(reader_op)) continue;
        ir_node_properties_t properties = ir_op_properties(reader_op);
        reads_frame = properties.is_call && ir_locations_intersect(properties.reads, IR_LOC_FRAME);
        break;
      }
      if (!reads_frame)
        pass_error(&errors, "StoreSlot v%u in b%u writes a slot that no node reading the frame reads next", node, index);
    }
  }
  return errors.count == 0;
}

static bool pass_verify_blocks(const ir_graph_t *graph, pass_place_t *places, pass_errors_t *errors) {
  uint32_t before = errors->count;
  for (uint32_t i = 0; i < ir_node_count(graph); i++) places[i] = (pass_place_t){.block = IR_NONE};
  for (ir_block_id_t index = 0; index < ir_block_count(graph); index++) {
    const ir_block_t *block = ir_block(graph, index);
    if (block->control == IR_NONE) {
      pass_error(errors, "b%u has no control node", index);
      continue;
    }
    if (!ir_op_is_control(&ir_node(graph, block->control)->op))
      pass_error(errors, "b%u ends in v%u, which is not a control node", index, block->control);
    for (uint32_t i = 0; i < block->phis.len; i++) {
      ir_node_id_t phi = block->phis.items[i];
      const ir_node_t *node = ir_node(graph, phi);
      if (node->op.code != IR_OP_PHI) pass_error(errors, "b%u has v%u among its phis", index, phi);
      if (node->inputs.len != block->predecessors.len)
        pass_error(errors, "phi v%u of b%u has %u inputs for %u predecessors", phi, index, node->inputs.len, block->predecessors.len);
    }
    for (uint32_t i = 0; i < block->body.len; i++) {
      const ir_op_t *op = &ir_node(graph, block->body.items[i])->op;
      if (op->code == IR_OP_PHI || ir_op_is_control(op) || op->code == IR_OP_CONSTANT)
        pass_error(errors, "b%u has %s v%u in its body", index, ir_op_name(op->code), block->body.items[i]);
    }
    for (uint32_t position = 0, count = ir_block_node_count(block); position < count; position++) {
      ir_node_id_t node = ir_block_node_at(block, position);
      if (places[node].block != IR_NONE) pass_error(errors, "v%u is in b%u and in b%u", node, places[node].block, index);
      else places[node] = (pass_place_t){.block = index, .index = position};
    }
  }
  return errors->count == before;
}

static bool pass_verify_edges(const ir_graph_t *graph, pass_errors_t *errors) {
  uint32_t before = errors->count;
  for (ir_block_id_t index = 0; index < ir_block_count(graph); index++) {
    const ir_block_t *block = ir_block(graph, index);
    ir_successors_t successors = ir_block_successors(graph, index);
    for (uint32_t i = 0; i < successors.len; i++) {
      ir_block_id_t successor = ir_successor_at(&successors, i);
      uint32_t edges_out = 0, edges_in = 0;
      for (uint32_t j = 0; j < successors.len; j++) edges_out += ir_successor_at(&successors, j) == successor;
      const ir_block_vec_t *predecessors = &ir_block(graph, successor)->predecessors;
      for (uint32_t j = 0; j < predecessors->len; j++) edges_in += predecessors->items[j] == index;
      if (edges_out != edges_in)
        pass_error(errors, "b%u has %u edges to b%u, which lists it %u times", index, edges_out, successor, edges_in);
    }
    if (block->phis.len) {
      for (uint32_t i = 0; i < block->predecessors.len; i++) {
        ir_successors_t predecessor_successors = ir_block_successors(graph, block->predecessors.items[i]);
        if (predecessor_successors.len != 1)
          pass_error(errors, "b%u has phis, but its predecessor b%u also leads elsewhere", index, block->predecessors.items[i]);
      }
    }
    for (uint32_t i = 0; i < block->predecessors.len; i++) {
      ir_block_id_t predecessor = block->predecessors.items[i];
      ir_successors_t predecessor_successors = ir_block_successors(graph, predecessor);
      if (!ir_successors_contains(&predecessor_successors, index))
        pass_error(errors, "b%u lists b%u as a predecessor, which does not branch to it", index, predecessor);
      if (predecessor >= index && !block->is_loop_header && !(ir_block(graph, predecessor)->is_cold && !block->is_cold))
        pass_error(errors, "b%u is reached from the later b%u but is no loop header", index, predecessor);
    }
  }
  return errors->count == before;
}

typedef struct {
  const ir_graph_t *graph;
  const pass_place_t *places;
  pass_dominators_t dominators;
} pass_definitions_t;

static bool pass_is_defined_before(const pass_definitions_t *definitions, ir_node_id_t value, ir_block_id_t block, uint32_t index) {
  ir_opcode_t code = ir_node(definitions->graph, value)->op.code;
  if (code == IR_OP_CONSTANT || code == IR_OP_VIRTUAL_OBJECT) return true;
  pass_place_t place = definitions->places[value];
  if (place.block == IR_NONE) return false;
  if (place.block == block) return place.index < index;
  return pass_dominates(&definitions->dominators, place.block, block);
}

static uint32_t pass_branch_input_reprs(ir_condition_t condition, opt_repr_t *reprs) {
  switch (condition.kind) {
    case IR_COND_BOOL: reprs[0] = OPT_REPR_BOOL; return 1;
    case IR_COND_INT32:
    case IR_COND_INDEX_IN_BOUNDS: reprs[0] = reprs[1] = OPT_REPR_INT32; return 2;
    case IR_COND_FLOAT64: reprs[0] = reprs[1] = OPT_REPR_FLOAT64; return 2;
    case IR_COND_TAGGED_EQUALS:
    case IR_COND_PROPERTY_ITERATOR_CACHE_VALID: reprs[0] = reprs[1] = OPT_REPR_TAGGED; return 2;
    case IR_COND_NULLISH:
    case IR_COND_UNDEFINED:
    case IR_COND_OBJECT:
    case IR_COND_NON_NEGATIVE_INT32:
    case IR_COND_INT32_VALUE:
    case IR_COND_DOUBLE:
    case IR_COND_STRING:
    case IR_COND_ELEMENTS_KIND:
    case IR_COND_ITERABLE_ARRAY:
    case IR_COND_ITERATOR_RETURN_INTACT:
    case IR_COND_BUILTIN: reprs[0] = OPT_REPR_TAGGED; return 1;
    case IR_COND_RESOLVED_STRING:
    case IR_COND_ASCII_STRING:
    case IR_COND_MAGICAL_LENGTH:
    case IR_COND_EXTENSIBLE:
    case IR_COND_ARRAY_ITERATOR:
    case IR_COND_BINDING_MUTABLE:
    case IR_COND_NEXT_BINDING_OF_SHAPE: reprs[0] = OPT_REPR_POINTER; return 1;
    case IR_COND_SHAPE:
      reprs[0] = OPT_REPR_TAGGED;
      reprs[1] = OPT_REPR_POINTER;
      return 2;
  }
  return 0;
}

static uint32_t pass_reprs(opt_repr_t *reprs, uint32_t count, ...) {
  va_list args;
  va_start(args, count);
  for (uint32_t i = 0; i < count; i++) reprs[i] = (opt_repr_t)va_arg(args, int);
  va_end(args);
  return count;
}

static uint32_t pass_expected_input_reprs(const ir_node_t *node, opt_repr_t *reprs, uint32_t capacity) {
  const opt_repr_t T = OPT_REPR_TAGGED, I = OPT_REPR_INT32, F = OPT_REPR_FLOAT64, P = OPT_REPR_POINTER, B = OPT_REPR_BOOL;
  switch (node->op.code) {
    case IR_OP_PHI:
    case IR_OP_CONSTANT:
    case IR_OP_STORE_SLOT:
    case IR_OP_CALL_NATIVE: return UINT32_MAX;
    case IR_OP_BRANCH: return pass_branch_input_reprs(node->op.branch.condition, reprs);
    case IR_OP_REFINE: return pass_branch_input_reprs(node->op.refine.condition, reprs);
    case IR_OP_UNBOX_INT32:
    case IR_OP_UNBOX_DOUBLE:
    case IR_OP_CHECK_NUMBER:
    case IR_OP_STRING_ADDRESS:
    case IR_OP_PRIMITIVE_TO_STRING: return pass_reprs(reprs, 1, T);
    case IR_OP_STRING_LENGTH: return pass_reprs(reprs, 1, P);
    case IR_OP_BOX_FLOAT64:
    case IR_OP_FLOAT64_UNARY:
    case IR_OP_FLOAT64_TO_INT32:
    case IR_OP_FLOAT64_TO_UINT8_CLAMPED: return pass_reprs(reprs, 1, F);
    case IR_OP_FLOAT64_BINARY:
    case IR_OP_FLOAT64_MIN:
    case IR_OP_FLOAT64_MAX:
    case IR_OP_FLOAT64_COMPARE: return pass_reprs(reprs, 2, F, F);
    case IR_OP_CALL_FLOAT64_FUNCTION: {
      static const uint8_t arity[] = {
#define ANT_MATH_DOUBLE(id, name, argc, expr) argc,
#include "modules/math_doubles.h"
      };
      if (node->op.function >= sizeof arity) return 0;
      return arity[node->op.function] == 2 ? pass_reprs(reprs, 2, F, F) : pass_reprs(reprs, 1, F);
    }
    case IR_OP_UINT32_SHIFT_RIGHT: return pass_reprs(reprs, 2, I, I);
    case IR_OP_UINT32_TO_FLOAT64:
    case IR_OP_INTEGER_TO_STRING: return pass_reprs(reprs, 1, I);
    case IR_OP_CONCATENATE_STRINGS:
      return pass_reprs(reprs, 2, node->op.count & IR_CONCAT_INT32_LHS ? I : T, node->op.count & IR_CONCAT_INT32_RHS ? I : T);
    case IR_OP_STRINGS_EQUAL:
    case IR_OP_STRING_BUILDER_APPEND:
    case IR_OP_TO_OBJECT: return pass_reprs(reprs, 2, T, T);
    case IR_OP_ARRAY_CREATE: return pass_reprs(reprs, 2, I, T);
    case IR_OP_LOAD_STRING_CODE_UNIT: return pass_reprs(reprs, 2, P, I);
    case IR_OP_SINGLE_CHARACTER_STRING:
    case IR_OP_INT32_ABS:
    case IR_OP_INT32_TO_FLOAT64: return pass_reprs(reprs, 1, I);
    case IR_OP_APPEND_ELEMENT: return pass_reprs(reprs, 3, P, I, T);
    case IR_OP_POP_ELEMENT: return pass_reprs(reprs, 2, P, I);
    case IR_OP_LOAD_ARGUMENT: return pass_reprs(reprs, 1, I);
    case IR_OP_LOAD_ELEMENTS_LENGTH:
    case IR_OP_LOAD_ELEMENTS_CAPACITY:
    case IR_OP_CAN_ADD_ELEMENT: return pass_reprs(reprs, 1, P);
    case IR_OP_LOAD_PROPERTY_ITERATOR_KEY: return pass_reprs(reprs, 2, T, I);
    case IR_OP_CREATE_ARRAY_ITERATOR: return pass_reprs(reprs, 2, T, T);
    case IR_OP_LOAD_ARRAY_ITERATOR_ARRAY:
    case IR_OP_LOAD_ARRAY_ITERATOR_INDEX:
    case IR_OP_FINISH_ARRAY_ITERATOR: return pass_reprs(reprs, 1, P);
    case IR_OP_STORE_ARRAY_ITERATOR_INDEX: return pass_reprs(reprs, 2, P, I);
    case IR_OP_BRANCH_ON_PC:
    case IR_OP_BOX_INT32: return pass_reprs(reprs, 1, I);
    case IR_OP_CHECK_SHAPE:
    case IR_OP_LOAD_NAMED:
    case IR_OP_LOAD_NAMED_STORAGE:
    case IR_OP_SHAPE_SWITCH:
    case IR_OP_CHECK_ACCESSOR_FUNCTION:
    case IR_OP_LOAD_ACCESSOR_FUNCTION: return pass_reprs(reprs, 2, T, P);
    case IR_OP_STORE_NAMED:
      if (node->inputs.len == 4) return pass_reprs(reprs, 4, T, T, P, P);
      return pass_reprs(reprs, 3, T, T, P);
    case IR_OP_ADD_NAMED: return pass_reprs(reprs, 3, T, T, P);
    case IR_OP_LOAD_ELEMENT_AT: return pass_reprs(reprs, 2, P, I);
    case IR_OP_STORE_ELEMENT_AT: return pass_reprs(reprs, 3, P, I, ir_elements_stored_repr(node->op.elements));
    case IR_OP_CHECK_BOUNDS: return pass_reprs(reprs, 2, I, I);
    case IR_OP_LOAD_TYPED_ARRAY_LENGTH:
    case IR_OP_LOAD_TYPED_ARRAY_DATA_OFFSET:
    case IR_OP_CHECK_TYPED_ARRAY_STORAGE: return pass_reprs(reprs, 1, P);
    case IR_OP_BOX_BOOL:
    case IR_OP_BOOL_TO_INT32: return pass_reprs(reprs, 1, B);
    case IR_OP_LOAD_FUNCTION_ENVIRONMENT:
    case IR_OP_BOX_CELL:
    case IR_OP_LOAD_OUTER_ENVIRONMENT:
    case IR_OP_LOAD_ENVIRONMENT_BINDING:
    case IR_OP_APPEND_ENVIRONMENT_BINDING: return pass_reprs(reprs, 1, P);
    case IR_OP_STORE_ENVIRONMENT_BINDING:
    case IR_OP_STORE_UPVALUE: return pass_reprs(reprs, 2, P, T);
    case IR_OP_LOAD_UPVALUE:
    case IR_OP_CHECK_UPVALUE_INITIALIZED:
    case IR_OP_LOAD_PROTOTYPE: return pass_reprs(reprs, 1, P);
    case IR_OP_CAN_ADD_NAMED:
    case IR_OP_PROBE_PROPERTY_CACHE: return pass_reprs(reprs, 2, T, P);
    case IR_OP_ALLOCATE_FUNCTION:
    case IR_OP_PROBE_GLOBAL_CACHE: return pass_reprs(reprs, 2, P, P);
    case IR_OP_PROBE_GLOBAL_STORE: return pass_reprs(reprs, 3, T, P, P);
    case IR_OP_INT32_BINARY:
    case IR_OP_INT32_WRAPPING_BINARY:
    case IR_OP_INT32_MIN:
    case IR_OP_INT32_MAX:
    case IR_OP_INT32_COMPARE: return pass_reprs(reprs, 2, I, I);
    default: {
      uint32_t count = node->inputs.len < capacity ? node->inputs.len : capacity;
      for (uint32_t i = 0; i < count; i++) reprs[i] = T;
      return node->inputs.len;
    }
  }
}

static void pass_check_input_reprs(const ir_graph_t *graph, ir_node_id_t node_id, opt_arena_t *scratch, pass_errors_t *errors) {
  const ir_node_t *node = ir_node(graph, node_id);
  uint32_t capacity = node->inputs.len + 4;
  opt_repr_t *expected = opt_arena_alloc(scratch, capacity * sizeof *expected);
  uint32_t count = pass_expected_input_reprs(node, expected, capacity);
  if (count == UINT32_MAX) return;
  if (node->inputs.len != count) {
    pass_error(errors, "v%u (%s) has %u inputs, but takes %u", node_id, ir_op_name(node->op.code), node->inputs.len, count);
    return;
  }
  for (uint32_t i = 0; i < count; i++) {
    opt_repr_t actual = ir_node(graph, node->inputs.items[i])->repr;
    if (actual == OPT_REPR_NONE) return;
    if (actual != expected[i]) {
      pass_error(errors, "v%u (%s) takes v%u as %s, but it is %s", node_id, ir_op_name(node->op.code),
        node->inputs.items[i], opt_repr_name(expected[i]), opt_repr_name(actual));
      return;
    }
  }
}

static void pass_verify_node(const pass_definitions_t *definitions, ir_block_id_t index, uint32_t position, ir_node_id_t node_id, opt_arena_t *scratch, pass_errors_t *errors) {
  const ir_graph_t *graph = definitions->graph;
  const ir_block_t *block = ir_block(graph, index);
  const ir_node_t *node = ir_node(graph, node_id);
  const char *name = ir_op_name(node->op.code);
  for (uint32_t i = 0; i < node->inputs.len; i++) {
    ir_node_id_t input = node->inputs.items[i];
    if (!pass_is_defined_before(definitions, input, index, position))
      pass_error(errors, "v%u (%s) in b%u uses v%u, which is not defined before it", node_id, name, index, input);
    else if (ir_node(graph, input)->repr == OPT_REPR_NONE)
      pass_error(errors, "v%u (%s) uses v%u, which has no value", node_id, name, input);
  }
  if (node->frame_state != IR_NONE) {
    ir_slot_values_t values = {0};
    ir_frame_state_values(graph, node->frame_state, scratch, &values);
    for (uint32_t i = 0; i < values.len; i++) {
      uint32_t slot = values.items[i].slot;
      ir_node_id_t value = values.items[i].value;
      const ir_node_t *value_node = ir_node(graph, value);
      if (value_node->repr == OPT_REPR_NONE && value_node->op.code != IR_OP_VIRTUAL_OBJECT)
        pass_error(errors, "the frame state of v%u has v%u for slot %u, which has no value", node_id, value, slot);
      if (value_node->repr == OPT_REPR_POINTER && value_node->op.code != IR_OP_CALL_NATIVE && value_node->op.code != IR_OP_PHI && value_node->op.code != IR_OP_LOAD_UPVALUE_CELL)
        pass_error(errors, "the frame state of v%u has the cell address v%u for slot %u", node_id, value, slot);
      if (!pass_is_defined_before(definitions, value, index, position))
        pass_error(errors, "the frame state of v%u (%s) in b%u has v%u for slot %u, which is not defined before it",
          node_id, name, index, value, slot);
    }
  }
  pass_check_input_reprs(graph, node_id, scratch, errors);
  if ((ir_op_can_eager_exit(&node->op) || ir_op_properties(&node->op).can_lazy_exit) && node->frame_state == IR_NONE)
    pass_error(errors, "v%u (%s) can exit, but has no frame state", node_id, name);
  if (node->op.code == IR_OP_JUMP && node->inputs.len) pass_error(errors, "v%u (Jump) has inputs", node_id);
  if (node->op.code == IR_OP_BRANCH && node->op.branch.condition.kind == IR_COND_SHAPE)
    pass_error(errors, "v%u branches on a shape", node_id);
  if (node->op.code == IR_OP_CALL_SLOW_PATH) {
    if (node->frame_state == IR_NONE) pass_error(errors, "v%u (CallSlowPath) has no frame state", node_id);
    if (node->op.slow.saves_registers && !block->is_cold)
      pass_error(errors, "v%u (CallSlowPath) is in b%u, which is not cold", node_id, index);
  }
  if (node->op.code == IR_OP_REFINE) {
    bool at_head = true;
    for (uint32_t i = 0; i < position - block->phis.len && at_head; i++)
      at_head = ir_node(graph, block->body.items[i])->op.code == IR_OP_REFINE;
    if (!at_head || block->predecessors.len != 1)
      pass_error(errors, "v%u (Refine) is not at the head of b%u, a block with one predecessor", node_id, index);
  }
  int refined = ir_op_refined_input(&node->op);
  if (refined >= 0 && node->repr != OPT_REPR_NONE && node->repr != ir_node(graph, node->inputs.items[refined])->repr)
    pass_error(errors, "v%u (%s) changes the representation of the value it refines", node_id, name);
  if ((node->op.code == IR_OP_LOAD_ELEMENT_AT || node->op.code == IR_OP_STORE_ELEMENT_AT) && ir_elements_is_typed(node->op.elements)) {
    ir_opcode_t input = ir_node(graph, node->inputs.items[0])->op.code;
    if (input != IR_OP_LOAD_TYPED_ARRAY_DATA_OFFSET && input != IR_OP_CHECK_TYPED_ARRAY_STORAGE)
      pass_error(errors, "v%u (%s) takes no storage offset", node_id, name);
  }
  if (node->op.code == IR_OP_SLOW_PATH_OUTPUT) {
    ir_node_id_t call = node->inputs.items[0];
    bool follows_call = false;
    if (ir_node(graph, call)->op.code == IR_OP_CALL_SLOW_PATH || ir_node(graph, call)->op.code == IR_OP_CALL_NATIVE) {
      for (uint32_t start = 0; start < position; start++) {
        if (ir_block_node_at(block, start) != call) continue;
        follows_call = true;
        for (uint32_t i = start + 1; i < position && follows_call; i++)
          follows_call = ir_node(graph, ir_block_node_at(block, i))->op.code == IR_OP_SLOW_PATH_OUTPUT;
        break;
      }
    }
    if (!follows_call)
      pass_error(errors, "v%u (SlowPathOutput) does not follow its slow path call v%u directly", node_id, call);
  }
}

static bool pass_verify_nodes(const ir_graph_t *graph, const pass_place_t *places, opt_arena_t *scratch, pass_errors_t *errors) {
  uint32_t before = errors->count;
  pass_definitions_t definitions = {.graph = graph, .places = places};
  pass_dominators_compute(&definitions.dominators, graph, scratch);
  for (ir_block_id_t index = 0; index < ir_block_count(graph); index++) {
    const ir_block_t *block = ir_block(graph, index);
    bool is_entry = ir_is_entry(graph, index);
    if ((block->predecessors.len == 0) != is_entry)
      pass_error(errors, "b%u has %u predecessors, but is %s", index, block->predecessors.len, is_entry ? "an entry" : "no entry");
    if (!definitions.dominators.reachable[index]) {
      pass_error(errors, "b%u is reachable from no entry", index);
      continue;
    }
    for (uint32_t p = 0; p < block->phis.len; p++) {
      ir_node_id_t phi = block->phis.items[p];
      const ir_node_t *phi_node = ir_node(graph, phi);
      for (uint32_t i = 0; i < phi_node->inputs.len && i < block->predecessors.len; i++) {
        ir_node_id_t input = phi_node->inputs.items[i];
        ir_block_id_t predecessor = block->predecessors.items[i];
        if (ir_node(graph, input)->repr != phi_node->repr)
          pass_error(errors, "phi v%u of b%u is %s, but its input v%u is %s", phi, index, opt_repr_name(phi_node->repr),
            input, opt_repr_name(ir_node(graph, input)->repr));
        uint32_t end = ir_block_node_count(ir_block(graph, predecessor));
        if (!pass_is_defined_before(&definitions, input, predecessor, end))
          pass_error(errors, "phi v%u of b%u uses v%u from b%u, where it is not defined", phi, index, input, predecessor);
      }
    }
    for (uint32_t position = 0, count = ir_block_node_count(block); position < count; position++) {
      ir_node_id_t node = ir_block_node_at(block, position);
      if (ir_node(graph, node)->op.code == IR_OP_PHI) continue;
      pass_verify_node(&definitions, index, position, node, scratch, errors);
    }
  }
  return errors->count == before;
}

static int pass_compare_slots(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return (x > y) - (x < y);
}

static bool pass_verify_frame_states(const ir_graph_t *graph, opt_arena_t *scratch, pass_errors_t *errors) {
  uint32_t before = errors->count;
  for (uint32_t index = 0; index < graph->frame_states.len; index++) {
    const ir_frame_state_t *state = ir_frame_state(graph, index);
    uint32_t count = state->values.len + state->in_frame.len;
    if (count < 2) continue;
    uint32_t *slots = opt_arena_alloc(scratch, count * sizeof *slots);
    for (uint32_t i = 0; i < state->values.len; i++) slots[i] = state->values.items[i].slot;
    for (uint32_t i = 0; i < state->in_frame.len; i++) slots[state->values.len + i] = state->in_frame.items[i];
    qsort(slots, count, sizeof *slots, pass_compare_slots);
    for (uint32_t i = 1; i < count; i++) {
      if (slots[i - 1] == slots[i]) {
        pass_error(errors, "fs%u lists a slot more than once", index);
        break;
      }
    }
  }
  return errors->count == before;
}

bool pass_verify(const ir_graph_t *graph, opt_text_t *out) {
  opt_arena_t scratch;
  opt_arena_init(&scratch);
  pass_errors_t errors = {.out = out};
  pass_place_t *places = opt_arena_alloc(&scratch, (ir_node_count(graph) + 1) * sizeof *places);
  bool ok = pass_verify_blocks(graph, places, &errors)
    && pass_verify_edges(graph, &errors)
    && pass_verify_nodes(graph, places, &scratch, &errors)
    && pass_verify_frame_states(graph, &scratch, &errors);
  opt_arena_free(&scratch);
  return ok;
}
