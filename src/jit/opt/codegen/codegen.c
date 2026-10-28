// TODO: split into multiple module files

#include "codegen.h"
#include "../asm/disasm.h"
#include "../asm/masm.h"
#include "../build/build.h"
#include "../hash.h"
#include "../ir/ir_value.h"
#include "../regalloc/regalloc.h"

#include "cage.h"
#include "gc.h"
#include "gc/strings.h"
#include "gc/objects.h"
#include "pool.h"
#include "object.h"
#include "shapes.h"
#include "silver/engine.h"
#include "silver/feedback.h"
#include "protectors.h"
#include "modules/math.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static constexpr uint64_t OPT_OBJECT_TAG = (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)kTypeObject;
static constexpr uint64_t OPT_NULL_TAG = (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)kTypeNull;
static constexpr uint64_t OPT_UNDEFINED_TAG = (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)kTypeUndefined;
static constexpr uint64_t OPT_ARRAY_TAG = (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)kTypeArray;
static constexpr uint64_t OPT_FUNCTION_TAG = (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)kTypeFunction;
static constexpr uint64_t OPT_ERROR_TAG = (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)kTypeError;
static constexpr int64_t OPT_STRING_TAG_SIGNED = (int64_t)(((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)kTypeString) | ~(UINT64_MAX >> NANBOX_TYPE_SHIFT));
static constexpr int64_t OPT_FUNCTION_TAG_SIGNED = (int64_t)(OPT_FUNCTION_TAG | ~(UINT64_MAX >> NANBOX_TYPE_SHIFT));
static constexpr int64_t OPT_ERROR_TAG_SIGNED = (int64_t)(OPT_ERROR_TAG | ~(UINT64_MAX >> NANBOX_TYPE_SHIFT));
static constexpr uint32_t OPT_OUTGOING_BYTES = 16;
static constexpr uint32_t OPT_IDENTITY_TYPES =
  (1u << kTypeObject) | (1u << kTypeArray) | (1u << kTypeFunction) | (1u << kTypePromise) | (1u << kTypeGenerator) |
  (1u << kTypeUndefined) | (1u << kTypeNull) | (1u << kTypeBool) | (1u << kTypeSymbol);

#if defined(__aarch64__)
static constexpr uint32_t OPT_CG_GPRS = 31;
static constexpr uint32_t OPT_CG_FPRS = 32;
#else
static constexpr uint32_t OPT_CG_GPRS = 16;
static constexpr uint32_t OPT_CG_FPRS = 16;
#endif

typedef struct {
  uint32_t offset;
  ir_node_id_t node;
} opt_cg_mark_t;

typedef struct {
  asm_label_t label;
  uint32_t index;
  bool catches;
} opt_cg_exit_stub_t;

typedef struct {
  bool constant;
  asm_gpr_t reg;
  uint64_t bits;
} opt_cg_arg_t;

typedef struct {
  asm_label_t label;
  asm_label_t resume;
  uint64_t helper;
  uint32_t argc;
  opt_cg_arg_t args[3];
} opt_cg_barrier_t;

typedef struct {
  asm_label_t label;
  asm_label_t exit;
  asm_label_t thrown;
} opt_cg_generic_t;

typedef struct {
  asm_label_t label;
  asm_label_t resume;
  asm_label_t exit;
  asm_gpr_t lhs, rhs;
} opt_cg_zero_product_t;

typedef struct {
  asm_label_t label;
  asm_label_t done;
  asm_label_t exit;
  asm_gpr_t output;
} opt_cg_object_kinds_t;

typedef struct {
  asm_label_t label;
  asm_label_t hole_exit;
  asm_label_t int32_exit;
  asm_gpr_t value;
} opt_cg_element_int32_t;

typedef struct {
  asm_label_t label;
  asm_label_t done;
  ir_node_id_t node;
  bool this_stored;
} opt_cg_call_slow_t;

typedef struct {
  asm_label_t label;
  asm_label_t resume;
  ir_node_id_t node;
} opt_cg_allocation_t;

typedef struct {
  size_t offset;
  asm_label_t exit;
} opt_cg_invalidation_t;

typedef struct {
  masm_t masm;
  const ir_graph_t *graph;
  const ra_allocation_t *alloc;
  const ir_frame_layout_t *layout;
  opt_code_t *code;
  opt_arena_t arena;
  opt_register_set_t registers;
  opt_pinned_registers_t pinned;
  asm_frame_t frame;
  uint32_t entry_off, index_off, error_off, copy_off, call_off, spill_off;
  asm_label_t *labels;
  ir_block_id_t *landing;
  asm_label_t exit_stub;
  asm_label_t leave;
  asm_label_t throw_label;
  asm_label_t overflow;
  asm_label_t save, restore;
  asm_label_t save_gprs, restore_gprs;
  asm_label_t osr_entry, copy_params, have_params;
  asm_regset_t saved_gprs, saved_fprs;
  bool saves;
  bool gpr_saves;
  bool has_frame;
  ir_block_id_t block;
  OPT_VEC(opt_cg_exit_stub_t) exit_stubs;
  OPT_VEC(opt_cg_barrier_t) barriers;
  OPT_VEC(opt_cg_generic_t) generics;
  OPT_VEC(opt_cg_allocation_t) allocations;
  OPT_VEC(opt_cg_zero_product_t) zero_products;
  OPT_VEC(opt_cg_element_int32_t) element_int32s;
  OPT_VEC(opt_cg_call_slow_t) call_slows;
  OPT_VEC(opt_cg_object_kinds_t) object_kinds;
  OPT_VEC(opt_site_t) sites;
  OPT_VEC(opt_site_frame_t) site_frames;
  opt_map_t site_index;
  opt_map_t frames_index;
  OPT_VEC(opt_call_site_t) call_sites;
  uint32_t exit_ret_off;
  bool failed;
  const char *failure;
  ant_t *js;
  bool marks_wanted;
  OPT_VEC(opt_cg_mark_t) marks;
  OPT_VEC(opt_cg_invalidation_t) invalidations;
  OPT_VEC(opt_patch_t) patches;
} opt_cg_t;

static void opt_cg_fail(opt_cg_t *cg, const char *reason) {
  if (!cg->failed) cg->failure = reason;
  cg->failed = true;
}

static inline asm_address_t opt_cg_local(const opt_cg_t *cg, uint32_t offset) {
  return asm_addr(MASM_STACK_POINTER, (int32_t)offset);
}

static inline asm_address_t opt_cg_spill(const opt_cg_t *cg, uint32_t slot) {
  return opt_cg_local(cg, cg->spill_off + 8 * slot);
}

static inline asm_address_t opt_cg_entry_field(const opt_cg_t *cg, size_t field) {
  return opt_cg_local(cg, cg->entry_off + (uint32_t)field);
}

static inline bool opt_cg_is_fpr(uint8_t reg) {
  return reg >= OPT_FPR_BASE;
}

static inline asm_fpr_t opt_cg_fpr(uint8_t reg) {
  return (asm_fpr_t)(reg - OPT_FPR_BASE);
}

static const ra_node_allocation_t *opt_cg_node(const opt_cg_t *cg, ir_node_id_t node) {
  return &cg->alloc->nodes[node];
}

static asm_gpr_t opt_cg_input(const opt_cg_t *cg, ir_node_id_t node, uint32_t index) {
  ra_location_t location = opt_cg_node(cg, node)->inputs.items[index];
  assert(location.kind == RA_LOC_REGISTER && !opt_cg_is_fpr(location.reg));
  return location.reg;
}

static asm_fpr_t opt_cg_float_input(const opt_cg_t *cg, ir_node_id_t node, uint32_t index) {
  ra_location_t location = opt_cg_node(cg, node)->inputs.items[index];
  assert(location.kind == RA_LOC_REGISTER && opt_cg_is_fpr(location.reg));
  return opt_cg_fpr(location.reg);
}

static asm_gpr_t opt_cg_output(const opt_cg_t *cg, ir_node_id_t node) {
  const ra_node_allocation_t *allocation = opt_cg_node(cg, node);
  assert(allocation->has_output && allocation->output.kind == RA_LOC_REGISTER && !opt_cg_is_fpr(allocation->output.reg));
  return allocation->output.reg;
}

static asm_fpr_t opt_cg_float_output(const opt_cg_t *cg, ir_node_id_t node) {
  const ra_node_allocation_t *allocation = opt_cg_node(cg, node);
  assert(allocation->has_output && allocation->output.kind == RA_LOC_REGISTER && opt_cg_is_fpr(allocation->output.reg));
  return opt_cg_fpr(allocation->output.reg);
}

static asm_gpr_t opt_cg_temp(const opt_cg_t *cg, ir_node_id_t node, uint32_t index) {
  return opt_cg_node(cg, node)->temps.items[index];
}

static bool opt_cg_input_constant(const opt_cg_t *cg, ir_node_id_t node, uint32_t index, uint64_t *bits) {
  ra_location_t location = opt_cg_node(cg, node)->inputs.items[index];
  if (location.kind != RA_LOC_CONSTANT) return false;
  *bits = location.bits;
  return true;
}

static inline asm_fpr_t opt_cg_lowering_fpr(uint32_t index) {
  return (asm_fpr_t)index;
}

static void opt_cg_used_register(asm_regset_t *gprs, asm_regset_t *fprs, ra_location_t location) {
  if (location.kind != RA_LOC_REGISTER) return;
  if (opt_cg_is_fpr(location.reg)) *fprs = asm_regset_with(*fprs, opt_cg_fpr(location.reg));
  else *gprs = asm_regset_with(*gprs, location.reg);
}

static void opt_cg_lay_out_frame(opt_cg_t *cg) {
  uint32_t call_slots = 0;
  for (ir_node_id_t node = 0; node < ir_node_count(cg->graph); node++) {
    const ir_op_t *op = &ir_node(cg->graph, node)->op;
    if (op->code == IR_OP_CALL_DIRECT || op->code == IR_OP_CALL_NATIVE || op->code == IR_OP_CALL_CONSTRUCT)
      call_slots = call_slots > 2u + op->call.argc ? call_slots : 2u + op->call.argc;
  }
  cg->entry_off = OPT_OUTGOING_BYTES;
  cg->index_off = (uint32_t)((cg->entry_off + sizeof(opt_entry_t) + 7) & ~(size_t)7);
  cg->error_off = cg->index_off + 8;
  cg->copy_off = cg->error_off + (cg->graph->handlers.len ? 8 : 0);
  cg->call_off = cg->copy_off + 8 * cg->layout->number_of_arguments;
  cg->spill_off = cg->call_off + 8 * call_slots;
  uint32_t size = cg->spill_off + 8 * cg->alloc->spill_slot_count;

  asm_regset_t gprs = 0, fprs = 0;
  for (uint32_t i = 0; i < cg->alloc->node_count; i++) {
    const ra_node_allocation_t *node = &cg->alloc->nodes[i];
    for (uint32_t j = 0; j < node->inputs.len; j++) opt_cg_used_register(&gprs, &fprs, node->inputs.items[j]);
    if (node->has_output) opt_cg_used_register(&gprs, &fprs, node->output);
    for (uint32_t j = 0; j < node->moves_before.len; j++) {
      opt_cg_used_register(&gprs, &fprs, node->moves_before.items[j].from);
      opt_cg_used_register(&gprs, &fprs, node->moves_before.items[j].to);
    }
    for (uint32_t j = 0; j < node->temps.len; j++) gprs = asm_regset_with(gprs, node->temps.items[j]);
  }
  for (uint32_t i = 0; i < cg->alloc->exit_table.values.len; i++) {
    const opt_exit_value_t *value = &cg->alloc->exit_table.values.items[i];
    if (value->kind == RA_LOC_REGISTER) opt_cg_used_register(&gprs, &fprs, ra_register(value->reg));
  }
  for (uint32_t b = 0; b < cg->alloc->block_count; b++) {
    const ra_moves_t *moves = &cg->alloc->block_end_moves[b];
    for (uint32_t j = 0; j < moves->len; j++) {
      opt_cg_used_register(&gprs, &fprs, moves->items[j].from);
      opt_cg_used_register(&gprs, &fprs, moves->items[j].to);
    }
  }
  cg->saved_gprs = gprs & ~MASM_SCRATCH_GPRS & ~asm_regset_with(0, MASM_STACK_POINTER);
  cg->saved_fprs = fprs & ~MASM_SCRATCH_FPRS;
  gprs = asm_regset_with(asm_regset_with(asm_regset_with(gprs, cg->pinned.vm), cg->pinned.frame), cg->pinned.scratch);
  cg->frame = masm_frame(gprs & MASM_CALLEE_SAVED_GPRS, fprs & MASM_CALLEE_SAVED_FPRS, size);
}

static void opt_cg_move(opt_cg_t *cg, ra_move_t move) {
  masm_t *m = &cg->masm;
  asm_gpr_t scratch = cg->pinned.scratch;
  switch (move.from.kind) {
    case RA_LOC_REGISTER:
      if (move.to.kind == RA_LOC_REGISTER) {
        if (opt_cg_is_fpr(move.from.reg)) masm_move_double(m, opt_cg_fpr(move.to.reg), opt_cg_fpr(move.from.reg));
        else masm_move64(m, move.to.reg, move.from.reg);
      } else if (opt_cg_is_fpr(move.from.reg)) masm_store_double(m, opt_cg_spill(cg, move.to.slot), opt_cg_fpr(move.from.reg));
      else masm_store64(m, opt_cg_spill(cg, move.to.slot), move.from.reg);
      break;
    case RA_LOC_STACK:
      if (move.to.kind == RA_LOC_REGISTER) {
        if (opt_cg_is_fpr(move.to.reg)) masm_load_double(m, opt_cg_fpr(move.to.reg), opt_cg_spill(cg, move.from.slot));
        else masm_load64(m, move.to.reg, opt_cg_spill(cg, move.from.slot));
      } else {
        masm_load64(m, scratch, opt_cg_spill(cg, move.from.slot));
        masm_store64(m, opt_cg_spill(cg, move.to.slot), scratch);
      }
      break;
    case RA_LOC_CONSTANT:
      if (move.to.kind == RA_LOC_STACK) masm_store_imm64(m, opt_cg_spill(cg, move.to.slot), move.from.bits);
      else if (opt_cg_is_fpr(move.to.reg)) masm_move_double_imm(m, opt_cg_fpr(move.to.reg), ir_value_to_double(move.from.bits));
      else masm_move_imm64(m, move.to.reg, move.from.bits);
      break;
  }
}

static uint32_t opt_cg_resume_depth(const ir_frame_state_t *state, const ir_frame_layout_t *layout) {
  if (state->mode == OPT_RESUME_RETURN) return 0;
  if (state->mode == OPT_RESUME_AFTER && state->dst != OPT_INVALID_SLOT) return state->dst;
  uint32_t depth = 0;
  for (uint32_t i = 0; i < state->values.len; i++) {
    uint32_t slot = state->values.items[i].slot;
    if (slot >= 1 && slot < layout->number_of_registers && slot >= depth) depth = slot;
  }
  for (uint32_t i = 0; i < state->in_frame.len; i++) {
    uint32_t slot = state->in_frame.items[i];
    if (slot >= 1 && slot < layout->number_of_registers && slot >= depth) depth = slot;
  }
  return depth;
}

static bool opt_cg_same_frames(const opt_site_frame_t *a, const opt_site_frame_t *b, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    if (a[i].func != b[i].func || a[i].bc_off != b[i].bc_off || a[i].call_off != b[i].call_off || a[i].depth != b[i].depth) return false;
    if (a[i].value_start != b[i].value_start || a[i].value_count != b[i].value_count || a[i].argc != b[i].argc) return false;
    if (a[i].mode != b[i].mode || a[i].construct != b[i].construct) return false;
  }
  return true;
}

static uint32_t opt_cg_intern_frames(opt_cg_t *cg, uint32_t start, uint32_t count) {
  const opt_site_frame_t *frames = &cg->site_frames.items[start];
  uint64_t hash = count;
  for (uint32_t i = 0; i < count; i++) {
    hash = opt_hash_add(hash, (uint64_t)(uintptr_t)frames[i].func);
    hash = opt_hash_add(hash, (uint64_t)frames[i].bc_off << 32 | frames[i].call_off);
    hash = opt_hash_add(hash, (uint64_t)frames[i].value_start << 32 | frames[i].value_count);
  }
  bool inserted = false;
  uint64_t *found = opt_map_entry(&cg->frames_index, &hash, &inserted);
  if (inserted) {
    *found = start;
    return start;
  }
  if (!opt_cg_same_frames(&cg->site_frames.items[*found], frames, count)) return start;
  cg->site_frames.len = start;
  return (uint32_t)*found;
}

static asm_label_t opt_cg_intern_site(opt_cg_t *cg, const opt_site_t *site) {
  uint64_t key[4] = {
    (uint64_t)site->bc_off << 32 | site->depth, (uint64_t)site->kind << 56 | (uint64_t)(site->handler & 0xffffff) << 32 | site->slot,
    (uint64_t)site->values.first << 32 | site->values.count, (uint64_t)site->frame_start << 32 | site->frame_count,
  };
  bool inserted = false;
  uint64_t *known = opt_map_entry(&cg->site_index, key, &inserted);
  if (!inserted) return cg->exit_stubs.items[*known].label;
  *known = cg->sites.len;
  opt_cg_exit_stub_t stub = {.label = masm_new_label(&cg->masm), .index = cg->sites.len, .catches = site->handler != UINT32_MAX};
  opt_vec_push(&cg->arena, &cg->sites, *site);
  opt_vec_push(&cg->arena, &cg->exit_stubs, stub);
  return stub.label;
}

static asm_label_t opt_cg_exit_site(opt_cg_t *cg, ir_node_id_t node) {
  const ir_node_t *data = ir_node(cg->graph, node);
  opt_exit_kind_t kind = OPT_EXIT_SLOW_PATH;
  ir_op_exit_kind(&data->op, &kind);
  if (ir_node_vec_contains(&cg->graph->speculatively_hoisted, node)) kind = OPT_EXIT_HOISTED;
  const ir_frame_state_t *state = ir_frame_state(cg->graph, data->frame_state);
  opt_exit_list_t values = opt_cg_node(cg, node)->exits;
  bool phi_check = data->op.code == IR_OP_CHECK_INT32 || data->op.code == IR_OP_CHECK_NUMBER;
  uint32_t slot = phi_check ? data->op.slot : 0;
  if (kind == OPT_EXIT_BAD_SHAPE) {
    uint32_t learned = opt_learned_shapes(state->func ? state->func : cg->graph->func, state->bc_off);
    if (learned) slot = learned + 1;
  }
  opt_site_t site = {.bc_off = state->bc_off, .kind = kind, .slot = slot, .values = values, .handler = UINT32_MAX};
  if (state->parent == IR_NONE) {
    site.depth = opt_cg_resume_depth(state, cg->layout);
  } else {
    ir_node_vec_t chain = {0};
    ir_frame_state_chain(cg->graph, data->frame_state, &cg->arena, &chain);
    site.frame_start = cg->site_frames.len;
    site.frame_count = chain.len;
    uint32_t next = 0;
    for (uint32_t k = 0; k < chain.len; k++) {
      const ir_frame_state_t *frame = ir_frame_state(cg->graph, chain.items[k]);
      sv_func_t *func = frame->func ? frame->func : cg->graph->func;
      ir_frame_layout_t layout = opt_frame_layout_of(func);
      opt_site_frame_t entry = {
        .func = func,
        .bc_off = frame->bc_off,
        .call_off = frame->call_off,
        .depth = opt_cg_resume_depth(frame, &layout),
        .value_start = next,
        .value_count = frame->values.len,
        .argc = frame->passed_argument_count,
        .mode = frame->mode,
        .construct = frame->construct,
      };
      next += frame->values.len;
      opt_vec_push(&cg->arena, &cg->site_frames, entry);
    }
    if (next > values.count) opt_cg_fail(cg, "exit-values");
    else if (next != values.count) opt_cg_fail(cg, "exit-virtual-objects");
    site.frame_start = opt_cg_intern_frames(cg, site.frame_start, site.frame_count);
  }
  return opt_cg_intern_site(cg, &site);
}

static asm_label_t opt_cg_catch_site(opt_cg_t *cg, ir_node_id_t node) {
  const ir_node_t *data = ir_node(cg->graph, node);
  const ir_handler_t *handler = &cg->graph->handlers.items[data->handler];
  opt_site_t site = {
    .bc_off = handler->catch_off + (handler->finally ? 0 : sv_op_size[OP_CATCH]),
    .depth = handler->depth + 1,
    .kind = OPT_EXIT_SLOW_PATH,
    .values = opt_cg_node(cg, node)->exits,
    .handler = data->handler,
  };
  return opt_cg_intern_site(cg, &site);
}

static asm_label_t opt_cg_throw_target(opt_cg_t *cg, ir_node_id_t node, asm_label_t fallback) {
  return ir_node(cg->graph, node)->handler == IR_NONE ? fallback : opt_cg_catch_site(cg, node);
}

static void opt_cg_record_call(opt_cg_t *cg, ir_frame_state_id_t position, int32_t bc_off) {
  opt_call_site_t site = {.ret_off = (uint32_t)masm_offset(&cg->masm), .bc_off = bc_off};
  if (position != IR_NONE) {
    ir_node_vec_t chain = {0};
    ir_frame_state_chain(cg->graph, position, &cg->arena, &chain);
    for (uint32_t k = chain.len; k-- > 0;) {
      const ir_frame_state_t *state = ir_frame_state(cg->graph, chain.items[k]);
      int32_t at = (int32_t)(state->mode == OPT_RESUME_AT ? state->bc_off : state->call_off);
      if (k == chain.len - 1) site.bc_off = at;
      else if (site.depth < SV_JIT_INLINE_FRAMES_MAX)
        site.frames[site.depth++] = (sv_jit_frame_t){state->func ? state->func : cg->graph->func, at};
    }
  }
  opt_vec_push(&cg->arena, &cg->call_sites, site);
}

static void opt_cg_branch_not_number(opt_cg_t *cg, asm_gpr_t value, asm_label_t target) {
  masm_move_imm64(&cg->masm, cg->pinned.scratch, NANBOX_PREFIX);
  masm_branch64(&cg->masm, ASM_COND_ABOVE, value, cg->pinned.scratch, target);
}

static void opt_cg_box_float(opt_cg_t *cg, asm_gpr_t dst, asm_fpr_t number, bool canonicalize) {
  masm_t *m = &cg->masm;
  masm_move_double_to_gpr(m, dst, number);
  if (!canonicalize) return;
  asm_label_t done = masm_new_label(m);
  masm_move_imm64(m, cg->pinned.scratch, NANBOX_PREFIX);
  masm_branch64(m, ASM_COND_BELOW_OR_EQUAL, dst, cg->pinned.scratch, done);
  masm_move_imm64(m, dst, IR_VALUE_CANONICAL_NAN);
  masm_bind(m, done);
}

static void opt_cg_box_int32(opt_cg_t *cg, asm_gpr_t dst, asm_gpr_t integer) {
  asm_fpr_t number = opt_cg_lowering_fpr(0);
  masm_convert_int32_to_double(&cg->masm, number, integer);
  masm_move_double_to_gpr(&cg->masm, dst, number);
}

static asm_cond_t opt_cg_int32_condition(ir_comparison_t comparison) {
  switch (comparison) {
    case IR_CMP_LESS_THAN: return ASM_COND_LESS_THAN;
    case IR_CMP_LESS_THAN_EQUALS: return ASM_COND_LESS_THAN_OR_EQUAL;
    case IR_CMP_GREATER_THAN: return ASM_COND_GREATER_THAN;
    case IR_CMP_GREATER_THAN_EQUALS: return ASM_COND_GREATER_THAN_OR_EQUAL;
    case IR_CMP_STRICTLY_EQUALS:
    case IR_CMP_LOOSELY_EQUALS: return ASM_COND_EQUAL;
    case IR_CMP_STRICTLY_INEQUALS:
    case IR_CMP_LOOSELY_INEQUALS: return ASM_COND_NOT_EQUAL;
    case IR_CMP_BELOW: return ASM_COND_BELOW;
    case IR_CMP_BELOW_OR_EQUAL: return ASM_COND_BELOW_OR_EQUAL;
    case IR_CMP_ABOVE: return ASM_COND_ABOVE;
    case IR_CMP_ABOVE_OR_EQUAL: return ASM_COND_ABOVE_OR_EQUAL;
  }
  return ASM_COND_EQUAL;
}

static asm_dcond_t opt_cg_double_condition(ir_comparison_t comparison) {
  switch (comparison) {
    case IR_CMP_LESS_THAN: return ASM_DCOND_LESS_THAN;
    case IR_CMP_LESS_THAN_EQUALS: return ASM_DCOND_LESS_THAN_OR_EQUAL;
    case IR_CMP_GREATER_THAN: return ASM_DCOND_GREATER_THAN;
    case IR_CMP_GREATER_THAN_EQUALS: return ASM_DCOND_GREATER_THAN_OR_EQUAL;
    case IR_CMP_STRICTLY_EQUALS:
    case IR_CMP_LOOSELY_EQUALS: return ASM_DCOND_EQUAL;
    case IR_CMP_STRICTLY_INEQUALS:
    case IR_CMP_LOOSELY_INEQUALS: return ASM_DCOND_NOT_EQUAL_OR_UNORDERED;
    default: break;
  }
  return ASM_DCOND_EQUAL;
}

static void opt_cg_int32_remainder(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  asm_gpr_t lhs = opt_cg_input(cg, node, 0), output = opt_cg_output(cg, node);
  asm_gpr_t scratch = cg->pinned.scratch, quotient = opt_cg_temp(cg, node, 0);
  asm_label_t exit = opt_cg_exit_site(cg, node), done = masm_new_label(m);
  uint64_t bits = 0;
  bool constant = opt_cg_input_constant(cg, node, 1, &bits);
  int32_t divisor = (int32_t)(uint32_t)bits;
  if (constant && !divisor) {
    masm_jump(m, exit);
    return;
  }
  uint32_t magnitude = constant ? (divisor < 0 ? 0u - (uint32_t)divisor : (uint32_t)divisor) : 0;
  if (constant && !(magnitude & (magnitude - 1))) {
    asm_label_t negative = masm_new_label(m);
    masm_branch32_imm(m, ASM_COND_LESS_THAN, lhs, 0, negative);
    masm_and32_imm(m, output, lhs, magnitude - 1);
    masm_jump(m, done);
    masm_bind(m, negative);
    masm_neg32(m, scratch, lhs);
    masm_and32_imm(m, scratch, scratch, magnitude - 1);
    masm_branch32_imm(m, ASM_COND_EQUAL, scratch, 0, exit);
    masm_neg32(m, output, scratch);
    masm_bind(m, done);
    return;
  }
#if defined(__aarch64__)
  asm_gpr_t rhs = scratch;
  if (constant) masm_move_imm32(m, scratch, (uint32_t)divisor);
  else {
    rhs = opt_cg_input(cg, node, 1);
    masm_branch_test32(m, ASM_COND_ZERO, rhs, UINT32_MAX, exit);
  }
  masm_remainder32(m, scratch, lhs, rhs, quotient);
#else
  asm_fpr_t lhs_number = opt_cg_lowering_fpr(0), rhs_number = opt_cg_lowering_fpr(1);
  masm_convert_int32_to_double(m, lhs_number, lhs);
  if (constant) {
    masm_move_double_imm(m, rhs_number, (double)divisor);
    masm_move_imm64(m, scratch, (uint64_t)(int64_t)divisor);
  } else {
    asm_gpr_t rhs = opt_cg_input(cg, node, 1);
    masm_branch_test32(m, ASM_COND_ZERO, rhs, UINT32_MAX, exit);
    masm_convert_int32_to_double(m, rhs_number, rhs);
    masm_sign_extend32_to_64(m, scratch, rhs);
  }
  masm_div_double(m, lhs_number, lhs_number, rhs_number);
  masm_truncate_double_to_int64(m, quotient, lhs_number);
  masm_mul64(m, quotient, quotient, scratch);
  masm_sign_extend32_to_64(m, scratch, lhs);
  masm_sub64(m, scratch, scratch, quotient);
#endif
  masm_branch_test32(m, ASM_COND_NON_ZERO, scratch, UINT32_MAX, done);
  masm_branch32_imm(m, ASM_COND_LESS_THAN, lhs, 0, exit);
  masm_bind(m, done);
  masm_move32(m, output, scratch);
}

static void opt_cg_int32_binary(opt_cg_t *cg, ir_node_id_t node, ir_binary_op_t op) {
  masm_t *m = &cg->masm;
  asm_gpr_t lhs = opt_cg_input(cg, node, 0), output = opt_cg_output(cg, node);
  uint64_t bits;
  if (op == IR_BINARY_MOD) {
    opt_cg_int32_remainder(cg, node);
    return;
  }
  if (opt_cg_input_constant(cg, node, 1, &bits)) {
    int32_t imm = (int32_t)(uint32_t)bits;
    switch (op) {
      case IR_BINARY_ADD: masm_branch_add32_imm_overflow(m, output, lhs, imm, opt_cg_exit_site(cg, node)); return;
      case IR_BINARY_SUB: masm_branch_sub32_imm_overflow(m, output, lhs, imm, opt_cg_exit_site(cg, node)); return;
      case IR_BINARY_BITWISE_AND: masm_and32_imm(m, output, lhs, (uint32_t)imm); return;
      case IR_BINARY_BITWISE_OR: masm_or32_imm(m, output, lhs, (uint32_t)imm); return;
      case IR_BINARY_BITWISE_XOR: masm_xor32_imm(m, output, lhs, (uint32_t)imm); return;
      case IR_BINARY_LEFT_SHIFT: masm_shl32_imm(m, output, lhs, (uint8_t)(imm & 31)); return;
      case IR_BINARY_RIGHT_SHIFT: masm_sar32_imm(m, output, lhs, (uint8_t)(imm & 31)); return;
      case IR_BINARY_UNSIGNED_RIGHT_SHIFT: {
        asm_label_t exit = opt_cg_exit_site(cg, node);
        masm_shr32_imm(m, output, lhs, (uint8_t)(imm & 31));
        masm_branch32_imm(m, ASM_COND_LESS_THAN, output, 0, exit);
        return;
      }
      default: opt_cg_fail(cg, "int32-immediate"); return;
    }
  }
  asm_gpr_t rhs = opt_cg_input(cg, node, 1);
  switch (op) {
    case IR_BINARY_ADD: masm_branch_add32_overflow(m, output, lhs, rhs, opt_cg_exit_site(cg, node)); return;
    case IR_BINARY_SUB: masm_branch_sub32_overflow(m, output, lhs, rhs, opt_cg_exit_site(cg, node)); return;
    case IR_BINARY_MUL: {
      asm_label_t exit = opt_cg_exit_site(cg, node);
      masm_branch_mul32_overflow(m, output, lhs, rhs, exit);
      opt_cg_zero_product_t zero = {.label = masm_new_label(m), .resume = masm_new_label(m), .exit = exit, .lhs = lhs, .rhs = rhs};
      masm_branch32_imm(m, ASM_COND_EQUAL, output, 0, zero.label);
      masm_bind(m, zero.resume);
      opt_vec_push(&cg->arena, &cg->zero_products, zero);
      return;
    }
    case IR_BINARY_BITWISE_AND: masm_and32(m, output, lhs, rhs); return;
    case IR_BINARY_BITWISE_OR: masm_or32(m, output, lhs, rhs); return;
    case IR_BINARY_BITWISE_XOR: masm_xor32(m, output, lhs, rhs); return;
    case IR_BINARY_LEFT_SHIFT: masm_shl32(m, output, lhs, rhs); return;
    case IR_BINARY_RIGHT_SHIFT: masm_sar32(m, output, lhs, rhs); return;
    case IR_BINARY_UNSIGNED_RIGHT_SHIFT: {
      asm_label_t exit = opt_cg_exit_site(cg, node);
      masm_shr32(m, output, lhs, rhs);
      masm_branch32_imm(m, ASM_COND_LESS_THAN, output, 0, exit);
      return;
    }
    default: opt_cg_fail(cg, "int32-binary"); return;
  }
}

