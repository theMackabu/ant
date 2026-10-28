#ifndef ANT_JIT_OPT_RA_INTERNAL_H
#define ANT_JIT_OPT_RA_INTERNAL_H

#include "regalloc.h"

typedef OPT_VEC(uint32_t) ra_positions_t;

typedef struct {
  opt_bitset_t *live_in;
  opt_bitset_t *live_out;
  ir_node_vec_t *dies_at;
  opt_bitset_t dead_outputs;
  opt_bitset_t needs_spill;
  uint32_t *positions;
  ra_positions_t *use_positions;
} ra_liveness_t;

typedef struct {
  ir_node_id_t phi;
  ir_node_id_t input;
} ra_phi_input_t;

typedef OPT_VEC(ra_phi_input_t) ra_phi_inputs_t;

static inline bool ra_is_allocated_value(const ir_graph_t *graph, ir_node_id_t node) {
  const ir_node_t *data = ir_node(graph, node);
  return data->repr != OPT_REPR_NONE && data->op.code != IR_OP_CONSTANT && data->op.code != IR_OP_VIRTUAL_OBJECT;
}

static inline bool ra_clobbers_caller_saved(const ir_op_t *op) {
  return ir_op_properties(op).is_call || op->code == IR_OP_INITIALIZE_FRAME || op->code == IR_OP_ENSURE_FRAME_INITIALIZED
    || op->code == IR_OP_CALL_NATIVE;
}

void ra_liveness_compute(ra_liveness_t *liveness, const ir_graph_t *graph, opt_arena_t *arena);
uint32_t *ra_spill_slot_classes(const ir_graph_t *graph, const ra_liveness_t *liveness, opt_arena_t *arena);
void ra_phi_inputs_from(const ir_graph_t *graph, ir_block_id_t block, ir_block_id_t successor, opt_arena_t *arena, ra_phi_inputs_t *inputs);

#endif
