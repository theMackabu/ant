#include "passes.h"
#include <stdlib.h>

typedef enum : uint8_t {
  PASS_HOLDS_NO,
  PASS_HOLDS_YES,
  PASS_HOLDS_MAYBE,
} pass_holds_t;

static inline pass_holds_t pass_holds_merge(pass_holds_t a, pass_holds_t b) {
  return a == b ? a : PASS_HOLDS_MAYBE;
}

typedef struct {
  pass_holds_t published;
  pass_holds_t initialized;
} pass_frame_state_t;

static inline pass_frame_state_t pass_frame_merge(pass_frame_state_t a, pass_frame_state_t b) {
  return (pass_frame_state_t){pass_holds_merge(a.published, b.published), pass_holds_merge(a.initialized, b.initialized)};
}

typedef enum {
  PASS_NEED_NOTHING,
  PASS_NEED_PUBLISHED,
  PASS_NEED_INITIALIZED,
  PASS_NEED_INITIALIZED_FOR_SLOW_PATH,
} pass_need_t;

static constexpr pass_frame_state_t PASS_FRAME_INITIALIZED = {PASS_HOLDS_YES, PASS_HOLDS_YES};

static ir_opcode_t pass_satisfy(pass_frame_state_t state, pass_need_t need, pass_frame_state_t *after) {
  *after = state;
  if (need == PASS_NEED_INITIALIZED && state.initialized == PASS_HOLDS_NO) {
    *after = PASS_FRAME_INITIALIZED;
    return IR_OP_INITIALIZE_FRAME;
  }
  if (need == PASS_NEED_INITIALIZED && state.initialized == PASS_HOLDS_MAYBE) {
    *after = PASS_FRAME_INITIALIZED;
    return IR_OP_ENSURE_FRAME_INITIALIZED;
  }
  if (need == PASS_NEED_PUBLISHED && state.published != PASS_HOLDS_YES) {
    after->published = PASS_HOLDS_YES;
    return IR_OP_PUBLISH_FRAME;
  }
  return IR_OP__COUNT;
}

static pass_need_t pass_need(const ir_graph_t *graph, const ir_node_t *node, const ir_frame_layout_t *layout) {
  const ir_op_t *op = &node->op;
  switch (op->code) {
    case IR_OP_STORE_SLOT: return op->slot >= layout->reserved_count ? PASS_NEED_INITIALIZED : PASS_NEED_NOTHING;
    case IR_OP_LOAD_SLOT:
      return op->slot >= layout->reserved_count && op->slot < ir_layout_arguments_base(layout)
        ? PASS_NEED_INITIALIZED : PASS_NEED_NOTHING;
    case IR_OP_CALL_SLOW_PATH:
      if (op->slow.func != graph->func) return PASS_NEED_PUBLISHED;
      return op->slow.saves_registers ? PASS_NEED_INITIALIZED_FOR_SLOW_PATH : PASS_NEED_INITIALIZED;
    case IR_OP_CALL_NATIVE: return op->call.needs_frame ? PASS_NEED_INITIALIZED : PASS_NEED_NOTHING;
    case IR_OP_CALL_DIRECT:
    case IR_OP_CALL_CONSTRUCT:
    case IR_OP_NEW_THIS:
    case IR_OP_ALLOCATE_OBJECT:
    case IR_OP_ALLOCATE_ARRAY:
    case IR_OP_CONCATENATE_STRINGS: return PASS_NEED_NOTHING;
    case IR_OP_TO_BOOLEAN:
    case IR_OP_STRICT_EQUALS:
    case IR_OP_CALL_FLOAT64_FUNCTION: return PASS_NEED_NOTHING;
    default: break;
  }
  ir_node_properties_t properties = ir_op_properties(op);
  if (properties.is_call || properties.can_lazy_exit) return PASS_NEED_INITIALIZED;
  if (properties.allocates) return PASS_NEED_PUBLISHED;
  return PASS_NEED_NOTHING;
}

typedef struct {
  ir_block_id_t block;
  uint32_t index;
  uint32_t order;
  ir_opcode_t op;
} pass_insertion_t;

static int pass_compare_insertions(const void *a, const void *b) {
  const pass_insertion_t *x = a, *y = b;
  if (x->block != y->block) return x->block < y->block ? -1 : 1;
  if (x->index != y->index) return x->index < y->index ? -1 : 1;
  return (x->order > y->order) - (x->order < y->order);
}

