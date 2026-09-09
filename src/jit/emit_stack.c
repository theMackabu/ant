#include "compile.h"

void jit_emit_stack(jit_compile_t *c) {
  switch (c->op) {
    case OP_POP:
      vstack_pop(&c->vs);
      break;

    case OP_DUP: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      jit_value_info_t info = vstack_value_info(&c->vs, c->vs.sp - 1);
      MIR_reg_t top = vstack_top(&c->vs);
      MIR_reg_t dst = vstack_push(&c->vs);
      vstack_set_value_info(&c->vs, c->vs.sp - 1, info);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, dst),
                                   MIR_new_reg_op(c->ctx, top)));
      break;
    }

    case OP_DUP2: {
      if (c->vs.sp < 2) {
        c->ok = false;
        break;
      }
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      jit_value_info_t ia = vstack_value_info(&c->vs, c->vs.sp - 2);
      jit_value_info_t ib = vstack_value_info(&c->vs, c->vs.sp - 1);
      MIR_reg_t ra = c->vs.regs[c->vs.sp - 2];
      MIR_reg_t rb = c->vs.regs[c->vs.sp - 1];
      MIR_reg_t da = vstack_push(&c->vs);
      MIR_reg_t db = vstack_push(&c->vs);
      vstack_set_value_info(&c->vs, c->vs.sp - 2, ia);
      vstack_set_value_info(&c->vs, c->vs.sp - 1, ib);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, da),
                                   MIR_new_reg_op(c->ctx, ra)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, db),
                                   MIR_new_reg_op(c->ctx, rb)));
      break;
    }

    case OP_NIP: {
      if (c->vs.sp < 2) {
        c->ok = false;
        break;
      }
      int top_idx = c->vs.sp - 1;
      int dst_idx = c->vs.sp - 2;
      vstack_ensure_boxed(&c->vs, top_idx, c->ctx, c->jit_func, c->r_d_slot);
      jit_value_info_t info = vstack_value_info(&c->vs, top_idx);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->vs.regs[dst_idx]),
                                   MIR_new_reg_op(c->ctx, c->vs.regs[top_idx])));
      c->vs.sp--;
      c->vs.slot_type[dst_idx] = SLOT_BOXED;
      vstack_set_value_info(&c->vs, dst_idx, info);
      break;
    }

    case OP_INSERT2: {
      if (c->vs.sp < 2) {
        c->ok = false;
        break;
      }
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      jit_value_info_t ia = vstack_value_info(&c->vs, c->vs.sp - 1);
      jit_value_info_t io = vstack_value_info(&c->vs, c->vs.sp - 2);
      MIR_reg_t r_a = c->vs.regs[c->vs.sp - 1];
      MIR_reg_t r_obj = c->vs.regs[c->vs.sp - 2];
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->r_tmp),
                                   MIR_new_reg_op(c->ctx, r_a)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->vs.regs[c->vs.sp - 1]),
                                   MIR_new_reg_op(c->ctx, r_obj)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->vs.regs[c->vs.sp - 2]),
                                   MIR_new_reg_op(c->ctx, c->r_tmp)));
      MIR_reg_t dup = vstack_push(&c->vs);
      vstack_set_value_info(&c->vs, c->vs.sp - 3, ia);
      vstack_set_value_info(&c->vs, c->vs.sp - 2, io);
      vstack_set_value_info(&c->vs, c->vs.sp - 1, ia);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, dup),
                                   MIR_new_reg_op(c->ctx, c->r_tmp)));
      break;
    }

    case OP_INSERT3: {
      if (c->vs.sp < 3) {
        c->ok = false;
        break;
      }
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      bool integer_key = c->vs.slot_type[c->vs.sp - 2] == SLOT_I32;
      if (!integer_key)
        vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 3, c->ctx, c->jit_func, c->r_d_slot);
      jit_value_info_t ia = vstack_value_info(&c->vs, c->vs.sp - 1);
      jit_value_info_t iprop = vstack_value_info(&c->vs, c->vs.sp - 2);
      jit_value_info_t io = vstack_value_info(&c->vs, c->vs.sp - 3);
      MIR_reg_t r_a = c->vs.regs[c->vs.sp - 1];
      MIR_reg_t r_prop = c->vs.regs[c->vs.sp - 2];
      MIR_reg_t r_obj = c->vs.regs[c->vs.sp - 3];
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->r_tmp),
                                   MIR_new_reg_op(c->ctx, r_a)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->vs.regs[c->vs.sp - 1]),
                                   MIR_new_reg_op(c->ctx, r_prop)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->vs.regs[c->vs.sp - 2]),
                                   MIR_new_reg_op(c->ctx, r_obj)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->vs.regs[c->vs.sp - 3]),
                                   MIR_new_reg_op(c->ctx, c->r_tmp)));
      MIR_reg_t dup = vstack_push(&c->vs);
      c->vs.slot_type[c->vs.sp - 3] = SLOT_BOXED;
      c->vs.slot_type[c->vs.sp - 2] = integer_key ? SLOT_I32 : SLOT_BOXED;
      vstack_set_value_info(&c->vs, c->vs.sp - 4, ia);
      vstack_set_value_info(&c->vs, c->vs.sp - 3, io);
      vstack_set_value_info(&c->vs, c->vs.sp - 2, iprop);
      vstack_set_value_info(&c->vs, c->vs.sp - 1, ia);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, dup),
                                   MIR_new_reg_op(c->ctx, c->r_tmp)));
      break;
    }

    case OP_SWAP: {
      if (c->vs.sp < 2) {
        c->ok = false;
        break;
      }
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      jit_value_info_t ia = vstack_value_info(&c->vs, c->vs.sp - 2);
      jit_value_info_t ib = vstack_value_info(&c->vs, c->vs.sp - 1);
      MIR_reg_t ra = c->vs.regs[c->vs.sp - 2];
      MIR_reg_t rb = c->vs.regs[c->vs.sp - 1];
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->r_tmp),
                                   MIR_new_reg_op(c->ctx, ra)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, ra),
                                   MIR_new_reg_op(c->ctx, rb)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, rb),
                                   MIR_new_reg_op(c->ctx, c->r_tmp)));
      vstack_set_value_info(&c->vs, c->vs.sp - 2, ib);
      vstack_set_value_info(&c->vs, c->vs.sp - 1, ia);
      break;
    }

    case OP_SWAP_UNDER: {
      if (c->vs.sp < 3) {
        c->ok = false;
        break;
      }
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 3, c->ctx, c->jit_func, c->r_d_slot);
      jit_value_info_t ia = vstack_value_info(&c->vs, c->vs.sp - 3);
      jit_value_info_t ib = vstack_value_info(&c->vs, c->vs.sp - 2);
      MIR_reg_t ra = c->vs.regs[c->vs.sp - 3];
      MIR_reg_t rb = c->vs.regs[c->vs.sp - 2];
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->r_tmp),
                                   MIR_new_reg_op(c->ctx, ra)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, ra),
                                   MIR_new_reg_op(c->ctx, rb)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, rb),
                                   MIR_new_reg_op(c->ctx, c->r_tmp)));
      vstack_set_value_info(&c->vs, c->vs.sp - 3, ib);
      vstack_set_value_info(&c->vs, c->vs.sp - 2, ia);
      break;
    }

    case OP_ROT4_UNDER: {
      if (c->vs.sp < 4) {
        c->ok = false;
        break;
      }
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 3, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 4, c->ctx, c->jit_func, c->r_d_slot);
      jit_value_info_t ia = vstack_value_info(&c->vs, c->vs.sp - 4);
      jit_value_info_t ib = vstack_value_info(&c->vs, c->vs.sp - 3);
      jit_value_info_t ic = vstack_value_info(&c->vs, c->vs.sp - 2);
      MIR_reg_t ra = c->vs.regs[c->vs.sp - 4];
      MIR_reg_t rb = c->vs.regs[c->vs.sp - 3];
      MIR_reg_t rc = c->vs.regs[c->vs.sp - 2];
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->r_tmp),
                                   MIR_new_reg_op(c->ctx, ra)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, ra),
                                   MIR_new_reg_op(c->ctx, rc)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, rc),
                                   MIR_new_reg_op(c->ctx, rb)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, rb),
                                   MIR_new_reg_op(c->ctx, c->r_tmp)));
      vstack_set_value_info(&c->vs, c->vs.sp - 4, ic);
      vstack_set_value_info(&c->vs, c->vs.sp - 3, ia);
      vstack_set_value_info(&c->vs, c->vs.sp - 2, ib);
      break;
    }

    case OP_ROT3L: {
      if (c->vs.sp < 3) {
        c->ok = false;
        break;
      }
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 2, c->ctx, c->jit_func, c->r_d_slot);
      vstack_ensure_boxed(&c->vs, c->vs.sp - 3, c->ctx, c->jit_func, c->r_d_slot);
      jit_value_info_t ix = vstack_value_info(&c->vs, c->vs.sp - 3);
      jit_value_info_t ia = vstack_value_info(&c->vs, c->vs.sp - 2);
      jit_value_info_t ib = vstack_value_info(&c->vs, c->vs.sp - 1);
      MIR_reg_t rx = c->vs.regs[c->vs.sp - 3];
      MIR_reg_t ra = c->vs.regs[c->vs.sp - 2];
      MIR_reg_t rb = c->vs.regs[c->vs.sp - 1];
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->r_tmp),
                                   MIR_new_reg_op(c->ctx, rx)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, rx),
                                   MIR_new_reg_op(c->ctx, ra)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, ra),
                                   MIR_new_reg_op(c->ctx, rb)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, rb),
                                   MIR_new_reg_op(c->ctx, c->r_tmp)));
      vstack_set_value_info(&c->vs, c->vs.sp - 3, ia);
      vstack_set_value_info(&c->vs, c->vs.sp - 2, ib);
      vstack_set_value_info(&c->vs, c->vs.sp - 1, ix);
      break;
    }

    default:
      __builtin_unreachable();
  }
}
