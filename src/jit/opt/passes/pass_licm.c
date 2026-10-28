#include "passes.h"

typedef struct {
  ir_block_id_t header;
  ir_block_id_t preheader;
  opt_bitset_t blocks;
  ir_block_vec_t back_edge_sources;
} pass_loop_t;

typedef OPT_VEC(pass_loop_t) pass_loops_t;

static bool pass_add_preheaders(ir_graph_t *graph, const pass_dominators_t *dominators, opt_arena_t *scratch) {
  uint32_t count = ir_block_count(graph);
  ir_block_vec_t order = {0};
  for (ir_block_id_t header = 0; header < count; header++) {
    ir_block_vec_t predecessors = {0}, back_edge_sources = {0}, entries = {0};
    opt_vec_clone(scratch, &predecessors, &ir_block(graph, header)->predecessors);
    bool later_entry = false;
    for (uint32_t i = 0; i < predecessors.len; i++) {
      ir_block_id_t predecessor = predecessors.items[i];
      if (pass_dominates(dominators, header, predecessor)) opt_vec_push(scratch, &back_edge_sources, predecessor);
      else {
        opt_vec_push(scratch, &entries, predecessor);
        later_entry |= predecessor > header;
      }
    }
    bool single_entry = false;
    if (entries.len == 1) {
      ir_successors_t successors = ir_block_successors(graph, entries.items[0]);
      single_entry = successors.len == 1 && ir_successor_at(&successors, 0) == header;
    }
    if (!back_edge_sources.len || !entries.len || later_entry || single_entry) {
      opt_vec_push(scratch, &order, header);
      continue;
    }
    ir_block_id_t preheader = ir_add_block(graph);
    opt_vec_copy(&graph->arena, &ir_block(graph, preheader)->predecessors, &entries);
    for (uint32_t i = 0; i < entries.len; i++) ir_replace_successor(graph, entries.items[i], header, preheader);
    ir_node_vec_t phis = {0};
    opt_vec_clone(scratch, &phis, &ir_block(graph, header)->phis);
    for (uint32_t p = 0; p < phis.len; p++) {
      ir_node_id_t phi = phis.items[p];
      ir_node_vec_t entry_inputs = {0}, back_inputs = {0};
      for (uint32_t i = 0; i < predecessors.len && i < ir_node(graph, phi)->inputs.len; i++) {
        ir_node_id_t input = ir_node(graph, phi)->inputs.items[i];
        if (ir_block_vec_position(&back_edge_sources, predecessors.items[i]) >= 0) opt_vec_push(scratch, &back_inputs, input);
        else opt_vec_push(scratch, &entry_inputs, input);
      }
      bool same = true;
      for (uint32_t i = 1; i < entry_inputs.len && same; i++) same = entry_inputs.items[i] == entry_inputs.items[0];
      ir_node_id_t entering = entry_inputs.items[0];
      if (!same) {
        entering = ir_add_node(graph, ir_op(IR_OP_PHI), entry_inputs.items, entry_inputs.len, ir_node(graph, phi)->repr, ir_node(graph, phi)->bc_off);
        opt_vec_push(&graph->arena, &ir_block(graph, preheader)->phis, entering);
      }
      ir_node_vec_t inputs = {0};
      opt_vec_push(scratch, &inputs, entering);
      for (uint32_t i = 0; i < back_inputs.len; i++) opt_vec_push(scratch, &inputs, back_inputs.items[i]);
      ir_set_inputs(graph, phi, inputs.items, inputs.len);
    }
    ir_node_id_t jump = ir_add_node(graph, ir_op(IR_OP_JUMP), NULL, 0, OPT_REPR_NONE, ir_control(graph, header)->bc_off);
    ir_node(graph, jump)->op.target = header;
    ir_block(graph, preheader)->control = jump;
    ir_block_vec_t header_predecessors = {0};
    opt_vec_push(&graph->arena, &header_predecessors, preheader);
    for (uint32_t i = 0; i < back_edge_sources.len; i++) opt_vec_push(&graph->arena, &header_predecessors, back_edge_sources.items[i]);
    ir_block(graph, header)->predecessors = header_predecessors;
    opt_vec_push(scratch, &order, preheader);
    opt_vec_push(scratch, &order, header);
  }
  if (ir_block_count(graph) == count) return false;
  ir_reorder_blocks(graph, order.items, order.len);
  return true;
}

static void pass_find_loops(const ir_graph_t *graph, const pass_dominators_t *dominators, opt_arena_t *scratch, pass_loops_t *loops) {
  for (ir_block_id_t header = 0; header < ir_block_count(graph); header++) {
    ir_block_vec_t back_edge_sources = {0};
    pass_back_edge_sources(dominators, graph, header, scratch, &back_edge_sources);
    if (!back_edge_sources.len) continue;
    opt_bitset_t blocks = opt_bitset_new(scratch, ir_block_count(graph));
    opt_bitset_insert(&blocks, header);
    ir_block_vec_t pending = {0};
    opt_vec_copy(scratch, &pending, &back_edge_sources);
    while (pending.len) {
      ir_block_id_t block = pending.items[--pending.len];
      if (opt_bitset_contains(&blocks, block)) continue;
      opt_bitset_insert(&blocks, block);
      const ir_block_vec_t *predecessors = &ir_block(graph, block)->predecessors;
      for (uint32_t i = 0; i < predecessors->len; i++) opt_vec_push(scratch, &pending, predecessors->items[i]);
    }
    const ir_block_vec_t *predecessors = &ir_block(graph, header)->predecessors;
    uint32_t entry_count = 0;
    ir_block_id_t preheader = IR_NONE;
    for (uint32_t i = 0; i < predecessors->len; i++) {
      if (opt_bitset_contains(&blocks, predecessors->items[i])) continue;
      entry_count++;
      preheader = predecessors->items[i];
    }
    if (entry_count != 1) continue;
    ir_successors_t successors = ir_block_successors(graph, preheader);
    if (successors.len != 1 || ir_successor_at(&successors, 0) != header) continue;
    pass_loop_t loop = {.header = header, .preheader = preheader, .blocks = blocks, .back_edge_sources = back_edge_sources};
    opt_vec_push(scratch, loops, loop);
  }
}

