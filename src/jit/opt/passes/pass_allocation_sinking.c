#include "passes.h"
#include "../ir/ir_value.h"
#include "shapes.h"
#include <assert.h>

typedef struct {
  ir_graph_t *graph;
  opt_arena_t *arena;
  ir_node_id_t object;
  ir_node_id_t address;
  ir_cell_t shape;
  uint32_t count;
  uint32_t limit;
  ir_node_id_t properties[ANT_INOBJ_LARGE_SLOTS];
  uint32_t adds;
  ir_node_vec_t absorbed;
  ir_node_vec_t observed;
  OPT_VEC(ir_frame_state_id_t) observed_states;
  ir_node_vec_t epochs;
  ir_block_vec_t epoch_blocks;
  ir_node_id_t undefined;
} pass_sinking_t;

static bool pass_is_object(const pass_sinking_t *sink, ir_node_id_t value) {
  return value == sink->object || (sink->address != IR_NONE && value == sink->address);
}

static bool pass_uses_object(const pass_sinking_t *sink, const ir_node_t *data) {
  for (uint32_t i = 0; i < data->inputs.len; i++)
    if (pass_is_object(sink, data->inputs.items[i])) return true;
  return false;
}

static bool pass_state_refers_to(const pass_sinking_t *sink, ir_frame_state_id_t frame_state) {
  for (ir_frame_state_id_t current = frame_state; current != IR_NONE; current = ir_frame_state(sink->graph, current)->parent) {
    const ir_frame_state_t *state = ir_frame_state(sink->graph, current);
    for (uint32_t i = 0; i < state->values.len; i++)
      if (state->values.items[i].value == sink->object) return true;
  }
  return false;
}

static bool pass_virtual_objects_refer_to(const pass_sinking_t *sink, ir_frame_state_id_t frame_state) {
  ir_node_vec_t objects = {0};
  ir_virtual_objects(sink->graph, frame_state, sink->arena, &objects);
  for (uint32_t i = 0; i < objects.len; i++) {
    const ir_node_t *object = ir_node(sink->graph, objects.items[i]);
    for (uint32_t j = 0; j < object->inputs.len; j++)
      if (object->inputs.items[j] == sink->object) return true;
  }
  return false;
}

static ir_frame_state_id_t pass_virtualize(pass_sinking_t *sink, ir_frame_state_id_t frame_state) {
  ir_graph_t *graph = sink->graph;
  ir_op_t op = ir_op(IR_OP_VIRTUAL_OBJECT);
  op.shape = sink->shape;
  ir_node_id_t object = ir_add_node(graph, op, sink->properties, sink->count, OPT_REPR_TAGGED, ir_node(graph, sink->object)->bc_off);
  ir_node_vec_t chain = {0};
  ir_frame_state_chain(graph, frame_state, sink->arena, &chain);
  ir_frame_state_id_t parent = IR_NONE;
  for (uint32_t i = chain.len; i-- > 0;) {
    ir_frame_state_t copy = ir_frame_state_copy(graph, chain.items[i]);
    for (uint32_t j = 0; j < copy.values.len; j++)
      if (copy.values.items[j].value == sink->object) copy.values.items[j].value = object;
    copy.parent = parent;
    parent = ir_add_frame_state(graph, &copy);
  }
  return parent;
}

static bool pass_absorb_add(pass_sinking_t *sink, const ir_node_t *data) {
  const ir_op_t *op = &data->op;
  if (!pass_is_object(sink, data->inputs.items[0]) || pass_is_object(sink, data->inputs.items[1])) return false;
  for (uint32_t i = 2; i < data->inputs.len; i++)
    if (!pass_is_object(sink, data->inputs.items[i])) return false;
  uint32_t slot = opt_named_index(op->add_named.offset);
  const ant_shape_t *to = (const ant_shape_t *)(uintptr_t)op->add_named.shape;
  if (slot != sink->count || slot >= sink->limit || opt_named_inobj_limit(op->add_named.offset) != sink->limit
    || ant_shape_get_inobj_limit(to) != sink->limit || op->add_named.property_count != slot + 1) return false;
  sink->properties[slot] = data->inputs.items[1];
  sink->shape = op->add_named.shape;
  sink->count = slot + 1;
  sink->adds++;
  return true;
}

