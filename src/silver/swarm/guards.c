#include "internal.h"
static uint8_t jit_bailout_slot_type(jit_vstack_t *vs, int idx,
                                     int left_idx, uint8_t left_type,
                                     int right_idx, uint8_t right_type) {
  if (idx == left_idx) return left_type;
  if (idx == right_idx) return right_type;
  return vs->slot_type ? vs->slot_type[idx] : SLOT_BOXED;
}

void mir_emit_dnum_rebox(MIR_context_t ctx, MIR_item_t fn,
                         const jit_bailout_emit_t *bail) {
  if (!bail->dnum_locals) return;
  for (int i = 0; i < bail->n_locals; i++)
    if (bail->dnum_locals[i])
      mir_d_to_i64(ctx, fn, bail->local_regs[i], bail->local_d_regs[i],
                   bail->d_slot);
}

void mir_emit_bailout_jump_typed(MIR_context_t ctx, MIR_item_t fn,
                                 int bc_off, int pre_op_sp,
                                 const jit_bailout_emit_t *bail,
                                 int left_idx, uint8_t left_type,
                                 int right_idx, uint8_t right_type) {
  for (int i = 0; i < pre_op_sp; i++) {
    uint8_t slot_type = jit_bailout_slot_type(
        bail->vstack, i, left_idx, left_type, right_idx, right_type);
    mir_emit_slot_boxed(
        ctx, fn, bail->vstack->regs[i], bail->vstack->d_regs[i],
        slot_type, bail->d_slot);
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_T_I64,
                                                (MIR_disp_t)(i * (int)sizeof(ant_value_t)), bail->args_buf, 0, 1),
                                 MIR_new_reg_op(ctx, bail->vstack->regs[i])));
  }
  mir_emit_dnum_rebox(ctx, fn, bail);
  for (int i = 0; i < bail->n_locals; i++)
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_T_I64,
                                                (MIR_disp_t)(i * (int)sizeof(ant_value_t)), bail->lbuf, 0, 1),
                                 MIR_new_reg_op(ctx, bail->local_regs[i])));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, bail->off),
                               MIR_new_int_op(ctx, bc_off)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, bail->sp),
                               MIR_new_int_op(ctx, pre_op_sp)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP,
                               MIR_new_label_op(ctx, bail->tramp)));
}

void mir_emit_bailout_check_typed(MIR_context_t ctx, MIR_item_t fn,
                                  MIR_reg_t res,
                                  MIR_reg_t restore_val,
                                  int bc_off, int pre_op_sp,
                                  const jit_bailout_emit_t *bail,
                                  int left_idx, uint8_t left_type,
                                  int right_idx, uint8_t right_type) {
  MIR_label_t no_bail = MIR_new_label(ctx);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, no_bail),
                               MIR_new_reg_op(ctx, res),
                               MIR_new_uint_op(ctx, (uint64_t)SV_JIT_BAILOUT)));
  if (restore_val)
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_reg_op(ctx, res),
                                 MIR_new_reg_op(ctx, restore_val)));
  mir_emit_bailout_jump_typed(ctx, fn, bc_off, pre_op_sp, bail,
                              left_idx, left_type, right_idx, right_type);
  MIR_append_insn(ctx, fn, no_bail);
}

void mir_emit_bailout_check(MIR_context_t ctx, MIR_item_t fn,
                            MIR_reg_t res,
                            MIR_reg_t restore_val,
                            int bc_off, int pre_op_sp,
                            const jit_bailout_emit_t *bail) {
  mir_emit_bailout_check_typed(ctx, fn, res, restore_val,
                               bc_off, pre_op_sp, bail, -1, false, -1, false);
}

void mir_emit_self_binding_guard(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t value, MIR_reg_t closure,
    MIR_reg_t tag_tmp, MIR_reg_t expected_tmp,
    int bc_off, int pre_op_sp,
    const jit_bailout_emit_t *bail) {
  MIR_label_t match = MIR_new_label(ctx);
  uint64_t func_tag = NANBOX_PREFIX | ((ant_value_t)(kTypeFunction & NANBOX_TYPE_MASK) << NANBOX_TYPE_SHIFT);

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, tag_tmp),
                               MIR_new_uint_op(ctx, func_tag)));
  mir_emit_cage_offset(ctx, fn, expected_tmp, closure);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_OR,
                               MIR_new_reg_op(ctx, expected_tmp),
                               MIR_new_reg_op(ctx, tag_tmp),
                               MIR_new_reg_op(ctx, expected_tmp)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, match),
                               MIR_new_reg_op(ctx, value),
                               MIR_new_reg_op(ctx, expected_tmp)));
  mir_emit_bailout_jump_typed(ctx, fn, bc_off, pre_op_sp, bail,
                              -1, false, -1, false);
  MIR_append_insn(ctx, fn, match);
}

