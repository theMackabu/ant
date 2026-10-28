#include "passes.h"
#include "../ir/ir_value.h"

#include <assert.h>

typedef struct {
  opt_repr_t repr;
  ir_opcode_t box_op;
  ir_opcode_t check_op;
  bool converts_int32;
} pass_unboxed_t;

static const pass_unboxed_t pass_unboxed[] = {
  {.repr = OPT_REPR_INT32, .box_op = IR_OP_BOX_INT32, .check_op = IR_OP_CHECK_INT32},
  {.repr = OPT_REPR_BOOL, .box_op = IR_OP_BOX_BOOL, .check_op = IR_OP__COUNT},
  {.repr = OPT_REPR_FLOAT64, .box_op = IR_OP_BOX_FLOAT64, .check_op = IR_OP_CHECK_NUMBER, .converts_int32 = true},
};

static bool pass_unboxed_constant(const pass_unboxed_t *unboxed, uint64_t bits, uint64_t *out) {
  int32_t integer;
  switch (unboxed->repr) {
    case OPT_REPR_INT32:
      if (!ir_value_as_int32(bits, &integer)) return false;
      *out = (uint32_t)integer;
      return true;
    case OPT_REPR_BOOL:
      if (bits != IR_VALUE_TRUE && bits != IR_VALUE_FALSE) return false;
      *out = bits == IR_VALUE_TRUE;
      return true;
    default:
      if (!ir_value_is_number(bits)) return false;
      *out = bits;
      return true;
  }
}

static bool pass_wants_unboxed(const pass_unboxed_t *unboxed, ir_opcode_t code) {
  switch (unboxed->repr) {
    case OPT_REPR_INT32: return code == IR_OP_CHECK_INT32;
    case OPT_REPR_BOOL: return code == IR_OP_BRANCH_TRUTHY;
    default: return code == IR_OP_UNBOX_DOUBLE || code == IR_OP_CHECK_NUMBER;
  }
}

typedef enum {
  PASS_UNBOXED_NONE,
  PASS_UNBOXED_VALUE,
  PASS_UNBOXED_CONSTANT,
  PASS_UNBOXED_CONVERTED,
} pass_unboxed_input_kind_t;

typedef struct {
  pass_unboxed_input_kind_t kind;
  ir_node_id_t value;
  uint64_t bits;
} pass_unboxed_input_t;

static pass_unboxed_input_t pass_unboxed_input(const ir_graph_t *graph, const pass_unboxed_t *unboxed, ir_node_id_t input, const opt_bitset_t *candidates) {
  const ir_node_t *node = ir_node(graph, input);
  if (node->op.code == unboxed->box_op) return (pass_unboxed_input_t){PASS_UNBOXED_VALUE, node->inputs.items[0], 0};
  if (node->op.code == IR_OP_CONSTANT && node->repr == OPT_REPR_TAGGED) {
    uint64_t bits;
    if (pass_unboxed_constant(unboxed, node->op.bits, &bits)) return (pass_unboxed_input_t){PASS_UNBOXED_CONSTANT, IR_NONE, bits};
    return (pass_unboxed_input_t){PASS_UNBOXED_NONE, IR_NONE, 0};
  }
  if (node->op.code == IR_OP_PHI && opt_bitset_has(candidates, input)) return (pass_unboxed_input_t){PASS_UNBOXED_VALUE, input, 0};
  if (node->op.code == IR_OP_BOX_INT32 && unboxed->converts_int32)
    return (pass_unboxed_input_t){PASS_UNBOXED_CONVERTED, node->inputs.items[0], 0};
  if (unboxed->converts_int32 && node->repr == OPT_REPR_INT32) return (pass_unboxed_input_t){PASS_UNBOXED_CONVERTED, input, 0};
  return (pass_unboxed_input_t){PASS_UNBOXED_NONE, IR_NONE, 0};
}

