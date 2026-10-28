#include "passes.h"
#include <assert.h>

static ir_comparison_t pass_mirrored(ir_comparison_t comparison) {
  switch (comparison) {
    case IR_CMP_LESS_THAN: return IR_CMP_GREATER_THAN;
    case IR_CMP_LESS_THAN_EQUALS: return IR_CMP_GREATER_THAN_EQUALS;
    case IR_CMP_GREATER_THAN: return IR_CMP_LESS_THAN;
    case IR_CMP_GREATER_THAN_EQUALS: return IR_CMP_LESS_THAN_EQUALS;
    case IR_CMP_BELOW: return IR_CMP_ABOVE;
    case IR_CMP_BELOW_OR_EQUAL: return IR_CMP_ABOVE_OR_EQUAL;
    case IR_CMP_ABOVE: return IR_CMP_BELOW;
    case IR_CMP_ABOVE_OR_EQUAL: return IR_CMP_BELOW_OR_EQUAL;
    default: return comparison;
  }
}

static uint32_t *pass_count_all_uses(const ir_graph_t *graph, opt_arena_t *arena) {
  uint32_t *uses = opt_arena_new(arena, uint32_t, ir_node_count(graph) + 1);
  ir_node_vec_t values = {0};
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    const ir_block_t *data = ir_block(graph, block);
    for (uint32_t i = 0, count = ir_block_node_count(data); i < count; i++) {
      ir_used_values(graph, ir_block_node_at(data, i), arena, &values);
      for (uint32_t j = 0; j < values.len; j++) uses[values.items[j]]++;
    }
  }
  return uses;
}

static void pass_remove_predecessor(ir_graph_t *graph, ir_block_id_t target, ir_block_id_t predecessor) {
  ir_block_t *data = ir_block(graph, target);
  int index = ir_block_vec_position(&data->predecessors, predecessor);
  assert(index >= 0);
  opt_vec_remove(&data->predecessors, (uint32_t)index);
  for (uint32_t i = 0; i < data->phis.len; i++) opt_vec_remove(&ir_node(graph, data->phis.items[i])->inputs, (uint32_t)index);
}

static void pass_add_predecessor(ir_graph_t *graph, ir_block_id_t target, ir_block_id_t predecessor, ir_block_id_t like) {
  ir_block_t *data = ir_block(graph, target);
  int index = ir_block_vec_position(&data->predecessors, like);
  assert(index >= 0);
  opt_vec_push(&graph->arena, &data->predecessors, predecessor);
  for (uint32_t i = 0; i < data->phis.len; i++) {
    ir_node_id_t phi = data->phis.items[i];
    ir_push_input(graph, phi, ir_node(graph, phi)->inputs.items[index]);
  }
}

