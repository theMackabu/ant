#include "passes.h"
#include "../ir/ir_value.h"

static bool pass_can_wrap(const ir_op_t *op) {
  if (op->code != IR_OP_INT32_BINARY) return false;
  return op->binary == IR_BINARY_ADD || op->binary == IR_BINARY_SUB || op->binary == IR_BINARY_UNSIGNED_RIGHT_SHIFT;
}

static bool pass_reads_low_bits(const ir_graph_t *graph, ir_node_id_t user, const opt_bitset_t *wrapping) {
  const ir_op_t *op = &ir_node(graph, user)->op;
  if (op->code != IR_OP_INT32_BINARY && op->code != IR_OP_INT32_WRAPPING_BINARY) return false;
  if (ir_binary_is_bitwise(op->binary)) return true;
  if (op->binary == IR_BINARY_MUL) return op->code == IR_OP_INT32_WRAPPING_BINARY;
  if (op->binary != IR_BINARY_ADD && op->binary != IR_BINARY_SUB) return false;
  return op->code == IR_OP_INT32_WRAPPING_BINARY || opt_bitset_contains(wrapping, user);
}

static bool pass_uint32_operand(const ir_graph_t *graph, ir_node_id_t value, bool float64, ir_node_id_t *bits, uint32_t *constant) {
  const ir_node_t *node = ir_node(graph, ir_unrefined(graph, value));
  uint64_t amount;
  *bits = IR_NONE;
  switch (node->op.code) {
    case IR_OP_CONSTANT: {
      if (!float64) {
        if ((int32_t)(uint32_t)node->op.bits < 0) return false;
        *constant = (uint32_t)node->op.bits;
        return true;
      }
      double number = ir_value_to_double(node->op.bits);
      if (!(number >= 0 && number < 4294967296.0) || number != (double)(uint32_t)number) return false;
      *constant = (uint32_t)number;
      return true;
    }
    case IR_OP_INT32_BINARY:
    case IR_OP_INT32_WRAPPING_BINARY:
      if (float64 || node->op.binary != IR_BINARY_UNSIGNED_RIGHT_SHIFT) return false;
      if (!ir_constant_value(graph, node->inputs.items[1], &amount) || (amount & 31)) return false;
      *bits = node->inputs.items[0];
      return true;
    case IR_OP_CHECK_NUMBER:
    case IR_OP_UNBOX_DOUBLE: {
      const ir_node_t *boxed = ir_node(graph, ir_unrefined(graph, node->inputs.items[0]));
      return float64 && boxed->op.code == IR_OP_BOX_FLOAT64 && pass_uint32_operand(graph, boxed->inputs.items[0], true, bits, constant);
    }
    case IR_OP_INT32_TO_FLOAT64: return float64 && pass_uint32_operand(graph, node->inputs.items[0], false, bits, constant);
    case IR_OP_UINT32_TO_FLOAT64: {
      if (!float64) return false;
      const ir_node_t *shift = ir_node(graph, node->inputs.items[0]);
      bool by_zero = shift->op.code == IR_OP_UINT32_SHIFT_RIGHT && ir_constant_value(graph, shift->inputs.items[1], &amount) && !(amount & 31);
      *bits = by_zero ? shift->inputs.items[0] : node->inputs.items[0];
      return true;
    }
    default: return false;
  }
}

static ir_comparison_t pass_unsigned(ir_comparison_t comparison) {
  switch (comparison) {
    case IR_CMP_LESS_THAN: return IR_CMP_BELOW;
    case IR_CMP_LESS_THAN_EQUALS: return IR_CMP_BELOW_OR_EQUAL;
    case IR_CMP_GREATER_THAN: return IR_CMP_ABOVE;
    case IR_CMP_GREATER_THAN_EQUALS: return IR_CMP_ABOVE_OR_EQUAL;
    default: return comparison;
  }
}