void mir_emit_self_binding_guard_value_kept(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t value, MIR_reg_t closure,
    MIR_reg_t tag_tmp, MIR_reg_t expected_tmp,
    int bc_off, int op_sz, int pre_op_sp,
    const jit_bailout_emit_t *bail) {
  MIR_label_t match = MIR_new_label(ctx);
  MIR_label_t not_undef = MIR_new_label(ctx);
  uint64_t func_tag = NANBOX_PREFIX | ((ant_value_t)(kTypeFunction & NANBOX_TYPE_MASK) << NANBOX_TYPE_SHIFT);

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, tag_tmp),
                               MIR_new_uint_op(ctx, func_tag)));
  mir_emit_cage_offset(ctx, fn, expected_tmp, closure);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_OR,
                               MIR_new_reg_op(ctx, expected_tmp),
                               MIR_new_reg_op(ctx, tag_tmp),
                               MIR_new_reg_op(ctx, expected_tmp)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, match),
                               MIR_new_reg_op(ctx, value),
                               MIR_new_reg_op(ctx, expected_tmp)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, not_undef),
                               MIR_new_reg_op(ctx, value),
                               MIR_new_uint_op(ctx, mkval(kTypeUndefined, 0))));
  mir_emit_bailout_jump_typed(ctx, fn, bc_off, pre_op_sp, bail,
                              -1, false, -1, false);
  MIR_append_insn(ctx, fn, not_undef);
  mir_emit_bailout_jump_typed(ctx, fn, bc_off + op_sz, pre_op_sp + 1, bail,
                              -1, false, -1, false);
  MIR_append_insn(ctx, fn, match);
}

void mir_load_imm(MIR_context_t ctx, MIR_item_t fn,
                  MIR_reg_t dst, uint64_t imm) {
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, dst),
                               MIR_new_uint_op(ctx, imm)));
}

void mir_emit_fill_param_slots_from_args(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t r_slotbuf, MIR_reg_t r_args, MIR_reg_t r_argc,
    bool *captured_params, int param_count, bool fill_all) {
  if (!captured_params && !fill_all) return;
  for (int i = 0; i < param_count; i++) {
    if (!fill_all && !captured_params[i]) continue;
    MIR_label_t arg_present = MIR_new_label(ctx);
    MIR_label_t arg_done = MIR_new_label(ctx);
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_UBGT,
                                 MIR_new_label_op(ctx, arg_present),
                                 MIR_new_reg_op(ctx, r_argc),
                                 MIR_new_int_op(ctx, (int64_t)i)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_T_I64,
                                                (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_slotbuf, 0, 1),
                                 MIR_new_uint_op(ctx, mkval(kTypeUndefined, 0))));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_JMP,
                                 MIR_new_label_op(ctx, arg_done)));
    MIR_append_insn(ctx, fn, arg_present);
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_T_I64,
                                                (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_slotbuf, 0, 1),
                                 MIR_new_mem_op(ctx, MIR_T_I64,
                                                (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args, 0, 1)));
    MIR_append_insn(ctx, fn, arg_done);
  }
}

void mir_emit_fill_uncaptured_param_slots_from_args(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t r_slotbuf, MIR_reg_t r_args, MIR_reg_t r_argc,
    bool *captured_params, int param_count) {
  if (!captured_params) return;
  for (int i = 0; i < param_count; i++) {
    if (captured_params[i]) continue;
    MIR_label_t arg_present = MIR_new_label(ctx);
    MIR_label_t arg_done = MIR_new_label(ctx);
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_UBGT,
                                 MIR_new_label_op(ctx, arg_present),
                                 MIR_new_reg_op(ctx, r_argc),
                                 MIR_new_int_op(ctx, (int64_t)i)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_T_I64,
                                                (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_slotbuf, 0, 1),
                                 MIR_new_uint_op(ctx, mkval(kTypeUndefined, 0))));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_JMP,
                                 MIR_new_label_op(ctx, arg_done)));
    MIR_append_insn(ctx, fn, arg_present);
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_T_I64,
                                                (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_slotbuf, 0, 1),
                                 MIR_new_mem_op(ctx, MIR_T_I64,
                                                (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_args, 0, 1)));
    MIR_append_insn(ctx, fn, arg_done);
  }
}

