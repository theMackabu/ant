#ifndef ANT_JIT_OPT_PASSES_H
#define ANT_JIT_OPT_PASSES_H

#include "../bitset.h"
#include "../hash.h"
#include "../ir/ir.h"

void pass_optimize(ir_graph_t *graph, const ir_frame_layout_t *layout, bool verify);
void pass_optimize_with_dumps(ir_graph_t *graph, const ir_frame_layout_t *layout, bool verify, opt_text_t *dumps);

void pass_simplify(ir_graph_t *graph);
void pass_truncation(ir_graph_t *graph);
void pass_boxed_arithmetic(ir_graph_t *graph);
void pass_bounds_check_elimination(ir_graph_t *graph);
void pass_representation(ir_graph_t *graph);
void pass_check_elimination(ir_graph_t *graph);
void pass_gvn(ir_graph_t *graph);
void pass_licm(ir_graph_t *graph);
void pass_dce(ir_graph_t *graph);
void pass_escape_analysis(ir_graph_t *graph);
void pass_branch_fusion(ir_graph_t *graph);
void pass_frame_initialization(ir_graph_t *graph, const ir_frame_layout_t *layout);

bool pass_verify(const ir_graph_t *graph, opt_text_t *errors);
bool pass_verify_frame_writes(const ir_graph_t *graph, const ir_frame_layout_t *layout, opt_text_t *errors);

typedef struct {
  ir_block_id_t *idom;
  bool *reachable;
  uint32_t count;
} pass_dominators_t;

void pass_dominators_compute(pass_dominators_t *dominators, const ir_graph_t *graph, opt_arena_t *arena);
bool pass_dominates(const pass_dominators_t *dominators, ir_block_id_t dominator, ir_block_id_t block);
void pass_back_edge_sources(const pass_dominators_t *dominators, const ir_graph_t *graph, ir_block_id_t header, opt_arena_t *arena, ir_block_vec_t *sources);
bool pass_runs_every_iteration(const pass_dominators_t *dominators, ir_block_id_t block, const ir_block_vec_t *back_edge_sources);
ir_block_vec_t *pass_dominator_children(const pass_dominators_t *dominators, opt_arena_t *arena);

static inline ir_block_id_t pass_idom(const pass_dominators_t *dominators, ir_block_id_t block) {
  return dominators->idom[block];
}

typedef struct {
  opt_arena_t *arena;
  ir_node_id_t *map;
  uint32_t len;
  uint32_t count;
} pass_replacements_t;

void pass_replacements_init(pass_replacements_t *replacements, const ir_graph_t *graph, opt_arena_t *arena);
void pass_replace(pass_replacements_t *replacements, ir_node_id_t value, ir_node_id_t replacement);
ir_node_id_t pass_resolve(const pass_replacements_t *replacements, ir_node_id_t value);
void pass_replacements_apply(const pass_replacements_t *replacements, ir_graph_t *graph);

static inline bool pass_replacements_is_empty(const pass_replacements_t *replacements) {
  return replacements->count == 0;
}

typedef struct {
  opt_map_t nodes;
} pass_constants_t;

void pass_constants_init(pass_constants_t *constants, const ir_graph_t *graph, opt_arena_t *arena);
ir_node_id_t pass_constant_typed(pass_constants_t *constants, ir_graph_t *graph, uint64_t bits, opt_repr_t repr);

static inline ir_node_id_t pass_constant(pass_constants_t *constants, ir_graph_t *graph, uint64_t bits) {
  return pass_constant_typed(constants, graph, bits, OPT_REPR_TAGGED);
}

void pass_remove_refinements(ir_graph_t *graph);
void pass_remove_nodes(ir_graph_t *graph, const opt_bitset_t *removed);
void pass_remove_unreachable_blocks(ir_graph_t *graph);
void pass_clean_up_control_flow(ir_graph_t *graph);
void pass_make_jump(ir_graph_t *graph, ir_node_id_t control, ir_block_id_t target);
bool pass_fold_branches_to_one_target(ir_graph_t *graph);
bool pass_frame_state_entering(const ir_graph_t *graph, ir_block_id_t block, ir_block_id_t predecessor, opt_arena_t *arena, ir_frame_state_t *state);
void pass_move_cold_blocks_last(ir_graph_t *graph);
void pass_remove_trivial_phis(ir_graph_t *graph);
void pass_remove_unused_frame_states(ir_graph_t *graph);

#endif
