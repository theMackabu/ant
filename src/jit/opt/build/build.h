#ifndef ANT_JIT_OPT_BUILD_H
#define ANT_JIT_OPT_BUILD_H

#include "../ir/ir.h"

#include "silver/engine.h"

static constexpr uint32_t OPT_THIS_SLOT = 0;

static inline uint32_t opt_stack_slot(uint32_t index) {
  return 1 + index;
}

static inline uint32_t opt_local_slot(const ir_frame_layout_t *layout, uint32_t index) {
  return layout->number_of_registers + index;
}

static inline uint32_t opt_param_slot(const ir_frame_layout_t *layout, uint32_t index) {
  return ir_layout_arguments_base(layout) + index;
}

static inline uint32_t opt_named_offset(uint32_t index, uint8_t inobj_limit) {
  return (uint32_t)inobj_limit << 16 | index;
}

static inline uint32_t opt_named_index(uint32_t offset) {
  return offset & 0xffff;
}

static inline uint32_t opt_named_inobj_limit(uint32_t offset) {
  return offset >> 16;
}

typedef struct {
  const char *reason;
  uint32_t bc_off;
  sv_func_t *func;
} opt_build_failure_t;

typedef const ir_exit_site_t *(*opt_build_exit_sites_fn)(ant_t *js, sv_func_t *func, uint32_t *count);

typedef struct {
  uint32_t inlined_calls;
} opt_build_stats_t;

static inline ir_frame_layout_t opt_frame_layout_of(const sv_func_t *func) {
  uint32_t stack = (uint32_t)(func->max_stack > 0 ? func->max_stack : 0), locals = (uint32_t)func->max_locals;
  return (ir_frame_layout_t){
    .reserved_count = 1,
    .number_of_registers = 1 + stack,
    .registers_and_locals_count = 1 + stack + locals,
    .number_of_constants = 0,
    .number_of_arguments = func->param_count,
  };
}

void opt_build_release(ant_t *js, ir_graph_t *graph);
uint32_t opt_learned_shapes(const sv_func_t *func, uint32_t bc_off);

bool opt_build_graph(
  ant_t *js, sv_func_t *func, sv_closure_t *closure, opt_build_exit_sites_fn exit_sites_of,
  ir_graph_t *graph, ir_frame_layout_t *layout, opt_build_failure_t *failure, opt_build_stats_t *stats
);

#endif
