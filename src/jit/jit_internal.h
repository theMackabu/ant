#ifndef ANT_JIT_INTERNAL_H
#define ANT_JIT_INTERNAL_H

#include "silver/jit.h"
#include "silver/glue.h"
#include "silver/call.h"
#include "silver/feedback.h"
#include "silver/opcode.h"
#include "../silver/ops/globals.h"
#include "../silver/ops/literals.h"
#include "debug.h"
#include "shapes.h"
#include "gc/roots.h"
#include <internal.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmacro-redefined"
#include <mir.h>
#include <mir-gen.h>
#pragma GCC diagnostic pop
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

static constexpr int JIT_VSTACK_SLACK = 4;
static constexpr int JIT_PARAM_HOIST_CAP = 8;

static constexpr int JIT_OSR_THRESHOLD_SCALE_BYTES = 512;
static constexpr int JIT_OSR_COLD_COMPILE_MIN_BYTES = 512;

static constexpr uint32_t JIT_HOT_COMPILE_BACKEDGE_THRESHOLD = SV_JIT_OSR_THRESHOLD / 8;

typedef struct {
  MIR_context_t ctx;
  MIR_context_t ctx_hot;
  bool externals_loaded;
} sv_jit_ctx_t;

enum jit_slot_type { 
  SLOT_BOXED = 0,
  SLOT_NUM,
  SLOT_I32
};

typedef struct {
  int64_t min, max;
  bool known;
} jit_integer_range_t;

typedef struct {
  MIR_reg_t *regs, *d_regs;
  sv_func_t **known_func;
  uint8_t *slot_type;
  uint64_t *known_const;
  bool *has_const;
  uint8_t *known_bool;
  jit_integer_range_t *integer_range;
  int sp, max;
  bool overflow;
} jit_vstack_t;

typedef struct {
  sv_func_t *known_func;
  uint64_t known_const;
  bool has_const;
  uint8_t known_bool;
  jit_integer_range_t integer_range;
} jit_value_info_t;

#define MAX_LABELS 1024
typedef struct {
  int bc_off;
  MIR_label_t label;
  int sp;
} jit_label_t;
typedef struct {
  jit_label_t entries[MAX_LABELS];
  int count;
} jit_label_map_t;
#define MIR_JSVAL MIR_T_I64
#define JIT_ERR_TAG ((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | kTypeError)
#define JIT_STR_TAG ((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | kTypeString)
#define NANBOX_TARR_TAG ((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)kTypeArray)
#if defined(__aarch64__) || defined(__x86_64__)
#define SV_JIT_HAS_BITCAST 1
#else
#define SV_JIT_HAS_BITCAST 0
#endif

typedef struct {
  MIR_reg_t val, off, sp;
  MIR_label_t tramp;
  MIR_reg_t args_buf;
  jit_vstack_t *vstack;
  MIR_reg_t *local_regs, *local_d_regs;
  const uint8_t *dnum_locals;
  int n_locals;
  MIR_reg_t lbuf, d_slot;
} jit_bailout_emit_t;
#define NANBOX_TFUNC_TAG ((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)kTypeFunction)
#define NANBOX_TOBJ_TAG ((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)kTypeObject)
#define NANBOX_TPROM_TAG ((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)kTypePromise)
#define MAX_OSR_ENTRIES 64
typedef struct {
  int offsets[MAX_OSR_ENTRIES];
  int count;
} osr_entry_map_t;
typedef enum {
  JIT_CHILD_PLAIN = 0,
  JIT_CHILD_INHERITED_ONLY,
  JIT_CHILD_LOCAL_ONLY,
  JIT_CHILD_PARAM_ONLY,
  JIT_CHILD_MIXED,
} jit_child_kind_t;
#define JIT_INLINE_MAX_BYTECODE 192
typedef struct {
  MIR_item_t helper1_proto, imp_get_length_inline;
  MIR_item_t imp_get_elem_inline;
  MIR_item_t put_field_proto, imp_put_field;
  MIR_item_t shape_transition_proto, imp_shape_transition;
  MIR_item_t remember_obj_proto, imp_remember_obj;
  MIR_item_t call_proto, imp_call;
  MIR_item_t call_method_proto, imp_call_method;
  MIR_item_t stable_load_proto, imp_load_stable_builtin;
  MIR_item_t stable_call_proto, imp_call_stable_builtin;
  MIR_item_t imp_band, imp_bor, imp_bxor, imp_shl, imp_shr, imp_ushr;
  MIR_item_t self_proto;
  MIR_reg_t r_args_buf;
} jit_inline_ext_t;
#define INL_MAX_LABELS 128
typedef struct {
  int bc_off;
  MIR_label_t label;
  int sp;
} inl_label_entry_t;
typedef struct {
  inl_label_entry_t entries[INL_MAX_LABELS];
  int count;
} inl_label_map_t;
typedef struct {
  bool needs_bailout, needs_inc_local, needs_args_buf, needs_iter_roots;
  bool needs_close_upval, needs_tco_args, needs_ic_epoch, needs_this;
  bool needs_new_target, needs_super;
  bool *builder_target_slots;
} jit_features_t;