static void pass_thread_boolean_phis(ir_graph_t *graph, const uint32_t *uses, uint32_t use_count, opt_arena_t *scratch) {
  uint32_t block_count = ir_block_count(graph);
  ir_block_vec_t *new_blocks = opt_arena_new(scratch, ir_block_vec_t, block_count + 1);
  opt_bitset_t threaded = opt_bitset_new(scratch, block_count);
  for (ir_block_id_t block = 0; block < block_count; block++) {
    const ir_block_t *data = ir_block(graph, block);
    if (data->control == IR_NONE || data->phis.len != 1) continue;
    ir_node_id_t phi = data->phis.items[0];
    ir_node_id_t control = data->control;
    ir_op_t op = ir_node(graph, control)->op;
    if (op.code != IR_OP_BRANCH || op.branch.condition.kind != IR_COND_BOOL) continue;
    ir_block_id_t if_true = op.branch.if_true, if_false = op.branch.if_false;
    bool all_jump_here = true;
    for (uint32_t i = 0; i < data->predecessors.len && all_jump_here; i++) {
      ir_block_id_t predecessor = data->predecessors.items[i];
      const ir_op_t *predecessor_op = &ir_control(graph, predecessor)->op;
      all_jump_here = predecessor_op->code == IR_OP_JUMP && predecessor_op->target == block && predecessor < block;
    }
    if (data->body.len || data->is_loop_header || ir_node(graph, control)->inputs.items[0] != phi
      || phi >= use_count || uses[phi] != 1 || if_true <= block || if_false <= block || !all_jump_here)
      continue;
    uint32_t pc = ir_node(graph, control)->bc_off;
    ir_block_vec_t predecessors = {0};
    ir_node_vec_t inputs = {0};
    opt_vec_clone(scratch, &predecessors, &data->predecessors);
    opt_vec_clone(scratch, &inputs, &ir_node(graph, phi)->inputs);
    for (uint32_t i = 0; i < predecessors.len && i < inputs.len; i++) {
      ir_block_id_t predecessor = predecessors.items[i];
      ir_node_id_t input = inputs.items[i];
      ir_node_id_t predecessor_control = ir_control_id(graph, predecessor);
      uint64_t bits;
      if (ir_node(graph, input)->repr == OPT_REPR_BOOL && ir_constant_value(graph, input, &bits)) {
        ir_block_id_t target = bits ? if_true : if_false;
        pass_make_jump(graph, predecessor_control, target);
        pass_add_predecessor(graph, target, predecessor, block);
        continue;
      }
      bool is_cold = ir_block(graph, predecessor)->is_cold;
      ir_block_id_t targets[2] = {if_true, if_false}, edges[2];
      for (uint32_t e = 0; e < 2; e++) {
        ir_block_id_t edge = ir_add_block(graph);
        opt_vec_push(&graph->arena, &ir_block(graph, edge)->predecessors, predecessor);
        ir_block(graph, edge)->is_cold = is_cold;
        ir_node_id_t jump = ir_add_node(graph, ir_op(IR_OP_JUMP), NULL, 0, OPT_REPR_NONE, pc);
        ir_node(graph, jump)->op.target = targets[e];
        ir_block(graph, edge)->control = jump;
        pass_add_predecessor(graph, targets[e], edge, block);
        edges[e] = edge;
      }
      ir_node_t *branch = ir_node(graph, predecessor_control);
      branch->op = ir_op(IR_OP_BRANCH);
      branch->op.branch.condition = ir_cond(IR_COND_BOOL);
      branch->op.branch.if_true = edges[0];
      branch->op.branch.if_false = edges[1];
      ir_set_inputs(graph, predecessor_control, &input, 1);
      opt_vec_push(scratch, &new_blocks[predecessor], edges[0]);
      opt_vec_push(scratch, &new_blocks[predecessor], edges[1]);
    }
    pass_remove_predecessor(graph, if_true, block);
    pass_remove_predecessor(graph, if_false, block);
    opt_bitset_insert(&threaded, block);
  }
  if (opt_bitset_is_empty(&threaded)) return;
  ir_block_vec_t order = {0};
  for (ir_block_id_t block = 0; block < block_count; block++) {
    if (opt_bitset_contains(&threaded, block)) continue;
    opt_vec_push(scratch, &order, block);
    for (uint32_t i = 0; i < new_blocks[block].len; i++) opt_vec_push(scratch, &order, new_blocks[block].items[i]);
  }
  ir_reorder_blocks(graph, order.items, order.len);
}

void pass_branch_fusion(ir_graph_t *graph) {
  opt_arena_t scratch;
  opt_arena_init(&scratch);
  uint32_t use_count = ir_node_count(graph);
  uint32_t *uses = pass_count_all_uses(graph, &scratch);
  pass_thread_boolean_phis(graph, uses, use_count, &scratch);
  opt_bitset_t removed = opt_bitset_new(&scratch, ir_node_count(graph));
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    ir_node_id_t control = ir_control_id(graph, block);
    ir_op_t op = ir_node(graph, control)->op;
    if (op.code != IR_OP_BRANCH || op.branch.condition.kind != IR_COND_BOOL) continue;
    ir_node_id_t comparison = ir_node(graph, control)->inputs.items[0];
    if (comparison >= use_count || uses[comparison] != 1 || !ir_node_vec_contains(&ir_block(graph, block)->body, comparison)) continue;
    const ir_node_t *compare = ir_node(graph, comparison);
    ir_condition_t condition;
    switch (compare->op.code) {
      case IR_OP_INT32_COMPARE:
        condition = ir_cond(IR_COND_INT32);
        condition.comparison = compare->op.comparison;
        break;
      case IR_OP_FLOAT64_COMPARE:
        condition = ir_cond(IR_COND_FLOAT64);
        condition.comparison = compare->op.comparison;
        break;
      case IR_OP_TAGGED_EQUALS:
        condition = ir_cond(IR_COND_TAGGED_EQUALS);
        condition.equal = compare->op.equal;
        break;
      default: continue;
    }
    ir_node_id_t inputs[2] = {compare->inputs.items[0], compare->inputs.items[1]};
    if (ir_is_constant(graph, inputs[0]) && !ir_is_constant(graph, inputs[1])) {
      ir_node_id_t swapped = inputs[0];
      inputs[0] = inputs[1];
      inputs[1] = swapped;
      if (condition.kind == IR_COND_INT32 || condition.kind == IR_COND_FLOAT64) condition.comparison = pass_mirrored(condition.comparison);
    }
    ir_node_t *branch = ir_node(graph, control);
    branch->op = ir_op(IR_OP_BRANCH);
    branch->op.branch.condition = condition;
    branch->op.branch.if_true = op.branch.if_true;
    branch->op.branch.if_false = op.branch.if_false;
    ir_set_inputs(graph, control, inputs, 2);
    opt_bitset_insert(&removed, comparison);
  }
  pass_remove_nodes(graph, &removed);
  opt_arena_free(&scratch);
}
