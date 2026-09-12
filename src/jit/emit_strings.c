#include "compile.h"

void jit_emit_strings(jit_compile_t *c) {
  switch (c->op) {
    case OP_STR_APPEND_LOCAL: {
      uint16_t slot_idx = sv_get_u16(c->ip + 1);
      int pre_op_sp = c->vs.sp;
      MIR_reg_t rhs;
      MIR_reg_t lhs;
      MIR_label_t slow = MIR_new_label(c->ctx);
      MIR_label_t append_done = MIR_new_label(c->ctx);

      if ((int)slot_idx < c->param_count) {
        if (!c->writes_params) {
          MIR_label_t arg_in_range = MIR_new_label(c->ctx);
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_UBGT,
                                       MIR_new_label_op(c->ctx, arg_in_range),
                                       MIR_new_reg_op(c->ctx, c->r_argc),
                                       MIR_new_int_op(c->ctx, (int64_t)slot_idx)));
          mir_load_imm(c->ctx, c->jit_func, c->r_bailout_val, (uint64_t)SV_JIT_BAILOUT);
          mir_emit_bailout_check(c->ctx, c->jit_func, c->r_bailout_val,
                                 0, c->bc_off, pre_op_sp, &c->bailout_ctx);
          MIR_append_insn(c->ctx, c->jit_func, arg_in_range);
        }

        vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
        rhs = vstack_pop(&c->vs);

        char lhs_name[48];
        snprintf(lhs_name, sizeof(lhs_name), "sab_param_%d", c->bc_off);
        lhs = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, lhs_name);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, lhs),
                                     MIR_new_mem_op(c->ctx, MIR_JSVAL,
                                                    (MIR_disp_t)((int)slot_idx * (int)sizeof(ant_value_t)),
                                                    c->writes_params ? c->r_slotbuf : c->r_args, 0, 1)));

        mir_emit_string_builder_append_ascii_byte(
            c->ctx, c->jit_func, lhs, rhs, c->r_err_tmp,
            c->r_d_slot,
            slow, append_done, -1, c->bc_off);
        MIR_append_insn(c->ctx, c->jit_func, slow);

        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_call_insn(c->ctx, 11,
                                          MIR_new_ref_op(c->ctx, c->str_append_local_proto),
                                          MIR_new_ref_op(c->ctx, c->imp_str_append_local),
                                          MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                          MIR_new_reg_op(c->ctx, c->r_vm),
                                          MIR_new_reg_op(c->ctx, c->r_js),
                                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                          c->writes_params ? MIR_new_reg_op(c->ctx, c->r_slotbuf) : MIR_new_reg_op(c->ctx, c->r_args),
                                          c->writes_params ? MIR_new_int_op(c->ctx, c->param_count) : MIR_new_reg_op(c->ctx, c->r_argc),
                                          MIR_new_uint_op(c->ctx, 0),
                                          MIR_new_int_op(c->ctx, (int64_t)slot_idx),
                                          MIR_new_reg_op(c->ctx, rhs)));
      } else {
        uint16_t local_idx = (uint16_t)(slot_idx - (uint16_t)c->param_count);
        if (local_idx >= (uint16_t)c->n_locals) {
          c->ok = false;
          break;
        }

        vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
        rhs = vstack_pop(&c->vs);

        lhs = c->local_regs[local_idx];
        mir_emit_string_builder_append_ascii_byte(
            c->ctx, c->jit_func, lhs, rhs, c->r_err_tmp,
            c->r_d_slot,
            slow, append_done, -1, c->bc_off);
        MIR_append_insn(c->ctx, c->jit_func, slow);

        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)((int)local_idx * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1),
                                     MIR_new_reg_op(c->ctx, c->local_regs[local_idx])));

        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_call_insn(c->ctx, 11,
                                          MIR_new_ref_op(c->ctx, c->str_append_local_proto),
                                          MIR_new_ref_op(c->ctx, c->imp_str_append_local),
                                          MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                          MIR_new_reg_op(c->ctx, c->r_vm),
                                          MIR_new_reg_op(c->ctx, c->r_js),
                                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                          MIR_new_uint_op(c->ctx, 0),
                                          MIR_new_int_op(c->ctx, (int64_t)c->param_count),
                                          MIR_new_reg_op(c->ctx, c->r_lbuf),
                                          MIR_new_int_op(c->ctx, (int64_t)slot_idx),
                                          MIR_new_reg_op(c->ctx, rhs)));

        if (c->has_captures) {
          for (int i = 0; i < c->n_locals; i++)
            if (c->captured_locals[i])
              MIR_append_insn(c->ctx, c->jit_func,
                              MIR_new_insn(c->ctx, MIR_MOV,
                                           MIR_new_reg_op(c->ctx, c->local_regs[i]),
                                           MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                          (MIR_disp_t)(i * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1)));
        }

        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->local_regs[local_idx]),
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)((int)local_idx * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1)));
        if (c->known_func_locals) c->known_func_locals[local_idx] = NULL;
        if (c->known_type_locals && c->known_type_locals[local_idx] != SV_TI_NUM)
          c->known_type_locals[local_idx] = SV_TI_UNKNOWN;
      }

      MIR_append_insn(c->ctx, c->jit_func, append_done);

      mir_emit_bailout_check(c->ctx, c->jit_func, c->r_err_tmp,
                             0, c->bc_off, pre_op_sp, &c->bailout_ctx);

      jit_emit_throw_if_error(c, c->r_err_tmp);
      if ((int)slot_idx >= c->param_count) {
        int gli = (int)slot_idx - c->param_count;
        if (gli < c->n_locals && c->local_d_regs && c->known_type_locals && c->known_type_locals[gli] == SV_TI_NUM)
          mir_emit_numeric_local_store_mirror(c->ctx, c->jit_func,
                                              c->local_d_regs[gli], c->local_regs[gli], 0, false,
                                              c->r_bool, c->bc_off + c->sz, c->vs.sp, &c->bailout_ctx);
      }
      break;
    }

    case OP_STR_ALC_SNAPSHOT: {
      uint16_t slot_idx = sv_get_u16(c->ip + 1);
      int pre_op_sp = c->vs.sp;
      MIR_label_t flat_append_done = NULL;
      if ((int)slot_idx < c->param_count) {
        if (!c->writes_params) {
          MIR_label_t arg_in_range = MIR_new_label(c->ctx);
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_UBGT,
                                       MIR_new_label_op(c->ctx, arg_in_range),
                                       MIR_new_reg_op(c->ctx, c->r_argc),
                                       MIR_new_int_op(c->ctx, (int64_t)slot_idx)));
          mir_load_imm(c->ctx, c->jit_func, c->r_bailout_val, (uint64_t)SV_JIT_BAILOUT);
          mir_emit_bailout_check(c->ctx, c->jit_func, c->r_bailout_val,
                                 0, c->bc_off, pre_op_sp, &c->bailout_ctx);
          MIR_append_insn(c->ctx, c->jit_func, arg_in_range);
        }

        vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
        vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
        MIR_reg_t rhs = vstack_pop(&c->vs);
        MIR_reg_t lhs = vstack_pop(&c->vs);

        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_call_insn(c->ctx, 12,
                                          MIR_new_ref_op(c->ctx, c->str_append_local_snapshot_proto),
                                          MIR_new_ref_op(c->ctx, c->imp_str_append_local_snapshot),
                                          MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                          MIR_new_reg_op(c->ctx, c->r_vm),
                                          MIR_new_reg_op(c->ctx, c->r_js),
                                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                          c->writes_params ? MIR_new_reg_op(c->ctx, c->r_slotbuf) : MIR_new_reg_op(c->ctx, c->r_args),
                                          c->writes_params ? MIR_new_int_op(c->ctx, c->param_count) : MIR_new_reg_op(c->ctx, c->r_argc),
                                          MIR_new_uint_op(c->ctx, 0),
                                          MIR_new_int_op(c->ctx, (int64_t)slot_idx),
                                          MIR_new_reg_op(c->ctx, lhs),
                                          MIR_new_reg_op(c->ctx, rhs)));
      } else {
        uint16_t local_idx = (uint16_t)(slot_idx - (uint16_t)c->param_count);
        if (local_idx >= (uint16_t)c->n_locals) {
          c->ok = false;
          break;
        }

        vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
        vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
        MIR_reg_t rhs = vstack_pop(&c->vs);
        MIR_reg_t lhs = vstack_pop(&c->vs);

        if ((!c->captured_locals || !c->captured_locals[local_idx]) &&
            (!c->known_type_locals || c->known_type_locals[local_idx] != SV_TI_NUM)) {
          MIR_label_t flat_append_slow = MIR_new_label(c->ctx);
          flat_append_done = MIR_new_label(c->ctx);
          mir_emit_string_concat_fastpath(c->ctx, c->jit_func, c->r_js, lhs, rhs, c->r_tmp, flat_append_slow, -1, c->bc_off, true);
          MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, c->local_regs[local_idx]), MIR_new_reg_op(c->ctx, c->r_tmp)));
          MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_mem_op(c->ctx, MIR_JSVAL, (MIR_disp_t)((size_t)local_idx * sizeof(ant_value_t)), c->r_lbuf, 0, 1), MIR_new_reg_op(c->ctx, c->r_tmp)));
          mir_load_imm(c->ctx, c->jit_func, c->r_err_tmp, js_mkundef());
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, flat_append_done)));
          MIR_append_insn(c->ctx, c->jit_func, flat_append_slow);
        }

        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)((int)local_idx * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1),
                                     MIR_new_reg_op(c->ctx, c->local_regs[local_idx])));

        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_call_insn(c->ctx, 12,
                                          MIR_new_ref_op(c->ctx, c->str_append_local_snapshot_proto),
                                          MIR_new_ref_op(c->ctx, c->imp_str_append_local_snapshot),
                                          MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                          MIR_new_reg_op(c->ctx, c->r_vm),
                                          MIR_new_reg_op(c->ctx, c->r_js),
                                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                          MIR_new_uint_op(c->ctx, 0),
                                          MIR_new_int_op(c->ctx, (int64_t)c->param_count),
                                          MIR_new_reg_op(c->ctx, c->r_lbuf),
                                          MIR_new_int_op(c->ctx, (int64_t)slot_idx),
                                          MIR_new_reg_op(c->ctx, lhs),
                                          MIR_new_reg_op(c->ctx, rhs)));

        if (c->has_captures) {
          for (int i = 0; i < c->n_locals; i++)
            if (c->captured_locals[i])
              MIR_append_insn(c->ctx, c->jit_func,
                              MIR_new_insn(c->ctx, MIR_MOV,
                                           MIR_new_reg_op(c->ctx, c->local_regs[i]),
                                           MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                          (MIR_disp_t)(i * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1)));
        }

        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->local_regs[local_idx]),
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)((int)local_idx * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1)));
        if (flat_append_done) MIR_append_insn(c->ctx, c->jit_func, flat_append_done);
        if (c->known_func_locals) c->known_func_locals[local_idx] = NULL;
        if (c->known_type_locals && c->known_type_locals[local_idx] != SV_TI_NUM)
          c->known_type_locals[local_idx] = SV_TI_UNKNOWN;
      }

      mir_emit_bailout_check(c->ctx, c->jit_func, c->r_err_tmp,
                             0, c->bc_off, pre_op_sp, &c->bailout_ctx);

      jit_emit_throw_if_error(c, c->r_err_tmp);
      if ((int)slot_idx >= c->param_count) {
        int gli = (int)slot_idx - c->param_count;
        if (gli < c->n_locals && c->local_d_regs && c->known_type_locals && c->known_type_locals[gli] == SV_TI_NUM)
          mir_emit_numeric_local_store_mirror(c->ctx, c->jit_func,
                                              c->local_d_regs[gli], c->local_regs[gli], 0, false,
                                              c->r_bool, c->bc_off + c->sz, c->vs.sp, &c->bailout_ctx);
      }
      break;
    }

    case OP_TO_STRING: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t src = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      MIR_label_t ts_is_str = MIR_new_label(c->ctx);
      MIR_label_t ts_done = MIR_new_label(c->ctx);
      MIR_label_t ts_helper = MIR_new_label(c->ctx);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_UBLE,
                                   MIR_new_label_op(c->ctx, ts_helper),
                                   MIR_new_reg_op(c->ctx, src),
                                   MIR_new_uint_op(c->ctx, NANBOX_PREFIX)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_URSH,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, src),
                                   MIR_new_uint_op(c->ctx, NANBOX_TYPE_SHIFT)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_AND,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx, NANBOX_TYPE_MASK)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, ts_is_str),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_int_op(c->ctx, kTypeString)));
      MIR_append_insn(c->ctx, c->jit_func, ts_helper);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 5,
                                        MIR_new_ref_op(c->ctx, c->to_string_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_to_string),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, src)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, ts_done)));
      MIR_append_insn(c->ctx, c->jit_func, ts_is_str);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, dst),
                                   MIR_new_reg_op(c->ctx, src)));
      MIR_append_insn(c->ctx, c->jit_func, ts_done);
      jit_emit_throw_if_error(c, dst);
      break;
    }

    case OP_TO_STRING_DEFER_NUMBER: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t src = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      MIR_label_t td_done = MIR_new_label(c->ctx);
      MIR_label_t td_helper = MIR_new_label(c->ctx);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, dst),
                                   MIR_new_reg_op(c->ctx, src)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_UBLE,
                                   MIR_new_label_op(c->ctx, td_done),
                                   MIR_new_reg_op(c->ctx, src),
                                   MIR_new_uint_op(c->ctx, NANBOX_PREFIX)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_URSH,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, src),
                                   MIR_new_uint_op(c->ctx, NANBOX_TYPE_SHIFT)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_AND,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx, NANBOX_TYPE_MASK)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BNE,
                                   MIR_new_label_op(c->ctx, td_helper),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_int_op(c->ctx, kTypeString)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, td_done)));
      MIR_append_insn(c->ctx, c->jit_func, td_helper);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 5,
                                        MIR_new_ref_op(c->ctx, c->to_string_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_to_string),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, src)));
      MIR_append_insn(c->ctx, c->jit_func, td_done);
      jit_emit_throw_if_error(c, dst);
      break;
    }

    case OP_STR_FLUSH_LOCAL: {
      uint16_t slot_idx = sv_get_u16(c->ip + 1);
      int pre_op_sp = c->vs.sp;
      if ((int)slot_idx < c->param_count) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_call_insn(c->ctx, 10,
                                          MIR_new_ref_op(c->ctx, c->str_flush_local_proto),
                                          MIR_new_ref_op(c->ctx, c->imp_str_flush_local),
                                          MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                          MIR_new_reg_op(c->ctx, c->r_vm),
                                          MIR_new_reg_op(c->ctx, c->r_js),
                                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                          c->writes_params ? MIR_new_reg_op(c->ctx, c->r_slotbuf) : MIR_new_reg_op(c->ctx, c->r_args),
                                          c->writes_params ? MIR_new_int_op(c->ctx, c->param_count) : MIR_new_reg_op(c->ctx, c->r_argc),
                                          MIR_new_uint_op(c->ctx, 0),
                                          MIR_new_int_op(c->ctx, (int64_t)slot_idx)));
      } else {
        uint16_t local_idx = (uint16_t)(slot_idx - (uint16_t)c->param_count);
        if (local_idx >= (uint16_t)c->n_locals) {
          c->ok = false;
          break;
        }

        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)((int)local_idx * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1),
                                     MIR_new_reg_op(c->ctx, c->local_regs[local_idx])));

        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_call_insn(c->ctx, 10,
                                          MIR_new_ref_op(c->ctx, c->str_flush_local_proto),
                                          MIR_new_ref_op(c->ctx, c->imp_str_flush_local),
                                          MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                          MIR_new_reg_op(c->ctx, c->r_vm),
                                          MIR_new_reg_op(c->ctx, c->r_js),
                                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                          MIR_new_uint_op(c->ctx, 0),
                                          MIR_new_int_op(c->ctx, (int64_t)c->param_count),
                                          MIR_new_reg_op(c->ctx, c->r_lbuf),
                                          MIR_new_int_op(c->ctx, (int64_t)slot_idx)));

        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->local_regs[local_idx]),
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)((int)local_idx * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1)));
        if (c->known_func_locals) c->known_func_locals[local_idx] = NULL;
        if (c->known_type_locals && c->known_type_locals[local_idx] != SV_TI_NUM)
          c->known_type_locals[local_idx] = SV_TI_UNKNOWN;
      }

      mir_emit_bailout_check(c->ctx, c->jit_func, c->r_err_tmp,
                             0, c->bc_off, pre_op_sp, &c->bailout_ctx);

      jit_emit_throw_if_error(c, c->r_err_tmp);
      if ((int)slot_idx >= c->param_count) {
        int gli = (int)slot_idx - c->param_count;
        if (gli < c->n_locals && c->local_d_regs && c->known_type_locals && c->known_type_locals[gli] == SV_TI_NUM)
          mir_emit_numeric_local_store_mirror(c->ctx, c->jit_func,
                                              c->local_d_regs[gli], c->local_regs[gli], 0, false,
                                              c->r_bool, c->bc_off + c->sz, c->vs.sp, &c->bailout_ctx);
      }
      break;
    }

    default:
      __builtin_unreachable();
  }
}