static void pass_compare_uint32_bits(ir_graph_t *graph) {
  pass_constants_t constants;
  opt_arena_t scratch;
  opt_arena_init(&scratch);
  pass_constants_init(&constants, graph, &scratch);
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    for (uint32_t index = 0; index < ir_block(graph, block)->body.len; index++) {
      ir_node_id_t node = ir_block(graph, block)->body.items[index];
      ir_op_t op = ir_node(graph, node)->op;
      if (op.code != IR_OP_INT32_COMPARE && op.code != IR_OP_FLOAT64_COMPARE) continue;
      bool float64 = op.code == IR_OP_FLOAT64_COMPARE;
      ir_node_id_t bits[2];
      uint32_t constant[2];
      if (!pass_uint32_operand(graph, ir_node(graph, node)->inputs.items[0], float64, &bits[0], &constant[0])) continue;
      if (!pass_uint32_operand(graph, ir_node(graph, node)->inputs.items[1], float64, &bits[1], &constant[1])) continue;
      if (bits[0] == IR_NONE && bits[1] == IR_NONE) continue;
      ir_node_id_t inputs[2];
      for (uint32_t i = 0; i < 2; i++)
        inputs[i] = bits[i] != IR_NONE ? bits[i] : pass_constant_typed(&constants, graph, constant[i], OPT_REPR_INT32);
      ir_op_t compare = ir_op(IR_OP_INT32_COMPARE);
      compare.comparison = pass_unsigned(op.comparison);
      ir_node(graph, node)->op = compare;
      ir_set_inputs(graph, node, inputs, 2);
    }
  }
  opt_arena_free(&scratch);
}

static constexpr ir_node_id_t PASS_OWNER_NONE = IR_NONE;
static constexpr ir_node_id_t PASS_OWNER_SHARED = IR_NONE - 1;

static void pass_share(ir_node_id_t *owners, ir_frame_state_id_t state, ir_node_id_t owner) {
  owners[state] = owners[state] == PASS_OWNER_NONE ? owner : PASS_OWNER_SHARED;
}

void pass_truncation(ir_graph_t *graph) {
  pass_compare_uint32_bits(graph);
  uint32_t count = ir_node_count(graph);
  opt_arena_t scratch;
  opt_arena_init(&scratch);
  opt_bitset_t wrapping = opt_bitset_new(&scratch, count);
  bool any = false;
  ir_node_vec_t *users = opt_arena_new(&scratch, ir_node_vec_t, count + 1);
  uint32_t states = graph->frame_states.len;
  ir_node_id_t *owners = opt_arena_alloc(&scratch, (states + 1) * sizeof *owners);
  for (uint32_t i = 0; i < states; i++) owners[i] = PASS_OWNER_NONE;
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    const ir_block_t *data = ir_block(graph, block);
    if (data->start_frame_state != IR_NONE) pass_share(owners, data->start_frame_state, PASS_OWNER_SHARED);
    for (uint32_t i = 0, nodes = ir_block_node_count(data); i < nodes; i++) {
      ir_node_id_t node = ir_block_node_at(data, i);
      const ir_node_t *node_data = ir_node(graph, node);
      if (pass_can_wrap(&node_data->op)) {
        opt_bitset_insert(&wrapping, node);
        any = true;
      }
      if (node_data->frame_state != IR_NONE) pass_share(owners, node_data->frame_state, node);
      for (uint32_t j = 0; j < node_data->inputs.len; j++) opt_vec_push(&scratch, &users[node_data->inputs.items[j]], node);
    }
  }
  if (!any) {
    opt_arena_free(&scratch);
    return;
  }
  for (uint32_t i = 0; i < states; i++) {
    ir_frame_state_id_t parent = ir_frame_state(graph, i)->parent;
    if (parent != IR_NONE) pass_share(owners, parent, PASS_OWNER_SHARED);
  }

  opt_bitset_t in_frame_state = opt_bitset_new(&scratch, count);
  for (bool changed = true; changed;) {
    changed = false;
    opt_bitset_clear(&in_frame_state);
    for (uint32_t i = 0; i < states; i++) {
      ir_node_id_t owner = owners[i];
      if (owner < count && opt_bitset_contains(&wrapping, owner)) continue;
      const ir_frame_state_t *state = ir_frame_state(graph, i);
      for (uint32_t j = 0; j < state->values.len; j++) opt_bitset_insert(&in_frame_state, state->values.items[j].value);
    }
    opt_bitset_for_each(&wrapping, node) {
      bool keeps_checking = opt_bitset_contains(&in_frame_state, node);
      for (uint32_t j = 0; j < users[node].len && !keeps_checking; j++) {
        ir_node_id_t user = users[node].items[j];
        bool unused_boxing = ir_node(graph, user)->op.code == IR_OP_BOX_INT32 && !users[user].len
          && !opt_bitset_contains(&in_frame_state, user);
        keeps_checking = !unused_boxing && !pass_reads_low_bits(graph, user, &wrapping);
      }
      if (keeps_checking) {
        opt_bitset_remove(&wrapping, node);
        changed = true;
      }
    }
  }

  opt_bitset_for_each(&wrapping, node) {
    ir_node_t *data = ir_node(graph, node);
    data->op.code = IR_OP_INT32_WRAPPING_BINARY;
    data->frame_state = IR_NONE;
  }
  opt_arena_free(&scratch);
}