static void opt_cg_int32_wrapping_binary(opt_cg_t *cg, ir_node_id_t node, ir_binary_op_t op) {
  masm_t *m = &cg->masm;
  asm_gpr_t lhs = opt_cg_input(cg, node, 0), output = opt_cg_output(cg, node);
  uint64_t bits;
  if (op != IR_BINARY_MUL && opt_cg_input_constant(cg, node, 1, &bits)) {
    int32_t imm = (int32_t)(uint32_t)bits;
    switch (op) {
      case IR_BINARY_ADD: masm_add32_imm(m, output, lhs, imm); return;
      case IR_BINARY_SUB: masm_sub32_imm(m, output, lhs, imm); return;
      case IR_BINARY_UNSIGNED_RIGHT_SHIFT: masm_shr32_imm(m, output, lhs, (uint8_t)(imm & 31)); return;
      default: opt_cg_fail(cg, "int32-wrapping-immediate"); return;
    }
  }
  asm_gpr_t rhs = opt_cg_input(cg, node, 1);
  switch (op) {
    case IR_BINARY_ADD: masm_add32(m, output, lhs, rhs); return;
    case IR_BINARY_SUB: masm_sub32(m, output, lhs, rhs); return;
    case IR_BINARY_MUL: masm_mul32(m, output, lhs, rhs); return;
    case IR_BINARY_UNSIGNED_RIGHT_SHIFT: masm_shr32(m, output, lhs, rhs); return;
    default: opt_cg_fail(cg, "int32-wrapping-binary"); return;
  }
}

static asm_address_t opt_cg_named_slot(asm_gpr_t base, uint32_t offset) {
  uint32_t index = opt_named_index(offset), limit = opt_named_inobj_limit(offset);
  if (index < limit) return asm_addr(base, (int32_t)(offsetof(ant_object_t, inobj) + 8 * index));
  return asm_addr(base, (int32_t)(8 * (index - limit)));
}

static bool opt_cg_needs_barrier(const opt_cg_t *cg, ir_node_id_t value) {
  uint64_t bits;
  const ir_node_t *node = ir_node(cg->graph, ir_unrefined(cg->graph, value));
  if (ir_constant_value(cg->graph, value, &bits)) return !ir_value_is_number(bits) && (ir_value_is_object(bits) || ir_value_is_string(bits));
  return node->op.code != IR_OP_BOX_INT32 && node->op.code != IR_OP_BOX_FLOAT64 && node->op.code != IR_OP_BOX_BOOL;
}

static inline opt_cg_arg_t opt_cg_reg_arg(asm_gpr_t reg) {
  return (opt_cg_arg_t){.reg = reg};
}

static opt_cg_arg_t opt_cg_input_arg(const opt_cg_t *cg, ir_node_id_t node, uint32_t index) {
  uint64_t bits;
  if (opt_cg_input_constant(cg, node, index, &bits)) return (opt_cg_arg_t){.constant = true, .bits = bits};
  return opt_cg_reg_arg(opt_cg_input(cg, node, index));
}

static asm_label_t opt_cg_out_of_line(opt_cg_t *cg, asm_label_t resume, uint64_t helper, const opt_cg_arg_t *args, uint32_t argc) {
  opt_cg_barrier_t call = {.label = masm_new_label(&cg->masm), .resume = resume, .helper = helper, .argc = argc};
  memcpy(call.args, args, argc * sizeof *args);
  opt_vec_push(&cg->arena, &cg->barriers, call);
  return call.label;
}

static void opt_cg_object_barrier(opt_cg_t *cg, asm_gpr_t object, asm_gpr_t value, uint64_t helper, const opt_cg_arg_t *index) {
  masm_t *m = &cg->masm;
  asm_gpr_t scratch = cg->pinned.scratch;
  asm_label_t resume = masm_new_label(m), decode = masm_new_label(m);
  opt_cg_arg_t args[3] = {opt_cg_reg_arg(object), index ? *index : opt_cg_reg_arg(value), opt_cg_reg_arg(value)};
  asm_label_t call = opt_cg_out_of_line(cg, resume, helper, args, index ? 3 : 2);
  masm_load16(m, scratch, asm_addr(object, (int32_t)offsetof(ant_object_t, flags)));
  masm_branch_test32(m, ASM_COND_ZERO, scratch, ANT_OBJECT_FLAG_GENERATION, resume);
  if (!index) masm_branch_test32(m, ASM_COND_NON_ZERO, scratch, ANT_OBJECT_FLAG_REMEMBERED, resume);
  masm_move_imm64(m, scratch, NANBOX_PREFIX);
  masm_branch64(m, ASM_COND_BELOW_OR_EQUAL, value, scratch, resume);
  masm_shr64_imm(m, scratch, value, NANBOX_TYPE_SHIFT);
  masm_branch64_imm(m, ASM_COND_EQUAL, scratch, (int64_t)OPT_OBJECT_TAG, decode);
  masm_branch64_imm(m, ASM_COND_EQUAL, scratch, (int64_t)OPT_ARRAY_TAG, decode);
  masm_branch64_imm(m, ASM_COND_EQUAL, scratch, (int64_t)((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | kTypePromise), decode);
  masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, (int64_t)((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | kTypeString), call);
  masm_and32_imm(m, scratch, value, (int32_t)STR_HEAP_TAG_MASK);
  masm_branch32_imm(m, ASM_COND_NOT_EQUAL, scratch, (int32_t)STR_HEAP_TAG_ROPE, resume);
  masm_and64_imm(m, scratch, value, NANBOX_DATA_MASK);
  masm_add64_imm(m, scratch, scratch, (int64_t)ant_cage_base());
  masm_load16(m, scratch, asm_addr(scratch, (int32_t)offsetof(ant_rope_heap_t, flags) - (int32_t)STR_HEAP_TAG_ROPE));
  masm_branch_test32(m, ASM_COND_NON_ZERO, scratch, ANT_ROPE_FLAG_YOUNG, call);
  masm_jump(m, resume);
  masm_bind(m, decode);
  masm_and64_imm(m, scratch, value, NANBOX_DATA_MASK);
  masm_add64_imm(m, scratch, scratch, (int64_t)ant_cage_base());
  masm_load16(m, scratch, asm_addr(scratch, (int32_t)offsetof(ant_object_t, flags)));
  masm_branch_test32(m, ASM_COND_ZERO, scratch, ANT_OBJECT_FLAG_GENERATION, call);
  masm_bind(m, resume);
}

static void opt_cg_store_named(opt_cg_t *cg, ir_node_id_t node, uint32_t offset) {
  masm_t *m = &cg->masm;
  asm_gpr_t value = opt_cg_input(cg, node, 1), object = opt_cg_input(cg, node, 2);
  asm_gpr_t base = ir_node(cg->graph, node)->inputs.len > 3 ? opt_cg_input(cg, node, 3) : object;
  masm_store64(m, opt_cg_named_slot(base, offset), value);
  if (!opt_cg_needs_barrier(cg, ir_node(cg->graph, node)->inputs.items[1])) return;
  opt_cg_object_barrier(cg, object, value, (uint64_t)(uintptr_t)opt_rt_write_barrier, NULL);
}

static constexpr uint32_t OPT_DUMP_BYTES = 8 * (OPT_DUMP_GPRS + OPT_DUMP_FPRS);
#if defined(__x86_64__)
static constexpr int32_t OPT_RETURN_ADDRESS_BYTES = 8;
#else
static constexpr int32_t OPT_RETURN_ADDRESS_BYTES = 0;
#endif

static void opt_cg_dump_registers(opt_cg_t *cg) {
  masm_t *m = &cg->masm;
  if (!cg->saves) {
    cg->save = masm_new_label(m);
    cg->restore = masm_new_label(m);
    cg->saves = true;
  }
  masm_sub64_imm(m, MASM_STACK_POINTER, MASM_STACK_POINTER, OPT_DUMP_BYTES);
  masm_call(m, cg->save);
}

static void opt_cg_restore_caller_saved(opt_cg_t *cg) {
  masm_t *m = &cg->masm;
  masm_call(m, cg->restore);
  masm_add64_imm(m, MASM_STACK_POINTER, MASM_STACK_POINTER, OPT_DUMP_BYTES);
}

static void opt_cg_dump_caller_saved_gprs(opt_cg_t *cg) {
  masm_t *m = &cg->masm;
  if (!cg->gpr_saves) {
    cg->save_gprs = masm_new_label(m);
    cg->restore_gprs = masm_new_label(m);
    cg->gpr_saves = true;
  }
  masm_sub64_imm(m, MASM_STACK_POINTER, MASM_STACK_POINTER, OPT_DUMP_BYTES);
  masm_call(m, cg->save_gprs);
}

static void opt_cg_restore_caller_saved_gprs(opt_cg_t *cg) {
  masm_t *m = &cg->masm;
  masm_call(m, cg->restore_gprs);
  masm_add64_imm(m, MASM_STACK_POINTER, MASM_STACK_POINTER, OPT_DUMP_BYTES);
}

static void opt_cg_slow_argument(opt_cg_t *cg, asm_gpr_t dst, ir_node_id_t node, uint32_t index) {
  masm_t *m = &cg->masm;
  uint64_t bits;
  if (opt_cg_input_constant(cg, node, index, &bits)) {
    masm_move_imm64(m, dst, bits);
    return;
  }
  asm_gpr_t reg = opt_cg_input(cg, node, index);
  if (asm_regset_contains(cg->saved_gprs & MASM_CALLER_SAVED_GPRS, (uint8_t)reg)) masm_load64(m, dst, asm_addr(MASM_STACK_POINTER, (int32_t)(8 * reg)));
  else masm_move64(m, dst, reg);
}

static inline asm_address_t opt_cg_dump_slot(uint32_t index) {
  return asm_addr(MASM_STACK_POINTER, OPT_RETURN_ADDRESS_BYTES + (int32_t)(8 * index));
}

static void opt_cg_emit_gpr_save_routines(opt_cg_t *cg) {
  if (!cg->gpr_saves) return;
  masm_t *m = &cg->masm;
  asm_regset_t gprs = cg->saved_gprs & MASM_CALLER_SAVED_GPRS;
  masm_bind(m, cg->save_gprs);
  for (uint32_t reg = 0; reg < OPT_CG_GPRS; reg++)
    if (asm_regset_contains(gprs, (uint8_t)reg)) masm_store64(m, opt_cg_dump_slot(reg), (asm_gpr_t)reg);
  masm_ret(m);
  masm_bind(m, cg->restore_gprs);
  for (uint32_t reg = 0; reg < OPT_CG_GPRS; reg++)
    if (asm_regset_contains(gprs, (uint8_t)reg)) masm_load64(m, (asm_gpr_t)reg, opt_cg_dump_slot(reg));
  masm_ret(m);
}

static void opt_cg_emit_save_routines(opt_cg_t *cg) {
  opt_cg_emit_gpr_save_routines(cg);
  if (!cg->saves) return;
  masm_t *m = &cg->masm;
  masm_bind(m, cg->save);
  for (uint32_t reg = 0; reg < OPT_CG_GPRS; reg++)
    if (asm_regset_contains(cg->saved_gprs, (uint8_t)reg)) masm_store64(m, opt_cg_dump_slot(reg), (asm_gpr_t)reg);
  for (uint32_t reg = 0; reg < OPT_CG_FPRS; reg++)
    if (asm_regset_contains(cg->saved_fprs, (uint8_t)reg)) masm_store_double(m, opt_cg_dump_slot(OPT_DUMP_GPRS + reg), (asm_fpr_t)reg);
  masm_ret(m);
  masm_bind(m, cg->restore);
  for (uint32_t reg = 0; reg < OPT_CG_GPRS; reg++)
    if (asm_regset_contains(cg->saved_gprs & MASM_CALLER_SAVED_GPRS, (uint8_t)reg)) masm_load64(m, (asm_gpr_t)reg, opt_cg_dump_slot(reg));
  for (uint32_t reg = 0; reg < OPT_CG_FPRS; reg++)
    if (asm_regset_contains(cg->saved_fprs & MASM_CALLER_SAVED_FPRS, (uint8_t)reg))
      masm_load_double(m, (asm_fpr_t)reg, opt_cg_dump_slot(OPT_DUMP_GPRS + reg));
  masm_ret(m);
}

static void opt_cg_emit_barriers(opt_cg_t *cg) {
  masm_t *m = &cg->masm;
  for (uint32_t i = 0; i < cg->barriers.len; i++) {
    opt_cg_barrier_t *barrier = &cg->barriers.items[i];
    masm_bind(m, barrier->label);
    opt_cg_dump_registers(cg);
    for (uint32_t a = 0; a < barrier->argc; a++) {
      const opt_cg_arg_t *arg = &barrier->args[a];
      if (arg->constant) masm_move_imm64(m, MASM_ARGUMENT_GPRS[a + 1], arg->bits);
      else masm_load64(m, MASM_ARGUMENT_GPRS[a + 1], asm_addr(MASM_STACK_POINTER, (int32_t)(8 * arg->reg)));
    }
    masm_move64(m, MASM_ARGUMENT_GPRS[0], cg->pinned.vm);
    masm_call_absolute(m, barrier->helper);
    opt_cg_restore_caller_saved(cg);
    masm_jump(m, barrier->resume);
  }
}

static ir_block_id_t opt_cg_next_block(const opt_cg_t *cg) {
  for (ir_block_id_t block = cg->block + 1; block < ir_block_count(cg->graph); block++)
    if (cg->landing[block] == block) return block;
  return IR_NONE;
}

static inline asm_label_t opt_cg_label(const opt_cg_t *cg, ir_block_id_t block) {
  return cg->labels[cg->landing[block]];
}

static void opt_cg_jump_to(opt_cg_t *cg, ir_block_id_t target) {
  target = cg->landing[target];
  if (target != opt_cg_next_block(cg)) masm_jump(&cg->masm, cg->labels[target]);
}

static bool opt_cg_only_jumps(const opt_cg_t *cg, ir_block_id_t block) {
  const ir_block_t *data = ir_block(cg->graph, block);
  return !data->phis.len && !data->body.len && ir_control(cg->graph, block)->op.code == IR_OP_JUMP &&
    !cg->alloc->block_end_moves[block].len && !opt_cg_node(cg, data->control)->moves_before.len;
}

static void opt_cg_landing_blocks(opt_cg_t *cg) {
  uint32_t count = ir_block_count(cg->graph);
  cg->landing = opt_arena_new(&cg->arena, ir_block_id_t, count + 1);
  for (ir_block_id_t block = 0; block < count; block++) {
    ir_block_id_t landing = block;
    for (uint32_t steps = 0; steps < count && opt_cg_only_jumps(cg, landing); steps++)
      landing = ir_control(cg->graph, landing)->op.target;
    cg->landing[block] = opt_cg_only_jumps(cg, landing) ? block : landing;
  }
}

static void opt_cg_elements_kind(opt_cg_t *cg, ir_node_id_t node, ir_elements_kind_t kind, asm_label_t exit);

static void opt_cg_branch(opt_cg_t *cg, ir_node_id_t node, ir_condition_t condition, ir_block_id_t if_true, ir_block_id_t if_false) {
  masm_t *m = &cg->masm;
  bool invert = opt_cg_next_block(cg) == cg->landing[if_true] && cg->landing[if_false] != cg->landing[if_true];
  asm_label_t target = invert ? opt_cg_label(cg, if_false) : opt_cg_label(cg, if_true);
  uint64_t bits;
  switch (condition.kind) {
    case IR_COND_FLOAT64: {
      asm_dcond_t cond = opt_cg_double_condition(condition.comparison);
      if (invert) cond = asm_dcond_invert(cond);
      masm_branch_double(m, cond, opt_cg_float_input(cg, node, 0), opt_cg_float_input(cg, node, 1), target);
      break;
    }
    case IR_COND_BOOL: {
      asm_cond_t cond = invert ? ASM_COND_ZERO : ASM_COND_NON_ZERO;
      masm_branch_test64(m, cond, opt_cg_input(cg, node, 0), 1, target);
      break;
    }
    case IR_COND_INT32: {
      asm_cond_t cond = opt_cg_int32_condition(condition.comparison);
      if (invert) cond = asm_cond_invert(cond);
      if (opt_cg_input_constant(cg, node, 1, &bits)) masm_branch32_imm(m, cond, opt_cg_input(cg, node, 0), (int32_t)(uint32_t)bits, target);
      else masm_branch32(m, cond, opt_cg_input(cg, node, 0), opt_cg_input(cg, node, 1), target);
      break;
    }
    case IR_COND_TAGGED_EQUALS: {
      asm_cond_t cond = condition.equal ? ASM_COND_EQUAL : ASM_COND_NOT_EQUAL;
      if (invert) cond = asm_cond_invert(cond);
      if (opt_cg_input_constant(cg, node, 1, &bits)) masm_branch64_imm(m, cond, opt_cg_input(cg, node, 0), (int64_t)bits, target);
      else masm_branch64(m, cond, opt_cg_input(cg, node, 0), opt_cg_input(cg, node, 1), target);
      break;
    }
    case IR_COND_STRING: {
      asm_cond_t cond = invert ? ASM_COND_NOT_EQUAL : ASM_COND_EQUAL;
      masm_sar64_imm(m, cg->pinned.scratch, opt_cg_input(cg, node, 0), NANBOX_TYPE_SHIFT);
      masm_branch64_imm(m, cond, cg->pinned.scratch, OPT_STRING_TAG_SIGNED, target);
      break;
    }
    case IR_COND_UNDEFINED: {
      asm_cond_t cond = invert ? ASM_COND_NOT_EQUAL : ASM_COND_EQUAL;
      masm_branch64_imm(m, cond, opt_cg_input(cg, node, 0), (int64_t)IR_VALUE_UNDEFINED, target);
      break;
    }
    case IR_COND_RESOLVED_STRING:
    case IR_COND_ASCII_STRING: {
      asm_gpr_t scratch = cg->pinned.scratch, string = opt_cg_input(cg, node, 0);
      asm_label_t no = opt_cg_label(cg, if_false);
      masm_branch_test64(m, ASM_COND_NON_ZERO, string, (int64_t)STR_HEAP_TAG_MASK, no);
      masm_load64(m, scratch, asm_addr(string, (int32_t)offsetof(ant_flat_string_t, meta)));
      masm_and64_imm(m, scratch, scratch, (int64_t)STR_META_ASCII_MASK);
      masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, (int64_t)((uint64_t)STR_ASCII_YES << STR_META_ASCII_SHIFT), no);
      if (condition.kind == IR_COND_ASCII_STRING) {
        opt_cg_jump_to(cg, if_true);
        return;
      }
      masm_load64(m, scratch, asm_addr(cg->pinned.vm, (int32_t)offsetof(sv_vm_t, js)));
      masm_load32(m, scratch, asm_addr(scratch, (int32_t)offsetof(ant_t, vm_exec_depth)));
      masm_branch32_imm(m, ASM_COND_EQUAL, scratch, 0, no);
      opt_cg_jump_to(cg, if_true);
      return;
    }
    case IR_COND_ELEMENTS_KIND:
      opt_cg_elements_kind(cg, node, condition.elements, opt_cg_label(cg, if_false));
      opt_cg_jump_to(cg, if_true);
      return;
    case IR_COND_ITERABLE_ARRAY: {
      asm_gpr_t value = opt_cg_input(cg, node, 0), scratch = cg->pinned.scratch, temp = opt_cg_temp(cg, node, 0);
      asm_label_t no = opt_cg_label(cg, if_false);
      masm_shr64_imm(m, scratch, value, NANBOX_TYPE_SHIFT);
      masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, (int64_t)OPT_ARRAY_TAG, no);
      masm_load64(m, scratch, asm_addr(cg->pinned.vm, (int32_t)offsetof(sv_vm_t, js)));
      masm_load32(m, temp, asm_addr(scratch, (int32_t)offsetof(ant_t, protectors_invalid)));
      masm_branch_test32(m, ASM_COND_NON_ZERO, temp, (int32_t)ANT_PROTECTOR_ARRAY_ITERATION, no);
      masm_load64(m, temp, asm_addr(scratch, (int32_t)offsetof(ant_t, sym.array_proto)));
      masm_and64_imm(m, scratch, value, NANBOX_DATA_MASK);
      masm_add64_imm(m, scratch, scratch, (int64_t)ant_cage_base());
      masm_load64(m, scratch, asm_addr(scratch, (int32_t)offsetof(ant_object_t, proto)));
      masm_branch64(m, ASM_COND_NOT_EQUAL, scratch, temp, no);
      opt_cg_jump_to(cg, if_true);
      return;
    }
    case IR_COND_TYPED_ARRAY_LENGTH: {
      asm_gpr_t value = opt_cg_input(cg, node, 0), scratch = cg->pinned.scratch;
      asm_gpr_t ptr = opt_cg_temp(cg, node, 0), data = opt_cg_temp(cg, node, 1), js = opt_cg_temp(cg, node, 2);
      asm_label_t no = opt_cg_label(cg, if_false), proto_ok = masm_new_label(m);
      masm_shr64_imm(m, scratch, value, NANBOX_TYPE_SHIFT);
      masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, (int64_t)OPT_OBJECT_TAG, no);
      masm_and64_imm(m, ptr, value, NANBOX_DATA_MASK);
      masm_add64_imm(m, ptr, ptr, (int64_t)ant_cage_base());
      masm_load32(m, scratch, asm_addr(ptr, (int32_t)offsetof(ant_object_t, native_tag)));
      masm_branch32_imm(m, ASM_COND_NOT_EQUAL, scratch, (int32_t)BUFFER_TYPEDARRAY_NATIVE_TAG, no);
      masm_load64(m, js, asm_addr(cg->pinned.vm, (int32_t)offsetof(sv_vm_t, js)));
      masm_load32(m, scratch, asm_addr(js, (int32_t)offsetof(ant_t, protectors_invalid)));
      masm_branch_test32(m, ASM_COND_NON_ZERO, scratch, (int32_t)ANT_PROTECTOR_TYPED_ARRAY_LENGTH, no);
      masm_load64(m, data, asm_addr(ptr, (int32_t)offsetof(ant_object_t, native_ptr)));
      masm_load32(m, scratch, asm_addr(data, (int32_t)offsetof(TypedArrayData, type)));
      masm_branch32_imm(m, ASM_COND_ABOVE_OR_EQUAL, scratch, (int32_t)TYPED_ARRAY_COUNT, no);
      masm_load64(m, scratch, asm_addr_indexed(js, scratch, ASM_SCALE_8, (int32_t)offsetof(ant_t, builtins.typedarray_protos)));
      masm_load64(m, ptr, asm_addr(ptr, (int32_t)offsetof(ant_object_t, proto)));
      masm_branch64(m, ASM_COND_EQUAL, ptr, scratch, proto_ok);
      masm_load64(m, scratch, asm_addr(js, (int32_t)offsetof(ant_t, builtins.buffer_proto)));
      masm_branch64(m, ASM_COND_NOT_EQUAL, ptr, scratch, no);
      masm_load32(m, scratch, asm_addr(data, (int32_t)offsetof(TypedArrayData, type)));
      masm_branch32_imm(m, ASM_COND_NOT_EQUAL, scratch, (int32_t)TYPED_ARRAY_UINT8, no);
      masm_bind(m, proto_ok);
      masm_load64(m, scratch, asm_addr(data, (int32_t)offsetof(TypedArrayData, buffer)));
      masm_branch64_imm(m, ASM_COND_EQUAL, scratch, 0, no);
      masm_load8(m, scratch, asm_addr(scratch, (int32_t)offsetof(ArrayBufferData, views_slow)));
      masm_branch32_imm(m, ASM_COND_NOT_EQUAL, scratch, 0, no);
      masm_load64(m, scratch, asm_addr(data, (int32_t)offsetof(TypedArrayData, length)));
      masm_move_imm64(m, data, (uint64_t)INT32_MAX);
      masm_branch64(m, ASM_COND_ABOVE, scratch, data, no);
      opt_cg_jump_to(cg, if_true);
      return;
    }
    case IR_COND_ITERATOR_RETURN_INTACT: {
      asm_gpr_t scratch = cg->pinned.scratch;
      masm_load64(m, scratch, asm_addr(cg->pinned.vm, (int32_t)offsetof(sv_vm_t, js)));
      masm_load32(m, scratch, asm_addr(scratch, (int32_t)offsetof(ant_t, protectors_invalid)));
      masm_branch_test32(m, ASM_COND_NON_ZERO, scratch, (int32_t)ANT_PROTECTOR_ITERATOR_RETURN, opt_cg_label(cg, if_false));
      opt_cg_jump_to(cg, if_true);
      return;
    }
    case IR_COND_INDEX_IN_BOUNDS: {
      asm_cond_t cond = invert ? ASM_COND_ABOVE_OR_EQUAL : ASM_COND_BELOW;
      if (opt_cg_input_constant(cg, node, 0, &bits)) {
        cond = invert ? ASM_COND_BELOW_OR_EQUAL : ASM_COND_ABOVE;
        masm_branch32_imm(m, cond, opt_cg_input(cg, node, 1), (int32_t)(uint32_t)bits, target);
      } else masm_branch32(m, cond, opt_cg_input(cg, node, 0), opt_cg_input(cg, node, 1), target);
      break;
    }
    case IR_COND_NULLISH: {
      asm_gpr_t scratch = cg->pinned.scratch;
      masm_shr64_imm(m, scratch, opt_cg_input(cg, node, 0), NANBOX_TYPE_SHIFT);
      masm_branch64_imm(m, ASM_COND_EQUAL, scratch, (int64_t)OPT_NULL_TAG, opt_cg_label(cg, if_true));
      masm_branch64_imm(m, ASM_COND_EQUAL, scratch, (int64_t)OPT_UNDEFINED_TAG, opt_cg_label(cg, if_true));
      opt_cg_jump_to(cg, if_false);
      return;
    }
    default:
      opt_cg_fail(cg, "branch-condition");
      return;
  }
  if (!invert) opt_cg_jump_to(cg, if_false);
}

static void opt_cg_branch_truthy(opt_cg_t *cg, ir_node_id_t node, ir_block_id_t if_true, ir_block_id_t if_false, ir_block_id_t fallback) {
  masm_t *m = &cg->masm;
  asm_gpr_t input = opt_cg_input(cg, node, 0), scratch = cg->pinned.scratch;
  asm_label_t yes = opt_cg_label(cg, if_true), no = opt_cg_label(cg, if_false), other = masm_new_label(m);
  masm_branch64_imm(m, ASM_COND_EQUAL, input, (int64_t)IR_VALUE_TRUE, yes);
  masm_branch64_imm(m, ASM_COND_EQUAL, input, (int64_t)IR_VALUE_FALSE, no);
  masm_move_imm64(m, scratch, NANBOX_PREFIX);
  masm_branch64(m, ASM_COND_ABOVE, input, scratch, other);
  asm_fpr_t number = opt_cg_lowering_fpr(0), zero = opt_cg_lowering_fpr(1);
  masm_move_gpr_to_double(m, number, input);
  masm_move_double_imm(m, zero, 0.0);
  masm_branch_double(m, ASM_DCOND_EQUAL_OR_UNORDERED, number, zero, no);
  masm_jump(m, yes);
  masm_bind(m, other);
  masm_branch64_imm(m, ASM_COND_EQUAL, input, (int64_t)IR_VALUE_UNDEFINED, no);
  masm_branch64_imm(m, ASM_COND_EQUAL, input, (int64_t)IR_VALUE_NULL, no);
  static_assert(kTypeString == 1 && kTypeGenerator == 6 && kTypeSymbol > kTypeGenerator);
  masm_shr64_imm(m, scratch, input, NANBOX_TYPE_SHIFT);
  masm_and32_imm(m, scratch, scratch, NANBOX_TYPE_MASK);
  asm_label_t ask = opt_cg_label(cg, fallback);
  masm_branch32_imm(m, ASM_COND_EQUAL, scratch, kTypeString, ask);
  masm_branch32_imm(m, ASM_COND_BELOW_OR_EQUAL, scratch, kTypeGenerator, yes);
  masm_branch32_imm(m, ASM_COND_EQUAL, scratch, kTypeSymbol, yes);
  opt_cg_jump_to(cg, fallback);
}

static void opt_cg_check_identity_comparable(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  asm_label_t exit = opt_cg_exit_site(cg, node), ok = masm_new_label(m);
  asm_gpr_t scratch = cg->pinned.scratch, temp = opt_cg_temp(cg, node, 0);
  uint32_t mask = (uint32_t)ir_node(cg->graph, node)->op.bits;
  if (mask) {
    for (uint32_t i = 0; i < 2; i++) {
      asm_gpr_t value = opt_cg_input(cg, node, i);
      masm_move_imm64(m, scratch, NANBOX_PREFIX);
      masm_branch64(m, ASM_COND_BELOW_OR_EQUAL, value, scratch, exit);
      masm_shr64_imm(m, scratch, value, NANBOX_TYPE_SHIFT);
      masm_and32_imm(m, scratch, scratch, NANBOX_TYPE_MASK);
      masm_move_imm32(m, temp, mask);
      masm_shr32(m, temp, temp, scratch);
      masm_branch_test32(m, ASM_COND_ZERO, temp, 1, exit);
    }
    return;
  }
  for (uint32_t i = 0; i < 2; i++) {
    asm_gpr_t value = opt_cg_input(cg, node, i);
    asm_label_t next = masm_new_label(m);
    masm_move_imm64(m, scratch, NANBOX_PREFIX);
    masm_branch64(m, ASM_COND_BELOW_OR_EQUAL, value, scratch, next);
    masm_shr64_imm(m, scratch, value, NANBOX_TYPE_SHIFT);
    masm_and32_imm(m, scratch, scratch, NANBOX_TYPE_MASK);
    masm_move_imm32(m, temp, OPT_IDENTITY_TYPES);
    masm_shr32(m, temp, temp, scratch);
    masm_branch_test32(m, ASM_COND_NON_ZERO, temp, 1, ok);
    masm_bind(m, next);
  }
  masm_jump(m, exit);
  masm_bind(m, ok);
}

static uint32_t opt_cg_frame_index(const opt_cg_t *cg, uint32_t slot) {
  const ir_frame_layout_t *layout = cg->layout;
  if (slot >= layout->number_of_registers) return slot - layout->number_of_registers;
  return layout->registers_and_locals_count - layout->number_of_registers + slot - 1;
}

static void opt_cg_load_slot(opt_cg_t *cg, ir_node_id_t node, uint32_t slot) {
  masm_t *m = &cg->masm;
  asm_gpr_t output = opt_cg_output(cg, node);
  const ir_frame_layout_t *layout = cg->layout;
  if (slot == OPT_THIS_SLOT) {
    masm_load64(m, output, opt_cg_entry_field(cg, offsetof(opt_entry_t, this_val)));
    return;
  }
  if (slot >= ir_layout_arguments_base(layout)) {
    masm_load64(m, output, asm_addr(cg->pinned.frame, (int32_t)(8 * (slot - ir_layout_arguments_base(layout)))));
    return;
  }
  size_t base = offsetof(sv_vm_t, jit.osr);
  bool local = slot >= layout->number_of_registers;
  uint32_t index = local ? slot - layout->number_of_registers : slot - 1;
  if (cg->has_frame) {
    masm_load64(m, output, opt_cg_entry_field(cg, offsetof(opt_entry_t, locals)));
    masm_load64(m, output, asm_addr(output, (int32_t)(8 * opt_cg_frame_index(cg, slot))));
    return;
  }
  masm_load64(m, output, asm_addr(cg->pinned.vm, (int32_t)(base + (local ? offsetof(sv_jit_osr_t, locals) : offsetof(sv_jit_osr_t, vstack)))));
  masm_load64(m, output, asm_addr(output, (int32_t)(8 * index)));
}