static void pass_spread_over_phis(const ir_graph_t *graph, const opt_bitset_t *phis, opt_bitset_t *marked) {
  bool changed = true;
  while (changed) {
    changed = false;
    opt_bitset_for_each(phis, phi) {
      const ir_node_t *node = ir_node(graph, phi);
      for (uint32_t i = 0; i < node->inputs.len; i++) {
        ir_node_id_t input = node->inputs.items[i];
        if (opt_bitset_has(phis, input) && opt_bitset_contains(marked, phi) != opt_bitset_contains(marked, input)) {
          opt_bitset_insert(marked, phi);
          opt_bitset_insert(marked, input);
          changed = true;
        }
      }
    }
  }
}

static opt_bitset_t pass_checked_phis(const ir_graph_t *graph, const pass_unboxed_t *unboxed, const opt_bitset_t *phis, opt_arena_t *scratch) {
  opt_bitset_t checked = opt_bitset_new(scratch, ir_node_count(graph));
  if (unboxed->check_op == IR_OP__COUNT) return checked;
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    const ir_block_t *data = ir_block(graph, block);
    for (uint32_t i = 0; i < data->body.len; i++) {
      const ir_node_t *node = ir_node(graph, data->body.items[i]);
      if (node->op.code == unboxed->check_op && opt_bitset_has(phis, node->inputs.items[0])) opt_bitset_insert(&checked, node->inputs.items[0]);
    }
  }
  pass_spread_over_phis(graph, phis, &checked);
  opt_bitset_t poisoned = opt_bitset_new(scratch, ir_node_count(graph));
  opt_bitset_for_each(phis, phi) {
    const ir_node_t *node = ir_node(graph, phi);
    for (uint32_t i = 0; i < node->inputs.len; i++) {
      const ir_node_t *input = ir_node(graph, node->inputs.items[i]);
      uint64_t bits;
      if (input->op.code == IR_OP_CONSTANT && input->repr == OPT_REPR_TAGGED && !pass_unboxed_constant(unboxed, input->op.bits, &bits))
        opt_bitset_insert(&poisoned, phi);
    }
  }
  pass_spread_over_phis(graph, phis, &poisoned);
  opt_bitset_subtract(&checked, &poisoned);
  return checked;
}

typedef struct {
  ir_block_id_t block;
  ir_frame_state_t *states;
  bool *valid;
} pass_checkable_t;

static pass_checkable_t *pass_checkable_inputs(const ir_graph_t *graph, const pass_unboxed_t *unboxed, const opt_bitset_t *checked, opt_arena_t *scratch) {
  pass_checkable_t *checkable = opt_arena_new(scratch, pass_checkable_t, ir_node_count(graph) + 1);
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    const ir_block_t *data = ir_block(graph, block);
    opt_exit_kind_t kind = OPT_EXIT_NOT_INT32;
    bool checkable_block = unboxed->check_op != IR_OP__COUNT && data->bytecode_start != IR_NONE;
    if (checkable_block) {
      ir_op_t check = ir_op(unboxed->check_op);
      checkable_block = ir_op_exit_kind(&check, &kind);
    }
    for (uint32_t p = 0; p < data->phis.len; p++) {
      ir_node_id_t phi = data->phis.items[p];
      const ir_node_t *phi_node = ir_node(graph, phi);
      bool gated = !checkable_block || ir_exit_sites_contain_slot(graph, data->func, data->bytecode_start, kind, phi_node->op.slot);
      uint32_t count = phi_node->inputs.len < data->predecessors.len ? phi_node->inputs.len : data->predecessors.len;
      pass_checkable_t entry = {
        .block = block,
        .states = opt_arena_new(scratch, ir_frame_state_t, count + 1),
        .valid = opt_arena_new(scratch, bool, count + 1),
      };
      for (uint32_t i = 0; i < count; i++) {
        ir_block_id_t predecessor = data->predecessors.items[i];
        ir_node_id_t input = phi_node->inputs.items[i];
        const ir_op_t *predecessor_control = &ir_control(graph, predecessor)->op;
        bool can_check = !gated && opt_bitset_contains(checked, phi) && ir_node(graph, input)->repr == OPT_REPR_TAGGED
          && predecessor_control->code == IR_OP_JUMP && predecessor_control->target == block;
        if (can_check) entry.valid[i] = pass_frame_state_entering(graph, block, predecessor, scratch, &entry.states[i]);
      }
      checkable[phi] = entry;
    }
  }
  return checkable;
}

