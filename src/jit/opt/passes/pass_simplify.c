#include "passes.h"
#include "../ir/ir_value.h"

static int pass_constant_truthiness(uint64_t bits, opt_repr_t repr) {
  switch (repr) {
    case OPT_REPR_BOOL: return bits != 0;
    case OPT_REPR_INT32: return (uint32_t)bits != 0;
    case OPT_REPR_FLOAT64:
    case OPT_REPR_POINTER:
    case OPT_REPR_NONE: return -1;
    case OPT_REPR_TAGGED: break;
  }
  int32_t integer;
  if (ir_value_is_bool(bits)) return (bits & 1) != 0;
  if (ir_value_as_int32(bits, &integer)) return integer != 0;
  if (bits == IR_VALUE_UNDEFINED || bits == IR_VALUE_NULL) return 0;
  return -1;
}

static bool pass_int32_of(const ir_graph_t *graph, ir_node_id_t node, int32_t *value) {
  const ir_node_t *data = ir_node(graph, node);
  if (data->op.code != IR_OP_CONSTANT || data->repr != OPT_REPR_INT32) return false;
  *value = (int32_t)(uint32_t)data->op.bits;
  return true;
}

static bool pass_fold_int32_binary(ir_binary_op_t op, int32_t lhs, int32_t rhs, int32_t *result) {
  uint32_t shift = (uint32_t)rhs & 31;
  switch (op) {
    case IR_BINARY_ADD: return !__builtin_add_overflow(lhs, rhs, result);
    case IR_BINARY_SUB: return !__builtin_sub_overflow(lhs, rhs, result);
    case IR_BINARY_MUL:
      if (__builtin_mul_overflow(lhs, rhs, result)) return false;
      return *result != 0 || (lhs >= 0 && rhs >= 0);
    case IR_BINARY_BITWISE_AND: *result = lhs & rhs; return true;
    case IR_BINARY_BITWISE_OR: *result = lhs | rhs; return true;
    case IR_BINARY_BITWISE_XOR: *result = lhs ^ rhs; return true;
    case IR_BINARY_LEFT_SHIFT: *result = (int32_t)((uint32_t)lhs << shift); return true;
    case IR_BINARY_RIGHT_SHIFT: *result = lhs >> shift; return true;
    case IR_BINARY_UNSIGNED_RIGHT_SHIFT: {
      uint32_t shifted = (uint32_t)lhs >> shift;
      if (shifted > INT32_MAX) return false;
      *result = (int32_t)shifted;
      return true;
    }
    case IR_BINARY_MOD:
      if (rhs == 0 || (lhs == INT32_MIN && rhs == -1)) return false;
      *result = lhs % rhs;
      return *result != 0 || lhs >= 0;
    case IR_BINARY_DIV: return false;
  }
  return false;
}

static int32_t pass_fold_int32_wrapping_binary(ir_binary_op_t op, int32_t lhs, int32_t rhs) {
  switch (op) {
    case IR_BINARY_ADD: return (int32_t)((uint32_t)lhs + (uint32_t)rhs);
    case IR_BINARY_SUB: return (int32_t)((uint32_t)lhs - (uint32_t)rhs);
    case IR_BINARY_MUL: return (int32_t)((uint32_t)lhs * (uint32_t)rhs);
    default: return (int32_t)((uint32_t)lhs >> ((uint32_t)rhs & 31));
  }
}

static bool pass_is_int32_identity(ir_binary_op_t op, int32_t rhs, bool wrapping) {
  switch (op) {
    case IR_BINARY_ADD:
    case IR_BINARY_SUB:
    case IR_BINARY_BITWISE_OR:
    case IR_BINARY_BITWISE_XOR: return rhs == 0;
    case IR_BINARY_LEFT_SHIFT:
    case IR_BINARY_RIGHT_SHIFT: return (rhs & 31) == 0;
    case IR_BINARY_UNSIGNED_RIGHT_SHIFT: return wrapping && (rhs & 31) == 0;
    case IR_BINARY_BITWISE_AND: return rhs == -1;
    case IR_BINARY_MUL: return rhs == 1;
    case IR_BINARY_MOD:
    case IR_BINARY_DIV: return false;
  }
  return false;
}