static void opt_cg_store_slot(opt_cg_t *cg, ir_node_id_t node_id, uint32_t slot) {
  masm_t *m = &cg->masm;
  const ir_node_t *node = ir_node(cg->graph, node_id);
  ir_node_id_t input = node->inputs.items[0];
  asm_gpr_t value = opt_cg_temp(cg, node_id, 0);
  switch (ir_node(cg->graph, input)->repr) {
    case OPT_REPR_INT32: opt_cg_box_int32(cg, value, opt_cg_input(cg, node_id, 0)); break;
    case OPT_REPR_FLOAT64:
      opt_cg_box_float(cg, value, opt_cg_float_input(cg, node_id, 0), opt_float_may_be_negative_nan(cg->graph, input));
      break;
    case OPT_REPR_BOOL:
      masm_move32(m, value, opt_cg_input(cg, node_id, 0));
      masm_or64_imm(m, value, value, ANT_BOOL_TAG);
      break;
    default: value = opt_cg_input(cg, node_id, 0); break;
  }
  const ir_frame_layout_t *layout = cg->layout;
  if (slot >= ir_layout_arguments_base(layout)) {
    masm_store64(m, asm_addr(cg->pinned.frame, (int32_t)(8 * (slot - ir_layout_arguments_base(layout)))), value);
    return;
  }
  masm_load64(m, cg->pinned.scratch, opt_cg_entry_field(cg, offsetof(opt_entry_t, locals)));
  masm_store64(m, asm_addr(cg->pinned.scratch, (int32_t)(8 * opt_cg_frame_index(cg, slot))), value);
}

static void opt_cg_call_frame_helper(opt_cg_t *cg, uint64_t helper) {
  masm_t *m = &cg->masm;
  masm_move64(m, MASM_ARGUMENT_GPRS[0], cg->pinned.vm);
  masm_load_effective_address(m, MASM_ARGUMENT_GPRS[1], opt_cg_entry_field(cg, 0));
  masm_move_imm64(m, MASM_ARGUMENT_GPRS[2], (uint64_t)(uintptr_t)cg->code);
  masm_call_absolute(m, helper);
}

static uint32_t opt_cg_free_gprs(asm_gpr_t *out, uint32_t count) {
  uint32_t found = 0;
  for (uint8_t reg = 0; reg < OPT_CG_GPRS && found < count; reg++)
    if (asm_regset_contains(MASM_CALLER_SAVED_GPRS, reg) && !asm_regset_contains(MASM_SCRATCH_GPRS, reg)) out[found++] = (asm_gpr_t)reg;
  return found;
}

typedef struct {
  asm_gpr_t vm, s;
  asm_gpr_t closure, value, fp, slots, sp, frame;
  asm_label_t slow;
} opt_cg_frame_regs_t;

static void opt_cg_frame_this(opt_cg_t *cg, const opt_cg_frame_regs_t *r) {
  masm_t *m = &cg->masm;
  sv_func_t *func = cg->graph->func;
  masm_load64(m, r->value, opt_cg_entry_field(cg, offsetof(opt_entry_t, this_val)));
  if (func->is_strict || func->is_arrow) return;
  asm_label_t this_ok = masm_new_label(m), global = masm_new_label(m);
  masm_branch64_imm(m, ASM_COND_EQUAL, r->value, (int64_t)IR_VALUE_UNDEFINED, global);
  masm_branch64_imm(m, ASM_COND_EQUAL, r->value, (int64_t)IR_VALUE_NULL, global);
  masm_move_imm64(m, r->s, NANBOX_PREFIX);
  masm_branch64(m, ASM_COND_BELOW_OR_EQUAL, r->value, r->s, r->slow);
  masm_shr64_imm(m, r->s, r->value, NANBOX_TYPE_SHIFT);
  masm_and32_imm(m, r->s, r->s, NANBOX_TYPE_MASK);
  masm_move_imm32(m, r->frame, T_OBJECT_MASK | (1u << kTypeBuiltin));
  masm_shr32(m, r->frame, r->frame, r->s);
  masm_branch_test32(m, ASM_COND_ZERO, r->frame, 1, r->slow);
  masm_jump(m, this_ok);
  masm_bind(m, global);
  masm_load64(m, r->s, asm_addr(r->vm, (int32_t)offsetof(sv_vm_t, js)));
  masm_load64(m, r->value, asm_addr(r->s, (int32_t)offsetof(ant_t, global_this)));
  masm_bind(m, this_ok);
}

static void opt_cg_frame_checks(opt_cg_t *cg, const opt_cg_frame_regs_t *r) {
  masm_t *m = &cg->masm;
  sv_func_t *func = cg->graph->func;
  uint32_t params = (uint32_t)func->param_count;
  int64_t reserve = (int64_t)func->max_locals + (func->max_stack > 0 ? func->max_stack : 0);
  masm_load64(m, r->closure, opt_cg_entry_field(cg, offsetof(opt_entry_t, closure)));
  masm_load64(m, r->s, asm_addr(r->vm, (int32_t)offsetof(sv_vm_t, js)));
  const int32_t closures = (int32_t)offsetof(ant_t, closure_arena);
  masm_load64(m, r->value, asm_addr(r->s, closures + (int32_t)offsetof(ant_fixed_arena_t, table_base)));
  masm_sub64(m, r->value, r->closure, r->value);
  masm_shr64_imm(m, r->value, r->value, ANT_ARENA_CHUNK_SHIFT);
  masm_branch64_memory(m, ASM_COND_BELOW_OR_EQUAL, asm_addr(r->s, closures + (int32_t)offsetof(ant_fixed_arena_t, table_slots)), r->value, r->slow);
  masm_load32(m, r->fp, asm_addr(r->s, closures + (int32_t)offsetof(ant_fixed_arena_t, id)));
  masm_load64(m, r->s, asm_addr(r->s, closures + (int32_t)offsetof(ant_fixed_arena_t, owners)));
  masm_load32(m, r->value, asm_addr_indexed(r->s, r->value, ASM_SCALE_4, 0));
  masm_and32_imm(m, r->value, r->value, (int32_t)~ANT_ARENA_PARTIAL_CHUNK);
  masm_branch32(m, ASM_COND_NOT_EQUAL, r->value, r->fp, r->slow);
  masm_load32(m, r->s, asm_addr(r->closure, (int32_t)offsetof(sv_closure_t, call_flags)));
  masm_branch_test32(m, ASM_COND_NON_ZERO, r->s, SV_CALL_HAS_EVAL_ENV, r->slow);
  opt_cg_frame_this(cg, r);
  masm_load32(m, r->fp, asm_addr(r->vm, (int32_t)offsetof(sv_vm_t, fp)));
  masm_add32_imm(m, r->fp, r->fp, 1);
  masm_load32(m, r->s, asm_addr(r->vm, (int32_t)offsetof(sv_vm_t, max_frames)));
  masm_branch32(m, ASM_COND_GREATER_THAN_OR_EQUAL, r->fp, r->s, r->slow);
  masm_load64(m, r->slots, opt_cg_entry_field(cg, offsetof(opt_entry_t, argc)));
  if (params) {
    asm_label_t enough = masm_new_label(m);
    masm_branch64_imm(m, ASM_COND_GREATER_THAN_OR_EQUAL, r->slots, params, enough);
    masm_move_imm64(m, r->slots, params);
    masm_bind(m, enough);
  }
  masm_load32(m, r->sp, asm_addr(r->vm, (int32_t)offsetof(sv_vm_t, sp)));
  masm_add64(m, r->s, r->sp, r->slots);
  masm_add64_imm(m, r->s, r->s, reserve);
  masm_load32(m, r->frame, asm_addr(r->vm, (int32_t)offsetof(sv_vm_t, stack_size)));
  masm_branch64(m, ASM_COND_GREATER_THAN, r->s, r->frame, r->slow);
}

static void opt_cg_frame_push(opt_cg_t *cg, const opt_cg_frame_regs_t *r) {
  masm_t *m = &cg->masm;
  uint32_t locals = (uint32_t)cg->graph->func->max_locals;
  masm_load64(m, r->frame, asm_addr(r->vm, (int32_t)offsetof(sv_vm_t, frames)));
  masm_mul32_imm(m, r->s, r->fp, (uint32_t)sizeof(sv_frame_t));
  masm_add64(m, r->frame, r->frame, r->s);
  masm_store64(m, asm_addr(r->frame, (int32_t)offsetof(sv_frame_t, this)), r->value);
  masm_move_imm64(m, r->s, ant_cage_base());
  masm_sub64(m, r->value, r->closure, r->s);
  masm_move_imm64(m, r->s, NANBOX_PREFIX | ((uint64_t)kTypeFunction << NANBOX_TYPE_SHIFT));
  masm_or64(m, r->value, r->value, r->s);
  masm_store64(m, asm_addr(r->frame, (int32_t)offsetof(sv_frame_t, callee)), r->value);
  masm_load64(m, r->value, asm_addr(r->closure, (int32_t)offsetof(sv_closure_t, upvalues)));
  masm_store64(m, asm_addr(r->frame, (int32_t)offsetof(sv_frame_t, upvalues)), r->value);
  masm_store32(m, asm_addr(r->frame, (int32_t)offsetof(sv_frame_t, prev_sp)), r->sp);
  masm_load64(m, r->closure, asm_addr(r->vm, (int32_t)offsetof(sv_vm_t, stack)));
  masm_shl64_imm(m, r->s, r->sp, 3);
  masm_add64(m, r->closure, r->closure, r->s);
  masm_store64(m, asm_addr(r->frame, (int32_t)offsetof(sv_frame_t, bp)), r->closure);
  masm_add64(m, r->s, r->sp, r->slots);
  masm_add32_imm(m, r->s, r->s, (int32_t)locals);
  masm_store32(m, asm_addr(r->vm, (int32_t)offsetof(sv_vm_t, sp)), r->s);
  masm_store32(m, asm_addr(r->vm, (int32_t)offsetof(sv_vm_t, fp)), r->fp);
  masm_store32(m, opt_cg_entry_field(cg, offsetof(opt_entry_t, seg) + offsetof(gc_vm_seg_t, entry_vm_fp)), r->fp);
}

static void opt_cg_frame_copy_arguments(opt_cg_t *cg, const opt_cg_frame_regs_t *r) {
  masm_t *m = &cg->masm;
  uint32_t params = (uint32_t)cg->graph->func->param_count;
  asm_gpr_t bp = r->closure, cursor = r->sp, end = r->value, source = r->fp, undefined = r->fp;
  asm_label_t copy = masm_new_label(m), copied = masm_new_label(m);
  masm_load64(m, source, opt_cg_entry_field(cg, offsetof(opt_entry_t, args)));
  masm_load64(m, r->s, opt_cg_entry_field(cg, offsetof(opt_entry_t, argc)));
  masm_shl64_imm(m, r->s, r->s, 3);
  masm_add64(m, end, bp, r->s);
  masm_move64(m, cursor, bp);
  masm_bind(m, copy);
  masm_branch64(m, ASM_COND_ABOVE_OR_EQUAL, cursor, end, copied);
  masm_load64(m, r->s, asm_addr(source, 0));
  masm_store64(m, asm_addr(cursor, 0), r->s);
  masm_add64_imm(m, source, source, 8);
  masm_add64_imm(m, cursor, cursor, 8);
  masm_jump(m, copy);
  masm_bind(m, copied);
  masm_move_imm64(m, undefined, IR_VALUE_UNDEFINED);
  if (!params) return;
  asm_label_t pad = masm_new_label(m), padded = masm_new_label(m);
  masm_add64_imm(m, end, bp, 8 * (int64_t)params);
  masm_bind(m, pad);
  masm_branch64(m, ASM_COND_ABOVE_OR_EQUAL, cursor, end, padded);
  masm_store64(m, asm_addr(cursor, 0), undefined);
  masm_add64_imm(m, cursor, cursor, 8);
  masm_jump(m, pad);
  masm_bind(m, padded);
}

static void opt_cg_frame_fill_locals(opt_cg_t *cg, const opt_cg_frame_regs_t *r) {
  masm_t *m = &cg->masm;
  uint32_t locals = (uint32_t)cg->graph->func->max_locals;
  asm_gpr_t bp = r->closure, cursor = r->sp, end = r->value, undefined = r->fp, lp = r->slots;
  masm_shl64_imm(m, r->s, r->slots, 3);
  masm_add64(m, lp, bp, r->s);
  masm_store64(m, asm_addr(r->frame, (int32_t)offsetof(sv_frame_t, lp)), lp);
  if (locals <= 16) {
    for (uint32_t j = 0; j < locals; j++) masm_store64(m, asm_addr(lp, (int32_t)(8 * j)), undefined);
    return;
  }
  asm_label_t fill = masm_new_label(m);
  masm_move64(m, cursor, lp);
  masm_add64_imm(m, end, lp, 8 * (int64_t)locals);
  masm_bind(m, fill);
  masm_store64(m, asm_addr(cursor, 0), undefined);
  masm_add64_imm(m, cursor, cursor, 8);
  masm_branch64(m, ASM_COND_BELOW, cursor, end, fill);
}

static void opt_cg_frame_fields(opt_cg_t *cg, const opt_cg_frame_regs_t *r) {
  masm_t *m = &cg->masm;
  sv_func_t *func = cg->graph->func;
  asm_gpr_t frame = r->frame, s = r->s, undefined = r->fp;
  masm_move_imm64(m, s, (uint64_t)(uintptr_t)func->code);
  masm_store64(m, asm_addr(frame, (int32_t)offsetof(sv_frame_t, ip)), s);
  masm_move_imm64(m, s, (uint64_t)(uintptr_t)func);
  masm_store64(m, asm_addr(frame, (int32_t)offsetof(sv_frame_t, func)), s);
  masm_load64(m, s, opt_cg_entry_field(cg, offsetof(opt_entry_t, new_target)));
  masm_store64(m, asm_addr(frame, (int32_t)offsetof(sv_frame_t, new_target)), s);
  masm_load64(m, s, opt_cg_entry_field(cg, offsetof(opt_entry_t, super_val)));
  masm_store64(m, asm_addr(frame, (int32_t)offsetof(sv_frame_t, super_val)), s);
  masm_load64(m, s, opt_cg_entry_field(cg, offsetof(opt_entry_t, argc)));
  masm_store32(m, asm_addr(frame, (int32_t)offsetof(sv_frame_t, argc)), s);
  masm_store_imm32(m, asm_addr(frame, (int32_t)(offsetof(sv_frame_t, completion) + offsetof(sv_completion_t, kind))), SV_COMPLETION_NONE);
  masm_store64(m, asm_addr(frame, (int32_t)(offsetof(sv_frame_t, completion) + offsetof(sv_completion_t, value))), undefined);
  masm_store_imm32(m, asm_addr(frame, (int32_t)offsetof(sv_frame_t, upvalue_count)), (uint32_t)func->upvalue_count);
  masm_load32(m, s, asm_addr(r->vm, (int32_t)offsetof(sv_vm_t, handler_depth)));
  masm_store16(m, asm_addr(frame, (int32_t)offsetof(sv_frame_t, handler_base)), s);
  masm_store16(m, asm_addr(frame, (int32_t)offsetof(sv_frame_t, handler_top)), s);
  masm_store64(m, asm_addr(frame, (int32_t)offsetof(sv_frame_t, arguments_obj)), undefined);
  masm_store64(m, asm_addr(frame, (int32_t)offsetof(sv_frame_t, eval_env)), undefined);
}

static void opt_cg_frame_link_segment(opt_cg_t *cg, const opt_cg_frame_regs_t *r) {
  masm_t *m = &cg->masm;
  asm_gpr_t s = r->s, end = r->value, bp = r->closure, lp = r->slots;
  size_t seg = offsetof(opt_entry_t, seg);
  masm_load64(m, s, asm_addr(r->vm, (int32_t)offsetof(sv_vm_t, js)));
  masm_load64(m, end, asm_addr(s, (int32_t)offsetof(ant_t, vm_segs)));
  masm_store64(m, opt_cg_entry_field(cg, seg + offsetof(gc_vm_seg_t, prev)), end);
  masm_store_imm64(m, opt_cg_entry_field(cg, seg + offsetof(gc_vm_seg_t, lo)), 0);
  masm_store_imm64(m, opt_cg_entry_field(cg, seg + offsetof(gc_vm_seg_t, hi)), 0);
  masm_store_imm64(m, opt_cg_entry_field(cg, seg + offsetof(gc_vm_seg_t, fp)), 0);
  masm_store_imm32(m, opt_cg_entry_field(cg, seg + offsetof(gc_vm_seg_t, jit_depth)), UINT32_MAX);
  masm_load_effective_address(m, end, opt_cg_entry_field(cg, seg));
  masm_store64(m, asm_addr(s, (int32_t)offsetof(ant_t, vm_segs)), end);
  masm_store64(m, opt_cg_entry_field(cg, offsetof(opt_entry_t, frame)), r->frame);
  masm_store64(m, opt_cg_entry_field(cg, offsetof(opt_entry_t, locals)), lp);
  masm_store64(m, opt_cg_entry_field(cg, offsetof(opt_entry_t, params)), bp);
  masm_move_imm32(m, s, 1);
  masm_store8(m, opt_cg_entry_field(cg, offsetof(opt_entry_t, owns_frame)), s);
  masm_move64(m, cg->pinned.frame, bp);
}

static void opt_cg_initialize_frame(opt_cg_t *cg, bool ensure) {
  masm_t *m = &cg->masm;
  asm_label_t done = masm_new_label(m), slow = masm_new_label(m);
  if (ensure) {
    masm_load64(m, cg->pinned.scratch, opt_cg_entry_field(cg, offsetof(opt_entry_t, frame)));
    masm_branch64_imm(m, ASM_COND_NOT_EQUAL, cg->pinned.scratch, 0, done);
  }
  asm_gpr_t gprs[6];
  if (opt_cg_free_gprs(gprs, 6) < 6) {
    opt_cg_fail(cg, "frame-registers");
    return;
  }
  opt_cg_frame_regs_t regs = {
    .vm = cg->pinned.vm, .s = cg->pinned.scratch, .slow = slow,
    .closure = gprs[0], .value = gprs[1], .fp = gprs[2], .slots = gprs[3], .sp = gprs[4], .frame = gprs[5],
  };
  opt_cg_frame_checks(cg, &regs);
  opt_cg_frame_push(cg, &regs);
  opt_cg_frame_copy_arguments(cg, &regs);
  opt_cg_frame_fill_locals(cg, &regs);
  opt_cg_frame_fields(cg, &regs);
  opt_cg_frame_link_segment(cg, &regs);
  masm_jump(m, done);

  masm_bind(m, slow);
  opt_cg_call_frame_helper(cg, (uint64_t)(uintptr_t)opt_rt_enter_frame);
  masm_branch64_imm(m, ASM_COND_NOT_EQUAL, MASM_RETURN_GPRS[0], 0, cg->throw_label);
  masm_load64(m, cg->pinned.frame, opt_cg_entry_field(cg, offsetof(opt_entry_t, params)));
  masm_bind(m, done);
}

static void opt_cg_count_slow_path(opt_cg_t *cg, sv_func_t *func, uint32_t bc_off) {
  masm_t *m = &cg->masm;
  asm_label_t counted = masm_new_label(m);
  opt_cg_arg_t args[3] = {
    {.constant = true, .bits = (uint64_t)(uintptr_t)cg->code},
    {.constant = true, .bits = (uint64_t)(uintptr_t)func},
    {.constant = true, .bits = bc_off},
  };
  asm_label_t hot = opt_cg_out_of_line(cg, counted, (uint64_t)(uintptr_t)opt_rt_slow_paths_hot, args, 3);
  masm_move_imm64(m, cg->pinned.scratch, (uint64_t)(uintptr_t)&cg->code->slow_path_runs);
  masm_add64_to_memory_imm(m, asm_addr(cg->pinned.scratch, 0), 1);
  masm_branch64_memory_imm(m, ASM_COND_EQUAL, asm_addr(cg->pinned.scratch, 0), (int64_t)OPT_SLOW_PATH_LIMIT, hot);
  masm_bind(m, counted);
}

static void opt_cg_generic(opt_cg_t *cg, ir_node_id_t node_id) {
  masm_t *m = &cg->masm;
  const ir_op_t *op = &ir_node(cg->graph, node_id)->op;
  if (ir_block(cg->graph, cg->block)->is_cold) opt_cg_count_slow_path(cg, op->slow.func, op->slow.bc_off);
  masm_move64(m, MASM_ARGUMENT_GPRS[0], cg->pinned.vm);
  masm_load64(m, MASM_ARGUMENT_GPRS[1], opt_cg_entry_field(cg, offsetof(opt_entry_t, frame)));
  masm_move_imm64(m, MASM_ARGUMENT_GPRS[2], (uint64_t)(uintptr_t)(op->slow.func->code + op->slow.bc_off));
  masm_move_imm32(m, MASM_ARGUMENT_GPRS[3], op->slow.depth);
  masm_call_absolute(m, opt_tier_generic_step());
  opt_cg_record_call(cg, IR_NONE, (int32_t)op->slow.bc_off);
  opt_cg_generic_t generic = {.label = masm_new_label(m), .exit = opt_cg_exit_site(cg, node_id), .thrown = opt_cg_throw_target(cg, node_id, cg->leave)};
  masm_branch64_imm(m, ASM_COND_NOT_EQUAL, MASM_RETURN_GPRS[0], 0, generic.label);
  opt_vec_push(&cg->arena, &cg->generics, generic);
}

static void opt_cg_return(opt_cg_t *cg) {
  masm_emit_epilogue(&cg->masm, &cg->frame);
  masm_ret(&cg->masm);
}

static void opt_cg_return_from_frame(opt_cg_t *cg) {
  if (cg->has_frame) {
    masm_load64(&cg->masm, cg->pinned.scratch, opt_cg_entry_field(cg, offsetof(opt_entry_t, frame)));
    masm_branch64_imm(&cg->masm, ASM_COND_NOT_EQUAL, cg->pinned.scratch, 0, cg->leave);
  }
  opt_cg_return(cg);
}

static void opt_cg_float64_to_int32(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  asm_fpr_t input = opt_cg_float_input(cg, node, 0);
  asm_gpr_t output = opt_cg_output(cg, node);
  if (masm_has_js_convert()) {
    masm_js_convert_double_to_int32(m, output, input);
    return;
  }
  asm_label_t large = masm_new_label(m), done = masm_new_label(m);
  masm_truncate_double_to_int64(m, output, input);
  masm_branch64_imm(m, ASM_COND_EQUAL, output, INT64_MIN, large);
  masm_branch64_imm(m, ASM_COND_EQUAL, output, INT64_MAX, large);
  masm_move32(m, output, output);
  masm_jump(m, done);
  masm_bind(m, large);
  asm_fpr_t multiple = opt_cg_lowering_fpr(0), constant = opt_cg_lowering_fpr(1);
  masm_move_double_imm(m, constant, 0x1p-32);
  masm_mul_double(m, multiple, input, constant);
  masm_floor_double(m, multiple, multiple);
  masm_move_double_imm(m, constant, 0x1p32);
  masm_mul_double(m, multiple, multiple, constant);
  masm_sub_double(m, multiple, input, multiple);
  masm_truncate_double_to_int64(m, output, multiple);
  masm_move32(m, output, output);
  masm_bind(m, done);
}

static void opt_cg_uint32_shift_right(opt_cg_t *cg, ir_node_id_t node) {
  uint64_t bits;
  asm_gpr_t lhs = opt_cg_input(cg, node, 0), output = opt_cg_output(cg, node);
  if (opt_cg_input_constant(cg, node, 1, &bits)) masm_shr32_imm(&cg->masm, output, lhs, (uint8_t)(bits & 31));
  else masm_shr32(&cg->masm, output, lhs, opt_cg_input(cg, node, 1));
}

static void opt_cg_nullish_distance(opt_cg_t *cg, asm_gpr_t value) {
  static_assert(kTypeNull == kTypeUndefined + 1);
  masm_shr64_imm(&cg->masm, cg->pinned.scratch, value, NANBOX_TYPE_SHIFT);
  masm_sub64_imm(&cg->masm, cg->pinned.scratch, cg->pinned.scratch, (int64_t)OPT_UNDEFINED_TAG);
}

static void opt_cg_is_nullish(opt_cg_t *cg, ir_node_id_t node, bool equal) {
  opt_cg_nullish_distance(cg, opt_cg_input(cg, node, 0));
  asm_cond_t cond = equal ? ASM_COND_BELOW_OR_EQUAL : ASM_COND_ABOVE;
  masm_compare64_imm_set(&cg->masm, cond, opt_cg_output(cg, node), cg->pinned.scratch, 1);
}