void jit_load_externals_once(sv_jit_ctx_t *jc);
jit_value_info_t vstack_value_info(const jit_vstack_t *vs, int idx);
void vstack_set_value_info(
    jit_vstack_t *vs, int idx, jit_value_info_t info);
void vstack_clear_value_info(jit_vstack_t *vs, int idx);
MIR_reg_t vstack_push(jit_vstack_t *vs);
MIR_reg_t vstack_push_const(jit_vstack_t *vs, uint64_t val);
MIR_reg_t vstack_pop(jit_vstack_t *vs);
MIR_reg_t vstack_top(jit_vstack_t *vs);
jit_integer_range_t jit_word_range(sv_op_t op, jit_integer_range_t left, jit_integer_range_t right);
void jit_emit_integer_constant(
    MIR_context_t ctx, MIR_item_t fn, jit_vstack_t *vs, MIR_reg_t dst, double number);
void jit_entry_integer_ranges(
    sv_func_t *func, jit_integer_range_t *ranges,
    int n_locals, int param_count);
bool jit_emit_integer_arithmetic(
    MIR_context_t ctx, MIR_item_t fn, jit_vstack_t *vs, sv_op_t op);
MIR_label_t label_for_offset(MIR_context_t ctx, jit_label_map_t *lm,
                             int bc_off);
int jit_constant_object_span(sv_func_t *func, sv_obj_site_cache_t *site, jit_label_map_t *lm);
MIR_label_t label_for_branch(MIR_context_t ctx, jit_label_map_t *lm,
                             int bc_off, int sp);
void mir_emit_decode_ref(
    MIR_context_t ctx, MIR_item_t fn, MIR_reg_t dst, MIR_reg_t value);
void mir_emit_cage_offset(
    MIR_context_t ctx, MIR_item_t fn, MIR_reg_t dst, MIR_reg_t ptr);
void mir_i64_to_d(MIR_context_t ctx, MIR_item_t fn,
                  MIR_reg_t dst_d, MIR_reg_t src_i64,
                  MIR_reg_t slot);
void mir_d_to_i64_non_nan(MIR_context_t ctx, MIR_item_t fn,
                          MIR_reg_t dst_i64, MIR_reg_t src_d,
                          MIR_reg_t slot);
void mir_d_to_i64(MIR_context_t ctx, MIR_item_t fn,
                  MIR_reg_t dst_i64, MIR_reg_t src_d,
                  MIR_reg_t slot);
void mir_emit_get_length(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t obj, MIR_reg_t dst,
    MIR_reg_t r_vm, MIR_reg_t r_js, MIR_reg_t r_d_slot,
    MIR_item_t helper1_proto, MIR_item_t imp_get_length,
    bool builder_slot,
    int owner_id, int bc_off);
void mir_emit_slot_boxed(MIR_context_t ctx, MIR_item_t fn,
                         MIR_reg_t boxed, MIR_reg_t number,
                         uint8_t slot_type, MIR_reg_t d_slot);
void vstack_ensure_boxed(jit_vstack_t *vs, int idx,
                         MIR_context_t ctx, MIR_item_t fn,
                         MIR_reg_t d_slot);
void vstack_ensure_num(jit_vstack_t *vs, int idx,
                       MIR_context_t ctx, MIR_item_t fn,
                       MIR_reg_t d_slot);
bool vstack_prepare_num(jit_vstack_t *vs, int idx,
                        MIR_context_t ctx, MIR_item_t fn,
                        MIR_reg_t d_slot);
void vstack_flush_to_boxed(jit_vstack_t *vs,
                           MIR_context_t ctx, MIR_item_t fn,
                           MIR_reg_t d_slot);
void vstack_rebox_binop_operands(
    jit_vstack_t *vs, MIR_context_t ctx, MIR_item_t fn,
    uint8_t lhs_type, uint8_t rhs_type,
    MIR_reg_t d_slot);
