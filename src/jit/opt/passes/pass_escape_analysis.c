#include "passes.h"
#include "../ir/ir_value.h"
#include <assert.h>
#include <stdlib.h>

typedef struct {
  ir_node_id_t object;
  ir_cell_t shape;
  uint32_t count;
} pass_object_shape_t;

typedef OPT_VEC(pass_object_shape_t) pass_object_shapes_t;

typedef struct {
  ir_node_id_t object;
  uint32_t offset;
  ir_node_id_t value;
} pass_property_t;

typedef OPT_VEC(pass_property_t) pass_properties_t;

typedef struct {
  ir_node_id_t node;
  ir_node_id_t root;
  ir_cell_t shape;
  uint32_t count;
} pass_shape_use_t;

typedef OPT_VEC(pass_shape_use_t) pass_shape_uses_t;

typedef struct {
  ir_node_id_t value;
  ir_node_id_t target;
  uint32_t offset;
} pass_store_t;

typedef struct {
  ir_node_vec_t candidates;
  ir_node_id_t *roots;
  uint32_t root_count;
  opt_bitset_t escaped;
  bool has_nested_objects;
} pass_analysis_t;

static inline ir_node_id_t pass_root(const pass_analysis_t *analysis, ir_node_id_t value) {
  return value < analysis->root_count ? analysis->roots[value] : IR_NONE;
}

static pass_object_shape_t *pass_shape_get(const pass_object_shapes_t *shapes, ir_node_id_t object) {
  uint32_t low = 0, high = shapes->len;
  while (low < high) {
    uint32_t mid = low + (high - low) / 2;
    if (shapes->items[mid].object == object) return &shapes->items[mid];
    if (shapes->items[mid].object < object) low = mid + 1;
    else high = mid;
  }
  return NULL;
}

static void pass_shape_set(opt_arena_t *arena, pass_object_shapes_t *shapes, ir_node_id_t object, ir_cell_t shape, uint32_t count) {
  uint32_t low = 0, high = shapes->len;
  while (low < high) {
    uint32_t mid = low + (high - low) / 2;
    if (shapes->items[mid].object == object) {
      shapes->items[mid].shape = shape;
      shapes->items[mid].count = count;
      return;
    }
    if (shapes->items[mid].object < object) low = mid + 1;
    else high = mid;
  }
  pass_object_shape_t entry = {.object = object, .shape = shape, .count = count};
  opt_vec_insert(arena, shapes, low, entry);
}

static int pass_property_compare(ir_node_id_t object, uint32_t offset, const pass_property_t *property) {
  if (object != property->object) return object < property->object ? -1 : 1;
  if (offset != property->offset) return offset < property->offset ? -1 : 1;
  return 0;
}

static pass_property_t *pass_property_get(const pass_properties_t *properties, ir_node_id_t object, uint32_t offset) {
  uint32_t low = 0, high = properties->len;
  while (low < high) {
    uint32_t mid = low + (high - low) / 2;
    int order = pass_property_compare(object, offset, &properties->items[mid]);
    if (!order) return &properties->items[mid];
    if (order > 0) low = mid + 1;
    else high = mid;
  }
  return NULL;
}

static void pass_property_set(opt_arena_t *arena, pass_properties_t *properties, ir_node_id_t object, uint32_t offset, ir_node_id_t value) {
  uint32_t low = 0, high = properties->len;
  while (low < high) {
    uint32_t mid = low + (high - low) / 2;
    int order = pass_property_compare(object, offset, &properties->items[mid]);
    if (!order) {
      properties->items[mid].value = value;
      return;
    }
    if (order > 0) low = mid + 1;
    else high = mid;
  }
  pass_property_t entry = {.object = object, .offset = offset, .value = value};
  opt_vec_insert(arena, properties, low, entry);
}