void pass_licm(ir_graph_t *graph) {
  bool has_loops = false;
  for (ir_block_id_t block = 0; block < ir_block_count(graph) && !has_loops; block++) {
    const ir_block_vec_t *predecessors = &ir_block(graph, block)->predecessors;
    for (uint32_t i = 0; i < predecessors->len && !has_loops; i++) has_loops = predecessors->items[i] >= block;
  }
  if (!has_loops) return;
  opt_arena_t scratch;
  opt_arena_init(&scratch);
  pass_dominators_t dominators;
  pass_dominators_compute(&dominators, graph, &scratch);
  if (pass_add_preheaders(graph, &dominators, &scratch)) {
    pass_remove_trivial_phis(graph);
    pass_dominators_compute(&dominators, graph, &scratch);
  }
  pass_loops_t loops = {0};
  pass_find_loops(graph, &dominators, &scratch, &loops);
  if (!loops.len) {
    opt_arena_free(&scratch);
    return;
  }
  ir_block_id_t *block_of = opt_arena_alloc(&scratch, (ir_node_count(graph) + 1) * sizeof *block_of);
  for (uint32_t i = 0; i < ir_node_count(graph); i++) block_of[i] = IR_NONE;
  for (ir_block_id_t block = 0; block < ir_block_count(graph); block++) {
    const ir_block_t *data = ir_block(graph, block);
    for (uint32_t i = 0, count = ir_block_node_count(data); i < count; i++) block_of[ir_block_node_at(data, i)] = block;
  }

  ir_node_vec_t body = {0}, hoisted = {0};
  for (uint32_t l = loops.len; l-- > 0;) {
    const pass_loop_t *loop = &loops.items[l];
    ir_locations_t writes = IR_LOC_NONE;
    opt_bitset_for_each(&loop->blocks, block) {
      const ir_block_t *data = ir_block(graph, block);
      for (uint32_t i = 0, count = ir_block_node_count(data); i < count; i++) {
        ir_node_properties_t properties = ir_op_properties(&ir_node(graph, ir_block_node_at(data, i))->op);
        writes |= properties.runs_code ? IR_LOC_ALL : properties.writes;
      }
    }
    uint32_t header_pc = ir_block(graph, loop->header)->bytecode_start;
    bool exit_state_known = false;
    ir_frame_state_id_t exit_state = IR_NONE;
    hoisted.len = 0;
    opt_bitset_for_each(&loop->blocks, block) {
      bool runs_every_iteration = pass_runs_every_iteration(&dominators, block, &loop->back_edge_sources);
      bool is_cold = ir_block(graph, block)->is_cold;
      opt_vec_clone(&scratch, &body, &ir_block(graph, block)->body);
      for (uint32_t b = 0; b < body.len; b++) {
        ir_node_id_t node = body.items[b];
        const ir_node_t *data = ir_node(graph, node);
        ir_node_properties_t properties = ir_op_properties(&data->op);
        bool invariant_inputs = true;
        for (uint32_t i = 0; i < data->inputs.len && invariant_inputs; i++) {
          ir_block_id_t input_block = block_of[data->inputs.items[i]];
          invariant_inputs = input_block == IR_NONE || !opt_bitset_contains(&loop->blocks, input_block);
        }
        bool movable = data->op.code != IR_OP_REFINE && !(properties.is_call && is_cold) && !properties.can_lazy_exit
          && !properties.allocates && properties.writes == IR_LOC_NONE && !properties.runs_code
          && !ir_locations_intersect(properties.reads, writes);
        if (!invariant_inputs || !movable) continue;
        opt_exit_kind_t kind;
        if (ir_op_exit_kind(&data->op, &kind)) {
          sv_func_t *header_func = ir_block(graph, loop->header)->func;
          bool speculative = !runs_every_iteration || ir_node_vec_contains(&graph->speculatively_hoisted, node);
          bool exited = header_pc != IR_NONE && ir_exit_sites_contain(graph, header_func, header_pc, kind);
          bool speculation_exited = header_pc != IR_NONE && ir_exit_sites_contain(graph, header_func, header_pc, OPT_EXIT_HOISTED);
          if ((speculative && is_cold) || exited || (speculative && speculation_exited)) continue;
          if (!exit_state_known) {
            exit_state_known = true;
            ir_frame_state_t state;
            if (pass_frame_state_entering(graph, loop->header, loop->preheader, &scratch, &state))
              exit_state = ir_add_frame_state(graph, &state);
          }
          if (exit_state == IR_NONE) continue;
          ir_node(graph, node)->frame_state = exit_state;
          if (speculative && !ir_node_vec_contains(&graph->speculatively_hoisted, node))
            opt_vec_push(&graph->arena, &graph->speculatively_hoisted, node);
        }
        ir_node_vec_t *block_body = &ir_block(graph, block)->body;
        int position = ir_node_vec_position(block_body, node);
        if (position >= 0) opt_vec_remove(block_body, (uint32_t)position);
        block_of[node] = loop->preheader;
        opt_vec_push(&scratch, &hoisted, node);
      }
    }
    for (uint32_t i = 0; i < hoisted.len; i++) opt_vec_push(&graph->arena, &ir_block(graph, loop->preheader)->body, hoisted.items[i]);
  }
  opt_arena_free(&scratch);
}
