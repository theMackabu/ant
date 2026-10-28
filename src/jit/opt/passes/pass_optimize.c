#include "passes.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct {
  const char *name;
  void (*run)(ir_graph_t *graph);
} pass_t;

static const pass_t pass_list[] = {
  {"trivial phi removal", pass_remove_trivial_phis},
  {"boxed arithmetic", pass_boxed_arithmetic},
  {"truncation", pass_truncation},
  {"simplification", pass_simplify},
  {"representation selection", pass_representation},
  {"simplification", pass_simplify},
  {"global value numbering", pass_gvn},
  {"check elimination", pass_check_elimination},
  {"global value numbering", pass_gvn},
  {"loop invariant code motion", pass_licm},
  {"bounds check elimination", pass_bounds_check_elimination},
  {"dead code elimination", pass_dce},
  {"branch fusion", pass_branch_fusion},
};

static void pass_check(const ir_graph_t *graph, const ir_frame_layout_t *layout, bool verify, const char *after) {
  if (!verify) return;
  opt_text_t errors = {0};
  bool valid = pass_verify(graph, &errors) && pass_verify_frame_writes(graph, layout, &errors);
  if (valid) return;
  opt_text_t dump = {0};
  ir_dump(graph, layout, &dump);
  fprintf(stderr, "invalid graph after %s:\n%s\n%s", after, errors.data ? errors.data : "", dump.data ? dump.data : "");
  abort();
}

static void pass_observe(const ir_graph_t *graph, const ir_frame_layout_t *layout, opt_text_t *dumps, const char *after) {
  if (!dumps) return;
  opt_text_appendf(dumps, "=== after %s\n", after);
  ir_dump(graph, layout, dumps);
}

static void pass_run_all(ir_graph_t *graph, const ir_frame_layout_t *layout, bool verify, opt_text_t *dumps) {
  pass_remove_unreachable_blocks(graph);
  pass_check(graph, layout, verify, "graph building");
  pass_observe(graph, layout, dumps, "graph building");
  for (uint32_t i = 0; i < sizeof pass_list / sizeof *pass_list; i++) {
    pass_list[i].run(graph);
    pass_check(graph, layout, verify, pass_list[i].name);
    pass_observe(graph, layout, dumps, pass_list[i].name);
  }
  pass_remove_refinements(graph);
  pass_gvn(graph);
  pass_check(graph, layout, verify, "refinement removal");
  pass_observe(graph, layout, dumps, "refinement removal");
  pass_frame_initialization(graph, layout);
  pass_check(graph, layout, verify, "frame initialization");
  pass_remove_unused_frame_states(graph);
  for (uint32_t i = 0; i < graph->osr_entries.len; i++) ir_block(graph, graph->osr_entries.items[i].block)->is_cold = true;
  pass_move_cold_blocks_last(graph);
  pass_check(graph, layout, verify, "cold block layout");
  pass_observe(graph, layout, dumps, "frame initialization");
}

void pass_optimize(ir_graph_t *graph, const ir_frame_layout_t *layout, bool verify) {
  pass_run_all(graph, layout, verify, NULL);
}

void pass_optimize_with_dumps(ir_graph_t *graph, const ir_frame_layout_t *layout, bool verify, opt_text_t *dumps) {
  pass_run_all(graph, layout, verify, dumps);
}