static void pass_merge_shapes(opt_arena_t *arena, const pass_object_shapes_t *const *exits, uint32_t count, opt_bitset_t *escaped, pass_object_shapes_t *merged) {
  merged->items = NULL;
  merged->len = merged->cap = 0;
  if (!count) return;
  opt_vec_copy(arena, merged, exits[0]);
  for (uint32_t e = 1; e < count; e++) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < merged->len; i++) {
      const pass_object_shape_t *other = pass_shape_get(exits[e], merged->items[i].object);
      if (!other) continue;
      if (other->shape != merged->items[i].shape || other->count != merged->items[i].count) {
        opt_bitset_insert(escaped, merged->items[i].object);
        continue;
      }
      merged->items[kept++] = merged->items[i];
    }
    merged->len = kept;
  }
}

static void pass_shapes_before_uses(const ir_graph_t *graph, const pass_analysis_t *analysis, const opt_bitset_t *candidates, opt_bitset_t *escaped, opt_arena_t *arena, opt_map_t *before, pass_shape_uses_t *uses) {
  uint32_t block_count = ir_block_count(graph);
  pass_object_shapes_t *entries = opt_arena_new(arena, pass_object_shapes_t, block_count + 1);
  pass_object_shapes_t *exits = opt_arena_new(arena, pass_object_shapes_t, block_count + 1);
  bool *has_exit = opt_arena_new(arena, bool, block_count + 1);
  OPT_VEC(const pass_object_shapes_t *) forward = {0};
  for (ir_block_id_t index = 0; index < block_count; index++) {
    const ir_block_t *block = ir_block(graph, index);
    forward.len = 0;
    for (uint32_t i = 0; i < block->predecessors.len; i++) {
      ir_block_id_t predecessor = block->predecessors.items[i];
      if (predecessor < index && has_exit[predecessor]) opt_vec_push(arena, &forward, &exits[predecessor]);
    }
    pass_object_shapes_t shapes;
    pass_merge_shapes(arena, forward.items, forward.len, escaped, &shapes);
    opt_vec_clone(arena, &entries[index], &shapes);
    for (uint32_t n = 0, count = ir_block_node_count(block); n < count; n++) {
      ir_node_id_t node = ir_block_node_at(block, n);
      const ir_node_t *data = ir_node(graph, node);
      for (uint32_t i = 0; i < data->inputs.len; i++) {
        ir_node_id_t root = pass_root(analysis, data->inputs.items[i]);
        if (root == IR_NONE) continue;
        const pass_object_shape_t *shape = pass_shape_get(&shapes, root);
        if (!shape) continue;
        pass_shape_use_t use = {.node = node, .root = root, .shape = shape->shape, .count = shape->count};
        bool inserted;
        uint64_t *slot = opt_map_entry(before, (uint64_t[]){node, root}, &inserted);
        if (inserted) {
          *slot = uses->len;
          opt_vec_push(arena, uses, use);
        } else {
          uses->items[*slot] = use;
        }
      }
      if (data->op.code == IR_OP_ALLOCATE_OBJECT && opt_bitset_has(candidates, node)) {
        pass_shape_set(arena, &shapes, node, data->op.allocate_object.shape, data->op.allocate_object.property_count);
      } else if (data->op.code == IR_OP_ADD_NAMED) {
        ir_node_id_t root = pass_root(analysis, data->inputs.items[0]);
        if (root != IR_NONE) pass_shape_set(arena, &shapes, root, data->op.add_named.shape, data->op.add_named.property_count);
      }
    }
    exits[index] = shapes;
    has_exit[index] = true;
  }
  for (ir_block_id_t index = 0; index < block_count; index++) {
    const ir_block_t *block = ir_block(graph, index);
    for (uint32_t i = 0; i < block->predecessors.len; i++) {
      ir_block_id_t predecessor = block->predecessors.items[i];
      if (predecessor < index || !has_exit[predecessor]) continue;
      for (uint32_t j = 0; j < entries[index].len; j++) {
        const pass_object_shape_t *entry = &entries[index].items[j];
        const pass_object_shape_t *other = pass_shape_get(&exits[predecessor], entry->object);
        if (other && (other->shape != entry->shape || other->count != entry->count)) opt_bitset_insert(escaped, entry->object);
      }
    }
  }
}