void mir_emit_dnum_rebox(MIR_context_t ctx, MIR_item_t fn,
                         const jit_bailout_emit_t *bail);
void mir_emit_bailout_jump_typed(MIR_context_t ctx, MIR_item_t fn,
                                 int bc_off, int pre_op_sp,
                                 const jit_bailout_emit_t *bail,
                                 int left_idx, uint8_t left_type,
                                 int right_idx, uint8_t right_type);
void mir_emit_bailout_check_typed(MIR_context_t ctx, MIR_item_t fn,
                                  MIR_reg_t res,
                                  MIR_reg_t restore_val,
                                  int bc_off, int pre_op_sp,
                                  const jit_bailout_emit_t *bail,
                                  int left_idx, uint8_t left_type,
                                  int right_idx, uint8_t right_type);
void mir_emit_bailout_check(MIR_context_t ctx, MIR_item_t fn,
                            MIR_reg_t res,
                            MIR_reg_t restore_val,
                            int bc_off, int pre_op_sp,
                            const jit_bailout_emit_t *bail);
void mir_emit_self_binding_guard(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t value, MIR_reg_t closure,
    MIR_reg_t tag_tmp, MIR_reg_t expected_tmp,
    int bc_off, int pre_op_sp,
    const jit_bailout_emit_t *bail);
void mir_emit_self_binding_guard_value_kept(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t value, MIR_reg_t closure,
    MIR_reg_t tag_tmp, MIR_reg_t expected_tmp,
    int bc_off, int op_sz, int pre_op_sp,
    const jit_bailout_emit_t *bail);
void mir_load_imm(MIR_context_t ctx, MIR_item_t fn,
                  MIR_reg_t dst, uint64_t imm);
void mir_emit_fill_param_slots_from_args(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t r_slotbuf, MIR_reg_t r_args, MIR_reg_t r_argc,
    bool *captured_params, int param_count, bool fill_all);
void mir_emit_fill_uncaptured_param_slots_from_args(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t r_slotbuf, MIR_reg_t r_args, MIR_reg_t r_argc,
    bool *captured_params, int param_count);
void mir_emit_spill_child_captured_locals(
    MIR_context_t ctx, MIR_item_t fn,
    sv_func_t *parent_func, sv_func_t *child,
    MIR_reg_t *local_regs, int n_locals, MIR_reg_t r_lbuf);
void mir_emit_close_marked_slots(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_item_t close_upval_proto, MIR_item_t imp_close_upval,
    MIR_reg_t r_vm, MIR_reg_t r_slots,
    MIR_reg_t r_open_upvalues,
    bool *captured, int start_idx, int slot_count, int site_id);
void mir_emit_upval_write_barrier(
    MIR_context_t ctx, MIR_item_t jit_func,
    MIR_item_t upval_barrier_proto, MIR_item_t imp_upval_barrier,
    MIR_reg_t r_js, MIR_reg_t r_uv, MIR_reg_t src, int un);
void mir_emit_exit_ret(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_item_t close_upval_proto, MIR_item_t imp_close_upval,
    MIR_item_t adopt_open_upvalues_proto, MIR_item_t imp_adopt_open_upvalues,
    MIR_reg_t r_vm, MIR_reg_t r_slotbuf, MIR_reg_t r_lbuf,
    MIR_reg_t r_jit_open_upvalues,
    bool has_captured_slots, bool *captured_params, int param_count,
    bool has_captures, bool *captured_locals, int n_locals,
    int *next_site, MIR_op_t ret_op);
void mir_emit_self_tail(
    MIR_context_t ctx, MIR_item_t fn,
    int call_argc, int param_count,
    MIR_reg_t r_tco_args, MIR_reg_t r_arg_arr,
    MIR_reg_t r_args, MIR_reg_t r_argc,
    MIR_reg_t *local_regs, int n_locals,
    bool has_captured_slots, MIR_reg_t r_slotbuf, bool *captured_params,
    bool fill_all_params,
    bool has_captures, bool *captured_locals,
    MIR_reg_t r_lbuf, MIR_label_t entry);
bool jit_const_is_heap(ant_value_t cv);
void mir_load_const_slot(MIR_context_t ctx, MIR_item_t fn,
                         MIR_reg_t dst, ant_value_t *slot);
void mir_call_helper2(MIR_context_t ctx, MIR_item_t fn,
                      MIR_reg_t dst,
                      MIR_item_t proto, MIR_item_t func_item,
                      MIR_reg_t vm_reg, MIR_reg_t js_reg,
                      MIR_reg_t arg0, MIR_reg_t arg1);