static bool pass_fold_int32_compare(ir_comparison_t comparison, int32_t lhs, int32_t rhs) {
  switch (comparison) {
    case IR_CMP_LESS_THAN: return lhs < rhs;
    case IR_CMP_LESS_THAN_EQUALS: return lhs <= rhs;
    case IR_CMP_GREATER_THAN: return lhs > rhs;
    case IR_CMP_GREATER_THAN_EQUALS: return lhs >= rhs;
    case IR_CMP_STRICTLY_EQUALS:
    case IR_CMP_LOOSELY_EQUALS: return lhs == rhs;
    case IR_CMP_STRICTLY_INEQUALS:
    case IR_CMP_LOOSELY_INEQUALS: return lhs != rhs;
    case IR_CMP_BELOW: return (uint32_t)lhs < (uint32_t)rhs;
    case IR_CMP_BELOW_OR_EQUAL: return (uint32_t)lhs <= (uint32_t)rhs;
    case IR_CMP_ABOVE: return (uint32_t)lhs > (uint32_t)rhs;
    case IR_CMP_ABOVE_OR_EQUAL: return (uint32_t)lhs >= (uint32_t)rhs;
  }
  return false;
}

static int pass_constant_condition(const ir_graph_t *graph, ir_condition_t condition, ir_node_id_t input, ir_node_id_t other) {
  if (ir_node(graph, input)->repr != OPT_REPR_TAGGED) return -1;
  if (condition.kind == IR_COND_INT32_VALUE && ir_node(graph, input)->op.code == IR_OP_BOX_INT32) return 1;
  uint64_t bits;
  if (!ir_constant_value(graph, input, &bits)) return -1;
  int32_t integer;
  switch (condition.kind) {
    case IR_COND_TAGGED_EQUALS: {
      uint64_t other_bits;
      if (other == IR_NONE || ir_node(graph, other)->repr != OPT_REPR_TAGGED) return -1;
      if (!ir_constant_value(graph, other, &other_bits)) return -1;
      return (bits == other_bits) == condition.equal;
    }
    case IR_COND_NULLISH: return bits == IR_VALUE_UNDEFINED || bits == IR_VALUE_NULL;
    case IR_COND_UNDEFINED: return bits == IR_VALUE_UNDEFINED;
    case IR_COND_OBJECT: return ir_value_is_object(bits);
    case IR_COND_NON_NEGATIVE_INT32: return ir_value_as_int32(bits, &integer) && integer >= 0;
    case IR_COND_INT32_VALUE: return ir_value_as_int32(bits, NULL);
    case IR_COND_DOUBLE: return ir_value_is_double(bits);
    case IR_COND_STRING: return ir_value_is_string(bits);
    default: return -1;
  }
}

static bool pass_constant_number(const ir_graph_t *graph, ir_node_id_t node, double *number) {
  uint64_t bits;
  if (ir_node(graph, node)->repr != OPT_REPR_TAGGED || !ir_constant_value(graph, node, &bits)) return false;
  if (!ir_value_is_number(bits)) return false;
  *number = ir_value_to_double(bits);
  return true;
}

static ir_node_id_t pass_boxed(const ir_graph_t *graph, ir_opcode_t box, ir_node_id_t input) {
  const ir_node_t *unrefined = ir_node(graph, ir_unrefined(graph, input));
  return unrefined->op.code == box ? unrefined->inputs.items[0] : IR_NONE;
}

static bool pass_may_stand_for_result(const ir_graph_t *graph, ir_node_id_t value) {
  const ir_node_t *check = ir_node(graph, ir_unrefined(graph, value));
  if (check->op.code != IR_OP_CHECK_INT32) return true;
  ir_opcode_t input = ir_node(graph, ir_unrefined(graph, check->inputs.items[0]))->op.code;
  return input != IR_OP_BOX_FLOAT64 && input != IR_OP_PHI && input != IR_OP_BOX_INT32;
}