static void opt_cg_has_in_prototype_chain(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  uint64_t proto = ir_node(cg->graph, node)->op.bits;
  asm_gpr_t value = opt_cg_input(cg, node, 0), output = opt_cg_output(cg, node), current = opt_cg_temp(cg, node, 0);
  asm_gpr_t scratch = cg->pinned.scratch;
  asm_label_t exit = opt_cg_exit_site(cg, node), walk = masm_new_label(m), step = masm_new_label(m);
  asm_label_t yes = masm_new_label(m), no = masm_new_label(m), done = masm_new_label(m);
  masm_move_imm64(m, scratch, NANBOX_PREFIX);
  masm_branch64(m, ASM_COND_BELOW_OR_EQUAL, value, scratch, no);
  masm_shr64_imm(m, scratch, value, NANBOX_TYPE_SHIFT);
  masm_branch64_imm(m, ASM_COND_EQUAL, scratch, (int64_t)OPT_OBJECT_TAG, walk);
  masm_branch64_imm(m, ASM_COND_EQUAL, scratch, (int64_t)OPT_ARRAY_TAG, walk);
  masm_branch64_imm(m, ASM_COND_EQUAL, scratch, (int64_t)OPT_FUNCTION_TAG, exit);
  masm_branch64_imm(m, ASM_COND_EQUAL, scratch, (int64_t)((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | kTypePromise), exit);
  masm_branch64_imm(m, ASM_COND_EQUAL, scratch, (int64_t)((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | kTypeGenerator), exit);
  masm_jump(m, no);
  masm_bind(m, walk);
  masm_move_imm32(m, output, 32);
  masm_move64(m, current, value);
  masm_bind(m, step);
  masm_and64_imm(m, current, current, NANBOX_DATA_MASK);
  masm_add64_imm(m, current, current, (int64_t)ant_cage_base());
  masm_load16(m, scratch, asm_addr(current, (int32_t)offsetof(ant_object_t, flags)));
  masm_branch_test32(m, ASM_COND_NON_ZERO, scratch, ANT_OBJECT_FLAG_EXOTIC, exit);
  masm_load64(m, current, asm_addr(current, (int32_t)offsetof(ant_object_t, proto)));
  masm_move_imm64(m, scratch, proto);
  masm_branch64(m, ASM_COND_EQUAL, current, scratch, yes);
  masm_shr64_imm(m, scratch, current, NANBOX_TYPE_SHIFT);
  masm_branch64_imm(m, ASM_COND_EQUAL, scratch, (int64_t)OPT_NULL_TAG, no);
  masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, (int64_t)OPT_OBJECT_TAG, exit);
  masm_sub32_imm(m, output, output, 1);
  masm_branch32_imm(m, ASM_COND_NOT_EQUAL, output, 0, step);
  masm_jump(m, exit);
  masm_bind(m, yes);
  masm_move_imm32(m, output, 1);
  masm_jump(m, done);
  masm_bind(m, no);
  masm_move_imm32(m, output, 0);
  masm_bind(m, done);
}

static void opt_cg_typeof_is_object(opt_cg_t *cg, asm_gpr_t input, asm_gpr_t output, bool object) {
  masm_t *m = &cg->masm;
  asm_gpr_t scratch = cg->pinned.scratch;
  static const uint8_t object_tags[] = {kTypeNull, kTypeArray, kTypePromise, kTypeGenerator};
  static const uint8_t function_tags[] = {kTypeFunction, kTypeBuiltin};
  const uint8_t *tags = object ? object_tags : function_tags;
  size_t tag_count = object ? sizeof object_tags : sizeof function_tags;
  asm_label_t yes = masm_new_label(m), no = masm_new_label(m), done = masm_new_label(m);
  masm_move_imm64(m, scratch, NANBOX_PREFIX);
  masm_branch64(m, ASM_COND_BELOW_OR_EQUAL, input, scratch, no);
  masm_shr64_imm(m, scratch, input, NANBOX_TYPE_SHIFT);
  for (size_t i = 0; i < tag_count; i++)
    masm_branch64_imm(m, ASM_COND_EQUAL, scratch, (int64_t)((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | tags[i]), yes);
  masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, (int64_t)OPT_OBJECT_TAG, no);
  masm_and64_imm(m, scratch, input, NANBOX_DATA_MASK);
  masm_add64_imm(m, scratch, scratch, (int64_t)ant_cage_base());
  masm_load16(m, scratch, asm_addr(scratch, (int32_t)offsetof(ant_object_t, flags)));
  masm_branch_test32(m, object ? ASM_COND_NON_ZERO : ASM_COND_ZERO, scratch, ANT_OBJECT_FLAG_CALLABLE, no);
  masm_bind(m, yes);
  masm_move_imm32(m, output, 1);
  masm_jump(m, done);
  masm_bind(m, no);
  masm_move_imm32(m, output, 0);
  masm_bind(m, done);
}

static void opt_cg_typeof_is(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  const ir_op_t *op = &ir_node(cg->graph, node)->op;
  asm_gpr_t input = opt_cg_input(cg, node, 0), output = opt_cg_output(cg, node), scratch = cg->pinned.scratch;
  static const uint8_t types[] = {
    [IR_TYPEOF_NUMBER] = kTypeNumber, [IR_TYPEOF_UNDEFINED] = kTypeUndefined, [IR_TYPEOF_STRING] = kTypeString,
    [IR_TYPEOF_SYMBOL] = kTypeSymbol, [IR_TYPEOF_BOOLEAN] = kTypeBool, [IR_TYPEOF_BIGINT] = kTypeBigInt,
  };
  if (op->typeof_is.kind == IR_TYPEOF_OBJECT || op->typeof_is.kind == IR_TYPEOF_FUNCTION) {
    opt_cg_typeof_is_object(cg, input, output, op->typeof_is.kind == IR_TYPEOF_OBJECT);
    if (!op->typeof_is.equal) masm_xor32_imm(m, output, output, 1);
    return;
  }
  if (op->typeof_is.kind >= sizeof types || (!types[op->typeof_is.kind] && op->typeof_is.kind != IR_TYPEOF_NUMBER)) {
    opt_cg_fail(cg, "typeof-kind");
    return;
  }
  if (op->typeof_is.kind == IR_TYPEOF_NUMBER) {
    masm_move_imm64(m, scratch, NANBOX_PREFIX);
    masm_compare64_set(m, ASM_COND_BELOW_OR_EQUAL, output, input, scratch);
  } else {
    masm_shr64_imm(m, scratch, input, NANBOX_TYPE_SHIFT);
    masm_compare64_imm_set(m, ASM_COND_EQUAL, output, scratch, (int64_t)((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | types[op->typeof_is.kind]));
  }
  if (!op->typeof_is.equal) masm_xor32_imm(m, output, output, 1);
}

static void opt_cg_loose_identity_equals(opt_cg_t *cg, ir_node_id_t node, bool equal) {
  masm_t *m = &cg->masm;
  asm_gpr_t lhs = opt_cg_input(cg, node, 0), rhs = opt_cg_input(cg, node, 1), output = opt_cg_output(cg, node);
  asm_label_t same = masm_new_label(m), differ = masm_new_label(m), done = masm_new_label(m);
  masm_branch64(m, ASM_COND_EQUAL, lhs, rhs, same);
  opt_cg_nullish_distance(cg, lhs);
  masm_branch64_imm(m, ASM_COND_ABOVE, cg->pinned.scratch, 1, differ);
  opt_cg_nullish_distance(cg, rhs);
  masm_branch64_imm(m, ASM_COND_ABOVE, cg->pinned.scratch, 1, differ);
  masm_bind(m, same);
  masm_move_imm32(m, output, equal ? 1 : 0);
  masm_jump(m, done);
  masm_bind(m, differ);
  masm_move_imm32(m, output, equal ? 0 : 1);
  masm_bind(m, done);
}

static void opt_cg_load_upvalue_cell(opt_cg_t *cg, ir_node_id_t node, uint32_t index) {
  masm_t *m = &cg->masm;
  asm_gpr_t output = opt_cg_output(cg, node);
  uint64_t bits;
  if (!ir_node(cg->graph, node)->inputs.len) masm_load64(m, output, opt_cg_entry_field(cg, offsetof(opt_entry_t, closure)));
  else if (opt_cg_input_constant(cg, node, 0, &bits)) masm_move_imm64(m, output, (uint64_t)(uintptr_t)js_func_closure(bits));
  else {
    masm_and64_imm(m, output, opt_cg_input(cg, node, 0), NANBOX_DATA_MASK);
    masm_add64_imm(m, output, output, (int64_t)ant_cage_base());
  }
  masm_load64(m, output, asm_addr(output, (int32_t)offsetof(sv_closure_t, upvalues)));
  masm_load64(m, output, asm_addr(output, (int32_t)(index * sizeof(sv_upvalue_t *))));
}

static void opt_cg_load_upvalue(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  const ir_op_t *op = &ir_node(cg->graph, node)->op;
  bool load = op->code == IR_OP_LOAD_UPVALUE, checked = !load || !op->initialized;
  asm_label_t exit = checked ? opt_cg_exit_site(cg, node) : 0;
  asm_gpr_t output = load ? opt_cg_output(cg, node) : opt_cg_temp(cg, node, 0);
  masm_load64(m, output, asm_addr(opt_cg_input(cg, node, 0), (int32_t)offsetof(sv_upvalue_t, location)));
  masm_load64(m, output, asm_addr(output, 0));
  if (checked) masm_branch64_imm(m, ASM_COND_EQUAL, output, (int64_t)SV_TDZ, exit);
}

static void opt_cg_store_upvalue(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  asm_gpr_t cell = opt_cg_input(cg, node, 0), value = opt_cg_input(cg, node, 1), location = opt_cg_temp(cg, node, 0);
  asm_gpr_t scratch = cg->pinned.scratch;
  if (ir_node(cg->graph, node)->op.code == IR_OP_STORE_ENVIRONMENT_BINDING)
    masm_store64(m, asm_addr(cell, (int32_t)offsetof(sv_upvalue_t, closed)), value);
  else {
    masm_load64(m, location, asm_addr(cell, (int32_t)offsetof(sv_upvalue_t, location)));
    masm_store64(m, asm_addr(location, 0), value);
  }
  asm_label_t done = masm_new_label(m);
  opt_cg_arg_t args[2] = {opt_cg_reg_arg(cell), opt_cg_reg_arg(value)};
  asm_label_t call = opt_cg_out_of_line(cg, done, (uint64_t)(uintptr_t)opt_rt_upvalue_barrier, args, 2);
  masm_load16(m, scratch, asm_addr(cell, (int32_t)offsetof(sv_upvalue_t, in_remember_set)));
  masm_branch32_imm(m, ASM_COND_EQUAL, scratch, 1, done);
  masm_branch32_imm(m, ASM_COND_NOT_EQUAL, scratch, 0, call);
  masm_move_imm64(m, scratch, NANBOX_PREFIX);
  masm_branch64(m, ASM_COND_BELOW_OR_EQUAL, value, scratch, done);
  masm_branch64_memory_imm(m, ASM_COND_NOT_EQUAL, asm_addr(cell, (int32_t)offsetof(sv_upvalue_t, gc_epoch)), 0, call);
  masm_bind(m, done);
}

static void opt_cg_check_global_lexicals(opt_cg_t *cg, ir_node_id_t node, uint32_t count) {
  masm_t *m = &cg->masm;
  asm_label_t exit = opt_cg_exit_site(cg, node);
  masm_load64(m, cg->pinned.scratch, asm_addr(cg->pinned.vm, (int32_t)offsetof(sv_vm_t, js)));
  masm_branch32_memory_imm(m, ASM_COND_NOT_EQUAL, asm_addr(cg->pinned.scratch, (int32_t)offsetof(ant_t, global_lexical_count)), (int32_t)count, exit);
}

static void opt_cg_elements_kind(opt_cg_t *cg, ir_node_id_t node, ir_elements_kind_t kind, asm_label_t exit) {
  masm_t *m = &cg->masm;
  asm_gpr_t value = opt_cg_input(cg, node, 0), address = opt_cg_temp(cg, node, 0), scratch = cg->pinned.scratch;
  if (ir_elements_is_typed(kind)) {
    opt_cg_fail(cg, "typed-elements");
    return;
  }
  uint32_t mask = ANT_OBJECT_FLAG_EXOTIC | ANT_OBJECT_FLAG_FAST_ARRAY | ANT_OBJECT_FLAG_DENSE_LENGTH_FITS | ANT_OBJECT_FLAG_ARGUMENTS;
  if (kind == IR_ELEMENTS_PACKED) mask |= ANT_OBJECT_FLAG_FROZEN;
  masm_shr64_imm(m, scratch, value, NANBOX_TYPE_SHIFT);
  masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, (int64_t)OPT_ARRAY_TAG, exit);
  masm_and64_imm(m, address, value, NANBOX_DATA_MASK);
  masm_add64_imm(m, address, address, (int64_t)ant_cage_base());
  masm_load16(m, scratch, asm_addr(address, (int32_t)offsetof(ant_object_t, flags)));
  masm_and32_imm(m, scratch, scratch, mask);
  masm_branch32_imm(m, ASM_COND_NOT_EQUAL, scratch, ANT_OBJECT_FLAG_FAST_ARRAY | ANT_OBJECT_FLAG_DENSE_LENGTH_FITS, exit);
  masm_branch64_memory_imm(m, ASM_COND_EQUAL, asm_addr(address, (int32_t)offsetof(ant_object_t, u.array.data)), 0, exit);
}

static void opt_cg_check_elements(opt_cg_t *cg, ir_node_id_t node, ir_elements_kind_t kind) {
  opt_cg_elements_kind(cg, node, kind, opt_cg_exit_site(cg, node));
}

static void opt_cg_check_bounds(opt_cg_t *cg, ir_node_id_t node) {
  asm_label_t exit = opt_cg_exit_site(cg, node);
  asm_gpr_t length = opt_cg_input(cg, node, 1);
  uint64_t bits;
  if (opt_cg_input_constant(cg, node, 0, &bits)) masm_branch32_imm(&cg->masm, ASM_COND_BELOW_OR_EQUAL, length, (int32_t)(uint32_t)bits, exit);
  else masm_branch32(&cg->masm, ASM_COND_ABOVE_OR_EQUAL, opt_cg_input(cg, node, 0), length, exit);
}

static asm_address_t opt_cg_element(const opt_cg_t *cg, ir_node_id_t node, asm_gpr_t data) {
  uint64_t bits;
  if (opt_cg_input_constant(cg, node, 1, &bits)) return asm_addr(data, (int32_t)(8 * (uint32_t)bits));
  return asm_addr_indexed(data, opt_cg_input(cg, node, 1), ASM_SCALE_8, 0);
}

static void opt_cg_load_element(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  asm_gpr_t data = opt_cg_temp(cg, node, 0);
  masm_load64(m, data, asm_addr(opt_cg_input(cg, node, 0), (int32_t)offsetof(ant_object_t, u.array.data)));
  masm_load64(m, opt_cg_output(cg, node), opt_cg_element(cg, node, data));
}

static void opt_cg_store_element(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  asm_gpr_t object = opt_cg_input(cg, node, 0), value = opt_cg_input(cg, node, 2), data = opt_cg_temp(cg, node, 0);
  masm_load64(m, data, asm_addr(object, (int32_t)offsetof(ant_object_t, u.array.data)));
  masm_store64(m, opt_cg_element(cg, node, data), value);
  if (!opt_cg_needs_barrier(cg, ir_node(cg->graph, node)->inputs.items[2])) return;
  opt_cg_arg_t index = opt_cg_input_arg(cg, node, 1);
  opt_cg_object_barrier(cg, object, value, (uint64_t)(uintptr_t)opt_rt_element_barrier, &index);
}

static void opt_cg_can_add_element(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  asm_gpr_t array = opt_cg_input(cg, node, 0), output = opt_cg_output(cg, node), scratch = cg->pinned.scratch;
  asm_label_t fail = masm_new_label(m), done = masm_new_label(m);
  if (ir_node(cg->graph, node)->op.bits) {
    masm_load64(m, scratch, asm_addr(array, (int32_t)offsetof(ant_object_t, shape)));
    masm_load8(m, scratch, asm_addr(scratch, (int32_t)ANT_SHAPE_INDEX_KEYS_OFFSET));
    masm_branch32_imm(m, ASM_COND_NOT_EQUAL, scratch, 0, fail);
  } else {
    masm_load16(m, scratch, asm_addr(array, (int32_t)offsetof(ant_object_t, flags)));
    masm_and32_imm(m, scratch, scratch, ANT_OBJECT_FLAG_EXTENSIBLE | ANT_OBJECT_FLAG_SEALED | ANT_OBJECT_FLAG_FROZEN | ANT_OBJECT_FLAG_EXOTIC);
    masm_branch32_imm(m, ASM_COND_NOT_EQUAL, scratch, ANT_OBJECT_FLAG_EXTENSIBLE, fail);
  }
  masm_load64(m, output, asm_addr(cg->pinned.vm, (int32_t)offsetof(sv_vm_t, js)));
  masm_load32(m, scratch, asm_addr(output, (int32_t)offsetof(ant_t, ic.epoch)));
  masm_load32(m, output, asm_addr(output, (int32_t)offsetof(ant_t, array_chain_plain_epoch)));
  masm_branch32(m, ASM_COND_NOT_EQUAL, output, scratch, fail);
  masm_load64(m, output, asm_addr(cg->pinned.vm, (int32_t)offsetof(sv_vm_t, js)));
  masm_load64(m, output, asm_addr(output, (int32_t)offsetof(ant_t, sym.array_proto)));
  masm_load64(m, scratch, asm_addr(array, (int32_t)offsetof(ant_object_t, proto)));
  masm_branch64(m, ASM_COND_NOT_EQUAL, output, scratch, fail);
  masm_and64_imm(m, output, output, NANBOX_DATA_MASK);
  masm_add64_imm(m, output, output, (int64_t)ant_cage_base());
  masm_branch32_memory_imm(m, ASM_COND_NOT_EQUAL, asm_addr(output, (int32_t)offsetof(ant_object_t, u.array.len)), 0, fail);
  masm_move_imm32(m, output, 1);
  masm_jump(m, done);
  masm_bind(m, fail);
  masm_move_imm32(m, output, 0);
  masm_bind(m, done);
}

static void opt_cg_pop_element(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  asm_gpr_t array = opt_cg_input(cg, node, 0), index = opt_cg_input(cg, node, 1);
  asm_gpr_t data = opt_cg_temp(cg, node, 0), scratch = cg->pinned.scratch;
  masm_load64(m, data, asm_addr(array, (int32_t)offsetof(ant_object_t, u.array.data)));
  masm_move_imm64(m, scratch, IR_VALUE_EMPTY);
  masm_store64(m, asm_addr_indexed(data, index, ASM_SCALE_8, 0), scratch);
  masm_store32(m, asm_addr(array, (int32_t)offsetof(ant_object_t, u.array.len)), index);
}

static void opt_cg_append_element(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  asm_gpr_t array = opt_cg_input(cg, node, 0), index = opt_cg_input(cg, node, 1), value = opt_cg_input(cg, node, 2);
  asm_gpr_t data = opt_cg_temp(cg, node, 0), scratch = cg->pinned.scratch;
  asm_label_t kept = masm_new_label(m);
  masm_load64(m, data, asm_addr(array, (int32_t)offsetof(ant_object_t, u.array.data)));
  masm_store64(m, asm_addr_indexed(data, index, ASM_SCALE_8, 0), value);
  masm_load32(m, scratch, asm_addr(array, (int32_t)offsetof(ant_object_t, u.array.len)));
  masm_branch32(m, ASM_COND_BELOW, index, scratch, kept);
  masm_add32_imm(m, scratch, index, 1);
  masm_store32(m, asm_addr(array, (int32_t)offsetof(ant_object_t, u.array.len)), scratch);
  masm_bind(m, kept);
  masm_load16(m, scratch, asm_addr(array, (int32_t)offsetof(ant_object_t, flags)));
  masm_or32_imm(m, scratch, scratch, ANT_OBJECT_FLAG_MAY_HAVE_DENSE_ELEMENTS);
  masm_store16(m, asm_addr(array, (int32_t)offsetof(ant_object_t, flags)), scratch);
  if (!opt_cg_needs_barrier(cg, ir_node(cg->graph, node)->inputs.items[2])) return;
  opt_cg_arg_t at = opt_cg_reg_arg(index);
  opt_cg_object_barrier(cg, array, value, (uint64_t)(uintptr_t)opt_rt_element_barrier, &at);
}

static void opt_cg_check_not_hole(opt_cg_t *cg, ir_node_id_t node) {
  asm_label_t exit = opt_cg_exit_site(cg, node);
  masm_branch64_imm(&cg->masm, ASM_COND_ABOVE_OR_EQUAL, opt_cg_input(cg, node, 0), (int64_t)ANT_SENTINEL_TAG, exit);
}

static void opt_cg_can_add_named(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  const ir_op_t *op = &ir_node(cg->graph, node)->op;
  asm_gpr_t object = opt_cg_input(cg, node, 1), temp = opt_cg_temp(cg, node, 0), output = opt_cg_output(cg, node);
  asm_gpr_t scratch = cg->pinned.scratch;
  asm_label_t fail = masm_new_label(m), done = masm_new_label(m);
  uint32_t slot = opt_named_index(op->add_named.offset);
  masm_load16(m, scratch, asm_addr(object, (int32_t)offsetof(ant_object_t, flags)));
  masm_and32_imm(m, scratch, scratch, ANT_OBJECT_FLAG_FROZEN | ANT_OBJECT_FLAG_SEALED | ANT_OBJECT_FLAG_EXTENSIBLE);
  masm_branch32_imm(m, ASM_COND_NOT_EQUAL, scratch, ANT_OBJECT_FLAG_EXTENSIBLE, fail);
  masm_load8(m, scratch, asm_addr(object, (int32_t)offsetof(ant_object_t, type_tag)));
  masm_branch32_imm(m, ASM_COND_EQUAL, scratch, kTypeArray, fail);
  masm_branch32_memory_imm(m, ASM_COND_NOT_EQUAL, asm_addr(object, (int32_t)offsetof(ant_object_t, prop_count)), (int32_t)slot, fail);
  uint32_t limit = opt_named_inobj_limit(op->add_named.offset);
  masm_load8(m, scratch, asm_addr(object, (int32_t)offsetof(ant_object_t, inobj_limit)));
  if (slot < limit) masm_branch32_imm(m, ASM_COND_BELOW_OR_EQUAL, scratch, (int32_t)slot, fail);
  else {
    masm_branch32_imm(m, ASM_COND_NOT_EQUAL, scratch, (int32_t)limit, fail);
    masm_load8(m, scratch, asm_addr(object, (int32_t)offsetof(ant_object_t, overflow_cap)));
    masm_branch32_imm(m, ASM_COND_BELOW, scratch, slot - limit < 4 ? 2 : 32 - __builtin_clz(slot - limit), fail);
  }
  masm_load64(m, scratch, asm_addr(object, (int32_t)offsetof(ant_object_t, proto)));
  if (op->add_named.proto & 1u) {
    masm_shr64_imm(m, temp, scratch, NANBOX_TYPE_SHIFT);
    masm_branch64_imm(m, ASM_COND_NOT_EQUAL, temp, (int64_t)OPT_OBJECT_TAG, fail);
    masm_and64_imm(m, scratch, scratch, NANBOX_DATA_MASK);
    masm_add64_imm(m, scratch, scratch, (int64_t)ant_cage_base());
    masm_branch32_memory_imm(m, ASM_COND_NOT_EQUAL, asm_addr(scratch, (int32_t)offsetof(ant_object_t, ic_identity)), (int32_t)(uint32_t)(op->add_named.proto >> 1), fail);
  } else {
    masm_move_imm64(m, temp, op->add_named.proto);
    masm_branch64(m, ASM_COND_NOT_EQUAL, scratch, temp, fail);
  }
  masm_load64(m, scratch, asm_addr(cg->pinned.vm, (int32_t)offsetof(sv_vm_t, js)));
  masm_load32(m, scratch, asm_addr(scratch, (int32_t)offsetof(ant_t, ic.epoch)));
  masm_move_imm64(m, temp, op->add_named.cell);
  masm_load32(m, temp, asm_addr(temp, 0));
  masm_branch32(m, ASM_COND_NOT_EQUAL, scratch, temp, fail);
  masm_move_imm32(m, output, 1);
  masm_jump(m, done);
  masm_bind(m, fail);
  masm_move_imm32(m, output, 0);
  masm_bind(m, done);
}

static void opt_cg_add_named(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  const ir_op_t *op = &ir_node(cg->graph, node)->op;
  asm_gpr_t object = opt_cg_input(cg, node, 2), value = opt_cg_input(cg, node, 1), from = opt_cg_temp(cg, node, 0);
  asm_gpr_t scratch = cg->pinned.scratch;
  uint32_t slot = opt_named_index(op->add_named.offset);
  asm_label_t resume = masm_new_label(m);
  opt_cg_arg_t args[2] = {opt_cg_reg_arg(object), {.constant = true, .bits = op->add_named.shape}};
  asm_label_t call = opt_cg_out_of_line(cg, resume, (uint64_t)(uintptr_t)opt_rt_shape_transition, args, 2);
  masm_load16(m, scratch, asm_addr(object, (int32_t)offsetof(ant_object_t, flags)));
  masm_branch_test32(m, ASM_COND_NON_ZERO, scratch, ANT_OBJECT_FLAG_GUARDS_ABSENCE, call);
  masm_load64(m, from, asm_addr(object, (int32_t)offsetof(ant_object_t, shape)));
  masm_load32(m, scratch, asm_addr(from, (int32_t)ANT_SHAPE_REF_COUNT_OFFSET));
  masm_branch32_imm(m, ASM_COND_BELOW_OR_EQUAL, scratch, 1, call);
  masm_sub32_imm(m, scratch, scratch, 1);
  masm_store32(m, asm_addr(from, (int32_t)ANT_SHAPE_REF_COUNT_OFFSET), scratch);
  masm_move_imm64(m, from, op->add_named.shape);
  masm_add32_to_memory_imm(m, asm_addr(from, (int32_t)ANT_SHAPE_REF_COUNT_OFFSET), 1);
  masm_store64(m, asm_addr(object, (int32_t)offsetof(ant_object_t, shape)), from);
  masm_bind(m, resume);
  masm_move_imm32(m, scratch, slot + 1);
  masm_store32(m, asm_addr(object, (int32_t)offsetof(ant_object_t, prop_count)), scratch);
  uint32_t limit = opt_named_inobj_limit(op->add_named.offset);
  if (slot < limit) masm_store64(m, asm_addr(object, (int32_t)(offsetof(ant_object_t, inobj) + 8 * slot)), value);
  else {
    masm_load64(m, scratch, asm_addr(object, (int32_t)offsetof(ant_object_t, overflow_prop)));
    masm_store64(m, asm_addr(scratch, (int32_t)(8 * (slot - limit))), value);
  }
  if (opt_cg_needs_barrier(cg, ir_node(cg->graph, node)->inputs.items[1]))
    opt_cg_object_barrier(cg, object, value, (uint64_t)(uintptr_t)opt_rt_write_barrier, NULL);
}

static void opt_cg_take_object(opt_cg_t *cg, asm_gpr_t out, asm_gpr_t count, asm_gpr_t temp, asm_gpr_t js, asm_label_t slow) {
  masm_t *m = &cg->masm;
  const int32_t arena = (int32_t)offsetof(ant_t, obj_arena);
  asm_label_t bump = masm_new_label(m), taken = masm_new_label(m);
  masm_load64(m, count, asm_addr(js, arena + (int32_t)offsetof(ant_fixed_arena_t, live_count)));
  masm_load64(m, temp, asm_addr(js, (int32_t)offsetof(ant_t, gc.alloc_limit)));
  masm_branch64(m, ASM_COND_ABOVE_OR_EQUAL, count, temp, slow);
  masm_load64(m, out, asm_addr(js, arena + (int32_t)offsetof(ant_fixed_arena_t, free_list)));
  masm_branch64_imm(m, ASM_COND_EQUAL, out, 0, bump);
  masm_load64(m, temp, asm_addr(out, 0));
  masm_store64(m, asm_addr(js, arena + (int32_t)offsetof(ant_fixed_arena_t, free_list)), temp);
  masm_jump(m, taken);
  masm_bind(m, bump);
  masm_load64(m, out, asm_addr(js, arena + (int32_t)offsetof(ant_fixed_arena_t, cursor)));
  masm_add64_imm(m, temp, out, (int64_t)sizeof(ant_object_t));
  masm_branch64_memory(m, ASM_COND_BELOW, asm_addr(js, arena + (int32_t)offsetof(ant_fixed_arena_t, end)), temp, slow);
  masm_store64(m, asm_addr(js, arena + (int32_t)offsetof(ant_fixed_arena_t, cursor)), temp);
  masm_bind(m, taken);
  masm_add64_imm(m, count, count, 1);
  masm_store64(m, asm_addr(js, arena + (int32_t)offsetof(ant_fixed_arena_t, live_count)), count);
  masm_load64(m, temp, asm_addr(js, (int32_t)offsetof(ant_t, objects)));
  masm_store64(m, asm_addr(out, (int32_t)offsetof(ant_object_t, next)), temp);
  masm_store64(m, asm_addr(js, (int32_t)offsetof(ant_t, objects)), out);
}

static void opt_cg_allocate_object(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  const ir_op_t *op = &ir_node(cg->graph, node)->op;
  asm_gpr_t out = opt_cg_output(cg, node), count = opt_cg_temp(cg, node, 0), temp = opt_cg_temp(cg, node, 1), js = cg->pinned.scratch;
  opt_cg_allocation_t allocation = {.label = masm_new_label(m), .resume = masm_new_label(m), .node = node};
  opt_vec_push(&cg->arena, &cg->allocations, allocation);
#ifdef ANT_GC_VERIFY
  masm_jump(m, allocation.label);
  masm_bind(m, allocation.resume);
  return;
#endif
  masm_load64(m, js, asm_addr(cg->pinned.vm, (int32_t)offsetof(sv_vm_t, js)));
  opt_cg_take_object(cg, out, count, temp, js, allocation.label);

  ant_shape_t *shape = (ant_shape_t *)(uintptr_t)op->allocate_object.shape;
  uint64_t proto;
  bool constant_proto = opt_cg_input_constant(cg, node, 0, &proto);
  ant_object_t image;
  memset(&image, 0, sizeof image);
  image.shape = shape;
  image.proto = constant_proto ? proto : 0;
  for (uint32_t i = 0; i < ANT_INOBJ_MAX_SLOTS; i++) image.inobj[i] = IR_VALUE_UNDEFINED;
  image.u.data.value = IR_VALUE_UNDEFINED;
  image.u.array.len = image.u.array.cap = 0;
  image.prop_count = op->allocate_object.property_count;
  image.type_tag = kTypeObject;
  image.inobj_limit = ant_shape_get_inobj_limit(shape);
  image.flags.raw = ANT_OBJECT_FLAG_EXTENSIBLE;
  uint64_t words[sizeof image / 8];
  memcpy(words, &image, sizeof words);
  bool stored[sizeof image / 8] = {0};
  stored[offsetof(ant_object_t, next) / 8] = true;
  stored[offsetof(ant_object_t, shape) / 8] = true;
  masm_move_imm64(m, temp, (uint64_t)(uintptr_t)shape);
  masm_store64(m, asm_addr(out, (int32_t)offsetof(ant_object_t, shape)), temp);
  masm_add32_to_memory_imm(m, asm_addr(temp, (int32_t)ANT_SHAPE_REF_COUNT_OFFSET), 1);
  if (!constant_proto) {
    stored[offsetof(ant_object_t, proto) / 8] = true;
    masm_store64(m, asm_addr(out, (int32_t)offsetof(ant_object_t, proto)), opt_cg_input(cg, node, 0));
  }
  for (uint32_t w = 0; w < sizeof words / sizeof *words; w++) {
    if (stored[w]) continue;
    masm_move_imm64(m, temp, words[w]);
    for (uint32_t v = w; v < sizeof words / sizeof *words; v++) {
      if (stored[v] || words[v] != words[w]) continue;
      masm_store64(m, asm_addr(out, (int32_t)(8 * v)), temp);
      stored[v] = true;
    }
  }
  masm_move_imm64(m, temp, ant_cage_base());
  masm_sub64(m, out, out, temp);
  masm_move_imm64(m, temp, NANBOX_PREFIX | ((uint64_t)kTypeObject << NANBOX_TYPE_SHIFT));
  masm_or64(m, out, out, temp);
  masm_bind(m, allocation.resume);
}

static constexpr uint64_t OPT_ASCII_META = (uint64_t)STR_ASCII_YES << STR_META_ASCII_SHIFT;
static constexpr int32_t OPT_FLAT_BYTES = (int32_t)offsetof(ant_flat_string_t, bytes);

typedef struct {
  asm_gpr_t value;
  bool int32;
  bool number;
  const ant_flat_string_t *constant;
} opt_cg_concat_side_t;

typedef struct {
  asm_gpr_t out;
  asm_gpr_t t[5];
  asm_label_t slow;
  asm_label_t done;
} opt_cg_concat_t;

static opt_cg_concat_side_t opt_cg_concat_side(const opt_cg_t *cg, ir_node_id_t node, uint32_t index) {
  const ir_node_t *data = ir_node(cg->graph, node);
  opt_cg_concat_side_t side = {.value = opt_cg_input(cg, node, index), .int32 = (data->op.count & (IR_CONCAT_INT32_LHS << index)) != 0};
  uint64_t bits;
  if (side.int32) return side;
  if (ir_constant_value(cg->graph, data->inputs.items[index], &bits) && ir_value_is_string(bits))
    side.constant = ant_str_flat_ptr((ant_value_t)bits);
  side.number = !side.constant && (data->op.count & IR_CONCAT_NUMBERS);
  return side;
}

static bool opt_cg_concat_ascii(const opt_cg_concat_side_t *side) {
  return !side->constant || str_flat_ascii_state(side->constant) == STR_ASCII_YES;
}

static void opt_cg_string_pointer(opt_cg_t *cg, asm_gpr_t dst, asm_gpr_t value) {
  masm_and64_imm(&cg->masm, dst, value, NANBOX_DATA_MASK);
  masm_add64_imm(&cg->masm, dst, dst, (int64_t)ant_cage_base());
}

static void opt_cg_branch_not_ascii(opt_cg_t *cg, asm_gpr_t pointer, asm_label_t slow) {
  masm_t *m = &cg->masm;
  asm_gpr_t scratch = cg->pinned.scratch;
  static_assert(STR_META_ASCII_SHIFT >= 56, "ascii state lives in the top byte of meta");
  masm_load8(m, scratch, asm_addr(pointer, (int32_t)offsetof(ant_flat_string_t, meta) + 7));
  masm_and32_imm(m, scratch, scratch, (int32_t)(STR_META_FIELD_MASK << (STR_META_ASCII_SHIFT - 56)));
  masm_branch32_imm(m, ASM_COND_NOT_EQUAL, scratch, (int32_t)((uint32_t)STR_ASCII_YES << (STR_META_ASCII_SHIFT - 56)), slow);
}

static void opt_cg_allocate_flat(opt_cg_t *cg, asm_gpr_t out, asm_gpr_t total, asm_gpr_t bucket, asm_gpr_t next, asm_label_t slow) {
  masm_t *m = &cg->masm;
  asm_gpr_t js = cg->pinned.scratch;
  const int32_t pool = (int32_t)offsetof(ant_t, gc.pool_alloc);
  const int32_t charge = (int32_t)sizeof(ant_flat_string_t) + 1;
  const size_t overhead = sizeof(ant_flat_string_t) + _Alignof(ant_flat_string_t);
  asm_label_t ready = masm_new_label(m), bump = masm_new_label(m), taken = masm_new_label(m);
  masm_load64(m, js, asm_addr(cg->pinned.vm, (int32_t)offsetof(sv_vm_t, js)));
  masm_load64(m, out, asm_addr(js, pool));
  masm_add64(m, out, out, total);
  masm_add64_imm(m, out, out, charge);
  masm_branch64_memory(m, ASM_COND_BELOW_OR_EQUAL, asm_addr(js, (int32_t)offsetof(ant_t, gc.pool_limit)), out, slow);
  for (uint32_t max = 1; max < (uint32_t)STR_SHORT_CONS_THRESHOLD; max++) {
    int index = pool_size_class_index(overhead + max);
    bool last = max + 1 == (uint32_t)STR_SHORT_CONS_THRESHOLD;
    if (index < 0) {
      masm_jump(m, slow);
      break;
    }
    if (!last && index == pool_size_class_index(overhead + max + 1)) continue;
    int64_t offset = (int64_t)offsetof(ant_t, pool.string.classes) + (int64_t)index * (int64_t)sizeof(ant_pool_bucket_t);
    asm_label_t next_class = masm_new_label(m);
    if (!last) masm_branch64_imm(m, ASM_COND_ABOVE, total, max, next_class);
    masm_add64_imm(m, bucket, js, offset);
    masm_jump(m, ready);
    masm_bind(m, next_class);
  }
  masm_bind(m, ready);
  masm_load64(m, out, asm_addr(bucket, (int32_t)offsetof(ant_pool_bucket_t, slot_free)));
  masm_branch64_imm(m, ASM_COND_EQUAL, out, 0, bump);
  masm_load64(m, next, asm_addr(out, 0));
  masm_store64(m, asm_addr(bucket, (int32_t)offsetof(ant_pool_bucket_t, slot_free)), next);
  masm_jump(m, taken);
  masm_bind(m, bump);
  masm_load64(m, js, asm_addr(bucket, (int32_t)offsetof(ant_pool_bucket_t, current)));
  masm_branch64_imm(m, ASM_COND_EQUAL, js, 0, slow);
  masm_load64(m, out, asm_addr(bucket, (int32_t)offsetof(ant_pool_bucket_t, cursor)));
  masm_load64(m, next, asm_addr(bucket, (int32_t)offsetof(ant_pool_bucket_t, slot_stride)));
  masm_add64(m, next, next, out);
  masm_branch64_memory(m, ASM_COND_BELOW, asm_addr(bucket, (int32_t)offsetof(ant_pool_bucket_t, end)), next, slow);
  masm_store64(m, asm_addr(bucket, (int32_t)offsetof(ant_pool_bucket_t, cursor)), next);
  masm_sub64(m, next, next, js);
  masm_sub64_imm(m, next, next, (int64_t)offsetof(ant_pool_block_t, data));
  masm_store64(m, asm_addr(js, (int32_t)offsetof(ant_pool_block_t, used)), next);
  masm_bind(m, taken);
  masm_load64(m, js, asm_addr(cg->pinned.vm, (int32_t)offsetof(sv_vm_t, js)));
  masm_load64(m, next, asm_addr(js, pool));
  masm_add64(m, next, next, total);
  masm_add64_imm(m, next, next, charge);
  masm_store64(m, asm_addr(js, pool), next);
  masm_store64(m, asm_addr(out, (int32_t)offsetof(ant_flat_string_t, len)), total);
  masm_store_imm64(m, asm_addr(out, (int32_t)offsetof(ant_flat_string_t, meta)), OPT_ASCII_META | STR_UTF16_LEN_UNKNOWN);
}

static void opt_cg_copy_short(opt_cg_t *cg, asm_gpr_t dst, asm_gpr_t src, asm_gpr_t len, asm_gpr_t index) {
  masm_t *m = &cg->masm;
  asm_gpr_t tmp = cg->pinned.scratch;
  asm_label_t four = masm_new_label(m), eight = masm_new_label(m), sixteen = masm_new_label(m), done = masm_new_label(m);
  masm_add64_imm(m, src, src, OPT_FLAT_BYTES);
  masm_branch64_imm(m, ASM_COND_ABOVE_OR_EQUAL, len, 4, four);
  masm_load8(m, tmp, asm_addr(src, 0));
  masm_store8(m, asm_addr(dst, 0), tmp);
  masm_shr64_imm(m, index, len, 1);
  masm_load8(m, tmp, asm_addr_indexed(src, index, ASM_SCALE_1, 0));
  masm_store8(m, asm_addr_indexed(dst, index, ASM_SCALE_1, 0), tmp);
  masm_sub64_imm(m, index, len, 1);
  masm_load8(m, tmp, asm_addr_indexed(src, index, ASM_SCALE_1, 0));
  masm_store8(m, asm_addr_indexed(dst, index, ASM_SCALE_1, 0), tmp);
  masm_jump(m, done);
  masm_bind(m, four);
  masm_branch64_imm(m, ASM_COND_ABOVE_OR_EQUAL, len, 8, eight);
  masm_load32(m, tmp, asm_addr(src, 0));
  masm_store32(m, asm_addr(dst, 0), tmp);
  masm_sub64_imm(m, index, len, 4);
  masm_load32(m, tmp, asm_addr_indexed(src, index, ASM_SCALE_1, 0));
  masm_store32(m, asm_addr_indexed(dst, index, ASM_SCALE_1, 0), tmp);
  masm_jump(m, done);
  masm_bind(m, eight);
  masm_branch64_imm(m, ASM_COND_ABOVE_OR_EQUAL, len, 16, sixteen);
  masm_load64(m, tmp, asm_addr(src, 0));
  masm_store64(m, asm_addr(dst, 0), tmp);
  masm_sub64_imm(m, index, len, 8);
  masm_load64(m, tmp, asm_addr_indexed(src, index, ASM_SCALE_1, 0));
  masm_store64(m, asm_addr_indexed(dst, index, ASM_SCALE_1, 0), tmp);
  masm_jump(m, done);
  masm_bind(m, sixteen);
  masm_load64(m, tmp, asm_addr(src, 0));
  masm_store64(m, asm_addr(dst, 0), tmp);
  masm_load64(m, tmp, asm_addr(src, 8));
  masm_store64(m, asm_addr(dst, 8), tmp);
  masm_sub64_imm(m, index, len, 16);
  masm_load64(m, tmp, asm_addr_indexed(src, index, ASM_SCALE_1, 0));
  masm_store64(m, asm_addr_indexed(dst, index, ASM_SCALE_1, 0), tmp);
  masm_add64_imm(m, index, index, 8);
  masm_load64(m, tmp, asm_addr_indexed(src, index, ASM_SCALE_1, 0));
  masm_store64(m, asm_addr_indexed(dst, index, ASM_SCALE_1, 0), tmp);
  masm_bind(m, done);
}

static void opt_cg_store_bytes(opt_cg_t *cg, asm_gpr_t dst, const char *bytes, uint32_t len) {
  masm_t *m = &cg->masm;
  uint64_t word = 0;
  uint32_t half = 0;
  uint32_t at = 0;
  for (; at + 8 <= len; at += 8) {
    memcpy(&word, bytes + at, 8);
    masm_store_imm64(m, asm_addr(dst, (int32_t)at), word);
  }
  if (at < len && len >= 8) {
    memcpy(&word, bytes + len - 8, 8);
    masm_store_imm64(m, asm_addr(dst, (int32_t)(len - 8)), word);
    return;
  }
  if (at + 4 <= len) {
    memcpy(&half, bytes + at, 4);
    masm_store_imm32(m, asm_addr(dst, (int32_t)at), half);
    at += 4;
  }
  if (at < len && len >= 4) {
    memcpy(&half, bytes + len - 4, 4);
    masm_store_imm32(m, asm_addr(dst, (int32_t)(len - 4)), half);
    return;
  }
  for (; at < len; at++) {
    masm_move_imm32(m, cg->pinned.scratch, (uint8_t)bytes[at]);
    masm_store8(m, asm_addr(dst, (int32_t)at), cg->pinned.scratch);
  }
}

static void opt_cg_tag_string(opt_cg_t *cg, asm_gpr_t out, asm_gpr_t temp, uint64_t heap_tag) {
  masm_move_imm64(&cg->masm, temp, mkval(kTypeString, heap_tag) - ant_cage_base());
  masm_add64(&cg->masm, out, out, temp);
}

static void opt_cg_concat_integer(opt_cg_t *cg, const opt_cg_concat_side_t *side, asm_gpr_t dst, bool checked, asm_label_t fail) {
  masm_t *m = &cg->masm;
  if (side->int32) {
    masm_move32(m, dst, side->value);
    return;
  }
  asm_fpr_t number = opt_cg_lowering_fpr(0);
  masm_move_gpr_to_double(m, number, side->value);
  if (checked) masm_branch_convert_double_to_int32(m, dst, number, fail, ASM_NEGATIVE_ZERO_FAIL);
  else masm_truncate_double_to_int64(m, dst, number);
}

static void opt_cg_concat_number(
  opt_cg_t *cg, const opt_cg_concat_t *c, const opt_cg_concat_side_t *number,
  const opt_cg_concat_side_t *string, bool number_first
) {
  masm_t *m = &cg->masm;
  asm_gpr_t out = c->out, scratch = cg->pinned.scratch;
  asm_gpr_t value = c->t[0], digits = c->t[1], total = c->t[2], cursor = c->t[3];
  if (!opt_cg_concat_ascii(string)) {
    masm_jump(m, c->slow);
    return;
  }
  opt_cg_concat_integer(cg, number, value, true, c->slow);
  if (string->constant) masm_move_imm64(m, total, string->constant->len);
  else {
    masm_sar64_imm(m, scratch, string->value, NANBOX_TYPE_SHIFT);
    masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, OPT_STRING_TAG_SIGNED, c->slow);
    opt_cg_string_pointer(cg, total, string->value);
    masm_branch_test64(m, ASM_COND_NON_ZERO, total, (int64_t)STR_HEAP_TAG_MASK, c->slow);
    opt_cg_branch_not_ascii(cg, total, c->slow);
    masm_load64(m, total, asm_addr(total, (int32_t)offsetof(ant_flat_string_t, len)));
  }
  asm_label_t positive = masm_new_label(m);
  masm_shr32_imm(m, digits, value, 31);
  masm_add32_imm(m, digits, digits, 1);
  masm_branch32_imm(m, ASM_COND_GREATER_THAN_OR_EQUAL, value, 0, positive);
  masm_neg32(m, value, value);
  masm_bind(m, positive);
  for (int32_t power = 10; power <= 1000000000; power *= 10) {
    masm_compare32_imm_set(m, ASM_COND_ABOVE_OR_EQUAL, scratch, value, power);
    masm_add32(m, digits, digits, scratch);
    if (power == 1000000000) break;
  }
  masm_add64(m, total, total, digits);
  masm_branch64_imm(m, ASM_COND_ABOVE_OR_EQUAL, total, STR_SHORT_CONS_THRESHOLD, c->slow);
  opt_cg_allocate_flat(cg, out, total, cursor, value, c->slow);

  masm_add64(m, cursor, out, number_first ? digits : total);
  masm_add64_imm(m, cursor, cursor, OPT_FLAT_BYTES);
  opt_cg_concat_integer(cg, number, value, false, c->slow);
  asm_label_t unsigned_value = masm_new_label(m), loop = masm_new_label(m), copied = masm_new_label(m);
  masm_branch32_imm(m, ASM_COND_GREATER_THAN_OR_EQUAL, value, 0, unsigned_value);
  masm_neg32(m, value, value);
  masm_bind(m, unsigned_value);
  masm_move_imm64(m, total, 0xcccccccdu);
  masm_bind(m, loop);
  masm_mul64(m, scratch, value, total);
  masm_shr64_imm(m, scratch, scratch, 35);
  masm_move_imm32(m, digits, 10);
  masm_mul64(m, digits, scratch, digits);
  masm_sub64(m, value, value, digits);
  masm_add32_imm(m, value, value, '0');
  masm_sub64_imm(m, cursor, cursor, 1);
  masm_store8(m, asm_addr(cursor, 0), value);
  masm_move32(m, value, scratch);
  masm_branch32_imm(m, ASM_COND_NOT_EQUAL, value, 0, loop);
  asm_label_t unsigned_sign = masm_new_label(m);
  if (number->int32) masm_branch32_imm(m, ASM_COND_GREATER_THAN_OR_EQUAL, number->value, 0, unsigned_sign);
  else masm_branch_test64(m, ASM_COND_ZERO, number->value, UINT64_C(1) << 63, unsigned_sign);
  masm_move_imm32(m, value, '-');
  masm_store8(m, asm_addr(cursor, -1), value);
  masm_bind(m, unsigned_sign);

  if (string->constant) {
    uint32_t len = (uint32_t)string->constant->len;
    if (number_first) {
      masm_load64(m, cursor, asm_addr(out, (int32_t)offsetof(ant_flat_string_t, len)));
      masm_add64(m, cursor, cursor, out);
      masm_add64_imm(m, cursor, cursor, OPT_FLAT_BYTES - (int64_t)len);
    } else masm_add64_imm(m, cursor, out, OPT_FLAT_BYTES);
    opt_cg_store_bytes(cg, cursor, string->constant->bytes, len);
  } else {
    opt_cg_string_pointer(cg, value, string->value);
    masm_load64(m, total, asm_addr(value, (int32_t)offsetof(ant_flat_string_t, len)));
    masm_branch64_imm(m, ASM_COND_EQUAL, total, 0, copied);
    if (number_first) {
      masm_load64(m, cursor, asm_addr(out, (int32_t)offsetof(ant_flat_string_t, len)));
      masm_sub64(m, cursor, cursor, total);
      masm_add64(m, cursor, cursor, out);
      masm_add64_imm(m, cursor, cursor, OPT_FLAT_BYTES);
    } else masm_add64_imm(m, cursor, out, OPT_FLAT_BYTES);
    opt_cg_copy_short(cg, cursor, value, total, digits);
  }
  masm_bind(m, copied);
  masm_load64(m, value, asm_addr(out, (int32_t)offsetof(ant_flat_string_t, len)));
  masm_add64(m, value, value, out);
  masm_move_imm32(m, scratch, 0);
  masm_store8(m, asm_addr(value, OPT_FLAT_BYTES), scratch);
  opt_cg_tag_string(cg, out, value, STR_HEAP_TAG_FLAT);
  masm_jump(m, c->done);
}