static bool pass_absorb_initialize(pass_sinking_t *sink, const ir_node_t *data) {
  if (data->inputs.items[0] != sink->object || pass_is_object(sink, data->inputs.items[1])) return false;
  uint32_t slot = opt_named_index(data->op.offset);
  if (slot >= sink->count || slot >= sink->limit) return false;
  sink->properties[slot] = data->inputs.items[1];
  return true;
}

static bool pass_passes_over(const pass_sinking_t *sink, const ir_node_t *data) {
  if (pass_uses_object(sink, data)) return false;
  ir_node_properties_t properties = ir_op_properties(&data->op);
  return !properties.is_call && !properties.runs_code && !properties.can_lazy_exit && !properties.allocates
    && properties.writes == IR_LOC_NONE;
}

static ir_block_id_t pass_straight_successor(const ir_graph_t *graph, ir_block_id_t block) {
  const ir_node_t *control = ir_control(graph, block);
  if (!control || control->op.code != IR_OP_JUMP) return IR_NONE;
  ir_block_id_t target = control->op.target;
  const ir_block_t *next = ir_block(graph, target);
  if (next->predecessors.len != 1 || next->phis.len || next->is_loop_header || ir_is_entry(graph, target)) return IR_NONE;
  return target;
}

static constexpr uint32_t PASS_REGION_BLOCKS = 32;
static constexpr uint32_t PASS_REGION_SPAN = 64;

static bool pass_region(const ir_graph_t *graph, ir_block_id_t entry, ir_block_id_t join, opt_arena_t *arena, ir_block_vec_t *blocks) {
  blocks->len = 0;
  ir_block_vec_t stack = {0};
  ir_successors_t successors = ir_block_successors(graph, entry);
  for (uint32_t i = 0; i < successors.len; i++) opt_vec_push(arena, &stack, ir_successor_at(&successors, i));
  bool reached = false;
  while (stack.len) {
    ir_block_id_t block = stack.items[--stack.len];
    if (block == join) {
      reached = true;
      continue;
    }
    if (block <= entry || ir_block(graph, block)->is_loop_header || ir_is_entry(graph, block)) return false;
    if (ir_block_vec_position(blocks, block) >= 0) continue;
    if (blocks->len == PASS_REGION_BLOCKS) return false;
    opt_vec_push(arena, blocks, block);
    ir_opcode_t code = ir_control(graph, block)->op.code;
    if (code == IR_OP_RETURN || code == IR_OP_THROW) return false;
    successors = ir_block_successors(graph, block);
    for (uint32_t i = 0; i < successors.len; i++) opt_vec_push(arena, &stack, ir_successor_at(&successors, i));
  }
  if (!reached) return false;
  for (uint32_t i = 0; i <= blocks->len; i++) {
    const ir_block_vec_t *predecessors = &ir_block(graph, i < blocks->len ? blocks->items[i] : join)->predecessors;
    for (uint32_t j = 0; j < predecessors->len; j++)
      if (predecessors->items[j] != entry && ir_block_vec_position(blocks, predecessors->items[j]) < 0) return false;
  }
  return true;
}

static bool pass_phis_avoid_object(const pass_sinking_t *sink, ir_block_id_t block) {
  const ir_block_t *data = ir_block(sink->graph, block);
  for (uint32_t i = 0; i < data->phis.len; i++)
    if (pass_uses_object(sink, ir_node(sink->graph, data->phis.items[i]))) return false;
  return true;
}

static bool pass_observe(pass_sinking_t *sink, ir_node_id_t node) {
  const ir_node_t *data = ir_node(sink->graph, node);
  if (data->op.code == IR_OP_EXIT ? pass_uses_object(sink, data) : !pass_passes_over(sink, data)) return false;
  if (data->frame_state != IR_NONE && pass_virtual_objects_refer_to(sink, data->frame_state)) return false;
  if (data->frame_state != IR_NONE && pass_state_refers_to(sink, data->frame_state)) {
    opt_vec_push(sink->arena, &sink->observed, node);
    opt_vec_push(sink->arena, &sink->observed_states, pass_virtualize(sink, data->frame_state));
  }
  return true;
}