static ir_node_id_t pass_fold_node(ir_graph_t *graph, pass_constants_t *constants, const ir_op_t *op, const ir_node_id_t *inputs) {
  int32_t lhs, rhs;
  uint64_t bits;
  switch (op->code) {
    case IR_OP_CHECK_NUMBER: {
      double number;
      if (pass_constant_number(graph, inputs[0], &number))
        return pass_constant_typed(constants, graph, ir_value_double_bits(number), OPT_REPR_FLOAT64);
      return pass_boxed(graph, IR_OP_BOX_FLOAT64, inputs[0]);
    }
    case IR_OP_UNBOX_DOUBLE: return pass_boxed(graph, IR_OP_BOX_FLOAT64, inputs[0]);
    case IR_OP_UNBOX_INT32: return pass_boxed(graph, IR_OP_BOX_INT32, inputs[0]);
    case IR_OP_EMPTY_TO_UNDEFINED: return ir_is_known_non_empty(graph, inputs[0], 0, 0) ? inputs[0] : IR_NONE;
    case IR_OP_CHECK_BOUNDS:
      if (pass_int32_of(graph, inputs[0], &lhs) && pass_int32_of(graph, inputs[1], &rhs) && (uint32_t)lhs < (uint32_t)rhs)
        return inputs[0];
      return IR_NONE;
    case IR_OP_INT32_TO_FLOAT64: {
      const ir_node_t *input = ir_node(graph, inputs[0]);
      return input->op.code == IR_OP_UNBOX_INT32 ? pass_boxed(graph, IR_OP_BOX_FLOAT64, input->inputs.items[0]) : IR_NONE;
    }
    case IR_OP_FLOAT64_TO_INT32: {
      const ir_node_t *input = ir_node(graph, inputs[0]);
      if (input->op.code != IR_OP_INT32_TO_FLOAT64 && input->op.code != IR_OP_UINT32_TO_FLOAT64) return IR_NONE;
      return pass_may_stand_for_result(graph, input->inputs.items[0]) ? input->inputs.items[0] : IR_NONE;
    }
    case IR_OP_BOX_INT32:
      return pass_int32_of(graph, inputs[0], &lhs) ? pass_constant(constants, graph, ir_value_int32(lhs)) : IR_NONE;
    case IR_OP_BOX_BOOL:
      if (!ir_constant_value(graph, inputs[0], &bits)) return IR_NONE;
      return pass_constant(constants, graph, bits ? IR_VALUE_TRUE : IR_VALUE_FALSE);
    case IR_OP_CHECK_INT32:
      if (!ir_constant_value(graph, inputs[0], &bits) || ir_node(graph, inputs[0])->repr != OPT_REPR_TAGGED) return IR_NONE;
      if (!ir_value_as_int32(bits, &lhs)) return IR_NONE;
      return pass_constant_typed(constants, graph, (uint32_t)lhs, OPT_REPR_INT32);
    case IR_OP_INT32_BINARY:
    case IR_OP_INT32_WRAPPING_BINARY: {
      bool wrapping = op->code == IR_OP_INT32_WRAPPING_BINARY;
      if (!pass_int32_of(graph, inputs[1], &rhs)) return IR_NONE;
      if (!pass_int32_of(graph, inputs[0], &lhs))
        return pass_is_int32_identity(op->binary, rhs, wrapping) && pass_may_stand_for_result(graph, inputs[0]) ? inputs[0] : IR_NONE;
      int32_t result = pass_fold_int32_wrapping_binary(op->binary, lhs, rhs);
      if (!wrapping && !pass_fold_int32_binary(op->binary, lhs, rhs, &result)) return IR_NONE;
      return pass_constant_typed(constants, graph, (uint32_t)result, OPT_REPR_INT32);
    }
    case IR_OP_INT32_MIN:
    case IR_OP_INT32_MAX:
      if (inputs[0] == inputs[1]) return inputs[0];
      if (!pass_int32_of(graph, inputs[0], &lhs) || !pass_int32_of(graph, inputs[1], &rhs)) return IR_NONE;
      return (op->code == IR_OP_INT32_MIN) == (lhs < rhs) ? inputs[0] : inputs[1];
    case IR_OP_FLOAT64_MIN:
    case IR_OP_FLOAT64_MAX: return inputs[0] == inputs[1] ? inputs[0] : IR_NONE;
    case IR_OP_INT32_COMPARE:
      if (!pass_int32_of(graph, inputs[0], &lhs) || !pass_int32_of(graph, inputs[1], &rhs)) return IR_NONE;
      return pass_constant_typed(constants, graph, pass_fold_int32_compare(op->comparison, lhs, rhs), OPT_REPR_BOOL);
    default: return IR_NONE;
  }
}