static void opt_cg_concat_classify(
  opt_cg_t *cg, const opt_cg_concat_side_t *side, asm_gpr_t pointer,
  asm_gpr_t length, asm_label_t number, asm_label_t slow
) {
  masm_t *m = &cg->masm;
  asm_gpr_t scratch = cg->pinned.scratch;
  if (side->constant) {
    masm_move_imm64(m, pointer, (uint64_t)(uintptr_t)side->constant);
    masm_move_imm64(m, length, side->constant->len);
    return;
  }
  asm_label_t flat = masm_new_label(m), classified = masm_new_label(m);
  masm_sar64_imm(m, scratch, side->value, NANBOX_TYPE_SHIFT);
  if (side->number) {
    asm_label_t string = masm_new_label(m);
    masm_branch64_imm(m, ASM_COND_EQUAL, scratch, OPT_STRING_TAG_SIGNED, string);
    opt_cg_branch_not_number(cg, side->value, slow);
    masm_jump(m, number);
    masm_bind(m, string);
  } else masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, OPT_STRING_TAG_SIGNED, slow);
  opt_cg_string_pointer(cg, pointer, side->value);
  masm_and32_imm(m, scratch, pointer, STR_HEAP_TAG_MASK);
  masm_branch32_imm(m, ASM_COND_EQUAL, scratch, STR_HEAP_TAG_FLAT, flat);
  masm_branch32_imm(m, ASM_COND_NOT_EQUAL, scratch, STR_HEAP_TAG_ROPE, slow);
  masm_load64(m, length, asm_addr(pointer, (int32_t)offsetof(ant_rope_heap_t, len) - (int32_t)STR_HEAP_TAG_ROPE));
  masm_jump(m, classified);
  masm_bind(m, flat);
  masm_load64(m, length, asm_addr(pointer, (int32_t)offsetof(ant_flat_string_t, len)));
  masm_bind(m, classified);
}

static void opt_cg_concat_copy(opt_cg_t *cg, const opt_cg_concat_side_t *side, asm_gpr_t cursor, asm_gpr_t pointer, asm_gpr_t len, asm_gpr_t index) {
  masm_t *m = &cg->masm;
  if (side->constant) {
    opt_cg_store_bytes(cg, cursor, side->constant->bytes, (uint32_t)side->constant->len);
    masm_add64_imm(m, cursor, cursor, (int64_t)side->constant->len);
    return;
  }
  masm_load64(m, len, asm_addr(pointer, (int32_t)offsetof(ant_flat_string_t, len)));
  opt_cg_copy_short(cg, cursor, pointer, len, index);
  masm_add64(m, cursor, cursor, len);
}

static void opt_cg_concat_depth(opt_cg_t *cg, const opt_cg_concat_side_t *side, asm_gpr_t depth, asm_gpr_t pointer) {
  masm_t *m = &cg->masm;
  masm_move_imm32(m, depth, 0);
  if (side->constant) return;
  asm_label_t flat = masm_new_label(m);
  masm_branch_test64(m, ASM_COND_ZERO, pointer, (int64_t)STR_HEAP_TAG_ROPE, flat);
  masm_load16(m, depth, asm_addr(pointer, (int32_t)offsetof(ant_rope_heap_t, depth) - (int32_t)STR_HEAP_TAG_ROPE));
  masm_bind(m, flat);
}

static void opt_cg_concat_strings(opt_cg_t *cg, const opt_cg_concat_t *c, const opt_cg_concat_side_t *lhs, const opt_cg_concat_side_t *rhs) {
  static_assert(offsetof(ant_rope_heap_t, flags) == offsetof(ant_rope_heap_t, depth) + 2, "rope header word");
  static_assert(offsetof(ant_rope_heap_t, mark_epoch) == offsetof(ant_rope_heap_t, depth) + 4, "rope header word");
  static_assert(sizeof(((ant_rope_heap_t *)0)->len) == 8 && sizeof(((ant_flat_string_t *)0)->len) == 8, "string lengths");
  static_assert(STR_HEAP_TAG_FLAT == 0 && STR_HEAP_TAG_ROPE == 1, "string heap tags");
  masm_t *m = &cg->masm;
  asm_gpr_t out = c->out, scratch = cg->pinned.scratch;
  asm_gpr_t left = c->t[0], right = c->t[1], left_len = c->t[2], right_len = c->t[3], total = c->t[4];
  asm_label_t slow = c->slow, take_left = masm_new_label(m), take_right = masm_new_label(m), rope = masm_new_label(m);
  asm_label_t numbers[2] = {masm_new_label(m), masm_new_label(m)};
  opt_cg_concat_classify(cg, lhs, left, left_len, numbers[0], slow);
  opt_cg_concat_classify(cg, rhs, right, right_len, numbers[1], slow);
  if (!lhs->constant || !lhs->constant->len) masm_branch64_imm(m, ASM_COND_EQUAL, left_len, 0, take_right);
  if (!rhs->constant || !rhs->constant->len) masm_branch64_imm(m, ASM_COND_EQUAL, right_len, 0, take_left);
  masm_add64(m, total, left_len, right_len);
  masm_branch64_imm(m, ASM_COND_ABOVE_OR_EQUAL, total, STR_SHORT_CONS_THRESHOLD, rope);
  if (!opt_cg_concat_ascii(lhs) || !opt_cg_concat_ascii(rhs)) masm_jump(m, slow);
  else {
    if (!lhs->constant && !rhs->constant) {
      masm_or64(m, scratch, left, right);
      masm_branch_test64(m, ASM_COND_NON_ZERO, scratch, (int64_t)STR_HEAP_TAG_MASK, slow);
    } else if (!lhs->constant || !rhs->constant)
      masm_branch_test64(m, ASM_COND_NON_ZERO, lhs->constant ? right : left, (int64_t)STR_HEAP_TAG_MASK, slow);
    if (!lhs->constant) opt_cg_branch_not_ascii(cg, left, slow);
    if (!rhs->constant) opt_cg_branch_not_ascii(cg, right, slow);
    opt_cg_allocate_flat(cg, out, total, left_len, right_len, slow);
    masm_add64_imm(m, total, out, OPT_FLAT_BYTES);
    opt_cg_concat_copy(cg, lhs, total, left, left_len, right_len);
    opt_cg_concat_copy(cg, rhs, total, right, left_len, right_len);
    masm_move_imm32(m, scratch, 0);
    masm_store8(m, asm_addr(total, 0), scratch);
    opt_cg_tag_string(cg, out, total, STR_HEAP_TAG_FLAT);
    masm_jump(m, c->done);
  }

  masm_bind(m, rope);
  asm_label_t deeper = masm_new_label(m), saturated = masm_new_label(m);
  opt_cg_concat_depth(cg, lhs, left_len, left);
  opt_cg_concat_depth(cg, rhs, right_len, right);
  masm_branch32(m, ASM_COND_ABOVE_OR_EQUAL, left_len, right_len, deeper);
  masm_move32(m, left_len, right_len);
  masm_bind(m, deeper);
  masm_branch32_imm(m, ASM_COND_EQUAL, left_len, ANT_ROPE_DEPTH_SATURATED, saturated);
  masm_add32_imm(m, left_len, left_len, 1);
  masm_bind(m, saturated);
  masm_load64(m, scratch, asm_addr(cg->pinned.vm, (int32_t)offsetof(sv_vm_t, js)));
  masm_branch64_memory_imm(m, ASM_COND_ABOVE_OR_EQUAL, asm_addr(scratch, (int32_t)offsetof(ant_t, rope_gc.young_alloc)), (int64_t)GC_ROPE_NURSERY_THRESHOLD, slow);
  masm_load64(m, out, asm_addr(scratch, (int32_t)offsetof(ant_t, rope_gc.young.head)));
  masm_branch64_imm(m, ASM_COND_EQUAL, out, 0, slow);
  masm_load64(m, right_len, asm_addr(out, (int32_t)offsetof(ant_pool_block_t, used)));
  masm_add64_imm(m, right_len, right_len, (int64_t)sizeof(ant_rope_heap_t));
  masm_branch64_memory(m, ASM_COND_BELOW, asm_addr(out, (int32_t)offsetof(ant_pool_block_t, cap)), right_len, slow);
  masm_store64(m, asm_addr(out, (int32_t)offsetof(ant_pool_block_t, used)), right_len);
  masm_add64(m, out, out, right_len);
  masm_add64_imm(m, out, out, (int64_t)offsetof(ant_pool_block_t, data) - (int64_t)sizeof(ant_rope_heap_t));
  masm_add64_to_memory_imm(m, asm_addr(scratch, (int32_t)offsetof(ant_t, rope_gc.young_alloc)), (int32_t)sizeof(ant_rope_heap_t));
  masm_add64_to_memory_imm(m, asm_addr(scratch, (int32_t)offsetof(ant_t, gc.pool_alloc)), (int32_t)sizeof(ant_rope_heap_t));
  masm_store64(m, asm_addr(out, (int32_t)offsetof(ant_rope_heap_t, len)), total);
  masm_or32_imm(m, left_len, left_len, (int32_t)ANT_ROPE_FLAG_YOUNG << 16);
  masm_store64(m, asm_addr(out, (int32_t)offsetof(ant_rope_heap_t, depth)), left_len);
  masm_store64(m, asm_addr(out, (int32_t)offsetof(ant_rope_heap_t, left)), lhs->value);
  masm_store64(m, asm_addr(out, (int32_t)offsetof(ant_rope_heap_t, right)), rhs->value);
  masm_store_imm64(m, asm_addr(out, (int32_t)offsetof(ant_rope_heap_t, cached)), IR_VALUE_UNDEFINED);
  opt_cg_tag_string(cg, out, total, STR_HEAP_TAG_ROPE);
  masm_jump(m, c->done);
  masm_bind(m, take_left);
  masm_move64(m, out, lhs->value);
  masm_jump(m, c->done);
  masm_bind(m, take_right);
  masm_move64(m, out, rhs->value);
  masm_jump(m, c->done);
  masm_bind(m, numbers[0]);
  if (lhs->number) opt_cg_concat_number(cg, c, lhs, rhs, true);
  masm_bind(m, numbers[1]);
  if (rhs->number) opt_cg_concat_number(cg, c, rhs, lhs, false);
}

static void opt_cg_concatenate_strings(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  opt_cg_allocation_t allocation = {.label = masm_new_label(m), .resume = masm_new_label(m), .node = node};
  opt_vec_push(&cg->arena, &cg->allocations, allocation);
#ifdef ANT_GC_VERIFY
  masm_jump(m, allocation.label);
  masm_bind(m, allocation.resume);
  return;
#endif
  opt_cg_concat_t c = {
    .out = opt_cg_output(cg, node),
    .t = {opt_cg_temp(cg, node, 0), opt_cg_temp(cg, node, 1), opt_cg_temp(cg, node, 2), opt_cg_temp(cg, node, 3), opt_cg_temp(cg, node, 4)},
    .slow = allocation.label,
    .done = allocation.resume,
  };
  opt_cg_concat_side_t lhs = opt_cg_concat_side(cg, node, 0), rhs = opt_cg_concat_side(cg, node, 1);
  if (lhs.int32 && rhs.int32) masm_jump(m, c.slow);
  else if (lhs.int32) opt_cg_concat_number(cg, &c, &lhs, &rhs, true);
  else if (rhs.int32) opt_cg_concat_number(cg, &c, &rhs, &lhs, false);
  else opt_cg_concat_strings(cg, &c, &lhs, &rhs);
  masm_bind(m, allocation.resume);
}

static void opt_cg_string_builder_append(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  asm_gpr_t lhs = opt_cg_input(cg, node, 0), output = opt_cg_output(cg, node);
  asm_gpr_t builder = opt_cg_temp(cg, node, 0), len = opt_cg_temp(cg, node, 1), source = opt_cg_temp(cg, node, 2), cursor = opt_cg_temp(cg, node, 3);
  asm_gpr_t scratch = cg->pinned.scratch;
  asm_label_t fail = masm_new_label(m), done = masm_new_label(m), counted = masm_new_label(m), stored = masm_new_label(m);
  uint64_t bits;
  bool constant = opt_cg_input_constant(cg, node, 1, &bits);
  const ant_flat_string_t *flat = constant && ir_value_is_string(bits) ? ant_str_flat_ptr((ant_value_t)bits) : NULL;
  bool known = flat && flat->len > 0 && flat->len < STR_SHORT_CONS_THRESHOLD && str_flat_ascii_state(flat) == STR_ASCII_YES;
  if (constant && !known) masm_jump(m, fail);
  else {
    masm_sar64_imm(m, scratch, lhs, NANBOX_TYPE_SHIFT);
    masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, OPT_STRING_TAG_SIGNED, fail);
    opt_cg_string_pointer(cg, builder, lhs);
    masm_and64_imm(m, scratch, builder, STR_HEAP_TAG_MASK);
    masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, STR_HEAP_TAG_BUILDER, fail);
    masm_and64_imm(m, builder, builder, ~(int64_t)STR_HEAP_TAG_MASK);
    if (known) masm_move_imm64(m, len, flat->len);
    else {
      asm_gpr_t rhs = opt_cg_input(cg, node, 1);
      masm_sar64_imm(m, scratch, rhs, NANBOX_TYPE_SHIFT);
      masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, OPT_STRING_TAG_SIGNED, fail);
      opt_cg_string_pointer(cg, source, rhs);
      masm_branch_test64(m, ASM_COND_NON_ZERO, source, (int64_t)STR_HEAP_TAG_MASK, fail);
      opt_cg_branch_not_ascii(cg, source, fail);
      masm_load64(m, len, asm_addr(source, (int32_t)offsetof(ant_flat_string_t, len)));
      masm_branch64_imm(m, ASM_COND_EQUAL, len, 0, fail);
      masm_branch64_imm(m, ASM_COND_ABOVE_OR_EQUAL, len, STR_SHORT_CONS_THRESHOLD, fail);
    }
    masm_load16(m, cursor, asm_addr(builder, (int32_t)offsetof(ant_string_builder_t, tail_len)));
    masm_add64(m, scratch, cursor, len);
    masm_branch64_imm(m, ASM_COND_ABOVE, scratch, (int64_t)STR_BUILDER_TAIL_CAP, fail);
    masm_store16(m, asm_addr(builder, (int32_t)offsetof(ant_string_builder_t, tail_len)), scratch);
    masm_add64(m, cursor, cursor, builder);
    masm_add64_imm(m, cursor, cursor, (int64_t)offsetof(ant_string_builder_t, tail));
    if (known) opt_cg_store_bytes(cg, cursor, flat->bytes, (uint32_t)flat->len);
    else {
      opt_cg_copy_short(cg, cursor, source, len, builder);
      opt_cg_string_pointer(cg, builder, lhs);
      masm_and64_imm(m, builder, builder, ~(int64_t)STR_HEAP_TAG_MASK);
    }
    masm_load64(m, scratch, asm_addr(builder, (int32_t)offsetof(ant_string_builder_t, len)));
    masm_add64(m, scratch, scratch, len);
    masm_store64(m, asm_addr(builder, (int32_t)offsetof(ant_string_builder_t, len)), scratch);
    masm_load64(m, source, asm_addr(builder, (int32_t)offsetof(ant_string_builder_t, cached)));
    masm_move_imm64(m, scratch, NANBOX_PREFIX);
    masm_branch64(m, ASM_COND_ABOVE, source, scratch, counted);
    asm_fpr_t total = opt_cg_lowering_fpr(0), added = opt_cg_lowering_fpr(1);
    masm_move_gpr_to_double(m, total, source);
    masm_convert_int32_to_double(m, added, len);
    masm_add_double(m, total, total, added);
    masm_move_double_to_gpr(m, source, total);
    masm_jump(m, stored);
    masm_bind(m, counted);
    masm_move_imm64(m, source, IR_VALUE_UNDEFINED);
    masm_bind(m, stored);
    masm_store64(m, asm_addr(builder, (int32_t)offsetof(ant_string_builder_t, cached)), source);
    masm_move_imm32(m, output, 1);
    masm_jump(m, done);
  }
  masm_bind(m, fail);
  masm_move_imm32(m, output, 0);
  masm_bind(m, done);
}

static void opt_cg_allocate_array(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  uint32_t count_elements = ir_node(cg->graph, node)->op.count;
  asm_gpr_t out = opt_cg_output(cg, node), count = opt_cg_temp(cg, node, 0), temp = opt_cg_temp(cg, node, 1);
  asm_gpr_t data = opt_cg_temp(cg, node, 2), js = cg->pinned.scratch;
  opt_cg_allocation_t allocation = {.label = masm_new_label(m), .resume = masm_new_label(m), .node = node};
  opt_vec_push(&cg->arena, &cg->allocations, allocation);
#ifdef ANT_GC_VERIFY
  masm_jump(m, allocation.label);
  masm_bind(m, allocation.resume);
  return;
#endif
  int cls = gc_array_storage_fit_class(count_elements);
  uint32_t capacity = gc_array_storage_fit(count_elements);
  const int32_t head = (int32_t)(offsetof(ant_t, array_storage.head) + sizeof(ant_value_t *) * (size_t)cls);
  ant_value_t array_proto = cg->js->sym.array_proto;
  masm_load64(m, js, asm_addr(cg->pinned.vm, (int32_t)offsetof(sv_vm_t, js)));
  masm_load64(m, temp, asm_addr(js, (int32_t)offsetof(ant_t, sym.array_proto)));
  masm_move_imm64(m, data, array_proto);
  masm_branch64(m, ASM_COND_NOT_EQUAL, temp, data, allocation.label);
  masm_load64(m, data, asm_addr(js, head));
  masm_branch64_imm(m, ASM_COND_EQUAL, data, 0, allocation.label);
  opt_cg_take_object(cg, out, count, temp, js, allocation.label);
  masm_load64(m, temp, asm_addr(data, 0));
  masm_store64(m, asm_addr(js, head), temp);
  masm_add32_to_memory_imm(m, asm_addr(js, (int32_t)(offsetof(ant_t, array_storage.count) + sizeof(uint32_t) * (size_t)cls)), -1);
  masm_add32_to_memory_imm(m, asm_addr(js, (int32_t)(offsetof(ant_t, array_storage.demand) + sizeof(uint32_t) * (size_t)cls)), 1);
  masm_load64(m, temp, asm_addr(js, (int32_t)offsetof(ant_t, alloc_bytes.arrays)));
  masm_add64_imm(m, temp, temp, (int64_t)(sizeof(ant_value_t) * capacity));
  masm_store64(m, asm_addr(js, (int32_t)offsetof(ant_t, alloc_bytes.arrays)), temp);
  asm_label_t below = masm_new_label(m);
  masm_branch64_memory(m, ASM_COND_ABOVE, asm_addr(js, (int32_t)offsetof(ant_t, gc.array_limit)), temp, below);
  masm_store_imm64(m, asm_addr(js, (int32_t)offsetof(ant_t, gc.alloc_limit)), 0);
  masm_bind(m, below);
  if (count_elements < capacity) {
    masm_move_imm64(m, temp, IR_VALUE_EMPTY);
    for (uint32_t i = count_elements; i < capacity; i++) masm_store64(m, asm_addr(data, (int32_t)(8 * i)), temp);
  }

  ant_shape_t *shape = ant_object_proto_root(cg->js, array_proto, (uint8_t)ANT_INOBJ_MAX_SLOTS);
  if (!shape) {
    opt_cg_fail(cg, "array-shape");
    return;
  }
  ant_shape_release(shape);
  ant_object_t image;
  memset(&image, 0, sizeof image);
  image.shape = shape;
  for (uint32_t i = 0; i < ANT_INOBJ_MAX_SLOTS; i++) image.inobj[i] = IR_VALUE_UNDEFINED;
  image.u.array.len = count_elements;
  image.u.array.cap = capacity;
  image.type_tag = kTypeArray;
  image.inobj_limit = ant_shape_get_inobj_limit(shape);
  image.flags = (ant_object_flags_t){.extensible = 1, .fast_array = 1, .dense_length_fits = 1, .may_have_dense_elements = 1};
  uint64_t words[sizeof image / 8];
  memcpy(words, &image, sizeof words);
  bool stored[sizeof image / 8] = {0};
  stored[offsetof(ant_object_t, next) / 8] = true;
  stored[offsetof(ant_object_t, shape) / 8] = true;
  stored[offsetof(ant_object_t, proto) / 8] = true;
  stored[offsetof(ant_object_t, u.array.data) / 8] = true;
  masm_store64(m, asm_addr(out, (int32_t)offsetof(ant_object_t, u.array.data)), data);
  masm_load64(m, temp, asm_addr(js, (int32_t)offsetof(ant_t, sym.array_proto)));
  masm_store64(m, asm_addr(out, (int32_t)offsetof(ant_object_t, proto)), temp);
  masm_move_imm64(m, temp, (uint64_t)(uintptr_t)shape);
  masm_store64(m, asm_addr(out, (int32_t)offsetof(ant_object_t, shape)), temp);
  masm_add32_to_memory_imm(m, asm_addr(temp, (int32_t)ANT_SHAPE_REF_COUNT_OFFSET), 1);
  for (uint32_t w = 0; w < sizeof words / sizeof *words; w++) {
    if (stored[w]) continue;
    masm_move_imm64(m, temp, words[w]);
    for (uint32_t v = w; v < sizeof words / sizeof *words; v++) {
      if (stored[v] || words[v] != words[w]) continue;
      masm_store64(m, asm_addr(out, (int32_t)(8 * v)), temp);
      stored[v] = true;
    }
  }
  masm_move_imm64(m, temp, ant_cage_base());
  masm_sub64(m, out, out, temp);
  masm_move_imm64(m, temp, NANBOX_PREFIX | ((uint64_t)kTypeArray << NANBOX_TYPE_SHIFT));
  masm_or64(m, out, out, temp);
  masm_bind(m, allocation.resume);
}

