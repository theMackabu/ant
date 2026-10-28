#ifndef ANT_JIT_OPT_TIER_H
#define ANT_JIT_OPT_TIER_H

#include "asm/executable.h"
#include "exits.h"
#include "ir/ir.h"

#include "gc/objects.h"
#include "silver/engine.h"
#include "silver/glue.h"
#include "silver/stack_trace.h"

#if (defined(__aarch64__) || defined(__x86_64__)) && (defined(__APPLE__) || defined(__linux__))
static constexpr bool OPT_TIER_SUPPORTED = true;
#else
static constexpr bool OPT_TIER_SUPPORTED = false;
#endif

typedef struct {
  ant_value_t this_val;
  ant_value_t new_target;
  ant_value_t super_val;
  ant_value_t *args;
  int64_t argc;
  sv_closure_t *closure;
  ant_value_t *params;
  sv_frame_t *frame;
  ant_value_t *locals;
  bool owns_frame;
  gc_vm_seg_t seg;
} opt_entry_t;

typedef opt_exit_value_t opt_site_value_t;

typedef enum : uint8_t {
  OPT_SITE_REGISTER,
  OPT_SITE_STACK,
  OPT_SITE_CONSTANT,
} opt_site_value_kind_t;

typedef struct {
  uint32_t bc_off;
  uint32_t depth;
  opt_exit_kind_t kind;
  uint32_t slot;
  opt_exit_list_t values;
  uint32_t frame_start;
  uint32_t frame_count;
  uint32_t handler;
} opt_site_t;

typedef struct {
  uint32_t catch_off;
  uint32_t depth;
  uint32_t entry;
  bool learnable;
  bool finally;
} opt_handler_t;

static constexpr uint32_t OPT_CATCH_MISS_LIMIT = 10;
static constexpr ant_value_t OPT_CAUGHT = ANT_SENTINEL_TAG | 0xCA7C4ULL;

typedef struct {
  sv_func_t *func;
  uint32_t bc_off;
  uint32_t call_off;
  uint32_t depth;
  uint32_t value_start;
  uint32_t value_count;
  uint32_t argc;
  opt_resume_mode_t mode;
  bool construct;
} opt_site_frame_t;

typedef struct {
  sv_func_t *func;
  sv_closure_t *closure;
  ant_value_t callee;
  ant_value_t this_val;
  ant_value_t *params;
  ant_value_t *locals;
  ant_value_t *stack;
  ant_value_t arguments;
  const opt_site_frame_t *site;
  sv_jit_finally_t *finally;
} opt_exit_frame_t;

typedef struct {
  uint32_t ret_off;
  int32_t bc_off;
  uint32_t depth;
  sv_jit_frame_t frames[SV_JIT_INLINE_FRAMES_MAX];
} opt_call_site_t;

typedef struct {
  uint32_t offset;
  uint8_t len;
  uint8_t bytes[11];
} opt_patch_t;

typedef struct opt_code {
  sv_func_t *func;
  sv_jit_func_t entry;
  asm_executable_t executable;
  opt_site_t *sites;
  opt_site_value_t *values;
  opt_exit_chunk_t *chunks;
  uint32_t *refs;
  opt_site_frame_t *frames;
  opt_call_site_t *call_sites;
  uint32_t call_site_count;
  uint32_t exit_ret_off;
  uint32_t param_count, local_count, stack_count;
  uint32_t entry_offset, spill_offset, index_offset, error_offset;
  opt_handler_t *handlers;
  uint32_t handler_count;
  uint32_t catch_misses;
  size_t code_bytes;
  uint64_t slow_path_runs;
  ant_value_t *roots;
  uint32_t root_count;
  struct opt_code *func_next;
  sv_func_t **funcs;
  uint32_t func_count;
  uint64_t active_epoch;
  opt_patch_t *patches;
  uint32_t patch_count;
  ant_proto_watch_t **watches;
  uint32_t watch_count;
  bool dropped;
  bool invalidated;
} opt_code_t;

static constexpr uint64_t OPT_SLOW_PATH_LIMIT = 1000;

static constexpr uint32_t OPT_DUMP_GPRS = 32;
static constexpr uint32_t OPT_DUMP_FPRS = 32;