void mir_emit_spill_child_captured_locals(
    MIR_context_t ctx, MIR_item_t fn,
    sv_func_t *parent_func, sv_func_t *child,
    MIR_reg_t *local_regs, int n_locals, MIR_reg_t r_lbuf) {
  if (!child || !parent_func || !local_regs || n_locals <= 0 || !r_lbuf) return;

  for (int i = 0; i < child->upvalue_count; i++) {
    sv_upval_desc_t *desc = &child->upval_descs[i];
    if (!desc->is_local) continue;

    int li = (int)desc->index - parent_func->param_count;
    if (li < 0 || li >= n_locals) continue;

    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_T_I64,
                                                (MIR_disp_t)(li * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                                 MIR_new_reg_op(ctx, local_regs[li])));
  }
}

void mir_emit_close_marked_slots(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_item_t close_upval_proto, MIR_item_t imp_close_upval,
    MIR_reg_t r_vm, MIR_reg_t r_slots,
    MIR_reg_t r_open_upvalues,
    bool *captured, int start_idx, int slot_count) {
  if (!captured || start_idx >= slot_count || slot_count <= 0 || !r_slots) return;
  if (start_idx < 0) start_idx = 0;

  int first_captured = -1;
  for (int i = start_idx; i < slot_count; i++)
    if (captured[i]) {
      first_captured = i;
      break;
    }
  if (first_captured < 0) return;

  static uint32_t close_guard_seq = 0;
  char open_name[32];
  snprintf(open_name, sizeof(open_name), "open_upvals_%u", close_guard_seq++);
  MIR_reg_t r_open = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, open_name);
  MIR_label_t no_open = MIR_new_label(ctx);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_open),
                               MIR_new_mem_op(ctx, MIR_T_I64,
                                              r_open_upvalues ? 0 : (MIR_disp_t)offsetof(sv_vm_t, open_upvalues),
                                              r_open_upvalues ? r_open_upvalues : r_vm, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, no_open),
                               MIR_new_reg_op(ctx, r_open),
                               MIR_new_uint_op(ctx, 0)));

  MIR_append_insn(ctx, fn,
                  MIR_new_call_insn(ctx, 7,
                                    MIR_new_ref_op(ctx, close_upval_proto),
                                    MIR_new_ref_op(ctx, imp_close_upval),
                                    MIR_new_reg_op(ctx, r_vm),
                                    MIR_new_uint_op(ctx, (uint64_t)first_captured),
                                    MIR_new_reg_op(ctx, r_slots),
                                    MIR_new_int_op(ctx, slot_count),
                                    r_open_upvalues ? MIR_new_reg_op(ctx, r_open_upvalues) : MIR_new_uint_op(ctx, 0)));
  MIR_append_insn(ctx, fn, no_open);
}

void mir_emit_upval_write_barrier(
    MIR_context_t ctx, MIR_item_t jit_func,
    MIR_item_t upval_barrier_proto, MIR_item_t imp_upval_barrier,
    MIR_reg_t r_js, MIR_reg_t r_uv, MIR_reg_t src, int un) {
  MIR_label_t skip_barrier = MIR_new_label(ctx);
  char rn_tmp[32];
  snprintf(rn_tmp, sizeof(rn_tmp), "uvwb%d", un);
  MIR_reg_t r_tmp = MIR_new_func_reg(ctx, jit_func->u.func, MIR_T_I64, rn_tmp);

  MIR_append_insn(ctx, jit_func,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_tmp),
                               MIR_new_mem_op(ctx, MIR_T_U8,
                                              (MIR_disp_t)offsetof(sv_upvalue_t, in_remember_set),
                                              r_uv, 0, 1)));
  MIR_append_insn(ctx, jit_func,
                  MIR_new_insn(ctx, MIR_BT,
                               MIR_new_label_op(ctx, skip_barrier),
                               MIR_new_reg_op(ctx, r_tmp)));
  MIR_append_insn(ctx, jit_func,
                  MIR_new_insn(ctx, MIR_UBLE,
                               MIR_new_label_op(ctx, skip_barrier),
                               MIR_new_reg_op(ctx, src),
                               MIR_new_uint_op(ctx, NANBOX_PREFIX)));
  MIR_append_insn(ctx, jit_func,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_tmp),
                               MIR_new_mem_op(ctx, MIR_T_I64,
                                              (MIR_disp_t)offsetof(sv_upvalue_t, gc_epoch),
                                              r_uv, 0, 1)));
  MIR_append_insn(ctx, jit_func,
                  MIR_new_insn(ctx, MIR_BF,
                               MIR_new_label_op(ctx, skip_barrier),
                               MIR_new_reg_op(ctx, r_tmp)));
  MIR_append_insn(ctx, jit_func,
                  MIR_new_call_insn(ctx, 5,
                                    MIR_new_ref_op(ctx, upval_barrier_proto),
                                    MIR_new_ref_op(ctx, imp_upval_barrier),
                                    MIR_new_reg_op(ctx, r_js),
                                    MIR_new_reg_op(ctx, r_uv),
                                    MIR_new_reg_op(ctx, src)));
  MIR_append_insn(ctx, jit_func, skip_barrier);
}