void mir_emit_is_num_guard(MIR_context_t ctx, MIR_item_t fn,
                           MIR_reg_t r_bool, MIR_reg_t v,
                           MIR_label_t slow);
int mir_next_reg_site(int *next_site);
MIR_reg_t mir_emit_exact_integer_guard(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t boxed, MIR_reg_t known_double, bool is_known_double,
    MIR_reg_t d_slot, double minimum, double maximum,
    MIR_label_t slow, int site);
void mir_emit_primitive_type_test(
    MIR_context_t ctx, MIR_item_t fn, MIR_reg_t value, MIR_reg_t scratch, uint8_t type);
void mir_emit_uncurried_char_code_at(
    MIR_context_t ctx, MIR_item_t fn, MIR_reg_t target, MIR_reg_t args,
    MIR_reg_t result, MIR_reg_t js, MIR_reg_t d_slot,
    MIR_label_t slow, MIR_label_t done, int site);
MIR_reg_t mir_emit_word32_guard(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t boxed, MIR_reg_t known_double,
    bool is_known_double, bool is_known_i32,
    MIR_reg_t d_slot, MIR_label_t slow, int site);
MIR_reg_t mir_emit_array_index_guard(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t boxed, MIR_reg_t known_double, bool is_known_double,
    MIR_reg_t d_slot, MIR_label_t slow, int site);
MIR_reg_t mir_emit_known_array_index_guard(
    MIR_context_t ctx, MIR_item_t fn, MIR_reg_t boxed,
    MIR_reg_t integer, MIR_label_t slow, MIR_reg_t d_slot, int site,
    MIR_reg_t cached_key, MIR_reg_t cached_index);
typedef enum {
  JIT_ELEMENT_NUMERIC_READ,
  JIT_ELEMENT_READ,
  JIT_ELEMENT_WRITE,
} jit_element_access_t;
MIR_reg_t mir_emit_dense_element_guard(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t object, MIR_reg_t index, MIR_reg_t value,
    jit_element_access_t access, MIR_label_t slow, int site);
void mir_emit_word32_binary(
    MIR_context_t ctx, MIR_item_t fn, sv_op_t op,
    MIR_reg_t left, MIR_reg_t right, MIR_reg_t result,
    MIR_reg_t result_double, bool materialize_double, int site);
void mir_emit_numeric_equality(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t left, MIR_reg_t right, MIR_reg_t result,
    MIR_reg_t scratch, bool invert);
void mir_emit_strict_tagged_equality(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t left, MIR_reg_t right, MIR_reg_t result,
    MIR_reg_t scratch, bool invert,
    MIR_label_t slow, MIR_label_t done);
void mir_emit_branch_if_string_builder(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t value, MIR_reg_t scratch, MIR_label_t builder);
bool jit_upvalue_is_builder_target(sv_func_t *func, uint32_t idx);
MIR_label_t mir_emit_string_builder_read_open(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t value, MIR_reg_t scratch,
    MIR_reg_t r_vm, MIR_reg_t r_js,
    MIR_item_t helper1_proto, MIR_item_t imp_str_read_value);
void mir_emit_string_builder_append_ascii_byte(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t lhs, MIR_reg_t rhs, MIR_reg_t result,
    MIR_reg_t d_slot,
    MIR_label_t slow, MIR_label_t done,
    int owner_id, int bc_off);
void mir_emit_numeric_local_store_mirror(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t local_d,
    MIR_reg_t src,
    MIR_reg_t src_d,
    bool src_is_num,
    MIR_reg_t r_bool,
    int resume_bc_off,
    int post_op_sp,
    const jit_bailout_emit_t *bail);
void mir_emit_call_stable_builtin(
    MIR_context_t ctx, MIR_item_t fn, ant_t *js,
    MIR_reg_t r_vm, MIR_reg_t r_js,
    int kind, MIR_reg_t call_func, MIR_reg_t call_this,
    MIR_reg_t arg0, MIR_reg_t args, int argc, MIR_reg_t dst,
    MIR_item_t call_proto, MIR_item_t imp_call,
    bool known_intrinsic, bool args_prepared, int site_id);
void mir_emit_load_stable_builtin(
    MIR_context_t ctx, MIR_item_t fn, ant_t *js,
    MIR_reg_t r_js, int kind,
    MIR_reg_t receiver, MIR_reg_t func, MIR_reg_t receiver_out,
    MIR_item_t load_proto, MIR_item_t imp_load, int site_id);