static bool pass_is_int32_term(const ir_graph_t *graph, ir_node_id_t value, uint32_t depth) {
  const ir_node_t *node = ir_node(graph, ir_unrefined(graph, value));
  if (node->op.code == IR_OP_INT32_TO_FLOAT64) return true;
  if (node->op.code == IR_OP_CONSTANT) return node->repr == OPT_REPR_FLOAT64 && ir_value_as_int32(node->op.bits, NULL);
  if (node->op.code != IR_OP_FLOAT64_BINARY || depth >= 8) return false;
  if (node->op.binary != IR_BINARY_ADD && node->op.binary != IR_BINARY_SUB) return false;
  return pass_is_int32_term(graph, node->inputs.items[0], depth + 1) && pass_is_int32_term(graph, node->inputs.items[1], depth + 1);
}

static ir_node_id_t pass_build_int32_term(ir_graph_t *graph, pass_constants_t *constants, ir_node_id_t value, ir_block_id_t block, uint32_t *position, uint32_t pc) {
  const ir_node_t *node = ir_node(graph, ir_unrefined(graph, value));
  int32_t integer = 0;
  if (node->op.code == IR_OP_INT32_TO_FLOAT64) return node->inputs.items[0];
  if (node->op.code == IR_OP_CONSTANT) {
    ir_value_as_int32(node->op.bits, &integer);
    return pass_constant_typed(constants, graph, (uint32_t)integer, OPT_REPR_INT32);
  }
  ir_op_t op = ir_op(IR_OP_INT32_WRAPPING_BINARY);
  op.binary = node->op.binary;
  ir_node_id_t lhs = node->inputs.items[0], rhs = node->inputs.items[1];
  lhs = pass_build_int32_term(graph, constants, lhs, block, position, pc);
  rhs = pass_build_int32_term(graph, constants, rhs, block, position, pc);
  ir_node_id_t result = ir_add_node(graph, op, (ir_node_id_t[]){lhs, rhs}, 2, OPT_REPR_INT32, pc);
  opt_vec_insert(&graph->arena, &ir_block(graph, block)->body, *position, result);
  (*position)++;
  return result;
}

static ir_node_id_t pass_checked_double(const ir_graph_t *graph, ir_node_id_t value) {
  const ir_node_t *check = ir_node(graph, value);
  if (check->op.code != IR_OP_CHECK_INT32) return IR_NONE;
  const ir_node_t *boxed = ir_node(graph, ir_unrefined(graph, check->inputs.items[0]));
  return boxed->op.code == IR_OP_BOX_FLOAT64 ? boxed->inputs.items[0] : IR_NONE;
}

static bool pass_is_signed_comparison(ir_comparison_t comparison) {
  return comparison < IR_CMP_BELOW;
}

static opt_bitset_t pass_dissolvable_checks(const ir_graph_t *graph, opt_arena_t *arena) {
  opt_bitset_t checks = opt_bitset_new(arena, ir_node_count(graph));
  opt_bitset_t blocked = opt_bitset_new(arena, ir_node_count(graph));
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    const ir_block_t *data = ir_block(graph, block);
    for (uint32_t i = 0, count = ir_block_node_count(data); i < count; i++) {
      ir_node_id_t node = ir_block_node_at(data, i);
      const ir_node_t *node_data = ir_node(graph, node);
      if (pass_checked_double(graph, node) != IR_NONE) opt_bitset_insert(&checks, node);
      bool takes_double = (node_data->op.code == IR_OP_INT32_COMPARE && pass_is_signed_comparison(node_data->op.comparison))
        || node_data->op.code == IR_OP_INT32_TO_FLOAT64 || node_data->op.code == IR_OP_BOX_INT32;
      if (takes_double) continue;
      for (uint32_t j = 0; j < node_data->inputs.len; j++) opt_bitset_insert(&blocked, node_data->inputs.items[j]);
    }
  }
  opt_bitset_subtract(&checks, &blocked);
  return checks;
}

static ir_node_id_t pass_dissolved_double(const ir_graph_t *graph, const opt_bitset_t *dissolvable, ir_node_id_t value) {
  if (value >= dissolvable->len || !opt_bitset_contains(dissolvable, value)) return IR_NONE;
  return pass_checked_double(graph, value);
}