static bool pass_region_avoids_object(pass_sinking_t *sink, ir_block_id_t entry, const ir_block_vec_t *blocks, ir_block_id_t join) {
  if (!pass_observe(sink, ir_control_id(sink->graph, entry)) || !pass_phis_avoid_object(sink, join)) return false;
  for (uint32_t i = 0; i < blocks->len; i++) {
    const ir_block_t *data = ir_block(sink->graph, blocks->items[i]);
    if (!pass_phis_avoid_object(sink, blocks->items[i])) return false;
    for (uint32_t n = 0; n < data->body.len; n++)
      if (!pass_observe(sink, data->body.items[n])) return false;
    if (!pass_observe(sink, data->control)) return false;
  }
  return true;
}

static ir_block_id_t pass_cross_region(pass_sinking_t *sink, ir_block_id_t entry) {
  const ir_graph_t *graph = sink->graph;
  if (ir_block_successors(graph, entry).len < 2) return IR_NONE;
  ir_block_vec_t blocks = {0};
  uint32_t end = ir_block_count(graph) < entry + PASS_REGION_SPAN ? ir_block_count(graph) : entry + PASS_REGION_SPAN;
  for (ir_block_id_t join = entry + 1; join < end; join++) {
    if (ir_block(graph, join)->is_loop_header || ir_is_entry(graph, join) || !pass_region(graph, entry, join, sink->arena, &blocks)) continue;
    uint32_t observed = sink->observed.len;
    if (pass_region_avoids_object(sink, entry, &blocks, join)) return join;
    sink->observed.len = sink->observed_states.len = observed;
    return IR_NONE;
  }
  return IR_NONE;
}

static void pass_materialize(pass_sinking_t *sink, ir_block_id_t block, uint32_t position, pass_replacements_t *replacements, opt_bitset_t *removed) {
  ir_graph_t *graph = sink->graph;
  const ir_node_t *original = ir_node(graph, sink->object);
  ir_op_t op = original->op;
  op.allocate_object.shape = sink->shape;
  op.allocate_object.property_count = sink->count;
  uint32_t bc_off = original->bc_off;
  ir_node_id_t proto = original->inputs.items[0];
  ir_node_id_t object = ir_add_node(graph, op, &proto, 1, OPT_REPR_TAGGED, bc_off);
  ir_node_id_t address = ir_add_node(graph, ir_op(IR_OP_CELL_ADDRESS), &object, 1, OPT_REPR_POINTER, bc_off);
  ir_node_vec_t inserted = {0};
  opt_vec_push(sink->arena, &inserted, object);
  opt_vec_push(sink->arena, &inserted, address);
  for (uint32_t slot = 0; slot < sink->count; slot++) {
    if (sink->properties[slot] == sink->undefined) continue;
    ir_op_t init = ir_op(IR_OP_INITIALIZE_NAMED);
    init.offset = opt_named_offset(slot, (uint8_t)sink->limit);
    ir_node_id_t node = ir_add_node(graph, init, (ir_node_id_t[]){object, sink->properties[slot], address}, 3, OPT_REPR_NONE, bc_off);
    opt_vec_push(sink->arena, &inserted, node);
  }
  if (sink->address != IR_NONE) pass_replace(replacements, sink->address, address);
  ir_node_vec_t *body = &ir_block(graph, block)->body;
  uint32_t tail = body->len - position;
  opt_vec_reserve(&graph->arena, body, body->len + inserted.len);
  memmove(&body->items[position + inserted.len], &body->items[position], tail * sizeof *body->items);
  memcpy(&body->items[position], inserted.items, inserted.len * sizeof *inserted.items);
  body->len += inserted.len;
  pass_replace(replacements, sink->object, object);
  opt_bitset_insert(removed, sink->object);
  for (uint32_t i = 0; i < sink->absorbed.len; i++) opt_bitset_insert(removed, sink->absorbed.items[i]);
}

static void pass_group_epochs(pass_sinking_t *sink) {
  if (sink->epochs.len < 2) return;
  ir_graph_t *graph = sink->graph;
  ir_node_id_t first = sink->epochs.items[0];
  ir_frame_state_id_t frame_state = ir_node(graph, first)->frame_state;
  for (uint32_t i = 1; i < sink->epochs.len; i++) {
    ir_node_vec_t *body = &ir_block(graph, sink->epoch_blocks.items[i])->body;
    int at = ir_node_vec_position(body, sink->epochs.items[i]);
    assert(at >= 0);
    memmove(&body->items[at], &body->items[at + 1], (body->len - (uint32_t)at - 1) * sizeof *body->items);
    body->len--;
  }
  ir_node_vec_t *body = &ir_block(graph, sink->epoch_blocks.items[0])->body;
  uint32_t at = (uint32_t)ir_node_vec_position(body, first) + 1, moved = sink->epochs.len - 1;
  opt_vec_reserve(&graph->arena, body, body->len + moved);
  memmove(&body->items[at + moved], &body->items[at], (body->len - at) * sizeof *body->items);
  for (uint32_t i = 0; i < moved; i++) {
    ir_node_id_t node = sink->epochs.items[i + 1];
    body->items[at + i] = node;
    ir_node(graph, node)->frame_state = frame_state;
  }
  body->len += moved;
}