void mir_emit_get_closure(MIR_context_t ctx, MIR_item_t fn,
                          MIR_reg_t dst, MIR_reg_t v,
                          MIR_reg_t r_tag, MIR_label_t fallback);
void mir_emit_resolve_call_this(MIR_context_t ctx, MIR_item_t fn,
                                MIR_reg_t dst, MIR_reg_t r_closure,
                                MIR_reg_t fallback_this,
                                MIR_reg_t r_flags, MIR_reg_t r_bound);
void mir_emit_value_to_objptr_or_jmp(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t v, MIR_reg_t out_ptr,
    MIR_reg_t r_tag, MIR_label_t slow);
void mir_emit_ic_obj_epoch_guard(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t ic, MIR_label_t slow,
    const char *prefix, int bc_off, uint16_t ic_idx);
void mir_emit_string_concat_fastpath(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t r_js, MIR_reg_t lhs, MIR_reg_t rhs, MIR_reg_t dst,
    MIR_label_t slow, int owner_id, int bc_off, bool flat_only);
bool mir_emit_put_field_ic_fastpath(
    MIR_context_t ctx, MIR_item_t fn, ant_t *js,
    sv_func_t *func, int bc_off, uint16_t ic_idx, sv_atom_t *atom,
    MIR_reg_t r_js, MIR_reg_t obj, MIR_reg_t val,
    MIR_label_t slow, MIR_reg_t r_global_epoch,
    MIR_item_t shape_transition_proto, MIR_item_t imp_shape_transition,
    MIR_item_t remember_proto, MIR_item_t imp_remember);
bool mir_emit_get_field_ic_fastpath(
    MIR_context_t ctx,
    MIR_item_t fn,
    sv_func_t *func,
    int bc_off,
    uint16_t ic_idx,
    sv_atom_t *atom,
    MIR_reg_t obj,
    MIR_reg_t dst,
    MIR_label_t slow,
    MIR_reg_t r_global_epoch);
bool mir_emit_get_global_ic_fastpath(
    MIR_context_t ctx, MIR_item_t fn,
    sv_func_t *func, int bc_off,
    MIR_reg_t r_js, MIR_reg_t dst,
    MIR_label_t slow, MIR_reg_t r_global_epoch,
    uint8_t *ip);
void scan_osr_entries(sv_func_t *func, osr_entry_map_t *osr);
bool func_writes_params(sv_func_t *func);
jit_child_kind_t classify_child_closure_kind(sv_func_t *parent, sv_func_t *child);
bool *scan_captured_locals(sv_func_t *func, int n_locals);
bool *scan_captured_params(sv_func_t *func);
bool jit_inlineable(sv_func_t *f);
bool jit_can_forward_arguments(sv_func_t *func);
bool jit_has_immediate_numeric_local_init(sv_func_t *func, uint8_t *ip, uint8_t *end, uint16_t local_idx);
void jit_emit_inline_body(
    MIR_context_t ctx, MIR_item_t jit_func, ant_t *js,
    sv_func_t *callee,
    MIR_reg_t *arg_regs, int caller_argc,
    const uint8_t *arg_num, const MIR_reg_t *arg_d,
    MIR_reg_t result, MIR_label_t slow, MIR_label_t join,
    MIR_reg_t r_bool, MIR_reg_t *p_d_slot, int id, int *p_reg_site,
    MIR_reg_t r_inl_closure, MIR_reg_t r_inl_this,
    MIR_reg_t r_inl_new_target, MIR_reg_t r_inl_super,
    MIR_reg_t r_vm, MIR_reg_t r_js, MIR_reg_t r_ic_epoch,
    MIR_item_t helper2_proto, MIR_item_t imp_seq,
    MIR_item_t imp_sne, MIR_item_t imp_eq, MIR_item_t imp_ne,
    MIR_item_t gf_proto, MIR_item_t imp_get_field_inline,
    MIR_item_t special_obj_proto, MIR_item_t imp_special_obj,
    const jit_inline_ext_t *ext);
bool scan_branch_targets(sv_func_t *func, jit_label_map_t *lm, MIR_context_t ctx);
jit_features_t jit_prescan_features(sv_func_t *func, int n_slots);
bool jit_mark_self_binding_guards(
    ant_t *js, sv_func_t *func, sv_closure_t *hint_closure,
    uint8_t *guard_sites);
bool jit_is_eligible(sv_func_t *func);

#endif