static void opt_cg_initialize_element(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  uint32_t index = ir_node(cg->graph, node)->op.index;
  asm_gpr_t array = opt_cg_input(cg, node, 0), address = opt_cg_temp(cg, node, 0), scratch = cg->pinned.scratch;
  masm_and64_imm(m, address, array, NANBOX_DATA_MASK);
  masm_add64_imm(m, address, address, (int64_t)ant_cage_base());
  masm_load64(m, scratch, asm_addr(address, (int32_t)offsetof(ant_object_t, u.array.data)));
  uint64_t bits;
  if (opt_cg_input_constant(cg, node, 1, &bits)) {
    masm_store_imm64(m, asm_addr(scratch, (int32_t)(8 * index)), bits);
    return;
  }
  asm_gpr_t value = opt_cg_input(cg, node, 1);
  masm_store64(m, asm_addr(scratch, (int32_t)(8 * index)), value);
  if (!opt_cg_needs_barrier(cg, ir_node(cg->graph, node)->inputs.items[1])) return;
  opt_cg_arg_t at = {.constant = true, .bits = index};
  opt_cg_object_barrier(cg, address, value, (uint64_t)(uintptr_t)opt_rt_element_barrier, &at);
}

static void opt_cg_emit_allocations(opt_cg_t *cg) {
  masm_t *m = &cg->masm;
  const asm_gpr_t *a = MASM_ARGUMENT_GPRS;
  for (uint32_t i = 0; i < cg->allocations.len; i++) {
    const opt_cg_allocation_t *allocation = &cg->allocations.items[i];
    const ir_op_t *op = &ir_node(cg->graph, allocation->node)->op;
    asm_gpr_t output = opt_cg_output(cg, allocation->node);
    masm_bind(m, allocation->label);
    opt_cg_dump_caller_saved_gprs(cg);
    masm_move64(m, a[0], cg->pinned.vm);
    if (op->code == IR_OP_ALLOCATE_ARRAY) {
      masm_move_imm64(m, a[1], op->count);
      masm_call_absolute(m, (uint64_t)(uintptr_t)opt_rt_allocate_array);
    } else if (op->code == IR_OP_CONCATENATE_STRINGS) {
      opt_cg_slow_argument(cg, a[1], allocation->node, 0);
      opt_cg_slow_argument(cg, a[2], allocation->node, 1);
      masm_move_imm64(m, a[5], op->count & (IR_CONCAT_INT32_LHS | IR_CONCAT_INT32_RHS));
      masm_call_absolute(m, (uint64_t)(uintptr_t)opt_rt_concat);
    } else {
      opt_cg_slow_argument(cg, a[2], allocation->node, 0);
      masm_move_imm64(m, a[1], op->allocate_object.shape);
      masm_move_imm64(m, a[3], op->allocate_object.property_count);
      masm_call_absolute(m, (uint64_t)(uintptr_t)opt_rt_allocate_object);
    }
    masm_store64(m, asm_addr(MASM_STACK_POINTER, (int32_t)(8 * output)), MASM_RETURN_GPRS[0]);
    masm_move64(m, output, MASM_RETURN_GPRS[0]);
    opt_cg_restore_caller_saved_gprs(cg);
    masm_shr64_imm(m, cg->pinned.scratch, output, NANBOX_TYPE_SHIFT);
    masm_branch64_imm(m, ASM_COND_NOT_EQUAL, cg->pinned.scratch, (int64_t)OPT_ERROR_TAG, allocation->resume);
    masm_move64(m, MASM_RETURN_GPRS[0], output);
    masm_jump(m, cg->throw_label);
  }
}

static void opt_cg_initialize_named(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  uint32_t offset = ir_node(cg->graph, node)->op.offset, index = opt_named_index(offset), limit = opt_named_inobj_limit(offset);
  asm_gpr_t object = opt_cg_input(cg, node, 0), value = opt_cg_input(cg, node, 1), address = opt_cg_temp(cg, node, 0);
  masm_and64_imm(m, address, object, NANBOX_DATA_MASK);
  masm_add64_imm(m, address, address, (int64_t)ant_cage_base());
  if (index < limit) masm_store64(m, asm_addr(address, (int32_t)(offsetof(ant_object_t, inobj) + 8 * index)), value);
  else {
    masm_load64(m, cg->pinned.scratch, asm_addr(address, (int32_t)offsetof(ant_object_t, overflow_prop)));
    masm_store64(m, asm_addr(cg->pinned.scratch, (int32_t)(8 * (index - limit))), value);
  }
  if (opt_cg_needs_barrier(cg, ir_node(cg->graph, node)->inputs.items[1]))
    opt_cg_object_barrier(cg, address, value, (uint64_t)(uintptr_t)opt_rt_write_barrier, NULL);
}

static inline asm_address_t opt_cg_call_slot(const opt_cg_t *cg, uint32_t index) {
  return opt_cg_local(cg, cg->call_off + 8 * index);
}

static void opt_cg_store_input(opt_cg_t *cg, ir_node_id_t node, uint32_t index, asm_address_t to) {
  masm_t *m = &cg->masm;
  ra_location_t location = opt_cg_node(cg, node)->inputs.items[index];
  switch (location.kind) {
    case RA_LOC_REGISTER: masm_store64(m, to, location.reg); break;
    case RA_LOC_STACK:
      masm_load64(m, cg->pinned.scratch, opt_cg_spill(cg, location.slot));
      masm_store64(m, to, cg->pinned.scratch);
      break;
    case RA_LOC_CONSTANT: masm_store_imm64(m, to, location.bits); break;
  }
}

static void opt_cg_call_runtime(opt_cg_t *cg, ir_node_id_t node, uint64_t helper) {
  masm_t *m = &cg->masm;
  const asm_gpr_t *a = MASM_ARGUMENT_GPRS;
  const ir_op_t *op = &ir_node(cg->graph, node)->op;
  masm_move64(m, a[0], cg->pinned.vm);
  masm_load64(m, a[1], opt_cg_call_slot(cg, 0));
  masm_load64(m, a[2], opt_cg_call_slot(cg, 1));
  masm_load_effective_address(m, a[3], opt_cg_call_slot(cg, 2));
  masm_move_imm64(m, a[4], op->call.argc);
  if (op->code == IR_OP_CALL_NATIVE) masm_move_imm64(m, a[5], op->call.extra);
  masm_call_absolute(m, helper);
  opt_cg_record_call(cg, ir_node(cg->graph, node)->frame_state, -1);
}

static void opt_cg_load_input(opt_cg_t *cg, ir_node_id_t node, uint32_t index, asm_gpr_t to) {
  masm_t *m = &cg->masm;
  ra_location_t location = opt_cg_node(cg, node)->inputs.items[index];
  switch (location.kind) {
    case RA_LOC_REGISTER:
      if (location.reg != to) masm_move64(m, to, location.reg);
      break;
    case RA_LOC_STACK: masm_load64(m, to, opt_cg_spill(cg, location.slot)); break;
    case RA_LOC_CONSTANT: masm_move_imm64(m, to, location.bits); break;
  }
}

static void opt_cg_branch_error(opt_cg_t *cg, ir_node_id_t node_id) {
  masm_t *m = &cg->masm;
  masm_sar64_imm(m, cg->pinned.scratch, MASM_RETURN_GPRS[0], NANBOX_TYPE_SHIFT);
  masm_branch64_imm(m, ASM_COND_EQUAL, cg->pinned.scratch, OPT_ERROR_TAG_SIGNED, opt_cg_throw_target(cg, node_id, cg->throw_label));
}

static void opt_cg_call_known(opt_cg_t *cg, ir_node_id_t node_id) {
  masm_t *m = &cg->masm;
  const ir_node_t *node = ir_node(cg->graph, node_id);
  const ir_op_t *op = &node->op;
  const asm_gpr_t *a = MASM_ARGUMENT_GPRS;
  asm_gpr_t scratch = cg->pinned.scratch;
  sv_func_t *target = op->call.target;
  asm_gpr_t callee = a[0], func = a[2], expected = a[3], closure = a[4], flags = a[5];
  for (uint32_t i = 2; i < node->inputs.len; i++) opt_cg_store_input(cg, node_id, i, opt_cg_call_slot(cg, i));
  ra_location_t self = opt_cg_node(cg, node_id)->inputs.items[1];
  bool this_stored = self.kind == RA_LOC_REGISTER
    && (self.reg == callee || self.reg == func || self.reg == expected || self.reg == closure || self.reg == flags);
  if (this_stored) opt_cg_store_input(cg, node_id, 1, opt_cg_call_slot(cg, 1));
  opt_cg_load_input(cg, node_id, 0, callee);
  asm_label_t slow = masm_new_label(m), done = masm_new_label(m);
  masm_sar64_imm(m, scratch, callee, NANBOX_TYPE_SHIFT);
  masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, OPT_FUNCTION_TAG_SIGNED, slow);
  masm_and64_imm(m, closure, callee, NANBOX_DATA_MASK);
  masm_add64_imm(m, closure, closure, (int64_t)ant_cage_base());
  masm_load32(m, flags, asm_addr(closure, (int32_t)offsetof(sv_closure_t, call_flags)));
  masm_branch_test32(m, ASM_COND_NON_ZERO, flags, SV_CALL_HAS_BOUND_ARGS | SV_CALL_HAS_BOUND_THIS, slow);
  masm_load64(m, func, asm_addr(closure, (int32_t)offsetof(sv_closure_t, func)));
  masm_move_imm64(m, expected, (uint64_t)(uintptr_t)target);
  masm_branch64(m, ASM_COND_NOT_EQUAL, func, expected, slow);
  masm_load64(m, scratch, asm_addr(func, (int32_t)offsetof(sv_func_t, jit_code)));
  masm_branch64_imm(m, ASM_COND_EQUAL, scratch, 0, slow);
  if (target->is_arrow) masm_load64(m, a[1], asm_addr(closure, (int32_t)offsetof(sv_closure_t, bound_this)));
  else if (this_stored) masm_load64(m, a[1], opt_cg_call_slot(cg, 1));
  else opt_cg_load_input(cg, node_id, 1, a[1]);
  masm_load64(m, a[3], asm_addr(closure, (int32_t)offsetof(sv_closure_t, super_val)));
#if defined(__aarch64__)
  masm_move64(m, a[6], closure);
#else
  masm_store64(m, opt_cg_local(cg, 0), closure);
#endif
  if (!target->is_arrow && !this_stored && self.kind == RA_LOC_CONSTANT && self.bits == IR_VALUE_UNDEFINED) masm_move64(m, a[2], a[1]);
  else masm_move_imm64(m, a[2], IR_VALUE_UNDEFINED);
  masm_load_effective_address(m, a[4], opt_cg_call_slot(cg, 2));
  masm_move_imm32(m, a[5], op->call.argc);
  masm_move64(m, a[0], cg->pinned.vm);
  masm_call_register(m, scratch);
  opt_cg_record_call(cg, node->frame_state, -1);
  masm_bind(m, done);
  opt_vec_push(&cg->arena, &cg->call_slows, ((opt_cg_call_slow_t){.label = slow, .done = done, .node = node_id, .this_stored = this_stored}));
  opt_cg_branch_error(cg, node_id);
}

static void opt_cg_emit_call_slows(opt_cg_t *cg) {
  masm_t *m = &cg->masm;
  for (uint32_t i = 0; i < cg->call_slows.len; i++) {
    const opt_cg_call_slow_t *slow = &cg->call_slows.items[i];
    masm_bind(m, slow->label);
    masm_store64(m, opt_cg_call_slot(cg, 0), MASM_ARGUMENT_GPRS[0]);
    if (!slow->this_stored) opt_cg_store_input(cg, slow->node, 1, opt_cg_call_slot(cg, 1));
    opt_cg_call_runtime(cg, slow->node, (uint64_t)(uintptr_t)opt_rt_call);
    masm_jump(m, slow->done);
  }
}

static void opt_cg_call(opt_cg_t *cg, ir_node_id_t node_id) {
  masm_t *m = &cg->masm;
  const ir_node_t *node = ir_node(cg->graph, node_id);
  const ir_op_t *op = &node->op;
  const asm_gpr_t *a = MASM_ARGUMENT_GPRS;
  asm_gpr_t scratch = cg->pinned.scratch;
  if (op->code == IR_OP_CALL_NATIVE && op->call.slow_path) opt_cg_count_slow_path(cg, op->call.func, op->call.bc_off);
  if (op->code == IR_OP_CALL_DIRECT && op->call.target) {
    opt_cg_call_known(cg, node_id);
    return;
  }
  for (uint32_t i = 0; i < node->inputs.len; i++) opt_cg_store_input(cg, node_id, i, opt_cg_call_slot(cg, i));
  if (op->code == IR_OP_CALL_CONSTRUCT) opt_cg_call_runtime(cg, node_id, (uint64_t)(uintptr_t)opt_rt_construct);
  else if (op->code == IR_OP_CALL_NATIVE) opt_cg_call_runtime(cg, node_id, op->call.helper);
  else {
    asm_label_t slow = masm_new_label(m), done = masm_new_label(m);
    masm_load64(m, a[0], opt_cg_call_slot(cg, 0));
    masm_sar64_imm(m, scratch, a[0], NANBOX_TYPE_SHIFT);
    masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, OPT_FUNCTION_TAG_SIGNED, slow);
    masm_and64_imm(m, a[0], a[0], NANBOX_DATA_MASK);
    masm_add64_imm(m, a[0], a[0], (int64_t)ant_cage_base());
    masm_load32(m, a[1], asm_addr(a[0], (int32_t)offsetof(sv_closure_t, call_flags)));
    masm_branch_test32(m, ASM_COND_NON_ZERO, a[1], SV_CALL_HAS_BOUND_ARGS | SV_CALL_HAS_BOUND_THIS, slow);
    masm_load64(m, a[2], asm_addr(a[0], (int32_t)offsetof(sv_closure_t, func)));
    masm_branch64_imm(m, ASM_COND_EQUAL, a[2], 0, slow);
    masm_load64(m, scratch, asm_addr(a[2], (int32_t)offsetof(sv_func_t, jit_code)));
    masm_branch64_imm(m, ASM_COND_EQUAL, scratch, 0, slow);
    asm_label_t lexical = masm_new_label(m), bound = masm_new_label(m);
    masm_branch_test32(m, ASM_COND_NON_ZERO, a[1], SV_CALL_IS_ARROW, lexical);
    masm_load64(m, a[1], opt_cg_call_slot(cg, 1));
    masm_jump(m, bound);
    masm_bind(m, lexical);
    masm_load64(m, a[1], asm_addr(a[0], (int32_t)offsetof(sv_closure_t, bound_this)));
    masm_bind(m, bound);
    masm_load64(m, a[3], asm_addr(a[0], (int32_t)offsetof(sv_closure_t, super_val)));
#if defined(__aarch64__)
    masm_move64(m, a[6], a[0]);
#else
    masm_store64(m, opt_cg_local(cg, 0), a[0]);
#endif
    masm_move_imm64(m, a[2], IR_VALUE_UNDEFINED);
    masm_load_effective_address(m, a[4], opt_cg_call_slot(cg, 2));
    masm_move_imm32(m, a[5], op->call.argc);
    masm_move64(m, a[0], cg->pinned.vm);
    masm_call_register(m, scratch);
    opt_cg_record_call(cg, node->frame_state, -1);
    masm_jump(m, done);
    masm_bind(m, slow);
    opt_cg_call_runtime(cg, node_id, (uint64_t)(uintptr_t)opt_rt_call);
    masm_bind(m, done);
  }
  opt_cg_branch_error(cg, node_id);
}

static void opt_cg_probe_property(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  const ir_op_t *op = &ir_node(cg->graph, node)->op;
  const sv_ic_entry_t *ic = &op->probe.func->ic_slots[op->probe.cache];
  asm_gpr_t object = opt_cg_input(cg, node, 1), output = opt_cg_output(cg, node);
  asm_gpr_t entry = opt_cg_temp(cg, node, 0), a = opt_cg_temp(cg, node, 1), limit = opt_cg_temp(cg, node, 2);
  asm_label_t miss = masm_new_label(m), overflow = masm_new_label(m), done = masm_new_label(m);
  masm_move_imm64(m, entry, (uint64_t)(uintptr_t)ic);
  masm_load8(m, a, asm_addr(entry, (int32_t)offsetof(sv_ic_entry_t, get_kind)));
  masm_branch32_imm(m, ASM_COND_NOT_EQUAL, a, SV_GF_IC_OWN, miss);
  masm_load64(m, limit, asm_addr(cg->pinned.vm, (int32_t)offsetof(sv_vm_t, js)));
  masm_load32(m, limit, asm_addr(limit, (int32_t)offsetof(ant_t, ic.epoch)));
  masm_load32(m, a, asm_addr(entry, (int32_t)offsetof(sv_ic_entry_t, epoch)));
  masm_branch32(m, ASM_COND_NOT_EQUAL, a, limit, miss);
  masm_load64(m, a, asm_addr(object, (int32_t)offsetof(ant_object_t, shape)));
  masm_branch64_memory(m, ASM_COND_NOT_EQUAL, asm_addr(entry, (int32_t)offsetof(sv_ic_entry_t, cached_shape)), a, miss);
  masm_load16(m, a, asm_addr(object, (int32_t)offsetof(ant_object_t, flags)));
  masm_branch_test32(m, ASM_COND_NON_ZERO, a, ANT_OBJECT_FLAG_EXOTIC, miss);
  masm_load32(m, a, asm_addr(entry, (int32_t)offsetof(sv_ic_entry_t, cached_index)));
  masm_load32(m, limit, asm_addr(object, (int32_t)offsetof(ant_object_t, prop_count)));
  masm_branch32(m, ASM_COND_ABOVE_OR_EQUAL, a, limit, miss);
  masm_load8(m, limit, asm_addr(object, (int32_t)offsetof(ant_object_t, inobj_limit)));
  masm_branch32(m, ASM_COND_ABOVE_OR_EQUAL, a, limit, overflow);
  masm_load64(m, output, asm_addr_indexed(object, a, ASM_SCALE_8, (int32_t)offsetof(ant_object_t, inobj)));
  masm_jump(m, done);
  masm_bind(m, overflow);
  masm_sub32(m, a, a, limit);
  masm_load64(m, limit, asm_addr(object, (int32_t)offsetof(ant_object_t, overflow_prop)));
  masm_branch64_imm(m, ASM_COND_EQUAL, limit, 0, miss);
  masm_load64(m, output, asm_addr_indexed(limit, a, ASM_SCALE_8, 0));
  masm_jump(m, done);
  masm_bind(m, miss);
  masm_move_imm64(m, output, IR_VALUE_EMPTY);
  masm_bind(m, done);
}

static void opt_cg_check_closure(opt_cg_t *cg, ir_node_id_t node) {
  masm_t *m = &cg->masm;
  asm_label_t exit = opt_cg_exit_site(cg, node);
  asm_gpr_t value = opt_cg_input(cg, node, 0), closure = opt_cg_temp(cg, node, 0), scratch = cg->pinned.scratch;
  masm_shr64_imm(m, scratch, value, NANBOX_TYPE_SHIFT);
  masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, (int64_t)OPT_FUNCTION_TAG, exit);
  masm_and64_imm(m, closure, value, NANBOX_DATA_MASK);
  masm_add64_imm(m, closure, closure, (int64_t)ant_cage_base());
  masm_load32(m, scratch, asm_addr(closure, (int32_t)offsetof(sv_closure_t, call_flags)));
  masm_branch_test32(m, ASM_COND_NON_ZERO, scratch, ~(uint32_t)(SV_CALL_IS_ARROW | SV_CALL_HAS_SUPER), exit);
  masm_load64(m, closure, asm_addr(closure, (int32_t)offsetof(sv_closure_t, func)));
  masm_move_imm64(m, scratch, (uint64_t)ir_node(cg->graph, node)->op.shared_data);
  masm_branch64(m, ASM_COND_NOT_EQUAL, closure, scratch, exit);
}

static bool opt_cg_frame_code(opt_cg_t *cg, ir_node_id_t node_id, const ir_node_t *node) {
  masm_t *m = &cg->masm;
  asm_gpr_t scratch = cg->pinned.scratch;
  switch (node->op.code) {
    case IR_OP_LOAD_SLOT: opt_cg_load_slot(cg, node_id, node->op.slot); break;
    case IR_OP_STORE_SLOT: opt_cg_store_slot(cg, node_id, node->op.slot); break;
    case IR_OP_INITIALIZE_FRAME: opt_cg_initialize_frame(cg, false); break;
    case IR_OP_ENSURE_FRAME_INITIALIZED: opt_cg_initialize_frame(cg, true); break;
    case IR_OP_LOAD_FRAME_FIELD:
      if (node->op.field == IR_FIELD_EXECUTABLE) {
        masm_load64(m, opt_cg_output(cg, node_id), opt_cg_entry_field(cg, offsetof(opt_entry_t, closure)));
        break;
      }
      if (node->op.field == IR_FIELD_CALLEE) {
        asm_gpr_t output = opt_cg_output(cg, node_id);
        masm_load64(m, output, opt_cg_entry_field(cg, offsetof(opt_entry_t, closure)));
        masm_sub64_imm(m, output, output, (int64_t)ant_cage_base());
        masm_and64_imm(m, output, output, NANBOX_DATA_MASK);
        masm_or64_imm(m, output, output, (int64_t)(NANBOX_PREFIX | (uint64_t)kTypeFunction << NANBOX_TYPE_SHIFT));
        break;
      }
      if (node->op.field != IR_FIELD_SUPER_VALUE) {
        opt_cg_fail(cg, "frame-field");
        break;
      }
      if (!node->inputs.len) masm_load64(m, opt_cg_output(cg, node_id), opt_cg_entry_field(cg, offsetof(opt_entry_t, super_val)));
      else {
        asm_gpr_t output = opt_cg_output(cg, node_id);
        masm_and64_imm(m, output, opt_cg_input(cg, node_id, 0), NANBOX_DATA_MASK);
        masm_add64_imm(m, output, output, (int64_t)ant_cage_base());
        masm_load64(m, output, asm_addr(output, (int32_t)offsetof(sv_closure_t, super_val)));
      }
      break;
    case IR_OP_ARGUMENT_COUNT:
      masm_load32(m, opt_cg_output(cg, node_id), opt_cg_entry_field(cg, offsetof(opt_entry_t, argc)));
      break;
    case IR_OP_LOAD_ARGUMENT: {
      asm_label_t exit = opt_cg_exit_site(cg, node_id);
      asm_gpr_t index = opt_cg_input(cg, node_id, 0), output = opt_cg_output(cg, node_id);
      masm_load32(m, scratch, opt_cg_entry_field(cg, offsetof(opt_entry_t, argc)));
      masm_branch32(m, ASM_COND_ABOVE_OR_EQUAL, index, scratch, exit);
      masm_load64(m, scratch, opt_cg_entry_field(cg, offsetof(opt_entry_t, args)));
      masm_load64(m, output, asm_addr_indexed(scratch, index, ASM_SCALE_8, 0));
      break;
    }
    case IR_OP_LOAD_UPVALUE_CELL: opt_cg_load_upvalue_cell(cg, node_id, node->op.index); break;
    case IR_OP_LOAD_UPVALUE:
    case IR_OP_CHECK_UPVALUE_INITIALIZED: opt_cg_load_upvalue(cg, node_id); break;
    case IR_OP_STORE_UPVALUE:
    case IR_OP_STORE_ENVIRONMENT_BINDING: opt_cg_store_upvalue(cg, node_id); break;
    case IR_OP_LOAD_ENVIRONMENT_BINDING:
      masm_load64(m, opt_cg_output(cg, node_id), asm_addr(opt_cg_input(cg, node_id, 0), (int32_t)offsetof(sv_upvalue_t, closed)));
      break;
    case IR_OP_CHECK_GLOBAL_LEXICALS: opt_cg_check_global_lexicals(cg, node_id, node->op.count); break;
    default: return false;
  }
  return true;
}

static bool opt_cg_int32_code(opt_cg_t *cg, ir_node_id_t node_id, const ir_node_t *node) {
  masm_t *m = &cg->masm;
  switch (node->op.code) {
    case IR_OP_CHECK_INT32: {
      asm_label_t exit = opt_cg_exit_site(cg, node_id);
      asm_gpr_t input = opt_cg_input(cg, node_id, 0);
      if (masm_has_js_convert()) {
        masm_move_gpr_to_double(m, opt_cg_lowering_fpr(0), input);
        masm_branch_js_convert_double_to_int32(m, opt_cg_output(cg, node_id), opt_cg_lowering_fpr(0), exit);
        break;
      }
      opt_cg_branch_not_number(cg, input, exit);
      masm_move_gpr_to_double(m, opt_cg_lowering_fpr(0), input);
      masm_branch_convert_double_to_int32(m, opt_cg_output(cg, node_id), opt_cg_lowering_fpr(0), exit, ASM_NEGATIVE_ZERO_FAIL);
      break;
    }
    case IR_OP_UNBOX_INT32:
      masm_move_gpr_to_double(m, opt_cg_lowering_fpr(0), opt_cg_input(cg, node_id, 0));
      masm_truncate_double_to_int64(m, opt_cg_output(cg, node_id), opt_cg_lowering_fpr(0));
      masm_move32(m, opt_cg_output(cg, node_id), opt_cg_output(cg, node_id));
      break;
    case IR_OP_BOX_INT32: opt_cg_box_int32(cg, opt_cg_output(cg, node_id), opt_cg_input(cg, node_id, 0)); break;
    case IR_OP_BOOL_TO_INT32: masm_move32(m, opt_cg_output(cg, node_id), opt_cg_input(cg, node_id, 0)); break;
    case IR_OP_INT32_BINARY: opt_cg_int32_binary(cg, node_id, node->op.binary); break;
    case IR_OP_INT32_WRAPPING_BINARY: opt_cg_int32_wrapping_binary(cg, node_id, node->op.binary); break;
    case IR_OP_INT32_COMPARE: {
      asm_cond_t cond = opt_cg_int32_condition(node->op.comparison);
      uint64_t bits;
      if (opt_cg_input_constant(cg, node_id, 1, &bits))
        masm_compare32_imm_set(m, cond, opt_cg_output(cg, node_id), opt_cg_input(cg, node_id, 0), (int32_t)(uint32_t)bits);
      else masm_compare32_set(m, cond, opt_cg_output(cg, node_id), opt_cg_input(cg, node_id, 0), opt_cg_input(cg, node_id, 1));
      break;
    }
    case IR_OP_INT32_MIN:
    case IR_OP_INT32_MAX: {
      asm_gpr_t lhs = opt_cg_input(cg, node_id, 0), rhs = opt_cg_input(cg, node_id, 1);
      asm_cond_t cond = node->op.code == IR_OP_INT32_MIN ? ASM_COND_LESS_THAN : ASM_COND_GREATER_THAN;
      masm_select32(m, cond, lhs, rhs, opt_cg_output(cg, node_id), lhs, rhs);
      break;
    }
    case IR_OP_FLOAT64_TO_INT32: opt_cg_float64_to_int32(cg, node_id); break;
    case IR_OP_UINT32_SHIFT_RIGHT: opt_cg_uint32_shift_right(cg, node_id); break;
    default: return false;
  }
  return true;
}

static void opt_cg_round_double(opt_cg_t *cg, asm_fpr_t output, asm_fpr_t input) {
  masm_t *m = &cg->masm;
  asm_fpr_t ceiling = opt_cg_lowering_fpr(0), probe = opt_cg_lowering_fpr(1);
  asm_label_t done = masm_new_label(m);
  masm_ceil_double(m, ceiling, input);
  masm_move_double_imm(m, probe, 0.5);
  masm_sub_double(m, probe, ceiling, probe);
  masm_branch_double(m, ASM_DCOND_LESS_THAN_OR_EQUAL_OR_UNORDERED, probe, input, done);
  masm_move_double_imm(m, probe, 1.0);
  masm_sub_double(m, ceiling, ceiling, probe);
  masm_bind(m, done);
  masm_move_double(m, output, ceiling);
}

static bool opt_cg_float64_code(opt_cg_t *cg, ir_node_id_t node_id, const ir_node_t *node) {
  masm_t *m = &cg->masm;
  asm_gpr_t scratch = cg->pinned.scratch;
  switch (node->op.code) {
    case IR_OP_CHECK_NUMBER: {
      asm_label_t exit = opt_cg_exit_site(cg, node_id);
      asm_gpr_t input = opt_cg_input(cg, node_id, 0);
      opt_cg_branch_not_number(cg, input, exit);
      masm_move_gpr_to_double(m, opt_cg_float_output(cg, node_id), input);
      break;
    }
    case IR_OP_UNBOX_DOUBLE: masm_move_gpr_to_double(m, opt_cg_float_output(cg, node_id), opt_cg_input(cg, node_id, 0)); break;
    case IR_OP_BOX_FLOAT64:
      opt_cg_box_float(cg, opt_cg_output(cg, node_id), opt_cg_float_input(cg, node_id, 0),
        opt_float_may_be_negative_nan(cg->graph, node->inputs.items[0]));
      break;
    case IR_OP_INT32_TO_FLOAT64: masm_convert_int32_to_double(m, opt_cg_float_output(cg, node_id), opt_cg_input(cg, node_id, 0)); break;
    case IR_OP_UINT32_TO_FLOAT64:
      masm_move32(m, scratch, opt_cg_input(cg, node_id, 0));
      masm_convert_int64_to_double(m, opt_cg_float_output(cg, node_id), scratch);
      break;
    case IR_OP_FLOAT64_MIN:
    case IR_OP_FLOAT64_MAX: {
      asm_fpr_t lhs = opt_cg_float_input(cg, node_id, 0), rhs = opt_cg_float_input(cg, node_id, 1), output = opt_cg_float_output(cg, node_id);
      if (node->op.code == IR_OP_FLOAT64_MIN) masm_min_double(m, output, lhs, rhs);
      else masm_max_double(m, output, lhs, rhs);
      break;
    }
    case IR_OP_FLOAT64_BINARY: {
      asm_fpr_t lhs = opt_cg_float_input(cg, node_id, 0), rhs = opt_cg_float_input(cg, node_id, 1), output = opt_cg_float_output(cg, node_id);
      switch (node->op.binary) {
        case IR_BINARY_ADD: masm_add_double(m, output, lhs, rhs); break;
        case IR_BINARY_SUB: masm_sub_double(m, output, lhs, rhs); break;
        case IR_BINARY_MUL: masm_mul_double(m, output, lhs, rhs); break;
        case IR_BINARY_DIV: masm_div_double(m, output, lhs, rhs); break;
        default: opt_cg_fail(cg, "float64-binary"); break;
      }
      break;
    }
    case IR_OP_FLOAT64_UNARY: {
      asm_fpr_t input = opt_cg_float_input(cg, node_id, 0), output = opt_cg_float_output(cg, node_id);
      switch (node->op.unary) {
        case IR_UNARY_NEGATE: masm_neg_double(m, output, input); break;
        case IR_UNARY_ABS: masm_abs_double(m, output, input); break;
        case IR_UNARY_SQRT: masm_sqrt_double(m, output, input); break;
        case IR_UNARY_FLOOR: masm_floor_double(m, output, input); break;
        case IR_UNARY_CEIL: masm_ceil_double(m, output, input); break;
        case IR_UNARY_TRUNC: masm_trunc_double(m, output, input); break;
        case IR_UNARY_ROUND: opt_cg_round_double(cg, output, input); break;
        default: opt_cg_fail(cg, "float64-unary"); break;
      }
      break;
    }
    case IR_OP_CALL_FLOAT64_FUNCTION: {
      const ant_math_double_info_t *info = &ant_math_doubles[node->op.function];
      for (uint32_t i = 0; i < node->inputs.len; i++) masm_move_double(m, opt_cg_lowering_fpr(i), opt_cg_float_input(cg, node_id, i));
      masm_call_absolute(m, (uint64_t)(uintptr_t)info->fn);
      masm_move_double(m, opt_cg_float_output(cg, node_id), opt_cg_lowering_fpr(0));
      break;
    }
    case IR_OP_FLOAT64_COMPARE:
      masm_compare_double_set(m, opt_cg_double_condition(node->op.comparison), opt_cg_output(cg, node_id),
        opt_cg_float_input(cg, node_id, 0), opt_cg_float_input(cg, node_id, 1));
      break;
    default: return false;
  }
  return true;
}

