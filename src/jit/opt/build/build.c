// TODO: split into multiple module files

#include "build.h"
#include "../bitset.h"
#include "../hash.h"
#include "../ir/ir_value.h"
#include "../tier.h"

#include "internal.h"
#include "protectors.h"
#include "proto_watch.h"
#include "gc/objects.h"
#include "modules/math.h"
#include "runtime.h"
#include "shapes.h"
#include "silver/call.h"
#include "silver/feedback.h"
#include "silver/generic_step.h"
#include "../../jit_internal.h"
#include "../../../silver/ops/eval_env.h"
#include "../../../silver/ops/iteration.h"

#include <stdlib.h>

typedef struct {
  ir_node_id_t node;
  bool in_sync;
  bool boxed;
} opt_slot_t;

typedef struct {
  opt_slot_t *slots;
  uint32_t sp;
  ir_node_id_t this_value;
} opt_frame_t;

typedef struct {
  ir_block_id_t pred;
  opt_frame_t frame;
} opt_incoming_t;

static constexpr uint32_t OPT_FINALLY_MAX = 8;
static constexpr uint32_t OPT_FINALLY_CONTS = 4;
static constexpr uint32_t OPT_BLOCK_SUCCS = OPT_FINALLY_CONTS + 3;

typedef struct {
  uint32_t off;
  int32_t outer;
  bool has_return;
  uint32_t conts[OPT_FINALLY_CONTS];
  uint32_t cont_count;
} opt_finally_t;

typedef struct {
  uint32_t first, last;
  uint32_t succ[OPT_BLOCK_SUCCS];
  uint32_t succ_count;
  uint32_t depth;
  int32_t rpo;
  uint32_t heads;
  uint32_t handler;
  bool is_header;
  bool started;
  ir_block_id_t ir;
  OPT_VEC(opt_incoming_t) incoming;
  OPT_VEC(uint32_t) preds;
  opt_frame_t header_frame;
} opt_bblock_t;

typedef struct {
  uint32_t header;
  opt_bitset_t blocks;
  opt_bitset_t assigned;
  opt_bitset_t captures;
  opt_bitset_t entry_boxes;
} opt_loop_t;

typedef enum {
  OPT_FLOW_CONTINUE,
  OPT_FLOW_ENDED,
  OPT_FLOW_FAILED,
  OPT_FLOW_GENERIC,
} opt_flow_t;

typedef struct opt_builder opt_builder_t;

typedef struct {
  ir_block_vec_t order;
  opt_map_t constants;
  opt_build_exit_sites_fn exit_sites_of;
  sv_closure_t *closure;
  OPT_VEC(sv_func_t *) no_inline;
  OPT_VEC(sv_func_t *) no_virtual_arguments;
  OPT_VEC(sv_func_t *) virtual_arguments;
  bool retry;
  bool no_catch_entries;
  bool built_catch_entries;
  bool no_native_finally;
  bool used_native_finally;
  uint32_t inlined_instructions;
  uint32_t inlined_calls;
  ir_node_vec_t object_like;
  ir_node_vec_t likely_nodes;
  OPT_VEC(ant_value_t) likely_values;
  ir_node_vec_t literal_nodes;
  OPT_VEC(ant_shape_t *) literal_shapes;
  ir_node_vec_t named_closures;
} opt_build_ctx_t;

typedef struct {
  opt_builder_t *caller;
  ir_frame_state_id_t caller_state;
  ir_frame_state_id_t caller_position;
  ir_node_id_t closure;
  ir_node_id_t receiver;
  uint32_t argc;
  const ir_node_id_t *args;
  bool construct;
  uint32_t depth;
  ir_block_id_t continuation;
  ir_node_vec_t returns;
} opt_inline_t;

struct opt_builder {
  ant_t *js;
  sv_func_t *func;
  ir_graph_t *graph;
  opt_build_ctx_t *ctx;
  opt_inline_t *inline_call;
  ir_frame_layout_t layout;
  opt_arena_t arena;
  opt_build_failure_t *failure;
  bool failed;

  uint32_t params, locals, stack, tracked;
  bool uses_this;
  bool building_entry;
  int *depth;
  int *try_at;
  opt_finally_t *finallies;
  uint32_t finally_count;
  uint32_t *ins_off;
  uint32_t ins_count;
  uint32_t *block_of_ins;
  int32_t *block_at;
  opt_bblock_t *blocks;
  uint32_t block_count;
  uint32_t *rpo;
  uint32_t rpo_count;
  opt_loop_t *loops;
  uint32_t loop_count;
  bool *in_loop;
  opt_bitset_t *live_in;
  opt_bitset_t memory;
  opt_bitset_t builders;
  opt_bitset_t boxable;
  opt_bitset_t patched_constants;

  const ir_exit_site_t *exit_sites;
  uint32_t exit_site_count;

  ir_block_id_t block;
  opt_frame_t frame;
  uint32_t ins;
  uint32_t pc;
  ir_frame_state_id_t eager;
  bool slow_path;
  const opt_rt_add_site_t *slow_adds;
  ir_block_id_t tail_header;
  ir_node_id_t *tail_phis;
  bool virtual_arguments;
  ir_node_id_t arguments_marker;
  ir_slot_values_t state_values;
  ir_slots_t state_in_frame;
};

static bool opt_fail(opt_builder_t *b, const char *reason) {
  if (!b->failed) {
    b->failed = true;
    b->failure->reason = reason;
    b->failure->bc_off = b->pc;
  }
  return false;
}

static inline sv_op_t opt_op_at(const opt_builder_t *b, uint32_t off) {
  return (sv_op_t)b->func->code[off];
}

static int32_t opt_branch_target(const sv_func_t *func, uint32_t off) {
  sv_op_t op = (sv_op_t)func->code[off];
  int32_t size = sv_op_size[op];
  uint32_t flags = sv_op_flags[op];
  if (flags & SV_OPF_LABEL_NOT_EDGE) return -1;
  if (flags & SV_OPF_JIT_BRANCH32) return (int32_t)off + size + sv_get_i32(func->code + off + 1);
  if (flags & SV_OPF_JIT_BRANCH8) return (int32_t)off + size + sv_get_i8(func->code + off + 1);
  return -1;
}

static uint32_t opt_handler_target(const sv_func_t *func, uint32_t off) {
  return (uint32_t)((int32_t)off + sv_op_size[func->code[off]] + sv_get_i32(func->code + off + 1));
}

static inline bool opt_generic_supported(sv_op_t op) {
  return sv_generic_step_supports(op) && !(sv_op_flags[op] & SV_OPF_OPT_NO_GENERIC);
}

static inline bool opt_op_native(sv_op_t op) {
  return (sv_op_flags[op] & SV_OPF_OPT_NATIVE) != 0;
}

static inline bool opt_op_supported(sv_op_t op) {
  return opt_op_native(op) || opt_generic_supported(op);
}

static uint32_t opt_access(const opt_builder_t *b, uint32_t off, bool *use, bool *def) {
  int32_t slot = sv_op_frame_slot(b->func->code + off, b->params, use, def);
  return slot < 0 ? IR_NONE : (uint32_t)slot;
}

static uint32_t opt_tracked_slot(const opt_builder_t *b, uint32_t tracked) {
  if (tracked < b->params) return opt_param_slot(&b->layout, tracked);
  if (tracked < b->params + b->locals) return opt_local_slot(&b->layout, tracked - b->params);
  return opt_stack_slot(tracked - b->params - b->locals);
}

static inline uint32_t opt_stack_index(const opt_builder_t *b, uint32_t k) {
  return b->params + b->locals + k;
}

static int32_t opt_try_state(const opt_builder_t *b, uint32_t off) {
  return b->try_at && b->try_at[off] >= 0 ? b->try_at[off] : -1;
}

static int32_t opt_try_parent(const opt_builder_t *b, int32_t state) {
  int32_t parent = b->try_at[b->func->code_len + state];
  return parent >= 0 ? parent : -1;
}

static int32_t opt_catch_state(const opt_builder_t *b, uint32_t off) {
  int32_t state = opt_try_state(b, off);
  while (state >= 0 && opt_op_at(b, (uint32_t)state) == OP_FINALLY) state = opt_try_parent(b, state);
  return state;
}

static bool opt_state_has(const opt_builder_t *b, uint32_t off, sv_op_t op) {
  for (int32_t state = opt_try_state(b, off); state >= 0; state = opt_try_parent(b, state))
    if (opt_op_at(b, (uint32_t)state) == op) return true;
  return false;
}

static int32_t opt_finally_index(const opt_builder_t *b, int32_t off) {
  for (uint32_t i = 0; off >= 0 && i < b->finally_count; i++)
    if (b->finallies[i].off == (uint32_t)off) return (int32_t)i;
  return -1;
}

static int32_t opt_finally_of(const opt_builder_t *b, uint32_t push) {
  uint32_t len = (uint32_t)b->func->code_len, handler = opt_handler_target(b->func, push);
  if (handler >= len || opt_op_at(b, handler) != OP_POP) return -1;
  uint32_t jump = handler + sv_op_size[OP_POP];
  if (jump >= len || (opt_op_at(b, jump) != OP_JMP && opt_op_at(b, jump) != OP_JMP8)) return -1;
  int32_t target = opt_branch_target(b->func, jump);
  return target >= 0 && (uint32_t)target < len && opt_op_at(b, (uint32_t)target) == OP_FINALLY ? target : -1;
}

static int32_t opt_enclosing_try_finally(const opt_builder_t *b, int32_t state) {
  for (int32_t s = state; s >= 0; s = opt_try_parent(b, s))
    if (opt_op_at(b, (uint32_t)s) == OP_TRY_PUSH_FINALLY) return s;
  return -1;
}

static int32_t opt_native_finally_target(const opt_builder_t *b, uint32_t off) {
  sv_op_t op = opt_op_at(b, off);
  if (!b->finally_count) return -1;
  if (op == OP_RETURN || op == OP_RETURN_UNDEF || op == OP_UNWIND_JMP) {
    int32_t push = opt_enclosing_try_finally(b, opt_try_state(b, off));
    return push >= 0 ? opt_finally_of(b, (uint32_t)push) : -1;
  }
  return -1;
}

static bool opt_scan_finally(opt_builder_t *b) {
  b->finally_count = 0;
  if (b->inline_call || !b->try_at || b->ctx->no_native_finally) return true;
  sv_func_t *func = b->func;
  uint32_t len = (uint32_t)func->code_len, count = 0;
  for (uint32_t off = 0; off < len; off += sv_op_size[func->code[off]])
    if (b->depth[off] >= 0 && opt_op_at(b, off) == OP_FINALLY) count++;
  if (!count || count > OPT_FINALLY_MAX) return true;
  b->finallies = opt_arena_new(&b->arena, opt_finally_t, count);
  for (uint32_t off = 0; off < len; off += sv_op_size[func->code[off]])
    if (b->depth[off] >= 0 && opt_op_at(b, off) == OP_FINALLY) b->finallies[b->finally_count++] = (opt_finally_t){.off = off, .outer = -1};
  bool ok = true;
  for (uint32_t off = 0; ok && off < len; off += sv_op_size[func->code[off]]) {
    if (b->depth[off] < 0) continue;
    sv_op_t op = opt_op_at(b, off);
    int32_t state = opt_try_state(b, off);
    if (op == OP_TRY_PUSH_FINALLY) {
      int32_t at = opt_finally_index(b, opt_finally_of(b, off));
      ok = at >= 0 && b->depth[b->finallies[at].off] == b->depth[off];
    } else if (op == OP_RETURN || op == OP_RETURN_UNDEF) {
      int32_t push = opt_enclosing_try_finally(b, state);
      if (push < 0) continue;
      int32_t at = opt_finally_index(b, opt_finally_of(b, (uint32_t)push));
      if (at < 0) ok = false;
      else b->finallies[at].has_return = true;
    } else if (op == OP_UNWIND_JMP) {
      int32_t at = state >= 0 && opt_op_at(b, (uint32_t)state) == OP_TRY_PUSH_FINALLY ? opt_finally_index(b, opt_finally_of(b, (uint32_t)state)) : -1;
      uint32_t cont = off + sv_op_size[OP_UNWIND_JMP] + (uint32_t)sv_get_i32(func->code + off + 1);
      if (at < 0 || cont >= len || b->depth[cont] < 0 || b->depth[cont] != b->depth[b->finallies[at].off]) {
        ok = false;
        continue;
      }
      opt_finally_t *fin = &b->finallies[at];
      uint32_t i = 0;
      while (i < fin->cont_count && fin->conts[i] != cont) i++;
      if (i < fin->cont_count) continue;
      if (fin->cont_count == OPT_FINALLY_CONTS) ok = false;
      else fin->conts[fin->cont_count++] = cont;
    } else if (op == OP_FINALLY_RET || op == OP_FINALLY_DISCARD) {
      ok = opt_finally_index(b, state) >= 0;
    }
  }
  for (uint32_t i = 0; ok && i < b->finally_count; i++) {
    int32_t push = opt_enclosing_try_finally(b, opt_try_parent(b, (int32_t)b->finallies[i].off));
    if (push < 0) continue;
    b->finallies[i].outer = opt_finally_index(b, opt_finally_of(b, (uint32_t)push));
    ok = b->finallies[i].outer >= 0;
  }
  for (bool changed = true; ok && changed;) {
    changed = false;
    for (uint32_t i = 0; i < b->finally_count; i++) {
      opt_finally_t *fin = &b->finallies[i];
      if (!fin->has_return || fin->outer < 0 || b->finallies[fin->outer].has_return) continue;
      b->finallies[fin->outer].has_return = true;
      changed = true;
    }
  }
  if (!ok) b->finally_count = 0;
  b->ctx->used_native_finally |= b->finally_count != 0;
  return true;
}

static int32_t opt_edge_target(const opt_builder_t *b, uint32_t off) {
  int32_t native = opt_native_finally_target(b, off);
  if (native >= 0) return native;
  if (opt_op_at(b, off) == OP_UNWIND_JMP) return (int32_t)off + sv_op_size[OP_UNWIND_JMP] + sv_get_i32(b->func->code + off + 1);
  if (opt_op_at(b, off) != OP_FINALLY_RET) return opt_branch_target(b->func, off);
  int32_t state = opt_try_state(b, off);
  return state >= 0 && opt_op_at(b, (uint32_t)state) == OP_FINALLY ? (int32_t)opt_handler_target(b->func, (uint32_t)state) : -1;
}

static bool opt_scan(opt_builder_t *b) {
  sv_func_t *func = b->func;
  uint32_t len = (uint32_t)func->code_len;
  b->ins_off = opt_arena_new(&b->arena, uint32_t, len + 1);
  b->block_at = opt_arena_alloc(&b->arena, (len + 1) * sizeof *b->block_at);
  bool *leader = opt_arena_new(&b->arena, bool, len + 1);
  for (uint32_t off = 0; off <= len; off++) b->block_at[off] = -1;

  bool ends = false;
  for (uint32_t off = 0; off < len;) {
    sv_op_t op = opt_op_at(b, off);
    uint32_t size = sv_op_size[op];
    b->pc = off;
    if (size == 0) return opt_fail(b, "op-size");
    if (b->depth[off] >= 0) {
      if (!opt_op_supported(op)) return opt_fail(b, "op");
      if (ends) leader[off] = true;
      ends = false;
      b->ins_off[b->ins_count++] = off;
      int32_t target = opt_edge_target(b, off);
      if (op == OP_FINALLY_RET && target < 0) return opt_fail(b, "finally-target");
      if (target >= 0) {
        if ((uint32_t)target >= len || b->depth[target] < 0) return opt_fail(b, "branch-target");
        leader[target] = true;
      }
      if (op == OP_TRY_PUSH || op == OP_TRY_PUSH_FINALLY) {
        uint32_t handler = opt_handler_target(func, off);
        if (handler >= len || b->depth[handler] < 0) return opt_fail(b, "handler-target");
        leader[handler] = true;
      }
      ends = target >= 0 || (sv_op_flags[op] & (SV_OPF_TERMINAL | SV_OPF_OPT_HANDLER));
      if (ends && off + size < len) leader[off + size] = true;
    }
    off += size;
  }
  if (!b->ins_count || b->ins_off[0] != 0) return opt_fail(b, "entry");
  leader[0] = true;

  b->block_of_ins = opt_arena_new(&b->arena, uint32_t, b->ins_count);
  b->blocks = opt_arena_new(&b->arena, opt_bblock_t, b->ins_count);
  for (uint32_t i = 0; i < b->ins_count; i++) {
    uint32_t off = b->ins_off[i];
    if (leader[off]) {
      opt_bblock_t *block = &b->blocks[b->block_count];
      *block = (opt_bblock_t){.first = i, .rpo = -1, .heads = IR_NONE, .ir = IR_NONE, .depth = (uint32_t)b->depth[off]};
      b->block_at[off] = (int32_t)b->block_count++;
    }
    b->block_of_ins[i] = b->block_count - 1;
    b->blocks[b->block_count - 1].last = i;
  }

  for (uint32_t bi = 0; bi < b->block_count; bi++) {
    opt_bblock_t *block = &b->blocks[bi];
    uint32_t off = b->ins_off[block->last];
    sv_op_t op = opt_op_at(b, off);
    uint32_t next = off + sv_op_size[op];
    int32_t target = opt_edge_target(b, off);
    b->pc = off;
    if (target >= 0) block->succ[block->succ_count++] = (uint32_t)b->block_at[target];
    bool unconditional = (sv_op_flags[op] & SV_OPF_TERMINAL) != 0;
    if ((target < 0 && !(sv_op_flags[op] & SV_OPF_TERMINAL)) || (target >= 0 && !unconditional)) {
      if (next >= len || b->block_at[next] < 0) return opt_fail(b, "fell-off");
      block->succ[block->succ_count++] = (uint32_t)b->block_at[next];
    }
    int32_t fin = op == OP_FINALLY_RET ? opt_finally_index(b, opt_try_state(b, off)) : -1;
    for (uint32_t c = 0; fin >= 0 && c <= b->finallies[fin].cont_count; c++) {
      const opt_finally_t *at = &b->finallies[fin];
      int32_t to = c < at->cont_count ? (int32_t)at->conts[c] : at->has_return && at->outer >= 0 ? (int32_t)b->finallies[at->outer].off : -1;
      if (to < 0) continue;
      if (b->block_at[to] < 0) return opt_fail(b, "finally-target");
      uint32_t succ = (uint32_t)b->block_at[to], s = 0;
      while (s < block->succ_count && block->succ[s] != succ) s++;
      if (s == block->succ_count) block->succ[block->succ_count++] = succ;
    }
    for (uint32_t s = 0; s < block->succ_count; s++)
      opt_vec_push(&b->arena, &b->blocks[block->succ[s]].preds, bi);
    block->handler = UINT32_MAX;
    int32_t state = opt_catch_state(b, b->ins_off[block->first]);
    if (state >= 0) block->handler = (uint32_t)b->block_at[opt_handler_target(func, (uint32_t)state)];
  }
  return true;
}

static bool opt_scan_try(opt_builder_t *b) {
  sv_func_t *func = b->func;
  if (!func->has_try) return true;
  int *map = sv_func_try_map(func);
  if (!map) return opt_fail(b, "try-map");
  b->try_at = opt_arena_dup(&b->arena, map, 2 * (size_t)func->code_len * sizeof *map);
  free(map);
  return true;
}

static bool opt_in_try(const opt_builder_t *b) {
  return !b->inline_call && opt_catch_state(b, b->pc) >= 0;
}

static void opt_record_handlers(opt_builder_t *b) {
  if (!b->try_at || b->inline_call) return;
  ir_graph_t *graph = b->graph;
  uint32_t len = (uint32_t)b->func->code_len;
  graph->handler_at = opt_arena_alloc(&graph->arena, len * sizeof *graph->handler_at);
  graph->handler_at_len = len;
  for (uint32_t off = 0; off < len; off++) graph->handler_at[off] = -1;
  for (uint32_t i = 0; i < b->ins_count; i++) {
    uint32_t off = b->ins_off[i];
    int32_t state = opt_catch_state(b, off);
    if (state < 0) continue;
    uint32_t index = 0;
    while (index < graph->handlers.len && graph->handlers.items[index].try_off != (uint32_t)state) index++;
    if (index == graph->handlers.len) {
      ir_handler_t handler = {
        .try_off = (uint32_t)state,
        .catch_off = opt_handler_target(b->func, (uint32_t)state),
        .depth = (uint32_t)b->depth[state],
        .finally = opt_op_at(b, (uint32_t)state) == OP_TRY_PUSH_FINALLY,
      };
      opt_vec_push(&graph->arena, &graph->handlers, handler);
    }
    graph->handler_at[off] = (int32_t)index;
  }
}

static bool opt_scan_memory(opt_builder_t *b) {
  sv_func_t *func = b->func;
  uint32_t width = b->params + b->locals;
  b->memory = opt_bitset_new(&b->arena, width);
  b->builders = opt_bitset_new(&b->arena, width);
  b->boxable = opt_bitset_new(&b->arena, width);
  opt_bitset_t captured = opt_bitset_new(&b->arena, width);
  b->patched_constants = opt_bitset_new(&b->arena, (uint32_t)func->const_count + 1);
  for (uint32_t i = 0; i < b->ins_count; i++) {
    const uint8_t *ip = func->code + b->ins_off[i];
    b->pc = b->ins_off[i];
    switch (opt_op_at(b, b->pc)) {
      case OP_CLOSURE: {
        uint32_t index = sv_get_u32(ip + 1);
        if (index >= (uint32_t)func->const_count || vtype(func->constants[index]) != kTypeFunctionInfo) return opt_fail(b, "closure-const");
        const sv_func_t *child = (const sv_func_t *)vptr(func->constants[index]);
        for (int u = 0; u < child->upvalue_count; u++) {
          const sv_upval_desc_t *desc = &child->upval_descs[u];
          if (desc->is_local && !desc->by_value && desc->index < width) opt_bitset_insert(&captured, desc->index);
        }
        break;
      }
      case OP_STR_APPEND_LOCAL: case OP_STR_ALC_SNAPSHOT: case OP_STR_FLUSH_LOCAL: {
        uint32_t slot = sv_get_u16(ip + 1);
        if (slot >= width) return opt_fail(b, "builder-slot");
        opt_bitset_insert(&b->builders, slot);
        break;
      }
      case OP_SPECIAL_OBJ:
        if (ip[1] == SV_SPECIAL_ARGUMENTS && !func->is_strict && b->params) return opt_fail(b, "mapped-arguments");
        break;
      case OP_PUT_CONST: {
        uint32_t index = sv_get_u32(ip + 1);
        if (index < (uint32_t)func->const_count) opt_bitset_insert(&b->patched_constants, index);
        break;
      }
      default: break;
    }
  }
  opt_bitset_union_with(b->inline_call ? &b->memory : &b->boxable, &captured);
  return true;
}

static inline bool opt_in_memory(const opt_builder_t *b, uint32_t tracked) {
  return tracked < b->params + b->locals && opt_bitset_contains(&b->memory, tracked);
}

static inline bool opt_boxable(const opt_builder_t *b, uint32_t tracked) {
  return opt_bitset_has(&b->boxable, tracked);
}

static void opt_compute_rpo(opt_builder_t *b) {
  uint32_t n = b->block_count;
  uint32_t *stack = opt_arena_new(&b->arena, uint32_t, n);
  uint32_t *next = opt_arena_new(&b->arena, uint32_t, n);
  uint32_t *post = opt_arena_new(&b->arena, uint32_t, n);
  bool *seen = opt_arena_new(&b->arena, bool, n);
  uint32_t top = 0, count = 0;
  stack[top++] = 0;
  seen[0] = true;
  while (top) {
    uint32_t bi = stack[top - 1];
    opt_bblock_t *block = &b->blocks[bi];
    if (next[bi] < block->succ_count + (block->handler != UINT32_MAX)) {
      uint32_t s = next[bi] < block->succ_count ? block->succ[next[bi]] : block->handler;
      next[bi]++;
      if (!seen[s]) {
        seen[s] = true;
        stack[top++] = s;
      }
      continue;
    }
    post[count++] = bi;
    top--;
  }
  b->rpo = opt_arena_new(&b->arena, uint32_t, count + 1);
  b->rpo_count = count;
  for (uint32_t i = 0; i < count; i++) {
    b->rpo[i] = post[count - 1 - i];
    b->blocks[b->rpo[i]].rpo = (int32_t)i;
  }
}

static void opt_find_loop_captures(opt_builder_t *b, const int32_t *loop_of_header) {
  bool *seen = opt_arena_new(&b->arena, bool, b->block_count + 1);
  uint32_t *pending = opt_arena_new(&b->arena, uint32_t, b->block_count + 1);
  for (uint32_t i = 0; i < b->ins_count; i++) {
    if (opt_op_at(b, b->ins_off[i]) != OP_CLOSURE) continue;
    const sv_func_t *child = (const sv_func_t *)vptr(b->func->constants[sv_get_u32(b->func->code + b->ins_off[i] + 1)]);
    uint32_t from = b->block_of_ins[i];
    for (uint32_t l = 0; l < b->loop_count; l++) {
      if (!opt_bitset_contains(&b->loops[l].blocks, from)) continue;
      for (int u = 0; u < child->upvalue_count; u++)
        if (child->upval_descs[u].is_local && !child->upval_descs[u].by_value && opt_boxable(b, child->upval_descs[u].index))
          opt_bitset_insert(&b->loops[l].captures, child->upval_descs[u].index);
    }
    memset(seen, 0, (b->block_count + 1) * sizeof *seen);
    uint32_t top = 0;
    pending[top++] = from;
    seen[from] = true;
    while (top) {
      uint32_t bi = pending[--top];
      for (uint32_t k = 0; k < b->blocks[bi].succ_count; k++) {
        uint32_t next = b->blocks[bi].succ[k];
        if (b->blocks[next].is_header && loop_of_header[next] >= 0) {
          opt_loop_t *loop = &b->loops[loop_of_header[next]];
          for (int u = 0; u < child->upvalue_count; u++)
            if (child->upval_descs[u].is_local && !child->upval_descs[u].by_value && opt_boxable(b, child->upval_descs[u].index))
              opt_bitset_insert(&loop->entry_boxes, child->upval_descs[u].index);
        }
        if (!seen[next]) {
          seen[next] = true;
          pending[top++] = next;
        }
      }
    }
  }
}

static void opt_find_loops(opt_builder_t *b) {
  b->loops = opt_arena_new(&b->arena, opt_loop_t, b->block_count + 1);
  int32_t *loop_of_header = opt_arena_alloc(&b->arena, b->block_count * sizeof *loop_of_header);
  for (uint32_t i = 0; i < b->block_count; i++) loop_of_header[i] = -1;
  OPT_VEC(uint32_t) pending = {0};

  for (uint32_t r = 0; r < b->rpo_count; r++) {
    uint32_t source = b->rpo[r];
    opt_bblock_t *block = &b->blocks[source];
    for (uint32_t s = 0; s < block->succ_count; s++) {
      uint32_t header = block->succ[s];
      if (b->blocks[header].rpo < 0 || b->blocks[header].rpo > block->rpo) continue;
      if (loop_of_header[header] < 0) {
        loop_of_header[header] = (int32_t)b->loop_count;
        b->loops[b->loop_count++] = (opt_loop_t){
          .header = header,
          .blocks = opt_bitset_new(&b->arena, b->block_count),
          .assigned = opt_bitset_new(&b->arena, b->tracked),
          .captures = opt_bitset_new(&b->arena, b->tracked),
          .entry_boxes = opt_bitset_new(&b->arena, b->tracked),
        };
        opt_bitset_insert(&b->loops[b->loop_count - 1].blocks, header);
        b->blocks[header].is_header = true;
        b->blocks[header].heads = b->loop_count - 1;
      }
      opt_loop_t *loop = &b->loops[loop_of_header[header]];
      pending.len = 0;
      opt_vec_push(&b->arena, &pending, source);
      while (pending.len) {
        uint32_t bi = pending.items[--pending.len];
        if (opt_bitset_contains(&loop->blocks, bi) || b->blocks[bi].rpo < 0) continue;
        opt_bitset_insert(&loop->blocks, bi);
        for (uint32_t p = 0; p < b->blocks[bi].preds.len; p++) opt_vec_push(&b->arena, &pending, b->blocks[bi].preds.items[p]);
      }
    }
  }

  if (!opt_bitset_is_empty(&b->boxable)) opt_find_loop_captures(b, loop_of_header);
  b->in_loop = opt_arena_new(&b->arena, bool, b->block_count + 1);
  for (uint32_t l = 0; l < b->loop_count; l++)
    opt_bitset_for_each(&b->loops[l].blocks, bi) b->in_loop[bi] = true;
  for (uint32_t l = 0; l < b->loop_count; l++) {
    opt_loop_t *loop = &b->loops[l];
    bool all = false;
    for (uint32_t other = 0; other < b->loop_count; other++)
      all |= other != l && opt_bitset_contains(&loop->blocks, b->loops[other].header);
    for (uint32_t k = 0; k < b->stack; k++) opt_bitset_insert(&loop->assigned, opt_stack_index(b, k));
    if (all)
      for (uint32_t j = 0; j < b->locals; j++) opt_bitset_insert(&loop->assigned, b->params + j);
    opt_bitset_for_each(&loop->blocks, bi) {
      for (uint32_t i = b->blocks[bi].first; i <= b->blocks[bi].last; i++) {
        bool use, def;
        uint32_t tracked = opt_access(b, b->ins_off[i], &use, &def);
        if (def && tracked < b->params + b->locals) opt_bitset_insert(&loop->assigned, tracked);
      }
    }
  }
}

static void opt_live_transfer(const opt_builder_t *b, uint32_t off, opt_bitset_t *live) {
  if (opt_op_at(b, off) == OP_CLOSURE) {
    const sv_func_t *child = (const sv_func_t *)vptr(b->func->constants[sv_get_u32(b->func->code + off + 1)]);
    uint32_t width = b->params + b->locals;
    for (int u = 0; u < child->upvalue_count; u++) {
      const sv_upval_desc_t *desc = &child->upval_descs[u];
      if (desc->is_local && desc->index < width && (desc->by_value || opt_boxable(b, desc->index))) opt_bitset_insert(live, desc->index);
    }
    return;
  }
  bool use, def;
  uint32_t tracked = opt_access(b, off, &use, &def);
  if (tracked >= b->params + b->locals) return;
  bool boxable = def && opt_boxable(b, tracked);
  if (def && !boxable) opt_bitset_remove(live, tracked);
  if (use || boxable) opt_bitset_insert(live, tracked);
}

static void opt_compute_liveness(opt_builder_t *b) {
  uint32_t width = b->params + b->locals;
  opt_bitset_t *block_in = opt_arena_new(&b->arena, opt_bitset_t, b->block_count);
  for (uint32_t bi = 0; bi < b->block_count; bi++) block_in[bi] = opt_bitset_new(&b->arena, width);
  opt_bitset_t live = opt_bitset_new(&b->arena, width);
  for (bool changed = true; changed;) {
    changed = false;
    for (uint32_t r = b->rpo_count; r-- > 0;) {
      uint32_t bi = b->rpo[r];
      opt_bblock_t *block = &b->blocks[bi];
      opt_bitset_clear(&live);
      for (uint32_t s = 0; s < block->succ_count; s++) opt_bitset_union_with(&live, &block_in[block->succ[s]]);
      for (uint32_t i = block->last + 1; i-- > block->first;) {
        opt_live_transfer(b, b->ins_off[i], &live);
        if (block->handler != UINT32_MAX) opt_bitset_union_with(&live, &block_in[block->handler]);
      }
      if (!opt_bitset_equal(&live, &block_in[bi])) {
        opt_bitset_copy(&block_in[bi], &live);
        changed = true;
      }
    }
  }
  b->live_in = opt_arena_new(&b->arena, opt_bitset_t, b->ins_count);
  for (uint32_t bi = 0; bi < b->block_count; bi++) {
    opt_bblock_t *block = &b->blocks[bi];
    opt_bitset_clear(&live);
    for (uint32_t s = 0; s < block->succ_count; s++) opt_bitset_union_with(&live, &block_in[block->succ[s]]);
    for (uint32_t i = block->last + 1; i-- > block->first;) {
      opt_live_transfer(b, b->ins_off[i], &live);
      if (block->handler != UINT32_MAX) opt_bitset_union_with(&live, &block_in[block->handler]);
      b->live_in[i] = opt_bitset_clone(&b->arena, &live);
    }
  }
}

static ir_node_id_t opt_constant(opt_builder_t *b, uint64_t bits);

static uint64_t opt_completion_bits(uint32_t kind) {
  return tov((double)kind);
}

static opt_frame_t opt_frame_new(opt_builder_t *b) {
  opt_frame_t frame = {.slots = opt_arena_new(&b->arena, opt_slot_t, b->tracked + 1 + 2 * b->finally_count), .this_value = IR_NONE};
  for (uint32_t t = 0; t < b->tracked; t++) frame.slots[t] = (opt_slot_t){.node = IR_NONE};
  for (uint32_t i = 0; i < b->finally_count; i++) {
    frame.slots[b->tracked + 1 + 2 * i] = (opt_slot_t){.node = opt_constant(b, opt_completion_bits(0))};
    frame.slots[b->tracked + 2 + 2 * i] = (opt_slot_t){.node = opt_constant(b, IR_VALUE_UNDEFINED)};
  }
  return frame;
}

static opt_frame_t opt_frame_clone(opt_builder_t *b, const opt_frame_t *frame) {
  opt_frame_t copy = {.slots = opt_arena_dup(&b->arena, frame->slots, (b->tracked + 1 + 2 * b->finally_count) * sizeof *frame->slots), .sp = frame->sp, .this_value = frame->this_value};
  return copy;
}

static ir_node_id_t opt_typed_constant(opt_builder_t *b, uint64_t bits, opt_repr_t repr) {
  bool inserted;
  uint64_t *entry = opt_map_entry(&b->ctx->constants, (uint64_t[]){bits, repr}, &inserted);
  if (!inserted) return (ir_node_id_t)*entry;
  ir_op_t op = ir_op(IR_OP_CONSTANT);
  op.bits = bits;
  ir_node_id_t node = ir_add_node(b->graph, op, NULL, 0, repr, 0);
  *entry = node;
  return node;
}

static inline ir_node_id_t opt_constant(opt_builder_t *b, uint64_t bits) {
  return opt_typed_constant(b, bits, OPT_REPR_TAGGED);
}

static inline ir_node_id_t opt_int32_constant(opt_builder_t *b, int32_t value) {
  return opt_typed_constant(b, (uint32_t)value, OPT_REPR_INT32);
}

static ir_node_id_t opt_emit(opt_builder_t *b, ir_op_t op, const ir_node_id_t *inputs, uint32_t count, opt_repr_t repr) {
  ir_node_id_t node = ir_add_node(b->graph, op, inputs, count, repr, b->pc);
  opt_vec_push(&b->graph->arena, &ir_block(b->graph, b->block)->body, node);
  return node;
}

static ir_node_id_t opt_emit1(opt_builder_t *b, ir_opcode_t code, ir_node_id_t input, opt_repr_t repr) {
  return opt_emit(b, ir_op(code), &input, 1, repr);
}

static ir_node_id_t opt_set_control(opt_builder_t *b, ir_op_t op, const ir_node_id_t *inputs, uint32_t count) {
  ir_node_id_t node = ir_add_node(b->graph, op, inputs, count, OPT_REPR_NONE, b->pc);
  ir_block(b->graph, b->block)->control = node;
  return node;
}

static ir_node_id_t opt_append_to_finished(opt_builder_t *b, ir_block_id_t block, ir_op_t op, const ir_node_id_t *inputs, uint32_t count, opt_repr_t repr) {
  uint32_t pc = ir_control(b->graph, block)->bc_off;
  ir_node_id_t node = ir_add_node(b->graph, op, inputs, count, repr, pc);
  opt_vec_push(&b->graph->arena, &ir_block(b->graph, block)->body, node);
  return node;
}

static ir_node_id_t opt_tagged_at_end_of(opt_builder_t *b, ir_block_id_t block, ir_node_id_t value) {
  const ir_node_t *node = ir_node(b->graph, value);
  if (node->repr != OPT_REPR_INT32) return value;
  uint64_t bits;
  if (ir_constant_value(b->graph, value, &bits)) return opt_constant(b, ir_value_int32((int32_t)(uint32_t)bits));
  if (node->op.code == IR_OP_CHECK_INT32) return node->inputs.items[0];
  if (block == b->block) return opt_emit1(b, IR_OP_BOX_INT32, value, OPT_REPR_TAGGED);
  return opt_append_to_finished(b, block, ir_op(IR_OP_BOX_INT32), &value, 1, OPT_REPR_TAGGED);
}

static inline ir_node_id_t opt_tagged(opt_builder_t *b, ir_node_id_t value) {
  return opt_tagged_at_end_of(b, b->block, value);
}

static ir_node_id_t opt_add_phi(opt_builder_t *b, uint32_t tracked, const ir_node_id_t *inputs, uint32_t count) {
  ir_op_t op = ir_op(IR_OP_PHI);
  op.slot = opt_tracked_slot(b, tracked);
  opt_repr_t repr = count && ir_node(b->graph, inputs[0])->repr == OPT_REPR_POINTER ? OPT_REPR_POINTER : OPT_REPR_TAGGED;
  ir_node_id_t phi = ir_add_node(b->graph, op, inputs, count, repr, b->pc);
  opt_vec_push(&b->graph->arena, &ir_block(b->graph, b->block)->phis, phi);
  return phi;
}

static ir_node_id_t opt_add_completion_phi(opt_builder_t *b, uint32_t index, const ir_node_id_t *inputs, uint32_t count) {
  ir_op_t op = ir_op(IR_OP_PHI);
  op.slot = OPT_COMPLETION_SLOT + 2 * b->finallies[index / 2].off + (index & 1);
  ir_node_id_t phi = ir_add_node(b->graph, op, inputs, count, OPT_REPR_TAGGED, b->pc);
  opt_vec_push(&b->graph->arena, &ir_block(b->graph, b->block)->phis, phi);
  return phi;
}

static ir_node_id_t opt_add_this_phi(opt_builder_t *b, const ir_node_id_t *inputs, uint32_t count) {
  ir_op_t op = ir_op(IR_OP_PHI);
  op.slot = OPT_THIS_SLOT;
  ir_node_id_t phi = ir_add_node(b->graph, op, inputs, count, OPT_REPR_TAGGED, b->pc);
  opt_vec_push(&b->graph->arena, &ir_block(b->graph, b->block)->phis, phi);
  return phi;
}

static ir_node_id_t opt_root_this(opt_builder_t *b) {
  if (b->frame.this_value != IR_NONE) return b->frame.this_value;
  ir_op_t load = ir_op(IR_OP_LOAD_SLOT);
  load.slot = OPT_THIS_SLOT;
  ir_node_id_t value = opt_emit(b, load, NULL, 0, OPT_REPR_TAGGED);
  if (b->uses_this) b->frame.this_value = value;
  return value;
}

uint32_t opt_learned_shapes(const sv_func_t *func, uint32_t bc_off) {
  if (bc_off >= (uint32_t)func->code_len || !func->ic_slots) return 0;
  sv_op_t op = (sv_op_t)func->code[bc_off];
  if (op != OP_GET_FIELD && op != OP_GET_FIELD2 && op != OP_PUT_FIELD) return 0;
  uint16_t ic_index = sv_get_u16(func->code + bc_off + 5);
  if (ic_index >= func->ic_count) return 0;
  const sv_ic_entry_t *ic = &func->ic_slots[ic_index];
  uint32_t count = ic->cached_shape ? 1 : 0;
  if (op == OP_PUT_FIELD) {
    if (ic->guard.add.from_shape) count++;
    const sv_pf_poly_t *poly = (ic->shape_ref_mask & SV_IC_HAS_PUT_POLY) ? ic->put_poly : NULL;
    for (uint32_t i = 0; poly && i < SV_GF_POLY_WAYS; i++) count += poly->entries[i].shape != NULL;
    return count;
  }
  const sv_gf_poly_t *poly = (ic->shape_ref_mask & SV_IC_HAS_GET_POLY) ? ic->guard.get.poly : NULL;
  for (uint32_t i = 0; poly && i < SV_GF_POLY_WAYS; i++)
    count += poly->entries[i].shape && poly->entries[i].shape != SV_IC_SHAPE_SLOT_DEAD;
  return count;
}

static bool opt_may_speculate(const opt_builder_t *b, opt_exit_kind_t kind) {
  for (uint32_t i = 0; i < b->exit_site_count; i++) {
    const ir_exit_site_t *site = &b->exit_sites[i];
    if (site->bc_off != b->pc || site->kind != kind) continue;
    if (!site->slot || (kind == OPT_EXIT_BAD_SHAPE && opt_learned_shapes(b->func, b->pc) < site->slot)) return false;
  }
  return true;
}

static ir_node_id_t opt_frame_value(const opt_builder_t *b, ir_node_id_t value) {
  const ir_node_t *node = ir_node(b->graph, value);
  bool box = node->op.code == IR_OP_BOX_INT32 || node->op.code == IR_OP_BOX_FLOAT64 || node->op.code == IR_OP_BOX_BOOL;
  return box ? node->inputs.items[0] : value;
}

static void opt_push_completion(opt_builder_t *b, ir_frame_state_t *state, uint32_t finally_off) {
  int32_t index = opt_finally_index(b, (int32_t)finally_off);
  if (index < 0) return;
  ir_node_id_t kind = b->frame.slots[b->tracked + 1 + 2 * index].node, value = b->frame.slots[b->tracked + 2 + 2 * index].node;
  uint64_t bits;
  if (ir_constant_value(b->graph, kind, &bits) && ir_node(b->graph, kind)->repr == OPT_REPR_TAGGED && bits == opt_completion_bits(0)) return;
  opt_vec_push(&b->arena, &state->values, ((ir_slot_value_t){OPT_COMPLETION_SLOT + 2 * finally_off, opt_frame_value(b, kind)}));
  opt_vec_push(&b->arena, &state->values, ((ir_slot_value_t){OPT_COMPLETION_SLOT + 2 * finally_off + 1, opt_frame_value(b, value)}));
}

static ir_frame_state_t opt_frame_state_at(opt_builder_t *b) {
  const opt_bitset_t *live = &b->live_in[b->ins];
  ir_frame_state_t state = {
    .func = b->func,
    .bc_off = b->pc,
    .mode = OPT_RESUME_AT,
    .dst = OPT_INVALID_SLOT,
    .values = b->state_values,
    .in_frame = b->state_in_frame,
    .parent = IR_NONE,
  };
  state.values.len = 0;
  state.in_frame.len = 0;
  bool is_virtual = b->inline_call != NULL;
  for (uint32_t k = 0; k < b->frame.sp; k++) {
    ir_node_id_t node = b->frame.slots[opt_stack_index(b, k)].node;
    if (node == IR_NONE) opt_vec_push(&b->arena, &state.in_frame, opt_stack_slot(k));
    else {
      ir_slot_value_t value = {opt_stack_slot(k), opt_frame_value(b, node)};
      opt_vec_push(&b->arena, &state.values, value);
    }
  }
  for (uint32_t j = 0; j < b->locals; j++) {
    if (!opt_bitset_contains(live, b->params + j) && !b->frame.slots[b->params + j].boxed) continue;
    ir_node_id_t node = b->frame.slots[b->params + j].node;
    if (node == IR_NONE) opt_vec_push(&b->arena, &state.in_frame, opt_local_slot(&b->layout, j));
    else {
      ir_slot_value_t value = {opt_local_slot(&b->layout, j), opt_frame_value(b, node)};
      opt_vec_push(&b->arena, &state.values, value);
    }
  }
  for (uint32_t p = 0; p < b->params; p++) {
    if (!opt_bitset_contains(live, p) && !b->frame.slots[p].boxed) continue;
    const opt_slot_t *slot = &b->frame.slots[p];
    if (slot->node == IR_NONE || (slot->in_sync && !is_virtual)) opt_vec_push(&b->arena, &state.in_frame, opt_param_slot(&b->layout, p));
    else {
      ir_slot_value_t value = {opt_param_slot(&b->layout, p), opt_frame_value(b, slot->node)};
      opt_vec_push(&b->arena, &state.values, value);
    }
  }
  if (b->finally_count && !is_virtual) {
    for (int32_t s = opt_try_state(b, b->pc); s >= 0; s = opt_try_parent(b, s))
      if (opt_op_at(b, (uint32_t)s) == OP_FINALLY) opt_push_completion(b, &state, (uint32_t)s);
    if (opt_op_at(b, b->pc) == OP_FINALLY) opt_push_completion(b, &state, b->pc);
  }
  if (is_virtual) {
    const opt_inline_t *call = b->inline_call;
    opt_vec_push(&b->arena, &state.values, ((ir_slot_value_t){OPT_THIS_SLOT, call->receiver}));
    opt_vec_push(&b->arena, &state.values, ((ir_slot_value_t){OPT_CALLEE_SLOT, call->closure}));
    for (uint32_t i = b->params; b->virtual_arguments && i < call->argc; i++)
      opt_vec_push(&b->arena, &state.values, ((ir_slot_value_t){opt_param_slot(&b->layout, i), opt_frame_value(b, call->args[i])}));
    state.parent = call->caller_state;
    state.passed_argument_count = call->argc;
    state.construct = call->construct;
  }
  b->state_values = state.values;
  b->state_in_frame = state.in_frame;
  return state;
}

static ir_frame_state_id_t opt_new_frame_state(opt_builder_t *b) {
  ir_frame_state_t state = opt_frame_state_at(b);
  return ir_add_frame_state(b->graph, &state);
}

static ir_frame_state_id_t opt_after_call_state(opt_builder_t *b, uint32_t operands, bool tail) {
  uint32_t ins = b->ins, pc = b->pc, sp = b->frame.sp;
  b->frame.sp = tail ? 0 : sp - operands;
  if (!tail) {
    b->ins = ins + 1;
    b->pc = b->ins_off[ins + 1];
  }
  ir_frame_state_t state = opt_frame_state_at(b);
  b->ins = ins;
  b->pc = pc;
  b->frame.sp = sp;
  if (tail) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < state.values.len; i++)
      if (state.values.items[i].slot == OPT_THIS_SLOT || state.values.items[i].slot == OPT_CALLEE_SLOT)
        state.values.items[kept++] = state.values.items[i];
    state.values.len = kept;
    state.in_frame.len = 0;
    state.bc_off = pc;
  }
  state.mode = tail ? OPT_RESUME_RETURN : OPT_RESUME_AFTER;
  state.dst = tail ? OPT_INVALID_SLOT : opt_stack_slot(sp - operands);
  state.call_off = pc;
  return ir_add_frame_state(b->graph, &state);
}

static ir_frame_state_id_t opt_call_position(opt_builder_t *b) {
  ir_frame_state_t state = {
    .func = b->func,
    .bc_off = b->pc,
    .mode = OPT_RESUME_AT,
    .dst = OPT_INVALID_SLOT,
    .parent = b->inline_call ? b->inline_call->caller_position : IR_NONE,
  };
  return ir_add_frame_state(b->graph, &state);
}

static void opt_mark_catch(opt_builder_t *b, ir_node_id_t node) {
  if (b->building_entry) return;
  const opt_builder_t *root = b;
  for (; root->inline_call; root = root->inline_call->caller) {
    if (opt_catch_state(root, root->pc) < 0) continue;
    opt_fail(b, "inline-handler");
    return;
  }
  int32_t state = opt_catch_state(root, root->pc);
  if (state < 0 || !b->graph->handler_at) return;
  ir_node_t *data = ir_node(b->graph, node);
  if (!b->inline_call && data->op.code != IR_OP_GENERIC) data->frame_state = opt_new_frame_state(b);
  else if (b->inline_call && data->frame_state == IR_NONE) data->frame_state = opt_call_position(b);
  data->handler = (uint32_t)b->graph->handler_at[root->pc];
}

static ir_frame_state_id_t opt_eager_frame_state(opt_builder_t *b) {
  if (b->eager == IR_NONE) b->eager = opt_new_frame_state(b);
  return b->eager;
}

static ir_node_id_t opt_emit_checked(opt_builder_t *b, ir_op_t op, const ir_node_id_t *inputs, uint32_t count, opt_repr_t repr) {
  int refined = ir_op_refined_input(&op);
  if (repr == OPT_REPR_NONE && refined >= 0) repr = ir_node(b->graph, inputs[refined])->repr;
  ir_frame_state_id_t state = opt_eager_frame_state(b);
  ir_node_id_t node = opt_emit(b, op, inputs, count, repr);
  ir_node(b->graph, node)->frame_state = state;
  return node;
}

static ir_node_id_t opt_read_tracked(opt_builder_t *b, uint32_t tracked) {
  opt_slot_t *slot = &b->frame.slots[tracked];
  if (slot->boxed) return opt_emit1(b, IR_OP_LOAD_ENVIRONMENT_BINDING, slot->node, OPT_REPR_TAGGED);
  if (slot->node != IR_NONE) return slot->node;
  ir_op_t op = ir_op(IR_OP_LOAD_SLOT);
  op.slot = opt_tracked_slot(b, tracked);
  slot->node = opt_emit(b, op, NULL, 0, OPT_REPR_TAGGED);
  slot->in_sync = true;
  if (opt_in_memory(b, tracked)) b->graph->uses_frame = true;
  return slot->node;
}

static ir_node_id_t opt_tagged(opt_builder_t *b, ir_node_id_t value);

static void opt_sync_tracked(opt_builder_t *b, uint32_t tracked) {
  opt_slot_t *slot = &b->frame.slots[tracked];
  if (slot->node == IR_NONE || slot->in_sync || slot->boxed) return;
  ir_op_t op = ir_op(IR_OP_STORE_SLOT);
  op.slot = opt_tracked_slot(b, tracked);
  ir_node_id_t value = opt_tagged(b, slot->node);
  opt_emit(b, op, &value, 1, OPT_REPR_NONE);
  slot->in_sync = true;
  b->graph->uses_frame = true;
}

static void opt_write_tracked(opt_builder_t *b, uint32_t tracked, ir_node_id_t value) {
  opt_slot_t *slot = &b->frame.slots[tracked];
  if (slot->boxed) {
    opt_emit(b, ir_op(IR_OP_STORE_ENVIRONMENT_BINDING), (ir_node_id_t[]){slot->node, opt_tagged(b, value)}, 2, OPT_REPR_NONE);
    return;
  }
  if (slot->node == value && slot->in_sync) return;
  *slot = (opt_slot_t){.node = value};
  if (opt_in_memory(b, tracked)) opt_sync_tracked(b, tracked);
}

static ir_node_id_t opt_runtime_call(opt_builder_t *b, uintptr_t helper, uintptr_t extra, const ir_node_id_t *inputs, uint32_t count, bool allocates_only, bool needs_frame);

static ir_node_id_t opt_new_box(opt_builder_t *b, ir_node_id_t value) {
  ir_node_id_t inputs[2] = {opt_tagged(b, value), opt_constant(b, IR_VALUE_UNDEFINED)};
  ir_node_id_t box = opt_runtime_call(b, (uintptr_t)opt_rt_new_box, 0, inputs, 2, true, false);
  ir_node(b->graph, box)->repr = OPT_REPR_POINTER;
  return box;
}

static void opt_box_slot(opt_builder_t *b, uint32_t tracked) {
  opt_slot_t *slot = &b->frame.slots[tracked];
  if (slot->boxed) return;
  *slot = (opt_slot_t){.node = opt_new_box(b, opt_read_tracked(b, tracked)), .boxed = true};
}

static inline ir_node_id_t opt_peek(opt_builder_t *b, uint32_t depth) {
  return b->frame.slots[opt_stack_index(b, b->frame.sp - 1 - depth)].node;
}

static inline void opt_poke(opt_builder_t *b, uint32_t depth, ir_node_id_t value) {
  b->frame.slots[opt_stack_index(b, b->frame.sp - 1 - depth)] = (opt_slot_t){.node = value};
}

static inline void opt_push(opt_builder_t *b, ir_node_id_t value) {
  b->frame.slots[opt_stack_index(b, b->frame.sp++)] = (opt_slot_t){.node = value};
}

static inline void opt_drop(opt_builder_t *b, uint32_t count) {
  b->frame.sp -= count;
}

static ir_block_id_t opt_ir_block(opt_builder_t *b, uint32_t bi) {
  if (b->blocks[bi].ir == IR_NONE) b->blocks[bi].ir = ir_add_block(b->graph);
  return b->blocks[bi].ir;
}

static ir_block_id_t opt_start_new_block(opt_builder_t *b, ir_block_id_t predecessor) {
  ir_block_id_t block = ir_add_block(b->graph);
  if (predecessor != IR_NONE) opt_vec_push(&b->graph->arena, &ir_block(b->graph, block)->predecessors, predecessor);
  opt_vec_push(&b->graph->arena, &b->ctx->order, block);
  b->block = block;
  return block;
}

static bool opt_slot_in_memory(const opt_slot_t *slot) {
  return slot->node == IR_NONE || slot->in_sync;
}

static bool opt_reconcile_back_edge(opt_builder_t *b, uint32_t target) {
  opt_bblock_t *header = &b->blocks[target];
  const opt_frame_t *header_frame = &header->header_frame;
  const opt_bitset_t *live = &b->live_in[header->first];
  if (b->frame.sp != header_frame->sp) return opt_fail(b, "depth");
  for (uint32_t t = 0; t < b->tracked; t++) {
    bool is_stack = t >= b->params + b->locals;
    if (header_frame->slots[t].boxed) {
      opt_box_slot(b, t);
      ir_node_id_t box = b->frame.slots[t].node, phi = header_frame->slots[t].node;
      if (ir_node_vec_contains(&ir_block(b->graph, header->ir)->phis, phi)) ir_push_input(b->graph, phi, box);
      else if (phi != box) return opt_fail(b, "loop-box-changed");
      continue;
    }
    if (b->frame.slots[t].boxed) return opt_fail(b, "loop-boxed");
    if (is_stack ? t - b->params - b->locals >= header_frame->sp : !opt_bitset_contains(live, t)) continue;
    if (opt_in_memory(b, t)) continue;
    const opt_slot_t *at_header = &header_frame->slots[t];
    const opt_slot_t *here = &b->frame.slots[t];
    if (at_header->node != IR_NONE && ir_node_vec_contains(&ir_block(b->graph, header->ir)->phis, at_header->node)) {
      ir_node_id_t value = here->node;
      if (value == IR_NONE) value = opt_read_tracked(b, t);
      ir_push_input(b->graph, at_header->node, opt_tagged(b, value));
      continue;
    }
    if (at_header->node != IR_NONE) {
      if (here->node != at_header->node) return opt_fail(b, "loop-invariant-changed");
      continue;
    }
    if (!opt_slot_in_memory(here)) return opt_fail(b, "loop-invariant-changed");
  }
  for (uint32_t j = 0; j < 2 * b->finally_count; j++) {
    uint32_t t = b->tracked + 1 + j;
    ir_node_id_t at_header = header_frame->slots[t].node, here = b->frame.slots[t].node;
    if (ir_node_vec_contains(&ir_block(b->graph, header->ir)->phis, at_header)) ir_push_input(b->graph, at_header, opt_tagged(b, here));
    else if (here != at_header) return opt_fail(b, "loop-completion-changed");
  }
  ir_node_id_t this_phi = header_frame->this_value;
  if (this_phi != IR_NONE && ir_node_vec_contains(&ir_block(b->graph, header->ir)->phis, this_phi))
    ir_push_input(b->graph, this_phi, opt_root_this(b));
  else if (this_phi != b->frame.this_value) return opt_fail(b, "loop-this-changed");
  return true;
}

static bool opt_end_with_jump(opt_builder_t *b, uint32_t target) {
  ir_block_id_t target_block = opt_ir_block(b, target);
  opt_bblock_t *bblock = &b->blocks[target];
  if (bblock->started) {
    if (!bblock->is_header) return opt_fail(b, "edge-to-started-block");
    if (!opt_reconcile_back_edge(b, target)) return false;
    opt_vec_push(&b->graph->arena, &ir_block(b->graph, target_block)->predecessors, b->block);
  } else {
    opt_incoming_t incoming = {.pred = b->block, .frame = opt_frame_clone(b, &b->frame)};
    opt_vec_push(&b->arena, &bblock->incoming, incoming);
  }
  ir_op_t op = ir_op(IR_OP_JUMP);
  op.target = target_block;
  opt_set_control(b, op, NULL, 0);
  return true;
}

static ir_block_id_t opt_edge_block(opt_builder_t *b, ir_block_id_t source) {
  ir_block_id_t block = ir_add_block(b->graph);
  opt_vec_push(&b->graph->arena, &ir_block(b->graph, block)->predecessors, source);
  opt_vec_push(&b->graph->arena, &b->ctx->order, block);
  return block;
}

static bool opt_end_with_branch(opt_builder_t *b, ir_condition_t condition, const ir_node_id_t *inputs, uint32_t count, uint32_t if_true, uint32_t if_false) {
  ir_block_id_t source = b->block;
  opt_frame_t frame = opt_frame_clone(b, &b->frame);
  ir_block_id_t true_edge = opt_edge_block(b, source);
  ir_block_id_t false_edge = opt_edge_block(b, source);
  ir_op_t op = ir_op(IR_OP_BRANCH);
  op.branch.condition = condition;
  op.branch.if_true = true_edge;
  op.branch.if_false = false_edge;
  opt_set_control(b, op, inputs, count);
  ir_block_id_t edges[2] = {true_edge, false_edge};
  uint32_t targets[2] = {if_true, if_false};
  for (uint32_t i = 0; i < 2; i++) {
    b->block = edges[i];
    b->frame = opt_frame_clone(b, &frame);
    if (!opt_end_with_jump(b, targets[i])) return false;
  }
  return true;
}

static bool opt_end_with_truthiness_branch(opt_builder_t *b, ir_node_id_t value, uint32_t if_true, uint32_t if_false) {
  const ir_node_t *node = ir_node(b->graph, value);
  if (node->op.code == IR_OP_BOX_BOOL) {
    ir_node_id_t boolean = node->inputs.items[0];
    return opt_end_with_branch(b, ir_cond(IR_COND_BOOL), &boolean, 1, if_true, if_false);
  }
  if (node->repr == OPT_REPR_INT32) {
    ir_condition_t condition = ir_cond(IR_COND_INT32);
    condition.comparison = IR_CMP_STRICTLY_INEQUALS;
    ir_node_id_t inputs[2] = {value, opt_int32_constant(b, 0)};
    return opt_end_with_branch(b, condition, inputs, 2, if_true, if_false);
  }
  value = opt_tagged(b, value);
  ir_block_id_t source = b->block;
  opt_frame_t frame = opt_frame_clone(b, &b->frame);
  ir_block_id_t true_edge = opt_edge_block(b, source);
  ir_block_id_t false_edge = opt_edge_block(b, source);
  ir_block_id_t fallback = opt_edge_block(b, source);
  ir_op_t op = ir_op(IR_OP_BRANCH_TRUTHY);
  op.truthy.if_true = true_edge;
  op.truthy.if_false = false_edge;
  op.truthy.fallback = fallback;
  opt_set_control(b, op, &value, 1);
  ir_block_id_t edges[2] = {true_edge, false_edge};
  uint32_t targets[2] = {if_true, if_false};
  for (uint32_t i = 0; i < 2; i++) {
    b->block = edges[i];
    b->frame = opt_frame_clone(b, &frame);
    if (!opt_end_with_jump(b, targets[i])) return false;
  }
  b->block = fallback;
  b->frame = frame;
  ir_node_id_t boolean = opt_emit1(b, IR_OP_TO_BOOLEAN, value, OPT_REPR_BOOL);
  return opt_end_with_branch(b, ir_cond(IR_COND_BOOL), &boolean, 1, if_true, if_false);
}

static bool opt_element_key_exited(const opt_builder_t *b) {
  for (uint32_t i = 0; i < b->exit_site_count; i++) {
    const ir_exit_site_t *site = &b->exit_sites[i];
    if (site->kind != OPT_EXIT_NOT_INT32 || site->bc_off >= (uint32_t)b->func->code_len) continue;
    sv_op_t op = opt_op_at(b, site->bc_off);
    if (op == OP_GET_ELEM || op == OP_PUT_ELEM) return true;
  }
  return false;
}

static opt_flow_t opt_end_with_exit(opt_builder_t *b, opt_exit_kind_t kind) {
  if (!opt_may_speculate(b, kind)) return OPT_FLOW_GENERIC;
  ir_frame_state_id_t state = opt_eager_frame_state(b);
  ir_op_t op = ir_op(IR_OP_EXIT);
  op.exit_kind = kind;
  ir_node_id_t node = opt_set_control(b, op, NULL, 0);
  ir_node(b->graph, node)->frame_state = state;
  return OPT_FLOW_ENDED;
}

static ir_node_id_t opt_box_at_end_of(opt_builder_t *b, ir_block_id_t block, uint32_t tracked, ir_node_id_t value) {
  if (value == IR_NONE) {
    ir_op_t load = ir_op(IR_OP_LOAD_SLOT);
    load.slot = opt_tracked_slot(b, tracked);
    value = opt_append_to_finished(b, block, load, NULL, 0, OPT_REPR_TAGGED);
  }
  ir_op_t call = ir_op(IR_OP_CALL_NATIVE);
  call.call.func = b->func;
  call.call.bc_off = b->pc;
  call.call.stores_operands = true;
  call.call.helper = (uintptr_t)opt_rt_new_box;
  call.call.allocates_only = true;
  ir_node_id_t inputs[2] = {opt_tagged_at_end_of(b, block, value), opt_constant(b, IR_VALUE_UNDEFINED)};
  ir_node_id_t box = opt_append_to_finished(b, block, call, inputs, 2, OPT_REPR_POINTER);
  ir_node(b->graph, box)->frame_state = opt_call_position(b);
  return box;
}

static opt_frame_t opt_merge(opt_builder_t *b, uint32_t bi) {
  opt_bblock_t *bblock = &b->blocks[bi];
  const opt_bitset_t *live = &b->live_in[bblock->first];
  const opt_bitset_t *assigned = bblock->is_header ? &b->loops[bblock->heads].assigned : NULL;
  opt_frame_t merged = opt_frame_new(b);
  merged.sp = bblock->depth;
  uint32_t count = bblock->incoming.len;
  ir_node_id_t *inputs = opt_arena_new(&b->arena, ir_node_id_t, count + 1);
  ir_node_id_t undefined = opt_constant(b, IR_VALUE_UNDEFINED);

  for (uint32_t t = 0; t < b->tracked; t++) {
    bool is_param = t < b->params;
    bool is_stack = t >= b->params + b->locals;
    if (opt_in_memory(b, t)) continue;
    if (opt_boxable(b, t)) {
      bool captured = bblock->is_header && opt_bitset_contains(&b->loops[bblock->heads].captures, t), boxed = captured;
      for (uint32_t i = 0; i < count; i++) boxed |= bblock->incoming.items[i].frame.slots[t].boxed;
      if (boxed && (captured || opt_bitset_contains(live, t))) {
        bool same = !bblock->is_header;
        for (uint32_t i = 0; i < count; i++) {
          const opt_incoming_t *incoming = &bblock->incoming.items[i];
          const opt_slot_t *slot = &incoming->frame.slots[t];
          inputs[i] = slot->boxed ? slot->node : opt_box_at_end_of(b, incoming->pred, t, slot->node);
          same &= inputs[i] == inputs[0];
        }
        merged.slots[t] = (opt_slot_t){.node = same ? inputs[0] : opt_add_phi(b, t, inputs, count), .boxed = true};
        continue;
      }
    }
    if (is_stack ? t - b->params - b->locals >= bblock->depth : !opt_bitset_contains(live, t)) {
      if (!is_param && !is_stack) merged.slots[t] = (opt_slot_t){.node = undefined};
      continue;
    }
    bool loop_assigned = assigned && opt_bitset_contains(assigned, t);
    if (is_param) {
      bool all_in_memory = true;
      for (uint32_t i = 0; i < count; i++) all_in_memory &= opt_slot_in_memory(&bblock->incoming.items[i].frame.slots[t]);
      if (all_in_memory && !loop_assigned && !assigned) continue;
    }
    ir_node_id_t first = bblock->incoming.items[0].frame.slots[t].node;
    bool same = first != IR_NONE;
    bool in_sync = true;
    for (uint32_t i = 0; i < count; i++) {
      const opt_slot_t *slot = &bblock->incoming.items[i].frame.slots[t];
      same &= slot->node == first;
      in_sync &= slot->in_sync;
    }
    if (same && !loop_assigned) {
      merged.slots[t] = (opt_slot_t){.node = first, .in_sync = is_param && in_sync};
      continue;
    }
    for (uint32_t i = 0; i < count; i++) {
      const opt_incoming_t *incoming = &bblock->incoming.items[i];
      ir_node_id_t value = incoming->frame.slots[t].node;
      if (value == IR_NONE) {
        ir_op_t op = ir_op(IR_OP_LOAD_SLOT);
        op.slot = opt_tracked_slot(b, t);
        value = opt_append_to_finished(b, incoming->pred, op, NULL, 0, OPT_REPR_TAGGED);
      }
      inputs[i] = opt_tagged_at_end_of(b, incoming->pred, value);
    }
    merged.slots[t] = (opt_slot_t){.node = opt_add_phi(b, t, inputs, count), .in_sync = is_param && !loop_assigned && in_sync};
  }
  for (uint32_t j = 0; j < 2 * b->finally_count; j++) {
    uint32_t t = b->tracked + 1 + j;
    ir_node_id_t first = bblock->incoming.items[0].frame.slots[t].node;
    bool same = true;
    for (uint32_t i = 0; i < count; i++) same &= bblock->incoming.items[i].frame.slots[t].node == first;
    if (same) {
      merged.slots[t] = (opt_slot_t){.node = first};
      continue;
    }
    for (uint32_t i = 0; i < count; i++)
      inputs[i] = opt_tagged_at_end_of(b, bblock->incoming.items[i].pred, bblock->incoming.items[i].frame.slots[t].node);
    merged.slots[t] = (opt_slot_t){.node = opt_add_completion_phi(b, j, inputs, count)};
  }
  bool this_known = true, this_same = true;
  for (uint32_t i = 0; i < count; i++) {
    inputs[i] = bblock->incoming.items[i].frame.this_value;
    this_known &= inputs[i] != IR_NONE;
    this_same &= inputs[i] == inputs[0];
  }
  if (this_known && this_same && !bblock->is_header) merged.this_value = inputs[0];
  else if (this_known) merged.this_value = opt_add_this_phi(b, inputs, count);
  return merged;
}

static bool opt_start_bytecode_block(opt_builder_t *b, uint32_t bi) {
  opt_bblock_t *bblock = &b->blocks[bi];
  if (!bblock->incoming.len) return false;
  ir_block_id_t block = opt_ir_block(b, bi);
  bblock->started = true;
  opt_vec_push(&b->graph->arena, &b->ctx->order, block);
  b->block = block;
  ir_block_t *data = ir_block(b->graph, block);
  for (uint32_t i = 0; i < bblock->incoming.len; i++) {
    if (bblock->incoming.items[i].frame.sp != bblock->depth) return opt_fail(b, "depth");
    opt_vec_push(&b->graph->arena, &data->predecessors, bblock->incoming.items[i].pred);
  }
  data->bytecode_start = b->ins_off[bblock->first];
  data->func = b->inline_call ? b->func : NULL;
  data->is_loop_header = bblock->is_header;
  b->ins = bblock->first;
  b->pc = b->ins_off[bblock->first];
  if (bblock->incoming.len == 1 && !bblock->is_header) b->frame = bblock->incoming.items[0].frame;
  else b->frame = opt_merge(b, bi);
  if (bblock->is_header) bblock->header_frame = opt_frame_clone(b, &b->frame);
  return true;
}

static uint8_t opt_feedback(const opt_builder_t *b) {
  uint8_t *feedback = sv_func_type_feedback(b->func);
  return feedback ? feedback[b->pc] & SV_TFB_CLASS_MASK : 0;
}

static bool opt_int32_constant_of(const opt_builder_t *b, ir_node_id_t value, int32_t *out) {
  const ir_node_t *node = ir_node(b->graph, value);
  if (node->op.code != IR_OP_CONSTANT) return false;
  if (node->repr == OPT_REPR_INT32) {
    *out = (int32_t)(uint32_t)node->op.bits;
    return true;
  }
  return node->repr == OPT_REPR_TAGGED && ir_value_as_int32(node->op.bits, out);
}

static bool opt_may_be_int32(const opt_builder_t *b, ir_node_id_t value) {
  int32_t integer;
  return !ir_is_constant(b->graph, value) || opt_int32_constant_of(b, value, &integer);
}

static bool opt_may_be_number(const opt_builder_t *b, ir_node_id_t value) {
  uint64_t bits;
  if (!ir_constant_value(b->graph, value, &bits)) return true;
  return ir_node(b->graph, value)->repr != OPT_REPR_TAGGED || ir_value_is_number(bits);
}

static ir_node_id_t opt_int32(opt_builder_t *b, ir_node_id_t value) {
  const ir_node_t *node = ir_node(b->graph, value);
  int32_t integer;
  if (node->repr == OPT_REPR_INT32) return value;
  if (node->op.code == IR_OP_BOX_INT32) return node->inputs.items[0];
  if (opt_int32_constant_of(b, value, &integer)) return opt_int32_constant(b, integer);
  return opt_emit_checked(b, ir_op(IR_OP_CHECK_INT32), &value, 1, OPT_REPR_INT32);
}

static ir_node_id_t opt_number(opt_builder_t *b, ir_node_id_t value) {
  const ir_node_t *node = ir_node(b->graph, value);
  uint64_t bits;
  if (node->repr == OPT_REPR_FLOAT64) return value;
  if (node->repr == OPT_REPR_INT32 || node->op.code == IR_OP_BOX_INT32) {
    ir_node_id_t integer = opt_int32(b, value);
    if (ir_constant_value(b->graph, integer, &bits))
      return opt_typed_constant(b, ir_value_double_bits((double)(int32_t)(uint32_t)bits), OPT_REPR_FLOAT64);
    return opt_emit1(b, IR_OP_INT32_TO_FLOAT64, integer, OPT_REPR_FLOAT64);
  }
  if (node->op.code == IR_OP_BOX_FLOAT64) return node->inputs.items[0];
  if (node->op.code == IR_OP_CONSTANT && ir_value_is_number(node->op.bits))
    return opt_typed_constant(b, node->op.bits, OPT_REPR_FLOAT64);
  return opt_emit_checked(b, ir_op(IR_OP_CHECK_NUMBER), &value, 1, OPT_REPR_FLOAT64);
}

static ir_node_id_t opt_int32_binary(opt_builder_t *b, ir_binary_op_t op, ir_node_id_t lhs, ir_node_id_t rhs) {
  ir_op_t ir = ir_op(IR_OP_INT32_BINARY);
  ir.binary = op;
  ir_node_id_t inputs[2] = {lhs, rhs};
  if (ir_binary_int32_can_overflow(op)) return opt_emit_checked(b, ir, inputs, 2, OPT_REPR_INT32);
  return opt_emit(b, ir, inputs, 2, OPT_REPR_INT32);
}

typedef enum {
  OPT_SPEC_INT32,
  OPT_SPEC_FLOAT64,
  OPT_SPEC_NONE,
} opt_spec_t;

static bool opt_saw_double(const opt_builder_t *b) {
  switch (opt_op_at(b, b->pc)) {
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_NEG: case OP_ADD_LOCAL:
    case OP_LT: case OP_LE: case OP_GT: case OP_GE:
      break;
    default:
      return false;
  }
  uint8_t *feedback = sv_func_type_feedback(b->func);
  return feedback && (feedback[b->pc] & SV_TFB_DOUBLE);
}

static opt_spec_t opt_number_speculation(const opt_builder_t *b, ir_node_id_t lhs, ir_node_id_t rhs, bool int32_allowed, bool can_overflow) {
  bool int32 = int32_allowed && !opt_saw_double(b) && opt_may_be_int32(b, lhs) && (rhs == IR_NONE || opt_may_be_int32(b, rhs))
    && opt_may_speculate(b, OPT_EXIT_NOT_INT32) && (!can_overflow || opt_may_speculate(b, OPT_EXIT_OVERFLOW));
  if (int32) return OPT_SPEC_INT32;
  if (opt_may_be_number(b, lhs) && (rhs == IR_NONE || opt_may_be_number(b, rhs)) && opt_may_speculate(b, OPT_EXIT_BAD_TYPE))
    return OPT_SPEC_FLOAT64;
  return OPT_SPEC_NONE;
}

static opt_flow_t opt_numeric_feedback(opt_builder_t *b, bool recorded) {
  if (!recorded) return OPT_FLOW_CONTINUE;
  uint8_t feedback = opt_feedback(b);
  if (feedback == 0) return opt_end_with_exit(b, OPT_EXIT_NO_FEEDBACK);
  return feedback == SV_TFB_NUM ? OPT_FLOW_CONTINUE : OPT_FLOW_GENERIC;
}

static bool opt_result_stored(const opt_builder_t *b) {
  uint32_t next = b->pc + sv_op_size[opt_op_at(b, b->pc)];
  for (; next < (uint32_t)b->func->code_len; next += sv_op_size[opt_op_at(b, next)]) {
    sv_op_t op = opt_op_at(b, next);
    if (sv_op_flags[op] & SV_OPF_MARKER) continue;
    switch (op) {
      case OP_DUP: case OP_INSERT2: case OP_INSERT3: case OP_SWAP: case OP_NIP:
        continue;
      case OP_PUT_FIELD: return true;
      default: return false;
    }
  }
  return false;
}

static ir_node_id_t opt_binary(opt_builder_t *b, ir_binary_op_t op, ir_node_id_t lhs, ir_node_id_t rhs) {
  bool int32_allowed = op != IR_BINARY_DIV && !opt_result_stored(b);
  switch (opt_number_speculation(b, lhs, rhs, int32_allowed, ir_binary_int32_can_overflow(op))) {
    case OPT_SPEC_INT32: {
      ir_node_id_t l = opt_int32(b, lhs);
      ir_node_id_t r = opt_int32(b, rhs);
      return opt_int32_binary(b, op, l, r);
    }
    case OPT_SPEC_FLOAT64: {
      ir_node_id_t inputs[2] = {opt_number(b, lhs), opt_number(b, rhs)};
      ir_op_t ir = ir_op(IR_OP_FLOAT64_BINARY);
      ir.binary = op;
      ir_node_id_t result = opt_emit(b, ir, inputs, 2, OPT_REPR_FLOAT64);
      return opt_emit1(b, IR_OP_BOX_FLOAT64, result, OPT_REPR_TAGGED);
    }
    case OPT_SPEC_NONE: break;
  }
  return IR_NONE;
}

static ir_node_id_t opt_compare(opt_builder_t *b, ir_comparison_t comparison, ir_node_id_t lhs, ir_node_id_t rhs) {
  ir_node_id_t inputs[2];
  ir_op_t op;
  switch (opt_number_speculation(b, lhs, rhs, true, false)) {
    case OPT_SPEC_INT32:
      inputs[0] = opt_int32(b, lhs);
      inputs[1] = opt_int32(b, rhs);
      op = ir_op(IR_OP_INT32_COMPARE);
      break;
    case OPT_SPEC_FLOAT64:
      inputs[0] = opt_number(b, lhs);
      inputs[1] = opt_number(b, rhs);
      op = ir_op(IR_OP_FLOAT64_COMPARE);
      break;
    case OPT_SPEC_NONE: return IR_NONE;
  }
  op.comparison = comparison;
  return opt_emit(b, op, inputs, 2, OPT_REPR_BOOL);
}

static ir_node_id_t opt_box_bool(opt_builder_t *b, ir_node_id_t boolean) {
  return opt_emit1(b, IR_OP_BOX_BOOL, boolean, OPT_REPR_TAGGED);
}

static bool opt_compares_by_identity(const opt_builder_t *b, ir_node_id_t value) {
  uint64_t bits;
  if (!ir_constant_value(b->graph, value, &bits) || ir_node(b->graph, value)->repr != OPT_REPR_TAGGED) return false;
  return bits == IR_VALUE_UNDEFINED || bits == IR_VALUE_NULL || ir_value_is_bool(bits);
}

static opt_flow_t opt_build_equality(opt_builder_t *b, bool equal) {
  ir_node_id_t lhs = opt_peek(b, 1), rhs = opt_peek(b, 0);
  ir_node_id_t result;
  if (opt_compares_by_identity(b, lhs) || opt_compares_by_identity(b, rhs)) {
    ir_op_t op = ir_op(IR_OP_TAGGED_EQUALS);
    op.equal = equal;
    ir_node_id_t inputs[2] = {opt_tagged(b, lhs), opt_tagged(b, rhs)};
    result = opt_emit(b, op, inputs, 2, OPT_REPR_BOOL);
  } else {
    uint8_t feedback = opt_feedback(b);
    if (feedback == 0) return opt_end_with_exit(b, OPT_EXIT_NO_FEEDBACK);
    if (feedback == SV_TFB_NUM) {
      result = opt_compare(b, equal ? IR_CMP_STRICTLY_EQUALS : IR_CMP_STRICTLY_INEQUALS, lhs, rhs);
      if (result == IR_NONE) return OPT_FLOW_GENERIC;
    } else if (!(feedback & (SV_TFB_NUM | SV_TFB_STR)) && opt_may_speculate(b, OPT_EXIT_BAD_TYPE)) {
      ir_node_id_t inputs[2] = {opt_tagged(b, lhs), opt_tagged(b, rhs)};
      inputs[0] = opt_emit_checked(b, ir_op(IR_OP_CHECK_IDENTITY_COMPARABLE), inputs, 2, OPT_REPR_NONE);
      ir_op_t op = ir_op(IR_OP_TAGGED_EQUALS);
      op.equal = equal;
      result = opt_emit(b, op, inputs, 2, OPT_REPR_BOOL);
    } else {
      ir_op_t op = ir_op(IR_OP_STRICT_EQUALS);
      op.equal = equal;
      result = opt_emit(b, op, (ir_node_id_t[]){opt_tagged(b, lhs), opt_tagged(b, rhs)}, 2, OPT_REPR_BOOL);
    }
  }
  opt_drop(b, 2);
  opt_push(b, opt_box_bool(b, result));
  return OPT_FLOW_CONTINUE;
}

static void opt_branch_both_ways(opt_builder_t *b, ir_condition_t condition, const ir_node_id_t *inputs, uint32_t count, ir_block_id_t *if_true, ir_block_id_t *if_false) {
  ir_block_id_t source = b->block;
  *if_true = opt_edge_block(b, source);
  *if_false = opt_edge_block(b, source);
  ir_op_t op = ir_op(IR_OP_BRANCH);
  op.branch.condition = condition;
  op.branch.if_true = *if_true;
  op.branch.if_false = *if_false;
  opt_set_control(b, op, inputs, count);
}

static ir_node_id_t opt_refine(opt_builder_t *b, ir_condition_t condition, const ir_node_id_t *inputs, uint32_t count) {
  ir_op_t op = ir_op(IR_OP_REFINE);
  op.refine.condition = condition;
  op.refine.holds = true;
  return opt_emit(b, op, inputs, count, ir_node(b->graph, inputs[0])->repr);
}

static ir_block_id_t opt_join_blocks(opt_builder_t *b, const ir_block_id_t *predecessors, uint32_t count) {
  ir_block_id_t join = ir_add_block(b->graph);
  for (uint32_t i = 0; i < count; i++) {
    b->block = predecessors[i];
    ir_op_t op = ir_op(IR_OP_JUMP);
    op.target = join;
    opt_set_control(b, op, NULL, 0);
    opt_vec_push(&b->graph->arena, &ir_block(b->graph, join)->predecessors, predecessors[i]);
  }
  opt_vec_push(&b->graph->arena, &b->ctx->order, join);
  b->block = join;
  return join;
}

static ir_node_id_t opt_value_phi(opt_builder_t *b, uint32_t stack_index, const ir_node_id_t *inputs, uint32_t count) {
  return opt_add_phi(b, opt_stack_index(b, stack_index), inputs, count);
}

static ir_node_id_t opt_bool_phi(opt_builder_t *b, const ir_node_id_t *inputs, uint32_t count) {
  ir_op_t op = ir_op(IR_OP_PHI);
  op.slot = opt_stack_slot(b->frame.sp - 1);
  ir_node_id_t phi = ir_add_node(b->graph, op, inputs, count, OPT_REPR_BOOL, b->pc);
  opt_vec_push(&b->graph->arena, &ir_block(b->graph, b->block)->phis, phi);
  return phi;
}

static int32_t opt_to_int32(double number) {
  if (!isfinite(number)) return 0;
  double wrapped = fmod(trunc(number), 4294967296.0);
  if (wrapped < 0) wrapped += 4294967296.0;
  return (int32_t)(uint32_t)wrapped;
}

static bool opt_known_word32(const opt_builder_t *b, ir_node_id_t value) {
  const ir_node_t *node = ir_node(b->graph, value);
  int32_t integer;
  uint64_t bits;
  if (node->repr == OPT_REPR_INT32 || node->op.code == IR_OP_BOX_INT32 || node->op.code == IR_OP_BOX_BOOL) return true;
  if (opt_int32_constant_of(b, value, &integer)) return true;
  return ir_constant_value(b->graph, value, &bits) && node->repr == OPT_REPR_TAGGED && ir_value_is_bool(bits);
}

static ir_node_id_t opt_word32(opt_builder_t *b, ir_node_id_t value) {
  const ir_node_t *node = ir_node(b->graph, value);
  int32_t integer;
  uint64_t bits;
  if (node->repr == OPT_REPR_INT32) return value;
  if (node->op.code == IR_OP_BOX_INT32) return node->inputs.items[0];
  if (node->op.code == IR_OP_BOX_BOOL) return opt_emit1(b, IR_OP_BOOL_TO_INT32, node->inputs.items[0], OPT_REPR_INT32);
  if (ir_constant_value(b->graph, value, &bits) && node->repr == OPT_REPR_TAGGED && ir_value_is_bool(bits))
    return opt_int32_constant(b, bits == IR_VALUE_TRUE);
  if (opt_int32_constant_of(b, value, &integer)) return opt_int32_constant(b, integer);
  if (ir_constant_value(b->graph, value, &bits)) {
    if (node->repr != OPT_REPR_TAGGED || !ir_value_is_number(bits)) return IR_NONE;
    return opt_int32_constant(b, opt_to_int32(ir_value_to_double(bits)));
  }
  if (node->op.code == IR_OP_BOX_FLOAT64) {
    ir_node_id_t number = node->inputs.items[0];
    const ir_node_t *source = ir_node(b->graph, number);
    if (source->op.code == IR_OP_UINT32_TO_FLOAT64) return source->inputs.items[0];
    return opt_emit1(b, IR_OP_FLOAT64_TO_INT32, number, OPT_REPR_INT32);
  }
  if (opt_may_speculate(b, OPT_EXIT_NOT_INT32)) return opt_int32(b, value);
  if (!opt_may_speculate(b, OPT_EXIT_BAD_TYPE)) return IR_NONE;
  return opt_emit1(b, IR_OP_FLOAT64_TO_INT32, opt_number(b, value), OPT_REPR_INT32);
}

static ir_binary_op_t opt_bitwise_of(sv_op_t op) {
  switch (op) {
    case OP_BAND: return IR_BINARY_BITWISE_AND;
    case OP_BOR: return IR_BINARY_BITWISE_OR;
    case OP_BXOR: return IR_BINARY_BITWISE_XOR;
    case OP_SHL: return IR_BINARY_LEFT_SHIFT;
    case OP_SHR: return IR_BINARY_RIGHT_SHIFT;
    default: return IR_BINARY_UNSIGNED_RIGHT_SHIFT;
  }
}

static opt_flow_t opt_build_bitwise(opt_builder_t *b, sv_op_t op) {
  bool known = opt_known_word32(b, opt_peek(b, 0)) && (op == OP_BNOT || opt_known_word32(b, opt_peek(b, 1)));
  opt_flow_t flow = known ? OPT_FLOW_CONTINUE : opt_numeric_feedback(b, true);
  if (flow != OPT_FLOW_CONTINUE) return flow;
  if (op == OP_BNOT) {
    ir_node_id_t value = opt_word32(b, opt_peek(b, 0));
    if (value == IR_NONE) return OPT_FLOW_GENERIC;
    opt_poke(b, 0, opt_int32_binary(b, IR_BINARY_BITWISE_XOR, value, opt_int32_constant(b, -1)));
    return OPT_FLOW_CONTINUE;
  }
  ir_node_id_t lhs = opt_word32(b, opt_peek(b, 1));
  ir_node_id_t rhs = lhs == IR_NONE ? IR_NONE : opt_word32(b, opt_peek(b, 0));
  if (rhs == IR_NONE) return OPT_FLOW_GENERIC;
  ir_binary_op_t binary = opt_bitwise_of(op);
  ir_node_id_t result;
  if (binary != IR_BINARY_UNSIGNED_RIGHT_SHIFT || opt_may_speculate(b, OPT_EXIT_OVERFLOW)) result = opt_int32_binary(b, binary, lhs, rhs);
  else {
    ir_node_id_t shifted = opt_emit(b, ir_op(IR_OP_UINT32_SHIFT_RIGHT), (ir_node_id_t[]){lhs, rhs}, 2, OPT_REPR_INT32);
    result = opt_emit1(b, IR_OP_BOX_FLOAT64, opt_emit1(b, IR_OP_UINT32_TO_FLOAT64, shifted, OPT_REPR_FLOAT64), OPT_REPR_TAGGED);
  }
  opt_drop(b, 2);
  opt_push(b, result);
  return OPT_FLOW_CONTINUE;
}

static ir_node_id_t opt_truthiness(opt_builder_t *b, ir_node_id_t value, bool negate) {
  const ir_node_t *node = ir_node(b->graph, value);
  if (node->op.code == IR_OP_BOX_BOOL && !negate) return node->inputs.items[0];
  if (node->op.code == IR_OP_BOX_BOOL) {
    ir_op_t op = ir_op(IR_OP_TAGGED_EQUALS);
    op.equal = true;
    return opt_emit(b, op, (ir_node_id_t[]){value, opt_constant(b, IR_VALUE_FALSE)}, 2, OPT_REPR_BOOL);
  }
  if (node->repr == OPT_REPR_INT32) {
    ir_op_t op = ir_op(IR_OP_INT32_COMPARE);
    op.comparison = negate ? IR_CMP_STRICTLY_EQUALS : IR_CMP_STRICTLY_INEQUALS;
    return opt_emit(b, op, (ir_node_id_t[]){value, opt_int32_constant(b, 0)}, 2, OPT_REPR_BOOL);
  }
  value = opt_tagged(b, value);
  ir_block_id_t source = b->block;
  ir_block_id_t blocks[4];
  blocks[0] = opt_edge_block(b, source);
  blocks[1] = opt_edge_block(b, source);
  ir_block_id_t fallback = opt_edge_block(b, source);
  ir_op_t op = ir_op(IR_OP_BRANCH_TRUTHY);
  op.truthy.if_true = blocks[0];
  op.truthy.if_false = blocks[1];
  op.truthy.fallback = fallback;
  opt_set_control(b, op, &value, 1);
  b->block = fallback;
  ir_node_id_t boolean = opt_emit1(b, IR_OP_TO_BOOLEAN, value, OPT_REPR_BOOL);
  opt_branch_both_ways(b, ir_cond(IR_COND_BOOL), &boolean, 1, &blocks[2], &blocks[3]);
  opt_join_blocks(b, blocks, 4);
  ir_node_id_t yes = opt_typed_constant(b, negate ? 0 : 1, OPT_REPR_BOOL), no = opt_typed_constant(b, negate ? 1 : 0, OPT_REPR_BOOL);
  return opt_bool_phi(b, (ir_node_id_t[]){yes, no, yes, no}, 4);
}

static bool opt_is_nullish_constant(const opt_builder_t *b, ir_node_id_t value) {
  uint64_t bits;
  return ir_constant_value(b->graph, value, &bits) && ir_node(b->graph, value)->repr == OPT_REPR_TAGGED
    && (bits == IR_VALUE_UNDEFINED || bits == IR_VALUE_NULL);
}

static ir_node_id_t opt_is_nullish(opt_builder_t *b, ir_node_id_t value, bool equal) {
  ir_op_t op = ir_op(IR_OP_IS_NULLISH);
  op.equal = equal;
  ir_node_id_t tagged = opt_tagged(b, value);
  return opt_emit(b, op, &tagged, 1, OPT_REPR_BOOL);
}

static opt_flow_t opt_build_loose_equality(opt_builder_t *b, bool equal) {
  ir_node_id_t lhs = opt_peek(b, 1), rhs = opt_peek(b, 0), result;
  if (opt_is_nullish_constant(b, lhs) || opt_is_nullish_constant(b, rhs))
    result = opt_is_nullish(b, opt_is_nullish_constant(b, lhs) ? rhs : lhs, equal);
  else {
    uint8_t feedback = opt_feedback(b);
    if (feedback == 0) return opt_end_with_exit(b, OPT_EXIT_NO_FEEDBACK);
    if (feedback == SV_TFB_NUM) {
      result = opt_compare(b, equal ? IR_CMP_STRICTLY_EQUALS : IR_CMP_STRICTLY_INEQUALS, lhs, rhs);
      if (result == IR_NONE) return OPT_FLOW_GENERIC;
    } else if ((feedback == SV_TFB_BOOL || feedback == SV_TFB_OTHER) && opt_may_speculate(b, OPT_EXIT_BAD_TYPE)) {
      ir_node_id_t inputs[2] = {opt_tagged(b, lhs), opt_tagged(b, rhs)};
      ir_op_t check = ir_op(IR_OP_CHECK_IDENTITY_COMPARABLE);
      check.bits = feedback == SV_TFB_BOOL ? 1u << kTypeBool : IR_VALUE_OBJECT_TYPES | (1u << kTypeBuiltin) | (1u << kTypeUndefined) | (1u << kTypeNull);
      inputs[0] = opt_emit_checked(b, check, inputs, 2, OPT_REPR_NONE);
      ir_op_t op = ir_op(feedback == SV_TFB_BOOL ? IR_OP_TAGGED_EQUALS : IR_OP_LOOSE_IDENTITY_EQUALS);
      op.equal = equal;
      result = opt_emit(b, op, inputs, 2, OPT_REPR_BOOL);
    } else return OPT_FLOW_GENERIC;
  }
  opt_drop(b, 2);
  opt_push(b, opt_box_bool(b, result));
  return OPT_FLOW_CONTINUE;
}

static bool opt_retain(opt_builder_t *b, ant_value_t value) {
  ir_graph_t *graph = b->graph;
  for (uint32_t i = 0; i < graph->retained.len; i++)
    if (graph->retained.items[i] == value) return true;
  if (!sv_code_retain_value(b->js, value)) return false;
  opt_vec_push(&graph->arena, &graph->retained, value);
  return true;
}

static void opt_retain_func(opt_builder_t *b, sv_func_t *func) {
  ir_graph_t *graph = b->graph;
  if (func == graph->func || !func->unit) return;
  for (uint32_t i = 0; i < graph->retained_funcs.len; i++)
    if (graph->retained_funcs.items[i] == func) return;
  opt_vec_push(&graph->arena, &graph->retained_funcs, func);
}

void opt_build_release(ant_t *js, ir_graph_t *graph) {
  graph->retained.len = 0;
  graph->retained_funcs.len = 0;
}

static constexpr uint32_t OPT_CALL_PLAIN_FLAGS = SV_CALL_IS_ARROW | SV_CALL_HAS_SUPER;

static ant_value_t opt_plain_getter(const ant_shape_prop_t *prop) {
  if (!prop->has_getter || vtype(prop->getter) != kTypeFunction) return 0;
  sv_closure_t *closure = js_func_closure(prop->getter);
  if (!closure || !closure->func || (closure->call_flags & ~OPT_CALL_PLAIN_FLAGS) || closure->func->is_async || closure->func->is_generator
    || closure->func->code_len <= 0) return 0;
  return prop->getter;
}

static bool opt_field_slot_ok(ant_t *js, ant_shape_t *shape, uint32_t index, const sv_atom_t *atom, bool store, ant_value_t *getter) {
  const ant_shape_prop_t *prop = ant_shape_prop_at(shape, index);
  if (!prop || prop->type != ANT_SHAPE_KEY_STRING || prop->key.interned != atom->str) return false;
  if (prop->has_getter || prop->has_setter) {
    if (store || !getter || !(*getter = opt_plain_getter(prop))) return false;
  }
  if (!atom->len || (atom->str[0] >= '0' && atom->str[0] <= '9')) return false;
  if (atom->len == 6 && memcmp(atom->str, "length", 6) == 0) return false;
  if (index > 0xffff) return false;
  if (store && ((prop->attrs & ANT_PROP_ATTR_WRITABLE) == 0 || ant_property_key_is_watched(js, atom->str) ||
      atom->str == js->intern.next)) return false;
  return true;
}

static bool opt_field_ok(ant_t *js, ant_shape_t *shape, uint32_t index, const sv_atom_t *atom, bool store) {
  return opt_field_slot_ok(js, shape, index, atom, store, NULL);
}

typedef struct {
  ant_shape_t *from, *to;
  uint32_t slot;
  uintptr_t proto;
  uint32_t epoch;
} opt_add_case_t;

static constexpr uint32_t OPT_MAX_ADDS = 1 + SV_GF_POLY_WAYS;

static bool opt_add_grows_storage(uint32_t slot, uint8_t limit) {
  if (slot < limit) return false;
  uint32_t overflow = slot - limit;
  return overflow == 0 || (overflow >= 4 && (overflow & (overflow - 1)) == 0);
}

static bool opt_add_case_ok(ant_t *js, const opt_add_case_t *add, const sv_atom_t *atom) {
  if (!add->proto || ((add->proto & 1u) && (add->proto >> 1) > UINT32_MAX)) return false;
  uint8_t limit = ant_shape_get_inobj_limit(add->to);
  if (opt_add_grows_storage(add->slot, limit) || add->slot >= limit + 16u || ant_shape_get_inobj_limit(add->from) != limit) return false;
  if (atom->len == 9 && memcmp(atom->str, "__proto__", 9) == 0) return false;
  return opt_field_ok(js, add->to, add->slot, atom, true);
}

static void opt_runtime_add_note(ant_shape_t *from, ant_shape_t **runtime, uint32_t *runtime_count) {
  if (!runtime) return;
  for (uint32_t j = 0; j < *runtime_count; j++)
    if (runtime[j] == from) return;
  if (*runtime_count < OPT_MAX_ADDS) runtime[(*runtime_count)++] = from;
}

static void opt_add_case_note(ant_t *js, const opt_add_case_t *add, const sv_atom_t *atom, opt_add_case_t *adds, uint32_t *count, ant_shape_t **runtime, uint32_t *runtime_count) {
  if (!add->from || !add->to) return;
  for (uint32_t j = 0; j < *count; j++) {
    if (adds[j].from != add->from || adds[j].proto != add->proto) continue;
    if (add->epoch == js->ic.epoch && add->to == adds[j].to && add->slot == adds[j].slot) adds[j].epoch = add->epoch;
    return;
  }
  if (opt_add_case_ok(js, add, atom)) adds[(*count)++] = *add;
  else opt_runtime_add_note(add->from, runtime, runtime_count);
}

static uint32_t opt_add_cases(ant_t *js, const sv_ic_entry_t *ic, const sv_atom_t *atom, opt_add_case_t *adds, ant_shape_t **runtime, uint32_t *runtime_count) {
  if (ic->shape_ref_mask & SV_IC_POLY_MEGA) return 0;
  uint32_t count = 0;
  opt_add_case_t add = {ic->guard.add.from_shape, ic->guard.add.to_shape, ic->guard.add.slot, ic->add_proto, ic->guard.add.epoch};
  opt_add_case_note(js, &add, atom, adds, &count, runtime, runtime_count);
  const sv_pf_poly_t *poly = (ic->shape_ref_mask & SV_IC_HAS_PUT_POLY) ? ic->put_poly : NULL;
  for (uint32_t i = 0; poly && i < SV_GF_POLY_WAYS && count < OPT_MAX_ADDS; i++) {
    const sv_pf_poly_entry_t *e = &poly->entries[i];
    if (!e->add) continue;
    add = (opt_add_case_t){e->shape, e->to_shape, e->index, e->proto, e->epoch};
    opt_add_case_note(js, &add, atom, adds, &count, runtime, runtime_count);
  }
  return count;
}

static ir_node_id_t opt_object_address(opt_builder_t *b, ir_node_id_t *object) {
  if (!opt_may_speculate(b, OPT_EXIT_BAD_TYPE)) return IR_NONE;
  return opt_emit_checked(b, ir_op(IR_OP_OBJECT_ADDRESS), object, 1, OPT_REPR_POINTER);
}

static ir_node_id_t opt_check_shape(opt_builder_t *b, ir_node_id_t object, ir_node_id_t address, ant_shape_t *shape) {
  ir_shape_check_t check = {.shape = (ir_cell_t)(uintptr_t)shape};
  ir_op_t op = ir_op(IR_OP_CHECK_SHAPE);
  op.shapes.items = ir_dup_shapes(b->graph, &check, 1);
  op.shapes.len = 1;
  return opt_emit_checked(b, op, (ir_node_id_t[]){object, address}, 2, OPT_REPR_TAGGED);
}

static ir_node_id_t opt_named_storage(opt_builder_t *b, ir_node_id_t object, ir_node_id_t address, uint32_t offset, const ant_shape_t *shape) {
  uint32_t index = opt_named_index(offset), count = ant_shape_count(shape);
  if (index < opt_named_inobj_limit(offset)) return IR_NONE;
  ir_op_t op = ir_op(IR_OP_LOAD_NAMED_STORAGE);
  op.count = count > index ? count : index + 1;
  return opt_emit_checked(b, op, (ir_node_id_t[]){object, address}, 2, OPT_REPR_POINTER);
}

static ir_node_id_t opt_load_named(opt_builder_t *b, ir_node_id_t object, ir_node_id_t address, uint32_t index, const ant_shape_t *shape) {
  ir_op_t op = ir_op(IR_OP_LOAD_NAMED);
  op.offset = opt_named_offset(index, ant_shape_get_inobj_limit(shape));
  ir_node_id_t storage = opt_named_storage(b, object, address, op.offset, shape);
  return opt_emit(b, op, (ir_node_id_t[]){object, storage == IR_NONE ? address : storage}, 2, OPT_REPR_TAGGED);
}

static void opt_store_named(opt_builder_t *b, ir_node_id_t object, ir_node_id_t address, ir_node_id_t value, uint32_t index, const ant_shape_t *shape) {
  ir_op_t op = ir_op(IR_OP_STORE_NAMED);
  op.offset = opt_named_offset(index, ant_shape_get_inobj_limit(shape));
  ir_node_id_t storage = opt_named_storage(b, object, address, op.offset, shape);
  if (storage == IR_NONE) opt_emit(b, op, (ir_node_id_t[]){object, value, address}, 3, OPT_REPR_NONE);
  else opt_emit(b, op, (ir_node_id_t[]){object, value, address, storage}, 4, OPT_REPR_NONE);
}

static opt_flow_t opt_build_generic(opt_builder_t *b);
static opt_flow_t opt_build_access_call(opt_builder_t *b, bool slow_path);
static opt_flow_t opt_build_out_of_line(opt_builder_t *b);
static opt_flow_t opt_build_probed_field(opt_builder_t *b, sv_func_t *func, uint16_t ic_index, sv_op_t opcode);
static ir_node_id_t opt_probe_field(opt_builder_t *b, sv_func_t *func, uint16_t ic_index, ir_node_id_t object, ir_node_id_t address, ir_block_id_t *slow, uint32_t *slow_count);
static opt_flow_t opt_build_string_field(opt_builder_t *b, const sv_ic_entry_t *ic, const sv_atom_t *atom, sv_op_t opcode);
static opt_flow_t opt_build_length(opt_builder_t *b);
static ir_node_id_t opt_value_phi(opt_builder_t *b, uint32_t stack_index, const ir_node_id_t *inputs, uint32_t count);
static ir_block_id_t opt_build_slow_path(opt_builder_t *b, const ir_block_id_t *entries, uint32_t count, const opt_frame_t *before);
static ir_block_id_t opt_build_slow_path_counted(opt_builder_t *b, const ir_block_id_t *entries, uint32_t count, const opt_frame_t *before, bool counted);
static ir_block_id_t opt_build_slow_path_or_exit(opt_builder_t *b, const ir_block_id_t *entries, uint32_t count, const opt_frame_t *before);
static void opt_join_slow_path(opt_builder_t *b, ir_block_id_t fast_end, ir_block_id_t slow_end, const opt_frame_t *slow, uint32_t results);
static ir_block_id_t opt_branch_off(opt_builder_t *b, ir_condition_t condition, const ir_node_id_t *inputs, uint32_t count, bool off_if, ir_block_id_t *off);

static void opt_add_fields(opt_builder_t *b, ir_node_id_t object, ir_node_id_t address, ir_node_id_t value, const opt_add_case_t *adds, const opt_rt_add_site_t *site, const uint32_t *members, uint32_t count, ir_block_id_t *slow, uint32_t *slow_count) {
  ir_block_id_t added[OPT_MAX_ADDS];
  for (uint32_t j = 0; j < count; j++) {
    const opt_add_case_t *add = &adds[members[j]];
    ir_op_t op = ir_op(IR_OP_ADD_NAMED);
    op.add_named.offset = opt_named_offset(add->slot, ant_shape_get_inobj_limit(add->to));
    op.add_named.shape = (ir_cell_t)(uintptr_t)add->to;
    op.add_named.property_count = add->slot + 1;
    op.add_named.proto = (uint64_t)add->proto;
    op.add_named.cell = (uint64_t)(uintptr_t)site->adds[members[j]].cell;
    ir_op_t can = op;
    can.code = IR_OP_CAN_ADD_NAMED;
    ir_node_id_t allowed = opt_emit(b, can, (ir_node_id_t[]){object, address}, 2, OPT_REPR_BOOL);
    ir_block_id_t no;
    opt_branch_off(b, ir_cond(IR_COND_BOOL), &allowed, 1, false, &no);
    opt_emit(b, op, (ir_node_id_t[]){object, value, address}, 3, OPT_REPR_NONE);
    added[j] = b->block;
    b->block = no;
  }
  slow[(*slow_count)++] = b->block;
  if (count > 1) opt_join_blocks(b, added, count);
  else b->block = added[0];
}

static constexpr uint32_t OPT_MAX_FIELD_SHAPES = 1 + SV_GF_POLY_WAYS;

static constexpr uint32_t OPT_MAX_PROTOTYPE_DEPTH = 4;

typedef struct {
  ant_shape_t *shape;
  uint32_t index;
  uint32_t depth;
  ant_value_t values[OPT_MAX_PROTOTYPE_DEPTH];
  ant_object_t *links[OPT_MAX_PROTOTYPE_DEPTH];
  uint32_t age;
  ant_value_t getter;
} opt_field_case_t;

static void opt_field_case_add(opt_field_case_t *cases, uint32_t *count, const opt_field_case_t *entry) {
  for (uint32_t j = 0; j < *count; j++)
    if (cases[j].shape == entry->shape && cases[j].depth == entry->depth && (!entry->depth || cases[j].values[0] == entry->values[0])) return;
  cases[(*count)++] = *entry;
}

static ant_object_t *opt_live_object(ant_t *js, ant_value_t value) {
  ant_object_t *object = js_obj_ptr(js_as_obj(value));
  return object && fixed_arena_contains(&js->obj_arena, object) && object->mark_epoch != ANT_GC_DEAD ? object : NULL;
}

static ant_object_t *opt_prototype_object(ant_t *js, ant_value_t value) {
  switch (vtype(value)) {
    case kTypeObject:
    case kTypeArray: return opt_live_object(js, value);
    case kTypeFunction: {
      sv_closure_t *closure = js_func_closure(value);
      if (!closure || !fixed_arena_contains(&js->closure_arena, closure)) return NULL;
      return is_object_type(closure->func_obj) ? opt_live_object(js, closure->func_obj) : NULL;
    }
    default: return NULL;
  }
}

static bool opt_prototype_case(ant_t *js, ant_shape_t *shape, ant_object_t *holder, ant_value_t proto, uint32_t index, const sv_atom_t *atom, opt_field_case_t *out) {
  if (ant_shape_proto(shape) != proto) return false;
  *out = (opt_field_case_t){.shape = shape, .index = index};
  ant_value_t value = proto;
  for (uint32_t depth = 0; depth < OPT_MAX_PROTOTYPE_DEPTH; depth++) {
    ant_object_t *object = opt_prototype_object(js, value);
    if (!object || object->flags.is_exotic || !object->shape) return false;
    out->values[depth] = value;
    out->links[depth] = object;
    out->depth = depth + 1;
    if (object == holder) return index < holder->prop_count && opt_field_slot_ok(js, holder->shape, index, atom, false, &out->getter);
    if (ant_shape_lookup_interned(object->shape, atom->str) >= 0) return false;
    value = object->proto;
  }
  return false;
}

static uint32_t opt_field_cases(opt_builder_t *b, const sv_ic_entry_t *ic, const sv_atom_t *atom, bool store, opt_field_case_t *cases) {
  uint32_t count = 0;
  ant_t *js = b->js;
  opt_field_case_t entry;
  if (ic->shape_ref_mask & SV_IC_POLY_MEGA) return 0;
  bool own = store || sv_gf_kind_is_own(ic->get_kind);
  bool held = ic->epoch == js->ic.epoch || (ic->shape_ref_mask & SV_IC_SHAPE_REF_CACHED);
  ant_value_t getter = 0;
  if (ic->cached_shape && own && held && opt_field_slot_ok(js, ic->cached_shape, ic->cached_index, atom, store, &getter))
    opt_field_case_add(cases, &count, &(opt_field_case_t){.shape = ic->cached_shape, .index = ic->cached_index, .getter = getter});
  if (store) {
    const sv_pf_poly_t *poly = (ic->shape_ref_mask & SV_IC_HAS_PUT_POLY) ? ic->put_poly : NULL;
    for (uint32_t i = 0; poly && i < SV_GF_POLY_WAYS; i++) {
      const sv_pf_poly_entry_t *e = &poly->entries[i];
      if (e->add || !e->shape || e->epoch != js->ic.epoch || !opt_field_ok(js, e->shape, e->index, atom, true)) continue;
      opt_field_case_add(cases, &count, &(opt_field_case_t){.shape = e->shape, .index = e->index});
    }
    return count;
  }
  if (ic->cached_shape && sv_gf_kind_is_proto(ic->get_kind)
    && opt_prototype_case(js, ic->cached_shape, ic->cached_holder, ic->guard.get.receiver_proto, ic->cached_index, atom, &entry))
    opt_field_case_add(cases, &count, &entry);
  const sv_gf_poly_t *poly = (ic->shape_ref_mask & SV_IC_HAS_GET_POLY) ? ic->guard.get.poly : NULL;
  for (uint32_t i = 0; poly && i < SV_GF_POLY_WAYS && count < OPT_MAX_FIELD_SHAPES; i++) {
    const sv_gf_poly_entry_t *e = &poly->entries[i];
    if (!e->shape || e->shape == SV_IC_SHAPE_SLOT_DEAD || e->epoch != js->ic.epoch) continue;
    getter = 0;
    if (sv_gf_kind_is_own(e->kind) && opt_field_slot_ok(js, e->shape, e->index, atom, false, &getter))
      opt_field_case_add(cases, &count, &(opt_field_case_t){.shape = e->shape, .index = e->index, .getter = getter});
    else if (sv_gf_kind_is_proto(e->kind) && opt_prototype_case(js, e->shape, e->holder, e->receiver_proto, e->index, atom, &entry))
      opt_field_case_add(cases, &count, &entry);
  }
  return count;
}

static uint32_t opt_field_memo_merge(opt_builder_t *b, const sv_atom_t *atom, bool store, opt_field_case_t *cases, uint32_t count) {
  opt_field_memo_case_t memo[OPT_FIELD_MEMO_CASES];
  uint32_t known = opt_tier_field_memo(b->js, b->func, b->pc, memo);
  opt_field_case_t entry;
  for (uint32_t i = 0; i < known && count < OPT_MAX_FIELD_SHAPES; i++) {
    const opt_field_memo_case_t *seen = &memo[i];
    ant_value_t getter = 0;
    if (!seen->holder) {
      if (opt_field_slot_ok(b->js, seen->shape, seen->index, atom, store, &getter))
        opt_field_case_add(cases, &count, &(opt_field_case_t){.shape = seen->shape, .index = seen->index, .age = seen->age + 1, .getter = getter});
    } else if (!store && opt_prototype_case(b->js, seen->shape, seen->holder, seen->proto, seen->index, atom, &entry)) {
      entry.age = seen->age + 1;
      opt_field_case_add(cases, &count, &entry);
    }
  }
  return count;
}

static void opt_field_memo_store(opt_builder_t *b, const opt_field_case_t *cases, uint32_t count) {
  opt_field_memo_case_t memo[OPT_FIELD_MEMO_CASES];
  uint32_t kept = 0;
  for (uint32_t i = 0; i < count && kept < OPT_FIELD_MEMO_CASES; i++) {
    const opt_field_case_t *access = &cases[i];
    if (access->depth && access->age > OPT_FIELD_MEMO_MAX_AGE) continue;
    memo[kept++] = (opt_field_memo_case_t){
      .shape = access->shape,
      .index = access->index,
      .holder = access->depth ? access->links[access->depth - 1] : NULL,
      .proto = access->depth ? access->values[0] : 0,
      .age = access->age,
    };
  }
  if (kept) opt_tier_field_remember(b->js, b->func, b->pc, memo, kept);
}

static bool opt_field_case_retain(opt_builder_t *b, const opt_field_case_t *access) {
  if (!jit_snapshot_shape(b->js, access->shape)) return false;
  for (uint32_t i = 0; i < access->depth; i++) {
    ant_proto_watch(access->links[i]);
    if (!jit_snapshot_shape(b->js, access->links[i]->shape) || !opt_retain(b, access->values[i])) return false;
  }
  return true;
}

static void opt_note_likely_value(opt_builder_t *b, ir_node_id_t node, ant_value_t value) {
  uint8_t type = vtype(value);
  if (type == kTypeFunction && !js_func_closure(value)) return;
  if (type != kTypeFunction && type != kTypeBuiltin && type != kTypeObject) return;
  opt_vec_push(&b->graph->arena, &b->ctx->likely_nodes, node);
  opt_vec_push(&b->graph->arena, &b->ctx->likely_values, value);
}

static ant_value_t opt_likely_value(const opt_builder_t *b, ir_node_id_t node) {
  ir_node_id_t unrefined = ir_unrefined(b->graph, node);
  for (uint32_t i = b->ctx->likely_nodes.len; i-- > 0;)
    if (b->ctx->likely_nodes.items[i] == node || b->ctx->likely_nodes.items[i] == unrefined) return b->ctx->likely_values.items[i];
  return 0;
}

static void opt_expect_value(opt_builder_t *b, ir_node_id_t value, ant_value_t expected, ir_block_id_t *slow, uint32_t *slow_count) {
  if (slow) {
    ir_condition_t same = ir_cond(IR_COND_TAGGED_EQUALS);
    same.equal = true;
    opt_branch_off(b, same, (ir_node_id_t[]){value, opt_constant(b, expected)}, 2, false, &slow[(*slow_count)++]);
    return;
  }
  ir_op_t check = ir_op(IR_OP_CHECK_VALUE);
  check.check_value.expected = expected;
  check.check_value.kind = OPT_EXIT_BAD_SHAPE;
  opt_emit_checked(b, check, &value, 1, OPT_REPR_NONE);
}

static void opt_expect_shape(opt_builder_t *b, ir_node_id_t object, ir_node_id_t address, ant_shape_t *shape, ir_block_id_t *slow, uint32_t *slow_count) {
  if (!slow) {
    opt_check_shape(b, object, address, shape);
    return;
  }
  ir_block_id_t source = b->block;
  ir_shape_case_t cases[2] = {
    {.check = {.shape = (ir_cell_t)(uintptr_t)shape}, .block = opt_edge_block(b, source)},
    {.check = {.shape = 0}, .block = opt_edge_block(b, source)},
  };
  ir_op_t shape_switch = ir_op(IR_OP_SHAPE_SWITCH);
  shape_switch.cases.items = ir_dup_cases(b->graph, cases, 2);
  shape_switch.cases.len = 2;
  ir_frame_state_id_t state = opt_eager_frame_state(b);
  ir_node(b->graph, opt_set_control(b, shape_switch, (ir_node_id_t[]){object, address}, 2))->frame_state = state;
  slow[(*slow_count)++] = cases[1].block;
  b->block = cases[0].block;
}

static bool opt_watch_chain(opt_builder_t *b, const opt_field_case_t *access) {
  ant_proto_watch_t *watches[OPT_MAX_PROTOTYPE_DEPTH];
  for (uint32_t i = 0; i < access->depth; i++)
    if (!(watches[i] = ant_proto_watch(access->links[i]))) return false;
  for (uint32_t i = 0; i < access->depth; i++)
    ir_depend_on(b->graph, (opt_dependency_t){.kind = OPT_DEPENDENCY_PROTOTYPE_WATCH, .a = (uint64_t)(uintptr_t)watches[i]});
  opt_emit_checked(b, ir_op(IR_OP_ASSUME_VALID), NULL, 0, OPT_REPR_NONE);
  return true;
}

static bool opt_holder_chain(opt_builder_t *b, ir_node_id_t prototype, const opt_field_case_t *access, bool watch, ir_block_id_t *slow, uint32_t *slow_count,
  ir_node_id_t *holder_object, ir_node_id_t *holder_address) {
  if (prototype != IR_NONE) opt_expect_value(b, prototype, access->values[0], slow, slow_count);
  if (watch && opt_watch_chain(b, access)) {
    *holder_object = opt_constant(b, access->values[access->depth - 1]);
    *holder_address = opt_typed_constant(b, (uint64_t)(uintptr_t)access->links[access->depth - 1], OPT_REPR_POINTER);
    return true;
  }
  ir_node_id_t object = IR_NONE, address = IR_NONE;
  for (uint32_t i = 0; i < access->depth; i++) {
    ant_object_t *link = access->links[i];
    object = opt_constant(b, access->values[i]);
    address = opt_typed_constant(b, (uint64_t)(uintptr_t)link, OPT_REPR_POINTER);
    opt_expect_shape(b, object, address, link->shape, slow, slow_count);
    if (i + 1 < access->depth && ant_shape_proto(link->shape) != access->values[i + 1])
      opt_expect_value(b, opt_emit(b, ir_op(IR_OP_LOAD_PROTOTYPE), &address, 1, OPT_REPR_TAGGED), access->values[i + 1], slow, slow_count);
  }
  *holder_object = object;
  *holder_address = address;
  return false;
}

static ir_node_id_t opt_holder_load(opt_builder_t *b, ir_node_id_t prototype, const opt_field_case_t *access, ir_block_id_t *slow, uint32_t *slow_count) {
  ir_node_id_t object, address;
  ant_object_t *holder = access->links[access->depth - 1];
  ant_value_t value = ant_object_prop_get_unchecked(holder, access->index);
  bool method = is_object_type(value) || vtype(value) == kTypeBuiltin;
  if (opt_holder_chain(b, prototype, access, method && opt_retain(b, value), slow, slow_count, &object, &address)) {
    ir_node_id_t constant = opt_constant(b, value);
    opt_note_likely_value(b, constant, value);
    return constant;
  }
  ir_node_id_t loaded = opt_load_named(b, object, address, access->index, holder->shape);
  opt_note_likely_value(b, loaded, value);
  return loaded;
}

static ir_node_id_t opt_named_access(opt_builder_t *b, ir_node_id_t object, ir_node_id_t address, ir_node_id_t value, const opt_field_case_t *access, ir_block_id_t *slow, uint32_t *slow_count) {
  if (access->depth) return opt_holder_load(b, IR_NONE, access, slow, slow_count);
  if (value == IR_NONE) return opt_load_named(b, object, address, access->index, access->shape);
  opt_store_named(b, object, address, value, access->index, access->shape);
  return IR_NONE;
}

static ir_node_id_t opt_value_phi(opt_builder_t *b, uint32_t stack_index, const ir_node_id_t *inputs, uint32_t count);

static ir_node_id_t opt_shape_access(opt_builder_t *b, ir_node_id_t object, ir_node_id_t address, ir_node_id_t value,
  const opt_field_case_t *cases, const uint32_t *members, uint32_t count, uint32_t result_index, ir_block_id_t *slow, uint32_t *slow_count) {
  if (count == 1) return opt_named_access(b, object, address, value, &cases[members[0]], slow, slow_count);
  ir_node_id_t prototype = opt_emit(b, ir_op(IR_OP_LOAD_PROTOTYPE), &address, 1, OPT_REPR_TAGGED);
  ir_block_id_t ends[OPT_MAX_FIELD_SHAPES];
  ir_node_id_t loaded[OPT_MAX_FIELD_SHAPES];
  for (uint32_t j = 0; j < count; j++) {
    const opt_field_case_t *access = &cases[members[j]];
    if (j + 1 < count || slow) {
      ir_condition_t same = ir_cond(IR_COND_TAGGED_EQUALS);
      same.equal = true;
      ir_block_id_t hit, miss;
      opt_branch_both_ways(b, same, (ir_node_id_t[]){prototype, opt_constant(b, access->values[0])}, 2, &hit, &miss);
      b->block = hit;
      loaded[j] = opt_holder_load(b, IR_NONE, access, slow, slow_count);
      ends[j] = b->block;
      b->block = miss;
      if (j + 1 == count) slow[(*slow_count)++] = miss;
    } else {
      loaded[j] = opt_holder_load(b, prototype, access, NULL, NULL);
      ends[j] = b->block;
    }
  }
  opt_join_blocks(b, ends, count);
  return opt_value_phi(b, result_index, loaded, count);
}

static opt_flow_t opt_build_string_field(opt_builder_t *b, const sv_ic_entry_t *ic, const sv_atom_t *atom, sv_op_t opcode) {
  ant_t *js = b->js;
  ant_value_t proto = js_primitive_prototype(js, kTypeString);
  ant_object_t *holder = ic->cached_holder;
  if (vtype(ic->guard.receiver_proto) != kTypeString || !holder || !is_object_type(proto) || js_obj_ptr(js_as_obj(proto)) != holder ||
      holder->flags.is_exotic || ic->cached_index >= holder->prop_count ||
      !opt_field_ok(js, holder->shape, ic->cached_index, atom, false)) return OPT_FLOW_GENERIC;
  if (!opt_may_speculate(b, OPT_EXIT_BAD_TYPE) || !opt_may_speculate(b, OPT_EXIT_BAD_SHAPE)) return OPT_FLOW_GENERIC;
  opt_field_case_t access = {.shape = holder->shape, .index = ic->cached_index, .depth = 1, .values = {proto}, .links = {holder}};
  if (!opt_field_case_retain(b, &access)) return OPT_FLOW_GENERIC;
  ir_node_id_t object = opt_tagged(b, opt_peek(b, 0));
  opt_emit_checked(b, ir_op(IR_OP_CHECK_STRING), &object, 1, OPT_REPR_NONE);
  ir_node_id_t loaded = opt_holder_load(b, IR_NONE, &access, NULL, NULL);
  if (opcode == OP_GET_FIELD) opt_drop(b, 1);
  opt_push(b, loaded);
  return OPT_FLOW_CONTINUE;
}

static uint8_t opt_length_feedback(const opt_builder_t *b) {
  uint8_t *feedback = sv_func_type_feedback(b->func);
  if (!feedback || opt_op_at(b, b->pc) != OP_GET_LENGTH) return 0;
  return feedback[b->pc] & SV_TFB_CLASS_MASK;
}

static opt_flow_t opt_build_array_length(opt_builder_t *b, ir_node_id_t value) {
  ir_op_t check = ir_op(IR_OP_CHECK_ELEMENTS);
  check.elements = IR_ELEMENTS_HOLEY;
  ir_node_id_t array = opt_emit_checked(b, check, &value, 1, OPT_REPR_NONE);
  ir_node_id_t address = opt_emit1(b, IR_OP_CELL_ADDRESS, array, OPT_REPR_POINTER);
  ir_node_id_t length = opt_emit1(b, IR_OP_LOAD_ELEMENTS_LENGTH, address, OPT_REPR_INT32);
  opt_drop(b, 1);
  opt_push(b, opt_tagged(b, length));
  return OPT_FLOW_CONTINUE;
}

static void opt_string_length(opt_builder_t *b, ir_node_id_t value, ir_node_id_t *lengths, ir_block_id_t *ends) {
  ir_block_id_t unresolved;
  ir_node_id_t string = opt_emit1(b, IR_OP_STRING_ADDRESS, value, OPT_REPR_POINTER);
  opt_branch_off(b, ir_cond(IR_COND_ASCII_STRING), &string, 1, false, &unresolved);
  lengths[0] = opt_emit1(b, IR_OP_STRING_LENGTH, string, OPT_REPR_INT32);
  ends[0] = b->block;
  b->block = unresolved;
  ir_block(b->graph, unresolved)->is_cold = true;
  ir_op_t call = ir_op(IR_OP_CALL_NATIVE);
  call.call.func = b->func;
  call.call.bc_off = b->pc;
  call.call.stores_operands = true;
  call.call.helper = (uintptr_t)opt_rt_string_length;
  call.call.computes_only = true;
  lengths[1] = opt_emit(b, call, (ir_node_id_t[]){value, opt_constant(b, IR_VALUE_UNDEFINED)}, 2, OPT_REPR_INT32);
  ir_node(b->graph, lengths[1])->frame_state = opt_call_position(b);
  ends[1] = b->block;
}

static opt_flow_t opt_build_string_length(opt_builder_t *b, ir_node_id_t value) {
  value = opt_emit_checked(b, ir_op(IR_OP_CHECK_STRING), &value, 1, OPT_REPR_NONE);
  ir_node_id_t lengths[2];
  ir_block_id_t ends[2];
  opt_string_length(b, value, lengths, ends);
  for (uint32_t i = 0; i < 2; i++) {
    b->block = ends[i];
    lengths[i] = opt_tagged(b, lengths[i]);
  }
  opt_join_blocks(b, ends, 2);
  opt_drop(b, 1);
  opt_push(b, opt_value_phi(b, b->frame.sp, lengths, 2));
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_length(opt_builder_t *b) {
  ir_node_id_t value = opt_tagged(b, opt_peek(b, 0));
  uint8_t seen = opt_length_feedback(b);
  if (seen == SV_TFB_LENGTH_ARRAY && opt_may_speculate(b, OPT_EXIT_BAD_ELEMENTS)) return opt_build_array_length(b, value);
  if (seen == SV_TFB_LENGTH_STRING && opt_may_speculate(b, OPT_EXIT_BAD_TYPE)) return opt_build_string_length(b, value);
  opt_frame_t before = opt_frame_clone(b, &b->frame);
  ir_block_id_t slow[1], not_array, not_string;
  ir_condition_t elements = ir_cond(IR_COND_ELEMENTS_KIND);
  elements.elements = IR_ELEMENTS_HOLEY;
  opt_branch_off(b, elements, &value, 1, false, &not_array);
  ir_node_id_t array = opt_refine(b, elements, &value, 1);
  ir_node_id_t address = opt_emit1(b, IR_OP_CELL_ADDRESS, array, OPT_REPR_POINTER);
  ir_node_id_t lengths[3];
  lengths[0] = opt_tagged(b, opt_emit1(b, IR_OP_LOAD_ELEMENTS_LENGTH, address, OPT_REPR_INT32));
  ir_block_id_t ends[3] = {b->block};
  b->block = not_array;
  opt_branch_off(b, ir_cond(IR_COND_STRING), &value, 1, false, &not_string);
  opt_string_length(b, opt_refine(b, ir_cond(IR_COND_STRING), &value, 1), lengths + 1, ends + 1);
  for (uint32_t i = 1; i < 3; i++) {
    b->block = ends[i];
    lengths[i] = opt_tagged(b, lengths[i]);
  }
  slow[0] = not_string;
  opt_join_blocks(b, ends, 3);
  opt_drop(b, 1);
  opt_push(b, opt_value_phi(b, b->frame.sp, lengths, 3));
  ir_block_id_t fast_end = b->block;
  opt_frame_t fast = b->frame;
  ir_block_id_t slow_end = opt_build_slow_path_or_exit(b, slow, 1, &before);
  opt_frame_t slow_frame = b->frame;
  b->frame = fast;
  opt_join_slow_path(b, fast_end, slow_end, &slow_frame, 1);
  return OPT_FLOW_CONTINUE;
}

static_assert(OPT_MAX_ADDS <= OPT_MAX_FIELD_SHAPES, "shape groups hold every addition case");

typedef struct {
  uint32_t count;
  ant_shape_t *shapes[OPT_MAX_FIELD_SHAPES];
  uint32_t members[OPT_MAX_FIELD_SHAPES][OPT_MAX_FIELD_SHAPES];
  uint32_t sizes[OPT_MAX_FIELD_SHAPES];
} opt_shape_groups_t;

typedef struct {
  sv_op_t opcode;
  bool store;
  uint16_t ic_index;
  opt_field_case_t cases[OPT_MAX_FIELD_SHAPES];
  uint32_t count;
  opt_add_case_t adds[OPT_MAX_ADDS];
  uint32_t add_count;
  ant_shape_t *runtime_adds[OPT_MAX_ADDS];
  uint32_t runtime_count;
  opt_rt_add_site_t *add_site;
  opt_shape_groups_t groups;
  opt_shape_groups_t add_groups;
} opt_field_plan_t;

static void opt_shape_groups_add(opt_shape_groups_t *groups, ant_shape_t *shape, uint32_t member) {
  uint32_t g = 0;
  while (g < groups->count && groups->shapes[g] != shape) g++;
  if (g == groups->count) groups->shapes[groups->count++] = shape;
  groups->members[g][groups->sizes[g]++] = member;
}

static opt_flow_t opt_build_field_without_cases(opt_builder_t *b, const sv_ic_entry_t *ic, uint16_t ic_index, bool store, sv_op_t opcode) {
  bool cached = ic->cached_shape || (ic->shape_ref_mask & (SV_IC_HAS_GET_POLY | SV_IC_HAS_PUT_POLY | SV_IC_POLY_MEGA));
  if (!store && (cached || ic->cached_aux)) return opt_build_probed_field(b, b->func, ic_index, opcode);
  if (cached || (store && ic->guard.add.to_shape)) return opt_build_access_call(b, false);
  if (store || ic->cached_aux) return opt_build_out_of_line(b);
  return opt_end_with_exit(b, OPT_EXIT_NO_FEEDBACK);
}

static void opt_drop_known_runtime_adds(opt_builder_t *b, opt_field_plan_t *plan) {
  for (uint32_t i = 0; i < plan->runtime_count;) {
    bool known = false;
    for (uint32_t j = 0; j < plan->count; j++) known |= plan->cases[j].shape == plan->runtime_adds[i];
    for (uint32_t j = 0; j < plan->add_count; j++) known |= plan->adds[j].from == plan->runtime_adds[i];
    if (known || !jit_snapshot_shape(b->js, plan->runtime_adds[i])) plan->runtime_adds[i] = plan->runtime_adds[--plan->runtime_count];
    else i++;
  }
}

static bool opt_field_add_site(opt_builder_t *b, opt_field_plan_t *plan) {
  if (!plan->add_count) return true;
  opt_rt_add_site_t *site = code_arena_bump(b->js, sizeof *site + plan->add_count * sizeof *site->adds);
  if (!site) return false;
  *site = (opt_rt_add_site_t){.site = {.func = b->func, .bc_off = b->pc}, .count = plan->add_count};
  for (uint32_t i = 0; i < plan->add_count; i++) {
    const opt_add_case_t *add = &plan->adds[i];
    uint32_t *cell = code_arena_bump(b->js, sizeof *cell);
    if (!cell || !jit_snapshot_shape(b->js, add->from) || !jit_snapshot_shape(b->js, add->to)) return false;
    *cell = add->epoch;
    site->adds[i] = (jit_add_cell_t){.from = add->from, .to = add->to, .proto = add->proto, .cell = cell};
  }
  plan->add_site = site;
  return true;
}

static bool opt_retain_field_plan(opt_builder_t *b, opt_field_plan_t *plan) {
  for (uint32_t i = 0; i < plan->count; i++)
    if (!opt_field_case_retain(b, &plan->cases[i])) return false;
  opt_field_memo_store(b, plan->cases, plan->count);
  opt_drop_known_runtime_adds(b, plan);
  return opt_field_add_site(b, plan);
}

static opt_flow_t opt_build_monomorphic_field(opt_builder_t *b, const opt_field_plan_t *plan, ir_node_id_t object, ir_node_id_t address, ir_node_id_t value) {
  const opt_field_case_t *access = &plan->cases[0];
  ir_node_id_t receiver = object;
  object = opt_check_shape(b, object, address, access->shape);
  ir_node_id_t loaded = opt_named_access(b, object, address, value, access, NULL, NULL);
  if (plan->store) {
    opt_drop(b, 2);
    return OPT_FLOW_CONTINUE;
  }
  ant_value_t known = opt_likely_value(b, receiver);
  ant_object_t *known_object = !access->depth && known ? opt_prototype_object(b->js, known) : NULL;
  if (known_object && known_object->shape == access->shape)
    opt_note_likely_value(b, loaded, ant_object_prop_get_unchecked(known_object, access->index));
  if (plan->opcode == OP_GET_FIELD) opt_drop(b, 1);
  opt_push(b, loaded);
  return OPT_FLOW_CONTINUE;
}

static ir_block_id_t opt_field_shape_switch(opt_builder_t *b, const opt_field_plan_t *plan, ir_node_id_t object, ir_node_id_t address, bool fallback,
  ir_shape_case_t *switch_cases, ir_block_id_t *runtime, ir_block_id_t *slow, uint32_t *slow_count) {
  ir_block_id_t source = b->block;
  uint32_t total = 0;
  for (uint32_t g = 0; g < plan->groups.count; g++)
    switch_cases[total++] = (ir_shape_case_t){.check = {.shape = (ir_cell_t)(uintptr_t)plan->groups.shapes[g]}, .block = opt_edge_block(b, source)};
  for (uint32_t g = 0; g < plan->add_groups.count; g++)
    switch_cases[total++] = (ir_shape_case_t){.check = {.shape = (ir_cell_t)(uintptr_t)plan->add_groups.shapes[g]}, .block = opt_edge_block(b, source)};
  for (uint32_t i = 0; i < plan->runtime_count; i++) {
    runtime[i] = opt_edge_block(b, source);
    switch_cases[total++] = (ir_shape_case_t){.check = {.shape = (ir_cell_t)(uintptr_t)plan->runtime_adds[i]}, .block = runtime[i]};
  }
  ir_block_id_t probe = IR_NONE;
  if (fallback && !plan->store) probe = switch_cases[total].block = opt_edge_block(b, source);
  else if (fallback) switch_cases[total].block = slow[(*slow_count)++] = opt_edge_block(b, source);
  if (fallback) switch_cases[total].check = (ir_shape_check_t){.shape = 0};
  ir_op_t shape_switch = ir_op(IR_OP_SHAPE_SWITCH);
  shape_switch.cases.items = ir_dup_cases(b->graph, switch_cases, total + fallback);
  shape_switch.cases.len = total + fallback;
  ir_frame_state_id_t state = opt_eager_frame_state(b);
  ir_node(b->graph, opt_set_control(b, shape_switch, (ir_node_id_t[]){object, address}, 2))->frame_state = state;
  return probe;
}

static opt_flow_t opt_finish_field_slow_paths(opt_builder_t *b, const opt_field_plan_t *plan, const opt_frame_t *before,
  const ir_block_id_t *runtime, const ir_block_id_t *slow, uint32_t slow_count) {
  ir_block_id_t fast_end = b->block;
  opt_frame_t fast = b->frame;
  if (plan->runtime_count) {
    ir_block_id_t runtime_end = opt_build_slow_path_counted(b, runtime, plan->runtime_count, before, false);
    opt_frame_t runtime_frame = b->frame;
    b->frame = fast;
    opt_join_slow_path(b, fast_end, runtime_end, &runtime_frame, 0);
    fast_end = b->block;
    fast = b->frame;
  }
  if (!slow_count) return OPT_FLOW_CONTINUE;
  b->slow_adds = plan->add_site;
  ir_block_id_t slow_end = opt_build_slow_path(b, slow, slow_count, before);
  b->slow_adds = NULL;
  opt_frame_t slow_frame = b->frame;
  b->frame = fast;
  opt_join_slow_path(b, fast_end, slow_end, &slow_frame, plan->store ? 0 : 1);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_polymorphic_field(opt_builder_t *b, opt_field_plan_t *plan, ir_node_id_t object, ir_node_id_t address, ir_node_id_t value, bool fallback) {
  for (uint32_t i = 0; i < plan->count; i++) opt_shape_groups_add(&plan->groups, plan->cases[i].shape, i);
  for (uint32_t i = 0; i < plan->add_count; i++) opt_shape_groups_add(&plan->add_groups, plan->adds[i].from, i);
  const opt_shape_groups_t *groups = &plan->groups, *add_groups = &plan->add_groups;
  uint32_t result_index = b->frame.sp - (plan->opcode == OP_GET_FIELD ? 1 : 0);

  opt_frame_t before = opt_frame_clone(b, &b->frame);
  ir_shape_case_t switch_cases[OPT_MAX_FIELD_SHAPES + 2 * OPT_MAX_ADDS + 1];
  ir_block_id_t ends[OPT_MAX_FIELD_SHAPES + OPT_MAX_ADDS + 1], slow[2 + 3 * OPT_MAX_FIELD_SHAPES + OPT_MAX_ADDS];
  ir_block_id_t runtime[OPT_MAX_ADDS];
  ir_node_id_t loaded[OPT_MAX_FIELD_SHAPES + 1];
  uint32_t slow_count = 0;
  ir_block_id_t probe = opt_field_shape_switch(b, plan, object, address, fallback, switch_cases, runtime, slow, &slow_count);
  for (uint32_t g = 0; g < groups->count; g++) {
    b->block = switch_cases[g].block;
    loaded[g] = opt_shape_access(b, object, address, value, plan->cases, groups->members[g], groups->sizes[g], result_index, fallback ? slow : NULL, &slow_count);
    ends[g] = b->block;
  }
  for (uint32_t g = 0; g < add_groups->count; g++) {
    b->block = switch_cases[groups->count + g].block;
    opt_add_fields(b, object, address, value, plan->adds, plan->add_site, add_groups->members[g], add_groups->sizes[g], slow, &slow_count);
    ends[groups->count + g] = b->block;
  }
  uint32_t paths = groups->count + add_groups->count;
  if (probe != IR_NONE) {
    b->block = probe;
    loaded[paths] = opt_probe_field(b, b->func, plan->ic_index, object, address, slow, &slow_count);
    ends[paths++] = b->block;
  }
  if (paths > 1) opt_join_blocks(b, ends, paths);
  if (plan->store) opt_drop(b, 2);
  else {
    if (plan->opcode == OP_GET_FIELD) opt_drop(b, 1);
    opt_push(b, paths > 1 ? opt_value_phi(b, result_index, loaded, paths) : loaded[0]);
  }
  return opt_finish_field_slow_paths(b, plan, &before, runtime, slow, slow_count);
}

static bool opt_is_literal(const opt_builder_t *b, ir_node_id_t node);

static void opt_narrow_to_prototype(opt_builder_t *b, opt_field_plan_t *plan, ir_node_id_t object) {
  const ir_node_t *node = ir_node(b->graph, object);
  uint64_t proto;
  if (node->op.code != IR_OP_ALLOCATE_OBJECT || node->inputs.len != 1 || opt_is_literal(b, object) || !ir_constant_value(b->graph, node->inputs.items[0], &proto)) return;
  uint32_t kept = 0;
  for (uint32_t i = 0; i < plan->count; i++) kept += !plan->cases[i].depth || plan->cases[i].values[0] == proto;
  if (!kept || kept == plan->count) return;
  kept = 0;
  for (uint32_t i = 0; i < plan->count; i++)
    if (!plan->cases[i].depth || plan->cases[i].values[0] == proto) plan->cases[kept++] = plan->cases[i];
  plan->count = kept;
}

static bool opt_getter_plan(opt_builder_t *b, opt_field_plan_t *plan) {
  uint32_t getters = 0;
  for (uint32_t i = 0; i < plan->count; i++) getters += plan->cases[i].getter != 0;
  if (!getters) return false;
  bool usable = getters == plan->count && opt_may_speculate(b, OPT_EXIT_BAD_SHAPE);
  for (uint32_t i = 0; usable && i < plan->count; i++)
    for (uint32_t j = 0; j < plan->count; j++)
      if (i != j && !plan->cases[i].depth && plan->cases[i].shape == plan->cases[j].shape) usable = false;
  for (uint32_t i = 0; usable && i < plan->count; i++) usable = opt_retain(b, plan->cases[i].getter);
  if (usable) return true;
  uint32_t kept = 0;
  for (uint32_t i = 0; i < plan->count; i++)
    if (!plan->cases[i].getter) plan->cases[kept++] = plan->cases[i];
  plan->count = kept;
  return false;
}

static opt_flow_t opt_build_getter_field(opt_builder_t *b, const opt_field_plan_t *plan, ir_node_id_t object, ir_node_id_t address);

static opt_flow_t opt_build_field(opt_builder_t *b, const uint8_t *ip, sv_op_t opcode) {
  sv_func_t *func = b->func;
  uint32_t atom_index = sv_get_u32(ip + 1);
  uint16_t ic_index = sv_get_u16(ip + 5);
  bool store = opcode == OP_PUT_FIELD;
  if (atom_index >= (uint32_t)func->atom_count || !func->ic_slots || ic_index >= func->ic_count) return OPT_FLOW_GENERIC;
  const sv_atom_t *atom = &func->atoms[atom_index];
  sv_ic_entry_t *ic = &func->ic_slots[ic_index];
  if (!opt_may_speculate(b, OPT_EXIT_SLOW_PATH)) return opt_build_access_call(b, false);
  if (!store && ic->get_kind == SV_GF_IC_PRIMITIVE_DATA) {
    opt_flow_t flow = opt_build_string_field(b, ic, atom, opcode);
    if (flow != OPT_FLOW_GENERIC) return flow;
  }
  if (!store && opcode == OP_GET_FIELD && atom->len == 6 && memcmp(atom->str, "length", 6) == 0) return opt_build_length(b);
  opt_field_plan_t plan = {.opcode = opcode, .store = store, .ic_index = ic_index};
  plan.count = opt_field_memo_merge(b, atom, store, plan.cases, opt_field_cases(b, ic, atom, store, plan.cases));
  plan.add_count = store ? opt_add_cases(b->js, ic, atom, plan.adds, plan.runtime_adds, &plan.runtime_count) : 0;
  if (store && !plan.add_count && ic->guard.add.to_shape) return opt_build_access_call(b, false);
  if (!store) opt_narrow_to_prototype(b, &plan, opt_peek(b, 0));
  bool getters = opt_getter_plan(b, &plan);
  if (!plan.count && !plan.add_count) return opt_build_field_without_cases(b, ic, ic_index, store, opcode);
  if (!opt_retain_field_plan(b, &plan)) return OPT_FLOW_GENERIC;

  ir_node_id_t object = opt_tagged(b, opt_peek(b, store ? 1 : 0));
  ir_node_id_t value = store ? opt_tagged(b, opt_peek(b, 0)) : IR_NONE;
  ir_node_id_t address = opt_object_address(b, &object);
  if (address == IR_NONE) return opt_build_access_call(b, false);
  if (getters) return opt_build_getter_field(b, &plan, object, address);
  bool fallback = plan.add_count || !opt_may_speculate(b, OPT_EXIT_BAD_SHAPE);
  if (!fallback) opt_vec_push(&b->graph->arena, &b->ctx->object_like, object);
  if (plan.count == 1 && !fallback) return opt_build_monomorphic_field(b, &plan, object, address, value);
  return opt_build_polymorphic_field(b, &plan, object, address, value, fallback);
}

static ir_node_id_t opt_probe_field(opt_builder_t *b, sv_func_t *func, uint16_t ic_index, ir_node_id_t object, ir_node_id_t address, ir_block_id_t *slow, uint32_t *slow_count) {
  ir_op_t op = ir_op(IR_OP_PROBE_PROPERTY_CACHE);
  op.probe.func = func;
  op.probe.cache = ic_index;
  ir_node_id_t value = opt_emit(b, op, (ir_node_id_t[]){object, address}, 2, OPT_REPR_TAGGED);
  ir_condition_t empty = ir_cond(IR_COND_TAGGED_EQUALS);
  empty.equal = true;
  opt_branch_off(b, empty, (ir_node_id_t[]){value, opt_constant(b, IR_VALUE_EMPTY)}, 2, true, &slow[(*slow_count)++]);
  return value;
}

static opt_flow_t opt_build_probed_field(opt_builder_t *b, sv_func_t *func, uint16_t ic_index, sv_op_t opcode) {
  ir_node_id_t object = opt_tagged(b, opt_peek(b, 0));
  ir_node_id_t address = opt_object_address(b, &object);
  if (address == IR_NONE) return opt_build_access_call(b, false);
  opt_frame_t before = opt_frame_clone(b, &b->frame);
  ir_block_id_t slow[1];
  uint32_t slow_count = 0;
  ir_node_id_t value = opt_probe_field(b, func, ic_index, object, address, slow, &slow_count);
  if (opcode == OP_GET_FIELD) opt_drop(b, 1);
  opt_push(b, value);
  ir_block_id_t fast_end = b->block;
  opt_frame_t fast = b->frame;
  ir_block_id_t slow_end = opt_build_slow_path(b, slow, slow_count, &before);
  opt_frame_t slow_frame = b->frame;
  b->frame = fast;
  opt_join_slow_path(b, fast_end, slow_end, &slow_frame, 1);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_out_of_line(opt_builder_t *b) {
  if (b->inline_call) return opt_end_with_exit(b, OPT_EXIT_NO_FEEDBACK);
  if (!opt_generic_supported(opt_op_at(b, b->pc))) return OPT_FLOW_GENERIC;
  opt_frame_t before = opt_frame_clone(b, &b->frame);
  ir_block_id_t entry = opt_edge_block(b, b->block);
  ir_op_t jump = ir_op(IR_OP_JUMP);
  jump.target = entry;
  opt_set_control(b, jump, NULL, 0);
  ir_block_id_t end = opt_build_slow_path(b, &entry, 1, &before);
  ir_block_id_t next = opt_edge_block(b, end);
  b->block = end;
  jump.target = next;
  opt_set_control(b, jump, NULL, 0);
  b->block = next;
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_global_call(opt_builder_t *b) {
  opt_rt_site_t *site = code_arena_bump(b->js, sizeof *site);
  if (!site) return OPT_FLOW_GENERIC;
  *site = (opt_rt_site_t){.func = b->func, .bc_off = b->pc};
  ir_node_id_t undefined = opt_constant(b, IR_VALUE_UNDEFINED);
  opt_push(b, opt_runtime_call(b, (uintptr_t)opt_rt_get_global, (uintptr_t)site, (ir_node_id_t[]){undefined, undefined}, 2, false, false));
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_get_global(opt_builder_t *b, const uint8_t *ip) {
  sv_func_t *func = b->func;
  ant_t *js = b->js;
  uint32_t atom_index = sv_get_u32(ip + 1);
  uint16_t ic_index = sv_get_u16(ip + 5);
  if (atom_index >= (uint32_t)func->atom_count || !func->ic_slots || ic_index >= func->ic_count) return OPT_FLOW_GENERIC;
  const sv_atom_t *atom = &func->atoms[atom_index];
  if (atom->len == 5 && memcmp(atom->str, "super", 5) == 0) return OPT_FLOW_GENERIC;
  sv_ic_entry_t *ic = &func->ic_slots[ic_index];
  ant_shape_t *shape = ic->cached_shape;
  if (!shape) return opt_may_speculate(b, OPT_EXIT_NO_FEEDBACK) ? opt_end_with_exit(b, OPT_EXIT_NO_FEEDBACK) : opt_build_global_call(b);
  ant_object_t *global = is_object_type(js->global) ? js_obj_ptr(js_as_obj(js->global)) : NULL;
  uint32_t index = ic->cached_index;
  if (!global || global->flags.is_exotic || ic->epoch != js->ic.epoch || global->shape != shape ||
      index >= global->prop_count || index > 0xffff || sv_global_lexical(js, atom->str)) return opt_build_global_call(b);
  const ant_shape_prop_t *prop = ant_shape_prop_at(shape, index);
  if (!prop || prop->type != ANT_SHAPE_KEY_STRING || prop->key.interned != atom->str || prop->has_getter || prop->has_setter)
    return opt_build_global_call(b);
  if (!jit_snapshot_shape(js, shape)) return opt_build_global_call(b);

  ir_op_t lexicals = ir_op(IR_OP_CHECK_GLOBAL_LEXICALS);
  lexicals.count = js->global_lexical_count;
  opt_emit_checked(b, lexicals, NULL, 0, OPT_REPR_NONE);
  ir_node_id_t object = opt_constant(b, js->global);
  ir_node_id_t address = opt_typed_constant(b, (uint64_t)(uintptr_t)global, OPT_REPR_POINTER);
  object = opt_check_shape(b, object, address, shape);
  ir_node_id_t loaded = opt_load_named(b, object, address, index, shape);
  opt_note_likely_value(b, loaded, ant_object_prop_get_unchecked(global, index));
  opt_push(b, loaded);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_upvalue(opt_builder_t *b, const uint8_t *ip, sv_op_t op) {
  uint32_t index = sv_get_u16(ip + 1);
  if (index >= (uint32_t)b->func->upvalue_count) return OPT_FLOW_GENERIC;
  uint64_t bits;
  sv_closure_t *known = NULL;
  if (b->inline_call && ir_constant_value(b->graph, b->inline_call->closure, &bits) && vtype(bits) == kTypeFunction)
    known = js_func_closure(bits);
  sv_closure_t *owner = b->inline_call ? known : b->ctx->closure;
  if (op == OP_GET_UPVAL_VAL) {
    ant_value_t value = known && known->func == b->func && known->upvalues ? (ant_value_t)(uintptr_t)known->upvalues[index] : 0;
    if (value && opt_retain(b, value)) {
      opt_push(b, opt_constant(b, value));
      return OPT_FLOW_CONTINUE;
    }
    ir_op_t load_value = ir_op(IR_OP_LOAD_UPVALUE_CELL);
    load_value.index = index;
    ir_node_id_t closure = b->inline_call ? b->inline_call->closure : IR_NONE;
    ir_node_id_t loaded = opt_emit(b, load_value, &closure, closure != IR_NONE, OPT_REPR_TAGGED);
    if (owner && owner->func == b->func && owner->upvalues) opt_note_likely_value(b, loaded, (ant_value_t)(uintptr_t)owner->upvalues[index]);
    opt_push(b, loaded);
    return OPT_FLOW_CONTINUE;
  }
  ant_value_t *location = owner && owner->func == b->func && owner->upvalues && owner->upvalues[index] ? owner->upvalues[index]->location : NULL;
  bool initialized = !b->func->upval_descs[index].may_tdz || (known && location && *location != SV_TDZ);
  if ((op == OP_GET_UPVAL && jit_upvalue_is_builder_target(b->func, index)) || (!initialized && !opt_may_speculate(b, OPT_EXIT_UNEXPECTED_VALUE)))
    return OPT_FLOW_GENERIC;
  ir_op_t load_cell = ir_op(IR_OP_LOAD_UPVALUE_CELL);
  load_cell.index = index;
  ir_node_id_t closure = b->inline_call ? b->inline_call->closure : IR_NONE;
  ir_node_id_t cell = opt_emit(b, load_cell, &closure, closure != IR_NONE, OPT_REPR_POINTER);
  if (!initialized) opt_emit_checked(b, ir_op(IR_OP_CHECK_UPVALUE_INITIALIZED), &cell, 1, OPT_REPR_NONE);
  if (op == OP_GET_UPVAL) {
    ir_op_t load = ir_op(IR_OP_LOAD_UPVALUE);
    load.initialized = true;
    ir_node_id_t loaded = opt_emit(b, load, &cell, 1, OPT_REPR_TAGGED);
    if (location) opt_note_likely_value(b, loaded, *location);
    opt_push(b, loaded);
    return OPT_FLOW_CONTINUE;
  }
  ir_node_id_t value = opt_tagged(b, opt_peek(b, 0));
  opt_emit(b, ir_op(IR_OP_STORE_UPVALUE), (ir_node_id_t[]){cell, value}, 2, OPT_REPR_NONE);
  if (op == OP_PUT_UPVAL) opt_drop(b, 1);
  return OPT_FLOW_CONTINUE;
}

static ir_block_id_t opt_build_slow_path_counted(opt_builder_t *b, const ir_block_id_t *entries, uint32_t count, const opt_frame_t *before, bool counted) {
  ir_block_id_t slow = count == 1 ? entries[0] : opt_join_blocks(b, entries, count);
  ir_block(b->graph, slow)->is_cold = true;
  b->block = slow;
  b->frame = opt_frame_clone(b, before);
  if (opt_build_access_call(b, counted) == OPT_FLOW_CONTINUE) return b->block;
  if (b->inline_call) {
    if (opt_end_with_exit(b, OPT_EXIT_SLOW_PATH) != OPT_FLOW_ENDED) opt_fail(b, "inline-slow-path");
    return IR_NONE;
  }
  b->slow_path = true;
  [[maybe_unused]] opt_flow_t flow = opt_build_generic(b);
  b->slow_path = false;
  return b->block;
}

static ir_block_id_t opt_build_slow_path(opt_builder_t *b, const ir_block_id_t *entries, uint32_t count, const opt_frame_t *before) {
  return opt_build_slow_path_counted(b, entries, count, before, opt_may_speculate(b, OPT_EXIT_SLOW_PATH));
}

static ir_block_id_t opt_build_slow_path_or_exit(opt_builder_t *b, const ir_block_id_t *entries, uint32_t count, const opt_frame_t *before) {
  if (!opt_may_speculate(b, OPT_EXIT_SLOW_PATH)) return opt_build_slow_path_counted(b, entries, count, before, false);
  ir_block_id_t slow = count == 1 ? entries[0] : opt_join_blocks(b, entries, count);
  ir_block(b->graph, slow)->is_cold = true;
  b->block = slow;
  b->frame = opt_frame_clone(b, before);
  [[maybe_unused]] opt_flow_t flow = opt_end_with_exit(b, OPT_EXIT_SLOW_PATH);
  return IR_NONE;
}

static void opt_join_slow_path(opt_builder_t *b, ir_block_id_t fast_end, ir_block_id_t slow_end, const opt_frame_t *slow, uint32_t results) {
  if (slow_end == IR_NONE) {
    b->block = fast_end;
    return;
  }
  opt_frame_t fast = b->frame;
  opt_frame_t joined = opt_frame_clone(b, &fast);
  uint32_t first = fast.sp - results;
  ir_node_id_t inputs[2][4];
  for (uint32_t k = first; k < fast.sp; k++) {
    uint32_t t = opt_stack_index(b, k);
    b->block = fast_end;
    inputs[k - first][0] = opt_tagged(b, fast.slots[t].node);
    b->block = slow_end;
    inputs[k - first][1] = opt_tagged(b, slow->slots[t].node);
  }
  opt_join_blocks(b, (ir_block_id_t[]){fast_end, slow_end}, 2);
  for (uint32_t t = 0; t < b->tracked; t++) {
    if (opt_in_memory(b, t)) joined.slots[t] = (opt_slot_t){.node = IR_NONE};
    else joined.slots[t].in_sync &= slow->slots[t].in_sync && slow->slots[t].node == fast.slots[t].node;
  }
  for (uint32_t k = first; k < fast.sp; k++)
    joined.slots[opt_stack_index(b, k)] = (opt_slot_t){.node = opt_add_phi(b, opt_stack_index(b, k), inputs[k - first], 2)};
  b->frame = joined;
}

static ir_node_id_t opt_element_index(opt_builder_t *b, ir_node_id_t key) {
  if (!opt_may_be_int32(b, key) || !opt_may_speculate(b, OPT_EXIT_NOT_INT32)) return IR_NONE;
  return opt_int32(b, key);
}

static ir_block_id_t opt_branch_off(opt_builder_t *b, ir_condition_t condition, const ir_node_id_t *inputs, uint32_t count, bool off_if, ir_block_id_t *off) {
  ir_block_id_t if_true, if_false;
  opt_branch_both_ways(b, condition, inputs, count, &if_true, &if_false);
  *off = off_if ? if_true : if_false;
  b->block = off_if ? if_false : if_true;
  return b->block;
}

static ir_node_id_t opt_elements_of(opt_builder_t *b, ir_node_id_t array, ir_elements_kind_t kind, ir_node_id_t *length, ir_block_id_t *slow, uint32_t *count) {
  ir_condition_t elements = ir_cond(IR_COND_ELEMENTS_KIND);
  elements.elements = kind;
  opt_branch_off(b, elements, &array, 1, false, &slow[(*count)++]);
  array = opt_refine(b, elements, &array, 1);
  ir_node_id_t address = opt_emit1(b, IR_OP_CELL_ADDRESS, array, OPT_REPR_POINTER);
  *length = opt_emit1(b, IR_OP_LOAD_ELEMENTS_LENGTH, address, OPT_REPR_INT32);
  return address;
}

static ir_node_id_t opt_load_element(opt_builder_t *b, ir_node_id_t address, ir_node_id_t index, ir_node_id_t length, uint32_t result_index, ir_block_id_t *slow, uint32_t *count) {
  ir_block_id_t in_bounds, past_end, hole;
  opt_branch_both_ways(b, ir_cond(IR_COND_INDEX_IN_BOUNDS), (ir_node_id_t[]){index, length}, 2, &in_bounds, &past_end);
  b->block = in_bounds;
  ir_node_id_t checked = opt_refine(b, ir_cond(IR_COND_INDEX_IN_BOUNDS), (ir_node_id_t[]){index, length}, 2);
  ir_op_t load = ir_op(IR_OP_LOAD_ELEMENT_AT);
  load.elements = IR_ELEMENTS_HOLEY;
  ir_node_id_t value = opt_emit(b, load, (ir_node_id_t[]){address, checked}, 2, OPT_REPR_TAGGED);
  ir_condition_t is_hole = ir_cond(IR_COND_TAGGED_EQUALS);
  is_hole.equal = true;
  opt_branch_off(b, is_hole, (ir_node_id_t[]){value, opt_constant(b, IR_VALUE_EMPTY)}, 2, true, &hole);
  ir_block_id_t loaded = b->block;
  b->block = past_end;
  ir_condition_t non_negative = ir_cond(IR_COND_INT32);
  non_negative.comparison = IR_CMP_GREATER_THAN_EQUALS;
  ir_block_id_t beyond = opt_branch_off(b, non_negative, (ir_node_id_t[]){index, opt_int32_constant(b, 0)}, 2, false, &slow[(*count)++]);
  opt_join_blocks(b, (ir_block_id_t[]){hole, beyond}, 2);
  ir_op_t plain = ir_op(IR_OP_CAN_ADD_ELEMENT);
  plain.bits = 1;
  ir_node_id_t absent = opt_emit(b, plain, &address, 1, OPT_REPR_BOOL);
  opt_branch_off(b, ir_cond(IR_COND_BOOL), &absent, 1, false, &slow[(*count)++]);
  opt_join_blocks(b, (ir_block_id_t[]){loaded, b->block}, 2);
  return opt_value_phi(b, result_index, (ir_node_id_t[]){value, opt_constant(b, IR_VALUE_UNDEFINED)}, 2);
}

static void opt_store_element(opt_builder_t *b, ir_node_id_t address, ir_node_id_t index, ir_node_id_t length, ir_node_id_t value, ir_block_id_t *slow, uint32_t *count, ir_block_id_t *grow, uint32_t *grows) {
  ir_block_id_t in_bounds, past_end, hole, append;
  opt_branch_both_ways(b, ir_cond(IR_COND_INDEX_IN_BOUNDS), (ir_node_id_t[]){index, length}, 2, &in_bounds, &past_end);
  b->block = in_bounds;
  ir_node_id_t checked = opt_refine(b, ir_cond(IR_COND_INDEX_IN_BOUNDS), (ir_node_id_t[]){index, length}, 2);
  ir_op_t load = ir_op(IR_OP_LOAD_ELEMENT_AT);
  load.elements = IR_ELEMENTS_HOLEY;
  ir_node_id_t old = opt_emit(b, load, (ir_node_id_t[]){address, checked}, 2, OPT_REPR_TAGGED);
  ir_condition_t is_hole = ir_cond(IR_COND_TAGGED_EQUALS);
  is_hole.equal = true;
  opt_branch_off(b, is_hole, (ir_node_id_t[]){old, opt_constant(b, IR_VALUE_EMPTY)}, 2, true, &hole);
  ir_op_t store = ir_op(IR_OP_STORE_ELEMENT_AT);
  store.elements = IR_ELEMENTS_PACKED;
  opt_emit(b, store, (ir_node_id_t[]){address, checked, value}, 3, OPT_REPR_NONE);
  ir_block_id_t stored = b->block;

  b->block = past_end;
  ir_condition_t at_end = ir_cond(IR_COND_INT32);
  at_end.comparison = IR_CMP_STRICTLY_EQUALS;
  opt_branch_off(b, at_end, (ir_node_id_t[]){index, length}, 2, false, &grow[(*grows)++]);
  ir_node_id_t capacity = opt_emit1(b, IR_OP_LOAD_ELEMENTS_CAPACITY, address, OPT_REPR_INT32);
  append = opt_branch_off(b, ir_cond(IR_COND_INDEX_IN_BOUNDS), (ir_node_id_t[]){index, capacity}, 2, false, &grow[(*grows)++]);
  opt_join_blocks(b, (ir_block_id_t[]){hole, append}, 2);
  ir_node_id_t allowed = opt_emit1(b, IR_OP_CAN_ADD_ELEMENT, address, OPT_REPR_BOOL);
  opt_branch_off(b, ir_cond(IR_COND_BOOL), &allowed, 1, false, &slow[(*count)++]);
  opt_emit(b, ir_op(IR_OP_APPEND_ELEMENT), (ir_node_id_t[]){address, index, value}, 3, OPT_REPR_NONE);
  opt_join_blocks(b, (ir_block_id_t[]){stored, b->block}, 2);
}

static void opt_build_speculated_element(opt_builder_t *b, sv_op_t op, ir_node_id_t array, ir_node_id_t index, ir_node_id_t value) {
  ir_op_t check = ir_op(IR_OP_CHECK_ELEMENTS);
  check.elements = op == OP_PUT_ELEM ? IR_ELEMENTS_PACKED : IR_ELEMENTS_HOLEY;
  ir_node_id_t object = opt_emit_checked(b, check, &array, 1, OPT_REPR_NONE);
  ir_node_id_t address = opt_emit1(b, IR_OP_CELL_ADDRESS, object, OPT_REPR_POINTER);
  ir_node_id_t length = opt_emit1(b, IR_OP_LOAD_ELEMENTS_LENGTH, address, OPT_REPR_INT32);
  index = opt_emit_checked(b, ir_op(IR_OP_CHECK_BOUNDS), (ir_node_id_t[]){index, length}, 2, OPT_REPR_INT32);
  ir_op_t load = ir_op(IR_OP_LOAD_ELEMENT_AT);
  load.elements = IR_ELEMENTS_HOLEY;
  ir_node_id_t element = opt_emit(b, load, (ir_node_id_t[]){address, index}, 2, OPT_REPR_TAGGED);
  element = opt_emit_checked(b, ir_op(IR_OP_CHECK_NOT_HOLE), &element, 1, OPT_REPR_TAGGED);
  opt_drop(b, op == OP_PUT_ELEM ? 3 : 2);
  if (op == OP_GET_ELEM) {
    opt_push(b, element);
    return;
  }
  ir_op_t store = ir_op(IR_OP_STORE_ELEMENT_AT);
  store.elements = IR_ELEMENTS_PACKED;
  opt_emit(b, store, (ir_node_id_t[]){address, index, value}, 3, OPT_REPR_NONE);
}

static opt_flow_t opt_build_element(opt_builder_t *b, sv_op_t op) {
  uint8_t *feedback = sv_func_type_feedback(b->func);
  if (op != OP_GET_LENGTH && (!feedback || !feedback[b->pc])) return opt_end_with_exit(b, OPT_EXIT_NO_FEEDBACK);
  uint32_t operands = op == OP_PUT_ELEM ? 3 : op == OP_GET_ELEM ? 2 : 1;
  ir_node_id_t array = opt_tagged(b, opt_peek(b, operands - 1)), index = IR_NONE;
  if (op != OP_GET_LENGTH && (feedback[b->pc] & SV_TFB_SPEC_MISMATCH) && opt_element_key_exited(b)) return opt_build_access_call(b, false);
  if (op != OP_GET_LENGTH && (index = opt_element_index(b, opt_peek(b, operands - 2))) == IR_NONE) return opt_build_access_call(b, false);
  ir_node_id_t value = op == OP_PUT_ELEM ? opt_tagged(b, opt_peek(b, 0)) : IR_NONE;
  bool slow_hot = !opt_may_speculate(b, OPT_EXIT_SLOW_PATH);
  bool dense = op != OP_GET_LENGTH && !sv_tfb_elem_saw_typed_array(feedback[b->pc]) && (sv_tfb_specialization_ready(feedback[b->pc])
    || (op == OP_GET_ELEM && (feedback[b->pc] & SV_TFB_CLASS_MASK) == SV_TFB_ELEMENT_DENSE));
  if (op != OP_GET_LENGTH && !slow_hot && dense && opt_may_speculate(b, OPT_EXIT_BAD_ELEMENTS)
    && opt_may_speculate(b, OPT_EXIT_OUT_OF_BOUNDS)) {
    opt_build_speculated_element(b, op, array, index, value);
    return OPT_FLOW_CONTINUE;
  }
  opt_frame_t before = opt_frame_clone(b, &b->frame);
  ir_block_id_t slow[6], grow[2];
  uint32_t count = 0, grows = 0;
  ir_node_id_t length;
  ir_node_id_t address = opt_elements_of(b, array, op == OP_PUT_ELEM ? IR_ELEMENTS_PACKED : IR_ELEMENTS_HOLEY, &length, slow, &count);
  ir_node_id_t result = IR_NONE;
  if (op == OP_GET_LENGTH) result = length;
  else if (op == OP_GET_ELEM) result = opt_load_element(b, address, index, length, b->frame.sp - operands, slow, &count);
  else opt_store_element(b, address, index, length, value, slow, &count, grow, &grows);
  opt_drop(b, operands);
  if (result != IR_NONE) opt_push(b, result);
  ir_block_id_t fast_end = b->block;
  opt_frame_t fast = b->frame;
  if (grows) {
    ir_block_id_t grow_end = opt_build_slow_path_counted(b, grow, grows, &before, false);
    opt_frame_t grown = b->frame;
    b->frame = fast;
    opt_join_slow_path(b, fast_end, grow_end, &grown, 0);
    fast_end = b->block;
    fast = b->frame;
  }
  ir_block_id_t slow_end = opt_build_slow_path_or_exit(b, slow, count, &before);
  opt_frame_t slow_frame = b->frame;
  b->frame = fast;
  opt_join_slow_path(b, fast_end, slow_end, &slow_frame, result != IR_NONE);
  return OPT_FLOW_CONTINUE;
}

static uint32_t opt_successor_after(opt_builder_t *b) {
  return b->blocks[b->block_of_ins[b->ins]].succ[b->blocks[b->block_of_ins[b->ins]].succ_count - 1];
}

static opt_flow_t opt_build_jump(opt_builder_t *b, sv_op_t op) {
  opt_bblock_t *bblock = &b->blocks[b->block_of_ins[b->ins]];
  if (op == OP_JMP || op == OP_JMP8) return opt_end_with_jump(b, bblock->succ[0]) ? OPT_FLOW_ENDED : OPT_FLOW_FAILED;
  bool peek = op == OP_JMP_FALSE_PEEK || op == OP_JMP_TRUE_PEEK;
  bool on_false = op == OP_JMP_FALSE || op == OP_JMP_FALSE8 || op == OP_JMP_FALSE_PEEK;
  ir_node_id_t value = opt_peek(b, 0);
  if (!peek) opt_drop(b, 1);
  uint32_t target = bblock->succ[0], next = opt_successor_after(b);
  uint32_t if_true = on_false ? next : target, if_false = on_false ? target : next;
  return opt_end_with_truthiness_branch(b, value, if_true, if_false) ? OPT_FLOW_ENDED : OPT_FLOW_FAILED;
}

static opt_flow_t opt_build_update(opt_builder_t *b, ir_binary_op_t op, ir_node_id_t value, ir_node_id_t *old, ir_node_id_t *result) {
  ir_node_id_t one = opt_constant(b, ir_value_int32(1));
  switch (opt_number_speculation(b, value, IR_NONE, true, true)) {
    case OPT_SPEC_INT32: {
      ir_node_id_t integer = opt_int32(b, value);
      *old = integer;
      *result = opt_int32_binary(b, op, integer, opt_int32_constant(b, 1));
      return OPT_FLOW_CONTINUE;
    }
    case OPT_SPEC_FLOAT64: {
      ir_node_id_t number = opt_number(b, value);
      ir_op_t ir = ir_op(IR_OP_FLOAT64_BINARY);
      ir.binary = op;
      ir_node_id_t inputs[2] = {number, opt_number(b, one)};
      *old = opt_emit1(b, IR_OP_BOX_FLOAT64, number, OPT_REPR_TAGGED);
      *result = opt_emit1(b, IR_OP_BOX_FLOAT64, opt_emit(b, ir, inputs, 2, OPT_REPR_FLOAT64), OPT_REPR_TAGGED);
      return OPT_FLOW_CONTINUE;
    }
    case OPT_SPEC_NONE: break;
  }
  return OPT_FLOW_GENERIC;
}

static ir_comparison_t opt_comparison_of(sv_op_t op) {
  switch (op) {
    case OP_LT: return IR_CMP_LESS_THAN;
    case OP_LE: return IR_CMP_LESS_THAN_EQUALS;
    case OP_GT: return IR_CMP_GREATER_THAN;
    default: return IR_CMP_GREATER_THAN_EQUALS;
  }
}

static ir_binary_op_t opt_binary_of(sv_op_t op) {
  switch (op) {
    case OP_ADD: case OP_ADD_NUM: return IR_BINARY_ADD;
    case OP_SUB: case OP_SUB_NUM: return IR_BINARY_SUB;
    case OP_MUL: case OP_MUL_NUM: return IR_BINARY_MUL;
    default: return IR_BINARY_DIV;
  }
}

static constexpr uint32_t OPT_MAX_CALL_ARGS = SV_JIT_ARGS_BUF_CAP;
static constexpr uint32_t OPT_INLINE_MAX_DEPTH = SV_JIT_INLINE_FRAMES_MAX;
static constexpr uint32_t OPT_INLINE_MAX_INSTRUCTIONS = 50;
static constexpr uint32_t OPT_INLINE_BUDGET = 400;

static bool opt_build_bytecode_block(opt_builder_t *b, uint32_t bi);
static bool opt_analyze(opt_builder_t *b);

static sv_func_t *opt_call_target(const opt_builder_t *b) {
  sv_func_t *target = sv_tfb_get_call_target(b->func, (int)b->pc);
  if (!target || target->is_async || target->is_generator || target->code_len <= 0) return NULL;
  return target;
}

static uint32_t opt_instruction_count(const sv_func_t *func) {
  uint32_t count = 0;
  for (uint32_t off = 0; off < (uint32_t)func->code_len;) {
    sv_op_t op = (sv_op_t)func->code[off];
    if (op >= OP__COUNT || !sv_op_size[op]) return UINT32_MAX;
    if (!(sv_op_flags[op] & SV_OPF_MARKER)) count++;
    off += sv_op_size[op];
  }
  return count;
}

static bool opt_reads_this(const sv_func_t *func) {
  for (uint32_t off = 0; off < (uint32_t)func->code_len; off += sv_op_size[func->code[off]])
    if (func->code[off] == OP_THIS) return true;
  return false;
}

static const char *opt_inline_try_op(const sv_func_t *func) {
  if (!func->has_try) return NULL;
  int *map = sv_func_try_map(func);
  if (!map) return "try-map";
  const char *refused = NULL;
  const uint8_t *feedback = sv_func_type_feedback(func);
  for (uint32_t off = 0; off < (uint32_t)func->code_len && !refused; off += sv_op_size[func->code[off]]) {
    sv_op_t op = (sv_op_t)func->code[off];
    if (map[off] >= 0 && !(sv_op_flags[op] & SV_OPF_OPT_INLINE_IN_TRY)) refused = sv_op_names[op];
    if ((op == OP_FOR_OF || op == OP_DESTRUCTURE_INIT) && (!feedback || feedback[off] != 1u << SV_ITER_ARRAY)) refused = sv_op_names[op];
  }
  free(map);
  return refused;
}

static const char *opt_inline_unsupported_op(const sv_func_t *func, bool construct) {
  const char *in_try = opt_inline_try_op(func);
  if (in_try) return in_try;
  for (uint32_t off = 0; off < (uint32_t)func->code_len; off += sv_op_size[func->code[off]]) {
    sv_op_t op = (sv_op_t)func->code[off];
    if (sv_op_flags[op] & SV_OPF_OPT_ROOT_ONLY) return sv_op_names[op];
    if (opt_op_native(op) || (construct && op == OP_CHECK_CTOR)) continue;
    if (op == OP_SPECIAL_OBJ && func->code[off + 1] == SV_SPECIAL_SUPER) continue;
    if (op == OP_SPECIAL_OBJ && func->code[off + 1] == SV_SPECIAL_ARGUMENTS && (func->is_strict || !func->param_count)) continue;
    return sv_op_names[op];
  }
  return NULL;
}

static sv_closure_t *opt_constant_closure(const opt_builder_t *b, ir_node_id_t node) {
  if (node == IR_NONE) return NULL;
  uint64_t bits;
  ant_value_t known = ir_constant_value(b->graph, node, &bits) && ir_node(b->graph, node)->repr == OPT_REPR_TAGGED ? bits : opt_likely_value(b, node);
  return known && vtype(known) == kTypeFunction ? js_func_closure(known) : NULL;
}

static bool opt_same_closure(const opt_builder_t *caller, sv_closure_t *callee) {
  if (!callee) return true;
  sv_closure_t *own = caller->inline_call ? opt_constant_closure(caller, caller->inline_call->closure) : caller->ctx->closure;
  return !own || own == callee;
}

static const char *opt_not_inlineable(const opt_builder_t *b, sv_func_t *target, bool construct, uint32_t *count, ir_node_id_t callee) {
  if (!target) return "no-target";
  if (target->is_async || target->is_generator || target->is_derived_ctor || target->has_dynamic_eval ||
      target->is_eval || target->is_tla || target->code_len <= 0) return "function-kind";
  if (target->is_arrow && (construct || opt_reads_this(target))) return "arrow-this";
  uint32_t depth = 0;
  for (const opt_builder_t *caller = b; caller; caller = caller->inline_call ? caller->inline_call->caller : NULL) {
    if (caller->func == target && opt_same_closure(caller, opt_constant_closure(b, callee))) return "recursive";
    depth++;
  }
  if (depth > OPT_INLINE_MAX_DEPTH) return "depth";
  for (uint32_t i = 0; i < b->ctx->no_inline.len; i++)
    if (b->ctx->no_inline.items[i] == target) return "failed-before";
  *count = opt_instruction_count(target);
  if (*count > OPT_INLINE_MAX_INSTRUCTIONS) return "size";
  if (b->ctx->inlined_instructions + *count > OPT_INLINE_BUDGET) return "budget";
  const char *op = opt_inline_unsupported_op(target, construct);
  if (op) return op;
  return opt_may_speculate(b, OPT_EXIT_BAD_CALL_TARGET) ? NULL : "exited-before";
}

static void opt_inline_refused(const opt_builder_t *b, sv_func_t *target, const char *reason) {
  if (!sv_jit_warn_unlikely || !target) return;
  fprintf(stderr, "jit: opt inline-refused caller=%s callee=%s bc=%u reason=%s\n",
    b->func->debug && b->func->debug->name ? b->func->debug->name : "<anonymous>",
    target->debug && target->debug->name ? target->debug->name : "<anonymous>", b->pc, reason);
}

static opt_flow_t opt_build_return(opt_builder_t *b, ir_node_id_t value);

static bool opt_is_virtual_arguments(const opt_builder_t *b, ir_node_id_t node) {
  return node != IR_NONE && node == b->arguments_marker;
}

static bool opt_is_any_virtual_arguments(const opt_builder_t *b, ir_node_id_t node) {
  uint64_t bits;
  return node != IR_NONE && ir_constant_value(b->graph, node, &bits) && ir_value_virtual_arguments_kind(bits) >= 0;
}

static opt_flow_t opt_no_virtual_arguments(opt_builder_t *b) {
  if (!b->ctx->retry) {
    opt_vec_push(&b->graph->arena, &b->ctx->no_virtual_arguments, b->func);
    b->ctx->retry = true;
  }
  opt_fail(b, "virtual-arguments");
  return OPT_FLOW_FAILED;
}

static opt_flow_t opt_build_arguments_length(opt_builder_t *b) {
  ir_node_id_t count = b->inline_call ? opt_int32_constant(b, (int32_t)b->inline_call->argc)
    : opt_emit(b, ir_op(IR_OP_ARGUMENT_COUNT), NULL, 0, OPT_REPR_INT32);
  opt_poke(b, 0, count);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_arguments_index(opt_builder_t *b) {
  ir_node_id_t key = opt_peek(b, 0), value;
  int32_t k;
  if (b->inline_call) {
    if (!opt_int32_constant_of(b, key, &k)) return opt_no_virtual_arguments(b);
    value = k >= 0 && (uint32_t)k < b->inline_call->argc ? b->inline_call->args[k] : opt_constant(b, IR_VALUE_UNDEFINED);
  } else {
    if (!opt_may_speculate(b, OPT_EXIT_ARGUMENTS_INDEX) || !opt_may_be_int32(b, key) || !opt_may_speculate(b, OPT_EXIT_NOT_INT32))
      return opt_no_virtual_arguments(b);
    ir_node_id_t index = opt_int32(b, key);
    ir_op_t load = ir_op(IR_OP_LOAD_ARGUMENT);
    load.arguments_base = ir_layout_arguments_base(&b->layout);
    value = opt_emit_checked(b, load, &index, 1, OPT_REPR_TAGGED);
  }
  opt_drop(b, 2);
  opt_push(b, value);
  return OPT_FLOW_CONTINUE;
}

static void opt_forget_memory(opt_builder_t *b) {
  for (opt_builder_t *caller = b; caller; caller = caller->inline_call ? caller->inline_call->caller : NULL)
    opt_bitset_for_each(&caller->memory, t) caller->frame.slots[t] = (opt_slot_t){.node = IR_NONE};
}

static opt_flow_t opt_finish_call(opt_builder_t *b, uint32_t operands, ir_node_id_t result, bool tail) {
  opt_drop(b, operands);
  opt_forget_memory(b);
  if (tail) return opt_build_return(b, result);
  opt_push(b, result);
  return OPT_FLOW_CONTINUE;
}

static constexpr uint32_t OPT_MAX_CALL_CANDIDATES = 8;

typedef struct {
  ant_value_t values[OPT_MAX_CALL_CANDIDATES];
  uint32_t count;
  bool complete;
} opt_call_candidates_t;

static bool opt_is_loop_phi(const opt_builder_t *b, ir_node_id_t node) {
  for (ir_block_id_t block = 0; block < ir_block_count(b->graph); block++) {
    const ir_block_t *data = ir_block(b->graph, block);
    if (data->is_loop_header && ir_node_vec_contains(&data->phis, node)) return true;
  }
  return false;
}

static bool opt_collect_callees(const opt_builder_t *b, ir_node_id_t node, uint32_t depth, opt_call_candidates_t *out) {
  const ir_node_t *data = ir_node(b->graph, node);
  if (data->op.code == IR_OP_PHI) {
    if (depth > 2 || opt_is_loop_phi(b, node)) return false;
    for (uint32_t i = 0; i < data->inputs.len; i++)
      if (!opt_collect_callees(b, data->inputs.items[i], depth + 1, out)) return false;
    return true;
  }
  ant_value_t known = opt_likely_value(b, node);
  uint64_t bits;
  if (ir_constant_value(b->graph, node, &bits) && data->repr == OPT_REPR_TAGGED) known = bits;
  sv_closure_t *closure = known && vtype(known) == kTypeFunction ? js_func_closure(known) : NULL;
  if (!closure || !closure->func || closure->func->is_async || closure->func->is_generator || closure->func->code_len <= 0) {
    out->complete = false;
    return true;
  }
  for (uint32_t i = 0; i < out->count; i++)
    if (out->values[i] == known) return true;
  if (out->count == OPT_MAX_CALL_CANDIDATES) return false;
  out->values[out->count++] = known;
  return true;
}

static ir_node_id_t opt_inline_receiver(opt_builder_t *b, sv_func_t *target, ir_node_id_t receiver) {
  if (!opt_reads_this(target)) return opt_constant(b, IR_VALUE_UNDEFINED);
  if (target->is_strict) return receiver;
  uint64_t bits;
  if (ir_constant_value(b->graph, receiver, &bits)) {
    if (ir_node(b->graph, receiver)->repr != OPT_REPR_TAGGED) return IR_NONE;
    if (bits == IR_VALUE_UNDEFINED || bits == IR_VALUE_NULL) return opt_constant(b, b->js->global_this);
    return ir_value_is_object(bits) ? receiver : IR_NONE;
  }
  if (ir_node_vec_contains(&b->ctx->object_like, receiver)) return receiver;
  if (!opt_may_speculate(b, OPT_EXIT_NOT_OBJECT)) return IR_NONE;
  return opt_emit_checked(b, ir_op(IR_OP_CHECK_OBJECT), &receiver, 1, OPT_REPR_NONE);
}

static ir_node_id_t opt_inline_closure(opt_builder_t *b, sv_func_t *target, ir_node_id_t callee) {
  uint64_t bits;
  ant_value_t known = 0;
  if (ir_constant_value(b->graph, callee, &bits) && ir_node(b->graph, callee)->repr == OPT_REPR_TAGGED) known = bits;
  else known = opt_likely_value(b, callee);
  opt_call_candidates_t candidates = {.complete = true};
  if (!known && ir_node(b->graph, callee)->op.code == IR_OP_PHI && opt_collect_callees(b, callee, 0, &candidates)
    && candidates.count == 1 && candidates.complete) known = candidates.values[0];
  if (known && vtype(known) == kTypeFunction) {
    sv_closure_t *closure = js_func_closure(known);
    if (closure->func != target || (closure->call_flags & ~OPT_CALL_PLAIN_FLAGS)) return IR_NONE;
    if (!opt_retain(b, known)) return IR_NONE;
    if (!ir_is_constant(b->graph, callee)) {
      ir_op_t check = ir_op(IR_OP_CHECK_VALUE);
      check.check_value.expected = known;
      check.check_value.kind = OPT_EXIT_BAD_CALL_TARGET;
      opt_emit_checked(b, check, &callee, 1, OPT_REPR_NONE);
    }
    return opt_constant(b, known);
  }
  opt_retain_func(b, target);
  ir_op_t check = ir_op(IR_OP_CHECK_CLOSURE);
  check.shared_data = (ir_cell_t)(uintptr_t)target;
  return opt_emit_checked(b, check, &callee, 1, OPT_REPR_NONE);
}

static ir_node_id_t opt_construct_this(opt_builder_t *b, ir_node_id_t closure) {
  ant_t *js = b->js;
  uint64_t bits;
  if (!ir_constant_value(b->graph, closure, &bits) || vtype(bits) != kTypeFunction || !opt_may_speculate(b, OPT_EXIT_BAD_SHAPE)) return IR_NONE;
  sv_closure_t *constructor = js_func_closure(bits);
  if (!sv_closure_is_plain_sync(constructor) || constructor->func->is_derived_ctor || !is_object_type(constructor->func_obj)) return IR_NONE;
  ant_object_t *func_obj = js_obj_ptr(js_as_obj(constructor->func_obj));
  ant_value_t proto;
  if (!func_obj->flags.is_constructor || func_obj->flags.is_exotic || !sv_construct_prototype_data(js, func_obj, constructor->func, &proto))
    return IR_NONE;
  const sv_func_sidecar_t *sidecar = sv_func_sidecar(constructor->func);
  uint32_t slot = sidecar->ctor_proto_slot;
  ant_value_t held = ant_object_prop_get_unchecked(func_obj, slot);
  if (func_obj->shape != sidecar->ctor_proto_shape || slot > 0xffff || !jit_snapshot_shape(js, func_obj->shape)
    || !opt_retain(b, held)) return IR_NONE;
  ant_shape_t *shape = ant_object_proto_root(b->js, proto, (uint8_t)ANT_INOBJ_MAX_SLOTS);
  if (!shape) return IR_NONE;
  bool held_shape = jit_snapshot_shape(js, shape);
  ant_shape_release(shape);
  if (!held_shape) return IR_NONE;

  ir_node_id_t object = opt_constant(b, constructor->func_obj);
  ir_node_id_t address = opt_typed_constant(b, (uint64_t)(uintptr_t)func_obj, OPT_REPR_POINTER);
  object = opt_check_shape(b, object, address, func_obj->shape);
  ir_node_id_t prototype = opt_load_named(b, object, address, slot, func_obj->shape);
  ir_op_t check = ir_op(IR_OP_CHECK_VALUE);
  check.check_value.expected = held;
  check.check_value.kind = OPT_EXIT_BAD_SHAPE;
  opt_emit_checked(b, check, &prototype, 1, OPT_REPR_NONE);
  ir_op_t op = ir_op(IR_OP_ALLOCATE_OBJECT);
  op.allocate_object.shape = (ir_cell_t)(uintptr_t)shape;
  op.allocate_object.reserve = ant_shape_get_inobj_limit(shape);
  ir_node_id_t input = opt_constant(b, proto);
  return opt_emit(b, op, &input, 1, OPT_REPR_TAGGED);
}

static bool opt_inline_build(opt_builder_t *b, opt_inline_t *call, sv_func_t *target, const ir_node_id_t *args, uint32_t argc) {
  opt_build_failure_t failure = {0};
  opt_builder_t callee = {
    .js = b->js,
    .func = target,
    .graph = b->graph,
    .ctx = b->ctx,
    .inline_call = call,
    .failure = &failure,
    .params = target->param_count,
    .locals = (uint32_t)target->max_locals,
    .stack = (uint32_t)(target->max_stack > 0 ? target->max_stack : 0),
    .eager = IR_NONE,
    .tail_header = IR_NONE,
    .arguments_marker = IR_NONE,
  };
  opt_builder_t *cb = &callee;
  cb->exit_sites = b->ctx->exit_sites_of ? b->ctx->exit_sites_of(b->js, target, &cb->exit_site_count) : NULL;
  opt_arena_init(&cb->arena);
  bool ok = opt_analyze(cb);
  if (ok && !opt_bitset_is_empty(&cb->memory)) ok = opt_fail(cb, "inline-frame-memory");
  if (ok) {
    cb->block = b->block;
    cb->frame = opt_frame_new(cb);
    ir_node_id_t undefined = opt_constant(cb, IR_VALUE_UNDEFINED);
    for (uint32_t p = 0; p < cb->params; p++) cb->frame.slots[p] = (opt_slot_t){.node = p < argc ? args[p] : undefined};
    for (uint32_t j = 0; j < cb->locals; j++) cb->frame.slots[cb->params + j] = (opt_slot_t){.node = undefined};
    ok = opt_end_with_jump(cb, 0);
  }
  for (uint32_t r = 0; ok && r < cb->rpo_count; r++) {
    uint32_t bi = cb->rpo[r];
    if (opt_start_bytecode_block(cb, bi)) ok = opt_build_bytecode_block(cb, bi);
    ok &= !cb->failed;
  }
  if (!ok && !b->failure->reason) *b->failure = failure;
  if (!ok && !b->failure->func) b->failure->func = target;
  if (ok) {
    for (uint32_t i = 0; i < cb->exit_site_count; i++) {
      ir_exit_site_t site = cb->exit_sites[i];
      site.func = target;
      opt_vec_push(&b->graph->arena, &b->graph->exit_sites, site);
    }
  }
  opt_arena_free(&cb->arena);
  return ok;
}

static bool opt_try_inline(opt_builder_t *b, sv_op_t op, uint32_t argc, uint32_t operands, ir_node_id_t callee, ir_node_id_t second,
  const ir_node_id_t *args, sv_func_t *target, opt_flow_t *flow) {
  bool construct = op == OP_NEW, tail = op == OP_TAIL_CALL || op == OP_TAIL_CALL_METHOD;
  bool method = op == OP_CALL_METHOD || op == OP_TAIL_CALL_METHOD;
  uint32_t count;
  const char *refused = opt_not_inlineable(b, target, construct, &count, callee);
  for (uint32_t i = 0; i < argc && !refused; i++)
    if (opt_is_any_virtual_arguments(b, args[i]) || opt_is_any_virtual_arguments(b, second)) refused = "arguments";
  if (!refused && construct && (callee != second || !opt_may_speculate(b, OPT_EXIT_OUT_OF_BOUNDS))) refused = "construct";
  ir_node_id_t receiver = IR_NONE;
  if (!refused && !construct) {
    receiver = opt_inline_receiver(b, target, method ? second : opt_constant(b, IR_VALUE_UNDEFINED));
    if (receiver == IR_NONE) refused = "this";
  }
  ir_node_id_t closure = refused ? IR_NONE : opt_inline_closure(b, target, callee);
  if (!refused && closure == IR_NONE) refused = "closure";
  if (refused) {
    opt_inline_refused(b, target, refused);
    return false;
  }
  if (construct && (receiver = opt_construct_this(b, closure)) == IR_NONE) {
    receiver = opt_emit(b, ir_op(IR_OP_NEW_THIS), &closure, 1, OPT_REPR_TAGGED);
    ir_node(b->graph, receiver)->frame_state = opt_call_position(b);
    receiver = opt_emit_checked(b, ir_op(IR_OP_CHECK_NOT_HOLE), &receiver, 1, OPT_REPR_NONE);
  }
  opt_retain_func(b, target);

  opt_frame_t caller_frame = opt_frame_clone(b, &b->frame);
  ir_frame_state_id_t caller_state = opt_after_call_state(b, operands, tail);
  ir_frame_state_t position = {
    .func = b->func,
    .bc_off = b->pc,
    .mode = tail ? OPT_RESUME_RETURN : OPT_RESUME_AFTER,
    .dst = OPT_INVALID_SLOT,
    .call_off = b->pc,
    .parent = b->inline_call ? b->inline_call->caller_position : IR_NONE,
  };
  opt_inline_t call = {
    .caller = b,
    .caller_state = caller_state,
    .caller_position = opt_in_try(b) ? caller_state : ir_add_frame_state(b->graph, &position),
    .closure = closure,
    .receiver = receiver,
    .argc = argc,
    .args = opt_arena_dup(&b->graph->arena, args, (argc + 1) * sizeof *args),
    .construct = construct,
    .depth = (b->inline_call ? b->inline_call->depth : 0) + 1,
    .continuation = IR_NONE,
  };
  if (!opt_inline_build(b, &call, target, args, argc)) {
    if (!b->ctx->retry) {
      char why[96];
      uint32_t at = b->failure->bc_off;
      snprintf(why, sizeof why, "%s@%u:%s", b->failure->reason ? b->failure->reason : "build", at,
        at < (uint32_t)target->code_len ? sv_op_names[target->code[at]] : "-");
      opt_inline_refused(b, target, why);
      opt_vec_push(&b->graph->arena, &b->ctx->no_inline, target);
      b->ctx->retry = true;
    }
    b->failed = true;
    *flow = OPT_FLOW_FAILED;
    return true;
  }
  b->ctx->inlined_instructions += count;
  b->ctx->inlined_calls++;
  if (call.continuation == IR_NONE) {
    *flow = OPT_FLOW_ENDED;
    return true;
  }
  b->block = call.continuation;
  opt_vec_push(&b->graph->arena, &b->ctx->order, call.continuation);
  b->frame = caller_frame;
  ir_node_id_t result = call.returns.items[0];
  bool same = true;
  for (uint32_t i = 1; i < call.returns.len; i++) same &= call.returns.items[i] == result;
  if (!same) result = opt_add_phi(b, opt_stack_index(b, b->frame.sp - operands), call.returns.items, call.returns.len);
  *flow = opt_finish_call(b, operands, result, tail);
  return true;
}

static opt_flow_t opt_build_return(opt_builder_t *b, ir_node_id_t value) {
  opt_inline_t *call = b->inline_call;
  if (call && opt_is_any_virtual_arguments(b, value)) return opt_no_virtual_arguments(b);
  if (!call) {
    opt_set_control(b, ir_op(IR_OP_RETURN), &value, 1);
    return OPT_FLOW_ENDED;
  }
  if (call->construct) {
    uint64_t bits;
    bool primitive = ir_constant_value(b->graph, value, &bits)
      && (ir_node(b->graph, value)->repr != OPT_REPR_TAGGED || !ir_value_is_object(bits));
    if (value != call->receiver && !primitive) {
      opt_fail(b, "construct-return");
      return OPT_FLOW_FAILED;
    }
    value = call->receiver;
  }
  if (call->continuation == IR_NONE) call->continuation = ir_add_block(b->graph);
  opt_vec_push(&b->graph->arena, &ir_block(b->graph, call->continuation)->predecessors, b->block);
  opt_vec_push(&call->caller->arena, &call->returns, value);
  ir_op_t jump = ir_op(IR_OP_JUMP);
  jump.target = call->continuation;
  opt_set_control(b, jump, NULL, 0);
  return OPT_FLOW_ENDED;
}

static bool opt_math_unary(ant_t *js, ant_value_t callee, ir_float64_unary_op_t *unary) {
  static const struct { ant_math_intrinsic_t kind; ir_float64_unary_op_t unary; } table[] = {
    {ANT_MATH_ABS, IR_UNARY_ABS}, {ANT_MATH_FLOOR, IR_UNARY_FLOOR}, {ANT_MATH_CEIL, IR_UNARY_CEIL}, {ANT_MATH_SQRT, IR_UNARY_SQRT},
    {ANT_MATH_ROUND, IR_UNARY_ROUND}, {ANT_MATH_TRUNC, IR_UNARY_TRUNC},
  };
  if (!callee || vtype(callee) != kTypeBuiltin) return false;
  for (uint32_t i = 0; i < sizeof table / sizeof *table; i++) {
    if (js->sym.math_fns[table[i].kind] != callee) continue;
    *unary = table[i].unary;
    return true;
  }
  return false;
}

static bool opt_try_math_imul(opt_builder_t *b, ir_node_id_t callee, ant_value_t known, uint32_t argc, const ir_node_id_t *args, ir_node_id_t *result) {
  if (argc < 2 || known != b->js->sym.math_fns[ANT_MATH_IMUL]) return false;
  if (!opt_may_speculate(b, OPT_EXIT_BAD_CALL_TARGET) || !opt_may_speculate(b, OPT_EXIT_NOT_INT32)) return false;
  if (!opt_may_be_int32(b, args[0]) || !opt_may_be_int32(b, args[1])) return false;
  if (!opt_retain(b, known)) return false;
  ir_op_t check = ir_op(IR_OP_CHECK_VALUE);
  check.check_value.expected = known;
  check.check_value.kind = OPT_EXIT_BAD_CALL_TARGET;
  opt_emit_checked(b, check, &callee, 1, OPT_REPR_NONE);
  ir_op_t mul = ir_op(IR_OP_INT32_WRAPPING_BINARY);
  mul.binary = IR_BINARY_MUL;
  ir_node_id_t product = opt_emit(b, mul, (ir_node_id_t[]){opt_int32(b, args[0]), opt_int32(b, args[1])}, 2, OPT_REPR_INT32);
  *result = opt_emit1(b, IR_OP_BOX_INT32, product, OPT_REPR_TAGGED);
  return true;
}

static bool opt_try_math_min_max(opt_builder_t *b, ir_node_id_t callee, ant_value_t known, uint32_t argc, const ir_node_id_t *args, ir_node_id_t *result) {
  bool max = known == b->js->sym.math_fns[ANT_MATH_MAX];
  if (!argc || argc > 8 || (!max && known != b->js->sym.math_fns[ANT_MATH_MIN])) return false;
  if (!opt_may_speculate(b, OPT_EXIT_BAD_CALL_TARGET)) return false;
  bool int32 = opt_may_speculate(b, OPT_EXIT_NOT_INT32), number = opt_may_speculate(b, OPT_EXIT_BAD_TYPE);
  for (uint32_t i = 0; i < argc; i++) {
    int32 &= opt_may_be_int32(b, args[i]);
    number &= opt_may_be_number(b, args[i]);
  }
  if (!int32 && !number) return false;
  if (!opt_retain(b, known)) return false;
  ir_op_t check = ir_op(IR_OP_CHECK_VALUE);
  check.check_value.expected = known;
  check.check_value.kind = OPT_EXIT_BAD_CALL_TARGET;
  opt_emit_checked(b, check, &callee, 1, OPT_REPR_NONE);
  ir_opcode_t code = int32 ? (max ? IR_OP_INT32_MAX : IR_OP_INT32_MIN) : (max ? IR_OP_FLOAT64_MAX : IR_OP_FLOAT64_MIN);
  opt_repr_t repr = int32 ? OPT_REPR_INT32 : OPT_REPR_FLOAT64;
  ir_node_id_t value = int32 ? opt_int32(b, args[0]) : opt_number(b, args[0]);
  for (uint32_t i = 1; i < argc; i++) {
    ir_node_id_t next = int32 ? opt_int32(b, args[i]) : opt_number(b, args[i]);
    value = opt_emit(b, ir_op(code), (ir_node_id_t[]){value, next}, 2, repr);
  }
  *result = opt_emit1(b, int32 ? IR_OP_BOX_INT32 : IR_OP_BOX_FLOAT64, value, OPT_REPR_TAGGED);
  return true;
}

static ir_node_id_t opt_call_float64_function(opt_builder_t *b, ant_math_double_t function, const ir_node_id_t *args) {
  uint32_t arity = ant_math_doubles[function].argc;
  ir_node_id_t numbers[2];
  for (uint32_t i = 0; i < arity; i++) numbers[i] = opt_number(b, args[i]);
  ir_op_t op = ir_op(IR_OP_CALL_FLOAT64_FUNCTION);
  op.function = (uint8_t)function;
  return opt_emit1(b, IR_OP_BOX_FLOAT64, opt_emit(b, op, numbers, arity, OPT_REPR_FLOAT64), OPT_REPR_TAGGED);
}

static bool opt_try_math_double(opt_builder_t *b, ir_node_id_t callee, ant_value_t known, uint32_t argc, const ir_node_id_t *args, ir_node_id_t *result) {
  int function = ant_math_double_of(known);
  if (function < 0 || argc < ant_math_doubles[function].argc) return false;
  if (!opt_may_speculate(b, OPT_EXIT_BAD_CALL_TARGET) || !opt_may_speculate(b, OPT_EXIT_BAD_TYPE)) return false;
  for (uint32_t i = 0; i < ant_math_doubles[function].argc; i++)
    if (!opt_may_be_number(b, args[i])) return false;
  if (!opt_retain(b, known)) return false;
  ir_op_t check = ir_op(IR_OP_CHECK_VALUE);
  check.check_value.expected = known;
  check.check_value.kind = OPT_EXIT_BAD_CALL_TARGET;
  opt_emit_checked(b, check, &callee, 1, OPT_REPR_NONE);
  *result = opt_call_float64_function(b, (ant_math_double_t)function, args);
  return true;
}

static bool opt_try_math(opt_builder_t *b, ir_node_id_t callee, uint32_t argc, const ir_node_id_t *args, ir_node_id_t *result) {
  ant_value_t known = opt_likely_value(b, callee);
  if (known && vtype(known) == kTypeBuiltin && opt_try_math_imul(b, callee, known, argc, args, result)) return true;
  if (known && vtype(known) == kTypeBuiltin && opt_try_math_min_max(b, callee, known, argc, args, result)) return true;
  if (known && vtype(known) == kTypeBuiltin && opt_try_math_double(b, callee, known, argc, args, result)) return true;
  ir_float64_unary_op_t unary;
  if (!argc || !opt_math_unary(b->js, known, &unary) || !opt_may_speculate(b, OPT_EXIT_BAD_CALL_TARGET)) return false;
  if (!opt_may_be_number(b, args[0]) || !opt_may_speculate(b, OPT_EXIT_BAD_TYPE)) return false;
  if (!opt_retain(b, known)) return false;
  ir_op_t check = ir_op(IR_OP_CHECK_VALUE);
  check.check_value.expected = known;
  check.check_value.kind = OPT_EXIT_BAD_CALL_TARGET;
  opt_emit_checked(b, check, &callee, 1, OPT_REPR_NONE);
  ir_node_id_t number = opt_number(b, args[0]);
  ir_op_t op = ir_op(IR_OP_FLOAT64_UNARY);
  op.unary = unary;
  *result = opt_emit1(b, IR_OP_BOX_FLOAT64, opt_emit(b, op, &number, 1, OPT_REPR_FLOAT64), OPT_REPR_TAGGED);
  return true;
}

static opt_flow_t opt_build_instanceof(opt_builder_t *b) {
  ant_t *js = b->js;
  ir_node_id_t lhs = opt_tagged(b, opt_peek(b, 1)), rhs = opt_tagged(b, opt_peek(b, 0));
  uint64_t bits;
  ant_value_t known = ir_constant_value(b->graph, rhs, &bits) && ir_node(b->graph, rhs)->repr == OPT_REPR_TAGGED ? bits : opt_likely_value(b, rhs);
  if (!known || vtype(known) != kTypeFunction || !opt_may_speculate(b, OPT_EXIT_BAD_TYPE) || !opt_may_speculate(b, OPT_EXIT_BAD_SHAPE))
    return OPT_FLOW_GENERIC;
  sv_closure_t *closure = js_func_closure(known);
  if (!closure || (closure->call_flags & (SV_CALL_HAS_BOUND_ARGS | SV_CALL_HAS_BOUND_THIS)) || !is_object_type(closure->func_obj)
    || !is_object_type(js->sym.function_proto)) return OPT_FLOW_GENERIC;
  ant_object_t *func_obj = js_obj_ptr(js_as_obj(closure->func_obj));
  ant_object_t *function_proto = js_obj_ptr(js_as_obj(js->sym.function_proto));
  ant_offset_t has_instance = (ant_offset_t)vdata(js->sym.hasInstance_sym);
  if (func_obj->flags.is_exotic || function_proto->flags.is_exotic || !func_obj->shape || !function_proto->shape
    || ant_shape_lookup_symbol(func_obj->shape, has_instance) >= 0 || vdata(func_obj->proto) != vdata(js->sym.function_proto))
    return OPT_FLOW_GENERIC;
  int32_t slot = ant_shape_lookup_interned(func_obj->shape, js->intern.prototype);
  if (slot < 0 || (uint32_t)slot >= func_obj->prop_count || slot > 0xffff) return OPT_FLOW_GENERIC;
  const ant_shape_prop_t *prop = ant_shape_prop_at(func_obj->shape, (uint32_t)slot);
  ant_value_t proto = ant_object_prop_get_unchecked(func_obj, (uint32_t)slot);
  if (!prop || prop->has_getter || prop->has_setter || vtype(proto) != kTypeObject) return OPT_FLOW_GENERIC;
  static const uint8_t primitives[] = {kTypeString, kTypeNumber, kTypeBool};
  for (uint32_t i = 0; i < sizeof primitives; i++)
    if (vdata(proto) == vdata(js_primitive_prototype(js, primitives[i]))) return OPT_FLOW_GENERIC;
  if (!jit_snapshot_shape(js, func_obj->shape) || !jit_snapshot_shape(js, function_proto->shape)
    || !opt_retain(b, known) || !opt_retain(b, proto))
    return OPT_FLOW_GENERIC;

  if (!ir_is_constant(b->graph, rhs)) {
    ir_op_t check = ir_op(IR_OP_CHECK_VALUE);
    check.check_value.expected = known;
    check.check_value.kind = OPT_EXIT_BAD_SHAPE;
    opt_emit_checked(b, check, &rhs, 1, OPT_REPR_NONE);
  }
  ir_node_id_t object = opt_constant(b, closure->func_obj);
  ir_node_id_t address = opt_typed_constant(b, (uint64_t)(uintptr_t)func_obj, OPT_REPR_POINTER);
  object = opt_check_shape(b, object, address, func_obj->shape);
  ir_node_id_t held = opt_load_named(b, object, address, (uint32_t)slot, func_obj->shape);
  ir_op_t check = ir_op(IR_OP_CHECK_VALUE);
  check.check_value.expected = proto;
  check.check_value.kind = OPT_EXIT_BAD_SHAPE;
  opt_emit_checked(b, check, &held, 1, OPT_REPR_NONE);
  opt_check_shape(b, opt_constant(b, js->sym.function_proto), opt_typed_constant(b, (uint64_t)(uintptr_t)function_proto, OPT_REPR_POINTER), function_proto->shape);
  ir_op_t walk = ir_op(IR_OP_HAS_IN_PROTOTYPE_CHAIN);
  walk.bits = proto;
  ir_node_id_t found = opt_emit_checked(b, walk, &lhs, 1, OPT_REPR_BOOL);
  opt_drop(b, 2);
  opt_push(b, opt_box_bool(b, found));
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_call_helper(opt_builder_t *b, const uint8_t *ip, sv_op_t op);

static bool opt_is_char_code_at_builtin(ant_value_t value) {
  return vtype(value) == kTypeBuiltin && js_as_cfunc(value) == builtin_string_charCodeAt;
}

static opt_flow_t opt_build_char_code_at(opt_builder_t *b, const uint8_t *ip) {
  ant_t *js = b->js;
  ant_value_t builtin;
  if (sv_get_u16(ip + 1) != 1 || b->frame.sp < 3 || !js_try_get_own_data_prop(js, js->sym.string_proto, "charCodeAt", 10, &builtin)
    || !opt_is_char_code_at_builtin(builtin) || !opt_retain(b, builtin)) return opt_build_call_helper(b, ip, OP_CALL_CHAR_CODE_AT);
  ir_node_id_t key = opt_peek(b, 0);
  if (!opt_may_be_int32(b, key) || !opt_may_speculate(b, OPT_EXIT_NOT_INT32)) return opt_build_call_helper(b, ip, OP_CALL_CHAR_CODE_AT);
  ir_node_id_t callee = opt_tagged(b, opt_peek(b, 1)), receiver = opt_tagged(b, opt_peek(b, 2));
  ir_node_id_t index = opt_int32(b, key);
  opt_frame_t before = opt_frame_clone(b, &b->frame);
  ir_block_id_t slow[4];
  uint32_t count = 0;
  bool exits = opt_may_speculate(b, OPT_EXIT_SLOW_PATH);
  if (exits && opt_may_speculate(b, OPT_EXIT_BAD_CALL_TARGET)) {
    ir_op_t check = ir_op(IR_OP_CHECK_VALUE);
    check.check_value.expected = builtin;
    check.check_value.kind = OPT_EXIT_BAD_CALL_TARGET;
    opt_emit_checked(b, check, &callee, 1, OPT_REPR_NONE);
  } else {
    ir_condition_t same = ir_cond(IR_COND_TAGGED_EQUALS);
    same.equal = true;
    opt_branch_off(b, same, (ir_node_id_t[]){callee, opt_constant(b, builtin)}, 2, false, &slow[count++]);
  }
  opt_branch_off(b, ir_cond(IR_COND_STRING), &receiver, 1, false, &slow[count++]);
  ir_node_id_t string = opt_refine(b, ir_cond(IR_COND_STRING), &receiver, 1);
  ir_node_id_t address = opt_emit1(b, IR_OP_STRING_ADDRESS, string, OPT_REPR_POINTER);
  opt_branch_off(b, ir_cond(IR_COND_RESOLVED_STRING), &address, 1, false, &slow[count++]);
  ir_node_id_t length = opt_emit1(b, IR_OP_STRING_LENGTH, address, OPT_REPR_INT32);
  opt_branch_off(b, ir_cond(IR_COND_INDEX_IN_BOUNDS), (ir_node_id_t[]){index, length}, 2, false, &slow[count++]);
  ir_node_id_t checked = opt_refine(b, ir_cond(IR_COND_INDEX_IN_BOUNDS), (ir_node_id_t[]){index, length}, 2);
  ir_node_id_t unit = opt_emit(b, ir_op(IR_OP_LOAD_STRING_CODE_UNIT), (ir_node_id_t[]){address, checked}, 2, OPT_REPR_INT32);
  opt_drop(b, 3);
  opt_push(b, unit);
  ir_block_id_t fast_end = b->block;
  opt_frame_t fast = b->frame;
  ir_block_id_t slow_block = opt_join_blocks(b, slow, count);
  ir_block(b->graph, slow_block)->is_cold = true;
  b->frame = opt_frame_clone(b, &before);
  if (exits) {
    if (opt_end_with_exit(b, OPT_EXIT_SLOW_PATH) != OPT_FLOW_ENDED) return OPT_FLOW_FAILED;
    b->block = fast_end;
    b->frame = fast;
    return OPT_FLOW_CONTINUE;
  }
  if (opt_build_call_helper(b, ip, OP_CALL_CHAR_CODE_AT) != OPT_FLOW_CONTINUE) return OPT_FLOW_FAILED;
  ir_block_id_t slow_end = b->block;
  opt_frame_t slow_frame = b->frame;
  b->frame = fast;
  opt_join_slow_path(b, fast_end, slow_end, &slow_frame, 1);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_call_helper(opt_builder_t *b, const uint8_t *ip, sv_op_t op) {
  uint32_t argc = 1;
  uintptr_t helper, extra = 0;
  bool pushes = true;
  switch (op) {
    case OP_CALL_CHAR_CODE_AT: argc = sv_get_u16(ip + 1); helper = (uintptr_t)opt_rt_char_code_at; break;
    case OP_CALL_ARRAY_INCLUDES: argc = sv_get_u16(ip + 1); helper = (uintptr_t)opt_rt_array_includes; break;
    case OP_CALL_STRING_INTRINSIC: extra = ip[1]; argc = sv_get_u16(ip + 2); helper = (uintptr_t)opt_rt_string_intrinsic; break;
    case OP_CALL_STABLE_BUILTIN: extra = ip[1]; argc = sv_get_u16(ip + 2); helper = (uintptr_t)opt_rt_stable_builtin; break;
    case OP_RE_EXEC_TRUTHY: helper = (uintptr_t)opt_rt_regexp_exec_truthy; break;
    case OP_RE_EXEC_DISCARD: helper = (uintptr_t)opt_rt_regexp_exec_truthy; pushes = false; break;
    default: {
      opt_rt_site_t *site = code_arena_bump(b->js, sizeof *site);
      if (!site) return OPT_FLOW_GENERIC;
      *site = (opt_rt_site_t){.func = b->func, .bc_off = b->pc};
      argc = 0;
      extra = (uintptr_t)site;
      helper = (uintptr_t)opt_rt_instanceof;
      break;
    }
  }
  uint32_t operands = argc + 2;
  if (argc > OPT_MAX_CALL_ARGS || operands > b->frame.sp) {
    opt_fail(b, "call-argc");
    return OPT_FLOW_FAILED;
  }
  ir_node_id_t inputs[2 + OPT_MAX_CALL_ARGS];
  bool swapped = op == OP_INSTANCEOF;
  inputs[0] = opt_tagged(b, opt_peek(b, argc + swapped));
  inputs[1] = opt_tagged(b, opt_peek(b, argc + !swapped));
  for (uint32_t i = 0; i < argc; i++) inputs[2 + i] = opt_tagged(b, opt_peek(b, argc - 1 - i));
  ir_op_t call = ir_op(IR_OP_CALL_NATIVE);
  call.call.func = b->func;
  call.call.bc_off = b->pc;
  call.call.stores_operands = true;
  call.call.argc = (uint16_t)argc;
  call.call.extra = extra;
  call.call.helper = helper;
  ir_node_id_t result = opt_emit(b, call, inputs, 2 + argc, OPT_REPR_TAGGED);
  ir_node(b->graph, result)->frame_state = opt_call_position(b);
  opt_mark_catch(b, result);
  opt_drop(b, operands);
  opt_forget_memory(b);
  if (pushes) opt_push(b, result);
  return OPT_FLOW_CONTINUE;
}

static bool opt_self_tail_loop_ok(const opt_builder_t *b) {
  sv_func_t *func = b->func;
  if (b->inline_call || !opt_bitset_is_empty(&b->memory)) return false;
  bool any = false;
  for (uint32_t i = 0; i < b->ins_count; i++) {
    uint32_t off = b->ins_off[i];
    sv_op_t op = opt_op_at(b, off);
    if (op == OP_THIS || op == OP_REST || (op == OP_SPECIAL_OBJ && func->code[off + 1] != SV_SPECIAL_SUPER)) return false;
    if ((op == OP_TAIL_CALL || op == OP_TAIL_CALL_METHOD) && sv_tfb_get_call_target(func, (int)off) == func) any = true;
  }
  return any;
}

static void opt_build_tail_header(opt_builder_t *b) {
  ir_block_id_t entry = b->block;
  ir_node_id_t *initial = opt_arena_new(&b->arena, ir_node_id_t, b->params + 1);
  for (uint32_t p = 0; p < b->params; p++) initial[p] = opt_read_tracked(b, p);
  b->tail_phis = opt_arena_new(&b->arena, ir_node_id_t, b->params + 1);
  ir_block_id_t header = ir_add_block(b->graph);
  ir_op_t jump = ir_op(IR_OP_JUMP);
  jump.target = header;
  opt_set_control(b, jump, NULL, 0);
  opt_vec_push(&b->graph->arena, &ir_block(b->graph, header)->predecessors, entry);
  opt_vec_push(&b->graph->arena, &b->ctx->order, header);
  b->block = header;
  for (uint32_t p = 0; p < b->params; p++) {
    b->tail_phis[p] = opt_add_phi(b, p, &initial[p], 1);
    b->frame.slots[p] = (opt_slot_t){.node = b->tail_phis[p]};
  }
  ir_block_t *data = ir_block(b->graph, header);
  data->is_loop_header = true;
  data->bytecode_start = 0;
  data->start_frame_state = opt_new_frame_state(b);
  b->tail_header = header;
}

static void opt_build_self_tail(opt_builder_t *b, ir_node_id_t callee, const ir_node_id_t *args, uint32_t argc) {
  ir_op_t load = ir_op(IR_OP_LOAD_FRAME_FIELD);
  load.field = IR_FIELD_CALLEE;
  ir_node_id_t self = opt_emit(b, load, NULL, 0, OPT_REPR_TAGGED);
  ir_condition_t condition = ir_cond(IR_COND_TAGGED_EQUALS);
  condition.equal = true;
  ir_block_id_t loop, other;
  opt_branch_both_ways(b, condition, (ir_node_id_t[]){callee, self}, 2, &loop, &other);
  b->block = loop;
  ir_node_id_t undefined = opt_constant(b, IR_VALUE_UNDEFINED);
  for (uint32_t p = 0; p < b->params; p++) ir_push_input(b->graph, b->tail_phis[p], p < argc ? args[p] : undefined);
  opt_vec_push(&b->graph->arena, &ir_block(b->graph, b->tail_header)->predecessors, loop);
  ir_op_t jump = ir_op(IR_OP_JUMP);
  jump.target = b->tail_header;
  opt_set_control(b, jump, NULL, 0);
  b->block = other;
  ir_block(b->graph, other)->is_cold = true;
}

static bool opt_expect_builtin(opt_builder_t *b, ir_node_id_t callee, bool (*is)(ant_value_t)) {
  uint64_t bits;
  if (ir_constant_value(b->graph, callee, &bits) && ir_node(b->graph, callee)->repr == OPT_REPR_TAGGED) return is(bits);
  ant_value_t known = opt_likely_value(b, callee);
  if (!known || !is(known) || !opt_may_speculate(b, OPT_EXIT_BAD_CALL_TARGET)) return false;
  ir_op_t check = ir_op(IR_OP_CHECK_VALUE);
  check.check_value.expected = known;
  check.check_value.kind = OPT_EXIT_BAD_CALL_TARGET;
  opt_emit_checked(b, check, &callee, 1, OPT_REPR_NONE);
  return true;
}

static bool opt_expect_array_ctor(opt_builder_t *b, ir_node_id_t callee) {
  ant_value_t ctor = b->js->sym.array_ctor;
  uint64_t bits;
  if (ir_constant_value(b->graph, callee, &bits) && ir_node(b->graph, callee)->repr == OPT_REPR_TAGGED) return bits == ctor;
  if (opt_likely_value(b, callee) != ctor || !opt_may_speculate(b, OPT_EXIT_BAD_CALL_TARGET)) return false;
  ir_op_t check = ir_op(IR_OP_CHECK_VALUE);
  check.check_value.expected = ctor;
  check.check_value.kind = OPT_EXIT_BAD_CALL_TARGET;
  opt_emit_checked(b, check, &callee, 1, OPT_REPR_NONE);
  return true;
}

static sv_func_t *opt_known_target(opt_builder_t *b, ir_node_id_t callee) {
  ant_value_t known = opt_likely_value(b, callee);
  uint64_t bits;
  if (ir_constant_value(b->graph, callee, &bits) && ir_node(b->graph, callee)->repr == OPT_REPR_TAGGED) known = bits;
  sv_func_t *target = known && vtype(known) == kTypeFunction ? js_func_closure(known)->func : NULL;
  return target && !target->is_async && !target->is_generator && target->code_len > 0 ? target : NULL;
}

static opt_flow_t opt_build_push_pop(opt_builder_t *b, bool push, const ir_node_id_t *inputs, uint32_t operands, bool tail) {
  ir_block_id_t slow[4], ends[3];
  ir_node_id_t results[3], length;
  uint32_t count = 0, paths = 0;
  ir_node_id_t address = opt_elements_of(b, inputs[1], IR_ELEMENTS_PACKED, &length, slow, &count);
  ir_node_id_t allowed = opt_emit1(b, IR_OP_CAN_ADD_ELEMENT, address, OPT_REPR_BOOL);
  opt_branch_off(b, ir_cond(IR_COND_BOOL), &allowed, 1, false, &slow[count++]);
  if (push) {
    ir_node_id_t capacity = opt_emit1(b, IR_OP_LOAD_ELEMENTS_CAPACITY, address, OPT_REPR_INT32);
    opt_branch_off(b, ir_cond(IR_COND_INDEX_IN_BOUNDS), (ir_node_id_t[]){length, capacity}, 2, false, &slow[count++]);
    opt_emit(b, ir_op(IR_OP_APPEND_ELEMENT), (ir_node_id_t[]){address, length, inputs[2]}, 3, OPT_REPR_NONE);
    ir_op_t add = ir_op(IR_OP_INT32_WRAPPING_BINARY);
    add.binary = IR_BINARY_ADD;
    results[paths] = opt_tagged(b, opt_emit(b, add, (ir_node_id_t[]){length, opt_int32_constant(b, 1)}, 2, OPT_REPR_INT32));
    ends[paths++] = b->block;
  } else {
    ir_condition_t empty = ir_cond(IR_COND_INT32);
    empty.comparison = IR_CMP_STRICTLY_EQUALS;
    ir_block_id_t none, some;
    opt_branch_both_ways(b, empty, (ir_node_id_t[]){length, opt_int32_constant(b, 0)}, 2, &none, &some);
    results[paths] = opt_constant(b, IR_VALUE_UNDEFINED);
    ends[paths++] = none;
    b->block = some;
    ir_op_t sub = ir_op(IR_OP_INT32_WRAPPING_BINARY);
    sub.binary = IR_BINARY_SUB;
    ir_node_id_t last = opt_emit(b, sub, (ir_node_id_t[]){length, opt_int32_constant(b, 1)}, 2, OPT_REPR_INT32);
    ir_op_t load = ir_op(IR_OP_LOAD_ELEMENT_AT);
    load.elements = IR_ELEMENTS_HOLEY;
    ir_node_id_t value = opt_emit(b, load, (ir_node_id_t[]){address, last}, 2, OPT_REPR_TAGGED);
    ir_condition_t is_hole = ir_cond(IR_COND_TAGGED_EQUALS);
    is_hole.equal = true;
    opt_branch_off(b, is_hole, (ir_node_id_t[]){value, opt_constant(b, IR_VALUE_EMPTY)}, 2, true, &slow[count++]);
    opt_emit(b, ir_op(IR_OP_POP_ELEMENT), (ir_node_id_t[]){address, last}, 2, OPT_REPR_NONE);
    results[paths] = value;
    ends[paths++] = b->block;
  }
  opt_join_blocks(b, slow, count);
  ir_block(b->graph, b->block)->is_cold = true;
  ir_op_t call = ir_op(IR_OP_CALL_DIRECT);
  call.call.func = b->func;
  call.call.bc_off = b->pc;
  call.call.stores_operands = true;
  call.call.argc = push;
  results[paths] = opt_emit(b, call, inputs, 2 + push, OPT_REPR_TAGGED);
  ir_node(b->graph, results[paths])->frame_state = opt_call_position(b);
  opt_mark_catch(b, results[paths]);
  ends[paths++] = b->block;
  opt_join_blocks(b, ends, paths);
  return opt_finish_call(b, operands, opt_value_phi(b, b->frame.sp - operands, results, paths), tail);
}

static opt_flow_t opt_emit_call(opt_builder_t *b, bool construct, sv_func_t *target, const ir_node_id_t *inputs, uint32_t argc, uint32_t operands, bool tail) {
  ir_op_t call = ir_op(construct ? IR_OP_CALL_CONSTRUCT : IR_OP_CALL_DIRECT);
  call.call.func = b->func;
  call.call.bc_off = b->pc;
  call.call.stores_operands = true;
  call.call.argc = (uint16_t)argc;
  if (target && !construct && target->code[0] != OP_CHECK_CTOR) {
    call.call.target = target;
    opt_retain_func(b, target);
  }
  ir_node_id_t result = opt_emit(b, call, inputs, 2 + argc, OPT_REPR_TAGGED);
  ir_node(b->graph, result)->frame_state = opt_call_position(b);
  opt_mark_catch(b, result);
  return opt_finish_call(b, operands, result, tail);
}

static constexpr uint32_t OPT_MAX_CALL_ARMS = OPT_MAX_CALL_CANDIDATES + 1;

static_assert(OPT_MAX_FIELD_SHAPES <= OPT_MAX_CALL_ARMS, "getter arms fit the call arm table");

typedef struct {
  opt_frame_t before;
  ir_block_id_t ends[OPT_MAX_CALL_ARMS];
  opt_frame_t frames[OPT_MAX_CALL_ARMS];
  uint32_t count;
  bool failed;
} opt_call_arms_t;

static void opt_call_arm_begin(opt_builder_t *b, opt_call_arms_t *arms, ir_block_id_t block) {
  b->block = block;
  b->frame = opt_frame_clone(b, &arms->before);
}

static void opt_call_arm_end(opt_builder_t *b, opt_call_arms_t *arms, opt_flow_t step) {
  arms->failed |= step == OPT_FLOW_FAILED;
  if (step != OPT_FLOW_CONTINUE) return;
  arms->ends[arms->count] = b->block;
  arms->frames[arms->count++] = b->frame;
}

static opt_flow_t opt_call_known(opt_builder_t *b, sv_op_t op, ant_value_t known, ir_node_id_t *inputs, uint32_t argc, uint32_t operands) {
  bool tail = op == OP_TAIL_CALL || op == OP_TAIL_CALL_METHOD;
  inputs[0] = opt_constant(b, known);
  sv_func_t *target = js_func_closure(known)->func;
  opt_flow_t step;
  if (!opt_try_inline(b, op, argc, operands, inputs[0], inputs[1], inputs + 2, target, &step))
    step = opt_emit_call(b, false, target, inputs, argc, operands, tail);
  return step;
}

static opt_flow_t opt_join_call_arms(opt_builder_t *b, opt_call_arms_t *arms) {
  if (arms->failed) return OPT_FLOW_FAILED;
  if (!arms->count) return OPT_FLOW_ENDED;
  if (arms->count == 1) {
    b->block = arms->ends[0];
    b->frame = arms->frames[0];
    return OPT_FLOW_CONTINUE;
  }
  const opt_frame_t *frames = arms->frames;
  opt_frame_t joined = opt_frame_clone(b, &frames[0]);
  uint32_t top = opt_stack_index(b, joined.sp - 1);
  for (uint32_t i = 1; i < arms->count; i++)
    if (frames[i].sp != joined.sp) {
      opt_fail(b, "polymorphic-call-frame");
      return OPT_FLOW_FAILED;
    }
  for (uint32_t t = 0; t < b->tracked; t++) {
    if (t == top) continue;
    if (opt_in_memory(b, t)) {
      joined.slots[t] = (opt_slot_t){.node = IR_NONE};
      continue;
    }
    for (uint32_t i = 1; i < arms->count; i++) {
      if (frames[i].slots[t].node != joined.slots[t].node || frames[i].slots[t].boxed != joined.slots[t].boxed) {
        opt_fail(b, "polymorphic-call-frame");
        return OPT_FLOW_FAILED;
      }
      joined.slots[t].in_sync &= frames[i].slots[t].in_sync;
    }
  }
  ir_node_id_t results[OPT_MAX_CALL_ARMS];
  for (uint32_t i = 0; i < arms->count; i++) {
    b->block = arms->ends[i];
    results[i] = opt_tagged(b, frames[i].slots[top].node);
    if (frames[i].this_value != joined.this_value) joined.this_value = IR_NONE;
  }
  opt_join_blocks(b, arms->ends, arms->count);
  joined.slots[top] = (opt_slot_t){.node = opt_add_phi(b, top, results, arms->count)};
  b->frame = joined;
  return OPT_FLOW_CONTINUE;
}

static bool opt_try_polymorphic_call(opt_builder_t *b, sv_op_t op, uint32_t argc, uint32_t operands, const ir_node_id_t *inputs, opt_flow_t *flow) {
  bool tail = op == OP_TAIL_CALL || op == OP_TAIL_CALL_METHOD;
  ir_node_id_t callee = inputs[0];
  opt_call_candidates_t candidates = {.complete = true};
  if (ir_node(b->graph, callee)->op.code != IR_OP_PHI || !opt_collect_callees(b, callee, 0, &candidates) || candidates.count < 2) return false;
  for (uint32_t i = 0; i < candidates.count; i++)
    if (!opt_retain(b, candidates.values[i])) return false;
  opt_call_arms_t arms = {.before = opt_frame_clone(b, &b->frame)};
  ir_node_id_t arm[2 + OPT_MAX_CALL_ARGS];
  memcpy(arm, inputs, (2 + argc) * sizeof *arm);
  for (uint32_t i = 0; i < candidates.count && !arms.failed; i++) {
    ir_condition_t same = ir_cond(IR_COND_TAGGED_EQUALS);
    same.equal = true;
    ir_block_id_t hit, miss;
    opt_branch_both_ways(b, same, (ir_node_id_t[]){callee, opt_constant(b, candidates.values[i])}, 2, &hit, &miss);
    opt_call_arm_begin(b, &arms, hit);
    opt_call_arm_end(b, &arms, opt_call_known(b, op, candidates.values[i], arm, argc, operands));
    opt_call_arm_begin(b, &arms, miss);
  }
  if (!arms.failed) {
    if (candidates.complete && opt_may_speculate(b, OPT_EXIT_BAD_CALL_TARGET)) opt_end_with_exit(b, OPT_EXIT_BAD_CALL_TARGET);
    else opt_call_arm_end(b, &arms, opt_emit_call(b, false, NULL, inputs, argc, operands, tail));
  }
  *flow = opt_join_call_arms(b, &arms);
  return true;
}

static opt_flow_t opt_build_getter_field(opt_builder_t *b, const opt_field_plan_t *plan, ir_node_id_t object, ir_node_id_t address) {
  uint32_t operands = plan->opcode == OP_GET_FIELD ? 1 : 0;
  opt_shape_groups_t groups = {0};
  for (uint32_t i = 0; i < plan->count; i++) opt_shape_groups_add(&groups, plan->cases[i].shape, i);
  ir_block_id_t blocks[OPT_MAX_FIELD_SHAPES];
  if (groups.count == 1) {
    object = opt_check_shape(b, object, address, groups.shapes[0]);
    blocks[0] = b->block;
  } else {
    ir_block_id_t source = b->block;
    ir_shape_case_t cases[OPT_MAX_FIELD_SHAPES];
    for (uint32_t g = 0; g < groups.count; g++) {
      blocks[g] = opt_edge_block(b, source);
      cases[g] = (ir_shape_case_t){.check = {.shape = (ir_cell_t)(uintptr_t)groups.shapes[g]}, .block = blocks[g]};
    }
    ir_op_t shape_switch = ir_op(IR_OP_SHAPE_SWITCH);
    shape_switch.cases.items = ir_dup_cases(b->graph, cases, groups.count);
    shape_switch.cases.len = groups.count;
    ir_frame_state_id_t state = opt_eager_frame_state(b);
    ir_node(b->graph, opt_set_control(b, shape_switch, (ir_node_id_t[]){object, address}, 2))->frame_state = state;
  }
  opt_call_arms_t arms = {.before = opt_frame_clone(b, &b->frame)};
  for (uint32_t g = 0; g < groups.count && !arms.failed; g++) {
    b->block = blocks[g];
    ir_node_id_t prototype = plan->cases[groups.members[g][0]].depth ? opt_emit(b, ir_op(IR_OP_LOAD_PROTOTYPE), &address, 1, OPT_REPR_TAGGED) : IR_NONE;
    for (uint32_t j = 0; j < groups.sizes[g] && !arms.failed; j++) {
      const opt_field_case_t *access = &plan->cases[groups.members[g][j]];
      bool last = j + 1 == groups.sizes[g];
      ir_block_id_t arm = b->block, next = IR_NONE;
      if (!last) {
        ir_condition_t same = ir_cond(IR_COND_TAGGED_EQUALS);
        same.equal = true;
        opt_branch_both_ways(b, same, (ir_node_id_t[]){prototype, opt_constant(b, access->values[0])}, 2, &arm, &next);
      }
      opt_call_arm_begin(b, &arms, arm);
      if (access->depth) {
        ir_node_id_t holder, holder_address;
        opt_holder_chain(b, last ? prototype : IR_NONE, access, true, NULL, NULL, &holder, &holder_address);
      }
      ir_node_id_t inputs[3] = {IR_NONE, object, IR_NONE};
      opt_call_arm_end(b, &arms, opt_call_known(b, OP_CALL_METHOD, access->getter, inputs, 0, operands));
      b->block = next;
    }
  }
  return opt_join_call_arms(b, &arms);
}

static opt_flow_t opt_build_call(opt_builder_t *b, const uint8_t *ip, sv_op_t op) {
  bool construct = op == OP_NEW, tail = op == OP_TAIL_CALL || op == OP_TAIL_CALL_METHOD;
  bool method = op == OP_CALL_METHOD || op == OP_TAIL_CALL_METHOD;
  uint32_t argc = sv_get_u16(ip + 1);
  uint32_t operands = argc + 1 + (method || construct);
  if (argc > OPT_MAX_CALL_ARGS || operands > b->frame.sp) {
    opt_fail(b, "call-argc");
    return OPT_FLOW_FAILED;
  }
  sv_func_t *target = opt_call_target(b);
  bool self_tail = tail && !b->inline_call && target == b->func;
  if (self_tail && b->tail_header == IR_NONE) {
    opt_fail(b, "self-tail-call");
    return OPT_FLOW_FAILED;
  }
  ir_node_id_t callee = opt_tagged(b, opt_peek(b, argc + construct));
  ir_node_id_t second = method ? opt_tagged(b, opt_peek(b, argc + 1))
    : construct ? opt_tagged(b, opt_peek(b, argc)) : opt_constant(b, IR_VALUE_UNDEFINED);
  ir_node_id_t inputs[2 + OPT_MAX_CALL_ARGS];
  inputs[0] = callee;
  inputs[1] = second;
  for (uint32_t i = 0; i < argc; i++) inputs[2 + i] = opt_tagged(b, opt_peek(b, argc - 1 - i));
  if (self_tail) opt_build_self_tail(b, callee, inputs + 2, argc);
  if (method && argc == 2 && opt_is_virtual_arguments(b, inputs[3])) {
    if (!b->inline_call || !opt_expect_builtin(b, callee, js_is_function_apply_builtin)) return opt_no_virtual_arguments(b);
    const opt_inline_t *call = b->inline_call;
    callee = second;
    second = inputs[2];
    argc = call->argc;
    inputs[0] = callee;
    inputs[1] = second;
    for (uint32_t i = 0; i < argc; i++) inputs[2 + i] = call->args[i];
    target = opt_known_target(b, callee);
  } else if (method && opt_expect_builtin(b, callee, js_is_function_call_builtin)) {
    callee = second;
    second = argc ? inputs[2] : opt_constant(b, IR_VALUE_UNDEFINED);
    if (argc) argc--;
    for (uint32_t i = 0; i < argc; i++) inputs[2 + i] = inputs[3 + i];
    inputs[0] = callee;
    inputs[1] = second;
    target = opt_known_target(b, callee);
  }
  if (!construct && method && argc <= 1) {
    bool push = argc == 1 && opt_expect_builtin(b, callee, js_is_array_push_builtin);
    if (push || (!argc && opt_expect_builtin(b, callee, js_is_array_pop_builtin))) return opt_build_push_pop(b, push, inputs, operands, tail);
  }

  if (construct && !argc && callee == second && opt_expect_array_ctor(b, callee)) {
    ir_node_id_t empty[2] = {opt_constant(b, IR_VALUE_UNDEFINED), opt_constant(b, IR_VALUE_UNDEFINED)};
    return opt_finish_call(b, operands, opt_runtime_call(b, (uintptr_t)opt_rt_array, 0, empty, 2, true, false), tail);
  }

  ir_node_id_t math;
  if (!construct && opt_try_math(b, callee, argc, inputs + 2, &math)) return opt_finish_call(b, operands, math, tail);
  opt_flow_t flow;
  if (!construct && opt_try_polymorphic_call(b, op, argc, operands, inputs, &flow)) return flow;
  if (opt_try_inline(b, op, argc, operands, callee, second, inputs + 2, target, &flow)) return flow;
  return opt_emit_call(b, construct, target, inputs, argc, operands, tail);
}

static ir_node_id_t opt_runtime_call(opt_builder_t *b, uintptr_t helper, uintptr_t extra, const ir_node_id_t *inputs, uint32_t count, bool allocates_only, bool needs_frame) {
  ir_op_t call = ir_op(IR_OP_CALL_NATIVE);
  call.call.func = b->func;
  call.call.bc_off = b->pc;
  call.call.stores_operands = true;
  call.call.argc = (uint16_t)(count - 2);
  call.call.extra = extra;
  call.call.helper = helper;
  call.call.allocates_only = allocates_only;
  call.call.needs_frame = needs_frame;
  ir_node_id_t result = opt_emit(b, call, inputs, count, OPT_REPR_TAGGED);
  ir_node(b->graph, result)->frame_state = opt_call_position(b);
  if (!allocates_only) opt_mark_catch(b, result);
  b->graph->uses_frame |= needs_frame;
  return result;
}

static opt_flow_t opt_build_access_call(opt_builder_t *b, bool slow_path) {
  sv_op_t op = opt_op_at(b, b->pc);
  uint32_t operands, popped;
  switch (op) {
    case OP_GET_FIELD: operands = popped = 1; break;
    case OP_GET_FIELD2: operands = 1; popped = 0; break;
    case OP_PUT_FIELD: operands = popped = 2; break;
    case OP_GET_ELEM: operands = popped = 2; break;
    case OP_PUT_ELEM: operands = popped = 3; break;
    case OP_GET_LENGTH: operands = popped = 1; break;
    default: return OPT_FLOW_GENERIC;
  }
  uintptr_t helper = op == OP_PUT_ELEM && !slow_path ? (uintptr_t)opt_rt_put_elem_plain : opt_tier_access_helper(op, b->slow_adds != NULL);
  opt_rt_site_t *site = code_arena_bump(b->js, sizeof *site);
  if (!site) return OPT_FLOW_GENERIC;
  *site = (opt_rt_site_t){.func = b->func, .bc_off = b->pc};
  uintptr_t extra = op == OP_PUT_FIELD && b->slow_adds ? (uintptr_t)b->slow_adds : (uintptr_t)site;
  ir_node_id_t inputs[3];
  inputs[1] = opt_constant(b, IR_VALUE_UNDEFINED);
  for (uint32_t i = 0; i < operands; i++) inputs[i] = opt_tagged(b, opt_peek(b, operands - 1 - i));
  ir_node_id_t result = opt_runtime_call(b, helper, extra, inputs, operands < 2 ? 2 : operands, false, false);
  ir_node(b->graph, result)->op.call.slow_path = slow_path;
  opt_drop(b, popped);
  if (op != OP_PUT_FIELD && op != OP_PUT_ELEM) opt_push(b, result);
  opt_forget_memory(b);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_object(opt_builder_t *b) {
  ant_t *js = b->js;
  sv_obj_site_cache_t *site = sv_obj_site_for_offset(b->func, b->pc);
  if (!site || site->shape_build_failed || !is_object_type(js->sym.object_proto)) return OPT_FLOW_GENERIC;
  ant_shape_t *shape = site->shared_shape;
  if (!shape) return opt_end_with_exit(b, OPT_EXIT_NO_FEEDBACK);
  if (ant_shape_proto(shape) != js->sym.object_proto) return OPT_FLOW_GENERIC;
  if (!jit_snapshot_shape(js, shape)) return OPT_FLOW_GENERIC;
  uint32_t count = ant_shape_count(shape);
  uint8_t limit = ant_shape_get_inobj_limit(shape);
  ir_node_id_t object;
  if (count <= limit) {
    ir_op_t op = ir_op(IR_OP_ALLOCATE_OBJECT);
    op.allocate_object.shape = (ir_cell_t)(uintptr_t)shape;
    op.allocate_object.property_count = count;
    op.allocate_object.reserve = limit;
    ir_node_id_t proto = opt_constant(b, js->sym.object_proto);
    object = opt_emit(b, op, &proto, 1, OPT_REPR_TAGGED);
  } else {
    opt_rt_site_t *at = code_arena_bump(js, sizeof *at);
    if (!at) return OPT_FLOW_GENERIC;
    *at = (opt_rt_site_t){.func = b->func, .bc_off = b->pc};
    ir_node_id_t undefined = opt_constant(b, IR_VALUE_UNDEFINED);
    object = opt_runtime_call(b, (uintptr_t)opt_rt_object, (uintptr_t)at, (ir_node_id_t[]){undefined, undefined}, 2, true, false);
  }
  opt_vec_push(&b->graph->arena, &b->ctx->literal_nodes, object);
  opt_vec_push(&b->graph->arena, &b->ctx->literal_shapes, shape);
  opt_push(b, object);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_define_slot(opt_builder_t *b, const uint8_t *ip) {
  uint32_t atom_index = sv_get_u32(ip + 1), slot = sv_get_u16(ip + 5);
  ir_node_id_t object = opt_peek(b, 1);
  ant_shape_t *shape = NULL;
  for (uint32_t i = b->ctx->literal_nodes.len; i-- > 0 && !shape;)
    if (b->ctx->literal_nodes.items[i] == object) shape = b->ctx->literal_shapes.items[i];
  if (!shape || atom_index >= (uint32_t)b->func->atom_count) return OPT_FLOW_GENERIC;
  const sv_atom_t *atom = &b->func->atoms[atom_index];
  const ant_shape_prop_t *prop = ant_shape_prop_at(shape, slot);
  if (!prop || prop->type != ANT_SHAPE_KEY_STRING || prop->key.interned != atom->str || prop->has_getter || prop->has_setter
    || ant_property_key_is_watched(b->js, atom->str)) return OPT_FLOW_GENERIC;
  ir_op_t op = ir_op(IR_OP_INITIALIZE_NAMED);
  op.offset = opt_named_offset(slot, ant_shape_get_inobj_limit(shape));
  opt_emit(b, op, (ir_node_id_t[]){object, opt_tagged(b, opt_peek(b, 0))}, 2, OPT_REPR_NONE);
  opt_drop(b, 1);
  return OPT_FLOW_CONTINUE;
}

static constexpr uint32_t OPT_MAX_INLINE_ARRAY = 32;
static_assert(OPT_MAX_INLINE_ARRAY >= OPT_MAX_CALL_ARGS, "inline array literals take every call-sized literal");

static opt_flow_t opt_build_array(opt_builder_t *b, const uint8_t *ip) {
  uint32_t count = sv_get_u16(ip + 1);
  bool inline_elements = count > 0 && count <= OPT_MAX_INLINE_ARRAY && gc_array_storage_fit_class(count) >= 0;
  for (uint32_t i = 0; i < count && inline_elements; i++) {
    ir_node_id_t element = opt_peek(b, count - 1 - i);
    uint64_t bits;
    if (ir_constant_value(b->graph, element, &bits) ? bits == IR_VALUE_EMPTY : ir_node(b->graph, element)->op.code == IR_OP_GENERIC)
      inline_elements = false;
  }
  if (!inline_elements && count > OPT_MAX_CALL_ARGS) return OPT_FLOW_GENERIC;
  ir_node_id_t inputs[2 + OPT_MAX_INLINE_ARRAY];
  inputs[0] = inputs[1] = opt_constant(b, IR_VALUE_UNDEFINED);
  for (uint32_t i = 0; i < count; i++) inputs[2 + i] = opt_tagged(b, opt_peek(b, count - 1 - i));
  ir_node_id_t array;
  if (inline_elements) {
    ir_op_t op = ir_op(IR_OP_ALLOCATE_ARRAY);
    op.count = count;
    array = opt_emit(b, op, NULL, 0, OPT_REPR_TAGGED);
    for (uint32_t i = 0; i < count; i++) {
      ir_op_t init = ir_op(IR_OP_INITIALIZE_ELEMENT);
      init.index = i;
      opt_emit(b, init, (ir_node_id_t[]){array, inputs[2 + i]}, 2, OPT_REPR_NONE);
    }
  } else array = opt_runtime_call(b, (uintptr_t)opt_rt_array, 0, inputs, 2 + count, true, false);
  opt_drop(b, count);
  opt_push(b, array);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_closure(opt_builder_t *b, const uint8_t *ip) {
  sv_func_t *func = b->func;
  uint32_t index = sv_get_u32(ip + 1);
  if (index >= (uint32_t)func->const_count || vtype(func->constants[index]) != kTypeFunctionInfo) return OPT_FLOW_GENERIC;
  const sv_func_t *child = (const sv_func_t *)vptr(func->constants[index]);
  bool captures = false, boxed = true, by_reference = false;
  for (int u = 0; u < child->upvalue_count; u++) {
    const sv_upval_desc_t *desc = &child->upval_descs[u];
    if (!desc->is_local) continue;
    captures = true;
    if (desc->by_value) continue;
    by_reference = true;
    boxed &= opt_boxable(b, desc->index);
  }
  if (by_reference && !boxed) {
    if (opt_bitset_is_empty(&b->boxable)) return OPT_FLOW_GENERIC;
    opt_fail(b, "closure-capture");
    return OPT_FLOW_FAILED;
  }
  if (child->is_arrow && b->inline_call) return OPT_FLOW_GENERIC;
  ir_node_id_t this_val = child->is_arrow ? opt_root_this(b) : opt_constant(b, IR_VALUE_UNDEFINED), parent;
  uint64_t bits;
  if (!b->inline_call) {
    ir_op_t load = ir_op(IR_OP_LOAD_FRAME_FIELD);
    load.field = IR_FIELD_EXECUTABLE;
    parent = opt_emit(b, load, NULL, 0, OPT_REPR_POINTER);
  } else if (ir_constant_value(b->graph, b->inline_call->closure, &bits) && vtype(bits) == kTypeFunction) {
    parent = opt_typed_constant(b, (uint64_t)(uintptr_t)js_func_closure(bits), OPT_REPR_POINTER);
  } else parent = opt_emit1(b, IR_OP_CELL_ADDRESS, b->inline_call->closure, OPT_REPR_POINTER);

  opt_rt_closure_site_t *site = code_arena_bump(b->js, sizeof *site);
  if (!site) return OPT_FLOW_GENERIC;
  *site = (opt_rt_closure_site_t){.func = func, .index = index};
  bool named = b->ins + 1 <= b->blocks[b->block_of_ins[b->ins]].last && opt_op_at(b, b->ins_off[b->ins + 1]) == OP_SET_NAME;
  if (named) {
    uint32_t atom = sv_get_u32(func->code + b->ins_off[b->ins + 1] + 1);
    if (atom >= (uint32_t)func->atom_count) named = false;
    else {
      site->name = func->atoms[atom].str;
      site->name_len = func->atoms[atom].len;
    }
  }
  ir_node_id_t inputs[2 + OPT_MAX_CALL_ARGS] = {parent, this_val};
  uint32_t count = 2;
  for (int u = 0; by_reference && u < child->upvalue_count; u++)
    for (int v = 0; v < u; v++)
      if (child->upval_descs[u].is_local && !child->upval_descs[u].by_value && child->upval_descs[v].is_local &&
          !child->upval_descs[v].by_value && child->upval_descs[v].index == child->upval_descs[u].index)
        opt_box_slot(b, child->upval_descs[u].index);
  for (int u = 0; captures && u < child->upvalue_count; u++) {
    const sv_upval_desc_t *desc = &child->upval_descs[u];
    if (!desc->is_local) continue;
    if (count >= 2 + OPT_MAX_CALL_ARGS) {
      opt_fail(b, "closure-captures");
      return OPT_FLOW_FAILED;
    }
    if (desc->by_value) {
      inputs[count++] = opt_tagged(b, opt_read_tracked(b, desc->index));
      continue;
    }
    opt_slot_t *slot = &b->frame.slots[desc->index];
    if (!slot->boxed) site->fresh |= 1u << (count - 2);
    inputs[count++] = slot->boxed ? slot->node : opt_tagged(b, opt_read_tracked(b, desc->index));
  }
  ir_node_id_t closure = opt_runtime_call(b, captures ? (uintptr_t)opt_rt_closure_boxes : (uintptr_t)opt_rt_closure, (uintptr_t)site, inputs, count, true, false);
  for (int u = 0; site->fresh && u < child->upvalue_count; u++) {
    const sv_upval_desc_t *desc = &child->upval_descs[u];
    opt_slot_t *slot = &b->frame.slots[desc->index];
    if (!desc->is_local || desc->by_value || slot->boxed) continue;
    ir_op_t load_cell = ir_op(IR_OP_LOAD_UPVALUE_CELL);
    load_cell.index = (uint32_t)u;
    ir_node_id_t cell = opt_emit(b, load_cell, &closure, 1, OPT_REPR_POINTER);
    *slot = (opt_slot_t){.node = cell, .boxed = true};
  }
  if (named) opt_vec_push(&b->graph->arena, &b->ctx->named_closures, closure);
  opt_push(b, closure);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_call_call(opt_builder_t *b, const uint8_t *ip, sv_op_t op) {
  uint32_t argc = op == OP_CALL_CALL ? ip[2] : 1, operands = 2 + argc - (op == OP_CALL_CALL_SLOT);
  if ((op == OP_CALL_CALL && ip[1] != 1) || argc > OPT_MAX_CALL_ARGS || operands > b->frame.sp) {
    opt_fail(b, "call-argc");
    return OPT_FLOW_FAILED;
  }
  ir_node_id_t inputs[2 + OPT_MAX_CALL_ARGS];
  for (uint32_t i = 0; i < operands; i++) inputs[i] = opt_tagged(b, opt_peek(b, operands - 1 - i));
  if (op == OP_CALL_CALL_SLOT) {
    uint32_t slot = sv_get_u16(ip + 1);
    if (slot >= b->params + b->locals || opt_in_memory(b, slot) || b->frame.slots[slot].boxed) {
      opt_fail(b, "call-call-slot");
      return OPT_FLOW_FAILED;
    }
    inputs[2] = opt_tagged(b, opt_read_tracked(b, slot));
  }
  ir_node_id_t result = opt_runtime_call(b, (uintptr_t)opt_rt_call_call, 0, inputs, 2 + argc, false, false);
  return opt_finish_call(b, operands, result, false);
}

static bool opt_concat_feedback(const opt_builder_t *b) {
  uint8_t classes = opt_feedback(b) & SV_TFB_CLASS_MASK;
  return (classes & SV_TFB_STR) && !(classes & ~(SV_TFB_STR | SV_TFB_NUM));
}

static void opt_expect_string_or_number(opt_builder_t *b, ir_node_id_t value) {
  const ir_node_t *node = ir_node(b->graph, value);
  if (node->repr == OPT_REPR_INT32 || node->repr == OPT_REPR_FLOAT64) return;
  if (node->op.code == IR_OP_CONSTANT && (ir_value_is_number(node->op.bits) || vtype((ant_value_t)node->op.bits) == kTypeString)) return;
  ir_block_id_t string, other;
  opt_branch_both_ways(b, ir_cond(IR_COND_STRING), &value, 1, &string, &other);
  b->block = other;
  opt_emit_checked(b, ir_op(IR_OP_CHECK_NUMBER), &value, 1, OPT_REPR_FLOAT64);
  opt_join_blocks(b, (ir_block_id_t[]){string, b->block}, 2);
}

static opt_flow_t opt_build_concat(opt_builder_t *b) {
  if (!opt_may_speculate(b, OPT_EXIT_BAD_TYPE)) return OPT_FLOW_GENERIC;
  ir_op_t op = ir_op(IR_OP_CONCATENATE_STRINGS);
  ir_node_id_t inputs[2] = {opt_peek(b, 1), opt_peek(b, 0)};
  for (uint32_t i = 0; i < 2; i++) {
    if (ir_node(b->graph, inputs[i])->repr == OPT_REPR_INT32) {
      op.count |= IR_CONCAT_INT32_LHS << i;
      continue;
    }
    inputs[i] = opt_tagged(b, inputs[i]);
    opt_expect_string_or_number(b, inputs[i]);
  }
  if (opt_feedback(b) & SV_TFB_NUM) op.count |= IR_CONCAT_NUMBERS;
  ir_node_id_t result = opt_emit(b, op, inputs, 2, OPT_REPR_TAGGED);
  opt_drop(b, 2);
  opt_push(b, result);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_bad_operand(opt_builder_t *b) {
  opt_fail(b, "operand");
  return OPT_FLOW_FAILED;
}

static opt_flow_t opt_build_constant_op(opt_builder_t *b, const uint8_t *ip, sv_op_t op) {
  uint64_t bits;
  switch (op) {
    case OP_CONST_I8: bits = ir_value_int32(sv_get_i8(ip + 1)); break;
    case OP_CONST: case OP_CONST8: {
      uint32_t index = op == OP_CONST ? sv_get_u32(ip + 1) : sv_get_u8(ip + 1);
      if (index >= (uint32_t)b->func->const_count) return opt_build_bad_operand(b);
      ant_value_t value = b->func->constants[index];
      if (vtype(value) == kTypeFunctionInfo || opt_bitset_contains(&b->patched_constants, index)) return OPT_FLOW_GENERIC;
      bits = vtype(value) == kTypeNumber ? ir_value_number(tod(value)) : value;
      break;
    }
    case OP_UNDEF: bits = IR_VALUE_UNDEFINED; break;
    case OP_NULL: bits = IR_VALUE_NULL; break;
    case OP_TRUE: bits = IR_VALUE_TRUE; break;
    case OP_FALSE: bits = IR_VALUE_FALSE; break;
    case OP_GLOBAL: bits = b->js->global_this; break;
    default: return opt_build_bad_operand(b);
  }
  opt_push(b, opt_constant(b, bits));
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_this(opt_builder_t *b) {
  opt_push(b, b->inline_call ? b->inline_call->receiver : opt_root_this(b));
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_stack_shuffle(opt_builder_t *b, sv_op_t op) {
  switch (op) {
    case OP_POP: opt_drop(b, 1); break;
    case OP_DUP: opt_push(b, opt_peek(b, 0)); break;
    case OP_DUP2: {
      ir_node_id_t x = opt_peek(b, 1), y = opt_peek(b, 0);
      opt_push(b, x);
      opt_push(b, y);
      break;
    }
    case OP_SWAP: {
      ir_node_id_t top = opt_peek(b, 0);
      opt_poke(b, 0, opt_peek(b, 1));
      opt_poke(b, 1, top);
      break;
    }
    case OP_NIP: {
      ir_node_id_t top = opt_peek(b, 0);
      opt_drop(b, 1);
      opt_poke(b, 0, top);
      break;
    }
    case OP_INSERT2: {
      ir_node_id_t a = opt_peek(b, 0), object = opt_peek(b, 1);
      opt_poke(b, 1, a);
      opt_poke(b, 0, object);
      opt_push(b, a);
      break;
    }
    case OP_INSERT3: {
      ir_node_id_t a = opt_peek(b, 0), prop = opt_peek(b, 1), object = opt_peek(b, 2);
      opt_poke(b, 2, a);
      opt_poke(b, 1, object);
      opt_poke(b, 0, prop);
      opt_push(b, a);
      break;
    }
    case OP_SWAP_UNDER: {
      ir_node_id_t t = opt_peek(b, 1);
      opt_poke(b, 1, opt_peek(b, 2));
      opt_poke(b, 2, t);
      break;
    }
    case OP_ROT3L: {
      ir_node_id_t x = opt_peek(b, 2), a = opt_peek(b, 1), c = opt_peek(b, 0);
      opt_poke(b, 2, a);
      opt_poke(b, 1, c);
      opt_poke(b, 0, x);
      break;
    }
    case OP_ROT3R: {
      ir_node_id_t a = opt_peek(b, 2), c = opt_peek(b, 1), x = opt_peek(b, 0);
      opt_poke(b, 2, x);
      opt_poke(b, 1, a);
      opt_poke(b, 0, c);
      break;
    }
    case OP_ROT4_UNDER: {
      ir_node_id_t a = opt_peek(b, 3), c = opt_peek(b, 2), d = opt_peek(b, 1);
      opt_poke(b, 3, d);
      opt_poke(b, 2, a);
      opt_poke(b, 1, c);
      break;
    }
    default: return opt_build_bad_operand(b);
  }
  return OPT_FLOW_CONTINUE;
}

static ir_node_id_t opt_read_local_value(opt_builder_t *b, uint32_t tracked) {
  ir_node_id_t value = opt_read_tracked(b, tracked);
  if (!opt_bitset_contains(&b->builders, tracked)) return value;
  ir_node_id_t inputs[2] = {opt_tagged(b, value), opt_constant(b, IR_VALUE_UNDEFINED)};
  return opt_runtime_call(b, (uintptr_t)opt_rt_builder_read, 0, inputs, 2, true, false);
}

static opt_flow_t opt_build_argument_slot(opt_builder_t *b, const uint8_t *ip, sv_op_t op) {
  uint32_t index = sv_get_u16(ip + 1);
  if (index >= b->params + (op == OP_GET_SLOT_RAW ? b->locals : 0)) return opt_build_bad_operand(b);
  if (op == OP_GET_ARG) opt_push(b, opt_read_local_value(b, index));
  else if (op == OP_GET_SLOT_RAW) opt_push(b, opt_read_tracked(b, index));
  else {
    opt_write_tracked(b, index, opt_peek(b, 0));
    if (op == OP_PUT_ARG) opt_drop(b, 1);
  }
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_local_slot(opt_builder_t *b, const uint8_t *ip, sv_op_t op) {
  uint32_t index = sv_op_slot_operand(ip);
  if (index >= b->locals) return opt_build_bad_operand(b);
  uint32_t tracked = b->params + index;
  bool read = op == OP_GET_LOCAL || op == OP_GET_LOCAL8;
  if (read) opt_push(b, opt_read_local_value(b, tracked));
  else if (op == OP_SET_LOCAL_UNDEF) opt_write_tracked(b, tracked, opt_constant(b, IR_VALUE_EMPTY));
  else {
    opt_write_tracked(b, tracked, opt_peek(b, 0));
    if (op == OP_PUT_LOCAL || op == OP_PUT_LOCAL8) opt_drop(b, 1);
  }
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_checked_local(opt_builder_t *b, const uint8_t *ip, sv_op_t op) {
  uint32_t index = sv_get_u16(ip + 1);
  uint32_t tracked = b->params + index;
  if (index >= b->locals || !b->frame.slots[tracked].boxed) return OPT_FLOW_GENERIC;
  if (!opt_may_speculate(b, OPT_EXIT_UNEXPECTED_VALUE)) {
    opt_fail(b, "boxed-tdz");
    return OPT_FLOW_FAILED;
  }
  ir_node_id_t box = b->frame.slots[tracked].node;
  ir_node_id_t value = opt_emit_checked(b, ir_op(IR_OP_LOAD_UPVALUE), &box, 1, OPT_REPR_TAGGED);
  if (op == OP_GET_LOCAL_CHK) opt_push(b, value);
  else {
    opt_write_tracked(b, tracked, opt_peek(b, 0));
    opt_drop(b, 1);
  }
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_local_update(opt_builder_t *b, const uint8_t *ip, sv_op_t op) {
  uint32_t index = sv_op_slot_operand(ip);
  if (index >= b->locals) return opt_build_bad_operand(b);
  uint32_t tracked = b->params + index;
  if (opt_bitset_contains(&b->builders, tracked)) {
    if (!b->frame.slots[tracked].boxed) return OPT_FLOW_GENERIC;
    opt_fail(b, "builder-boxed");
    return OPT_FLOW_FAILED;
  }
  ir_node_id_t result;
  if (op == OP_ADD_LOCAL) {
    opt_flow_t flow = opt_numeric_feedback(b, true);
    if (flow != OPT_FLOW_CONTINUE) return flow;
    result = opt_binary(b, IR_BINARY_ADD, opt_read_tracked(b, tracked), opt_peek(b, 0));
    if (result == IR_NONE) return OPT_FLOW_GENERIC;
    opt_drop(b, 1);
  } else {
    ir_node_id_t old;
    opt_flow_t flow = opt_build_update(b, op == OP_INC_LOCAL ? IR_BINARY_ADD : IR_BINARY_SUB, opt_read_tracked(b, tracked), &old, &result);
    if (flow != OPT_FLOW_CONTINUE) return flow;
  }
  opt_write_tracked(b, tracked, result);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_arithmetic(opt_builder_t *b, sv_op_t op) {
  bool recorded = op == OP_ADD || op == OP_SUB || op == OP_MUL || op == OP_DIV;
  if (op == OP_ADD && opt_concat_feedback(b)) return opt_build_concat(b);
  opt_flow_t flow = opt_numeric_feedback(b, recorded);
  if (flow != OPT_FLOW_CONTINUE) return flow;
  ir_node_id_t result = opt_binary(b, opt_binary_of(op), opt_peek(b, 1), opt_peek(b, 0));
  if (result == IR_NONE) return OPT_FLOW_GENERIC;
  opt_drop(b, 2);
  opt_push(b, result);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_exponentiation(opt_builder_t *b) {
  ir_node_id_t args[2] = {opt_peek(b, 1), opt_peek(b, 0)};
  if (!opt_may_speculate(b, OPT_EXIT_BAD_TYPE) || !opt_may_be_number(b, args[0]) || !opt_may_be_number(b, args[1])) return OPT_FLOW_GENERIC;
  ir_node_id_t result = opt_call_float64_function(b, ANT_MATH_DOUBLE_POW, args);
  opt_drop(b, 2);
  opt_push(b, result);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_increment(opt_builder_t *b, sv_op_t op) {
  bool post = op == OP_POST_INC || op == OP_POST_DEC;
  bool increment = op == OP_INC || op == OP_POST_INC;
  ir_node_id_t old, result;
  opt_flow_t flow = opt_build_update(b, increment ? IR_BINARY_ADD : IR_BINARY_SUB, opt_peek(b, 0), &old, &result);
  if (flow != OPT_FLOW_CONTINUE) return flow;
  if (post) {
    opt_poke(b, 0, old);
    opt_push(b, result);
  } else opt_poke(b, 0, result);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_negate(opt_builder_t *b) {
  opt_flow_t flow = opt_numeric_feedback(b, true);
  if (flow != OPT_FLOW_CONTINUE) return flow;
  ir_node_id_t value = opt_peek(b, 0), result;
  switch (opt_number_speculation(b, value, IR_NONE, true, true)) {
    case OPT_SPEC_INT32:
      result = opt_int32_binary(b, IR_BINARY_MUL, opt_int32(b, value), opt_int32_constant(b, -1));
      break;
    case OPT_SPEC_FLOAT64: {
      ir_op_t unary = ir_op(IR_OP_FLOAT64_UNARY);
      unary.unary = IR_UNARY_NEGATE;
      ir_node_id_t number = opt_number(b, value);
      result = opt_emit1(b, IR_OP_BOX_FLOAT64, opt_emit(b, unary, &number, 1, OPT_REPR_FLOAT64), OPT_REPR_TAGGED);
      break;
    }
    default: return OPT_FLOW_GENERIC;
  }
  opt_poke(b, 0, result);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_remainder(opt_builder_t *b) {
  opt_flow_t flow = opt_numeric_feedback(b, true);
  if (flow != OPT_FLOW_CONTINUE) return flow;
  ir_node_id_t lhs = opt_peek(b, 1), rhs = opt_peek(b, 0);
  if (opt_number_speculation(b, lhs, rhs, true, true) != OPT_SPEC_INT32) return OPT_FLOW_GENERIC;
  ir_node_id_t l = opt_int32(b, lhs), r = opt_int32(b, rhs);
  ir_node_id_t result = opt_int32_binary(b, IR_BINARY_MOD, l, r);
  opt_drop(b, 2);
  opt_push(b, result);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_relational(opt_builder_t *b, sv_op_t op) {
  opt_flow_t flow = opt_numeric_feedback(b, true);
  if (flow != OPT_FLOW_CONTINUE) return flow;
  ir_node_id_t result = opt_compare(b, opt_comparison_of(op), opt_peek(b, 1), opt_peek(b, 0));
  if (result == IR_NONE) return OPT_FLOW_GENERIC;
  opt_drop(b, 2);
  opt_push(b, opt_box_bool(b, result));
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_is_constant(opt_builder_t *b, sv_op_t op) {
  ir_op_t equals = ir_op(IR_OP_TAGGED_EQUALS);
  equals.equal = true;
  ir_node_id_t inputs[2] = {opt_tagged(b, opt_peek(b, 0)), opt_constant(b, op == OP_IS_UNDEF ? IR_VALUE_UNDEFINED : IR_VALUE_NULL)};
  opt_poke(b, 0, opt_box_bool(b, opt_emit(b, equals, inputs, 2, OPT_REPR_BOOL)));
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_typeof_is(opt_builder_t *b, const uint8_t *ip) {
  ir_op_t is = ir_op(IR_OP_TYPEOF_IS);
  is.typeof_is.equal = true;
  switch (ip[1]) {
    case kTypeNumber: is.typeof_is.kind = IR_TYPEOF_NUMBER; break;
    case kTypeUndefined: is.typeof_is.kind = IR_TYPEOF_UNDEFINED; break;
    case kTypeString: is.typeof_is.kind = IR_TYPEOF_STRING; break;
    case kTypeSymbol: is.typeof_is.kind = IR_TYPEOF_SYMBOL; break;
    case kTypeBool: is.typeof_is.kind = IR_TYPEOF_BOOLEAN; break;
    case kTypeBigInt: is.typeof_is.kind = IR_TYPEOF_BIGINT; break;
    case kTypeObject: is.typeof_is.kind = IR_TYPEOF_OBJECT; break;
    case kTypeFunction: is.typeof_is.kind = IR_TYPEOF_FUNCTION; break;
    default: return OPT_FLOW_GENERIC;
  }
  ir_node_id_t value = opt_tagged(b, opt_peek(b, 0));
  opt_poke(b, 0, opt_box_bool(b, opt_emit(b, is, &value, 1, OPT_REPR_BOOL)));
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_jump_not_nullish(opt_builder_t *b) {
  ir_node_id_t value = opt_tagged(b, opt_peek(b, 0));
  opt_bblock_t *bblock = &b->blocks[b->block_of_ins[b->ins]];
  return opt_end_with_branch(b, ir_cond(IR_COND_NULLISH), &value, 1, opt_successor_after(b), bblock->succ[0])
    ? OPT_FLOW_ENDED : OPT_FLOW_FAILED;
}

static opt_flow_t opt_build_throw(opt_builder_t *b) {
  ir_node_id_t value = opt_tagged(b, opt_peek(b, 0));
  ir_node_id_t node = opt_set_control(b, ir_op(IR_OP_THROW), &value, 1);
  opt_mark_catch(b, node);
  return OPT_FLOW_ENDED;
}

static opt_flow_t opt_build_virtual_arguments(opt_builder_t *b) {
  if (b->arguments_marker == IR_NONE) {
    opt_vec_push(&b->graph->arena, &b->ctx->virtual_arguments, b->func);
    ir_op_t marker = ir_op(IR_OP_CONSTANT);
    marker.bits = b->func->is_strict ? IR_VALUE_VIRTUAL_UNMAPPED_ARGUMENTS : IR_VALUE_VIRTUAL_MAPPED_ARGUMENTS;
    b->arguments_marker = ir_add_node(b->graph, marker, NULL, 0, OPT_REPR_TAGGED, 0);
  }
  opt_push(b, b->arguments_marker);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_super_value(opt_builder_t *b) {
  ir_op_t load = ir_op(IR_OP_LOAD_FRAME_FIELD);
  load.field = IR_FIELD_SUPER_VALUE;
  if (!b->inline_call) {
    opt_push(b, opt_emit(b, load, NULL, 0, OPT_REPR_TAGGED));
    return OPT_FLOW_CONTINUE;
  }
  uint64_t bits;
  ir_node_id_t closure = b->inline_call->closure;
  if (ir_constant_value(b->graph, closure, &bits) && vtype(bits) == kTypeFunction) {
    opt_push(b, opt_constant(b, js_func_closure(bits)->super_val));
    return OPT_FLOW_CONTINUE;
  }
  opt_push(b, opt_emit(b, load, &closure, 1, OPT_REPR_TAGGED));
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_special_object(opt_builder_t *b, const uint8_t *ip) {
  if (ip[1] == SV_SPECIAL_ARGUMENTS && b->virtual_arguments) return opt_build_virtual_arguments(b);
  if (ip[1] == SV_SPECIAL_SUPER) return opt_build_super_value(b);
  return OPT_FLOW_GENERIC;
}

static opt_flow_t opt_build_to_string(opt_builder_t *b, sv_op_t op) {
  ir_node_id_t value = opt_peek(b, 0);
  const ir_node_t *node = ir_node(b->graph, value);
  uint64_t bits;
  bool number = node->repr == OPT_REPR_INT32 || node->repr == OPT_REPR_FLOAT64 || (ir_constant_value(b->graph, value, &bits) && ir_value_is_number(bits));
  if (op == OP_TO_STRING_DEFER_NUMBER && number) return OPT_FLOW_CONTINUE;
  if (node->op.code == IR_OP_CONSTANT && node->repr == OPT_REPR_TAGGED && vtype((ant_value_t)node->op.bits) == kTypeString) return OPT_FLOW_CONTINUE;
  ir_node_id_t inputs[2] = {opt_tagged(b, value), opt_constant(b, IR_VALUE_UNDEFINED)};
  if (number) {
    opt_poke(b, 0, opt_runtime_call(b, (uintptr_t)opt_rt_to_string, op == OP_TO_STRING_DEFER_NUMBER, inputs, 2, false, false));
    return OPT_FLOW_CONTINUE;
  }
  ir_block_id_t not_string;
  opt_branch_off(b, ir_cond(IR_COND_STRING), &inputs[0], 1, false, &not_string);
  ir_node_id_t results[2] = {inputs[0]};
  ir_block_id_t ends[2] = {b->block};
  b->block = not_string;
  results[1] = opt_runtime_call(b, (uintptr_t)opt_rt_to_string, op == OP_TO_STRING_DEFER_NUMBER, inputs, 2, false, false);
  ends[1] = b->block;
  opt_join_blocks(b, ends, 2);
  opt_poke(b, 0, opt_value_phi(b, b->frame.sp - 1, results, 2));
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_regexp(opt_builder_t *b) {
  ir_node_id_t inputs[2] = {opt_tagged(b, opt_peek(b, 1)), opt_tagged(b, opt_peek(b, 0))};
  ir_node_id_t regexp = opt_runtime_call(b, (uintptr_t)opt_rt_regexp, 0, inputs, 2, false, false);
  opt_drop(b, 2);
  opt_push(b, regexp);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_in_field(opt_builder_t *b) {
  opt_rt_site_t *site = code_arena_bump(b->js, sizeof *site);
  if (!site) return OPT_FLOW_GENERIC;
  *site = (opt_rt_site_t){.func = b->func, .bc_off = b->pc};
  ir_node_id_t inputs[2] = {opt_tagged(b, opt_peek(b, 0)), opt_constant(b, IR_VALUE_UNDEFINED)};
  opt_poke(b, 0, opt_runtime_call(b, (uintptr_t)opt_rt_in_field, (uintptr_t)site, inputs, 2, false, false));
  opt_forget_memory(b);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_in(opt_builder_t *b) {
  ir_node_id_t inputs[2] = {opt_tagged(b, opt_peek(b, 1)), opt_tagged(b, opt_peek(b, 0))};
  ir_node_id_t result = opt_runtime_call(b, (uintptr_t)opt_rt_in, 0, inputs, 2, false, false);
  opt_drop(b, 2);
  opt_push(b, result);
  opt_forget_memory(b);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_for_in(opt_builder_t *b) {
  ir_node_id_t inputs[2] = {opt_tagged(b, opt_peek(b, 0)), opt_constant(b, IR_VALUE_UNDEFINED)};
  opt_poke(b, 0, opt_runtime_call(b, (uintptr_t)opt_rt_for_in, 0, inputs, 2, false, false));
  opt_forget_memory(b);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_typeof(opt_builder_t *b) {
  ir_node_id_t inputs[2] = {opt_tagged(b, opt_peek(b, 0)), opt_constant(b, IR_VALUE_UNDEFINED)};
  opt_poke(b, 0, opt_runtime_call(b, (uintptr_t)opt_rt_typeof, 0, inputs, 2, true, false));
  return OPT_FLOW_CONTINUE;
}

static bool opt_is_literal(const opt_builder_t *b, ir_node_id_t node) {
  for (uint32_t i = b->ctx->literal_nodes.len; i-- > 0;)
    if (b->ctx->literal_nodes.items[i] == node) return true;
  return false;
}

static opt_flow_t opt_build_set_proto(opt_builder_t *b) {
  ir_node_id_t object = opt_tagged(b, opt_peek(b, 1)), proto = opt_tagged(b, opt_peek(b, 0));
  uint64_t bits;
  if (ir_constant_value(b->graph, proto, &bits)) {
    ir_node_t *node = ir_node(b->graph, object);
    if (!js_literal_proto_ok((ant_value_t)bits)) {
      opt_drop(b, 1);
      return OPT_FLOW_CONTINUE;
    }
    if (node->op.code == IR_OP_ALLOCATE_OBJECT && node->inputs.len == 1 && opt_is_literal(b, object)) {
      ir_set_inputs(b->graph, object, &proto, 1);
      opt_drop(b, 1);
      return OPT_FLOW_CONTINUE;
    }
  }
  opt_runtime_call(b, (uintptr_t)opt_rt_set_proto, 0, (ir_node_id_t[]){object, proto}, 2, false, false);
  opt_drop(b, 1);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_string_append(opt_builder_t *b, const uint8_t *ip, sv_op_t op) {
  uint32_t slot = sv_get_u16(ip + 1);
  if (slot >= b->params + b->locals) return opt_build_bad_operand(b);
  if (opt_in_memory(b, slot)) return OPT_FLOW_GENERIC;
  bool snapshot = op == OP_STR_ALC_SNAPSHOT, boxed = b->frame.slots[slot].boxed;
  ir_node_id_t box = boxed ? b->frame.slots[slot].node : IR_NONE;
  uintptr_t helper = snapshot ? (uintptr_t)opt_rt_str_append_snapshot : (uintptr_t)opt_rt_str_append;
  ir_node_id_t current = boxed ? IR_NONE : opt_tagged(b, opt_read_tracked(b, slot)), rhs = opt_tagged(b, opt_peek(b, 0));
  if (snapshot) {
    ir_node_id_t inputs[3] = {boxed ? box : current, rhs, opt_tagged(b, opt_peek(b, 1))};
    ir_node_id_t result = opt_runtime_call(b, helper, boxed, inputs, 3, false, false);
    opt_drop(b, 2);
    if (!boxed) opt_write_tracked(b, slot, result);
    opt_forget_memory(b);
    return OPT_FLOW_CONTINUE;
  }
  opt_frame_t before = opt_frame_clone(b, &b->frame);
  ir_node_id_t target = boxed ? opt_emit1(b, IR_OP_LOAD_ENVIRONMENT_BINDING, box, OPT_REPR_TAGGED) : current;
  ir_node_id_t appended = opt_emit(b, ir_op(IR_OP_STRING_BUILDER_APPEND), (ir_node_id_t[]){target, rhs}, 2, OPT_REPR_BOOL);
  ir_block_id_t slow;
  opt_branch_off(b, ir_cond(IR_COND_BOOL), &appended, 1, false, &slow);
  opt_drop(b, 1);
  opt_push(b, target);
  ir_block_id_t fast_end = b->block;
  opt_frame_t fast = b->frame;
  ir_block(b->graph, slow)->is_cold = true;
  b->block = slow;
  b->frame = opt_frame_clone(b, &before);
  ir_node_id_t result = opt_runtime_call(b, helper, boxed, (ir_node_id_t[]){boxed ? box : current, rhs}, 2, false, false);
  opt_forget_memory(b);
  opt_drop(b, 1);
  opt_push(b, result);
  ir_block_id_t slow_end = b->block;
  opt_frame_t slow_frame = b->frame;
  b->frame = fast;
  opt_join_slow_path(b, fast_end, slow_end, &slow_frame, 1);
  ir_node_id_t value = opt_peek(b, 0);
  opt_drop(b, 1);
  if (!boxed) opt_write_tracked(b, slot, value);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_close_upvalues(opt_builder_t *b, const uint8_t *ip) {
  uint32_t from = sv_get_u16(ip + 1);
  if (!opt_bitset_is_empty(&b->boxable)) {
    opt_bitset_for_each(&b->boxable, t) {
      opt_slot_t *slot = &b->frame.slots[t];
      if (t >= from && slot->boxed) *slot = (opt_slot_t){.node = opt_emit1(b, IR_OP_LOAD_ENVIRONMENT_BINDING, slot->node, OPT_REPR_TAGGED)};
    }
    return OPT_FLOW_CONTINUE;
  }
  if (opt_bitset_is_empty(&b->memory)) return OPT_FLOW_CONTINUE;
  if (b->inline_call) return OPT_FLOW_GENERIC;
  ir_node_id_t undefined = opt_constant(b, IR_VALUE_UNDEFINED);
  opt_runtime_call(b, (uintptr_t)opt_rt_close_upvalues, from, (ir_node_id_t[]){undefined, undefined}, 2, false, true);
  return OPT_FLOW_CONTINUE;
}

static ir_node_id_t opt_iteration_call(opt_builder_t *b, uintptr_t helper, uintptr_t extra, const ir_node_id_t *inputs, uint32_t count, uint32_t slots) {
  ir_op_t call = ir_op(IR_OP_CALL_NATIVE);
  call.call.func = b->func;
  call.call.bc_off = b->pc;
  call.call.stores_operands = true;
  call.call.argc = (uint16_t)(slots - 2);
  call.call.extra = extra;
  call.call.helper = helper;
  ir_node_id_t result = opt_emit(b, call, inputs, count, OPT_REPR_TAGGED);
  ir_node(b->graph, result)->frame_state = opt_call_position(b);
  opt_mark_catch(b, result);
  opt_forget_memory(b);
  return result;
}

static ir_node_id_t opt_call_output(opt_builder_t *b, ir_node_id_t call, uint32_t slot) {
  ir_op_t op = ir_op(IR_OP_SLOW_PATH_OUTPUT);
  op.output_index = (uint8_t)slot;
  return opt_emit(b, op, &call, 1, OPT_REPR_TAGGED);
}

static uint64_t opt_iteration_tag(uint8_t kind) {
  return tov((double)kind);
}

static bool opt_is_iteration_tag(const opt_builder_t *b, ir_node_id_t tag, uint8_t kind) {
  uint64_t bits;
  return ir_constant_value(b->graph, tag, &bits) && ir_node(b->graph, tag)->repr == OPT_REPR_TAGGED && bits == opt_iteration_tag(kind);
}

static bool opt_is_known_iteration_tag(const opt_builder_t *b, ir_node_id_t tag) {
  for (uint8_t kind = SV_ITER_GENERIC; kind <= SV_ITER_DONE; kind++)
    if (opt_is_iteration_tag(b, tag, kind)) return true;
  return false;
}

static bool opt_iteration_tag_may_be(const opt_builder_t *b, ir_node_id_t tag, uint8_t kind) {
  if (opt_is_known_iteration_tag(b, tag)) return opt_is_iteration_tag(b, tag, kind);
  const ir_node_t *node = ir_node(b->graph, tag);
  if (node->op.code != IR_OP_PHI || node->inputs.len == 0) return true;
  for (uint32_t i = 0; i < node->inputs.len; i++) {
    ir_node_id_t input = node->inputs.items[i];
    if (!opt_is_known_iteration_tag(b, input) || opt_is_iteration_tag(b, input, kind)) return true;
  }
  return false;
}

static uint32_t opt_ins_index(const opt_builder_t *b, uint32_t off) {
  uint32_t low = 0, high = b->ins_count;
  while (low < high) {
    uint32_t mid = (low + high) / 2;
    if (b->ins_off[mid] < off) low = mid + 1;
    else high = mid;
  }
  return low < b->ins_count && b->ins_off[low] == off ? low : UINT32_MAX;
}

static uint8_t opt_iteration_seen(const opt_builder_t *b, uint32_t init) {
  uint8_t *feedback = sv_func_type_feedback(b->func);
  return feedback && init < (uint32_t)b->func->code_len ? feedback[init] : 0;
}

static uint32_t opt_iteration_init_of(const opt_builder_t *b) {
  int32_t state = opt_try_state(b, b->pc);
  uint32_t at = state >= 0 ? opt_ins_index(b, (uint32_t)state) : UINT32_MAX;
  if (at == UINT32_MAX || at == 0) return UINT32_MAX;
  uint32_t init = b->ins_off[at - 1];
  sv_op_t op = opt_op_at(b, init);
  return op == OP_FOR_OF || op == OP_DESTRUCTURE_INIT ? init : UINT32_MAX;
}

static void opt_exit_unless(opt_builder_t *b, ir_condition_t condition, const ir_node_id_t *inputs, uint32_t count, opt_exit_kind_t kind) {
  ir_frame_state_id_t state = opt_eager_frame_state(b);
  ir_block_id_t off;
  ir_block_id_t keep = opt_branch_off(b, condition, inputs, count, false, &off);
  ir_block(b->graph, off)->is_cold = true;
  b->block = off;
  ir_op_t exit = ir_op(IR_OP_EXIT);
  exit.exit_kind = kind;
  ir_node(b->graph, opt_set_control(b, exit, NULL, 0))->frame_state = state;
  b->block = keep;
}

static opt_flow_t opt_build_iteration_init(opt_builder_t *b) {
  ir_node_id_t iterable = opt_tagged(b, opt_peek(b, 0));
  uint8_t seen = opt_iteration_seen(b, b->pc);
  if (!seen && opt_may_speculate(b, OPT_EXIT_NO_FEEDBACK)) return opt_end_with_exit(b, OPT_EXIT_NO_FEEDBACK);
  if (seen == 1u << SV_ITER_ARRAY && opt_may_speculate(b, OPT_EXIT_BAD_TYPE)) {
    opt_exit_unless(b, ir_cond(IR_COND_ITERABLE_ARRAY), &iterable, 1, OPT_EXIT_BAD_TYPE);
    opt_poke(b, 0, iterable);
    opt_push(b, opt_int32_constant(b, 0));
    opt_push(b, opt_constant(b, opt_iteration_tag(SV_ITER_ARRAY)));
    return OPT_FLOW_CONTINUE;
  }
  ir_node_id_t inputs[2] = {iterable, opt_constant(b, IR_VALUE_UNDEFINED)};
  ir_node_id_t call = opt_iteration_call(b, (uintptr_t)opt_rt_for_of, 0, inputs, 2, 5);
  ir_node_id_t outputs[3];
  for (uint32_t i = 0; i < 3; i++) outputs[i] = opt_call_output(b, call, 2 + i);
  opt_drop(b, 1);
  for (uint32_t i = 0; i < 3; i++) opt_push(b, outputs[i]);
  return OPT_FLOW_CONTINUE;
}

typedef struct {
  ir_block_id_t block;
  ir_node_id_t next;
  ir_node_id_t value;
  ir_node_id_t done;
  ir_node_id_t tag;
} opt_iteration_path_t;

typedef OPT_VEC(opt_iteration_path_t) opt_iteration_paths_t;

static void opt_iteration_path(opt_builder_t *b, opt_iteration_paths_t *paths, ir_node_id_t next, ir_node_id_t value, ir_node_id_t done, ir_node_id_t tag) {
  opt_iteration_path_t path = {.block = b->block, .next = next, .value = value, .done = done, .tag = tag};
  opt_vec_push(&b->arena, paths, path);
}

static ir_node_id_t opt_iteration_step_call(opt_builder_t *b, uintptr_t helper, uintptr_t extra, ir_node_id_t iter, ir_node_id_t next, ir_node_id_t tag) {
  ir_node_id_t inputs[3] = {iter, opt_tagged(b, next), tag};
  opt_poke(b, 0, opt_constant(b, opt_iteration_tag(SV_ITER_DONE)));
  ir_node_id_t call = opt_iteration_call(b, helper, extra, inputs, 3, 5);
  opt_poke(b, 0, tag);
  return call;
}

static void opt_iteration_known_done(opt_builder_t *b, opt_iteration_paths_t *paths, ir_node_id_t done, ir_node_id_t index, ir_node_id_t value, ir_node_id_t tag) {
  ir_condition_t is_true = ir_cond(IR_COND_TAGGED_EQUALS);
  is_true.equal = true;
  ir_block_id_t finished, more;
  opt_branch_both_ways(b, is_true, (ir_node_id_t[]){done, opt_constant(b, IR_VALUE_TRUE)}, 2, &finished, &more);
  b->block = finished;
  opt_iteration_path(b, paths, index, value, opt_constant(b, IR_VALUE_TRUE), opt_constant(b, opt_iteration_tag(SV_ITER_DONE)));
  b->block = more;
  ir_op_t add = ir_op(IR_OP_INT32_WRAPPING_BINARY);
  add.binary = IR_BINARY_ADD;
  ir_node_id_t after = opt_emit(b, add, (ir_node_id_t[]){index, opt_int32_constant(b, 1)}, 2, OPT_REPR_INT32);
  opt_iteration_path(b, paths, after, value, opt_constant(b, IR_VALUE_FALSE), tag);
}

static void opt_iteration_array(opt_builder_t *b, sv_op_t op, ir_node_id_t iter, ir_node_id_t next, ir_node_id_t tag, uintptr_t helper, uintptr_t extra, opt_iteration_paths_t *paths) {
  ir_node_id_t index = opt_int32(b, next);
  opt_frame_t before = opt_frame_clone(b, &b->frame);
  ir_block_id_t slow[2], in_bounds, past_end, hole;
  uint32_t count = 0;
  ir_node_id_t length;
  ir_node_id_t address = opt_elements_of(b, iter, IR_ELEMENTS_HOLEY, &length, slow, &count);
  opt_branch_both_ways(b, ir_cond(IR_COND_INDEX_IN_BOUNDS), (ir_node_id_t[]){index, length}, 2, &in_bounds, &past_end);
  b->block = in_bounds;
  ir_op_t load = ir_op(IR_OP_LOAD_ELEMENT_AT);
  load.elements = IR_ELEMENTS_HOLEY;
  ir_node_id_t value = opt_emit(b, load, (ir_node_id_t[]){address, index}, 2, OPT_REPR_TAGGED);
  ir_condition_t is_hole = ir_cond(IR_COND_TAGGED_EQUALS);
  is_hole.equal = true;
  opt_branch_off(b, is_hole, (ir_node_id_t[]){value, opt_constant(b, IR_VALUE_EMPTY)}, 2, true, &hole);
  slow[count++] = hole;
  ir_op_t add = ir_op(IR_OP_INT32_WRAPPING_BINARY);
  add.binary = IR_BINARY_ADD;
  ir_node_id_t after = opt_emit(b, add, (ir_node_id_t[]){index, opt_int32_constant(b, 1)}, 2, OPT_REPR_INT32);
  opt_iteration_path(b, paths, after, value, opt_constant(b, IR_VALUE_FALSE), tag);
  b->block = past_end;
  opt_iteration_path(b, paths, index, opt_constant(b, IR_VALUE_UNDEFINED), opt_constant(b, IR_VALUE_TRUE), opt_constant(b, opt_iteration_tag(SV_ITER_DONE)));
  opt_join_blocks(b, slow, count);
  ir_block(b->graph, b->block)->is_cold = true;
  b->frame = before;
  if (b->inline_call || opt_may_speculate(b, OPT_EXIT_SLOW_PATH)) {
    if (opt_end_with_exit(b, OPT_EXIT_SLOW_PATH) != OPT_FLOW_ENDED) opt_fail(b, "inline-slow-path");
    return;
  }
  ir_node_id_t call = opt_iteration_step_call(b, helper, extra, iter, index, tag);
  ir_node_id_t result = opt_call_output(b, call, 3);
  if (op == OP_ITER_NEXT) {
    opt_iteration_known_done(b, paths, opt_call_output(b, call, 4), index, result, tag);
    return;
  }
  opt_iteration_path(b, paths, opt_call_output(b, call, 1), result, IR_NONE, opt_call_output(b, call, 2));
}

static void opt_iteration_generic(opt_builder_t *b, sv_op_t op, ir_node_id_t iter, ir_node_id_t next, ir_node_id_t tag, uintptr_t helper, uintptr_t extra, opt_iteration_paths_t *paths) {
  ir_node_id_t call = opt_iteration_step_call(b, helper, extra, iter, next, tag);
  ir_node_id_t after = opt_call_output(b, call, 1), value = opt_call_output(b, call, 3);
  opt_iteration_path(b, paths, after, value, op == OP_ITER_NEXT ? opt_call_output(b, call, 4) : IR_NONE, opt_call_output(b, call, 2));
}

static ir_node_id_t opt_iteration_phi(opt_builder_t *b, uint32_t stack_index, const opt_iteration_path_t *items, uint32_t count, size_t field) {
  ir_node_id_t *inputs = opt_arena_new(&b->arena, ir_node_id_t, count + 1);
  bool same = true, int32 = true;
  for (uint32_t i = 0; i < count; i++) {
    inputs[i] = *(const ir_node_id_t *)((const char *)&items[i] + field);
    same &= inputs[i] == inputs[0];
    int32 &= ir_node(b->graph, inputs[i])->repr == OPT_REPR_INT32;
  }
  if (same) return inputs[0];
  if (int32) {
    ir_op_t op = ir_op(IR_OP_PHI);
    op.slot = opt_tracked_slot(b, opt_stack_index(b, stack_index));
    ir_node_id_t phi = ir_add_node(b->graph, op, inputs, count, OPT_REPR_INT32, b->pc);
    opt_vec_push(&b->graph->arena, &ir_block(b->graph, b->block)->phis, phi);
    return phi;
  }
  return opt_value_phi(b, stack_index, inputs, count);
}

static void opt_iteration_tag_inputs(opt_builder_t *b, opt_iteration_path_t *items, uint32_t count, size_t field) {
  bool int32 = true;
  for (uint32_t i = 0; i < count; i++) int32 &= ir_node(b->graph, *(ir_node_id_t *)((char *)&items[i] + field))->repr == OPT_REPR_INT32;
  if (int32) return;
  for (uint32_t i = 0; i < count; i++) {
    ir_node_id_t *value = (ir_node_id_t *)((char *)&items[i] + field);
    b->block = items[i].block;
    *value = opt_tagged(b, *value);
  }
}

static void opt_iteration_join(opt_builder_t *b, const opt_frame_t *base, opt_iteration_paths_t *paths, bool with_done) {
  opt_iteration_path_t *items = paths->items;
  uint32_t count = paths->len;
  opt_iteration_tag_inputs(b, items, count, offsetof(opt_iteration_path_t, next));
  opt_iteration_tag_inputs(b, items, count, offsetof(opt_iteration_path_t, value));
  if (with_done) opt_iteration_tag_inputs(b, items, count, offsetof(opt_iteration_path_t, done));
  opt_iteration_tag_inputs(b, items, count, offsetof(opt_iteration_path_t, tag));
  ir_block_id_t *ends = opt_arena_new(&b->arena, ir_block_id_t, count + 1);
  for (uint32_t i = 0; i < count; i++) ends[i] = items[i].block;
  opt_join_blocks(b, ends, count);
  opt_frame_t joined = opt_frame_clone(b, base);
  for (uint32_t t = 0; t < b->tracked; t++)
    if (opt_in_memory(b, t)) joined.slots[t] = (opt_slot_t){.node = IR_NONE};
  b->frame = joined;
  uint32_t top = b->frame.sp - 2;
  b->frame.slots[opt_stack_index(b, top)] = (opt_slot_t){.node = opt_iteration_phi(b, top, items, count, offsetof(opt_iteration_path_t, next))};
  b->frame.slots[opt_stack_index(b, top + 1)] = (opt_slot_t){.node = opt_iteration_phi(b, top + 1, items, count, offsetof(opt_iteration_path_t, tag))};
  ir_node_id_t value = opt_iteration_phi(b, b->frame.sp, items, count, offsetof(opt_iteration_path_t, value));
  opt_push(b, value);
  if (with_done) opt_push(b, opt_iteration_phi(b, b->frame.sp, items, count, offsetof(opt_iteration_path_t, done)));
}

static bool opt_iteration_branch(opt_builder_t *b, const opt_frame_t *base, const opt_iteration_paths_t *paths, uint32_t if_done, uint32_t if_more) {
  for (uint32_t i = 0; i < paths->len; i++) {
    const opt_iteration_path_t *path = &paths->items[i];
    b->block = path->block;
    b->frame = opt_frame_clone(b, base);
    for (uint32_t t = 0; t < b->tracked; t++)
      if (opt_in_memory(b, t)) b->frame.slots[t] = (opt_slot_t){.node = IR_NONE};
    b->frame.slots[opt_stack_index(b, b->frame.sp - 2)] = (opt_slot_t){.node = path->next};
    b->frame.slots[opt_stack_index(b, b->frame.sp - 1)] = (opt_slot_t){.node = path->tag};
    opt_push(b, path->value);
    uint64_t bits;
    if (ir_constant_value(b->graph, path->done, &bits)) {
      if (!opt_end_with_jump(b, bits == IR_VALUE_TRUE ? if_done : if_more)) return false;
      continue;
    }
    ir_condition_t is_true = ir_cond(IR_COND_TAGGED_EQUALS);
    is_true.equal = true;
    if (!opt_end_with_branch(b, is_true, (ir_node_id_t[]){path->done, opt_constant(b, IR_VALUE_TRUE)}, 2, if_done, if_more)) return false;
  }
  return true;
}

static opt_flow_t opt_build_iteration_step(opt_builder_t *b, const uint8_t *ip, sv_op_t op) {
  uint8_t hint = op == OP_ITER_NEXT ? ip[1] : 0;
  ir_node_id_t iter = opt_tagged(b, opt_peek(b, 2)), next = opt_peek(b, 1), tag = opt_tagged(b, opt_peek(b, 0));
  bool array_hint = hint == SV_ITER_GENERIC || hint == SV_ITER_ARRAY;
  uint32_t init = opt_iteration_init_of(b);
  uint8_t seen = init != UINT32_MAX ? opt_iteration_seen(b, init) : 0;
  bool known = opt_is_known_iteration_tag(b, tag);
  if (!known && array_hint && op != OP_DESTRUCTURE_REST && seen == 1u << SV_ITER_ARRAY && opt_may_speculate(b, OPT_EXIT_BAD_TYPE)) {
    ir_op_t check = ir_op(IR_OP_CHECK_VALUE);
    check.check_value.expected = opt_iteration_tag(SV_ITER_ARRAY);
    check.check_value.kind = OPT_EXIT_BAD_TYPE;
    opt_emit_checked(b, check, &tag, 1, OPT_REPR_NONE);
    tag = opt_constant(b, opt_iteration_tag(SV_ITER_ARRAY));
    opt_poke(b, 0, tag);
    known = true;
  }
  uintptr_t helper = op == OP_ITER_NEXT ? (uintptr_t)opt_rt_iter_next
    : op == OP_DESTRUCTURE_NEXT ? (uintptr_t)opt_rt_destructure_next : (uintptr_t)opt_rt_destructure_rest;
  bool array = array_hint && op != OP_DESTRUCTURE_REST && (known ? opt_is_iteration_tag(b, tag, SV_ITER_ARRAY) : (seen & 1u << SV_ITER_ARRAY) != 0);
  opt_bblock_t *bblock = &b->blocks[b->block_of_ins[b->ins]];
  bool fused = op == OP_ITER_NEXT && b->ins + 1 == bblock->last && bblock->succ_count == 2;
  if (fused) {
    sv_op_t jump = opt_op_at(b, b->ins_off[b->ins + 1]);
    fused = jump == OP_JMP_TRUE || jump == OP_JMP_TRUE8;
  }
  opt_frame_t base = opt_frame_clone(b, &b->frame);
  opt_iteration_paths_t paths = {0};
  if (array && !known) {
    ir_condition_t is_array = ir_cond(IR_COND_TAGGED_EQUALS);
    is_array.equal = true;
    ir_block_id_t on_array, other;
    opt_branch_both_ways(b, is_array, (ir_node_id_t[]){tag, opt_constant(b, opt_iteration_tag(SV_ITER_ARRAY))}, 2, &on_array, &other);
    b->block = other;
    b->frame = opt_frame_clone(b, &base);
    opt_iteration_generic(b, op, iter, next, tag, helper, hint, &paths);
    b->block = on_array;
    b->frame = opt_frame_clone(b, &base);
  }
  if (array) opt_iteration_array(b, op, iter, next, tag, helper, hint, &paths);
  else opt_iteration_generic(b, op, iter, next, tag, helper, hint, &paths);
  if (fused) {
    uint32_t if_done = bblock->succ[0], if_more = bblock->succ[1];
    return opt_iteration_branch(b, &base, &paths, if_done, if_more) ? OPT_FLOW_ENDED : OPT_FLOW_FAILED;
  }
  opt_iteration_join(b, &base, &paths, op == OP_ITER_NEXT);
  return OPT_FLOW_CONTINUE;
}

static opt_flow_t opt_build_iteration_close(opt_builder_t *b, const uint8_t *ip) {
  ir_node_id_t tag = opt_tagged(b, opt_peek(b, 0));
  bool generic = opt_iteration_tag_may_be(b, tag, SV_ITER_GENERIC), fast = false;
  for (uint8_t kind = SV_ITER_ARRAY; kind <= SV_ITER_STRING; kind++) fast |= opt_iteration_tag_may_be(b, tag, kind);
  ir_node_id_t inputs[3] = {opt_tagged(b, opt_peek(b, 2)), opt_tagged(b, opt_peek(b, 1)), tag};
  if (!generic && !fast) {
    opt_drop(b, 3);
    return OPT_FLOW_CONTINUE;
  }
  if (!fast || !ant_protector_intact(b->js, ANT_PROTECTOR_ITERATOR_RETURN)) {
    opt_iteration_call(b, (uintptr_t)opt_rt_iter_close, ip[1], inputs, 3, 3);
    opt_drop(b, 3);
    return OPT_FLOW_CONTINUE;
  }
  opt_frame_t base = opt_frame_clone(b, &b->frame);
  ir_block_id_t to_call[2], intact, broken;
  uint32_t count = 0;
  if (generic) {
    ir_condition_t is_generic = ir_cond(IR_COND_TAGGED_EQUALS);
    is_generic.equal = true;
    ir_block_id_t other;
    opt_branch_both_ways(b, is_generic, (ir_node_id_t[]){tag, opt_constant(b, opt_iteration_tag(SV_ITER_GENERIC))}, 2, &to_call[count++], &other);
    b->block = other;
  }
  opt_branch_both_ways(b, ir_cond(IR_COND_ITERATOR_RETURN_INTACT), &tag, 1, &intact, &broken);
  to_call[count++] = broken;
  opt_join_blocks(b, to_call, count);
  if (!generic) ir_block(b->graph, b->block)->is_cold = true;
  b->frame = opt_frame_clone(b, &base);
  opt_iteration_call(b, (uintptr_t)opt_rt_iter_close, ip[1], inputs, 3, 3);
  ir_block_id_t ends[2] = {intact, b->block};
  opt_join_blocks(b, ends, 2);
  b->frame = opt_frame_clone(b, &base);
  for (uint32_t t = 0; t < b->tracked; t++)
    if (opt_in_memory(b, t)) b->frame.slots[t] = (opt_slot_t){.node = IR_NONE};
  opt_drop(b, 3);
  return OPT_FLOW_CONTINUE;
}

static bool opt_returns_through_finally(const opt_builder_t *b) {
  return !b->inline_call && opt_state_has(b, b->pc, OP_TRY_PUSH_FINALLY);
}

static void opt_set_completion(opt_builder_t *b, uint32_t finally_off, uint32_t kind, ir_node_id_t value) {
  int32_t index = opt_finally_index(b, (int32_t)finally_off);
  if (index < 0) return;
  b->frame.slots[b->tracked + 1 + 2 * index] = (opt_slot_t){.node = opt_constant(b, opt_completion_bits(kind))};
  b->frame.slots[b->tracked + 2 + 2 * index] = (opt_slot_t){.node = value};
}

static opt_flow_t opt_jump_to_finally(opt_builder_t *b, uint32_t push, uint32_t kind, ir_node_id_t value) {
  int32_t target = opt_finally_of(b, push);
  if (target < 0 || b->block_at[target] < 0) {
    opt_fail(b, "finally-target");
    return OPT_FLOW_FAILED;
  }
  opt_set_completion(b, (uint32_t)target, kind, value);
  b->frame.sp = (uint32_t)b->depth[push];
  return opt_end_with_jump(b, (uint32_t)b->block_at[target]) ? OPT_FLOW_ENDED : OPT_FLOW_FAILED;
}

static opt_flow_t opt_return_through_finally(opt_builder_t *b, ir_node_id_t value, int32_t state) {
  for (int32_t s = state; s >= 0; s = opt_try_parent(b, s)) {
    sv_op_t op = opt_op_at(b, (uint32_t)s);
    if (op == OP_FINALLY) opt_set_completion(b, (uint32_t)s, 0, opt_constant(b, IR_VALUE_UNDEFINED));
    if (op == OP_TRY_PUSH_FINALLY) return opt_jump_to_finally(b, (uint32_t)s, OPT_PENDING_RETURN, value);
  }
  return opt_build_return(b, value);
}

static opt_flow_t opt_build_unwind_jump(opt_builder_t *b) {
  int32_t state = opt_try_state(b, b->pc);
  uint32_t cont = b->pc + sv_op_size[OP_UNWIND_JMP] + (uint32_t)sv_get_i32(b->func->code + b->pc + 1);
  return opt_jump_to_finally(b, (uint32_t)state, OPT_PENDING_JUMP + cont, opt_constant(b, IR_VALUE_UNDEFINED));
}

static uint32_t opt_completion_kinds(const opt_builder_t *b, const opt_finally_t *fin, ir_node_id_t kind, uint32_t *kinds) {
  uint32_t count = 0;
  const ir_node_t *node = ir_node(b->graph, kind);
  bool phi = node->op.code == IR_OP_PHI, exact = phi || node->op.code == IR_OP_CONSTANT;
  uint32_t total = phi ? node->inputs.len : 1;
  for (uint32_t i = 0; exact && i < total; i++) {
    uint64_t bits;
    ir_node_id_t input = phi ? node->inputs.items[i] : kind;
    if (!ir_constant_value(b->graph, input, &bits) || ir_node(b->graph, input)->repr != OPT_REPR_TAGGED) {
      exact = false;
      break;
    }
    ant_value_t constant = (ant_value_t)bits;
    if (vtype(constant) != kTypeNumber) {
      exact = false;
      break;
    }
    uint32_t value = (uint32_t)js_getnum(constant), k = 0;
    if (!value) continue;
    while (k < count && kinds[k] != value) k++;
    if (k == count && count < OPT_FINALLY_CONTS + 1) kinds[count++] = value;
  }
  if (exact) return count;
  count = 0;
  if (fin->has_return) kinds[count++] = OPT_PENDING_RETURN;
  for (uint32_t c = 0; c < fin->cont_count; c++) kinds[count++] = OPT_PENDING_JUMP + fin->conts[c];
  return count;
}

static opt_flow_t opt_build_finally_ret(opt_builder_t *b) {
  int32_t state = opt_try_state(b, b->pc), index = opt_finally_index(b, state);
  uint32_t end = b->blocks[b->block_of_ins[b->ins]].succ[0];
  if (index < 0) return opt_end_with_jump(b, end) ? OPT_FLOW_ENDED : OPT_FLOW_FAILED;
  const opt_finally_t *fin = &b->finallies[index];
  ir_node_id_t kind = b->frame.slots[b->tracked + 1 + 2 * index].node, value = b->frame.slots[b->tracked + 2 + 2 * index].node;
  opt_set_completion(b, (uint32_t)state, 0, opt_constant(b, IR_VALUE_UNDEFINED));
  uint32_t kinds[OPT_FINALLY_CONTS + 1];
  uint32_t count = opt_completion_kinds(b, fin, kind, kinds);
  if (!count) return opt_end_with_jump(b, end) ? OPT_FLOW_ENDED : OPT_FLOW_FAILED;
  opt_frame_t base = opt_frame_clone(b, &b->frame);
  ir_node_id_t tagged_kind = opt_tagged(b, kind);
  for (uint32_t i = 0; i < count; i++) {
    ir_condition_t equal = ir_cond(IR_COND_TAGGED_EQUALS);
    equal.equal = true;
    ir_block_id_t taken, other;
    opt_branch_both_ways(b, equal, (ir_node_id_t[]){tagged_kind, opt_constant(b, opt_completion_bits(kinds[i]))}, 2, &taken, &other);
    b->block = taken;
    b->frame = opt_frame_clone(b, &base);
    opt_flow_t flow;
    if (kinds[i] == OPT_PENDING_RETURN) flow = opt_return_through_finally(b, opt_tagged(b, value), opt_try_parent(b, state));
    else {
      uint32_t cont = kinds[i] - OPT_PENDING_JUMP;
      int32_t target = cont < (uint32_t)b->func->code_len ? b->block_at[cont] : -1;
      if (target < 0 || b->blocks[target].depth != b->frame.sp) {
        opt_fail(b, "finally-target");
        return OPT_FLOW_FAILED;
      }
      flow = opt_end_with_jump(b, (uint32_t)target) ? OPT_FLOW_ENDED : OPT_FLOW_FAILED;
    }
    if (flow != OPT_FLOW_ENDED) return OPT_FLOW_FAILED;
    b->block = other;
  }
  b->frame = opt_frame_clone(b, &base);
  return opt_end_with_jump(b, end) ? OPT_FLOW_ENDED : OPT_FLOW_FAILED;
}

static opt_flow_t opt_build_unwind(opt_builder_t *b) {
  ir_frame_state_id_t state = opt_eager_frame_state(b);
  ir_op_t op = ir_op(IR_OP_EXIT);
  op.exit_kind = OPT_EXIT_UNWIND;
  ir_node(b->graph, opt_set_control(b, op, NULL, 0))->frame_state = state;
  return OPT_FLOW_ENDED;
}

static opt_flow_t opt_build_instruction(opt_builder_t *b) {
  const uint8_t *ip = b->func->code + b->pc;
  sv_op_t op = opt_op_at(b, b->pc);
  if (sv_op_flags[op] & SV_OPF_MARKER) return OPT_FLOW_CONTINUE;

  switch (op) {
    case OP_CONST: case OP_CONST_I8: case OP_CONST8:
    case OP_UNDEF: case OP_NULL: case OP_TRUE: case OP_FALSE: case OP_GLOBAL:
      return opt_build_constant_op(b, ip, op);
    case OP_THIS:
      return opt_build_this(b);
    case OP_POP: case OP_DUP: case OP_DUP2: case OP_SWAP: case OP_NIP: case OP_INSERT2: case OP_INSERT3:
    case OP_SWAP_UNDER: case OP_ROT3L: case OP_ROT3R: case OP_ROT4_UNDER:
      return opt_build_stack_shuffle(b, op);

    case OP_GET_ARG: case OP_PUT_ARG: case OP_SET_ARG: case OP_GET_SLOT_RAW:
      return opt_build_argument_slot(b, ip, op);
    case OP_GET_LOCAL: case OP_GET_LOCAL8: case OP_PUT_LOCAL: case OP_PUT_LOCAL8:
    case OP_SET_LOCAL: case OP_SET_LOCAL8: case OP_SET_LOCAL_UNDEF:
      return opt_build_local_slot(b, ip, op);
    case OP_GET_LOCAL_CHK: case OP_PUT_LOCAL_CHK:
      return opt_build_checked_local(b, ip, op);
    case OP_INC_LOCAL: case OP_DEC_LOCAL: case OP_ADD_LOCAL:
      return opt_build_local_update(b, ip, op);
    case OP_STR_APPEND_LOCAL: case OP_STR_ALC_SNAPSHOT:
      return opt_build_string_append(b, ip, op);
    case OP_GET_UPVAL: case OP_GET_UPVAL_VAL: case OP_PUT_UPVAL: case OP_SET_UPVAL:
      return opt_build_upvalue(b, ip, op);
    case OP_CLOSE_UPVAL:
      return opt_build_close_upvalues(b, ip);
    case OP_GET_GLOBAL: case OP_GET_GLOBAL_UNDEF:
      return opt_build_get_global(b, ip);
    case OP_SPECIAL_OBJ:
      return opt_build_special_object(b, ip);

    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
    case OP_ADD_NUM: case OP_SUB_NUM: case OP_MUL_NUM: case OP_DIV_NUM:
      return opt_build_arithmetic(b, op);
    case OP_INC: case OP_DEC: case OP_POST_INC: case OP_POST_DEC:
      return opt_build_increment(b, op);
    case OP_NEG:
      return opt_build_negate(b);
    case OP_MOD:
      return opt_build_remainder(b);
    case OP_EXP:
      return opt_build_exponentiation(b);
    case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: case OP_USHR: case OP_BNOT:
      return opt_build_bitwise(b, op);

    case OP_LT: case OP_LE: case OP_GT: case OP_GE:
      return opt_build_relational(b, op);
    case OP_SEQ: case OP_SNE:
      return opt_build_equality(b, op == OP_SEQ);
    case OP_EQ: case OP_NE:
      return opt_build_loose_equality(b, op == OP_EQ);
    case OP_IS_UNDEF: case OP_IS_NULL:
      return opt_build_is_constant(b, op);
    case OP_IS_NULLISH: case OP_IS_UNDEF_OR_NULL:
      opt_poke(b, 0, opt_box_bool(b, opt_is_nullish(b, opt_peek(b, 0), true)));
      return OPT_FLOW_CONTINUE;
    case OP_IS_PRIMITIVE_TYPE:
      return opt_build_typeof_is(b, ip);
    case OP_TYPEOF:
      return opt_build_typeof(b);
    case OP_IN:
      return opt_build_in(b);
    case OP_IN_FIELD:
      return opt_build_in_field(b);
    case OP_FOR_IN:
      return opt_build_for_in(b);
    case OP_NOT:
      opt_poke(b, 0, opt_box_bool(b, opt_truthiness(b, opt_peek(b, 0), true)));
      return OPT_FLOW_CONTINUE;

    case OP_JMP: case OP_JMP8: case OP_JMP_FALSE: case OP_JMP_FALSE8: case OP_JMP_TRUE: case OP_JMP_TRUE8:
    case OP_JMP_FALSE_PEEK: case OP_JMP_TRUE_PEEK:
      return opt_build_jump(b, op);
    case OP_JMP_NOT_NULLISH:
      return opt_build_jump_not_nullish(b);
    case OP_RETURN:
      if (opt_returns_through_finally(b))
        return b->finally_count ? opt_return_through_finally(b, opt_tagged(b, opt_peek(b, 0)), opt_try_state(b, b->pc)) : opt_build_unwind(b);
      return opt_build_return(b, opt_tagged(b, opt_peek(b, 0)));
    case OP_RETURN_UNDEF:
      if (opt_returns_through_finally(b))
        return b->finally_count ? opt_return_through_finally(b, opt_constant(b, IR_VALUE_UNDEFINED), opt_try_state(b, b->pc)) : opt_build_unwind(b);
      return opt_build_return(b, opt_constant(b, IR_VALUE_UNDEFINED));
    case OP_THROW:
      return opt_build_throw(b);
    case OP_TRY_PUSH: case OP_TRY_POP: case OP_TRY_PUSH_FINALLY: case OP_FINALLY:
      return OPT_FLOW_CONTINUE;
    case OP_FINALLY_DISCARD:
      if (b->finally_count) opt_set_completion(b, (uint32_t)opt_try_state(b, b->pc), 0, opt_constant(b, IR_VALUE_UNDEFINED));
      return OPT_FLOW_CONTINUE;
    case OP_FINALLY_RET:
      if (b->finally_count) return opt_build_finally_ret(b);
      return opt_end_with_jump(b, b->blocks[b->block_of_ins[b->ins]].succ[0]) ? OPT_FLOW_ENDED : OPT_FLOW_FAILED;
    case OP_UNWIND_JMP:
      return b->finally_count ? opt_build_unwind_jump(b) : opt_build_unwind(b);
    case OP_FOR_OF: case OP_DESTRUCTURE_INIT:
      return opt_build_iteration_init(b);
    case OP_ITER_NEXT: case OP_DESTRUCTURE_NEXT: case OP_DESTRUCTURE_REST:
      return opt_build_iteration_step(b, ip, op);
    case OP_ITER_CLOSE: case OP_DESTRUCTURE_CLOSE:
      return opt_build_iteration_close(b, ip);
    case OP_CHECK_CTOR:
      return b->inline_call && b->inline_call->construct ? OPT_FLOW_CONTINUE : OPT_FLOW_GENERIC;

    case OP_CALL: case OP_CALL_METHOD: case OP_TAIL_CALL: case OP_TAIL_CALL_METHOD: case OP_NEW:
      return opt_build_call(b, ip, op);
    case OP_CALL_CALL: case OP_CALL_CALL_SLOT:
      return opt_build_call_call(b, ip, op);
    case OP_INSTANCEOF: {
      opt_flow_t flow = opt_build_instanceof(b);
      return flow == OPT_FLOW_GENERIC ? opt_build_call_helper(b, ip, op) : flow;
    }
    case OP_CALL_CHAR_CODE_AT:
      return opt_build_char_code_at(b, ip);
    case OP_CALL_ARRAY_INCLUDES: case OP_CALL_STRING_INTRINSIC: case OP_CALL_STABLE_BUILTIN:
    case OP_RE_EXEC_TRUTHY: case OP_RE_EXEC_DISCARD:
      return opt_build_call_helper(b, ip, op);

    case OP_GET_FIELD: case OP_GET_FIELD2: case OP_PUT_FIELD:
      return opt_build_field(b, ip, op);
    case OP_GET_ELEM:
      if (opt_is_virtual_arguments(b, opt_peek(b, 1))) return opt_build_arguments_index(b);
      return opt_build_element(b, op);
    case OP_PUT_ELEM:
      return opt_build_element(b, op);
    case OP_GET_LENGTH:
      if (opt_is_virtual_arguments(b, opt_peek(b, 0))) return opt_build_arguments_length(b);
      return opt_build_length(b);

    case OP_OBJECT:
      return opt_build_object(b);
    case OP_DEFINE_SLOT:
      return opt_build_define_slot(b, ip);
    case OP_SET_PROTO:
      return opt_build_set_proto(b);
    case OP_ARRAY:
      return opt_build_array(b, ip);
    case OP_CLOSURE:
      return opt_build_closure(b, ip);
    case OP_SET_NAME:
      return ir_node_vec_contains(&b->ctx->named_closures, opt_peek(b, 0)) ? OPT_FLOW_CONTINUE : OPT_FLOW_GENERIC;
    case OP_REGEXP:
      return opt_build_regexp(b);
    case OP_VOID:
      opt_poke(b, 0, opt_constant(b, IR_VALUE_UNDEFINED));
      return OPT_FLOW_CONTINUE;
    case OP_TO_STRING: case OP_TO_STRING_DEFER_NUMBER:
      return opt_build_to_string(b, op);

    default:
      return opt_op_native(op) ? opt_build_bad_operand(b) : OPT_FLOW_GENERIC;
  }
}

static uint32_t opt_generic_slot(const opt_builder_t *b, const uint8_t *ip, sv_op_t op, bool *writes, bool *all_params) {
  bool use, def;
  *all_params = op == OP_SPECIAL_OBJ && ip[1] == SV_SPECIAL_ARGUMENTS;
  uint32_t tracked = opt_access(b, (uint32_t)(ip - b->func->code), &use, &def);
  *writes = def;
  return tracked < b->params + b->locals ? tracked : IR_NONE;
}

static opt_flow_t opt_build_generic(opt_builder_t *b) {
  sv_func_t *func = b->func;
  const uint8_t *ip = func->code + b->pc;
  sv_op_t op = opt_op_at(b, b->pc);
  int pops, pushes;
  if (!opt_generic_supported(op) || b->inline_call) {
    opt_fail(b, b->inline_call ? "inline-generic" : "op");
    return OPT_FLOW_FAILED;
  }
  if (!sv_op_stack_effect(func, ip, &pops, &pushes) || (uint32_t)pops > b->frame.sp
    || b->frame.sp - (uint32_t)pops + (uint32_t)pushes > b->stack || b->frame.sp > UINT16_MAX) {
    opt_fail(b, "generic-stack");
    return OPT_FLOW_FAILED;
  }
  bool terminal = sv_op_flags[op] & SV_OPF_TERMINAL;
  if (!terminal && (b->ins + 1 >= b->ins_count || b->ins_off[b->ins + 1] != b->pc + sv_op_size[op])) {
    opt_fail(b, "generic-next");
    return OPT_FLOW_FAILED;
  }
  ir_frame_state_id_t state = terminal ? opt_eager_frame_state(b) : IR_NONE;
  for (uint32_t k = 0; k < b->frame.sp; k++) opt_sync_tracked(b, opt_stack_index(b, k));
  bool writes, all_params;
  uint32_t slot = opt_generic_slot(b, ip, op, &writes, &all_params);
  if (slot != IR_NONE) opt_sync_tracked(b, slot);
  for (uint32_t p = 0; all_params && p < b->params; p++) opt_sync_tracked(b, p);

  ir_op_t generic = ir_op(IR_OP_GENERIC);
  generic.slow.opcode = (uint16_t)op;
  generic.slow.depth = (uint16_t)b->frame.sp;
  generic.slow.func = func;
  generic.slow.bc_off = b->pc;
  generic.slow.in_loop = !b->slow_path && b->in_loop[b->block_of_ins[b->ins]];
  ir_node_id_t node = opt_emit(b, generic, NULL, 0, OPT_REPR_NONE);
  b->graph->uses_frame = true;

  opt_bitset_for_each(&b->memory, t) b->frame.slots[t] = (opt_slot_t){.node = IR_NONE};
  if (slot != IR_NONE && writes) b->frame.slots[slot] = (opt_slot_t){.node = IR_NONE};
  if (terminal) {
    ir_node(b->graph, node)->frame_state = state;
    opt_mark_catch(b, node);
    opt_set_control(b, ir_op(IR_OP_UNREACHABLE), NULL, 0);
    return OPT_FLOW_ENDED;
  }
  opt_drop(b, (uint32_t)pops);
  for (int i = 0; i < pushes; i++) opt_push(b, IR_NONE);
  uint32_t ins = b->ins, pc = b->pc;
  b->ins = ins + 1;
  b->pc = b->ins_off[ins + 1];
  ir_node(b->graph, node)->frame_state = opt_new_frame_state(b);
  b->ins = ins;
  b->pc = pc;
  opt_mark_catch(b, node);
  for (uint32_t k = b->frame.sp - (uint32_t)pushes; k < b->frame.sp; k++) {
    ir_op_t load = ir_op(IR_OP_LOAD_SLOT);
    load.slot = opt_stack_slot(k);
    b->frame.slots[opt_stack_index(b, k)] = (opt_slot_t){.node = opt_emit(b, load, NULL, 0, OPT_REPR_TAGGED), .in_sync = true};
  }
  return OPT_FLOW_CONTINUE;
}

static bool opt_build_bytecode_block(opt_builder_t *b, uint32_t bi) {
  opt_bblock_t *bblock = &b->blocks[bi];
  ir_block_t *data = ir_block(b->graph, b->block);
  bool is_merge = data->predecessors.len > 1 || data->is_loop_header;
  for (uint32_t i = bblock->first; i <= bblock->last; i++) {
    b->ins = i;
    b->pc = b->ins_off[i];
    b->eager = IR_NONE;
    if ((uint32_t)b->depth[b->pc] != b->frame.sp) return opt_fail(b, "depth");
    if (i == bblock->first && is_merge) ir_block(b->graph, b->block)->start_frame_state = opt_eager_frame_state(b);
    opt_flow_t flow = opt_build_instruction(b);
    if (flow == OPT_FLOW_GENERIC) flow = opt_build_generic(b);
    if (flow == OPT_FLOW_FAILED) return false;
    if (flow == OPT_FLOW_ENDED) return true;
  }
  return opt_end_with_jump(b, bblock->succ[bblock->succ_count - 1]);
}

static bool opt_build_entry(opt_builder_t *b, uint32_t target, const opt_bitset_t *boxes) {
  ir_node_id_t undefined = opt_constant(b, IR_VALUE_UNDEFINED);
  opt_bblock_t *bblock = &b->blocks[target];
  opt_start_new_block(b, IR_NONE);
  b->ins = bblock->first;
  b->pc = b->ins_off[bblock->first];
  b->frame = opt_frame_new(b);
  b->frame.sp = bblock->depth;
  b->building_entry = true;
  const opt_bitset_t *live = &b->live_in[bblock->first];
  opt_bitset_for_each(boxes, t) {
    ir_node_id_t box = opt_runtime_call(b, (uintptr_t)opt_rt_osr_box, t, (ir_node_id_t[]){undefined, undefined}, 2, false, false);
    ir_node(b->graph, box)->repr = OPT_REPR_POINTER;
    b->frame.slots[t] = (opt_slot_t){.node = box, .boxed = true};
  }
  for (uint32_t j = 0; j < b->locals; j++) {
    if (opt_in_memory(b, b->params + j) || b->frame.slots[b->params + j].boxed) continue;
    if (opt_bitset_contains(live, b->params + j)) opt_read_tracked(b, b->params + j);
    else b->frame.slots[b->params + j] = (opt_slot_t){.node = undefined};
  }
  for (uint32_t k = 0; k < bblock->depth; k++) opt_read_tracked(b, opt_stack_index(b, k));
  for (uint32_t t = b->params; t < b->tracked; t++)
    if (!opt_in_memory(b, t)) b->frame.slots[t].in_sync = false;
  if (b->uses_this) opt_root_this(b);
  b->building_entry = false;
  ir_osr_entry_t entry = {.bc_off = b->pc, .block = b->block};
  if (!opt_end_with_jump(b, target)) return false;
  opt_vec_push(&b->graph->arena, &b->graph->osr_entries, entry);
  return true;
}

static bool opt_build_osr_entries(opt_builder_t *b) {
  for (uint32_t l = 0; l < b->loop_count; l++) {
    uint32_t header = b->loops[l].header;
    if (opt_state_has(b, b->ins_off[b->blocks[header].first], OP_FINALLY)) continue;
    if (!opt_build_entry(b, header, &b->loops[l].entry_boxes)) return false;
  }
  return true;
}

static bool opt_has_entry_at(const opt_builder_t *b, uint32_t bc_off) {
  for (uint32_t i = 0; i < b->graph->osr_entries.len; i++)
    if (b->graph->osr_entries.items[i].bc_off == bc_off) return true;
  return false;
}

static bool opt_build_catch_entries(opt_builder_t *b) {
  uint8_t *feedback = sv_func_type_feedback(b->func);
  ir_graph_t *graph = b->graph;
  for (uint32_t h = 0; feedback && h < graph->handlers.len; h++) {
    ir_handler_t *handler = &graph->handlers.items[h];
    uint32_t resume = handler->catch_off + sv_op_size[OP_CATCH];
    int32_t catch_block = b->block_at[handler->catch_off], target = resume < (uint32_t)b->func->code_len ? b->block_at[resume] : -1;
    if (handler->finally || catch_block < 0 || target < 0 || b->blocks[catch_block].rpo < 0) continue;
    if (b->ctx->no_catch_entries || opt_state_has(b, handler->catch_off, OP_FINALLY) || opt_has_entry_at(b, resume)) continue;
    if (!(feedback[handler->catch_off] & SV_TFB_CLASS_MASK)) {
      handler->learnable = true;
      continue;
    }
    b->ctx->built_catch_entries = true;
    if (!opt_build_entry(b, (uint32_t)target, &b->boxable)) return false;
    handler->has_entry = true;
    graph->uses_frame = true;
  }
  return true;
}

static void opt_decide_virtual_arguments(opt_builder_t *b) {
  sv_func_t *func = b->func;
  if (!func->is_strict && b->params) return;
  for (uint32_t i = 0; i < b->ctx->no_virtual_arguments.len; i++)
    if (b->ctx->no_virtual_arguments.items[i] == func) return;
  bool uses = false;
  for (uint32_t i = 0; i < b->ins_count; i++) {
    uint32_t off = b->ins_off[i];
    sv_op_t op = opt_op_at(b, off);
    if (op == OP_PUT_ARG || op == OP_SET_ARG) return;
    uses |= op == OP_SPECIAL_OBJ && func->code[off + 1] == SV_SPECIAL_ARGUMENTS;
  }
  b->virtual_arguments = uses;
}

static bool opt_analyze(opt_builder_t *b) {
  sv_func_t *func = b->func;
  b->tracked = b->params + b->locals + b->stack;
  b->layout = opt_frame_layout_of(func);
  if (func->is_generator || func->is_async || func->has_await || func->is_derived_ctor || func->has_dynamic_eval ||
      func->is_tla || func->is_eval || func->code_len <= 0) return opt_fail(b, "function-kind");
  int *depth = sv_func_stack_depth_map(func);
  if (!depth) return opt_fail(b, "depth-map");
  b->depth = opt_arena_dup(&b->arena, depth, (size_t)func->code_len * sizeof *depth);
  free(depth);
  if (!opt_scan_try(b) || !opt_scan_finally(b) || !opt_scan(b) || !opt_scan_memory(b)) return false;
  opt_record_handlers(b);
  opt_decide_virtual_arguments(b);
  opt_compute_rpo(b);
  opt_find_loops(b);
  opt_compute_liveness(b);
  return true;
}

static bool opt_graph_uses_virtual_arguments(const ir_graph_t *graph) {
  for (ir_node_id_t n = 0; n < ir_node_count(graph); n++) {
    const ir_node_t *node = ir_node(graph, n);
    for (uint32_t i = 0; i < node->inputs.len; i++) {
      uint64_t bits;
      if (ir_constant_value(graph, node->inputs.items[i], &bits) && ir_value_virtual_arguments_kind(bits) >= 0) return true;
    }
  }
  return false;
}

static bool opt_build_attempt(ant_t *js, sv_func_t *func, opt_build_ctx_t *ctx, ir_graph_t *graph, opt_build_failure_t *failure) {
  opt_builder_t builder = {
    .js = js,
    .func = func,
    .graph = graph,
    .ctx = ctx,
    .failure = failure,
    .params = func->param_count,
    .locals = (uint32_t)func->max_locals,
    .stack = (uint32_t)(func->max_stack > 0 ? func->max_stack : 0),
    .eager = IR_NONE,
    .tail_header = IR_NONE,
    .arguments_marker = IR_NONE,
  };
  opt_builder_t *b = &builder;
  b->exit_sites = ctx->exit_sites_of ? ctx->exit_sites_of(js, func, &b->exit_site_count) : NULL;
  opt_arena_init(&b->arena);
  opt_map_init(&ctx->constants, &b->arena, 2);
  graph->func = func;
  for (uint32_t i = 0; i < b->exit_site_count; i++) opt_vec_push(&graph->arena, &graph->exit_sites, b->exit_sites[i]);

  bool ok = opt_analyze(b);
  for (uint32_t i = 0; ok && !func->is_arrow && i < b->ins_count; i++) b->uses_this |= opt_op_at(b, b->ins_off[i]) == OP_THIS;
  if (ok) {
    opt_start_new_block(b, IR_NONE);
    b->frame = opt_frame_new(b);
    ir_node_id_t undefined = opt_constant(b, IR_VALUE_UNDEFINED);
    for (uint32_t j = 0; j < b->locals; j++)
      if (!opt_in_memory(b, b->params + j)) b->frame.slots[b->params + j] = (opt_slot_t){.node = undefined};
    b->ins = 0;
    b->pc = 0;
    if (b->uses_this) opt_root_this(b);
    if (opt_self_tail_loop_ok(b)) opt_build_tail_header(b);
    ok = opt_end_with_jump(b, 0) && (b->virtual_arguments || (opt_build_osr_entries(b) && opt_build_catch_entries(b)));
  }
  for (uint32_t r = 0; ok && r < b->rpo_count; r++) {
    uint32_t bi = b->rpo[r];
    if (opt_start_bytecode_block(b, bi)) ok = opt_build_bytecode_block(b, bi);
    ok &= !b->failed;
  }
  ir_block_vec_t *order = &ctx->order;
  if (ok) {
    for (uint32_t i = 0; i < order->len; i++) {
      ir_block_t *block = ir_block(graph, order->items[i]);
      if (block->control == IR_NONE) ok = opt_fail(b, "unfinished-block");
      for (uint32_t p = 0; ok && p < block->phis.len; p++)
        if (ir_node(graph, block->phis.items[p])->inputs.len != block->predecessors.len) ok = opt_fail(b, "phi-inputs");
    }
  }
  if (ok && order->len != ir_block_count(graph)) ok = opt_fail(b, "unstarted-block");
  if (ok && ctx->virtual_arguments.len && opt_graph_uses_virtual_arguments(graph)) {
    for (uint32_t i = 0; i < ctx->virtual_arguments.len; i++) opt_vec_push(&graph->arena, &ctx->no_virtual_arguments, ctx->virtual_arguments.items[i]);
    ctx->retry = true;
    ok = opt_fail(b, "virtual-arguments-escape");
  }
  if (ok && graph->uses_frame) {
    ir_node_id_t initialize = ir_add_node(graph, ir_op(IR_OP_INITIALIZE_FRAME), NULL, 0, OPT_REPR_NONE, 0);
    opt_vec_insert(&graph->arena, &ir_block(graph, order->items[0])->body, 0, initialize);
  }
  if (ok) ir_reorder_blocks(graph, order->items, order->len);
  opt_arena_free(&b->arena);
  return ok;
}

bool opt_build_graph(
  ant_t *js, sv_func_t *func, sv_closure_t *closure, opt_build_exit_sites_fn exit_sites_of,
  ir_graph_t *graph, ir_frame_layout_t *layout, opt_build_failure_t *failure, opt_build_stats_t *stats
) {
  *layout = opt_frame_layout_of(func);
  sv_func_t *no_inline[16], *no_virtual[16];
  uint32_t no_inline_count = 0, no_virtual_count = 0;
  bool no_catch_entries = false, no_native_finally = false;
  for (uint32_t attempt = 0;; attempt++) {
    opt_build_ctx_t ctx = {.exit_sites_of = exit_sites_of, .closure = closure, .no_catch_entries = no_catch_entries, .no_native_finally = no_native_finally};
    for (uint32_t i = 0; i < no_inline_count; i++) opt_vec_push(&graph->arena, &ctx.no_inline, no_inline[i]);
    for (uint32_t i = 0; i < no_virtual_count; i++) opt_vec_push(&graph->arena, &ctx.no_virtual_arguments, no_virtual[i]);
    *failure = (opt_build_failure_t){0};
    bool ok = opt_build_attempt(js, func, &ctx, graph, failure);
    if (stats) *stats = (opt_build_stats_t){ctx.inlined_calls};
    if (!ok && !ctx.retry && ctx.built_catch_entries && attempt < 8) {
      ctx.retry = true;
      no_catch_entries = true;
    } else if (!ok && !ctx.retry && ctx.used_native_finally && attempt < 8) {
      ctx.retry = true;
      no_native_finally = true;
    }
    if (ok || !ctx.retry || attempt >= 8) return ok;
    for (uint32_t i = no_inline_count; i < ctx.no_inline.len && no_inline_count < 16; i++) no_inline[no_inline_count++] = ctx.no_inline.items[i];
    for (uint32_t i = no_virtual_count; i < ctx.no_virtual_arguments.len && no_virtual_count < 16; i++) no_virtual[no_virtual_count++] = ctx.no_virtual_arguments.items[i];
    opt_build_release(js, graph);
    ir_graph_free(graph);
    ir_graph_init(graph);
  }
}