static inline bool opt_float_may_be_negative_nan_in(const ir_graph_t *graph, ir_node_id_t value, ir_node_id_t *seen, uint32_t *count) {
  for (uint32_t i = 0; i < *count; i++)
    if (seen[i] == value) return false;
  if (*count == 32) return true;
  seen[(*count)++] = value;
  const ir_node_t *node = ir_node(graph, value);
  switch (node->op.code) {
    case IR_OP_INT32_TO_FLOAT64:
    case IR_OP_CHECK_NUMBER:
    case IR_OP_UNBOX_DOUBLE: return false;
    case IR_OP_CONSTANT:
      return node->repr != OPT_REPR_FLOAT64 || (node->op.bits >> 63 && (node->op.bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull &&
        (node->op.bits & 0x000fffffffffffffull));
    case IR_OP_FLOAT64_UNARY:
      if (node->op.unary == IR_UNARY_ABS) return false;
      if (node->op.unary == IR_UNARY_NEGATE) return true;
      break;
    case IR_OP_FLOAT64_BINARY:
    case IR_OP_FLOAT64_MIN:
    case IR_OP_FLOAT64_MAX:
    case IR_OP_PHI: break;
    default: return true;
  }
  for (uint32_t i = 0; i < node->inputs.len; i++)
    if (opt_float_may_be_negative_nan_in(graph, node->inputs.items[i], seen, count)) return true;
  return false;
}

static inline bool opt_float_may_be_negative_nan(const ir_graph_t *graph, ir_node_id_t value) {
#if defined(__aarch64__)
  ir_node_id_t seen[32];
  uint32_t count = 0;
  return opt_float_may_be_negative_nan_in(graph, value, seen, &count);
#else
  (void)graph;
  (void)value;
  return true;
#endif
}

typedef struct {
  ant_shape_t *shape;
  uint32_t index;
  ant_object_t *holder;
  ant_value_t proto;
  uint32_t age;
} opt_field_memo_case_t;

static constexpr uint32_t OPT_FIELD_MEMO_CASES = 9;
static constexpr uint32_t OPT_FIELD_MEMO_MAX_AGE = 2;

uint32_t opt_tier_field_memo(ant_t *js, sv_func_t *func, uint32_t bc_off, opt_field_memo_case_t *out);
void opt_tier_field_remember(ant_t *js, sv_func_t *func, uint32_t bc_off, const opt_field_memo_case_t *cases, uint32_t count);

bool opt_tier_enabled(void);
uintptr_t opt_tier_generic_step(void);
uintptr_t opt_tier_access_helper(sv_op_t op, bool adding);
sv_jit_func_t opt_tier_compile(ant_t *js, sv_func_t *func, sv_closure_t *closure, bool sole_tier);

opt_code_t *opt_code_new(sv_func_t *func);
void opt_code_invalidate(opt_code_t *code);
void opt_code_free(opt_code_t *code);

ant_value_t opt_rt_exit(
  sv_vm_t *vm, opt_code_t *code, const opt_site_t *site, opt_entry_t *entry,
  ant_value_t *params, ant_value_t *locals, ant_value_t *stack, sv_jit_finally_t *finally
);

ant_value_t opt_rt_resume(
  sv_vm_t *vm, opt_code_t *code, opt_entry_t *entry, uint32_t bc_off, uint32_t depth,
  ant_value_t *params, ant_value_t *locals, ant_value_t *stack, sv_jit_finally_t *finally
);

void opt_rt_caught(sv_vm_t *vm, opt_code_t *code, const opt_handler_t *handler, bool in_place);
ant_value_t opt_rt_for_of(sv_vm_t *vm, ant_value_t iterable, ant_value_t unused, ant_value_t *args, int64_t argc, int64_t kind);
ant_value_t opt_rt_iter_next(sv_vm_t *vm, ant_value_t iter, ant_value_t next, ant_value_t *args, int64_t argc, int64_t hint);
ant_value_t opt_rt_destructure_next(sv_vm_t *vm, ant_value_t iter, ant_value_t next, ant_value_t *args, int64_t argc, int64_t kind);
ant_value_t opt_rt_destructure_rest(sv_vm_t *vm, ant_value_t iter, ant_value_t next, ant_value_t *args, int64_t argc, int64_t kind);
ant_value_t opt_rt_iter_close(sv_vm_t *vm, ant_value_t iter, ant_value_t next, ant_value_t *args, int64_t argc, int64_t suppress);
ant_value_t opt_rt_exit_frames(sv_vm_t *vm, opt_code_t *code, const opt_site_t *site, opt_entry_t *entry, opt_exit_frame_t *frames, uint32_t count);

ant_value_t opt_rt_call(sv_vm_t *vm, ant_value_t callee, ant_value_t this_val, ant_value_t *args, int64_t argc);
ant_value_t opt_rt_construct(sv_vm_t *vm, ant_value_t callee, ant_value_t new_target, ant_value_t *args, int64_t argc);
ant_value_t opt_rt_new_this(sv_vm_t *vm, ant_value_t callee);
ant_value_t opt_rt_stack_overflow(sv_vm_t *vm);
ant_value_t opt_rt_char_code_at(sv_vm_t *vm, ant_value_t callee, ant_value_t this_val, ant_value_t *args, int64_t argc, int64_t kind);
ant_value_t opt_rt_array_includes(sv_vm_t *vm, ant_value_t callee, ant_value_t this_val, ant_value_t *args, int64_t argc, int64_t kind);
ant_value_t opt_rt_string_intrinsic(sv_vm_t *vm, ant_value_t callee, ant_value_t this_val, ant_value_t *args, int64_t argc, int64_t kind);
ant_value_t opt_rt_stable_builtin(sv_vm_t *vm, ant_value_t callee, ant_value_t this_val, ant_value_t *args, int64_t argc, int64_t kind);
ant_value_t opt_rt_regexp_exec_truthy(sv_vm_t *vm, ant_value_t callee, ant_value_t this_val, ant_value_t *args, int64_t argc, int64_t kind);
ant_value_t opt_rt_call_call(sv_vm_t *vm, ant_value_t callee, ant_value_t arg, ant_value_t *args, int64_t argc, int64_t kind);
ant_value_t opt_rt_to_string(sv_vm_t *vm, ant_value_t value, ant_value_t unused, ant_value_t *args, int64_t argc, int64_t defer_number);
ant_value_t opt_rt_regexp(sv_vm_t *vm, ant_value_t pattern, ant_value_t flags, ant_value_t *args, int64_t argc, int64_t kind);

ant_value_t opt_rt_concat(sv_vm_t *vm, ant_value_t lhs, ant_value_t rhs, ant_value_t *args, int64_t argc, int64_t kind);
uint64_t opt_rt_string_length(sv_vm_t *vm, ant_value_t string, ant_value_t unused, ant_value_t *args, int64_t argc, int64_t kind);

typedef struct {
  sv_func_t *func;
  uint32_t bc_off;
} opt_rt_site_t;

ant_value_t opt_rt_typeof(sv_vm_t *vm, ant_value_t value, ant_value_t unused, ant_value_t *args, int64_t argc, int64_t kind);
ant_value_t opt_rt_in(sv_vm_t *vm, ant_value_t key, ant_value_t object, ant_value_t *args, int64_t argc, int64_t kind);
ant_value_t opt_rt_for_in(sv_vm_t *vm, ant_value_t object, ant_value_t unused, ant_value_t *args, int64_t argc, int64_t kind);
ant_value_t opt_rt_set_proto(sv_vm_t *vm, ant_value_t object, ant_value_t proto, ant_value_t *args, int64_t argc, int64_t kind);
ant_value_t opt_rt_builder_read(sv_vm_t *vm, ant_value_t value, ant_value_t unused, ant_value_t *args, int64_t argc, int64_t kind);

ant_value_t opt_rt_allocate_object(sv_vm_t *vm, ant_shape_t *shape, ant_value_t proto, uint64_t property_count);
ant_value_t opt_rt_allocate_array(sv_vm_t *vm, uint64_t count);
ant_value_t opt_rt_object(sv_vm_t *vm, ant_value_t unused, ant_value_t unused2, ant_value_t *args, int64_t argc, const opt_rt_site_t *site);
ant_value_t opt_rt_array(sv_vm_t *vm, ant_value_t unused, ant_value_t unused2, ant_value_t *args, int64_t argc, int64_t kind);

typedef struct {
  sv_func_t *func;
  uint32_t index;
  const char *name;
  uint32_t name_len;
  uint32_t fresh;
} opt_rt_closure_site_t;

ant_value_t opt_rt_closure(sv_vm_t *vm, sv_closure_t *parent, ant_value_t this_val, ant_value_t *args, int64_t argc, const opt_rt_closure_site_t *site);
ant_value_t opt_rt_close_upvalues(sv_vm_t *vm, ant_value_t unused, ant_value_t unused2, ant_value_t *args, int64_t argc, int64_t slot);
ant_value_t opt_rt_closure_boxes(sv_vm_t *vm, sv_closure_t *parent, ant_value_t this_val, ant_value_t *args, int64_t argc, const opt_rt_closure_site_t *site);
ant_value_t opt_rt_new_box(sv_vm_t *vm, ant_value_t value, ant_value_t unused, ant_value_t *args, int64_t argc, int64_t kind);
ant_value_t opt_rt_osr_box(sv_vm_t *vm, ant_value_t unused, ant_value_t unused2, ant_value_t *args, int64_t argc, int64_t slot);
void opt_rt_reopen_box(sv_vm_t *vm, sv_upvalue_t *box, ant_value_t *slot);
void opt_rt_close_storage(sv_vm_t *vm, const ant_value_t *from, const ant_value_t *to);
ant_value_t opt_rt_arguments_object(sv_vm_t *vm, ant_value_t callee, ant_value_t *args, int argc, bool is_strict);

ant_value_t opt_rt_str_append(sv_vm_t *vm, ant_value_t current, ant_value_t rhs, ant_value_t *args, int64_t argc, int64_t boxed);
ant_value_t opt_rt_str_append_snapshot(sv_vm_t *vm, ant_value_t current, ant_value_t rhs, ant_value_t *args, int64_t argc, int64_t boxed);

ant_value_t opt_rt_get_global(sv_vm_t *vm, ant_value_t unused, ant_value_t unused2, ant_value_t *args, int64_t argc, const opt_rt_site_t *site);
ant_value_t opt_rt_get_field(sv_vm_t *vm, ant_value_t object, ant_value_t unused, ant_value_t *args, int64_t argc, const opt_rt_site_t *site);
ant_value_t opt_rt_put_field(sv_vm_t *vm, ant_value_t object, ant_value_t value, ant_value_t *args, int64_t argc, const opt_rt_site_t *site);
ant_value_t opt_rt_get_elem(sv_vm_t *vm, ant_value_t object, ant_value_t key, ant_value_t *args, int64_t argc, const opt_rt_site_t *site);
ant_value_t opt_rt_put_elem(sv_vm_t *vm, ant_value_t object, ant_value_t key, ant_value_t *args, int64_t argc, const opt_rt_site_t *site);
ant_value_t opt_rt_put_elem_plain(sv_vm_t *vm, ant_value_t object, ant_value_t key, ant_value_t *args, int64_t argc, const opt_rt_site_t *site);
ant_value_t opt_rt_get_length(sv_vm_t *vm, ant_value_t object, ant_value_t unused, ant_value_t *args, int64_t argc, const opt_rt_site_t *site);

typedef struct {
  opt_rt_site_t site;
  uint32_t count;
  jit_add_cell_t adds[];
} opt_rt_add_site_t;

ant_value_t opt_rt_put_field_adding(sv_vm_t *vm, ant_value_t object, ant_value_t value, ant_value_t *args, int64_t argc, const opt_rt_add_site_t *site);
ant_value_t opt_rt_instanceof(sv_vm_t *vm, ant_value_t lhs, ant_value_t rhs, ant_value_t *args, int64_t argc, const opt_rt_site_t *site);
ant_value_t opt_rt_in_field(sv_vm_t *vm, ant_value_t object, ant_value_t unused, ant_value_t *args, int64_t argc, const opt_rt_site_t *site);
ant_value_t opt_rt_throw(ant_value_t value, sv_vm_t *vm);

ant_value_t opt_rt_enter_frame(sv_vm_t *vm, opt_entry_t *entry, const opt_code_t *code);
void opt_rt_borrow_frame(sv_vm_t *vm, opt_entry_t *entry);
ant_value_t opt_rt_leave_frame(ant_value_t result, sv_vm_t *vm, opt_entry_t *entry);

ant_value_t opt_rt_normalize_this(sv_vm_t *vm, sv_func_t *func, ant_value_t this_val);
uint64_t opt_rt_to_boolean(ant_value_t value, sv_vm_t *vm);
uint64_t opt_rt_strict_equals(ant_value_t lhs, ant_value_t rhs, sv_vm_t *vm);
void opt_rt_write_barrier(sv_vm_t *vm, ant_object_t *object, ant_value_t value);
void opt_rt_element_barrier(sv_vm_t *vm, ant_object_t *array, uint32_t index, ant_value_t value);
void opt_rt_upvalue_barrier(sv_vm_t *vm, sv_upvalue_t *upvalue, ant_value_t value);
void opt_rt_shape_transition(sv_vm_t *vm, ant_object_t *object, ant_shape_t *to_shape);
void opt_rt_slow_paths_hot(sv_vm_t *vm, opt_code_t *code, sv_func_t *func, uint64_t bc_off);

#endif