static bool opt_cg_value_code(opt_cg_t *cg, ir_node_id_t node_id, const ir_node_t *node) {
  masm_t *m = &cg->masm;
  asm_gpr_t scratch = cg->pinned.scratch;
  switch (node->op.code) {
    case IR_OP_CHECK_IDENTITY_COMPARABLE: opt_cg_check_identity_comparable(cg, node_id); break;
    case IR_OP_CHECK_CLOSURE: opt_cg_check_closure(cg, node_id); break;
    case IR_OP_ASSUME_VALID: {
      asm_label_t exit = opt_cg_exit_site(cg, node_id);
      opt_vec_push(&cg->arena, &cg->invalidations, ((opt_cg_invalidation_t){.offset = masm_patchable_nop(m), .exit = exit}));
      break;
    }
    case IR_OP_CHECK_VALUE: {
      asm_label_t exit = opt_cg_exit_site(cg, node_id);
      masm_move_imm64(m, scratch, node->op.check_value.expected);
      masm_branch64(m, ASM_COND_NOT_EQUAL, opt_cg_input(cg, node_id, 0), scratch, exit);
      break;
    }
    case IR_OP_CHECK_STRING: {
      asm_label_t exit = opt_cg_exit_site(cg, node_id);
      masm_sar64_imm(m, scratch, opt_cg_input(cg, node_id, 0), NANBOX_TYPE_SHIFT);
      masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, OPT_STRING_TAG_SIGNED, exit);
      break;
    }
    case IR_OP_CHECK_OBJECT: {
      asm_label_t exit = opt_cg_exit_site(cg, node_id);
      masm_move_imm64(m, scratch, -(NANBOX_PREFIX + 1));
      masm_add64(m, scratch, opt_cg_input(cg, node_id, 0), scratch);
      masm_branch64_imm(m, ASM_COND_ABOVE_OR_EQUAL, scratch, (int64_t)NANBOX_DATA_MASK, exit);
      break;
    }
    case IR_OP_BOX_BOOL: {
      asm_gpr_t output = opt_cg_output(cg, node_id);
      masm_move32(m, output, opt_cg_input(cg, node_id, 0));
      masm_or64_imm(m, output, output, ANT_BOOL_TAG);
      break;
    }
    case IR_OP_TAGGED_EQUALS: {
      asm_cond_t cond = node->op.equal ? ASM_COND_EQUAL : ASM_COND_NOT_EQUAL;
      uint64_t bits;
      if (opt_cg_input_constant(cg, node_id, 1, &bits))
        masm_compare64_imm_set(m, cond, opt_cg_output(cg, node_id), opt_cg_input(cg, node_id, 0), (int64_t)bits);
      else masm_compare64_set(m, cond, opt_cg_output(cg, node_id), opt_cg_input(cg, node_id, 0), opt_cg_input(cg, node_id, 1));
      break;
    }
    case IR_OP_STRICT_EQUALS: {
      asm_gpr_t lhs = opt_cg_input(cg, node_id, 0), rhs = opt_cg_input(cg, node_id, 1), output = opt_cg_output(cg, node_id);
      asm_label_t call = masm_new_label(m), done = masm_new_label(m);
      masm_branch64(m, ASM_COND_NOT_EQUAL, lhs, rhs, call);
      masm_move_imm64(m, scratch, NANBOX_PREFIX);
      masm_branch64(m, ASM_COND_BELOW_OR_EQUAL, lhs, scratch, call);
      masm_move_imm32(m, output, 1);
      masm_jump(m, done);
      masm_bind(m, call);
      masm_move64(m, MASM_ARGUMENT_GPRS[2], cg->pinned.vm);
      masm_call_absolute(m, (uint64_t)(uintptr_t)opt_rt_strict_equals);
      masm_bind(m, done);
      if (!node->op.equal) masm_xor32_imm(m, output, output, 1);
      break;
    }
    case IR_OP_LOOSE_IDENTITY_EQUALS: opt_cg_loose_identity_equals(cg, node_id, node->op.equal); break;
    case IR_OP_TO_BOOLEAN:
      masm_move64(m, MASM_ARGUMENT_GPRS[1], cg->pinned.vm);
      masm_call_absolute(m, (uint64_t)(uintptr_t)opt_rt_to_boolean);
      break;
    case IR_OP_IS_NULLISH: opt_cg_is_nullish(cg, node_id, node->op.equal); break;
    case IR_OP_TYPEOF_IS: opt_cg_typeof_is(cg, node_id); break;
    default: return false;
  }
  return true;
}

static bool opt_cg_object_code(opt_cg_t *cg, ir_node_id_t node_id, const ir_node_t *node) {
  masm_t *m = &cg->masm;
  asm_gpr_t scratch = cg->pinned.scratch;
  switch (node->op.code) {
    case IR_OP_OBJECT_ADDRESS: {
      asm_gpr_t input = opt_cg_input(cg, node_id, 0), output = opt_cg_output(cg, node_id);
      opt_cg_object_kinds_t kinds = {.label = masm_new_label(m), .done = masm_new_label(m), .exit = opt_cg_exit_site(cg, node_id), .output = output};
      masm_shr64_imm(m, scratch, input, NANBOX_TYPE_SHIFT);
      masm_and64_imm(m, output, input, NANBOX_DATA_MASK);
      masm_add64_imm(m, output, output, (int64_t)ant_cage_base());
      masm_xor64_imm(m, scratch, scratch, OPT_OBJECT_TAG);
      masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, 0, kinds.label);
      masm_bind(m, kinds.done);
      opt_vec_push(&cg->arena, &cg->object_kinds, kinds);
      break;
    }
    case IR_OP_CELL_ADDRESS: {
      asm_gpr_t output = opt_cg_output(cg, node_id);
      masm_and64_imm(m, output, opt_cg_input(cg, node_id, 0), NANBOX_DATA_MASK);
      masm_add64_imm(m, output, output, (int64_t)ant_cage_base());
      break;
    }
    case IR_OP_CHECK_SHAPE: {
      if (node->op.shapes.len != 1) {
        opt_cg_fail(cg, "shapes");
        break;
      }
      asm_label_t exit = opt_cg_exit_site(cg, node_id);
      asm_gpr_t object = opt_cg_input(cg, node_id, 1);
      masm_load64(m, scratch, asm_addr(object, (int32_t)offsetof(ant_object_t, shape)));
      masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, (int64_t)node->op.shapes.items[0].shape, exit);
      masm_load16(m, scratch, asm_addr(object, (int32_t)offsetof(ant_object_t, flags)));
      masm_branch_test32(m, ASM_COND_NON_ZERO, scratch, ANT_OBJECT_FLAG_EXOTIC, exit);
      break;
    }
    case IR_OP_LOAD_NAMED: masm_load64(m, opt_cg_output(cg, node_id), opt_cg_named_slot(opt_cg_input(cg, node_id, 1), node->op.offset)); break;
    case IR_OP_LOAD_NAMED_STORAGE: {
      asm_label_t exit = opt_cg_exit_site(cg, node_id);
      asm_gpr_t object = opt_cg_input(cg, node_id, 1), output = opt_cg_output(cg, node_id);
      masm_branch32_memory_imm(m, ASM_COND_BELOW, asm_addr(object, (int32_t)offsetof(ant_object_t, prop_count)), (int32_t)node->op.count, exit);
      masm_load64(m, output, asm_addr(object, (int32_t)offsetof(ant_object_t, overflow_prop)));
      masm_branch64_imm(m, ASM_COND_EQUAL, output, 0, exit);
      break;
    }
    case IR_OP_STORE_NAMED: opt_cg_store_named(cg, node_id, node->op.offset); break;
    case IR_OP_ADD_NAMED: opt_cg_add_named(cg, node_id); break;
    case IR_OP_CAN_ADD_NAMED: opt_cg_can_add_named(cg, node_id); break;
    case IR_OP_ALLOCATE_OBJECT: opt_cg_allocate_object(cg, node_id); break;
    case IR_OP_INITIALIZE_NAMED: opt_cg_initialize_named(cg, node_id); break;
    case IR_OP_LOAD_PROTOTYPE:
      masm_load64(m, opt_cg_output(cg, node_id), asm_addr(opt_cg_input(cg, node_id, 0), (int32_t)offsetof(ant_object_t, proto)));
      break;
    case IR_OP_HAS_IN_PROTOTYPE_CHAIN: opt_cg_has_in_prototype_chain(cg, node_id); break;
    case IR_OP_PROBE_PROPERTY_CACHE: opt_cg_probe_property(cg, node_id); break;
    default: return false;
  }
  return true;
}

static bool opt_cg_string_code(opt_cg_t *cg, ir_node_id_t node_id, const ir_node_t *node) {
  masm_t *m = &cg->masm;
  asm_gpr_t scratch = cg->pinned.scratch;
  switch (node->op.code) {
    case IR_OP_STRING_LENGTH:
      masm_load32(m, opt_cg_output(cg, node_id), asm_addr(opt_cg_input(cg, node_id, 0), (int32_t)offsetof(ant_flat_string_t, len)));
      break;
    case IR_OP_LOAD_STRING_CODE_UNIT:
      masm_move32(m, scratch, opt_cg_input(cg, node_id, 1));
      masm_add64(m, scratch, scratch, opt_cg_input(cg, node_id, 0));
      masm_load8(m, opt_cg_output(cg, node_id), asm_addr(scratch, (int32_t)offsetof(ant_flat_string_t, bytes)));
      break;
    case IR_OP_STRING_ADDRESS: {
      asm_gpr_t output = opt_cg_output(cg, node_id), cached = opt_cg_temp(cg, node_id, 0);
      asm_label_t done = masm_new_label(m);
      masm_and64_imm(m, output, opt_cg_input(cg, node_id, 0), NANBOX_DATA_MASK);
      masm_add64_imm(m, output, output, (int64_t)ant_cage_base());
      masm_and64_imm(m, scratch, output, (int64_t)STR_HEAP_TAG_MASK);
      masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, (int64_t)STR_HEAP_TAG_ROPE, done);
      masm_load64(m, cached, asm_addr(output, (int32_t)(offsetof(ant_rope_heap_t, cached) - STR_HEAP_TAG_ROPE)));
      masm_shr64_imm(m, scratch, cached, NANBOX_TYPE_SHIFT);
      masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, (int64_t)((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | kTypeString), done);
      masm_branch_test64(m, ASM_COND_NON_ZERO, cached, (int64_t)STR_HEAP_TAG_MASK, done);
      masm_and64_imm(m, output, cached, NANBOX_DATA_MASK);
      masm_add64_imm(m, output, output, (int64_t)ant_cage_base());
      masm_bind(m, done);
      break;
    }
    case IR_OP_CONCATENATE_STRINGS: opt_cg_concatenate_strings(cg, node_id); break;
    case IR_OP_STRING_BUILDER_APPEND: opt_cg_string_builder_append(cg, node_id); break;
    default: return false;
  }
  return true;
}

static bool opt_cg_element_code(opt_cg_t *cg, ir_node_id_t node_id, const ir_node_t *node) {
  masm_t *m = &cg->masm;
  switch (node->op.code) {
    case IR_OP_CHECK_ELEMENTS: opt_cg_check_elements(cg, node_id, node->op.elements); break;
    case IR_OP_LOAD_ELEMENTS_LENGTH:
      masm_load32(m, opt_cg_output(cg, node_id), asm_addr(opt_cg_input(cg, node_id, 0), (int32_t)offsetof(ant_object_t, u.array.len)));
      break;
    case IR_OP_LOAD_ELEMENTS_CAPACITY:
      masm_load32(m, opt_cg_output(cg, node_id), asm_addr(opt_cg_input(cg, node_id, 0), (int32_t)offsetof(ant_object_t, u.array.cap)));
      break;
    case IR_OP_LOAD_TYPED_ARRAY_LENGTH: {
      asm_gpr_t out = opt_cg_output(cg, node_id);
      masm_load64(m, out, asm_addr(opt_cg_input(cg, node_id, 0), (int32_t)offsetof(ant_object_t, native_ptr)));
      masm_load32(m, out, asm_addr(out, (int32_t)offsetof(TypedArrayData, length)));
      break;
    }
    case IR_OP_CHECK_BOUNDS: opt_cg_check_bounds(cg, node_id); break;
    case IR_OP_LOAD_ELEMENT_AT: opt_cg_load_element(cg, node_id); break;
    case IR_OP_STORE_ELEMENT_AT: opt_cg_store_element(cg, node_id); break;
    case IR_OP_CHECK_NOT_HOLE: opt_cg_check_not_hole(cg, node_id); break;
    case IR_OP_CAN_ADD_ELEMENT: opt_cg_can_add_element(cg, node_id); break;
    case IR_OP_APPEND_ELEMENT: opt_cg_append_element(cg, node_id); break;
    case IR_OP_POP_ELEMENT: opt_cg_pop_element(cg, node_id); break;
    case IR_OP_ALLOCATE_ARRAY: opt_cg_allocate_array(cg, node_id); break;
    case IR_OP_INITIALIZE_ELEMENT: opt_cg_initialize_element(cg, node_id); break;
    default: return false;
  }
  return true;
}

static bool opt_cg_call_code(opt_cg_t *cg, ir_node_id_t node_id, const ir_node_t *node) {
  masm_t *m = &cg->masm;
  switch (node->op.code) {
    case IR_OP_GENERIC: opt_cg_generic(cg, node_id); break;
    case IR_OP_CALL_DIRECT:
    case IR_OP_CALL_NATIVE:
    case IR_OP_CALL_CONSTRUCT: opt_cg_call(cg, node_id); break;
    case IR_OP_SLOW_PATH_OUTPUT: masm_load64(m, opt_cg_output(cg, node_id), opt_cg_call_slot(cg, node->op.output_index)); break;
    case IR_OP_NEW_THIS:
      masm_move64(m, MASM_ARGUMENT_GPRS[0], cg->pinned.vm);
      masm_call_absolute(m, (uint64_t)(uintptr_t)opt_rt_new_this);
      opt_cg_record_call(cg, node->frame_state, -1);
      break;
    case IR_OP_THROW:
      masm_move64(m, MASM_ARGUMENT_GPRS[1], cg->pinned.vm);
      masm_call_absolute(m, (uint64_t)(uintptr_t)opt_rt_throw);
      masm_jump(m, opt_cg_throw_target(cg, node_id, cg->throw_label));
      break;
    default: return false;
  }
  return true;
}

static void opt_cg_shape_switch(opt_cg_t *cg, ir_node_id_t node_id, const ir_node_t *node) {
  masm_t *m = &cg->masm;
  asm_gpr_t scratch = cg->pinned.scratch;
  asm_label_t exit = opt_cg_exit_site(cg, node_id);
  asm_gpr_t object = opt_cg_input(cg, node_id, 1);
  const ir_shape_case_t *last = &node->op.cases.items[node->op.cases.len - 1];
  asm_label_t other = last->check.shape ? exit : opt_cg_label(cg, last->block);
  masm_load16(m, scratch, asm_addr(object, (int32_t)offsetof(ant_object_t, flags)));
  masm_branch_test32(m, ASM_COND_NON_ZERO, scratch, ANT_OBJECT_FLAG_EXOTIC, other);
  masm_load64(m, scratch, asm_addr(object, (int32_t)offsetof(ant_object_t, shape)));
  for (uint32_t i = 0; i < node->op.cases.len; i++) {
    const ir_shape_case_t *shape_case = &node->op.cases.items[i];
    if (shape_case->check.shape) masm_branch64_imm(m, ASM_COND_EQUAL, scratch, (int64_t)shape_case->check.shape, opt_cg_label(cg, shape_case->block));
    else exit = opt_cg_label(cg, shape_case->block);
  }
  masm_jump(m, exit);
}

static bool opt_cg_control_code(opt_cg_t *cg, ir_node_id_t node_id, const ir_node_t *node) {
  masm_t *m = &cg->masm;
  switch (node->op.code) {
    case IR_OP_JUMP: opt_cg_jump_to(cg, node->op.target); break;
    case IR_OP_BRANCH: opt_cg_branch(cg, node_id, node->op.branch.condition, node->op.branch.if_true, node->op.branch.if_false); break;
    case IR_OP_SHAPE_SWITCH: opt_cg_shape_switch(cg, node_id, node); break;
    case IR_OP_BRANCH_TRUTHY: opt_cg_branch_truthy(cg, node_id, node->op.truthy.if_true, node->op.truthy.if_false, node->op.truthy.fallback); break;
    case IR_OP_RETURN: opt_cg_return_from_frame(cg); break;
    case IR_OP_EXIT: masm_jump(m, opt_cg_exit_site(cg, node_id)); break;
    case IR_OP_UNREACHABLE: masm_unreachable(m); break;
    default: return false;
  }
  return true;
}

static void opt_cg_node_code(opt_cg_t *cg, ir_node_id_t node_id) {
  const ir_node_t *node = ir_node(cg->graph, node_id);
  const ra_node_allocation_t *allocation = opt_cg_node(cg, node_id);
  for (uint32_t i = 0; i < allocation->moves_before.len; i++) opt_cg_move(cg, allocation->moves_before.items[i]);

  bool handled = opt_cg_frame_code(cg, node_id, node) || opt_cg_int32_code(cg, node_id, node) ||
    opt_cg_float64_code(cg, node_id, node) || opt_cg_value_code(cg, node_id, node) ||
    opt_cg_object_code(cg, node_id, node) || opt_cg_string_code(cg, node_id, node) ||
    opt_cg_element_code(cg, node_id, node) || opt_cg_call_code(cg, node_id, node) ||
    opt_cg_control_code(cg, node_id, node);
  if (!handled) opt_cg_fail(cg, ir_op_name(node->op.code));

  if (allocation->has_output && allocation->output.kind == RA_LOC_REGISTER && allocation->spill_slot != IR_NONE)
    opt_cg_move(cg, (ra_move_t){.from = allocation->output, .to = ra_stack(allocation->spill_slot)});
}

static bool opt_cg_hole_then_int32(opt_cg_t *cg, ir_node_id_t hole, ir_node_id_t check) {
  const ir_node_t *hole_node = ir_node(cg->graph, hole), *check_node = ir_node(cg->graph, check);
  if (!masm_has_js_convert() || hole_node->op.code != IR_OP_CHECK_NOT_HOLE || check_node->op.code != IR_OP_CHECK_INT32) return false;
  if (hole_node->inputs.items[0] != check_node->inputs.items[0]) return false;
  const ra_node_allocation_t *hole_alloc = opt_cg_node(cg, hole), *check_alloc = opt_cg_node(cg, check);
  if (hole_alloc->has_output || hole_alloc->moves_before.len || check_alloc->moves_before.len) return false;
  if (!check_alloc->has_output || check_alloc->output.kind != RA_LOC_REGISTER || opt_cg_is_fpr(check_alloc->output.reg)) return false;
  opt_exit_cursor_t cursor = opt_exit_cursor(opt_exit_table_view(&cg->alloc->exit_table), hole_alloc->exits);
  for (const opt_exit_value_t *at; (at = opt_exit_next(&cursor));)
    if (at->kind == RA_LOC_REGISTER && at->reg == check_alloc->output.reg) return false;
  masm_t *m = &cg->masm;
  opt_cg_element_int32_t fused = {
    .label = masm_new_label(m), .hole_exit = opt_cg_exit_site(cg, hole), .int32_exit = opt_cg_exit_site(cg, check),
    .value = opt_cg_input(cg, check, 0),
  };
  masm_move_gpr_to_double(m, opt_cg_lowering_fpr(0), fused.value);
  masm_branch_js_convert_double_to_int32(m, opt_cg_output(cg, check), opt_cg_lowering_fpr(0), fused.label);
  opt_vec_push(&cg->arena, &cg->element_int32s, fused);
  if (check_alloc->spill_slot != IR_NONE) opt_cg_move(cg, (ra_move_t){.from = check_alloc->output, .to = ra_stack(check_alloc->spill_slot)});
  return true;
}

static void opt_cg_block(opt_cg_t *cg, ir_block_id_t block) {
  const ir_block_t *data = ir_block(cg->graph, block);
  cg->block = block;
  masm_bind(&cg->masm, cg->labels[block]);
  for (uint32_t i = 0; i < data->phis.len; i++) {
    uint32_t slot = ra_phi_spill_store(cg->graph, cg->alloc, data->phis.items[i]);
    if (slot != IR_NONE) opt_cg_move(cg, (ra_move_t){.from = opt_cg_node(cg, data->phis.items[i])->output, .to = ra_stack(slot)});
  }
  for (uint32_t i = 0; i < data->body.len; i++) {
    if (cg->marks_wanted) opt_vec_push(&cg->arena, &cg->marks, ((opt_cg_mark_t){(uint32_t)masm_offset(&cg->masm), data->body.items[i]}));
    if (i + 1 < data->body.len && opt_cg_hole_then_int32(cg, data->body.items[i], data->body.items[i + 1])) i++;
    else opt_cg_node_code(cg, data->body.items[i]);
  }
  const ra_moves_t *moves = &cg->alloc->block_end_moves[block];
  for (uint32_t i = 0; i < moves->len; i++) opt_cg_move(cg, moves->items[i]);
  opt_cg_node_code(cg, data->control);
}

static ir_block_id_t opt_cg_first_block(const opt_cg_t *cg) {
  for (ir_block_id_t block = 0; block < ir_block_count(cg->graph); block++)
    if (cg->landing[block] == block) return block;
  return IR_NONE;
}

static void opt_cg_prologue(opt_cg_t *cg) {
  masm_t *m = &cg->masm;
  const asm_gpr_t *args = MASM_ARGUMENT_GPRS;
  uint32_t arg_count = sizeof MASM_ARGUMENT_GPRS / sizeof *MASM_ARGUMENT_GPRS;
  asm_gpr_t scratch = cg->pinned.scratch;
  sv_func_t *func = cg->graph->func;
  masm_emit_prologue(m, &cg->frame);
  masm_move64(m, cg->pinned.vm, args[0]);
  masm_load64(m, scratch, asm_addr(args[0], (int32_t)offsetof(sv_vm_t, js)));
  masm_branch_if_stack_pointer_below(m, asm_addr(scratch, (int32_t)offsetof(ant_t, cstk.floor)), cg->overflow);
  masm_store64(m, opt_cg_entry_field(cg, offsetof(opt_entry_t, this_val)), args[1]);
  masm_store64(m, opt_cg_entry_field(cg, offsetof(opt_entry_t, new_target)), args[2]);
  masm_store64(m, opt_cg_entry_field(cg, offsetof(opt_entry_t, super_val)), args[3]);
  masm_store64(m, opt_cg_entry_field(cg, offsetof(opt_entry_t, args)), args[4]);
  masm_sign_extend32_to_64(m, args[5], args[5]);
  masm_store64(m, opt_cg_entry_field(cg, offsetof(opt_entry_t, argc)), args[5]);
  masm_store_imm64(m, opt_cg_entry_field(cg, offsetof(opt_entry_t, frame)), 0);
  if (arg_count > 6) masm_store64(m, opt_cg_entry_field(cg, offsetof(opt_entry_t, closure)), args[6]);
  else {
    masm_load64(m, scratch, opt_cg_local(cg, cg->frame.caller_stack_offset));
    masm_store64(m, opt_cg_entry_field(cg, offsetof(opt_entry_t, closure)), scratch);
  }
  masm_move64(m, cg->pinned.frame, args[4]);

  bool uses_this = false;
  for (uint32_t i = 0; i < ir_node_count(cg->graph); i++) {
    const ir_node_t *node = ir_node(cg->graph, i);
    uses_this |= node->op.code == IR_OP_LOAD_SLOT && node->op.slot == OPT_THIS_SLOT;
  }
  if (uses_this && !func->is_strict && !func->is_arrow) {
    asm_label_t normalized = masm_new_label(m), normalize = masm_new_label(m);
    masm_branch64_imm(m, ASM_COND_EQUAL, args[1], (int64_t)NANBOX_PREFIX, normalize);
    masm_shr64_imm(m, scratch, args[1], NANBOX_TYPE_SHIFT);
    masm_branch64_imm(m, ASM_COND_EQUAL, scratch, (int64_t)OPT_OBJECT_TAG, normalized);
    masm_bind(m, normalize);
    masm_move64(m, args[2], args[1]);
    masm_move_imm64(m, args[1], (uint64_t)(uintptr_t)func);
    masm_move64(m, args[0], cg->pinned.vm);
    masm_call_absolute(m, (uint64_t)(uintptr_t)opt_rt_normalize_this);
    masm_store64(m, opt_cg_entry_field(cg, offsetof(opt_entry_t, this_val)), MASM_RETURN_GPRS[0]);
    masm_load64(m, args[5], opt_cg_entry_field(cg, offsetof(opt_entry_t, argc)));
    masm_bind(m, normalized);
  }

  uint32_t params = cg->layout->number_of_arguments;
  masm_load8(m, scratch, asm_addr(cg->pinned.vm, (int32_t)(offsetof(sv_vm_t, jit.osr) + offsetof(sv_jit_osr_t, active))));
  masm_branch32_imm(m, ASM_COND_NOT_EQUAL, scratch, 0, cg->osr_entry);
  if (params) masm_branch64_imm(m, ASM_COND_LESS_THAN, args[5], params, cg->copy_params);
  masm_bind(m, cg->have_params);
  masm_store64(m, opt_cg_entry_field(cg, offsetof(opt_entry_t, params)), cg->pinned.frame);
  ir_block_id_t entry = cg->landing[0];
  if (entry != opt_cg_first_block(cg)) masm_jump(m, cg->labels[entry]);
}

static void opt_cg_entry_tails(opt_cg_t *cg, asm_label_t retry) {
  masm_t *m = &cg->masm;
  asm_gpr_t scratch = cg->pinned.scratch;
  uint32_t params = cg->layout->number_of_arguments;
  if (params) {
    masm_bind(m, cg->copy_params);
    asm_gpr_t value = MASM_ARGUMENT_GPRS[0], argc = MASM_ARGUMENT_GPRS[5];
    for (uint32_t i = 0; i < params; i++) {
      asm_label_t missing = masm_new_label(m);
      masm_move_imm64(m, value, IR_VALUE_UNDEFINED);
      masm_branch64_imm(m, ASM_COND_LESS_THAN_OR_EQUAL, argc, i, missing);
      masm_load64(m, value, asm_addr(cg->pinned.frame, (int32_t)(8 * i)));
      masm_bind(m, missing);
      masm_store64(m, opt_cg_local(cg, cg->copy_off + 8 * i), value);
    }
    masm_load_effective_address(m, cg->pinned.frame, opt_cg_local(cg, cg->copy_off));
    masm_jump(m, cg->have_params);
  }

  masm_bind(m, cg->osr_entry);
  masm_store64(m, opt_cg_entry_field(cg, offsetof(opt_entry_t, params)), cg->pinned.frame);
  if (cg->has_frame && cg->graph->osr_entries.len) {
    masm_move64(m, MASM_ARGUMENT_GPRS[0], cg->pinned.vm);
    masm_load_effective_address(m, MASM_ARGUMENT_GPRS[1], opt_cg_entry_field(cg, 0));
    masm_call_absolute(m, (uint64_t)(uintptr_t)opt_rt_borrow_frame);
  }
  masm_load32(m, scratch, asm_addr(cg->pinned.vm, (int32_t)(offsetof(sv_vm_t, jit.osr) + offsetof(sv_jit_osr_t, bc_offset))));
  asm_label_t *entries = opt_arena_new(&cg->arena, asm_label_t, cg->graph->osr_entries.len + 1);
  for (uint32_t i = 0; i < cg->graph->osr_entries.len; i++) {
    entries[i] = masm_new_label(m);
    masm_branch32_imm(m, ASM_COND_EQUAL, scratch, (int32_t)cg->graph->osr_entries.items[i].bc_off, entries[i]);
  }
  masm_jump(m, retry);
  for (uint32_t i = 0; i < cg->graph->osr_entries.len; i++) {
    masm_bind(m, entries[i]);
    masm_move_imm32(m, scratch, 0);
    masm_store8(m, asm_addr(cg->pinned.vm, (int32_t)(offsetof(sv_vm_t, jit.osr) + offsetof(sv_jit_osr_t, active))), scratch);
    masm_jump(m, opt_cg_label(cg, cg->graph->osr_entries.items[i].block));
  }
}

static uint64_t opt_cg_site_bits(const opt_site_value_t *value, const uint64_t *dump, const uint64_t *spills) {
  switch (value->kind) {
    case OPT_SITE_REGISTER: return value->reg < OPT_FPR_BASE ? dump[value->reg] : dump[OPT_DUMP_GPRS + value->reg - OPT_FPR_BASE];
    case OPT_SITE_STACK: return spills[value->bits];
    default: return value->bits;
  }
}

static ant_value_t opt_cg_materialize_arguments(sv_vm_t *vm, ant_value_t *slots, uint32_t count, ant_value_t callee, ant_value_t *args, uint32_t argc) {
  ant_value_t arguments = js_mkundef();
  for (uint32_t i = 0; i < count; i++) {
    int kind = ir_value_virtual_arguments_kind(slots[i]);
    if (kind < 0) continue;
    if (!is_object_type(arguments)) arguments = opt_rt_arguments_object(vm, callee, args, (int)argc, kind == 0);
    slots[i] = arguments;
  }
  return arguments;
}

static bool opt_cg_completion_value(const opt_site_value_t *value) {
  return value->slot >= OPT_COMPLETION_SLOT && value->slot < OPT_CALLEE_SLOT && value->repr != OPT_REPR_POINTER;
}

static uint32_t opt_cg_site_finally_count(const opt_code_t *code, const opt_site_t *site) {
  opt_exit_cursor_t cursor = opt_exit_cursor((opt_exit_view_t){code->values, code->chunks, code->refs}, site->values);
  for (const opt_site_value_t *value; (value = opt_exit_next(&cursor));) {
    if (!opt_cg_completion_value(value)) continue;
    int count = sv_func_finally_index(code->func, code->func->code_len);
    return count > 0 ? (uint32_t)count : 0;
  }
  return 0;
}

static sv_jit_finally_t *opt_cg_site_finally(const opt_code_t *code, const opt_site_t *site, const uint64_t *dump, const uint64_t *spills, sv_jit_finally_t *out, uint32_t count) {
  if (!count) return NULL;
  for (uint32_t i = 0; i < count; i++) out[i] = (sv_jit_finally_t){.kind = SV_COMPLETION_NONE, .value = js_mkundef()};
  sv_func_t *func = code->func;
  bool pending = false;
  opt_exit_cursor_t cursor = opt_exit_cursor((opt_exit_view_t){code->values, code->chunks, code->refs}, site->values);
  for (const opt_site_value_t *value; (value = opt_exit_next(&cursor));) {
    if (!opt_cg_completion_value(value)) continue;
    int index = sv_func_finally_index(func, (int)((value->slot - OPT_COMPLETION_SLOT) >> 1));
    if (index < 0 || (uint32_t)index >= count) continue;
    sv_jit_finally_t *entry = &out[index];
    ant_value_t boxed = ir_value_boxed_constant(opt_cg_site_bits(value, dump, spills), (opt_repr_t)value->repr);
    if (value->slot & 1) {
      if (entry->kind != SV_COMPLETION_JUMP) entry->value = boxed;
      continue;
    }
    uint32_t kind = vtype(boxed) == kTypeNumber ? (uint32_t)js_getnum(boxed) : 0;
    if (kind == OPT_PENDING_RETURN) entry->kind = SV_COMPLETION_RETURN;
    else if (kind >= OPT_PENDING_JUMP && kind - OPT_PENDING_JUMP < (uint32_t)func->code_len) {
      entry->kind = SV_COMPLETION_JUMP;
      entry->target = kind - OPT_PENDING_JUMP;
      entry->value = js_mkundef();
    } else continue;
    pending = true;
  }
  return pending ? out : NULL;
}