static void pass_analyze(const ir_graph_t *graph, const ir_node_vec_t *placed, const ir_node_vec_t *escaping, opt_arena_t *arena, pass_analysis_t *analysis) {
  uint32_t count = ir_node_count(graph);
  *analysis = (pass_analysis_t){
    .roots = opt_arena_alloc(arena, (count + 1) * sizeof(ir_node_id_t)),
    .root_count = count,
    .escaped = opt_bitset_new(arena, count),
  };
  for (uint32_t i = 0; i < count; i++) analysis->roots[i] = IR_NONE;
  opt_bitset_t candidates = opt_bitset_new(arena, count);
  for (uint32_t i = 0; i < placed->len; i++) {
    ir_node_id_t node = placed->items[i];
    if (ir_node(graph, node)->op.code != IR_OP_ALLOCATE_OBJECT) continue;
    opt_vec_push(arena, &analysis->candidates, node);
    opt_bitset_insert(&candidates, node);
    analysis->roots[node] = node;
  }
  for (uint32_t i = 0; i < escaping->len; i++) opt_bitset_insert(&analysis->escaped, escaping->items[i]);
  if (!analysis->candidates.len) return;

  bool changed = true;
  while (changed) {
    changed = false;
    for (uint32_t i = 0; i < placed->len; i++) {
      ir_node_id_t node = placed->items[i];
      const ir_node_t *data = ir_node(graph, node);
      bool stands_for_input = data->op.code == IR_OP_EMPTY_TO_UNDEFINED || data->op.code == IR_OP_CELL_ADDRESS
        || (ir_op_refined_input(&data->op) == 0 && data->repr != OPT_REPR_NONE);
      if (!stands_for_input || analysis->roots[node] != IR_NONE) continue;
      ir_node_id_t root = pass_root(analysis, data->inputs.items[0]);
      if (root == IR_NONE) continue;
      analysis->roots[node] = root;
      changed = true;
    }
  }

  opt_map_t before;
  opt_map_init(&before, arena, 2);
  pass_shape_uses_t uses = {0};
  pass_shapes_before_uses(graph, analysis, &candidates, &analysis->escaped, arena, &before, &uses);

  OPT_VEC(pass_store_t) stored_into = {0};
  for (uint32_t p = 0; p < placed->len; p++) {
    ir_node_id_t node = placed->items[p];
    const ir_node_t *data = ir_node(graph, node);
    for (uint32_t index = 0; index < data->inputs.len; index++) {
      ir_node_id_t root = pass_root(analysis, data->inputs.items[index]);
      if (root == IR_NONE) continue;
      uint64_t *use_index = opt_map_get2(&before, node, root);
      if (!use_index) {
        opt_bitset_insert(&analysis->escaped, root);
        continue;
      }
      const pass_shape_use_t *use = &uses.items[*use_index];
      bool accesses_object = false;
      const ir_op_t *op = &data->op;
      switch (op->code) {
        case IR_OP_EMPTY_TO_UNDEFINED:
        case IR_OP_CELL_ADDRESS:
        case IR_OP_CHECK_OBJECT: accesses_object = true; break;
        case IR_OP_LOAD_NAMED: accesses_object = op->offset < use->count; break;
        case IR_OP_CHECK_SHAPE:
          for (uint32_t i = 0; i < op->shapes.len && !accesses_object; i++)
            accesses_object = op->shapes.items[i].shape == use->shape && !op->shapes.items[i].has_generation;
          break;
        case IR_OP_INITIALIZE_NAMED:
        case IR_OP_STORE_NAMED:
        case IR_OP_ADD_NAMED: {
          uint32_t offset = op->code == IR_OP_ADD_NAMED ? op->add_named.offset : op->offset;
          if (index != 1) {
            accesses_object = op->code == IR_OP_ADD_NAMED ? offset == use->count : offset < use->count;
            break;
          }
          pass_store_t store = {.value = root, .target = pass_root(analysis, data->inputs.items[0]), .offset = offset};
          opt_vec_push(arena, &stored_into, store);
          accesses_object = true;
          break;
        }
        default: break;
      }
      if (!accesses_object) opt_bitset_insert(&analysis->escaped, root);
    }
  }
  OPT_VEC(pass_store_t) loaded = {0};
  for (uint32_t p = 0; p < placed->len; p++) {
    const ir_node_t *data = ir_node(graph, placed->items[p]);
    if (data->op.code != IR_OP_LOAD_NAMED) continue;
    ir_node_id_t root = pass_root(analysis, data->inputs.items[0]);
    if (root == IR_NONE) continue;
    pass_store_t load = {.target = root, .offset = data->op.offset};
    opt_vec_push(arena, &loaded, load);
  }
  changed = true;
  while (changed) {
    changed = false;
    for (uint32_t i = 0; i < stored_into.len; i++) {
      const pass_store_t *store = &stored_into.items[i];
      bool target_escapes = store->target == IR_NONE || opt_bitset_contains(&analysis->escaped, store->target);
      for (uint32_t j = 0; j < loaded.len && !target_escapes; j++)
        target_escapes = loaded.items[j].target == store->target && loaded.items[j].offset == store->offset;
      if (target_escapes && !opt_bitset_contains(&analysis->escaped, store->value)) {
        opt_bitset_insert(&analysis->escaped, store->value);
        changed = true;
      }
    }
  }
  for (uint32_t i = 0; i < stored_into.len; i++) {
    const pass_store_t *store = &stored_into.items[i];
    if (store->target != IR_NONE && !opt_bitset_contains(&analysis->escaped, store->value)) analysis->has_nested_objects = true;
  }
}