static ir_node_id_t pass_check_at_end(ir_graph_t *graph, const pass_unboxed_t *unboxed, ir_block_id_t predecessor, ir_node_id_t value, const ir_frame_state_t *state, uint32_t slot) {
  ir_frame_state_id_t frame_state = ir_add_frame_state(graph, state);
  ir_op_t op = ir_op(unboxed->check_op);
  op.slot = slot;
  ir_node_id_t check = ir_add_node(graph, op, &value, 1, unboxed->repr, state->bc_off);
  ir_node(graph, check)->frame_state = frame_state;
  opt_vec_push(&graph->arena, &ir_block(graph, predecessor)->body, check);
  return check;
}

static ir_node_id_t pass_convert_int32(ir_graph_t *graph, ir_node_id_t integer) {
  ir_block_id_t block = IR_NONE;
  uint32_t position = 0;
  for (ir_block_id_t index = 0; index < ir_block_count(graph) && block == IR_NONE; index++) {
    const ir_block_t *data = ir_block(graph, index);
    if (ir_node_vec_contains(&data->phis, integer)) {
      block = index;
      position = 0;
      break;
    }
    int found = ir_node_vec_position(&data->body, integer);
    if (found >= 0) {
      block = index;
      position = (uint32_t)found + 1;
    }
  }
  assert(block != IR_NONE);
  ir_node_id_t converted = ir_add_node(graph, ir_op(IR_OP_INT32_TO_FLOAT64), &integer, 1, OPT_REPR_FLOAT64, ir_node(graph, integer)->bc_off);
  opt_vec_insert(&graph->arena, &ir_block(graph, block)->body, position, converted);
  return converted;
}

static ir_node_id_t pass_box_phi(ir_graph_t *graph, const pass_unboxed_t *unboxed, ir_node_id_t phi) {
  ir_block_id_t block = IR_NONE;
  for (ir_block_id_t index = 0; index < ir_block_count(graph); index++) {
    if (ir_node_vec_contains(&ir_block(graph, index)->phis, phi)) {
      block = index;
      break;
    }
  }
  assert(block != IR_NONE);
  ir_node_id_t boxed = ir_add_node(graph, ir_op(unboxed->box_op), &phi, 1, OPT_REPR_TAGGED, ir_node(graph, phi)->bc_off);
  opt_vec_insert(&graph->arena, &ir_block(graph, block)->body, 0, boxed);
  return boxed;
}