static bool pass_sink(pass_sinking_t *sink, ir_block_id_t block, uint32_t index, pass_replacements_t *replacements, opt_bitset_t *removed) {
  ir_graph_t *graph = sink->graph;
  uint32_t position = index + 1;
  for (;;) {
    const ir_block_t *data = ir_block(graph, block);
    if (position == data->body.len) {
      ir_block_id_t next = pass_straight_successor(graph, block);
      if (next == IR_NONE) next = pass_cross_region(sink, block);
      if (next == IR_NONE) break;
      block = next;
      position = 0;
      continue;
    }
    ir_node_id_t node = data->body.items[position];
    const ir_node_t *node_data = ir_node(graph, node);
    bool absorbed = false;
    switch (node_data->op.code) {
      case IR_OP_ADD_NAMED: absorbed = pass_absorb_add(sink, node_data); break;
      case IR_OP_INITIALIZE_NAMED: absorbed = pass_absorb_initialize(sink, node_data); break;
      case IR_OP_OBJECT_ADDRESS: absorbed = sink->address == IR_NONE && node_data->inputs.items[0] == sink->object; break;
      default: break;
    }
    if (absorbed) {
      if (node_data->op.code == IR_OP_OBJECT_ADDRESS) sink->address = node;
      opt_vec_push(sink->arena, &sink->absorbed, node);
      position++;
      continue;
    }
    if (!pass_observe(sink, node)) break;
    if (node_data->op.code == IR_OP_CHECK_ADD_EPOCH) {
      opt_vec_push(sink->arena, &sink->epochs, node);
      opt_vec_push(sink->arena, &sink->epoch_blocks, block);
    }
    position++;
  }
  if (!sink->adds) return false;
  for (uint32_t i = 0; i < sink->observed.len; i++) ir_node(graph, sink->observed.items[i])->frame_state = sink->observed_states.items[i];
  pass_materialize(sink, block, position, replacements, removed);
  pass_group_epochs(sink);
  return true;
}

void pass_allocation_sinking(ir_graph_t *graph) {
  opt_arena_t scratch;
  opt_arena_init(&scratch);
  pass_constants_t constants;
  pass_constants_init(&constants, graph, &scratch);
  ir_node_id_t undefined = pass_constant(&constants, graph, IR_VALUE_UNDEFINED);
  pass_replacements_t replacements;
  pass_replacements_init(&replacements, graph, &scratch);
  opt_bitset_t removed = opt_bitset_new(&scratch, ir_node_count(graph));
  bool changed = false;
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    for (uint32_t i = 0; i < ir_block(graph, block)->body.len; i++) {
      ir_node_id_t node = ir_block(graph, block)->body.items[i];
      const ir_node_t *data = ir_node(graph, node);
      if (data->op.code != IR_OP_ALLOCATE_OBJECT || data->inputs.len != 1 || opt_bitset_has(&removed, node)) continue;
      const ant_shape_t *shape = (const ant_shape_t *)(uintptr_t)data->op.allocate_object.shape;
      uint32_t limit = ant_shape_get_inobj_limit(shape);
      if (limit > ANT_INOBJ_LARGE_SLOTS || data->op.allocate_object.property_count > limit) continue;
      pass_sinking_t sink = {
        .graph = graph, .arena = &scratch, .object = node, .address = IR_NONE,
        .shape = data->op.allocate_object.shape, .count = data->op.allocate_object.property_count,
        .limit = limit, .undefined = undefined,
      };
      for (uint32_t slot = 0; slot < limit; slot++) sink.properties[slot] = undefined;
      changed |= pass_sink(&sink, block, i, &replacements, &removed);
    }
  }
  if (changed) {
    pass_replacements_apply(&replacements, graph);
    pass_remove_nodes(graph, &removed);
  }
  opt_arena_free(&scratch);
}