void pass_frame_initialization(ir_graph_t *graph, const ir_frame_layout_t *layout) {
  ir_block_id_t entry_block = IR_NONE;
  ir_node_id_t entry_node = IR_NONE;
  for (ir_block_id_t block = 0; block < ir_block_count(graph) && entry_block == IR_NONE; block++) {
    const ir_node_vec_t *body = &ir_block(graph, block)->body;
    for (uint32_t i = 0; i < body->len; i++) {
      if (ir_node(graph, body->items[i])->op.code != IR_OP_INITIALIZE_FRAME) continue;
      entry_block = block;
      entry_node = body->items[i];
      break;
    }
  }
  if (entry_block == IR_NONE) return;
  ir_node_vec_t *entry_body = &ir_block(graph, entry_block)->body;
  uint32_t kept = 0;
  for (uint32_t i = 0; i < entry_body->len; i++)
    if (entry_body->items[i] != entry_node) entry_body->items[kept++] = entry_body->items[i];
  entry_body->len = kept;
  uint32_t entry_pc = ir_node(graph, entry_node)->bc_off;

  opt_arena_t scratch;
  opt_arena_init(&scratch);
  uint32_t block_count = ir_block_count(graph);
  pass_frame_state_t *end_states = opt_arena_new(&scratch, pass_frame_state_t, block_count + 1);
  bool *has_end_state = opt_arena_new(&scratch, bool, block_count + 1);
  OPT_VEC(pass_insertion_t) insertions = {0};
  ir_node_vec_t slow_paths = {0};
  for (;;) {
    insertions.len = 0;
    slow_paths.len = 0;
    bool changed = false;
    for (ir_block_id_t block = 0; block < block_count; block++) {
      const ir_block_t *data = ir_block(graph, block);
      pass_frame_state_t current = PASS_FRAME_INITIALIZED;
      bool osr_entry = false;
      for (uint32_t i = 0; i < graph->osr_entries.len; i++) osr_entry |= graph->osr_entries.items[i].block == block;
      if (osr_entry) {
        current = PASS_FRAME_INITIALIZED;
      } else if (!data->predecessors.len) {
        current = (pass_frame_state_t){PASS_HOLDS_NO, PASS_HOLDS_NO};
      } else {
        bool any = false;
        for (uint32_t i = 0; i < data->predecessors.len; i++) {
          ir_block_id_t predecessor = data->predecessors.items[i];
          if (!has_end_state[predecessor]) continue;
          current = any ? pass_frame_merge(current, end_states[predecessor]) : end_states[predecessor];
          any = true;
        }
        if (!any) continue;
      }
      uint32_t count = data->body.len + (data->control != IR_NONE);
      for (uint32_t index = 0; index < count; index++) {
        ir_node_id_t node = index < data->body.len ? data->body.items[index] : data->control;
        pass_need_t need = pass_need(graph, ir_node(graph, node), layout);
        if (need == PASS_NEED_INITIALIZED_FOR_SLOW_PATH) {
          if (current.initialized != PASS_HOLDS_YES) {
            opt_vec_push(&scratch, &slow_paths, node);
            current = (pass_frame_state_t){pass_holds_merge(current.published, PASS_HOLDS_YES), PASS_HOLDS_MAYBE};
          }
          continue;
        }
        pass_frame_state_t after;
        ir_opcode_t op = pass_satisfy(current, need, &after);
        if (op != IR_OP__COUNT) {
          pass_insertion_t insertion = {.block = block, .index = index, .order = insertions.len, .op = op};
          opt_vec_push(&scratch, &insertions, insertion);
        }
        current = after;
      }
      if (!has_end_state[block] || end_states[block].published != current.published
        || end_states[block].initialized != current.initialized) {
        end_states[block] = current;
        has_end_state[block] = true;
        changed = true;
      }
    }
    if (!changed) break;
  }

  if (insertions.len) qsort(insertions.items, insertions.len, sizeof *insertions.items, pass_compare_insertions);
  for (uint32_t i = insertions.len; i-- > 0;) {
    pass_insertion_t insertion = insertions.items[i];
    ir_node_id_t node = ir_add_node(graph, ir_op(insertion.op), NULL, 0, OPT_REPR_NONE, entry_pc);
    opt_vec_insert(&graph->arena, &ir_block(graph, insertion.block)->body, insertion.index, node);
  }
  graph->slow_paths_initializing_frame.len = 0;
  for (uint32_t i = 0; i < slow_paths.len; i++) opt_vec_push(&graph->arena, &graph->slow_paths_initializing_frame, slow_paths.items[i]);
  opt_arena_free(&scratch);
}