typedef struct {
  ir_node_id_t root;
  ir_node_id_t object;
} pass_virtual_t;

typedef OPT_VEC(pass_virtual_t) pass_virtuals_t;

typedef struct {
  ir_graph_t *graph;
  const pass_analysis_t *analysis;
  const pass_replacements_t *replacements;
  const pass_properties_t *properties;
  const pass_object_shapes_t *shapes;
  opt_arena_t *arena;
} pass_virtualizer_t;

static bool pass_is_replaced(const pass_analysis_t *analysis, ir_node_id_t value) {
  ir_node_id_t root = pass_root(analysis, value);
  return root != IR_NONE && !opt_bitset_contains(&analysis->escaped, root);
}

static ir_node_id_t pass_virtual_object(pass_virtualizer_t *virtualizer, pass_virtuals_t *objects, ir_node_id_t value) {
  value = pass_resolve(virtualizer->replacements, value);
  if (!pass_is_replaced(virtualizer->analysis, value)) return value;
  ir_node_id_t root = pass_root(virtualizer->analysis, value);
  for (uint32_t i = 0; i < objects->len; i++)
    if (objects->items[i].root == root) return objects->items[i].object;
  const pass_object_shape_t *shape = pass_shape_get(virtualizer->shapes, root);
  assert(shape);
  ir_op_t op = ir_op(IR_OP_VIRTUAL_OBJECT);
  op.shape = shape->shape;
  ir_node_id_t object = ir_add_node(virtualizer->graph, op, NULL, 0, OPT_REPR_TAGGED, ir_node(virtualizer->graph, root)->bc_off);
  pass_virtual_t entry = {.root = root, .object = object};
  opt_vec_push(virtualizer->arena, objects, entry);
  return object;
}

