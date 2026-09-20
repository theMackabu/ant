#include "compile.h"
#include "../silver/ops/iteration.h"

static void jit_emit_dense_array_next(jit_compile_t *c, MIR_label_t done) {
  int hint = sv_get_u8(c->ip + 1);
  if (hint && hint != SV_ITER_ARRAY) return;

  MIR_context_t ctx = c->ctx;
  MIR_item_t fn = c->jit_func;
  MIR_label_t slow = MIR_new_label(ctx);
  int site = mir_next_reg_site(&c->reg_site_n);
  const char *names[] = {"tag", "array", "bits", "index", "value", "counter"};
  MIR_reg_t regs[6];
  for (int i = 0; i < 6; i++) {
    char name[48];
    snprintf(name, sizeof(name), "iter_array_%s_%d", names[i], site);
    regs[i] = MIR_new_func_reg(ctx, fn->u.func, i == 5 ? MIR_T_D : MIR_T_I64, name);
  }
  MIR_reg_t tag = regs[0], array = regs[1], bits = regs[2];
  MIR_reg_t index = regs[3], value = regs[4], counter = regs[5];
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, tag),
      MIR_new_mem_op(ctx, MIR_JSVAL, 2 * sizeof(ant_value_t), c->r_iter_buf, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, tag), MIR_new_uint_op(ctx, tov(SV_ITER_ARRAY))));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, array),
      MIR_new_mem_op(ctx, MIR_JSVAL, 0, c->r_iter_buf, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, bits),
      MIR_new_mem_op(ctx, MIR_JSVAL, sizeof(ant_value_t), c->r_iter_buf, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_UBGT, MIR_new_label_op(ctx, slow),
      MIR_new_reg_op(ctx, bits), MIR_new_uint_op(ctx, tov((double)INT32_MAX - 1))));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_DMOV, MIR_new_reg_op(ctx, counter),
      MIR_new_mem_op(ctx, MIR_T_D, sizeof(ant_value_t), c->r_iter_buf, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_D2I,
      MIR_new_reg_op(ctx, index), MIR_new_reg_op(ctx, counter)));
  mir_emit_dense_element_guard(ctx, fn, array, index, value, JIT_ELEMENT_READ, slow, site);

  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_DADD, MIR_new_reg_op(ctx, counter),
      MIR_new_reg_op(ctx, counter), MIR_new_double_op(ctx, 1.0)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_DMOV,
      MIR_new_mem_op(ctx, MIR_T_D, sizeof(ant_value_t), c->r_iter_buf, 0, 1),
      MIR_new_reg_op(ctx, counter)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_mem_op(ctx, MIR_JSVAL, 3 * sizeof(ant_value_t), c->r_iter_buf, 0, 1),
      MIR_new_reg_op(ctx, value)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_mem_op(ctx, MIR_JSVAL, 4 * sizeof(ant_value_t), c->r_iter_buf, 0, 1),
      MIR_new_uint_op(ctx, js_false)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
  MIR_append_insn(ctx, fn, slow);
}

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
      MIR_label_t next_done = MIR_new_label(c->ctx);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_ADD,
                                   MIR_new_reg_op(c->ctx, c->r_iter_buf),
                                   MIR_new_reg_op(c->ctx, c->r_iter_roots),
                                   MIR_new_int_op(c->ctx,
                                                  (int64_t)iter_base * (int64_t)sizeof(ant_value_t))));
      jit_emit_dense_array_next(c, next_done);
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
      MIR_append_insn(c->ctx, c->jit_func, next_done);
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
                      MIR_new_call_insn(c->ctx, 7,
                                        MIR_new_ref_op(c->ctx, c->destructure_close_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_dclose),
                                        MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, c->r_iter_buf),
                                        MIR_new_int_op(c->ctx, sv_get_u8(c->ip + 1))));
      jit_emit_throw_if_error(c, c->r_err_tmp);
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
                      MIR_new_call_insn(c->ctx, 7,
                                        MIR_new_ref_op(c->ctx, c->destructure_close_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_dclose),
                                        MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_reg_op(c->ctx, c->r_args_buf),
                                        MIR_new_int_op(c->ctx, sv_get_u8(c->ip + 1))));
      jit_emit_throw_if_error(c, c->r_err_tmp);
      vstack_pop(&c->vs);
      vstack_pop(&c->vs);
      vstack_pop(&c->vs);
      break;
    }

    default:
      __builtin_unreachable();
  }
}
