#include "compile.h"

void jit_emit_iteration(jit_compile_t *c) {
  switch (c->op) {
    case OP_FOR_OF:
    case OP_DESTRUCTURE_INIT: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t iterable = vstack_pop(&c->vs);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 7,
                                        MIR_new_ref_op(c->ctx, c->for_of_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_for_of),
                                        MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, iterable),
                                        MIR_new_reg_op(c->ctx, c->r_args_buf)));
      jit_emit_throw_if_error(c, c->r_err_tmp);
      for (int i = 0; i < 3; i++) {
        MIR_reg_t dst = vstack_push(&c->vs);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, dst),
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)(i * (int)sizeof(ant_value_t)), c->r_args_buf, 0, 1)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)((c->vs.sp - 1) * (int)sizeof(ant_value_t)), c->r_iter_roots, 0, 1),
                                     MIR_new_reg_op(c->ctx, dst)));
      }
      break;
    }

    case OP_ITER_NEXT: {
      int iter_base = c->vs.sp - 3;
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_ADD,
                                   MIR_new_reg_op(c->ctx, c->r_iter_buf),
                                   MIR_new_reg_op(c->ctx, c->r_iter_roots),
                                   MIR_new_int_op(c->ctx,
                                                  (int64_t)iter_base * (int64_t)sizeof(ant_value_t))));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 7,
                                        MIR_new_ref_op(c->ctx, c->iter_next_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_iter_next),
                                        MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, c->r_iter_buf),
                                        MIR_new_int_op(c->ctx, (int64_t)sv_get_u8(c->ip + 1))));
      jit_emit_throw_if_error(c, c->r_err_tmp);
      vstack_pop(&c->vs);
      vstack_pop(&c->vs);
      vstack_pop(&c->vs);
      for (int i = 0; i < 5; i++) {
        MIR_reg_t dst = vstack_push(&c->vs);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, dst),
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)(i * (int)sizeof(ant_value_t)), c->r_iter_buf, 0, 1)));
      }
      if (c->vs.known_bool) c->vs.known_bool[c->vs.sp - 1] = true;
      break;
    }

    case OP_ITER_CLOSE: {
      int iter_base = c->vs.sp - 3;
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_ADD,
                                   MIR_new_reg_op(c->ctx, c->r_iter_buf),
                                   MIR_new_reg_op(c->ctx, c->r_iter_roots),
                                   MIR_new_int_op(c->ctx,
                                                  (int64_t)iter_base * (int64_t)sizeof(ant_value_t))));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 5,
                                        MIR_new_ref_op(c->ctx, c->destructure_close_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_dclose),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, c->r_iter_buf)));
      vstack_pop(&c->vs);
      vstack_pop(&c->vs);
      vstack_pop(&c->vs);
      break;
    }

    case OP_DESTRUCTURE_NEXT: {
      int iter_base = c->vs.sp - 3;
      for (int i = 0; i < 3; i++) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)(i * (int)sizeof(ant_value_t)), c->r_args_buf, 0, 1),
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)((iter_base + i) * (int)sizeof(ant_value_t)), c->r_iter_roots, 0, 1)));
      }
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 6,
                                        MIR_new_ref_op(c->ctx, c->destructure_next_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_dnext),
                                        MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, c->r_args_buf)));
      jit_emit_throw_if_error(c, c->r_err_tmp);
      vstack_pop(&c->vs);
      vstack_pop(&c->vs);
      vstack_pop(&c->vs);
      for (int i = 0; i < 4; i++) {
        MIR_reg_t dst = vstack_push(&c->vs);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, dst),
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)(i * (int)sizeof(ant_value_t)), c->r_args_buf, 0, 1)));
        if (i < 3) {
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_insn(c->ctx, MIR_MOV,
                                       MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                      (MIR_disp_t)((c->vs.sp - 1) * (int)sizeof(ant_value_t)), c->r_iter_roots, 0, 1),
                                       MIR_new_reg_op(c->ctx, dst)));
        }
      }
      break;
    }

    case OP_DESTRUCTURE_CLOSE: {
      int iter_base = c->vs.sp - 3;
      for (int i = 0; i < 3; i++) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)(i * (int)sizeof(ant_value_t)), c->r_args_buf, 0, 1),
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)((iter_base + i) * (int)sizeof(ant_value_t)), c->r_iter_roots, 0, 1)));
      }
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 5,
                                        MIR_new_ref_op(c->ctx, c->destructure_close_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_dclose),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, c->r_args_buf)));
      vstack_pop(&c->vs);
      vstack_pop(&c->vs);
      vstack_pop(&c->vs);
      break;
    }

    default:
      __builtin_unreachable();
  }
}