static ir_frame_state_id_t pass_virtualize_frame_state(pass_virtualizer_t *virtualizer, ir_frame_state_id_t frame_state) {
  ir_graph_t *graph = virtualizer->graph;
  opt_arena_t *arena = virtualizer->arena;
  ir_node_vec_t chain = {0};
  ir_frame_state_chain(graph, frame_state, arena, &chain);
  bool refers_to_replaced = false;
  for (uint32_t i = 0; i < chain.len && !refers_to_replaced; i++) {
    const ir_frame_state_t *state = ir_frame_state(graph, chain.items[i]);
    for (uint32_t j = 0; j < state->values.len && !refers_to_replaced; j++)
      refers_to_replaced = pass_is_replaced(virtualizer->analysis, pass_resolve(virtualizer->replacements, state->values.items[j].value));
  }
  if (!refers_to_replaced) return IR_NONE;
  pass_virtuals_t objects = {0};
  ir_frame_state_id_t parent = IR_NONE;
  for (uint32_t i = chain.len; i-- > 0;) {
    const ir_frame_state_t *original = ir_frame_state(graph, chain.items[i]);
    ir_frame_state_t copy = *original;
    opt_vec_clone(arena, &copy.values, &original->values);
    for (uint32_t j = 0; j < copy.values.len; j++)
      copy.values.items[j].value = pass_virtual_object(virtualizer, &objects, copy.values.items[j].value);
    copy.parent = parent;
    parent = ir_add_frame_state(graph, &copy);
  }
  ir_node_vec_t inputs = {0};
  for (uint32_t filled = 0; filled < objects.len; filled++) {
    ir_node_id_t root = objects.items[filled].root;
    ir_node_id_t object = objects.items[filled].object;
    const pass_object_shape_t *shape = pass_shape_get(virtualizer->shapes, root);
    assert(shape);
    uint32_t property_count = shape->count;
    inputs.len = 0;
    for (uint32_t offset = 0; offset < property_count; offset++) {
      const pass_property_t *property = pass_property_get(virtualizer->properties, root, offset);
      assert(property);
      opt_vec_push(arena, &inputs, pass_virtual_object(virtualizer, &objects, property->value));
    }
    ir_set_inputs(graph, object, inputs.items, inputs.len);
  }
  return parent;
}

typedef struct {
  ir_node_id_t phi;
  ir_block_id_t block;
  ir_node_id_t object;
  uint32_t offset;
} pass_pending_phi_t;

static int pass_compare_nodes(const void *a, const void *b) {
  ir_node_id_t x = *(const ir_node_id_t *)a, y = *(const ir_node_id_t *)b;
  return (x > y) - (x < y);
}