static ant_value_t opt_cg_exit_frames(sv_vm_t *vm, const opt_site_t *site, opt_code_t *code, opt_entry_t *entry, const uint64_t *dump, const uint64_t *spills) {
  uint32_t count = site->frame_count;
  size_t total = 1, value_total = 1 + site->values.count;
  for (uint32_t k = 0; k < count; k++) {
    const opt_site_frame_t *at = &code->frames[site->frame_start + k];
    uint32_t params = at->func->param_count > at->argc ? at->func->param_count : at->argc;
    total += (size_t)params + (size_t)at->func->max_locals + at->depth;
  }
  ant_value_t storage[total];
  opt_exit_frame_t frames[count];
  ant_value_t *cursor = storage;
  const opt_site_value_t *boxes[value_total];
  ant_value_t *box_slots[value_total];
  uint32_t box_count = 0;
  opt_exit_cursor_t values = opt_exit_cursor((opt_exit_view_t){code->values, code->chunks, code->refs}, site->values);
  for (uint32_t k = 0; k < count; k++) {
    const opt_site_frame_t *at = &code->frames[site->frame_start + k];
    sv_func_t *func = at->func;
    bool root = k == count - 1;
    uint32_t params = func->param_count, locals = (uint32_t)func->max_locals, depth = at->depth;
    uint32_t room = root || params > at->argc ? params : at->argc;
    uint32_t stack_slots = (uint32_t)(func->max_stack > 0 ? func->max_stack : 0);
    opt_exit_frame_t *frame = &frames[k];
    *frame = (opt_exit_frame_t){.func = func, .callee = js_mkundef(), .this_val = js_mkundef(), .params = cursor, .arguments = js_mkundef(), .site = at};
    frame->locals = frame->params + room;
    frame->stack = frame->locals + locals;
    cursor = frame->stack + depth;
    for (uint32_t i = 0; i < room; i++) frame->params[i] = root ? entry->params[i] : js_mkundef();
    for (uint32_t i = 0; i < locals + depth; i++) frame->locals[i] = root && entry->frame ? entry->locals[i] : js_mkundef();
    for (uint32_t i = 0; i < at->value_count; i++) {
      const opt_site_value_t *value = opt_exit_next(&values);
      uint64_t bits = opt_cg_site_bits(value, dump, spills);
      ant_value_t boxed = ir_value_boxed_constant(bits, (opt_repr_t)value->repr);
      uint32_t slot = value->slot;
      ant_value_t *to = NULL;
      if (slot == OPT_THIS_SLOT) frame->this_val = boxed;
      else if (slot == OPT_CALLEE_SLOT) frame->callee = boxed;
      else if (slot <= stack_slots) {
        if (slot - 1 < depth) to = &frame->stack[slot - 1];
      } else if (slot <= stack_slots + locals) to = &frame->locals[slot - stack_slots - 1];
      else if (slot - stack_slots - locals - 1 < room) to = &frame->params[slot - stack_slots - locals - 1];
      if (to && value->repr == OPT_REPR_POINTER) {
        boxes[box_count] = value;
        box_slots[box_count++] = to;
      } else if (to) *to = boxed;
    }
    if (!root) frame->closure = js_func_closure(frame->callee);
  }
  for (uint32_t k = 0; k < count; k++) {
    opt_exit_frame_t *frame = &frames[k];
    bool root = k == count - 1;
    uint32_t slots = (uint32_t)(frame->stack + frame->site->depth - frame->locals);
    if (root) {
      ant_value_t callee = sv_closure_heap_value(vm->js, entry->closure);
      frame->arguments = opt_cg_materialize_arguments(vm, frame->locals, slots, callee, entry->args, (uint32_t)entry->argc);
    } else frame->arguments = opt_cg_materialize_arguments(vm, frame->locals, slots, frame->callee, frame->params, frame->site->argc);
  }
  for (uint32_t i = 0; i < box_count; i++)
    opt_rt_reopen_box(vm, (sv_upvalue_t *)(uintptr_t)opt_cg_site_bits(boxes[i], dump, spills), box_slots[i]);
  uint32_t finally_count = opt_cg_site_finally_count(code, site);
  sv_jit_finally_t finally[finally_count ? finally_count : 1];
  frames[count - 1].finally = opt_cg_site_finally(code, site, dump, spills, finally, finally_count);
  ant_value_t result = opt_rt_exit_frames(vm, code, site, entry, frames, count);
  opt_rt_close_storage(vm, storage, storage + total);
  return result;
}

static ant_value_t *opt_cg_site_slot(const opt_code_t *code, uint32_t slot, uint32_t depth, ant_value_t *params, ant_value_t *locals, ant_value_t *stack) {
  uint32_t stack_slots = code->stack_count, local_count = code->local_count;
  if (slot >= 1 && slot <= stack_slots) return slot - 1 < depth ? &stack[slot - 1] : NULL;
  if (slot > stack_slots && slot <= stack_slots + local_count) return &locals[slot - stack_slots - 1];
  if (slot > stack_slots + local_count && slot - stack_slots - local_count - 1 < code->param_count) return &params[slot - stack_slots - local_count - 1];
  return NULL;
}

static bool opt_cg_site_values_plain(const opt_code_t *code, const opt_site_t *site, const uint64_t *dump, const uint64_t *spills) {
  opt_exit_cursor_t cursor = opt_exit_cursor((opt_exit_view_t){code->values, code->chunks, code->refs}, site->values);
  for (const opt_site_value_t *value; (value = opt_exit_next(&cursor));)
    if (value->repr != OPT_REPR_POINTER && ir_value_virtual_arguments_kind(ir_value_boxed_constant(opt_cg_site_bits(value, dump, spills), (opt_repr_t)value->repr)) >= 0)
      return false;
  return true;
}

static void opt_cg_site_write(sv_vm_t *vm, const opt_code_t *code, const opt_site_t *site, const uint64_t *dump, const uint64_t *spills,
  uint32_t depth, ant_value_t *params, ant_value_t *locals, ant_value_t *stack) {
  for (uint32_t pass = 0; pass < 2; pass++) {
    opt_exit_cursor_t cursor = opt_exit_cursor((opt_exit_view_t){code->values, code->chunks, code->refs}, site->values);
    for (const opt_site_value_t *value; (value = opt_exit_next(&cursor));) {
      if ((value->repr == OPT_REPR_POINTER) != (pass == 1)) continue;
      ant_value_t *to = opt_cg_site_slot(code, value->slot, depth, params, locals, stack);
      if (!to) continue;
      uint64_t bits = opt_cg_site_bits(value, dump, spills);
      if (pass == 1) opt_rt_reopen_box(vm, (sv_upvalue_t *)(uintptr_t)bits, to);
      else *to = ir_value_boxed_constant(bits, (opt_repr_t)value->repr);
    }
  }
}

static ant_value_t opt_cg_catch_in_place(sv_vm_t *vm, uint8_t *locals, const opt_site_t *site, opt_code_t *code, const uint64_t *dump, ant_value_t caught) {
  opt_entry_t *entry = (opt_entry_t *)(void *)(locals + code->entry_offset);
  const uint64_t *spills = (const uint64_t *)(void *)(locals + code->spill_offset);
  const opt_handler_t *handler = &code->handlers[site->handler];
  uint32_t params = code->param_count, depth = handler->depth;
  if (!entry->frame) {
    ant_value_t current[params + 1];
    for (uint32_t i = 0; i < params; i++) current[i] = entry->params[i];
    ant_value_t failed = opt_rt_enter_frame(vm, entry, code);
    if (failed) return failed;
    for (uint32_t i = 0; i < params; i++) entry->params[i] = current[i];
  }
  ant_value_t *stack = entry->locals + code->local_count;
  opt_cg_site_write(vm, code, site, dump, spills, depth, entry->params, entry->locals, stack);
  stack[depth] = caught;
  int top = (int)(stack - vm->stack) + (int)depth + 1;
  if (vm->sp < top) vm->sp = top;
  entry->frame->completion.kind = SV_COMPLETION_NONE;
  entry->frame->completion.value = js_mkundef();
  *(uint32_t *)(void *)(locals + code->index_offset) = site->handler;
  opt_rt_caught(vm, code, handler, true);
  return OPT_CAUGHT;
}

static ant_value_t opt_cg_catch(sv_vm_t *vm, uint8_t *locals, const opt_site_t *site, opt_code_t *code, const uint64_t *dump) {
  ant_t *js = vm->js;
  opt_entry_t *entry = (opt_entry_t *)(void *)(locals + code->entry_offset);
  const uint64_t *spills = (const uint64_t *)(void *)(locals + code->spill_offset);
  ant_value_t error = *(ant_value_t *)(void *)(locals + code->error_offset);
  const opt_handler_t *handler = &code->handlers[site->handler];
  if (js->terminating) return opt_rt_leave_frame(error, vm, entry);
  ant_value_t caught = js_take_thrown(js, error);
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, caught);
  sv_tfb_record1(code->func, code->func->code + handler->catch_off, caught);
  if (handler->entry != UINT32_MAX && !code->invalidated && opt_cg_site_values_plain(code, site, dump, spills)) {
    ant_value_t result = opt_cg_catch_in_place(vm, locals, site, code, dump, caught);
    GC_ROOT_RESTORE(js, mark);
    return result == OPT_CAUGHT ? result : opt_rt_leave_frame(result, vm, entry);
  }
  uint32_t params = code->param_count, local_count = code->local_count, depth = handler->depth;
  uint32_t total = params + local_count + depth + 1;
  ant_value_t frame[total + 1];
  for (uint32_t i = 0; i < params; i++) frame[i] = entry->params[i];
  for (uint32_t i = params; i < params + local_count + depth; i++) frame[i] = entry->frame ? entry->locals[i - params] : IR_VALUE_UNDEFINED;
  frame[params + local_count + depth] = caught;
  ant_value_t *frame_locals = frame + params, *stack = frame + params + local_count;
  opt_exit_cursor_t cursor = opt_exit_cursor((opt_exit_view_t){code->values, code->chunks, code->refs}, site->values);
  for (const opt_site_value_t *value; (value = opt_exit_next(&cursor));) {
    if (value->repr == OPT_REPR_POINTER) continue;
    ant_value_t *to = opt_cg_site_slot(code, value->slot, depth, frame, frame_locals, stack);
    if (to) *to = ir_value_boxed_constant(opt_cg_site_bits(value, dump, spills), (opt_repr_t)value->repr);
  }
  ant_value_t callee = sv_closure_heap_value(js, entry->closure);
  ant_value_t arguments = opt_cg_materialize_arguments(vm, frame_locals, local_count + depth, callee, entry->args, (uint32_t)entry->argc);
  if (is_object_type(arguments)) vm->jit.resume.arguments_obj = arguments;
  cursor = opt_exit_cursor((opt_exit_view_t){code->values, code->chunks, code->refs}, site->values);
  for (const opt_site_value_t *value; (value = opt_exit_next(&cursor));) {
    if (value->repr != OPT_REPR_POINTER) continue;
    ant_value_t *to = opt_cg_site_slot(code, value->slot, depth, frame, frame_locals, stack);
    if (to) opt_rt_reopen_box(vm, (sv_upvalue_t *)(uintptr_t)opt_cg_site_bits(value, dump, spills), to);
  }
  opt_rt_caught(vm, code, handler, false);
  uint32_t finally_count = opt_cg_site_finally_count(code, site);
  sv_jit_finally_t finally[finally_count ? finally_count : 1];
  sv_jit_finally_t *pending = opt_cg_site_finally(code, site, dump, spills, finally, finally_count);
  vm->jit.resume.throwing = handler->finally;
  ant_value_t result = opt_rt_resume(vm, code, entry, site->bc_off, depth + 1, frame, frame_locals, stack, pending);
  opt_rt_close_storage(vm, frame, frame + total + 1);
  GC_ROOT_RESTORE(js, mark);
  return result;
}

static ant_value_t opt_cg_exit(sv_vm_t *vm, uint8_t *locals, uint32_t index, opt_code_t *code, const uint64_t *dump) {
  const opt_site_t *site = &code->sites[index];
  opt_entry_t *entry = (opt_entry_t *)(void *)(locals + code->entry_offset);
  const uint64_t *spills = (const uint64_t *)(void *)(locals + code->spill_offset);
  if (site->handler != UINT32_MAX) return opt_cg_catch(vm, locals, site, code, dump);
  if (site->frame_count) return opt_cg_exit_frames(vm, site, code, entry, dump, spills);
  uint32_t params = code->param_count, local_count = code->local_count, depth = site->depth;
  ant_value_t frame[params + local_count + depth + 1];
  for (uint32_t i = 0; i < params; i++) frame[i] = entry->params[i];
  for (uint32_t i = params; i < params + local_count + depth; i++)
    frame[i] = entry->frame ? entry->locals[i - params] : IR_VALUE_UNDEFINED;
  uint32_t stack_slots = code->stack_count;
  for (uint32_t pass = 0; pass < 2; pass++) {
    opt_exit_cursor_t cursor = opt_exit_cursor((opt_exit_view_t){code->values, code->chunks, code->refs}, site->values);
    for (const opt_site_value_t *value; (value = opt_exit_next(&cursor));) {
      if ((value->repr == OPT_REPR_POINTER) != (pass == 1)) continue;
      uint64_t bits = opt_cg_site_bits(value, dump, spills);
      uint32_t slot = value->slot;
      ant_value_t *to = NULL;
      if (slot >= 1 && slot <= stack_slots) {
        if (slot - 1 < depth) to = &frame[params + local_count + slot - 1];
      } else if (slot > stack_slots && slot <= stack_slots + local_count) to = &frame[params + slot - stack_slots - 1];
      else if (slot > stack_slots + local_count && slot - stack_slots - local_count - 1 < params) to = &frame[slot - stack_slots - local_count - 1];
      if (!to) continue;
      if (pass == 1) opt_rt_reopen_box(vm, (sv_upvalue_t *)(uintptr_t)bits, to);
      else *to = ir_value_boxed_constant(bits, (opt_repr_t)value->repr);
    }
    if (pass == 0) {
      ant_value_t callee = sv_closure_heap_value(vm->js, entry->closure);
      ant_value_t arguments = opt_cg_materialize_arguments(vm, frame + params, local_count + depth, callee, entry->args, (uint32_t)entry->argc);
      if (is_object_type(arguments)) vm->jit.resume.arguments_obj = arguments;
    }
  }
  uint32_t finally_count = opt_cg_site_finally_count(code, site);
  sv_jit_finally_t finally[finally_count ? finally_count : 1];
  sv_jit_finally_t *pending = opt_cg_site_finally(code, site, dump, spills, finally, finally_count);
  ant_value_t result = opt_rt_exit(vm, code, site, entry, frame, frame + params, frame + params + local_count, pending);
  opt_rt_close_storage(vm, frame, frame + params + local_count + depth + 1);
  return result;
}

static void opt_cg_pop_frame(opt_cg_t *cg) {
  masm_t *m = &cg->masm;
  asm_gpr_t frame = cg->pinned.scratch, a = MASM_ARGUMENT_GPRS[1], b = MASM_ARGUMENT_GPRS[2], vm = cg->pinned.vm;
  asm_label_t slow = masm_new_label(m), no_upvalues = masm_new_label(m), done = masm_new_label(m);
  masm_load64(m, frame, opt_cg_entry_field(cg, offsetof(opt_entry_t, frame)));
  masm_load64(m, a, asm_addr(vm, (int32_t)offsetof(sv_vm_t, open_upvalues)));
  masm_branch64_imm(m, ASM_COND_EQUAL, a, 0, no_upvalues);
  masm_load64(m, a, asm_addr(a, (int32_t)offsetof(sv_upvalue_t, location)));
  masm_load64(m, b, asm_addr(frame, (int32_t)offsetof(sv_frame_t, bp)));
  masm_branch64(m, ASM_COND_ABOVE_OR_EQUAL, a, b, slow);
  masm_bind(m, no_upvalues);
  masm_load64(m, a, asm_addr(frame, (int32_t)offsetof(sv_frame_t, arguments_obj)));
  masm_branch64_imm(m, ASM_COND_NOT_EQUAL, a, (int64_t)IR_VALUE_UNDEFINED, slow);
  masm_load8(m, a, opt_cg_entry_field(cg, offsetof(opt_entry_t, owns_frame)));
  masm_branch32_imm(m, ASM_COND_EQUAL, a, 0, done);
  masm_load32(m, a, asm_addr(frame, (int32_t)offsetof(sv_frame_t, prev_sp)));
  masm_store32(m, asm_addr(vm, (int32_t)offsetof(sv_vm_t, sp)), a);
  masm_load32(m, a, asm_addr(vm, (int32_t)offsetof(sv_vm_t, fp)));
  masm_sub32_imm(m, a, a, 1);
  masm_store32(m, asm_addr(vm, (int32_t)offsetof(sv_vm_t, fp)), a);
  masm_load64(m, a, opt_cg_entry_field(cg, offsetof(opt_entry_t, seg) + offsetof(gc_vm_seg_t, prev)));
  masm_load64(m, b, asm_addr(vm, (int32_t)offsetof(sv_vm_t, js)));
  masm_store64(m, asm_addr(b, (int32_t)offsetof(ant_t, vm_segs)), a);
  masm_bind(m, done);
  opt_cg_return(cg);
  masm_bind(m, slow);
}

static ir_block_id_t opt_cg_entry_at(const opt_cg_t *cg, uint32_t bc_off) {
  for (uint32_t i = 0; i < cg->graph->osr_entries.len; i++)
    if (cg->graph->osr_entries.items[i].bc_off == bc_off) return cg->graph->osr_entries.items[i].block;
  return IR_NONE;
}

static void opt_cg_catch_dispatch(opt_cg_t *cg) {
  masm_t *m = &cg->masm;
  const ir_graph_t *graph = cg->graph;
  bool entries = false;
  for (uint32_t h = 0; h < graph->handlers.len; h++) entries |= graph->handlers.items[h].has_entry;
  if (!entries) return;
  asm_gpr_t scratch = cg->pinned.scratch;
  asm_label_t returned = masm_new_label(m);
  masm_move_imm64(m, scratch, OPT_CAUGHT);
  masm_branch64(m, ASM_COND_NOT_EQUAL, MASM_RETURN_GPRS[0], scratch, returned);
  masm_load64(m, cg->pinned.frame, opt_cg_entry_field(cg, offsetof(opt_entry_t, params)));
  masm_load32(m, scratch, opt_cg_local(cg, cg->index_off));
  for (uint32_t h = 0; h < graph->handlers.len; h++) {
    const ir_handler_t *handler = &graph->handlers.items[h];
    ir_block_id_t block = handler->has_entry ? opt_cg_entry_at(cg, handler->catch_off + sv_op_size[OP_CATCH]) : IR_NONE;
    if (block != IR_NONE) masm_branch32_imm(m, ASM_COND_EQUAL, scratch, (int32_t)h, opt_cg_label(cg, block));
  }
  masm_unreachable(m);
  masm_bind(m, returned);
}

static void opt_cg_tails(opt_cg_t *cg, asm_label_t retry) {
  masm_t *m = &cg->masm;
  opt_cg_entry_tails(cg, retry);
  opt_cg_emit_call_slows(cg);
  masm_bind(m, retry);
  masm_move_imm64(m, MASM_RETURN_GPRS[0], SV_JIT_RETRY_INTERP);
  opt_cg_return(cg);

  for (uint32_t i = 0; i < cg->generics.len; i++) {
    masm_bind(m, cg->generics.items[i].label);
    masm_branch64_imm(m, ASM_COND_EQUAL, MASM_RETURN_GPRS[0], (int64_t)SV_JIT_RETRY_INTERP, cg->generics.items[i].exit);
    masm_jump(m, cg->generics.items[i].thrown);
  }
  masm_bind(m, cg->throw_label);
  opt_cg_return_from_frame(cg);
  masm_bind(m, cg->overflow);
  masm_move64(m, MASM_ARGUMENT_GPRS[0], cg->pinned.vm);
  masm_call_absolute(m, (uint64_t)(uintptr_t)opt_rt_stack_overflow);
  opt_cg_return(cg);

  if (cg->has_frame) {
    masm_bind(m, cg->leave);
    opt_cg_pop_frame(cg);
    masm_move64(m, MASM_ARGUMENT_GPRS[0], MASM_RETURN_GPRS[0]);
    masm_move64(m, MASM_ARGUMENT_GPRS[1], cg->pinned.vm);
    masm_load_effective_address(m, MASM_ARGUMENT_GPRS[2], opt_cg_entry_field(cg, 0));
    masm_call_absolute(m, (uint64_t)(uintptr_t)opt_rt_leave_frame);
    opt_cg_return(cg);
  }

  for (uint32_t i = 0; i < cg->exit_stubs.len; i++) {
    masm_bind(m, cg->exit_stubs.items[i].label);
    if (cg->exit_stubs.items[i].catches) masm_store64(m, opt_cg_local(cg, cg->error_off), MASM_RETURN_GPRS[0]);
    masm_store_imm32(m, opt_cg_local(cg, cg->index_off), cg->exit_stubs.items[i].index);
    masm_jump(m, cg->exit_stub);
  }
  if (cg->exit_stubs.len) {
    masm_bind(m, cg->exit_stub);
    opt_cg_dump_registers(cg);
    masm_move64(m, MASM_ARGUMENT_GPRS[0], cg->pinned.vm);
    masm_load_effective_address(m, MASM_ARGUMENT_GPRS[1], asm_addr(MASM_STACK_POINTER, (int32_t)OPT_DUMP_BYTES));
    masm_load32(m, MASM_ARGUMENT_GPRS[2], asm_addr(MASM_STACK_POINTER, (int32_t)(OPT_DUMP_BYTES + cg->index_off)));
    masm_move_imm64(m, MASM_ARGUMENT_GPRS[3], (uint64_t)(uintptr_t)cg->code);
    masm_move64(m, MASM_ARGUMENT_GPRS[4], MASM_STACK_POINTER);
    masm_call_absolute(m, (uint64_t)(uintptr_t)opt_cg_exit);
    cg->exit_ret_off = (uint32_t)masm_offset(m);
    masm_add64_imm(m, MASM_STACK_POINTER, MASM_STACK_POINTER, OPT_DUMP_BYTES);
    opt_cg_catch_dispatch(cg);
    opt_cg_return(cg);
  }
  opt_cg_emit_barriers(cg);
  opt_cg_emit_allocations(cg);
  for (uint32_t i = 0; i < cg->object_kinds.len; i++) {
    const opt_cg_object_kinds_t *kinds = &cg->object_kinds.items[i];
    asm_gpr_t scratch = cg->pinned.scratch;
    masm_bind(m, kinds->label);
    masm_branch64_imm(m, ASM_COND_EQUAL, scratch, (int64_t)(OPT_ARRAY_TAG ^ OPT_OBJECT_TAG), kinds->done);
    masm_branch64_imm(m, ASM_COND_EQUAL, scratch, (int64_t)(((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | kTypePromise) ^ OPT_OBJECT_TAG), kinds->done);
    masm_branch64_imm(m, ASM_COND_NOT_EQUAL, scratch, (int64_t)(((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | kTypeFunction) ^ OPT_OBJECT_TAG), kinds->exit);
    masm_load64(m, kinds->output, asm_addr(kinds->output, (int32_t)offsetof(sv_closure_t, func_obj)));
    masm_branch64_imm(m, ASM_COND_EQUAL, kinds->output, 0, kinds->exit);
    masm_and64_imm(m, kinds->output, kinds->output, NANBOX_DATA_MASK);
    masm_add64_imm(m, kinds->output, kinds->output, (int64_t)ant_cage_base());
    masm_jump(m, kinds->done);
  }
  for (uint32_t i = 0; i < cg->element_int32s.len; i++) {
    const opt_cg_element_int32_t *fused = &cg->element_int32s.items[i];
    masm_bind(m, fused->label);
    masm_branch64_imm(m, ASM_COND_ABOVE_OR_EQUAL, fused->value, (int64_t)ANT_SENTINEL_TAG, fused->hole_exit);
    masm_jump(m, fused->int32_exit);
  }
  for (uint32_t i = 0; i < cg->zero_products.len; i++) {
    const opt_cg_zero_product_t *zero = &cg->zero_products.items[i];
    masm_bind(m, zero->label);
    masm_or32(m, cg->pinned.scratch, zero->lhs, zero->rhs);
    masm_branch32_imm(m, ASM_COND_LESS_THAN, cg->pinned.scratch, 0, zero->exit);
    masm_jump(m, zero->resume);
  }
  opt_cg_emit_save_routines(cg);
}

static void *opt_cg_dup_array(const void *items, uint32_t len, size_t elem) {
  void *copy = malloc((len + 1) * elem);
  if (!copy) opt_out_of_memory();
  if (len) memcpy(copy, items, len * elem);
  return copy;
}

opt_code_t *opt_cg_generate(ant_t *js, const ir_graph_t *graph, const ir_frame_layout_t *layout, asm_code_heap_t *heap, bool verify, opt_text_t *dump, const char **failure) {
  opt_cg_t cg = {.graph = graph, .layout = layout, .js = js, .marks_wanted = dump != NULL};
  opt_arena_init(&cg.arena);
  opt_map_init(&cg.site_index, &cg.arena, 4);
  opt_map_init(&cg.frames_index, &cg.arena, 1);
  opt_target_registers(OPT_TARGET_HOST, &cg.registers, &cg.pinned);
  ra_allocation_t allocation;
  ra_allocate(graph, &cg.registers, &allocation);
  cg.alloc = &allocation;
  if (dump) ra_dump_allocation(graph, &allocation, dump);
  if (verify) {
    opt_text_t errors = {0};
    if (!ra_verify(graph, &cg.registers, &allocation, &errors)) {
      fprintf(stderr, "jit: opt register allocation invalid:\n%s\n", errors.data ? errors.data : "");
      opt_text_free(&errors);
      abort();
    }
    opt_text_free(&errors);
  }

  cg.code = opt_code_new(graph->func);
  cg.code->param_count = layout->number_of_arguments;
  cg.code->local_count = layout->registers_and_locals_count - layout->number_of_registers;
  cg.code->stack_count = layout->number_of_registers - 1;
  masm_init(&cg.masm);
  opt_cg_lay_out_frame(&cg);
  cg.labels = opt_arena_new(&cg.arena, asm_label_t, ir_block_count(graph) + 1);
  for (uint32_t i = 0; i < ir_block_count(graph); i++) cg.labels[i] = masm_new_label(&cg.masm);
  cg.exit_stub = masm_new_label(&cg.masm);
  cg.leave = masm_new_label(&cg.masm);
  cg.throw_label = masm_new_label(&cg.masm);
  cg.overflow = masm_new_label(&cg.masm);
  cg.exit_ret_off = UINT32_MAX;
  cg.has_frame = graph->uses_frame;
  asm_label_t retry = masm_new_label(&cg.masm);
  cg.osr_entry = masm_new_label(&cg.masm);
  cg.copy_params = masm_new_label(&cg.masm);
  cg.have_params = masm_new_label(&cg.masm);
  opt_cg_landing_blocks(&cg);

  opt_cg_prologue(&cg);
  for (ir_block_id_t block = 0; block < ir_block_count(graph) && !cg.failed; block++)
    if (cg.landing[block] == block) opt_cg_block(&cg, block);
  if (!cg.failed) opt_cg_tails(&cg, retry);

  for (uint32_t i = 0; i < cg.invalidations.len && !cg.failed; i++) {
    const opt_cg_invalidation_t *point = &cg.invalidations.items[i];
    opt_patch_t patch = {.offset = (uint32_t)point->offset};
    patch.len = (uint8_t)masm_jump_patch(point->offset, masm_label_offset(&cg.masm, point->exit), patch.bytes);
    opt_vec_push(&cg.arena, &cg.patches, patch);
  }

  asm_code_t machine = {0};
  size_t code_len = 0;
  if (!cg.failed && masm_finish(&cg.masm, &machine) != ASM_OK) opt_cg_fail(&cg, "assemble");
  else if (cg.failed) masm_free(&cg.masm);
  code_len = machine.len;
  if (!cg.failed && dump) {
    asm_listing_t listing = {0};
    asm_listing_with_data(&listing, MASM_ARCH, machine.bytes, machine.len, machine.data_offset);
    uint32_t mark = 0;
    for (size_t i = 0; i < listing.count; i++) {
      const asm_insn_t *insn = &listing.items[i];
      for (; mark < cg.marks.len && cg.marks.items[mark].offset <= insn->offset; mark++)
        fprintf(stderr, "        ; v%u\n", cg.marks.items[mark].node);
      fprintf(stderr, "%6zx  %s", insn->offset, insn->text);
      if (insn->has_constant && insn->text[0] != '.') fprintf(stderr, "  ; 0x%" PRIx64, insn->constant);
      fputc('\n', stderr);
    }
    asm_listing_free(&listing);
  }
  if (!cg.failed && !asm_code_heap_place(heap, &cg.code->executable, machine.bytes, machine.len)) opt_cg_fail(&cg, "executable");
  asm_code_free(&machine);
  if (!cg.failed) {
    const opt_exit_table_t *exits = &allocation.exit_table;
    cg.code->values = opt_cg_dup_array(exits->values.items, exits->values.len, sizeof *exits->values.items);
    cg.code->chunks = opt_cg_dup_array(exits->chunks.items, exits->chunks.len, sizeof *exits->chunks.items);
    cg.code->refs = opt_cg_dup_array(exits->refs.items, exits->refs.len, sizeof *exits->refs.items);
  }
  ra_allocation_free(&allocation);

  if (cg.failed) {
    if (dump) opt_text_appendf(dump, "codegen failed: %s\n", cg.failure);
    if (failure) *failure = cg.failure;
    opt_arena_free(&cg.arena);
    opt_code_free(cg.code);
    return NULL;
  }
  opt_code_t *code = cg.code;
  code->entry = (sv_jit_func_t)cg.code->executable.address;
  code->code_bytes = code_len;
  code->entry_offset = cg.entry_off;
  code->spill_offset = cg.spill_off;
  code->index_offset = cg.index_off;
  code->error_offset = cg.error_off;
  code->handler_count = graph->handlers.len;
  code->handlers = malloc((graph->handlers.len + 1) * sizeof *code->handlers);
  if (!code->handlers) opt_out_of_memory();
  for (uint32_t h = 0; h < graph->handlers.len; h++) {
    const ir_handler_t *handler = &graph->handlers.items[h];
    code->handlers[h] = (opt_handler_t){.catch_off = handler->catch_off, .depth = handler->depth, .entry = handler->has_entry ? h : UINT32_MAX, .learnable = handler->learnable, .finally = handler->finally};
  }
  code->sites = opt_cg_dup_array(cg.sites.items, cg.sites.len, sizeof *cg.sites.items);
  code->frames = malloc((cg.site_frames.len + 1) * sizeof *code->frames);
  code->call_site_count = cg.call_sites.len;
  code->call_sites = malloc((cg.call_sites.len + 1) * sizeof *code->call_sites);
  if (!code->frames || !code->call_sites) opt_out_of_memory();
  if (cg.site_frames.len) memcpy(code->frames, cg.site_frames.items, cg.site_frames.len * sizeof *code->frames);
  if (cg.call_sites.len) memcpy(code->call_sites, cg.call_sites.items, cg.call_sites.len * sizeof *code->call_sites);
  code->exit_ret_off = cg.exit_ret_off;
  code->patch_count = cg.patches.len;
  code->patches = opt_cg_dup_array(cg.patches.items, cg.patches.len, sizeof *cg.patches.items);
  opt_arena_free(&cg.arena);
  return code;
}
