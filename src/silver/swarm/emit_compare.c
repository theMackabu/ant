#include "compile.h"

void jit_emit_compare(jit_compile_t *c) {
  switch (c->op) {
    case OP_LT: {
      uint8_t fb = sv_func_type_feedback(c->func) ? sv_func_type_feedback(c->func)[c->bc_off] : 0;
      bool fb_num_only = fb && !(fb & ~SV_TFB_NUM);
      bool fb_never_num = fb && !(fb & SV_TFB_NUM);

      bool l_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      bool r_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);

      MIR_reg_t rr = vstack_pop(&c->vs);
      MIR_reg_t rl = vstack_pop(&c->vs);
      MIR_reg_t rd = vstack_push(&c->vs);
      if (c->vs.known_bool) c->vs.known_bool[c->vs.sp - 1] = 1;

      if (fb_never_num) {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                     MIR_new_reg_op(c->ctx, rl)));
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_lt,
                         c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
      } else if (fb_num_only && l_is_num && r_is_num) {
        MIR_reg_t fd_l = c->vs.d_regs[c->vs.sp - 1];
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DLT,
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_reg_op(c->ctx, fd_l),
                                     MIR_new_reg_op(c->ctx, fd_r)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_tmp),
                                     MIR_new_reg_op(c->ctx, c->r_bool)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_OR,
                                     MIR_new_reg_op(c->ctx, rd),
                                     MIR_new_uint_op(c->ctx, js_false),
                                     MIR_new_reg_op(c->ctx, c->r_tmp)));
      } else if (fb_num_only && (l_is_num || r_is_num)) {
        MIR_label_t bail_direct = MIR_new_label(c->ctx);
        MIR_reg_t boxed_reg = l_is_num ? rr : rl;
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, boxed_reg, bail_direct);
        int boxed_idx = l_is_num ? (int)c->vs.sp : (int)(c->vs.sp - 1);
        mir_i64_to_d(c->ctx, c->jit_func, c->vs.d_regs[boxed_idx],
                     c->vs.regs[boxed_idx], c->r_d_slot);
        MIR_reg_t fd_l = c->vs.d_regs[c->vs.sp - 1];
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DLT,
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_reg_op(c->ctx, fd_l),
                                     MIR_new_reg_op(c->ctx, fd_r)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_tmp),
                                     MIR_new_reg_op(c->ctx, c->r_bool)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_OR,
                                     MIR_new_reg_op(c->ctx, rd),
                                     MIR_new_uint_op(c->ctx, js_false),
                                     MIR_new_reg_op(c->ctx, c->r_tmp)));
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
        int ltn = c->arith_n++;
        char d1[32], d2[32];
        snprintf(d1, sizeof(d1), "lt_d1_%d", ltn);
        snprintf(d2, sizeof(d2), "lt_d2_%d", ltn);
        MIR_reg_t fd1 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d1);
        MIR_reg_t fd2 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d2);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DLT,
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_reg_op(c->ctx, fd1),
                                     MIR_new_reg_op(c->ctx, fd2)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_tmp),
                                     MIR_new_reg_op(c->ctx, c->r_bool)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_OR,
                                     MIR_new_reg_op(c->ctx, rd),
                                     MIR_new_uint_op(c->ctx, js_false),
                                     MIR_new_reg_op(c->ctx, c->r_tmp)));
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
        int ltn = c->arith_n++;
        char d1[32], d2[32];
        snprintf(d1, sizeof(d1), "lt_d1_%d", ltn);
        snprintf(d2, sizeof(d2), "lt_d2_%d", ltn);
        MIR_reg_t fd1 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d1);
        MIR_reg_t fd2 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d2);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DLT,
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_reg_op(c->ctx, fd1),
                                     MIR_new_reg_op(c->ctx, fd2)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_tmp),
                                     MIR_new_reg_op(c->ctx, c->r_bool)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_OR,
                                     MIR_new_reg_op(c->ctx, rd),
                                     MIR_new_uint_op(c->ctx, js_false),
                                     MIR_new_reg_op(c->ctx, c->r_tmp)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                     MIR_new_reg_op(c->ctx, rl)));
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_lt, c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        MIR_append_insn(c->ctx, c->jit_func, done);
      }
      break;
    }

    case OP_LE: {
      uint8_t fb = sv_func_type_feedback(c->func) ? sv_func_type_feedback(c->func)[c->bc_off] : 0;
      bool fb_num_only = fb && !(fb & ~SV_TFB_NUM);
      bool fb_never_num = fb && !(fb & SV_TFB_NUM);

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
                         c->helper2_proto, c->imp_le,
                         c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
      } else if (fb_num_only && l_is_num && r_is_num) {
        MIR_reg_t fd_l = c->vs.d_regs[c->vs.sp - 1];
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DLE,
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_reg_op(c->ctx, fd_l),
                                     MIR_new_reg_op(c->ctx, fd_r)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_tmp),
                                     MIR_new_reg_op(c->ctx, c->r_bool)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_OR,
                                     MIR_new_reg_op(c->ctx, rd),
                                     MIR_new_uint_op(c->ctx, js_false),
                                     MIR_new_reg_op(c->ctx, c->r_tmp)));
      } else if (fb_num_only && (l_is_num || r_is_num)) {
        MIR_label_t bail_direct = MIR_new_label(c->ctx);
        MIR_reg_t boxed_reg = l_is_num ? rr : rl;
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, boxed_reg, bail_direct);
        int boxed_idx = l_is_num ? (int)c->vs.sp : (int)(c->vs.sp - 1);
        mir_i64_to_d(c->ctx, c->jit_func, c->vs.d_regs[boxed_idx],
                     c->vs.regs[boxed_idx], c->r_d_slot);
        MIR_reg_t fd_l = c->vs.d_regs[c->vs.sp - 1];
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DLE,
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_reg_op(c->ctx, fd_l),
                                     MIR_new_reg_op(c->ctx, fd_r)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_tmp),
                                     MIR_new_reg_op(c->ctx, c->r_bool)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_OR,
                                     MIR_new_reg_op(c->ctx, rd),
                                     MIR_new_uint_op(c->ctx, js_false),
                                     MIR_new_reg_op(c->ctx, c->r_tmp)));
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
        int len = c->arith_n++;
        char d1[32], d2[32];
        snprintf(d1, sizeof(d1), "le_d1_%d", len);
        snprintf(d2, sizeof(d2), "le_d2_%d", len);
        MIR_reg_t fd1 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d1);
        MIR_reg_t fd2 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d2);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DLE,
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_reg_op(c->ctx, fd1),
                                     MIR_new_reg_op(c->ctx, fd2)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_tmp),
                                     MIR_new_reg_op(c->ctx, c->r_bool)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_OR,
                                     MIR_new_reg_op(c->ctx, rd),
                                     MIR_new_uint_op(c->ctx, js_false),
                                     MIR_new_reg_op(c->ctx, c->r_tmp)));
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
        int len = c->arith_n++;
        char d1[32], d2[32];
        snprintf(d1, sizeof(d1), "le_d1_%d", len);
        snprintf(d2, sizeof(d2), "le_d2_%d", len);
        MIR_reg_t fd1 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d1);
        MIR_reg_t fd2 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d2);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DLE,
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_reg_op(c->ctx, fd1),
                                     MIR_new_reg_op(c->ctx, fd2)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_tmp),
                                     MIR_new_reg_op(c->ctx, c->r_bool)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_OR,
                                     MIR_new_reg_op(c->ctx, rd),
                                     MIR_new_uint_op(c->ctx, js_false),
                                     MIR_new_reg_op(c->ctx, c->r_tmp)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                     MIR_new_reg_op(c->ctx, rl)));
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_le, c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        MIR_append_insn(c->ctx, c->jit_func, done);
      }
      break;
    }

    case OP_SEQ:
    case OP_EQ:
    case OP_NE:
    case OP_SNE: {
      uint8_t feedback = sv_func_type_feedback(c->func)
                             ? sv_func_type_feedback(c->func)[c->bc_off]
                             : 0;
      bool numeric_only = sv_tfb_specialization_ready(feedback) &&
                          (feedback & SV_TFB_CLASS_MASK) == SV_TFB_NUM;
      bool invert = c->op == OP_NE || c->op == OP_SNE;
      bool r_const = c->vs.has_const && c->vs.has_const[c->vs.sp - 1];
      bool l_const = c->vs.has_const && c->vs.has_const[c->vs.sp - 2];
      uint64_t cval = r_const   ? c->vs.known_const[c->vs.sp - 1]
                      : l_const ? c->vs.known_const[c->vs.sp - 2]
                                : 0;
      bool strict = c->op == OP_SEQ || c->op == OP_SNE;
      bool is_nullish = !strict && (r_const || l_const) &&
                        (cval == mkval(kTypeNull, 0) || cval == mkval(kTypeUndefined, 0));

      if (numeric_only) {
        int right_idx = c->vs.sp - 1;
        int left_idx = c->vs.sp - 2;
        bool right_is_num = vstack_prepare_num(
            &c->vs, right_idx, c->ctx, c->jit_func, c->r_d_slot);
        bool left_is_num = vstack_prepare_num(
            &c->vs, left_idx, c->ctx, c->jit_func, c->r_d_slot);
        MIR_reg_t rr = vstack_pop(&c->vs);
        MIR_reg_t rl = vstack_pop(&c->vs);
        MIR_reg_t dst = vstack_push(&c->vs);
        MIR_label_t eq_slow = MIR_new_label(c->ctx);
        MIR_label_t done = MIR_new_label(c->ctx);

        if (!left_is_num) {
          mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rl, eq_slow);
          mir_i64_to_d(c->ctx, c->jit_func, c->vs.d_regs[left_idx], rl, c->r_d_slot);
        }
        if (!right_is_num) {
          mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, rr, eq_slow);
          mir_i64_to_d(c->ctx, c->jit_func, c->vs.d_regs[right_idx], rr, c->r_d_slot);
        }
        mir_emit_numeric_equality(
            c->ctx, c->jit_func, c->vs.d_regs[left_idx], c->vs.d_regs[right_idx],
            dst, c->r_bool, invert);
        if (c->vs.known_bool) c->vs.known_bool[c->vs.sp - 1] = 1;
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
        MIR_append_insn(c->ctx, c->jit_func, eq_slow);
        if (left_is_num)
          mir_d_to_i64(c->ctx, c->jit_func, rl, c->vs.d_regs[left_idx], c->r_d_slot);
        if (right_is_num)
          mir_d_to_i64(c->ctx, c->jit_func, rr, c->vs.d_regs[right_idx], c->r_d_slot);
        MIR_item_t helper = c->op == OP_SEQ  ? c->imp_seq
                            : c->op == OP_EQ ? c->imp_eq
                            : c->op == OP_NE ? c->imp_ne
                                             : c->imp_sne;
        mir_call_helper2(c->ctx, c->jit_func, dst,
                         c->helper2_proto, helper,
                         c->r_vm, c->r_js, rl, rr);
        MIR_append_insn(c->ctx, c->jit_func, done);
        break;
      }

      int right_idx = c->vs.sp - 1;
      int left_idx = c->vs.sp - 2;
      uint8_t right_type = c->vs.slot_type
                               ? c->vs.slot_type[right_idx]
                               : SLOT_BOXED;
      uint8_t left_type = c->vs.slot_type
                              ? c->vs.slot_type[left_idx]
                              : SLOT_BOXED;
      uint8_t singleton_other_type = r_const ? left_type : right_type;
      bool known_number_vs_singleton = strict && (r_const || l_const) &&
                                       singleton_other_type != SLOT_BOXED;
      if (!known_number_vs_singleton) {
        vstack_ensure_boxed(&c->vs, right_idx, c->ctx, c->jit_func, c->r_d_slot);
        vstack_ensure_boxed(&c->vs, left_idx, c->ctx, c->jit_func, c->r_d_slot);
      }
      MIR_reg_t rr = vstack_pop(&c->vs);
      MIR_reg_t rl = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      if (c->vs.known_bool) c->vs.known_bool[c->vs.sp - 1] = 1;

      if (strict && (r_const || l_const)) {
        if (known_number_vs_singleton)
          mir_load_imm(c->ctx, c->jit_func, dst, invert ? js_true : js_false);
        else {
          MIR_reg_t other = r_const ? rl : rr;
          MIR_label_t matched = MIR_new_label(c->ctx);
          MIR_label_t eq_done = MIR_new_label(c->ctx);
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_BEQ,
                                       MIR_new_label_op(c->ctx, matched),
                                       MIR_new_reg_op(c->ctx, other),
                                       MIR_new_uint_op(c->ctx, cval)));
          mir_load_imm(c->ctx, c->jit_func, dst, invert ? js_true : js_false);
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, eq_done)));
          MIR_append_insn(c->ctx, c->jit_func, matched);
          mir_load_imm(c->ctx, c->jit_func, dst, invert ? js_false : js_true);
          MIR_append_insn(c->ctx, c->jit_func, eq_done);
        }
      } else if (strict) {
        MIR_label_t eq_slow = MIR_new_label(c->ctx);
        MIR_label_t eq_done = MIR_new_label(c->ctx);
        mir_emit_strict_tagged_equality(
            c->ctx, c->jit_func, rl, rr, dst, c->r_bool, invert, eq_slow, eq_done);
        MIR_append_insn(c->ctx, c->jit_func, eq_slow);
        MIR_item_t helper = c->op == OP_SEQ ? c->imp_seq : c->imp_sne;
        mir_call_helper2(c->ctx, c->jit_func, dst,
                         c->helper2_proto, helper,
                         c->r_vm, c->r_js, rl, rr);
        MIR_append_insn(c->ctx, c->jit_func, eq_done);
      } else if (is_nullish) {
        MIR_reg_t other = r_const ? rl : rr;
        MIR_label_t matched = MIR_new_label(c->ctx);
        MIR_label_t is_done = MIR_new_label(c->ctx);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BEQ,
                                     MIR_new_label_op(c->ctx, matched),
                                     MIR_new_reg_op(c->ctx, other),
                                     MIR_new_uint_op(c->ctx, mkval(kTypeNull, 0))));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BEQ,
                                     MIR_new_label_op(c->ctx, matched),
                                     MIR_new_reg_op(c->ctx, other),
                                     MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0))));
        mir_load_imm(c->ctx, c->jit_func, dst, invert ? js_true : js_false);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, is_done)));
        MIR_append_insn(c->ctx, c->jit_func, matched);
        mir_load_imm(c->ctx, c->jit_func, dst, invert ? js_false : js_true);
        MIR_append_insn(c->ctx, c->jit_func, is_done);
      } else {
        MIR_item_t helper = c->op == OP_SEQ  ? c->imp_seq
                            : c->op == OP_EQ ? c->imp_eq
                            : c->op == OP_NE ? c->imp_ne
                                             : c->imp_sne;
        mir_call_helper2(c->ctx, c->jit_func, dst,
                         c->helper2_proto, helper,
                         c->r_vm, c->r_js, rl, rr);
      }
      break;
    }

    case OP_GT: {
      uint8_t fb = sv_func_type_feedback(c->func) ? sv_func_type_feedback(c->func)[c->bc_off] : 0;
      bool fb_num_only = fb && !(fb & ~SV_TFB_NUM);
      bool fb_never_num = fb && !(fb & SV_TFB_NUM);

      bool l_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      bool r_is_num = vstack_prepare_num(
          &c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);

      MIR_reg_t rr = vstack_pop(&c->vs);
      MIR_reg_t rl = vstack_pop(&c->vs);
      MIR_reg_t rd = vstack_push(&c->vs);
      if (c->vs.known_bool) c->vs.known_bool[c->vs.sp - 1] = 1;

      if (fb_never_num) {
        vstack_rebox_binop_operands(
            &c->vs, c->ctx, c->jit_func, l_is_num, r_is_num, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                     MIR_new_reg_op(c->ctx, rl)));
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_gt,
                         c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
      } else if (fb_num_only && l_is_num && r_is_num) {
        MIR_reg_t fd_l = c->vs.d_regs[c->vs.sp - 1];
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DGT,
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_reg_op(c->ctx, fd_l),
                                     MIR_new_reg_op(c->ctx, fd_r)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_tmp),
                                     MIR_new_reg_op(c->ctx, c->r_bool)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_OR,
                                     MIR_new_reg_op(c->ctx, rd),
                                     MIR_new_uint_op(c->ctx, js_false),
                                     MIR_new_reg_op(c->ctx, c->r_tmp)));
      } else if (fb_num_only && (l_is_num || r_is_num)) {
        MIR_label_t bail_direct = MIR_new_label(c->ctx);
        MIR_reg_t boxed_reg = l_is_num ? rr : rl;
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, boxed_reg, bail_direct);
        int boxed_idx = l_is_num ? (int)c->vs.sp : (int)(c->vs.sp - 1);
        mir_i64_to_d(c->ctx, c->jit_func, c->vs.d_regs[boxed_idx],
                     c->vs.regs[boxed_idx], c->r_d_slot);
        MIR_reg_t fd_l = c->vs.d_regs[c->vs.sp - 1];
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DGT,
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_reg_op(c->ctx, fd_l),
                                     MIR_new_reg_op(c->ctx, fd_r)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_tmp),
                                     MIR_new_reg_op(c->ctx, c->r_bool)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_OR,
                                     MIR_new_reg_op(c->ctx, rd),
                                     MIR_new_uint_op(c->ctx, js_false),
                                     MIR_new_reg_op(c->ctx, c->r_tmp)));
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
        int gtn = c->arith_n++;
        char d1[32], d2[32];
        snprintf(d1, sizeof(d1), "gt_d1_%d", gtn);
        snprintf(d2, sizeof(d2), "gt_d2_%d", gtn);
        MIR_reg_t fd1 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d1);
        MIR_reg_t fd2 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d2);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DGT,
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_reg_op(c->ctx, fd1),
                                     MIR_new_reg_op(c->ctx, fd2)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_tmp),
                                     MIR_new_reg_op(c->ctx, c->r_bool)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_OR,
                                     MIR_new_reg_op(c->ctx, rd),
                                     MIR_new_uint_op(c->ctx, js_false),
                                     MIR_new_reg_op(c->ctx, c->r_tmp)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                     MIR_new_reg_op(c->ctx, rl)));
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_gt, c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        MIR_append_insn(c->ctx, c->jit_func, done);
      }
      break;
    }

    case OP_GE: {
      uint8_t fb = sv_func_type_feedback(c->func) ? sv_func_type_feedback(c->func)[c->bc_off] : 0;
      bool fb_num_only = fb && !(fb & ~SV_TFB_NUM);
      bool fb_never_num = fb && !(fb & SV_TFB_NUM);

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
                         c->helper2_proto, c->imp_ge,
                         c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
      } else if (fb_num_only && l_is_num && r_is_num) {
        MIR_reg_t fd_l = c->vs.d_regs[c->vs.sp - 1];
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DGE,
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_reg_op(c->ctx, fd_l),
                                     MIR_new_reg_op(c->ctx, fd_r)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_tmp),
                                     MIR_new_reg_op(c->ctx, c->r_bool)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_OR,
                                     MIR_new_reg_op(c->ctx, rd),
                                     MIR_new_uint_op(c->ctx, js_false),
                                     MIR_new_reg_op(c->ctx, c->r_tmp)));
      } else if (fb_num_only && (l_is_num || r_is_num)) {
        MIR_label_t bail_direct = MIR_new_label(c->ctx);
        MIR_reg_t boxed_reg = l_is_num ? rr : rl;
        mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, boxed_reg, bail_direct);
        int boxed_idx = l_is_num ? (int)c->vs.sp : (int)(c->vs.sp - 1);
        mir_i64_to_d(c->ctx, c->jit_func, c->vs.d_regs[boxed_idx],
                     c->vs.regs[boxed_idx], c->r_d_slot);
        MIR_reg_t fd_l = c->vs.d_regs[c->vs.sp - 1];
        MIR_reg_t fd_r = c->vs.d_regs[c->vs.sp];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DGE,
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_reg_op(c->ctx, fd_l),
                                     MIR_new_reg_op(c->ctx, fd_r)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_tmp),
                                     MIR_new_reg_op(c->ctx, c->r_bool)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_OR,
                                     MIR_new_reg_op(c->ctx, rd),
                                     MIR_new_uint_op(c->ctx, js_false),
                                     MIR_new_reg_op(c->ctx, c->r_tmp)));
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
        int gen = c->arith_n++;
        char d1[32], d2[32];
        snprintf(d1, sizeof(d1), "ge_d1_%d", gen);
        snprintf(d2, sizeof(d2), "ge_d2_%d", gen);
        MIR_reg_t fd1 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d1);
        MIR_reg_t fd2 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, d2);
        mir_i64_to_d(c->ctx, c->jit_func, fd1, rl, c->r_d_slot);
        mir_i64_to_d(c->ctx, c->jit_func, fd2, rr, c->r_d_slot);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_DGE,
                                     MIR_new_reg_op(c->ctx, c->r_bool),
                                     MIR_new_reg_op(c->ctx, fd1),
                                     MIR_new_reg_op(c->ctx, fd2)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_tmp),
                                     MIR_new_reg_op(c->ctx, c->r_bool)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_OR,
                                     MIR_new_reg_op(c->ctx, rd),
                                     MIR_new_uint_op(c->ctx, js_false),
                                     MIR_new_reg_op(c->ctx, c->r_tmp)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
        MIR_append_insn(c->ctx, c->jit_func, slow);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_bailout_val),
                                     MIR_new_reg_op(c->ctx, rl)));
        mir_call_helper2(c->ctx, c->jit_func, rd,
                         c->helper2_proto, c->imp_ge, c->r_vm, c->r_js, rl, rr);
        mir_emit_bailout_check_typed(c->ctx, c->jit_func, rd,
                                     c->r_bailout_val, c->bc_off, c->vs.sp + 1, &c->bailout_ctx,
                                     c->vs.sp - 1, l_is_num, c->vs.sp, r_is_num);
        MIR_append_insn(c->ctx, c->jit_func, done);
      }
      break;
    }

    case OP_IN: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t rr = vstack_pop(&c->vs);
      MIR_reg_t rl = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      mir_call_helper2(c->ctx, c->jit_func, dst,
                       c->helper2_proto, c->imp_in,
                       c->r_vm, c->r_js, rl, rr);
      jit_emit_throw_if_error(c, dst);
      break;
    }

    case OP_INSTANCEOF: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t rr = vstack_pop(&c->vs);
      MIR_reg_t rl = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      uint16_t ic_idx = sv_get_u16(c->ip + 1);
      bool has_ic = c->func->ic_slots && ic_idx < c->func->ic_count;
      MIR_label_t slow = has_ic ? MIR_new_label(c->ctx) : NULL;
      MIR_label_t no_err = MIR_new_label(c->ctx);

      if (has_ic) {
        sv_ic_entry_t *ic = &c->func->ic_slots[ic_idx];
        char inst_ic_name[32], inst_ice_name[32];
        char inst_rt_name[32], inst_ro_name[32], inst_ica_name[32], inst_lt_name[32];
        char inst_lo_name[32], inst_lf_name[32], inst_lp_name[32], inst_icp_name[32];
        char inst_lpt_name[32], inst_lpo_name[32], inst_ls_name[32], inst_ich_name[32];
        char inst_ics_name[32], inst_ici_name[32], inst_pe_name[32], inst_ipe_name[32];
        snprintf(inst_ic_name, sizeof(inst_ic_name), "inst_ic_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(inst_ice_name, sizeof(inst_ice_name), "inst_ice_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(inst_rt_name, sizeof(inst_rt_name), "inst_rt_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(inst_ro_name, sizeof(inst_ro_name), "inst_ro_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(inst_ica_name, sizeof(inst_ica_name), "inst_ica_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(inst_lt_name, sizeof(inst_lt_name), "inst_lt_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(inst_lo_name, sizeof(inst_lo_name), "inst_lo_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(inst_lf_name, sizeof(inst_lf_name), "inst_lf_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(inst_lp_name, sizeof(inst_lp_name), "inst_lp_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(inst_icp_name, sizeof(inst_icp_name), "inst_icp_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(inst_lpt_name, sizeof(inst_lpt_name), "inst_lpt_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(inst_lpo_name, sizeof(inst_lpo_name), "inst_lpo_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(inst_ls_name, sizeof(inst_ls_name), "inst_ls_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(inst_ich_name, sizeof(inst_ich_name), "inst_ich_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(inst_ics_name, sizeof(inst_ics_name), "inst_ics_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(inst_ici_name, sizeof(inst_ici_name), "inst_ici_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(inst_pe_name, sizeof(inst_pe_name), "inst_pe_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(inst_ipe_name, sizeof(inst_ipe_name), "inst_ipe_%d_%u", c->bc_off, (unsigned)ic_idx);

        MIR_reg_t r_ic = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, inst_ic_name);
        MIR_reg_t r_ic_epoch = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, inst_ice_name);
        MIR_reg_t r_rhs_tag = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, inst_rt_name);
        MIR_reg_t r_rhs_obj = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, inst_ro_name);
        MIR_reg_t r_ic_aux = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, inst_ica_name);
        MIR_reg_t r_lhs_tag = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, inst_lt_name);
        MIR_reg_t r_lhs_obj = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, inst_lo_name);
        MIR_reg_t r_lhs_flags = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, inst_lf_name);
        MIR_reg_t r_lhs_proto = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, inst_lp_name);
        MIR_reg_t r_ic_direct_proto = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, inst_icp_name);
        MIR_reg_t r_lhs_proto_tag = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, inst_lpt_name);
        MIR_reg_t r_lhs_proto_obj = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, inst_lpo_name);
        MIR_reg_t r_lhs_shape = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, inst_ls_name);
        MIR_reg_t r_ic_holder = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, inst_ich_name);
        MIR_reg_t r_ic_shape = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, inst_ics_name);
        MIR_reg_t r_ic_idx_val = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, inst_ici_name);
        MIR_reg_t r_proto_epoch = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, inst_pe_name);
        MIR_reg_t r_ic_proto_epoch = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, inst_ipe_name);
        MIR_label_t direct_true = MIR_new_label(c->ctx);

        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, r_ic),
                                     MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)ic)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, r_ic_epoch),
                                     MIR_new_mem_op(c->ctx, MIR_T_U32,
                                                    (MIR_disp_t)offsetof(sv_ic_entry_t, epoch), r_ic, 0, 1)));
        {
          char ice_cur_name[40];
          snprintf(ice_cur_name, sizeof(ice_cur_name), "inst_ce_%d_%u", c->bc_off, (unsigned)ic_idx);
          MIR_reg_t r_cur_ep = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, ice_cur_name);
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_MOV,
                                       MIR_new_reg_op(c->ctx, r_cur_ep),
                                       MIR_new_mem_op(c->ctx, MIR_T_U32, 0, c->r_ic_epoch_val, 0, 1)));
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_BNE,
                                       MIR_new_label_op(c->ctx, slow),
                                       MIR_new_reg_op(c->ctx, r_ic_epoch),
                                       MIR_new_reg_op(c->ctx, r_cur_ep)));
        }

        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, r_proto_epoch),
                                     MIR_new_mem_op(c->ctx, MIR_T_U32,
                                                    (MIR_disp_t)offsetof(ant_t, prototype_write_epoch), c->r_js, 0, 1)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, r_ic_proto_epoch),
                                     MIR_new_mem_op(c->ctx, MIR_T_U32,
                                                    (MIR_disp_t)offsetof(sv_ic_entry_t, prototype_epoch), r_ic, 0, 1)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BNE,
                                     MIR_new_label_op(c->ctx, slow),
                                     MIR_new_reg_op(c->ctx, r_ic_proto_epoch),
                                     MIR_new_reg_op(c->ctx, r_proto_epoch)));

        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_URSH,
                                     MIR_new_reg_op(c->ctx, r_rhs_tag),
                                     MIR_new_reg_op(c->ctx, rr),
                                     MIR_new_uint_op(c->ctx, NANBOX_TYPE_SHIFT)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BNE,
                                     MIR_new_label_op(c->ctx, slow),
                                     MIR_new_reg_op(c->ctx, r_rhs_tag),
                                     MIR_new_uint_op(c->ctx, NANBOX_TFUNC_TAG)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_AND,
                                     MIR_new_reg_op(c->ctx, r_rhs_obj),
                                     MIR_new_reg_op(c->ctx, rr),
                                     MIR_new_uint_op(c->ctx, NANBOX_DATA_MASK)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, r_ic_aux),
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)offsetof(sv_ic_entry_t, cached_aux), r_ic, 0, 1)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BNE,
                                     MIR_new_label_op(c->ctx, slow),
                                     MIR_new_reg_op(c->ctx, r_rhs_obj),
                                     MIR_new_reg_op(c->ctx, r_ic_aux)));

        mir_emit_value_to_objptr_or_jmp(
            c->ctx, c->jit_func, rl, r_lhs_obj, r_lhs_tag, slow);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, r_lhs_flags),
                                     MIR_new_mem_op(c->ctx, MIR_T_U8,
                                                    (MIR_disp_t)offsetof(ant_object_t, flags), r_lhs_obj, 0, 1)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_AND,
                                     MIR_new_reg_op(c->ctx, r_lhs_flags),
                                     MIR_new_reg_op(c->ctx, r_lhs_flags),
                                     MIR_new_uint_op(c->ctx, 1u << 3)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BNE,
                                     MIR_new_label_op(c->ctx, slow),
                                     MIR_new_reg_op(c->ctx, r_lhs_flags),
                                     MIR_new_uint_op(c->ctx, 0)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, r_lhs_proto),
                                     MIR_new_mem_op(c->ctx, MIR_JSVAL,
                                                    (MIR_disp_t)offsetof(ant_object_t, proto), r_lhs_obj, 0, 1)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, r_ic_direct_proto),
                                     MIR_new_mem_op(c->ctx, MIR_JSVAL,
                                                    (MIR_disp_t)offsetof(
                                                        sv_ic_entry_t, guard.comparison.receiver_proto),
                                                    r_ic, 0, 1)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BEQ,
                                     MIR_new_label_op(c->ctx, direct_true),
                                     MIR_new_reg_op(c->ctx, r_lhs_proto),
                                     MIR_new_reg_op(c->ctx, r_ic_direct_proto)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, r_lhs_shape),
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)offsetof(ant_object_t, shape), r_lhs_obj, 0, 1)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, r_ic_shape),
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)offsetof(sv_ic_entry_t, cached_shape), r_ic, 0, 1)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BNE,
                                     MIR_new_label_op(c->ctx, slow),
                                     MIR_new_reg_op(c->ctx, r_lhs_shape),
                                     MIR_new_reg_op(c->ctx, r_ic_shape)));
        mir_emit_value_to_objptr_or_jmp(
            c->ctx, c->jit_func, r_lhs_proto, r_lhs_proto_obj, r_lhs_proto_tag, slow);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, r_ic_holder),
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)offsetof(sv_ic_entry_t, cached_holder), r_ic, 0, 1)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BNE,
                                     MIR_new_label_op(c->ctx, slow),
                                     MIR_new_reg_op(c->ctx, r_lhs_proto_obj),
                                     MIR_new_reg_op(c->ctx, r_ic_holder)));
        mir_emit_ic_obj_epoch_guard(
            c->ctx, c->jit_func, r_ic, slow, "inst", c->bc_off, ic_idx);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, r_ic_idx_val),
                                     MIR_new_mem_op(c->ctx, MIR_T_I32,
                                                    (MIR_disp_t)offsetof(sv_ic_entry_t, cached_index), r_ic, 0, 1)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_OR,
                                     MIR_new_reg_op(c->ctx, dst),
                                     MIR_new_uint_op(c->ctx, js_false),
                                     MIR_new_reg_op(c->ctx, r_ic_idx_val)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, no_err)));

        MIR_append_insn(c->ctx, c->jit_func, direct_true);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, dst),
                                     MIR_new_uint_op(c->ctx, js_true)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, no_err)));

        MIR_append_insn(c->ctx, c->jit_func, slow);
      }

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 9,
                                        MIR_new_ref_op(c->ctx, c->inst_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_instanceof),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, rl),
                                        MIR_new_reg_op(c->ctx, rr),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                        MIR_new_int_op(c->ctx, (int64_t)c->bc_off)));
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
                      MIR_new_insn(c->ctx, MIR_URSH,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, dst),
                                   MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BNE,
                                   MIR_new_label_op(c->ctx, no_err),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx, JIT_ERR_TAG)));
      if (c->jit_try_depth > 0) {
        jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->vs.regs[h->saved_sp]),
                                     MIR_new_reg_op(c->ctx, dst)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, h->catch_label)));
      } else {
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, dst));
      }
      MIR_append_insn(c->ctx, c->jit_func, no_err);
      break;
    }

    case OP_CALL_IS_PROTO: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 3, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t arg = vstack_pop(&c->vs);
      MIR_reg_t fn = vstack_pop(&c->vs);
      MIR_reg_t this_obj = vstack_pop(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      uint16_t ic_idx = sv_get_u16(c->ip + 1);
      bool has_ic = c->func->ic_slots && ic_idx < c->func->ic_count;
      MIR_label_t slow = has_ic ? MIR_new_label(c->ctx) : NULL;
      MIR_label_t no_err = MIR_new_label(c->ctx);

      if (has_ic) {
        sv_ic_entry_t *ic = &c->func->ic_slots[ic_idx];
        char cip_bi_name[32], cip_ic_name[32], cip_ice_name[32];
        char cip_pt_name[32], cip_pp_name[32], cip_ot_name[32];
        char cip_op_name[32], cip_ich_name[32], cip_ics_name[32], cip_ici_name[32];
        snprintf(cip_bi_name, sizeof(cip_bi_name), "cip_bi_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(cip_ic_name, sizeof(cip_ic_name), "cip_ic_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(cip_ice_name, sizeof(cip_ice_name), "cip_ice_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(cip_pt_name, sizeof(cip_pt_name), "cip_pt_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(cip_pp_name, sizeof(cip_pp_name), "cip_pp_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(cip_ot_name, sizeof(cip_ot_name), "cip_ot_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(cip_op_name, sizeof(cip_op_name), "cip_op_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(cip_ich_name, sizeof(cip_ich_name), "cip_ich_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(cip_ics_name, sizeof(cip_ics_name), "cip_ics_%d_%u", c->bc_off, (unsigned)ic_idx);
        snprintf(cip_ici_name, sizeof(cip_ici_name), "cip_ici_%d_%u", c->bc_off, (unsigned)ic_idx);

        MIR_reg_t r_builtin = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, cip_bi_name);
        MIR_reg_t r_ic = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, cip_ic_name);
        MIR_reg_t r_ic_epoch = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, cip_ice_name);
        MIR_reg_t r_proto_tag = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, cip_pt_name);
        MIR_reg_t r_proto_ptr = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, cip_pp_name);
        MIR_reg_t r_obj_tag = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, cip_ot_name);
        MIR_reg_t r_obj_ptr = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, cip_op_name);
        MIR_reg_t r_ic_holder = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, cip_ich_name);
        MIR_reg_t r_ic_shape = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, cip_ics_name);
        MIR_reg_t r_ic_idx_val = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, cip_ici_name);

        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, r_builtin),
                                     MIR_new_uint_op(c->ctx, (uint64_t)js_mkfun(builtin_object_isPrototypeOf))));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BNE,
                                     MIR_new_label_op(c->ctx, slow),
                                     MIR_new_reg_op(c->ctx, fn),
                                     MIR_new_reg_op(c->ctx, r_builtin)));

        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, r_ic),
                                     MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)ic)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, r_ic_epoch),
                                     MIR_new_mem_op(c->ctx, MIR_T_U32,
                                                    (MIR_disp_t)offsetof(sv_ic_entry_t, epoch), r_ic, 0, 1)));
        {
          char cip_ce_name[40];
          snprintf(cip_ce_name, sizeof(cip_ce_name), "cip_ce_%d_%u", c->bc_off, (unsigned)ic_idx);
          MIR_reg_t r_cur_ep = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, cip_ce_name);
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_MOV,
                                       MIR_new_reg_op(c->ctx, r_cur_ep),
                                       MIR_new_mem_op(c->ctx, MIR_T_U32, 0, c->r_ic_epoch_val, 0, 1)));
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_BNE,
                                       MIR_new_label_op(c->ctx, slow),
                                       MIR_new_reg_op(c->ctx, r_ic_epoch),
                                       MIR_new_reg_op(c->ctx, r_cur_ep)));
        }

        mir_emit_value_to_objptr_or_jmp(
            c->ctx, c->jit_func, this_obj, r_proto_ptr, r_proto_tag, slow);
        mir_emit_value_to_objptr_or_jmp(
            c->ctx, c->jit_func, arg, r_obj_ptr, r_obj_tag, slow);

        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, r_ic_holder),
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)offsetof(sv_ic_entry_t, cached_holder), r_ic, 0, 1)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BNE,
                                     MIR_new_label_op(c->ctx, slow),
                                     MIR_new_reg_op(c->ctx, r_proto_ptr),
                                     MIR_new_reg_op(c->ctx, r_ic_holder)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, r_ic_shape),
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)offsetof(sv_ic_entry_t, cached_shape), r_ic, 0, 1)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BNE,
                                     MIR_new_label_op(c->ctx, slow),
                                     MIR_new_reg_op(c->ctx, r_obj_ptr),
                                     MIR_new_reg_op(c->ctx, r_ic_shape)));
        mir_emit_ic_obj_epoch_guard(
            c->ctx, c->jit_func, r_ic, slow, "cip", c->bc_off, ic_idx);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, r_ic_idx_val),
                                     MIR_new_mem_op(c->ctx, MIR_T_I32,
                                                    (MIR_disp_t)offsetof(sv_ic_entry_t, cached_index), r_ic, 0, 1)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_OR,
                                     MIR_new_reg_op(c->ctx, dst),
                                     MIR_new_uint_op(c->ctx, js_false),
                                     MIR_new_reg_op(c->ctx, r_ic_idx_val)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, no_err)));

        MIR_append_insn(c->ctx, c->jit_func, slow);
      }

      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 10,
                                        MIR_new_ref_op(c->ctx, c->call_is_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_call_is_proto),
                                        MIR_new_reg_op(c->ctx, dst),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, this_obj),
                                        MIR_new_reg_op(c->ctx, fn),
                                        MIR_new_reg_op(c->ctx, arg),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->func),
                                        MIR_new_int_op(c->ctx, (int64_t)c->bc_off)));
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
                      MIR_new_insn(c->ctx, MIR_URSH,
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_reg_op(c->ctx, dst),
                                   MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BNE,
                                   MIR_new_label_op(c->ctx, no_err),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx, JIT_ERR_TAG)));
      if (c->jit_try_depth > 0) {
        jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->vs.regs[h->saved_sp]),
                                     MIR_new_reg_op(c->ctx, dst)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, h->catch_label)));
      } else {
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, dst));
      }
      MIR_append_insn(c->ctx, c->jit_func, no_err);
      break;
    }

    default:
      __builtin_unreachable();
  }
}