static void pass_unbox_phis(ir_graph_t *graph, const pass_unboxed_t *unboxed, opt_arena_t *scratch) {
  uint32_t count = ir_node_count(graph);
  opt_bitset_t candidates = opt_bitset_new(scratch, count);
  ir_node_vec_t phis = {0};
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    const ir_block_t *data = ir_block(graph, block);
    for (uint32_t i = 0; i < data->phis.len; i++) {
      if (ir_node(graph, data->phis.items[i])->repr != OPT_REPR_TAGGED) continue;
      opt_vec_push(scratch, &phis, data->phis.items[i]);
      opt_bitset_insert(&candidates, data->phis.items[i]);
    }
  }
  opt_bitset_t benefiting = opt_bitset_new(scratch, count);
  opt_bitset_for_each(&candidates, phi) {
    const ir_node_t *node = ir_node(graph, phi);
    for (uint32_t i = 0; i < node->inputs.len; i++) {
      if (ir_node(graph, node->inputs.items[i])->op.code == unboxed->box_op) {
        opt_bitset_insert(&benefiting, phi);
        break;
      }
    }
  }
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    const ir_block_t *data = ir_block(graph, block);
    for (uint32_t i = 0; i < data->body.len + 1; i++) {
      ir_node_id_t node = i < data->body.len ? data->body.items[i] : data->control;
      if (node == IR_NONE) continue;
      const ir_node_t *user = ir_node(graph, node);
      if (pass_wants_unboxed(unboxed, user->op.code) && user->inputs.len && opt_bitset_has(&candidates, user->inputs.items[0]))
        opt_bitset_insert(&benefiting, user->inputs.items[0]);
    }
  }
  if (opt_bitset_is_empty(&benefiting)) return;
  opt_bitset_t checked = pass_checked_phis(graph, unboxed, &candidates, scratch);
  pass_checkable_t *checkable = pass_checkable_inputs(graph, unboxed, &checked, scratch);

  bool changed = true;
  while (changed) {
    changed = false;
    for (uint32_t p = 0; p < phis.len; p++) {
      ir_node_id_t phi = phis.items[p];
      if (!opt_bitset_contains(&candidates, phi)) continue;
      const ir_node_t *node = ir_node(graph, phi);
      for (uint32_t i = 0; i < node->inputs.len; i++) {
        pass_unboxed_input_t input = pass_unboxed_input(graph, unboxed, node->inputs.items[i], &candidates);
        if (input.kind == PASS_UNBOXED_NONE && !checkable[phi].valid[i]) {
          opt_bitset_remove(&candidates, phi);
          changed = true;
          break;
        }
      }
    }
  }

  opt_bitset_t useful = opt_bitset_new(scratch, count);
  opt_bitset_for_each(&benefiting, phi)
    if (opt_bitset_contains(&candidates, phi)) opt_bitset_insert(&useful, phi);
  pass_spread_over_phis(graph, &candidates, &useful);
  candidates = useful;
  if (opt_bitset_is_empty(&candidates)) return;

  pass_constants_t constants;
  pass_constants_init(&constants, graph, scratch);
  opt_map_t conversions, checks;
  opt_map_init(&conversions, scratch, 1);
  opt_map_init(&checks, scratch, 2);
  ir_node_vec_t inputs = {0};
  opt_bitset_for_each(&candidates, phi) {
    const pass_checkable_t *entry = &checkable[phi];
    inputs.len = 0;
    for (uint32_t i = 0; i < ir_node(graph, phi)->inputs.len; i++) {
      ir_node_id_t input = ir_node(graph, phi)->inputs.items[i];
      pass_unboxed_input_t unboxed_input = pass_unboxed_input(graph, unboxed, input, &candidates);
      ir_node_id_t value = IR_NONE;
      uint64_t bits;
      switch (unboxed_input.kind) {
        case PASS_UNBOXED_VALUE: value = unboxed_input.value; break;
        case PASS_UNBOXED_CONSTANT: value = pass_constant_typed(&constants, graph, unboxed_input.bits, unboxed->repr); break;
        case PASS_UNBOXED_CONVERTED: {
          ir_node_id_t integer = unboxed_input.value;
          if (ir_constant_value(graph, integer, &bits)) {
            double number = (double)(int32_t)(uint32_t)bits;
            value = pass_constant_typed(&constants, graph, ir_value_double_bits(number), unboxed->repr);
            break;
          }
          uint64_t *known = opt_map_get1(&conversions, integer);
          if (known) value = (ir_node_id_t)*known;
          else {
            value = pass_convert_int32(graph, integer);
            opt_map_set1(&conversions, integer, value);
          }
          break;
        }
        case PASS_UNBOXED_NONE: {
          ir_block_id_t predecessor = ir_block(graph, entry->block)->predecessors.items[i];
          assert(entry->valid[i]);
          uint64_t *known = opt_map_get2(&checks, predecessor, input);
          if (known) value = (ir_node_id_t)*known;
          else {
            value = pass_check_at_end(graph, unboxed, predecessor, input, &entry->states[i], ir_node(graph, phi)->op.slot);
            opt_map_set2(&checks, predecessor, input, value);
          }
          break;
        }
      }
      opt_vec_push(scratch, &inputs, value);
    }
    ir_set_inputs(graph, phi, inputs.items, inputs.len);
    ir_node(graph, phi)->repr = unboxed->repr;
  }

  pass_replacements_t replacements;
  pass_replacements_init(&replacements, graph, scratch);
  opt_bitset_t removed = opt_bitset_new(scratch, ir_node_count(graph));
  opt_map_t boxes, local_boxes;
  opt_map_init(&boxes, scratch, 1);
  opt_map_init(&local_boxes, scratch, 2);
  ir_node_vec_t users = {0};
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    const ir_block_t *data = ir_block(graph, block);
    users.len = 0;
    for (uint32_t i = 0, nodes = ir_block_node_count(data); i < nodes; i++) opt_vec_push(scratch, &users, ir_block_node_at(data, i));
    for (uint32_t u = 0; u < users.len; u++) {
      ir_node_id_t user = users.items[u];
      const ir_node_t *user_node = ir_node(graph, user);
      if (user_node->op.code == unboxed->check_op && opt_bitset_has(&candidates, user_node->inputs.items[0])) {
        pass_replace(&replacements, user, user_node->inputs.items[0]);
        opt_bitset_insert(&removed, user);
        continue;
      }
      if ((user_node->op.code == IR_OP_PHI && opt_bitset_has(&candidates, user)) || user_node->op.code == unboxed->box_op
        || user_node->op.code == IR_OP_STORE_SLOT)
        continue;
      for (uint32_t i = 0; i < ir_node(graph, user)->inputs.len; i++) {
        ir_node_id_t input = ir_node(graph, user)->inputs.items[i];
        if (!opt_bitset_has(&candidates, input)) continue;
        bool phi_user = user_node->op.code == IR_OP_PHI;
        uint64_t *known = phi_user ? opt_map_get1(&boxes, input) : opt_map_get2(&local_boxes, input, block);
        ir_node_id_t boxed;
        if (known) boxed = (ir_node_id_t)*known;
        else if (phi_user) {
          boxed = pass_box_phi(graph, unboxed, input);
          opt_map_set1(&boxes, input, boxed);
        } else if (user_node->op.code == IR_OP_REFINE) {
          ir_block_id_t predecessor = ir_block(graph, block)->predecessors.items[0];
          uint64_t *outer = opt_map_get2(&local_boxes, input, predecessor);
          if (outer) boxed = (ir_node_id_t)*outer;
          else {
            boxed = ir_add_node(graph, ir_op(unboxed->box_op), &input, 1, OPT_REPR_TAGGED, ir_node(graph, user)->bc_off);
            opt_vec_push(&graph->arena, &ir_block(graph, predecessor)->body, boxed);
            opt_map_set2(&local_boxes, input, predecessor, boxed);
          }
          opt_map_set2(&local_boxes, input, block, boxed);
        } else {
          boxed = ir_add_node(graph, ir_op(unboxed->box_op), &input, 1, OPT_REPR_TAGGED, ir_node(graph, user)->bc_off);
          ir_node_vec_t *body = &ir_block(graph, block)->body;
          int position = ir_node_vec_position(body, user);
          opt_vec_insert(&graph->arena, body, position < 0 ? body->len : (uint32_t)position, boxed);
          opt_map_set2(&local_boxes, input, block, boxed);
        }
        user_node = ir_node(graph, user);
        ir_node(graph, user)->inputs.items[i] = boxed;
      }
    }
  }
  pass_replacements_apply(&replacements, graph);
  pass_remove_nodes(graph, &removed);
}

void pass_representation(ir_graph_t *graph) {
  for (uint32_t i = 0; i < sizeof pass_unboxed / sizeof *pass_unboxed; i++) {
    opt_arena_t scratch;
    opt_arena_init(&scratch);
    pass_unbox_phis(graph, &pass_unboxed[i], &scratch);
    opt_arena_free(&scratch);
  }
}
