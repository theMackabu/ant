#include "compile.h"

void jit_emit_arithmetic(jit_compile_t *c) {
  switch (c->op) {
    case OP_ADD:
    case OP_ADD_NUM: {
      if (jit_emit_integer_arithmetic(c->ctx, c->jit_func, &c->vs, c->op)) break;
      uint8_t fb = sv_func_type_feedback(c->func) ? sv_func_type_feedback(c->func)[c->bc_off] : 0;
      bool force_num_only = (c->op == OP_ADD_NUM);
      bool fb_num_only = force_num_only || (fb && !(fb & ~SV_TFB_NUM));
      bool fb_never_num = !force_num_only && fb && !(fb & SV_TFB_NUM);
      bool fb_str_only = !force_num_only && fb && !(fb & ~SV_TFB_STR);

      bool l_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      bool r_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);

      MIR_reg_t rr = vstack_pop(&c->vs);
      MIR_reg_t rl = vstack_pop(&c->vs);
      MIR_reg_t rd = vstack_push(&c->vs);

      if (fb_str_only) {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        MIR_label_t slow = MIR_new_label(c->ctx);
        MIR_label_t done = MIR_new_label(c->ctx);
        mir_emit_string_concat_fastpath(c->ctx, c->jit_func, c->r_js, rl, rr, rd, slow, -1, c->bc_off, false);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                     MIR_new_reg_op(c->ctx, rl)));
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_add,
                         c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        MIR_append_insn(c->ctx, c->jit_func, done);
      } else if (fb_never_num) {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                     MIR_new_reg_op(c->ctx, rl)));
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_add,
                         c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
      } else if (fb_num_only && l_is_num && r_is_num) {
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_reg_t fd_dst = c->vs.d_regs[c->vs.sp - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DADD,
                                     MIR_new_reg_op(c->ctx, fd_dst),
                                     MIR_new_reg_op(c->ctx, fd_dst),
                                     MIR_new_reg_op(c->ctx, fd_r)));
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
      } else if (fb_num_only && (l_is_num || r_is_num)) {
        MIR_label_t bail_direct = MIR_new_label(c->ctx);
        MIR_reg_t boxed_reg = l_is_num ? rr : rl;
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, boxed_reg, bail_direct);
        int boxed_idx = l_is_num ? (int)c->vs.sp : (int)(c->vs.sp - 1);
        mir_i64_to_d(c->ctx, c->jit_func, c->vs.d_regs[boxed_idx],
                     c->vs.regs[boxed_idx], c->r_d_slot);
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_reg_t fd_dst = c->vs.d_regs[c->vs.sp - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DADD,
                                     MIR_new_reg_op(c->ctx, fd_dst),
                                     MIR_new_reg_op(c->ctx, fd_dst),
                                     MIR_new_reg_op(c->ctx, fd_r)));
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
        MIR_label_t skip_bail = MIR_new_label(c->ctx);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, skip_bail)));
        MIR_append_insn(c->ctx, c->jit_func, bail_direct);
        int pre_op_sp = c->vs.sp + 1;
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, pre_op_sp,
                                    &c->bailout_ctx, c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        MIR_append_insn(c->ctx, c->jit_func, skip_bail);
      } else if (fb_num_only) {
        MIR_label_t bail_direct = MIR_new_label(c->ctx);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rl, bail_direct);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, bail_direct);
        int an = c->arith_n++;
        char d1[32], d2[32], d3[32];
        snprintf(d1, sizeof(d1), "add_d1_%d", an);
        snprintf(d2, sizeof(d2), "add_d2_%d", an);
        snprintf(d3, sizeof(d3), "add_d3_%d", an);
        MIR_reg_t fd1 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d1);
        MIR_reg_t fd2 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d2);
        MIR_reg_t fd3 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d3);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DADD,
                                     MIR_new_reg_op(c->ctx, fd3),
                                     MIR_new_reg_op(c->ctx, fd1),
                                     MIR_new_reg_op(c->ctx, fd2)));
        mir_d_to_i64(c->ctx, c->jit_func, rd, fd3, c->r_d_slot);
        MIR_label_t skip_bail = MIR_new_label(c->ctx);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, skip_bail)));
        MIR_append_insn(c->ctx, c->jit_func, bail_direct);
        int pre_op_sp = c->vs.sp + 1;
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, pre_op_sp,
                                    &c->bailout_ctx, c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        MIR_append_insn(c->ctx, c->jit_func, skip_bail);
      } else {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        MIR_label_t slow = MIR_new_label(c->ctx);
        MIR_label_t done = MIR_new_label(c->ctx);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rl, slow);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, slow);
        int an = c->arith_n++;
        char d1[32], d2[32], d3[32];
        snprintf(d1, sizeof(d1), "add_d1_%d", an);
        snprintf(d2, sizeof(d2), "add_d2_%d", an);
        snprintf(d3, sizeof(d3), "add_d3_%d", an);
        MIR_reg_t fd1 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d1);
        MIR_reg_t fd2 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d2);
        MIR_reg_t fd3 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d3);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DADD,
                                     MIR_new_reg_op(c->ctx, fd3),
                                     MIR_new_reg_op(c->ctx, fd1),
                                     MIR_new_reg_op(c->ctx, fd2)));
        mir_d_to_i64(c->ctx, c->jit_func, rd, fd3, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                     MIR_new_reg_op(c->ctx, rl)));
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_add,
                         c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        MIR_append_insn(c->ctx, c->jit_func, done);
      }
      break;
    }

    case OP_SUB:
    case OP_SUB_NUM: {
      if (jit_emit_integer_arithmetic(c->ctx, c->jit_func, &c->vs, c->op)) break;
      uint8_t fb = sv_func_type_feedback(c->func) ? sv_func_type_feedback(c->func)[c->bc_off] : 0;
      bool force_num_only = (c->op == OP_SUB_NUM);
      bool fb_num_only = force_num_only || (fb && !(fb & ~SV_TFB_NUM));
      bool fb_never_num = !force_num_only && fb && !(fb & SV_TFB_NUM);

      bool l_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      bool r_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);

      MIR_reg_t rr = vstack_pop(&c->vs);
      MIR_reg_t rl = vstack_pop(&c->vs);
      MIR_reg_t rd = vstack_push(&c->vs);

      if (fb_never_num) {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                     MIR_new_reg_op(c->ctx, rl)));
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_sub,
                         c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
      } else if (fb_num_only && l_is_num && r_is_num) {
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_reg_t fd_dst = c->vs.d_regs[c->vs.sp - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DSUB,
                                     MIR_new_reg_op(c->ctx, fd_dst),
                                     MIR_new_reg_op(c->ctx, fd_dst),
                                     MIR_new_reg_op(c->ctx, fd_r)));
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
      } else if (fb_num_only && (l_is_num || r_is_num)) {
        MIR_label_t bail_direct = MIR_new_label(c->ctx);
        MIR_reg_t boxed_reg = l_is_num ? rr : rl;
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, boxed_reg, bail_direct);
        int boxed_idx = l_is_num ? (int)c->vs.sp : (int)(c->vs.sp - 1);
        mir_i64_to_d(c->ctx, c->jit_func, c->vs.d_regs[boxed_idx],
                     c->vs.regs[boxed_idx], c->r_d_slot);
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_reg_t fd_dst = c->vs.d_regs[c->vs.sp - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DSUB,
                                     MIR_new_reg_op(c->ctx, fd_dst),
                                     MIR_new_reg_op(c->ctx, fd_dst),
                                     MIR_new_reg_op(c->ctx, fd_r)));
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
        MIR_label_t skip_bail = MIR_new_label(c->ctx);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, skip_bail)));
        MIR_append_insn(c->ctx, c->jit_func, bail_direct);
        int pre_op_sp = c->vs.sp + 1;
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, pre_op_sp,
                                    &c->bailout_ctx, c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        MIR_append_insn(c->ctx, c->jit_func, skip_bail);
      } else if (fb_num_only) {
        MIR_label_t bail_direct = MIR_new_label(c->ctx);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rl, bail_direct);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, bail_direct);
        int sn = c->arith_n++;
        char d1[32], d2[32], d3[32];
        snprintf(d1, sizeof(d1), "sub_d1_%d", sn);
        snprintf(d2, sizeof(d2), "sub_d2_%d", sn);
        snprintf(d3, sizeof(d3), "sub_d3_%d", sn);
        MIR_reg_t fd1 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d1);
        MIR_reg_t fd2 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d2);
        MIR_reg_t fd3 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d3);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DSUB,
                                     MIR_new_reg_op(c->ctx, fd3),
                                     MIR_new_reg_op(c->ctx, fd1),
                                     MIR_new_reg_op(c->ctx, fd2)));
        mir_d_to_i64(c->ctx, c->jit_func, rd, fd3, c->r_d_slot);
        MIR_label_t skip_bail = MIR_new_label(c->ctx);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, skip_bail)));
        MIR_append_insn(c->ctx, c->jit_func, bail_direct);
        int pre_op_sp = c->vs.sp + 1;
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, pre_op_sp,
                                    &c->bailout_ctx, c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        MIR_append_insn(c->ctx, c->jit_func, skip_bail);
      } else {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        MIR_label_t slow = MIR_new_label(c->ctx);
        MIR_label_t done = MIR_new_label(c->ctx);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rl, slow);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, slow);
        int sn = c->arith_n++;
        char d1[32], d2[32], d3[32];
        snprintf(d1, sizeof(d1), "sub_d1_%d", sn);
        snprintf(d2, sizeof(d2), "sub_d2_%d", sn);
        snprintf(d3, sizeof(d3), "sub_d3_%d", sn);
        MIR_reg_t fd1 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d1);
        MIR_reg_t fd2 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d2);
        MIR_reg_t fd3 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d3);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DSUB,
                                     MIR_new_reg_op(c->ctx, fd3),
                                     MIR_new_reg_op(c->ctx, fd1),
                                     MIR_new_reg_op(c->ctx, fd2)));
        mir_d_to_i64(c->ctx, c->jit_func, rd, fd3, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                     MIR_new_reg_op(c->ctx, rl)));
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_sub, c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        MIR_append_insn(c->ctx, c->jit_func, done);
      }
      break;
    }

    case OP_MUL:
    case OP_MUL_NUM: {
      if (jit_emit_integer_arithmetic(c->ctx, c->jit_func, &c->vs, c->op)) break;
      uint8_t fb = sv_func_type_feedback(c->func) ? sv_func_type_feedback(c->func)[c->bc_off] : 0;
      bool force_num_only = (c->op == OP_MUL_NUM);
      bool fb_num_only = force_num_only || (fb && !(fb & ~SV_TFB_NUM));
      bool fb_never_num = !force_num_only && fb && !(fb & SV_TFB_NUM);

      bool l_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      bool r_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);

      MIR_reg_t rr = vstack_pop(&c->vs);
      MIR_reg_t rl = vstack_pop(&c->vs);
      MIR_reg_t rd = vstack_push(&c->vs);

      if (fb_never_num) {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                     MIR_new_reg_op(c->ctx, rl)));
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_mul,
                         c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
      } else if (fb_num_only && l_is_num && r_is_num) {
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_reg_t fd_dst = c->vs.d_regs[c->vs.sp - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DMUL,
                                     MIR_new_reg_op(c->ctx, fd_dst),
                                     MIR_new_reg_op(c->ctx, fd_dst),
                                     MIR_new_reg_op(c->ctx, fd_r)));
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
      } else if (fb_num_only && (l_is_num || r_is_num)) {
        MIR_label_t bail_direct = MIR_new_label(c->ctx);
        MIR_reg_t boxed_reg = l_is_num ? rr : rl;
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, boxed_reg, bail_direct);
        int boxed_idx = l_is_num ? (int)c->vs.sp : (int)(c->vs.sp - 1);
        mir_i64_to_d(c->ctx, c->jit_func, c->vs.d_regs[boxed_idx],
                     c->vs.regs[boxed_idx], c->r_d_slot);
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_reg_t fd_dst = c->vs.d_regs[c->vs.sp - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DMUL,
                                     MIR_new_reg_op(c->ctx, fd_dst),
                                     MIR_new_reg_op(c->ctx, fd_dst),
                                     MIR_new_reg_op(c->ctx, fd_r)));
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
        MIR_label_t skip_bail = MIR_new_label(c->ctx);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, skip_bail)));
        MIR_append_insn(c->ctx, c->jit_func, bail_direct);
        int pre_op_sp = c->vs.sp + 1;
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, pre_op_sp,
                                    &c->bailout_ctx, c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        MIR_append_insn(c->ctx, c->jit_func, skip_bail);
      } else if (fb_num_only) {
        MIR_label_t bail_direct = MIR_new_label(c->ctx);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rl, bail_direct);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, bail_direct);
        int mn = c->arith_n++;
        char d1[32], d2[32], d3[32];
        snprintf(d1, sizeof(d1), "mul_d1_%d", mn);
        snprintf(d2, sizeof(d2), "mul_d2_%d", mn);
        snprintf(d3, sizeof(d3), "mul_d3_%d", mn);
        MIR_reg_t fd1 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d1);
        MIR_reg_t fd2 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d2);
        MIR_reg_t fd3 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d3);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DMUL,
                                     MIR_new_reg_op(c->ctx, fd3),
                                     MIR_new_reg_op(c->ctx, fd1),
                                     MIR_new_reg_op(c->ctx, fd2)));
        mir_d_to_i64(c->ctx, c->jit_func, rd, fd3, c->r_d_slot);
        MIR_label_t skip_bail = MIR_new_label(c->ctx);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, skip_bail)));
        MIR_append_insn(c->ctx, c->jit_func, bail_direct);
        int pre_op_sp = c->vs.sp + 1;
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, pre_op_sp,
                                    &c->bailout_ctx, c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        MIR_append_insn(c->ctx, c->jit_func, skip_bail);
      } else {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        MIR_label_t slow = MIR_new_label(c->ctx);
        MIR_label_t done = MIR_new_label(c->ctx);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rl, slow);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, slow);
        int mn = c->arith_n++;
        char d1[32], d2[32], d3[32];
        snprintf(d1, sizeof(d1), "mul_d1_%d", mn);
        snprintf(d2, sizeof(d2), "mul_d2_%d", mn);
        snprintf(d3, sizeof(d3), "mul_d3_%d", mn);
        MIR_reg_t fd1 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d1);
        MIR_reg_t fd2 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d2);
        MIR_reg_t fd3 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d3);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DMUL,
                                     MIR_new_reg_op(c->ctx, fd3),
                                     MIR_new_reg_op(c->ctx, fd1),
                                     MIR_new_reg_op(c->ctx, fd2)));
        mir_d_to_i64(c->ctx, c->jit_func, rd, fd3, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                     MIR_new_reg_op(c->ctx, rl)));
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_mul, c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        MIR_append_insn(c->ctx, c->jit_func, done);
      }
      break;
    }

    case OP_DIV:
    case OP_DIV_NUM: {
      uint8_t fb = sv_func_type_feedback(c->func) ? sv_func_type_feedback(c->func)[c->bc_off] : 0;
      bool force_num_only = (c->op == OP_DIV_NUM);
      bool fb_num_only = force_num_only || (fb && !(fb & ~SV_TFB_NUM));
      bool fb_never_num = !force_num_only && fb && !(fb & SV_TFB_NUM);

      bool l_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      bool r_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);

      MIR_reg_t rr = vstack_pop(&c->vs);
      MIR_reg_t rl = vstack_pop(&c->vs);
      MIR_reg_t rd = vstack_push(&c->vs);

      if (fb_never_num) {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                     MIR_new_reg_op(c->ctx, rl)));
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_div,
                         c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
      } else if (fb_num_only && l_is_num && r_is_num) {
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_reg_t fd_dst = c->vs.d_regs[c->vs.sp - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DDIV,
                                     MIR_new_reg_op(c->ctx, fd_dst),
                                     MIR_new_reg_op(c->ctx, fd_dst),
                                     MIR_new_reg_op(c->ctx, fd_r)));
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
      } else if (fb_num_only && (l_is_num || r_is_num)) {
        MIR_label_t bail_direct = MIR_new_label(c->ctx);
        MIR_reg_t boxed_reg = l_is_num ? rr : rl;
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, boxed_reg, bail_direct);
        int boxed_idx = l_is_num ? (int)c->vs.sp : (int)(c->vs.sp - 1);
        mir_i64_to_d(c->ctx, c->jit_func, c->vs.d_regs[boxed_idx],
                     c->vs.regs[boxed_idx], c->r_d_slot);
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_reg_t fd_dst = c->vs.d_regs[c->vs.sp - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DDIV,
                                     MIR_new_reg_op(c->ctx, fd_dst),
                                     MIR_new_reg_op(c->ctx, fd_dst),
                                     MIR_new_reg_op(c->ctx, fd_r)));
        c->vs.slot_type[c->vs.sp - 1] = SLOT_NUM;
        MIR_label_t skip_bail = MIR_new_label(c->ctx);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, skip_bail)));
        MIR_append_insn(c->ctx, c->jit_func, bail_direct);
        int pre_op_sp = c->vs.sp + 1;
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, pre_op_sp,
                                    &c->bailout_ctx, c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        MIR_append_insn(c->ctx, c->jit_func, skip_bail);
      } else if (fb_num_only) {
        MIR_label_t bail_direct = MIR_new_label(c->ctx);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rl, bail_direct);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, bail_direct);
        int dn = c->arith_n++;
        char d1[32], d2[32], d3[32];
        snprintf(d1, sizeof(d1), "dv_d1_%d", dn);
        snprintf(d2, sizeof(d2), "dv_d2_%d", dn);
        snprintf(d3, sizeof(d3), "dv_d3_%d", dn);
        MIR_reg_t fd1 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d1);
        MIR_reg_t fd2 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d2);
        MIR_reg_t fd3 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d3);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DDIV,
                                     MIR_new_reg_op(c->ctx, fd3),
                                     MIR_new_reg_op(c->ctx, fd1),
                                     MIR_new_reg_op(c->ctx, fd2)));
        mir_d_to_i64(c->ctx, c->jit_func, rd, fd3, c->r_d_slot);
        MIR_label_t skip_bail = MIR_new_label(c->ctx);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, skip_bail)));
        MIR_append_insn(c->ctx, c->jit_func, bail_direct);
        int pre_op_sp = c->vs.sp + 1;
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, pre_op_sp,
                                    &c->bailout_ctx, c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        MIR_append_insn(c->ctx, c->jit_func, skip_bail);
      } else {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        MIR_label_t slow = MIR_new_label(c->ctx);
        MIR_label_t done = MIR_new_label(c->ctx);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rl, slow);
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, slow);
        int dn = c->arith_n++;
        char d1[32], d2[32], d3[32];
        snprintf(d1, sizeof(d1), "dv_d1_%d", dn);
        snprintf(d2, sizeof(d2), "dv_d2_%d", dn);
        snprintf(d3, sizeof(d3), "dv_d3_%d", dn);
        MIR_reg_t fd1 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d1);
        MIR_reg_t fd2 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d2);
        MIR_reg_t fd3 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d3);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DDIV,
                                     MIR_new_reg_op(c->ctx, fd3),
                                     MIR_new_reg_op(c->ctx, fd1),
                                     MIR_new_reg_op(c->ctx, fd2)));
        mir_d_to_i64(c->ctx, c->jit_func, rd, fd3, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                     MIR_new_reg_op(c->ctx, rl)));
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_div, c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        MIR_append_insn(c->ctx, c->jit_func, done);
      }
      break;
    }

    case OP_MOD: {
      vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t rr = vstack_pop(&c->vs);
      MIR_reg_t rl = vstack_pop(&c->vs);
      MIR_reg_t rd = vstack_push(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                   MIR_new_reg_op(c->ctx, rl)));
      mir_call_helper2(c->ctx, c->jit_func, rd,
                       c->helper2_proto, c->imp_mod, c->r_vm, c->r_js, rl, rr);
      mir_emit_bailout_check(c->ctx, c->jit_func, rd,
                             c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx);
      break;
    }

    case OP_NEG: {
      int top_idx = c->vs.sp - 1;
      bool input_is_num = vstack_prepare_num(
          &c->vs, top_idx, c->ctx, c->jit_func, c->r_d_slot);
      vstack_clear_value_info(&c->vs, top_idx);

      if (input_is_num) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DNEG,
                                     MIR_new_reg_op(c->ctx, c->vs.d_regs[top_idx]),
                                     MIR_new_reg_op(c->ctx, c->vs.d_regs[top_idx])));
        break;
      }

      MIR_reg_t rs = vstack_top(&c->vs);
      MIR_label_t slow = MIR_new_label(c->ctx);
      MIR_label_t done = MIR_new_label(c->ctx);

      mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rs, slow);

      int nn = c->arith_n++;
      char neg_d1[32], neg_d2[32];
      snprintf(neg_d1, sizeof(neg_d1), "neg_d1_%d", nn);
      snprintf(neg_d2, sizeof(neg_d2), "neg_d2_%d", nn);
      MIR_reg_t fd1 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, neg_d1);
      MIR_reg_t fd2 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, neg_d2);
      mir_i64_to_d(c->ctx, c->jit_func, fd1, rs, c->r_d_slot);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_DNEG,
                                   MIR_new_reg_op(c->ctx, fd2),
                                   MIR_new_reg_op(c->ctx, fd1)));
      mir_d_to_i64(c->ctx, c->jit_func, rs, fd2, c->r_d_slot);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));

      MIR_append_insn(c->ctx, c->jit_func, slow);
      mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, c->vs.sp, &c->bailout_ctx, -1, SLOT_BOXED, -1, SLOT_BOXED);

      MIR_append_insn(c->ctx, c->jit_func, done);
      break;
    }

    case OP_INC:
    case OP_DEC: {
      int top_idx = c->vs.sp - 1;
      bool input_is_num = vstack_prepare_num(
          &c->vs, top_idx, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t rs = vstack_top(&c->vs);

      if (c->vs.known_func) c->vs.known_func[top_idx] = NULL;
      if (c->vs.has_const) c->vs.has_const[top_idx] = false;

      MIR_label_t bailout = input_is_num ? NULL : MIR_new_label(c->ctx);
      MIR_label_t done = input_is_num ? NULL : MIR_new_label(c->ctx);
      if (!input_is_num)
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rs, bailout);

      vstack_ensure_num(&c->vs, top_idx, c->ctx, c->jit_func, c->r_d_slot);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, c->op == OP_INC ? MIR_DADD : MIR_DSUB,
                                   MIR_new_reg_op(c->ctx, c->vs.d_regs[top_idx]),
                                   MIR_new_reg_op(c->ctx, c->vs.d_regs[top_idx]),
                                   MIR_new_reg_op(c->ctx, c->r_d_one)));

      if (!input_is_num) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
        MIR_append_insn(c->ctx, c->jit_func, bailout);
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, c->vs.sp,
                                    &c->bailout_ctx, top_idx, false, -1, false);
        MIR_append_insn(c->ctx, c->jit_func, done);
      }
      break;
    }

    case OP_POST_INC: {
      int top_idx = c->vs.sp - 1;
      vstack_ensure_boxed(&c->vs, top_idx, c->ctx, c->jit_func, c->r_d_slot);

      MIR_reg_t rold = vstack_top(&c->vs);
      MIR_reg_t rnew = vstack_push(&c->vs);

      int pin = c->arith_n++;
      char pi_d1[32], pi_d2[32];
      snprintf(pi_d1, sizeof(pi_d1), "pi_d1_%d", pin);
      snprintf(pi_d2, sizeof(pi_d2), "pi_d2_%d", pin);
      MIR_reg_t fd1 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, pi_d1);
      MIR_reg_t fd2 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, pi_d2);

      mir_i64_to_d(c->ctx, c->jit_func, fd1, rold, c->r_d_slot);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_DADD,
                                   MIR_new_reg_op(c->ctx, fd2),
                                   MIR_new_reg_op(c->ctx, fd1),
                                   MIR_new_reg_op(c->ctx, c->r_d_one)));
      mir_d_to_i64(c->ctx, c->jit_func, rnew, fd2, c->r_d_slot);
      break;
    }

    case OP_POST_DEC: {
      int old_idx = c->vs.sp - 1;
      bool input_is_num = vstack_prepare_num(
          &c->vs, old_idx, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t rold = vstack_top(&c->vs);
      vstack_push(&c->vs);
      int new_idx = c->vs.sp - 1;

      if (c->vs.known_func) {
        c->vs.known_func[old_idx] = NULL;
        c->vs.known_func[new_idx] = NULL;
      }
      if (c->vs.has_const) {
        c->vs.has_const[old_idx] = false;
        c->vs.has_const[new_idx] = false;
      }

      MIR_label_t bailout = input_is_num ? NULL : MIR_new_label(c->ctx);
      MIR_label_t done = input_is_num ? NULL : MIR_new_label(c->ctx);
      if (!input_is_num)
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rold, bailout);

      vstack_ensure_num(&c->vs, old_idx, c->ctx, c->jit_func, c->r_d_slot);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_DSUB,
                                   MIR_new_reg_op(c->ctx, c->vs.d_regs[new_idx]),
                                   MIR_new_reg_op(c->ctx, c->vs.d_regs[old_idx]),
                                   MIR_new_reg_op(c->ctx, c->r_d_one)));
      c->vs.slot_type[new_idx] = SLOT_NUM;

      if (!input_is_num) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
        MIR_append_insn(c->ctx, c->jit_func, bailout);
        mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, c->vs.sp - 1,
                                    &c->bailout_ctx, old_idx, false, -1, false);
        MIR_append_insn(c->ctx, c->jit_func, done);
      }
      break;
    }

    default:
      __builtin_unreachable();
  }
}