static ir_node_id_t pass_fold_adding(ir_graph_t *graph, pass_constants_t *constants, const pass_replacements_t *replacements, const opt_bitset_t *dissolvable, ir_block_id_t block, uint32_t *index, const ir_op_t *op, const ir_node_id_t *inputs) {
  uint32_t pc = ir_node(graph, ir_block(graph, block)->body.items[*index])->bc_off;
  if (op->code == IR_OP_INT32_TO_FLOAT64) return pass_dissolved_double(graph, dissolvable, inputs[0]);
  if (op->code == IR_OP_BOX_INT32) return pass_dissolved_double(graph, dissolvable, inputs[0]) != IR_NONE ? ir_node(graph, inputs[0])->inputs.items[0] : IR_NONE;
  if (op->code == IR_OP_INT32_COMPARE && pass_is_signed_comparison(op->comparison)) {
    ir_node_id_t doubles[2] = {pass_dissolved_double(graph, dissolvable, inputs[0]), pass_dissolved_double(graph, dissolvable, inputs[1])};
    if (doubles[0] == IR_NONE && doubles[1] == IR_NONE) return IR_NONE;
    for (uint32_t i = 0; i < 2; i++) {
      if (doubles[i] != IR_NONE) continue;
      doubles[i] = ir_add_node(graph, ir_op(IR_OP_INT32_TO_FLOAT64), &inputs[i], 1, OPT_REPR_FLOAT64, pc);
      opt_vec_insert(&graph->arena, &ir_block(graph, block)->body, *index, doubles[i]);
      (*index)++;
    }
    ir_op_t compare = ir_op(IR_OP_FLOAT64_COMPARE);
    compare.comparison = op->comparison;
    ir_node_id_t result = ir_add_node(graph, compare, doubles, 2, OPT_REPR_BOOL, pc);
    opt_vec_insert(&graph->arena, &ir_block(graph, block)->body, *index, result);
    (*index)++;
    return result;
  }
  if (op->code == IR_OP_FLOAT64_TO_INT32) {
    const ir_node_t *term = ir_node(graph, ir_unrefined(graph, inputs[0]));
    if (term->op.code != IR_OP_FLOAT64_BINARY || !pass_is_int32_term(graph, inputs[0], 0)) return IR_NONE;
    return pass_build_int32_term(graph, constants, inputs[0], block, index, pc);
  }
  if (op->code != IR_OP_CHECK_NUMBER) return IR_NONE;
  ir_node_id_t integer = pass_boxed(graph, IR_OP_BOX_INT32, inputs[0]);
  if (integer == IR_NONE) return IR_NONE;
  integer = pass_resolve(replacements, integer);
  ir_node_id_t dissolved = pass_dissolved_double(graph, dissolvable, integer);
  if (dissolved != IR_NONE) return dissolved;
  ir_node_id_t conversion = ir_add_node(graph, ir_op(IR_OP_INT32_TO_FLOAT64), &integer, 1, OPT_REPR_FLOAT64, pc);
  opt_vec_insert(&graph->arena, &ir_block(graph, block)->body, *index, conversion);
  (*index)++;
  return conversion;
}

static bool pass_simplify_control(ir_graph_t *graph, pass_constants_t *constants, const pass_replacements_t *replacements, ir_node_id_t control, ir_op_t *new_op, ir_node_id_t *new_inputs, uint32_t *new_count) {
  ir_op_t op = ir_node(graph, control)->op;
  const ir_node_vec_t *control_inputs = &ir_node(graph, control)->inputs;
  if (!control_inputs->len) return false;
  ir_node_id_t input = pass_resolve(replacements, control_inputs->items[0]);
  ir_node_id_t other = control_inputs->len > 1 ? pass_resolve(replacements, control_inputs->items[1]) : IR_NONE;
  bool truthy = op.code == IR_OP_BRANCH_TRUTHY;
  bool bool_branch = op.code == IR_OP_BRANCH && op.branch.condition.kind == IR_COND_BOOL;
  ir_block_id_t if_true = truthy ? op.truthy.if_true : op.branch.if_true;
  ir_block_id_t if_false = truthy ? op.truthy.if_false : op.branch.if_false;
  uint64_t bits;
  *new_count = 0;
  if ((truthy || bool_branch) && ir_constant_value(graph, input, &bits)) {
    opt_repr_t repr = ir_node(graph, input)->repr;
    int truth = pass_constant_truthiness(bits, repr == OPT_REPR_NONE ? OPT_REPR_TAGGED : repr);
    if (truth < 0) return false;
    *new_op = ir_op(IR_OP_JUMP);
    new_op->target = truth ? if_true : if_false;
    return true;
  }
  if (op.code == IR_OP_BRANCH) {
    int holds = pass_constant_condition(graph, op.branch.condition, input, other);
    if (holds < 0) return false;
    *new_op = ir_op(IR_OP_JUMP);
    new_op->target = holds ? if_true : if_false;
    return true;
  }
  if (!truthy) return false;
  const ir_node_t *input_node = ir_node(graph, input);
  if (input_node->op.code == IR_OP_BOX_BOOL) {
    *new_op = ir_op(IR_OP_BRANCH);
    new_op->branch.condition = ir_cond(IR_COND_BOOL);
    new_op->branch.if_true = if_true;
    new_op->branch.if_false = if_false;
    new_inputs[0] = input_node->inputs.items[0];
    *new_count = 1;
    return true;
  }
  if (input_node->op.code == IR_OP_BOX_INT32) {
    ir_node_id_t unboxed = input_node->inputs.items[0];
    *new_op = ir_op(IR_OP_BRANCH);
    new_op->branch.condition = ir_cond(IR_COND_INT32);
    new_op->branch.condition.comparison = IR_CMP_STRICTLY_INEQUALS;
    new_op->branch.if_true = if_true;
    new_op->branch.if_false = if_false;
    new_inputs[0] = unboxed;
    new_inputs[1] = pass_constant_typed(constants, graph, 0, OPT_REPR_INT32);
    *new_count = 2;
    return true;
  }
  return false;
}

