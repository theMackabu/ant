#include "compile.h"
#include "silver/feedback.h"

void jit_emit_locals(jit_compile_t *c) {
  switch (c->op) {
    case OP_DEC_LOCAL: {
      uint8_t idx = sv_get_u8(c->ip + 1);
      if (idx >= c->n_locals) {
        c->ok = false;
        break;
      }
      if (c->known_func_locals) c->known_func_locals[idx] = NULL;
      bool loc_is_num = c->known_type_locals && c->known_type_locals[idx] == SV_TI_NUM;
      int dn = c->arith_n++;
      char dl_d1[32], dl_d2[32];
      snprintf(dl_d1, sizeof(dl_d1), "dl_d1_%d", dn);
      snprintf(dl_d2, sizeof(dl_d2), "dl_d2_%d", dn);
      MIR_reg_t fd1 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, dl_d1);
      MIR_reg_t fd2 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, dl_d2);
      if (loc_is_num && c->local_d_regs)
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DMOV,
                                     MIR_new_reg_op(c->ctx, fd1),
                                     MIR_new_reg_op(c->ctx, c->local_d_regs[idx])));
      else
        mir_i64_to_d(c->ctx, c->jit_func, fd1, c->local_regs[idx], c->r_d_slot);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_DSUB,
                                   MIR_new_reg_op(c->ctx, fd2),
                                   MIR_new_reg_op(c->ctx, fd1),
                                   MIR_new_reg_op(c->ctx, c->r_d_one)));
      if (c->local_d_regs)
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DMOV,
                                     MIR_new_reg_op(c->ctx, c->local_d_regs[idx]),
                                     MIR_new_reg_op(c->ctx, fd2)));
      if (!(c->dnum_locals && c->dnum_locals[idx]))
        mir_d_to_i64(c->ctx, c->jit_func, c->local_regs[idx], fd2, c->r_d_slot);
      if (c->has_captures && c->captured_locals && c->captured_locals[idx])
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1),
                                     MIR_new_reg_op(c->ctx, c->local_regs[idx])));
      break;
    }

    case OP_ADD_LOCAL: {
      uint8_t idx = sv_get_u8(c->ip + 1);
      if (idx >= c->n_locals) {
        c->ok = false;
        break;
      }
      if (c->known_func_locals) c->known_func_locals[idx] = NULL;

      uint8_t fb = sv_func_type_feedback(c->func)
                       ? sv_func_type_feedback(c->func)[c->bc_off]
                       : 0;
      bool fb_num_only = fb && !(fb & ~SV_TFB_NUM);
      bool loc_is_num = c->known_type_locals && c->local_d_regs &&
                        c->known_type_locals[idx] == SV_TI_NUM &&
                        (!c->captured_locals || !c->captured_locals[idx]);
      bool rhs_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);

      if (fb_num_only && loc_is_num) {
        int rhs_idx = c->vs.sp - 1;
        int pre_op_sp = c->vs.sp;
        MIR_reg_t rr = c->vs.regs[rhs_idx];
        MIR_reg_t rr_d = c->vs.d_regs[rhs_idx];
        MIR_label_t bail_direct = MIR_new_label(c->ctx);
        MIR_label_t done = MIR_new_label(c->ctx);

        if (!rhs_is_num) {
          mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, bail_direct);
          mir_i64_to_d(c->ctx, c->jit_func, rr_d, rr, c->r_d_slot);
        }

        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DADD,
                                     MIR_new_reg_op(c->ctx, c->local_d_regs[idx]),
                                     MIR_new_reg_op(c->ctx, c->local_d_regs[idx]),
                                     MIR_new_reg_op(c->ctx, rr_d)));
        if (!(c->dnum_locals && c->dnum_locals[idx]))
          mir_d_to_i64(c->ctx, c->jit_func,
                       c->local_regs[idx], c->local_d_regs[idx], c->r_d_slot);
        if (c->has_captures && c->captured_locals && c->captured_locals[idx])
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_MOV,
                                       MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                      (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)),
                                                      c->r_lbuf, 0, 1),
                                       MIR_new_reg_op(c->ctx, c->local_regs[idx])));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));

        MIR_append_insn(c->ctx, c->jit_func, bail_direct);
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, pre_op_sp,
                                    &c->bailout_ctx, -1, false, rhs_idx, rhs_is_num);
        MIR_append_insn(c->ctx, c->jit_func, done);
        vstack_pop(&c->vs);
        break;
      }

      int pre_op_sp = c->vs.sp;
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t rr = vstack_pop(&c->vs);

      MIR_label_t slow = MIR_new_label(c->ctx);
      MIR_label_t done = MIR_new_label(c->ctx);

      if (c->dnum_locals && c->dnum_locals[idx])
        mir_d_to_i64(c->ctx, c->jit_func,
                     c->local_regs[idx], c->local_d_regs[idx], c->r_d_slot);
      mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, c->local_regs[idx], slow);
      mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, slow);

      int an = c->arith_n++;
      char al_d1[32], al_d2[32], al_d3[32];
      snprintf(al_d1, sizeof(al_d1), "al_d1_%d", an);
      snprintf(al_d2, sizeof(al_d2), "al_d2_%d", an);
      snprintf(al_d3, sizeof(al_d3), "al_d3_%d", an);
      MIR_reg_t fd1 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, al_d1);
      MIR_reg_t fd2 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, al_d2);
      MIR_reg_t fd3 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, al_d3);
      mir_i64_to_d(c->ctx, c->jit_func, fd1, c->local_regs[idx], c->r_d_slot);
      mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_DADD,
                                   MIR_new_reg_op(c->ctx, fd3),
                                   MIR_new_reg_op(c->ctx, fd1),
                                   MIR_new_reg_op(c->ctx, fd2)));
      if (c->local_d_regs)
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DMOV,
                                     MIR_new_reg_op(c->ctx, c->local_d_regs[idx]),
                                     MIR_new_reg_op(c->ctx, fd3)));
      mir_d_to_i64(c->ctx, c->jit_func, c->local_regs[idx], fd3, c->r_d_slot);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));

      MIR_append_insn(c->ctx, c->jit_func, slow);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                   MIR_new_reg_op(c->ctx, c->local_regs[idx])));
      mir_call_helper2(c->ctx, c->jit_func, c->local_regs[idx],
                       c->helper2_proto, c->imp_add,
                       c->r_vm, c->r_js, c->local_regs[idx], rr);
      mir_emit_bailout_check(c->ctx, c->jit_func, c->local_regs[idx],
                             c->r_bailout_val, c->bc_off, pre_op_sp, &c->bailout_ctx);

      MIR_append_insn(c->ctx, c->jit_func, done);
      if (c->has_captures && c->captured_locals && c->captured_locals[idx])
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1),
                                     MIR_new_reg_op(c->ctx, c->local_regs[idx])));
      break;
    }

    case OP_GET_ARG: {
      uint16_t idx = sv_get_u16(c->ip + 1);
      MIR_reg_t dst = vstack_push(&c->vs);
      if (idx < JIT_PARAM_HOIST_CAP && c->param_cache[idx]) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, dst),
                                     MIR_new_reg_op(c->ctx, c->param_cache[idx])));
      } else if (idx < (uint16_t)c->param_count && (c->writes_params || (c->has_captured_params && c->captured_params && c->captured_params[idx]))) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, dst),
                                     MIR_new_mem_op(c->ctx, MIR_JSVAL,
                                                    (MIR_disp_t)(idx * (int)sizeof(ant_value_t)),
                                                    c->r_slotbuf, 0, 1)));
      } else {
        MIR_label_t arg_in_range = MIR_new_label(c->ctx);
        MIR_label_t arg_done = MIR_new_label(c->ctx);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_UBGT,
                                     MIR_new_label_op(c->ctx, arg_in_range),
                                     MIR_new_reg_op(c->ctx, c->r_argc),
                                     MIR_new_int_op(c->ctx, (int64_t)idx)));
        mir_load_imm(c->ctx, c->jit_func, dst, mkval(kTypeUndefined, 0));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, arg_done)));
        MIR_append_insn(c->ctx, c->jit_func, arg_in_range);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, dst),
                                     MIR_new_mem_op(c->ctx, MIR_JSVAL,
                                                    (MIR_disp_t)(idx * (int)sizeof(ant_value_t)),
                                                    c->r_args, 0, 1)));
        MIR_append_insn(c->ctx, c->jit_func, arg_done);
      }
      if (!c->builder_target_slots || ((int)idx < c->param_count && c->builder_target_slots[idx])) {
        MIR_label_t sbr_done = mir_emit_string_builder_read_open(
            c->ctx, c->jit_func, dst, c->r_bool,
            c->r_vm, c->r_js, c->helper1_proto, c->imp_str_read_value);
        jit_emit_throw_if_error(c, dst);
        MIR_append_insn(c->ctx, c->jit_func, sbr_done);
      }
      break;
    }

    case OP_PUT_ARG:
    case OP_SET_ARG: {
      uint16_t idx = sv_get_u16(c->ip + 1);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t val = vstack_top(&c->vs);
      if (idx < (uint16_t)c->param_count && (c->writes_params || (c->has_captured_params && c->captured_params && c->captured_params[idx]))) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_JSVAL,
                                                    (MIR_disp_t)(idx * (int)sizeof(ant_value_t)),
                                                    c->r_slotbuf, 0, 1),
                                     MIR_new_reg_op(c->ctx, val)));
        if (!c->writes_params && idx < (uint16_t)c->param_count) {
          MIR_label_t arg_in_range = MIR_new_label(c->ctx);
          MIR_label_t arg_done = MIR_new_label(c->ctx);
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_UBGT,
                                       MIR_new_label_op(c->ctx, arg_in_range),
                                       MIR_new_reg_op(c->ctx, c->r_argc),
                                       MIR_new_int_op(c->ctx, (int64_t)idx)));
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, arg_done)));
          MIR_append_insn(c->ctx, c->jit_func, arg_in_range);
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_MOV,
                                       MIR_new_mem_op(c->ctx, MIR_JSVAL,
                                                      (MIR_disp_t)(idx * (int)sizeof(ant_value_t)),
                                                      c->r_args, 0, 1),
                                       MIR_new_reg_op(c->ctx, val)));
          MIR_append_insn(c->ctx, c->jit_func, arg_done);
        }
      } else {
        MIR_label_t arg_in_range = MIR_new_label(c->ctx);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_UBGT,
                                     MIR_new_label_op(c->ctx, arg_in_range),
                                     MIR_new_reg_op(c->ctx, c->r_argc),
                                     MIR_new_int_op(c->ctx, (int64_t)idx)));
        mir_load_imm(c->ctx, c->jit_func, c->r_bailout_val, (uint64_t)SV_JIT_BAILOUT);
        mir_emit_bailout_check(c->ctx, c->jit_func, c->r_bailout_val,
                               0, c->bc_off, c->vs.sp, &c->bailout_ctx);
        MIR_append_insn(c->ctx, c->jit_func, arg_in_range);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_JSVAL,
                                                    (MIR_disp_t)(idx * (int)sizeof(ant_value_t)),
                                                    c->r_args, 0, 1),
                                     MIR_new_reg_op(c->ctx, val)));
      }
      if (c->op == OP_PUT_ARG) (void)vstack_pop(&c->vs);
      break;
    }

    case OP_REST: {
      uint16_t start = sv_get_u16(c->ip + 1);
      MIR_reg_t dst = vstack_push(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 8,
                                        MIR_new_ref_op(c->ctx, c->rest_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_rest),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, c->r_args),
                                        MIR_new_reg_op(c->ctx, c->r_argc),
                                        MIR_new_int_op(c->ctx, (int64_t)start)));
      break;
    }

    case OP_GET_LOCAL: {
      uint16_t idx = sv_get_u16(c->ip + 1);
      if (idx >= (uint16_t)c->n_locals) {
        c->ok = false;
        break;
      }
      if (c->integer_locals && c->integer_locals[idx]) {
        MIR_reg_t dst = vstack_push(&c->vs);
        MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, dst), MIR_new_reg_op(c->ctx, c->integer_locals[idx])));
        c->vs.slot_type[c->vs.sp - 1] = SLOT_I32;
        c->vs.integer_range[c->vs.sp - 1] = c->integer_local_ranges[idx];
        break;
      }
      if (c->has_captures && c->captured_locals && c->captured_locals[idx])
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->local_regs[idx]),
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1)));
      MIR_reg_t dst = vstack_push(&c->vs);
      if (c->known_func_locals) c->vs.known_func[c->vs.sp - 1] = c->known_func_locals[idx];
      if (!(c->dnum_locals && c->dnum_locals[idx]))
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, dst),
                                     MIR_new_reg_op(c->ctx, c->local_regs[idx])));
      if ((!c->builder_target_slots || c->builder_target_slots[c->param_count + idx]) && (!c->known_type_locals || c->known_type_locals[idx] != SV_TI_NUM)) {
        MIR_label_t sbr_done = mir_emit_string_builder_read_open(
            c->ctx, c->jit_func, dst, c->r_bool,
            c->r_vm, c->r_js, c->helper1_proto, c->imp_str_read_value);
        jit_emit_throw_if_error(c, dst);
        MIR_append_insn(c->ctx, c->jit_func, sbr_done);
      }
      if (c->known_type_locals && c->known_type_locals[idx] == SV_TI_NUM) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DMOV,
                                     MIR_new_reg_op(c->ctx, c->vs.d_regs[c->vs.sp - 1]),
                                     MIR_new_reg_op(c->ctx, c->local_d_regs[idx])));
        if (c->vs.slot_type) c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
      }
      break;
    }
    case OP_GET_LOCAL8: {
      uint8_t idx = sv_get_u8(c->ip + 1);
      if (idx >= c->n_locals) {
        c->ok = false;
        break;
      }
      if (c->integer_locals && c->integer_locals[idx]) {
        MIR_reg_t dst = vstack_push(&c->vs);
        MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, dst), MIR_new_reg_op(c->ctx, c->integer_locals[idx])));
        c->vs.slot_type[c->vs.sp - 1] = SLOT_I32;
        c->vs.integer_range[c->vs.sp - 1] = c->integer_local_ranges[idx];
        break;
      }
      if (c->has_captures && c->captured_locals && c->captured_locals[idx])
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->local_regs[idx]),
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1)));
      MIR_reg_t dst = vstack_push(&c->vs);
      if (c->known_func_locals) c->vs.known_func[c->vs.sp - 1] = c->known_func_locals[idx];
      if (!(c->dnum_locals && c->dnum_locals[idx]))
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, dst),
                                     MIR_new_reg_op(c->ctx, c->local_regs[idx])));
      if ((!c->builder_target_slots || c->builder_target_slots[c->param_count + idx]) && (!c->known_type_locals || c->known_type_locals[idx] != SV_TI_NUM)) {
        MIR_label_t sbr_done = mir_emit_string_builder_read_open(
            c->ctx, c->jit_func, dst, c->r_bool,
            c->r_vm, c->r_js, c->helper1_proto, c->imp_str_read_value);
        jit_emit_throw_if_error(c, dst);
        MIR_append_insn(c->ctx, c->jit_func, sbr_done);
      }
      if (c->known_type_locals && c->known_type_locals[idx] == SV_TI_NUM) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DMOV,
                                     MIR_new_reg_op(c->ctx, c->vs.d_regs[c->vs.sp - 1]),
                                     MIR_new_reg_op(c->ctx, c->local_d_regs[idx])));
        if (c->vs.slot_type) c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
      }
      break;
    }

    case OP_GET_SLOT_RAW: {
      uint16_t slot_idx = sv_get_u16(c->ip + 1);
      if ((int)slot_idx < c->param_count) {
        uint16_t idx = slot_idx;
        MIR_reg_t dst = vstack_push(&c->vs);
        if (idx < (uint16_t)c->param_count && (c->writes_params || (c->has_captured_params && c->captured_params && c->captured_params[idx]))) {
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_MOV,
                                       MIR_new_reg_op(c->ctx, dst),
                                       MIR_new_mem_op(c->ctx, MIR_JSVAL,
                                                      (MIR_disp_t)(idx * (int)sizeof(ant_value_t)),
                                                      c->r_slotbuf, 0, 1)));
        } else {
          MIR_label_t arg_in_range = MIR_new_label(c->ctx);
          MIR_label_t arg_done = MIR_new_label(c->ctx);
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_UBGT,
                                       MIR_new_label_op(c->ctx, arg_in_range),
                                       MIR_new_reg_op(c->ctx, c->r_argc),
                                       MIR_new_int_op(c->ctx, (int64_t)idx)));
          mir_load_imm(c->ctx, c->jit_func, dst, mkval(kTypeUndefined, 0));
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, arg_done)));
          MIR_append_insn(c->ctx, c->jit_func, arg_in_range);
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_MOV,
                                       MIR_new_reg_op(c->ctx, dst),
                                       MIR_new_mem_op(c->ctx, MIR_JSVAL,
                                                      (MIR_disp_t)(idx * (int)sizeof(ant_value_t)),
                                                      c->r_args, 0, 1)));
          MIR_append_insn(c->ctx, c->jit_func, arg_done);
        }
      } else {
        uint16_t idx = (uint16_t)(slot_idx - (uint16_t)c->param_count);
        if (idx >= (uint16_t)c->n_locals) {
          c->ok = false;
          break;
        }
        if (c->has_captures && c->captured_locals && c->captured_locals[idx])
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_MOV,
                                       MIR_new_reg_op(c->ctx, c->local_regs[idx]),
                                       MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                      (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1)));
        MIR_reg_t dst = vstack_push(&c->vs);
        if (c->known_func_locals) c->vs.known_func[c->vs.sp - 1] = c->known_func_locals[idx];
        if (!(c->dnum_locals && c->dnum_locals[idx]))
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_MOV,
                                       MIR_new_reg_op(c->ctx, dst),
                                       MIR_new_reg_op(c->ctx, c->local_regs[idx])));
        if (c->known_type_locals && c->known_type_locals[idx] == SV_TI_NUM) {
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_DMOV,
                                       MIR_new_reg_op(c->ctx, c->vs.d_regs[c->vs.sp - 1]),
                                       MIR_new_reg_op(c->ctx, c->local_d_regs[idx])));
          if (c->vs.slot_type) c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
        }
      }
      break;
    }

    case OP_PUT_LOCAL: {
      uint16_t idx = sv_get_u16(c->ip + 1);
      if (idx >= (uint16_t)c->n_locals) {
        c->ok = false;
        break;
      }
      sv_func_t *kf = c->vs.known_func[c->vs.sp - 1];
      bool src_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t src_d = src_is_num ? c->vs.d_regs[c->vs.sp - 1] : 0;
      if (c->dnum_locals && c->dnum_locals[idx]) {
        if (!src_is_num)
          vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
        MIR_reg_t dsrc = vstack_pop(&c->vs);
        if (c->known_func_locals) c->known_func_locals[idx] = kf;
        mir_emit_numeric_local_store_mirror(c->ctx, c->jit_func,
                                            c->local_d_regs[idx], dsrc, src_d, src_is_num,
                                            c->r_bool, c->bc_off, c->vs.sp + 1, &c->bailout_ctx);
        break;
      }
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t src = vstack_pop(&c->vs);
      if (c->known_func_locals) c->known_func_locals[idx] = kf;
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->local_regs[idx]),
                                   MIR_new_reg_op(c->ctx, src)));
      if (c->local_d_regs && c->known_type_locals && c->known_type_locals[idx] == SV_TI_NUM) {
        mir_emit_numeric_local_store_mirror(c->ctx, c->jit_func,
                                            c->local_d_regs[idx], c->local_regs[idx], src_d, src_is_num,
                                            c->r_bool, c->bc_off + c->sz, c->vs.sp, &c->bailout_ctx);
      }
      if (c->has_captures && c->captured_locals && c->captured_locals[idx])
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1),
                                     MIR_new_reg_op(c->ctx, src)));
      break;
    }
    case OP_PUT_LOCAL8: {
      uint8_t idx = sv_get_u8(c->ip + 1);
      if (idx >= c->n_locals) {
        c->ok = false;
        break;
      }
      sv_func_t *kf = c->vs.known_func[c->vs.sp - 1];
      bool src_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t src_d = src_is_num ? c->vs.d_regs[c->vs.sp - 1] : 0;
      if (c->dnum_locals && c->dnum_locals[idx]) {
        if (!src_is_num)
          vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
        MIR_reg_t dsrc = vstack_pop(&c->vs);
        if (c->known_func_locals) c->known_func_locals[idx] = kf;
        mir_emit_numeric_local_store_mirror(c->ctx, c->jit_func,
                                            c->local_d_regs[idx], dsrc, src_d, src_is_num,
                                            c->r_bool, c->bc_off, c->vs.sp + 1, &c->bailout_ctx);
        break;
      }
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t src = vstack_pop(&c->vs);
      if (c->known_func_locals) c->known_func_locals[idx] = kf;
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->local_regs[idx]),
                                   MIR_new_reg_op(c->ctx, src)));
      if (c->local_d_regs && c->known_type_locals && c->known_type_locals[idx] == SV_TI_NUM) {
        mir_emit_numeric_local_store_mirror(c->ctx, c->jit_func,
                                            c->local_d_regs[idx], c->local_regs[idx], src_d, src_is_num,
                                            c->r_bool, c->bc_off + c->sz, c->vs.sp, &c->bailout_ctx);
      }
      if (c->has_captures && c->captured_locals && c->captured_locals[idx])
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1),
                                     MIR_new_reg_op(c->ctx, src)));
      break;
    }

    case OP_SET_LOCAL: {
      uint16_t idx = sv_get_u16(c->ip + 1);
      if (idx >= (uint16_t)c->n_locals) {
        c->ok = false;
        break;
      }
      bool src_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t src_d = src_is_num ? c->vs.d_regs[c->vs.sp - 1] : 0;
      if (c->dnum_locals && c->dnum_locals[idx]) {
        if (!src_is_num)
          vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
        if (c->known_func_locals) c->known_func_locals[idx] = c->vs.known_func[c->vs.sp - 1];
        mir_emit_numeric_local_store_mirror(c->ctx, c->jit_func,
                                            c->local_d_regs[idx], c->vs.regs[c->vs.sp - 1], src_d, src_is_num,
                                            c->r_bool, c->bc_off, c->vs.sp, &c->bailout_ctx);
        break;
      }
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t src = vstack_top(&c->vs);
      if (c->known_func_locals) c->known_func_locals[idx] = c->vs.known_func[c->vs.sp - 1];
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->local_regs[idx]),
                                   MIR_new_reg_op(c->ctx, src)));
      if (c->local_d_regs && c->known_type_locals && c->known_type_locals[idx] == SV_TI_NUM) {
        mir_emit_numeric_local_store_mirror(c->ctx, c->jit_func,
                                            c->local_d_regs[idx], c->local_regs[idx], src_d, src_is_num,
                                            c->r_bool, c->bc_off + c->sz, c->vs.sp, &c->bailout_ctx);
      }
      if (c->has_captures && c->captured_locals && c->captured_locals[idx])
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1),
                                     MIR_new_reg_op(c->ctx, src)));
      break;
    }
    case OP_SET_LOCAL8: {
      uint8_t idx = sv_get_u8(c->ip + 1);
      if (idx >= c->n_locals) {
        c->ok = false;
        break;
      }
      bool src_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t src_d = src_is_num ? c->vs.d_regs[c->vs.sp - 1] : 0;
      if (c->dnum_locals && c->dnum_locals[idx]) {
        if (!src_is_num)
          vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
        if (c->known_func_locals) c->known_func_locals[idx] = c->vs.known_func[c->vs.sp - 1];
        mir_emit_numeric_local_store_mirror(c->ctx, c->jit_func,
                                            c->local_d_regs[idx], c->vs.regs[c->vs.sp - 1], src_d, src_is_num,
                                            c->r_bool, c->bc_off, c->vs.sp, &c->bailout_ctx);
        break;
      }
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t src = vstack_top(&c->vs);
      if (c->known_func_locals) c->known_func_locals[idx] = c->vs.known_func[c->vs.sp - 1];
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->local_regs[idx]),
                                   MIR_new_reg_op(c->ctx, src)));
      if (c->local_d_regs && c->known_type_locals && c->known_type_locals[idx] == SV_TI_NUM) {
        mir_emit_numeric_local_store_mirror(c->ctx, c->jit_func,
                                            c->local_d_regs[idx], c->local_regs[idx], src_d, src_is_num,
                                            c->r_bool, c->bc_off + c->sz, c->vs.sp, &c->bailout_ctx);
      }
      if (c->has_captures && c->captured_locals && c->captured_locals[idx])
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1),
                                     MIR_new_reg_op(c->ctx, src)));
      break;
    }

    case OP_SET_LOCAL_UNDEF: {
      uint16_t idx = sv_get_u16(c->ip + 1);
      if (idx >= (uint16_t)c->n_locals) {
        c->ok = false;
        break;
      }
      if (c->known_type_locals) {
        bool was_num = c->known_type_locals[idx] == SV_TI_NUM;
        bool immediate_num_init =
            was_num &&
            (!c->captured_locals || !c->captured_locals[idx]) &&
            jit_has_immediate_numeric_local_init(c->func, c->ip + c->sz, c->end, idx);
        c->known_type_locals[idx] = immediate_num_init ? SV_TI_NUM : SV_TI_UNKNOWN;
      }
      break;
    }

    case OP_INC_LOCAL: {
      uint8_t idx = sv_get_u8(c->ip + 1);
      if (idx >= c->n_locals) {
        c->ok = false;
        break;
      }
      if (c->known_func_locals) c->known_func_locals[idx] = NULL;
      bool loc_is_num = c->known_type_locals && c->known_type_locals[idx] == SV_TI_NUM;
      int in = c->arith_n++;
      char il_d1[32], il_d2[32];
      snprintf(il_d1, sizeof(il_d1), "il_d1_%d", in);
      snprintf(il_d2, sizeof(il_d2), "il_d2_%d", in);
      MIR_reg_t fd1 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, il_d1);
      MIR_reg_t fd2 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, il_d2);
      if (loc_is_num && c->local_d_regs)
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DMOV,
                                     MIR_new_reg_op(c->ctx, fd1),
                                     MIR_new_reg_op(c->ctx, c->local_d_regs[idx])));
      else
        mir_i64_to_d(c->ctx, c->jit_func, fd1, c->local_regs[idx], c->r_d_slot);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_DADD,
                                   MIR_new_reg_op(c->ctx, fd2),
                                   MIR_new_reg_op(c->ctx, fd1),
                                   MIR_new_reg_op(c->ctx, c->r_d_one)));
      if (c->local_d_regs)
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DMOV,
                                     MIR_new_reg_op(c->ctx, c->local_d_regs[idx]),
                                     MIR_new_reg_op(c->ctx, fd2)));
      if (!(c->dnum_locals && c->dnum_locals[idx]))
        mir_d_to_i64(c->ctx, c->jit_func, c->local_regs[idx], fd2, c->r_d_slot);
      if (c->has_captures && c->captured_locals && c->captured_locals[idx])
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)((int)idx * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1),
                                     MIR_new_reg_op(c->ctx, c->local_regs[idx])));
      break;
    }

    default:
      __builtin_unreachable();
  }
}