static bool pass_replace_objects(ir_graph_t *graph, const pass_analysis_t *analysis, opt_arena_t *arena, ir_node_vec_t *conflicts) {
  bool any_replaced = false;
  for (uint32_t i = 0; i < analysis->candidates.len && !any_replaced; i++)
    any_replaced = !opt_bitset_contains(&analysis->escaped, analysis->candidates.items[i]);
  if (!any_replaced) return true;

  pass_constants_t constants;
  pass_constants_init(&constants, graph, arena);
  ir_node_id_t undefined = pass_constant(&constants, graph, IR_VALUE_UNDEFINED);
  pass_replacements_t replacements;
  pass_replacements_init(&replacements, graph, arena);
  opt_bitset_t removed = opt_bitset_new(arena, ir_node_count(graph));
  uint32_t block_count = ir_block_count(graph);
  pass_properties_t *exit_properties = opt_arena_new(arena, pass_properties_t, block_count + 1);
  pass_object_shapes_t *exit_shapes = opt_arena_new(arena, pass_object_shapes_t, block_count + 1);
  bool *has_exit = opt_arena_new(arena, bool, block_count + 1);
  OPT_VEC(pass_pending_phi_t) pending_phis = {0};
  conflicts->len = 0;

  ir_node_vec_t values = {0}, nodes = {0};
  for (ir_block_id_t block = 0; block < block_count; block++) {
    ir_block_vec_t predecessors = {0};
    opt_vec_clone(arena, &predecessors, &ir_block(graph, block)->predecessors);
    ir_block_id_t first = IR_NONE;
    uint32_t forward_count = 0;
    for (uint32_t i = 0; i < predecessors.len; i++) {
      if (predecessors.items[i] >= block) continue;
      if (first == IR_NONE) first = predecessors.items[i];
      forward_count++;
    }
    pass_properties_t properties = {0};
    pass_object_shapes_t shapes = {0};
    if (first != IR_NONE && has_exit[first]) opt_vec_copy(arena, &shapes, &exit_shapes[first]);
    if (first != IR_NONE) {
      bool has_back_edges = forward_count < predecessors.len;
      pass_properties_t first_properties = {0};
      if (has_exit[first]) opt_vec_copy(arena, &first_properties, &exit_properties[first]);
      for (uint32_t k = 0; k < first_properties.len; k++) {
        pass_property_t key = first_properties.items[k];
        values.len = 0;
        bool defined = true;
        for (uint32_t i = 0; i < predecessors.len && defined; i++) {
          ir_block_id_t predecessor = predecessors.items[i];
          if (predecessor >= block) {
            opt_vec_push(arena, &values, key.value);
            continue;
          }
          const pass_property_t *property = has_exit[predecessor]
            ? pass_property_get(&exit_properties[predecessor], key.object, key.offset) : NULL;
          if (!property) defined = false;
          else opt_vec_push(arena, &values, property->value);
        }
        if (!defined) continue;
        bool same = true;
        for (uint32_t i = 0; i < values.len && same; i++) same = values.items[i] == key.value;
        if (!has_back_edges && same) {
          pass_property_set(arena, &properties, key.object, key.offset, key.value);
          continue;
        }
        for (uint32_t i = 0; i < values.len; i++) {
          ir_node_id_t value = pass_resolve(&replacements, values.items[i]);
          if (pass_is_replaced(analysis, value)) opt_vec_push(arena, conflicts, pass_root(analysis, value));
        }
        ir_node_id_t phi = ir_add_node(graph, ir_op(IR_OP_PHI), values.items, values.len, OPT_REPR_TAGGED, 0);
        opt_vec_push(&graph->arena, &ir_block(graph, block)->phis, phi);
        if (has_back_edges) {
          pass_pending_phi_t pending = {.phi = phi, .block = block, .object = key.object, .offset = key.offset};
          opt_vec_push(arena, &pending_phis, pending);
        }
        pass_property_set(arena, &properties, key.object, key.offset, phi);
      }
    }

    const ir_block_t *data = ir_block(graph, block);
    nodes.len = 0;
    for (uint32_t i = 0; i < data->body.len; i++) opt_vec_push(arena, &nodes, data->body.items[i]);
    if (data->control != IR_NONE) opt_vec_push(arena, &nodes, data->control);
    for (uint32_t n = 0; n < nodes.len; n++) {
      ir_node_id_t node = nodes.items[n];
      ir_frame_state_id_t frame_state = ir_node(graph, node)->frame_state;
      if (frame_state != IR_NONE) {
        pass_virtualizer_t virtualizer = {
          .graph = graph, .analysis = analysis, .replacements = &replacements,
          .properties = &properties, .shapes = &shapes, .arena = arena,
        };
        ir_frame_state_id_t virtualized = pass_virtualize_frame_state(&virtualizer, frame_state);
        if (virtualized != IR_NONE) ir_node(graph, node)->frame_state = virtualized;
      }
      const ir_node_t *node_data = ir_node(graph, node);
      ir_op_t op = node_data->op;
      ir_node_id_t object = node_data->inputs.len && pass_is_replaced(analysis, node_data->inputs.items[0])
        ? node_data->inputs.items[0] : IR_NONE;
      ir_node_id_t root = object != IR_NONE ? pass_root(analysis, object) : IR_NONE;
      switch (op.code) {
        case IR_OP_ALLOCATE_OBJECT:
          if (!pass_is_replaced(analysis, node)) break;
          for (uint32_t offset = 0; offset < op.allocate_object.property_count; offset++)
            pass_property_set(arena, &properties, node, offset, undefined);
          pass_shape_set(arena, &shapes, node, op.allocate_object.shape, op.allocate_object.property_count);
          opt_bitset_insert(&removed, node);
          break;
        case IR_OP_ADD_NAMED:
          if (object == IR_NONE) break;
          pass_property_set(arena, &properties, root, op.add_named.offset, pass_resolve(&replacements, node_data->inputs.items[1]));
          pass_shape_set(arena, &shapes, root, op.add_named.shape, op.add_named.property_count);
          opt_bitset_insert(&removed, node);
          break;
        case IR_OP_INITIALIZE_NAMED:
        case IR_OP_STORE_NAMED:
          if (object == IR_NONE) break;
          pass_property_set(arena, &properties, root, op.offset, pass_resolve(&replacements, node_data->inputs.items[1]));
          opt_bitset_insert(&removed, node);
          break;
        case IR_OP_LOAD_NAMED: {
          if (object == IR_NONE) break;
          const pass_property_t *property = pass_property_get(&properties, root, op.offset);
          pass_replace(&replacements, node, property ? property->value : undefined);
          opt_bitset_insert(&removed, node);
          break;
        }
        case IR_OP_CHECK_SHAPE:
        case IR_OP_CHECK_OBJECT:
        case IR_OP_EMPTY_TO_UNDEFINED:
        case IR_OP_CELL_ADDRESS:
          if (object == IR_NONE) break;
          if (ir_op_refined_input(&op) >= 0 && node_data->repr != OPT_REPR_NONE)
            pass_replace(&replacements, node, node_data->inputs.items[0]);
          opt_bitset_insert(&removed, node);
          break;
        default: break;
      }
    }
    exit_properties[block] = properties;
    exit_shapes[block] = shapes;
    has_exit[block] = true;
  }

  for (uint32_t p = 0; p < pending_phis.len; p++) {
    pass_pending_phi_t pending = pending_phis.items[p];
    const ir_block_vec_t *predecessors = &ir_block(graph, pending.block)->predecessors;
    for (uint32_t index = 0; index < predecessors->len; index++) {
      ir_block_id_t predecessor = predecessors->items[index];
      if (predecessor < pending.block) continue;
      const pass_property_t *property = has_exit[predecessor]
        ? pass_property_get(&exit_properties[predecessor], pending.object, pending.offset) : NULL;
      ir_node_id_t value = pass_resolve(&replacements, property ? property->value : pending.phi);
      if (pass_is_replaced(analysis, value)) opt_vec_push(arena, conflicts, pass_root(analysis, value));
      ir_node(graph, pending.phi)->inputs.items[index] = value;
    }
  }

  if (conflicts->len) {
    qsort(conflicts->items, conflicts->len, sizeof *conflicts->items, pass_compare_nodes);
    uint32_t unique = 0;
    for (uint32_t i = 0; i < conflicts->len; i++)
      if (!unique || conflicts->items[unique - 1] != conflicts->items[i]) conflicts->items[unique++] = conflicts->items[i];
    conflicts->len = unique;
    return false;
  }
  pass_replacements_apply(&replacements, graph);
  pass_remove_nodes(graph, &removed);
  pass_remove_trivial_phis(graph);
  return true;
}

void pass_escape_analysis(ir_graph_t *graph) {
  opt_arena_t scratch;
  opt_arena_init(&scratch);
  ir_node_vec_t placed = {0}, escaping = {0}, conflicts = {0};
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    const ir_block_t *data = ir_block(graph, block);
    for (uint32_t i = 0, count = ir_block_node_count(data); i < count; i++) opt_vec_push(&scratch, &placed, ir_block_node_at(data, i));
  }
  for (;;) {
    pass_analysis_t analysis;
    pass_analyze(graph, &placed, &escaping, &scratch, &analysis);
    if (!analysis.has_nested_objects) {
      [[maybe_unused]] bool replaced = pass_replace_objects(graph, &analysis, &scratch, &conflicts);
      assert(replaced && "objects not stored into others merge in no phi of properties");
      break;
    }
    ir_graph_t copy;
    ir_graph_clone(&copy, graph);
    if (pass_replace_objects(&copy, &analysis, &scratch, &conflicts)) {
      ir_graph_free(graph);
      *graph = copy;
      break;
    }
    ir_graph_free(&copy);
    for (uint32_t i = 0; i < conflicts.len; i++) opt_vec_push(&scratch, &escaping, conflicts.items[i]);
  }
  opt_arena_free(&scratch);
}