void pass_simplify(ir_graph_t *graph) {
  opt_arena_t scratch;
  opt_arena_init(&scratch);
  pass_constants_t constants;
  pass_constants_init(&constants, graph, &scratch);
  pass_replacements_t replacements;
  pass_replacements_init(&replacements, graph, &scratch);
  opt_bitset_t removed = opt_bitset_new(&scratch, ir_node_count(graph));
  opt_bitset_t dissolvable = pass_dissolvable_checks(graph, &scratch);
  ir_node_vec_t inputs = {0};
  bool changed_control_flow = false;
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    for (uint32_t index = 0; index < ir_block(graph, block)->body.len; index++) {
      ir_node_id_t node = ir_block(graph, block)->body.items[index];
      const ir_node_t *data = ir_node(graph, node);
      inputs.len = 0;
      for (uint32_t i = 0; i < data->inputs.len; i++) opt_vec_push(&scratch, &inputs, pass_resolve(&replacements, data->inputs.items[i]));
      ir_op_t op = data->op;
      ir_node_id_t folded = pass_fold_node(graph, &constants, &op, inputs.items);
      if (folded == IR_NONE) folded = pass_fold_adding(graph, &constants, &replacements, &dissolvable, block, &index, &op, inputs.items);
      if (folded == IR_NONE) continue;
      pass_replace(&replacements, node, folded);
      opt_bitset_insert(&removed, node);
    }

    ir_node_id_t control = ir_control_id(graph, block);
    ir_op_t new_op;
    ir_node_id_t new_inputs[2];
    uint32_t new_count;
    if (pass_simplify_control(graph, &constants, &replacements, control, &new_op, new_inputs, &new_count)) {
      ir_node(graph, control)->op = new_op;
      ir_set_inputs(graph, control, new_inputs, new_count);
      changed_control_flow = true;
    }
  }
  pass_replacements_apply(&replacements, graph);
  pass_remove_nodes(graph, &removed);
  opt_bitset_subtract(&dissolvable, &removed);
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    const ir_block_t *data = ir_block(graph, block);
    for (uint32_t i = 0, count = ir_block_node_count(data); i < count; i++) {
      const ir_node_t *node = ir_node(graph, ir_block_node_at(data, i));
      for (uint32_t j = 0; j < node->inputs.len; j++)
        if (node->inputs.items[j] < dissolvable.len) opt_bitset_remove(&dissolvable, node->inputs.items[j]);
    }
  }
  pass_replacements_t doubles;
  pass_replacements_init(&doubles, graph, &scratch);
  opt_bitset_t dissolved = opt_bitset_new(&scratch, ir_node_count(graph));
  opt_bitset_for_each(&dissolvable, check) {
    ir_node_id_t double_value = pass_checked_double(graph, check);
    if (double_value == IR_NONE) continue;
    pass_replace(&doubles, check, double_value);
    opt_bitset_insert(&dissolved, check);
  }
  pass_replacements_apply(&doubles, graph);
  pass_remove_nodes(graph, &dissolved);
  opt_arena_free(&scratch);
  if (changed_control_flow) pass_clean_up_control_flow(graph);
}
