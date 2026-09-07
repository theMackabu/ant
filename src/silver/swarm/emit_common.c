#include "compile.h"

void jit_emit_exit_ret(jit_compile_t *c, MIR_op_t ret_op) {
  mir_emit_exit_ret(c->ctx, c->jit_func,
                    c->close_upval_proto, c->imp_close_upval,
                    c->adopt_open_upvalues_proto, c->imp_adopt_open_upvalues,
                    c->r_vm, c->r_slotbuf, c->r_lbuf, c->r_jit_open_upvalues,
                    c->has_captured_slots, c->captured_params, c->param_count,
                    c->has_captures, c->captured_locals, c->n_locals, ret_op);
}

void jit_emit_throw_if_error(jit_compile_t *c, MIR_reg_t value_reg) {
  MIR_label_t no_error = MIR_new_label(c->ctx);
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_URSH,
                               MIR_new_reg_op(c->ctx, c->r_bool),
                               MIR_new_reg_op(c->ctx, value_reg),
                               MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BNE,
                               MIR_new_label_op(c->ctx, no_error),
                               MIR_new_reg_op(c->ctx, c->r_bool),
                               MIR_new_uint_op(c->ctx, JIT_ERR_TAG)));
  if (c->jit_try_depth > 0) {
    jit_try_entry_t *handler = &c->jit_try_stack[c->jit_try_depth - 1];
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV,
                                 MIR_new_reg_op(c->ctx, c->vs.regs[handler->saved_sp]),
                                 MIR_new_reg_op(c->ctx, value_reg)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_JMP,
                                 MIR_new_label_op(c->ctx, handler->catch_label)));
  } else {
    jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, value_reg));
  }
  MIR_append_insn(c->ctx, c->jit_func, no_error);
}