static void mir_emit_exit_upvalue_cleanup(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_item_t close_upval_proto, MIR_item_t imp_close_upval,
    MIR_item_t adopt_open_upvalues_proto, MIR_item_t imp_adopt_open_upvalues,
    MIR_reg_t r_vm, MIR_reg_t r_slotbuf, MIR_reg_t r_lbuf,
    MIR_reg_t r_jit_open_upvalues,
    bool has_captured_slots, bool *captured_params, int param_count,
    bool has_captures, bool *captured_locals, int n_locals) {
  if (has_captured_slots)
    mir_emit_close_marked_slots(ctx, fn,
                                close_upval_proto, imp_close_upval,
                                r_vm, r_slotbuf, r_jit_open_upvalues, captured_params, 0, param_count);
  if (has_captures)
    mir_emit_close_marked_slots(ctx, fn,
                                close_upval_proto, imp_close_upval,
                                r_vm, r_lbuf, r_jit_open_upvalues, captured_locals, 0, n_locals);
  if (r_jit_open_upvalues) {
    MIR_append_insn(ctx, fn,
                    MIR_new_call_insn(ctx, 4,
                                      MIR_new_ref_op(ctx, adopt_open_upvalues_proto),
                                      MIR_new_ref_op(ctx, imp_adopt_open_upvalues),
                                      MIR_new_reg_op(ctx, r_vm),
                                      MIR_new_reg_op(ctx, r_jit_open_upvalues)));
  }
}

void mir_emit_exit_ret(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_item_t close_upval_proto, MIR_item_t imp_close_upval,
    MIR_item_t adopt_open_upvalues_proto, MIR_item_t imp_adopt_open_upvalues,
    MIR_reg_t r_vm, MIR_reg_t r_slotbuf, MIR_reg_t r_lbuf,
    MIR_reg_t r_jit_open_upvalues,
    bool has_captured_slots, bool *captured_params, int param_count,
    bool has_captures, bool *captured_locals, int n_locals,
    MIR_op_t ret_op) {
  mir_emit_exit_upvalue_cleanup(ctx, fn,
                                close_upval_proto, imp_close_upval,
                                adopt_open_upvalues_proto, imp_adopt_open_upvalues,
                                r_vm, r_slotbuf, r_lbuf, r_jit_open_upvalues,
                                has_captured_slots, captured_params, param_count,
                                has_captures, captured_locals, n_locals);
  MIR_append_insn(ctx, fn, MIR_new_ret_insn(ctx, 1, ret_op));
}

void mir_emit_self_tail(
    MIR_context_t ctx, MIR_item_t fn,
    int call_argc, int param_count,
    MIR_reg_t r_tco_args, MIR_reg_t r_arg_arr,
    MIR_reg_t r_args, MIR_reg_t r_argc,
    MIR_reg_t *local_regs, int n_locals,
    bool has_captured_slots, MIR_reg_t r_slotbuf, bool *captured_params,
    bool fill_all_params,
    bool has_captures, bool *captured_locals,
    MIR_reg_t r_lbuf, MIR_label_t entry) {
  for (int i = 0; i < call_argc && i < param_count; i++)
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_T_I64,
                                                (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_tco_args, 0, 1),
                                 MIR_new_mem_op(ctx, MIR_T_I64,
                                                (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_arg_arr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_args),
                               MIR_new_reg_op(ctx, r_tco_args)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_argc),
                               MIR_new_int_op(ctx, (int64_t)call_argc)));
  if (has_captured_slots)
    mir_emit_fill_param_slots_from_args(ctx, fn, r_slotbuf, r_tco_args, r_argc, captured_params, param_count, fill_all_params);
  for (int i = 0; i < n_locals; i++)
    mir_load_imm(ctx, fn, local_regs[i], mkval(kTypeUndefined, 0));
  if (has_captures) {
    for (int i = 0; i < n_locals; i++)
      if (captured_locals[i])
        MIR_append_insn(ctx, fn,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_mem_op(ctx, MIR_T_I64,
                                                    (MIR_disp_t)(i * (int)sizeof(ant_value_t)), r_lbuf, 0, 1),
                                     MIR_new_uint_op(ctx, mkval(kTypeUndefined, 0))));
  }
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP,
                               MIR_new_label_op(ctx, entry)));
}
