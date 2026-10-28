#include "compile.h"

static void jit_emit_promote_check(jit_compile_t *c, MIR_label_t loop, int64_t every) {
  MIR_reg_t r_due = c->r_tmp2;
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_SUB, MIR_new_reg_op(c->ctx, c->r_promote),
                               MIR_new_reg_op(c->ctx, c->r_promote), MIR_new_int_op(c->ctx, 1)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BNE, MIR_new_label_op(c->ctx, loop),
                               MIR_new_reg_op(c->ctx, c->r_promote), MIR_new_int_op(c->ctx, 0)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_call_insn(c->ctx, 4,
                                    MIR_new_ref_op(c->ctx, c->promote_due_proto),
                                    MIR_new_ref_op(c->ctx, c->imp_promote_due),
                                    MIR_new_reg_op(c->ctx, r_due),
                                    MIR_new_ref_op(c->ctx, c->cold_ns_item)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, c->r_promote),
                               MIR_new_int_op(c->ctx, every)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BNE, MIR_new_label_op(c->ctx, loop),
                               MIR_new_reg_op(c->ctx, r_due), MIR_new_int_op(c->ctx, 0)));
  mir_emit_bailout_jump_typed(c->ctx, c->jit_func, c->bc_off, c->vs.sp, &c->promote_ctx,
                              -1, SLOT_BOXED, -1, SLOT_BOXED);
  c->needs_promote = true;

}

static void jit_emit_fused_compare_branch(jit_compile_t *c, bool if_false, MIR_label_t target) {
  MIR_insn_code_t branch, inverse = MIR_INSN_BOUND;
  switch (c->cmp_code) {
    case MIR_LT: branch = MIR_BLT; inverse = MIR_BGE; break;
    case MIR_LE: branch = MIR_BLE; inverse = MIR_BGT; break;
    case MIR_GT: branch = MIR_BGT; inverse = MIR_BLE; break;
    case MIR_GE: branch = MIR_BGE; inverse = MIR_BLT; break;
    case MIR_DLT: branch = MIR_DBLT; break;
    case MIR_DLE: branch = MIR_DBLE; break;
    case MIR_DGT: branch = MIR_DBGT; break;
    case MIR_DGE: branch = MIR_DBGE; break;
    default:
      // anything else keeps testing the bit
      if (if_false) {
        MIR_label_t taken = MIR_new_label(c->ctx);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BT, MIR_new_label_op(c->ctx, taken), MIR_new_reg_op(c->ctx, c->cmp_bit)));
        MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, target)));
        MIR_append_insn(c->ctx, c->jit_func, taken);
      } else {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BT, MIR_new_label_op(c->ctx, target), MIR_new_reg_op(c->ctx, c->cmp_bit)));
      }
      return;
  }

  MIR_remove_insn(c->ctx, c->jit_func, c->cmp_insn);
  MIR_remove_insn(c->ctx, c->jit_func, c->cmp_box);
#define BRANCH(code, label) \
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, code, MIR_new_label_op(c->ctx, label), \
      MIR_new_reg_op(c->ctx, c->cmp_left), MIR_new_reg_op(c->ctx, c->cmp_right)))
  if (!if_false) BRANCH(branch, target);
  else if (inverse != MIR_INSN_BOUND) BRANCH(inverse, target);
  else {
    MIR_label_t taken = MIR_new_label(c->ctx);
    BRANCH(branch, taken);
    MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, target)));
    MIR_append_insn(c->ctx, c->jit_func, taken);
  }
#undef BRANCH
}

void jit_emit_await_exit(jit_compile_t *c, int n_pop, int n_push) {
  vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
  if (c->op == OP_AWAIT_RETURN && c->r_async_direct && c->vs.sp > 0) {
    MIR_label_t framed = MIR_new_label(c->ctx);
    char name[32];
    snprintf(name, sizeof(name), "await_return_res_%d", mir_next_reg_site(&c->reg_site_n));
    MIR_reg_t r_res = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, name);
    MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_BEQ,
      MIR_new_label_op(c->ctx, framed), MIR_new_reg_op(c->ctx, c->r_async_direct), MIR_new_int_op(c->ctx, 0)));
    MIR_append_insn(c->ctx, c->jit_func, MIR_new_call_insn(c->ctx, 7,
      MIR_new_ref_op(c->ctx, c->await_return_proto),
      MIR_new_ref_op(c->ctx, c->imp_await_return_direct),
      MIR_new_reg_op(c->ctx, r_res),
      MIR_new_reg_op(c->ctx, c->r_vm),
      MIR_new_reg_op(c->ctx, c->r_js),
      MIR_new_reg_op(c->ctx, c->vs.regs[c->vs.sp - 1]),
      MIR_new_reg_op(c->ctx, c->r_async_next_op)));
    jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, r_res));
    MIR_append_insn(c->ctx, c->jit_func, framed);
  }
  int site = c->await_park ? sv_jit_await_site_index(sv_func_jit_await_map(c->func), (uint32_t)c->bc_off) : -1;
  MIR_label_t nested = site >= 0 ? MIR_new_label(c->ctx) : NULL;
  if (site >= 0) {
    MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_BEQ,
      MIR_new_label_op(c->ctx, nested), MIR_new_reg_op(c->ctx, c->r_frame_lp), MIR_new_int_op(c->ctx, 0)));
    for (int i = 0; i < c->vs.sp; i++)
      MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV,
        MIR_new_mem_op(c->ctx, MIR_JSVAL, (MIR_disp_t)((c->n_locals + i) * (int)sizeof(ant_value_t)), c->r_frame_lp, 0, 1),
        MIR_new_reg_op(c->ctx, c->vs.regs[i])));
    const sv_jit_await_site_t *info = &sv_func_jit_await_map(c->func)->sites[site];
    const uint16_t *live = sv_func_jit_await_map(c->func)->live + info->live_start;
    for (int j = 0; j < info->live_count; j++) {
      int l = live[j];
      if (l >= c->n_locals) continue;
      MIR_reg_t value = c->local_regs[l];
      if (c->dnum_locals && c->dnum_locals[l]) {
        value = c->r_park_value;
        mir_d_to_i64(c->ctx, c->jit_func, value, c->local_d_regs[l], c->r_d_slot);
      }
      MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV,
        MIR_new_mem_op(c->ctx, MIR_JSVAL, (MIR_disp_t)(l * (int)sizeof(ant_value_t)), c->r_frame_lp, 0, 1),
        MIR_new_reg_op(c->ctx, value)));
    }
    MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV,
      MIR_new_reg_op(c->ctx, c->r_park_site), MIR_new_int_op(c->ctx, site)));
    MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV,
      MIR_new_reg_op(c->ctx, c->r_park_sp), MIR_new_int_op(c->ctx, c->vs.sp)));
    MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_JMP,
      MIR_new_label_op(c->ctx, c->await_park_block)));
    MIR_append_insn(c->ctx, c->jit_func, nested);
  }
  mir_emit_bailout_boxed_exit(
    c->ctx, c->jit_func, (int64_t)c->bc_off | SV_JIT_RESUME_AWAIT, c->vs.sp, &c->bailout_ctx);
  for (int i = 0; i < n_pop; i++) vstack_pop(&c->vs);
  for (int i = 0; i < n_push; i++) vstack_push_const(&c->vs, mkval(kTypeUndefined, 0));
}

uint8_t jit_awaited_async_call_op(jit_compile_t *c, sv_func_t *known) {
  if (!c->func->is_async || c->ip + c->sz >= c->end) return 0;
  sv_op_t next = (sv_op_t)c->ip[c->sz];
  if (next != OP_AWAIT && next != OP_AWAIT_RETURN) return 0;
  sv_func_t *target = known ? known : sv_tfb_get_call_target(c->func, c->bc_off);
  return target && target->is_async && !target->is_generator ? (uint8_t)next : 0;
}

void jit_emit_awaited_call(jit_compile_t *c, int call_argc, bool method, uint8_t next_op) {
  vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
  for (int i = call_argc - 1; i >= 0; i--) {
    MIR_reg_t arg = vstack_pop(&c->vs);
    MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV,
      MIR_new_mem_op(c->ctx, MIR_JSVAL, (MIR_disp_t)(i * (int)sizeof(ant_value_t)), c->r_args_buf, 0, 1),
      MIR_new_reg_op(c->ctx, arg)));
  }
  MIR_reg_t r_func = vstack_pop(&c->vs);
  MIR_reg_t r_this;
  if (method) r_this = vstack_pop(&c->vs);
  else {
    char name[32];
    snprintf(name, sizeof(name), "awaited_this%d", c->call_n++);
    r_this = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, name);
    mir_load_imm(c->ctx, c->jit_func, r_this, mkval(kTypeUndefined, 0));
  }
  MIR_reg_t r_res = vstack_push(&c->vs);
  MIR_append_insn(c->ctx, c->jit_func, MIR_new_call_insn(c->ctx, 10,
    MIR_new_ref_op(c->ctx, c->call_awaited_proto),
    MIR_new_ref_op(c->ctx, c->imp_call_awaited),
    MIR_new_reg_op(c->ctx, r_res),
    MIR_new_reg_op(c->ctx, c->r_vm),
    MIR_new_reg_op(c->ctx, c->r_js),
    MIR_new_reg_op(c->ctx, r_func),
    MIR_new_reg_op(c->ctx, r_this),
    MIR_new_reg_op(c->ctx, c->r_args_buf),
    MIR_new_int_op(c->ctx, call_argc),
    MIR_new_int_op(c->ctx, next_op)));
  if (c->has_captures) {
    for (int i = 0; i < c->n_locals; i++)
      if (c->captured_locals[i])
        MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV,
          MIR_new_reg_op(c->ctx, c->local_regs[i]),
          MIR_new_mem_op(c->ctx, MIR_T_I64, (MIR_disp_t)(i * (int)sizeof(ant_value_t)), c->r_lbuf, 0, 1)));
  }
  jit_emit_throw_if_error(c, r_res);
}

static const jit_try_entry_t *jit_innermost_finally(const jit_compile_t *c) {
  for (int i = c->jit_try_depth - 1; i >= 0; i--)
    if (c->jit_try_stack[i].finally_label) return &c->jit_try_stack[i];
  return NULL;
}

static void jit_emit_finally_route(jit_compile_t *c, const jit_try_entry_t *h,
                                   sv_completion_kind_t kind, MIR_op_t operand) {
  vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
  MIR_reg_t slot = c->vs.regs[h->saved_sp];
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, c->r_finally_kind),
                               MIR_new_int_op(c->ctx, kind)));
  if (kind == SV_COMPLETION_JUMP) {
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, c->r_finally_target), operand));
    operand = MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0));
  } else {
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, c->r_finally_value), operand));
  }
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, slot), operand));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, h->finally_label)));
}

static void jit_emit_close_frame_upvalues(jit_compile_t *c) {
  if (c->has_captured_slots)
    mir_emit_close_marked_slots(c->ctx, c->jit_func,
                                c->close_upval_proto, c->imp_close_upval,
                                c->r_vm, c->r_slotbuf, c->r_jit_open_upvalues, c->captured_params, 0, c->param_count,
                                mir_next_reg_site(&c->reg_site_n));
  if (c->has_captures)
    mir_emit_close_marked_slots(c->ctx, c->jit_func,
                                c->close_upval_proto, c->imp_close_upval,
                                c->r_vm, c->r_lbuf, c->r_jit_open_upvalues, c->captured_locals, 0, c->n_locals,
                                mir_next_reg_site(&c->reg_site_n));
}

static void jit_emit_return_value(jit_compile_t *c, MIR_reg_t ret_val) {
  const jit_try_entry_t *fin = jit_innermost_finally(c);
  if (fin) {
    jit_emit_finally_route(c, fin, SV_COMPLETION_RETURN, MIR_new_reg_op(c->ctx, ret_val));
    return;
  }
  jit_emit_close_frame_upvalues(c);
  if (c->func->is_derived_ctor) {
    MIR_label_t dctor_replace = MIR_new_label(c->ctx);
    MIR_label_t dctor_keep = MIR_new_label(c->ctx);
    char rn_dt[32];
    snprintf(rn_dt, sizeof(rn_dt), "dctor%d", c->upval_n++);
    MIR_reg_t r_dt = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, rn_dt);
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_UBLE,
                                 MIR_new_label_op(c->ctx, dctor_replace),
                                 MIR_new_reg_op(c->ctx, ret_val),
                                 MIR_new_uint_op(c->ctx, NANBOX_PREFIX)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_URSH,
                                 MIR_new_reg_op(c->ctx, r_dt),
                                 MIR_new_reg_op(c->ctx, ret_val),
                                 MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_AND,
                                 MIR_new_reg_op(c->ctx, r_dt),
                                 MIR_new_reg_op(c->ctx, r_dt),
                                 MIR_new_int_op(c->ctx, NANBOX_TYPE_MASK)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_LSH,
                                 MIR_new_reg_op(c->ctx, r_dt),
                                 MIR_new_int_op(c->ctx, 1),
                                 MIR_new_reg_op(c->ctx, r_dt)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_AND,
                                 MIR_new_reg_op(c->ctx, r_dt),
                                 MIR_new_reg_op(c->ctx, r_dt),
                                 MIR_new_int_op(c->ctx, (int64_t)(T_OBJECT_MASK | (1u << kTypeBuiltin)))));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_BT,
                                 MIR_new_label_op(c->ctx, dctor_keep),
                                 MIR_new_reg_op(c->ctx, r_dt)));
    MIR_label_t dctor_undef = MIR_new_label(c->ctx);
    MIR_append_insn(c->ctx, c->jit_func, dctor_replace);
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_BEQ,
                                 MIR_new_label_op(c->ctx, dctor_undef),
                                 MIR_new_reg_op(c->ctx, ret_val),
                                 MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0))));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_call_insn(c->ctx, 8,
                                      MIR_new_ref_op(c->ctx, c->throw_error_proto),
                                      MIR_new_ref_op(c->ctx, c->imp_throw_error),
                                      MIR_new_reg_op(c->ctx, ret_val),
                                      MIR_new_reg_op(c->ctx, c->r_vm),
                                      MIR_new_reg_op(c->ctx, c->r_js),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)SV_DERIVED_CTOR_RETURN_ERROR),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(sizeof(SV_DERIVED_CTOR_RETURN_ERROR) - 1)),
                                      MIR_new_int_op(c->ctx, (int64_t)JS_ERR_TYPE)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, dctor_keep)));
    MIR_append_insn(c->ctx, c->jit_func, dctor_undef);
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV,
                                 MIR_new_reg_op(c->ctx, ret_val),
                                 MIR_new_reg_op(c->ctx, c->r_this_curr)));
    MIR_append_insn(c->ctx, c->jit_func, dctor_keep);
  }
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_ret_insn(c->ctx, 1, MIR_new_reg_op(c->ctx, ret_val)));
}

static void jit_emit_throw_value(jit_compile_t *c, MIR_reg_t thrown) {
  if (c->jit_try_depth > 0) {
    jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV,
                                 MIR_new_reg_op(c->ctx, c->vs.regs[h->saved_sp]),
                                 MIR_new_reg_op(c->ctx, thrown)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_JMP,
                                 MIR_new_label_op(c->ctx, h->catch_label)));
    return;
  }
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_call_insn(c->ctx, 6,
                                    MIR_new_ref_op(c->ctx, c->helper1_proto),
                                    MIR_new_ref_op(c->ctx, c->imp_throw),
                                    MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                    MIR_new_reg_op(c->ctx, c->r_vm),
                                    MIR_new_reg_op(c->ctx, c->r_js),
                                    MIR_new_reg_op(c->ctx, thrown)));
  jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, c->r_err_tmp));
}

static MIR_op_t jit_finally_slot(jit_compile_t *c, int index, size_t field) {
  return MIR_new_mem_op(c->ctx, MIR_T_I64,
                        (MIR_disp_t)((size_t)index * sizeof(sv_jit_finally_t) + field),
                        c->r_finally_area, 0, 1);
}

static int jit_finally_of_region(const sv_func_t *func, int push) {
  int handler = push + sv_op_size[OP_TRY_PUSH_FINALLY] + sv_get_i32(func->code + push + 1);
  int jmp = handler + sv_op_size[OP_POP];
  if (handler < 0 || jmp + sv_op_size[OP_JMP] > func->code_len) return -1;
  if (func->code[handler] != OP_POP || func->code[jmp] != OP_JMP) return -1;
  int target = jmp + sv_op_size[OP_JMP] + sv_get_i32(func->code + jmp + 1);
  return target >= 0 && target < func->code_len && func->code[target] == OP_FINALLY ? target : -1;
}

static void jit_emit_finally_enter(jit_compile_t *c) {
  int index = sv_func_finally_index(c->func, c->bc_off);
  if (index < 0 || index >= c->finally_count) {
    c->ok = false;
    return;
  }
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, jit_finally_slot(c, index, offsetof(sv_jit_finally_t, kind)),
                               MIR_new_reg_op(c->ctx, c->r_finally_kind)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, jit_finally_slot(c, index, offsetof(sv_jit_finally_t, value)),
                               MIR_new_reg_op(c->ctx, c->r_finally_value)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, jit_finally_slot(c, index, offsetof(sv_jit_finally_t, target)),
                               MIR_new_reg_op(c->ctx, c->r_finally_target)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, c->r_finally_kind),
                               MIR_new_int_op(c->ctx, SV_COMPLETION_NONE)));
}

static void jit_emit_finally_ret(jit_compile_t *c) {
  int fin = c->try_at ? c->try_at[c->bc_off] : -1;
  if (fin < 0 || c->func->code[fin] != OP_FINALLY) {
    c->ok = false;
    return;
  }
  int index = sv_func_finally_index(c->func, fin);
  int after = fin + sv_op_size[OP_FINALLY] + sv_get_i32(c->func->code + fin + 1);
  vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
  if (index < 0 || index >= c->finally_count || c->depth[after] != c->vs.sp) {
    c->ok = false;
    return;
  }

  char name[32];
  int site = mir_next_reg_site(&c->reg_site_n);
  snprintf(name, sizeof(name), "finally_kind%d", site);
  MIR_reg_t kind = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, name);
  snprintf(name, sizeof(name), "finally_value%d", site);
  MIR_reg_t value = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, name);
  MIR_label_t on_throw = MIR_new_label(c->ctx);
  MIR_label_t on_return = MIR_new_label(c->ctx);

  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, kind),
                               jit_finally_slot(c, index, offsetof(sv_jit_finally_t, kind))));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BEQ, MIR_new_label_op(c->ctx, label_for_offset(c->ctx, &c->lm, after)),
                               MIR_new_reg_op(c->ctx, kind), MIR_new_int_op(c->ctx, SV_COMPLETION_NONE)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BEQ, MIR_new_label_op(c->ctx, on_throw),
                               MIR_new_reg_op(c->ctx, kind), MIR_new_int_op(c->ctx, SV_COMPLETION_THROW)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BEQ, MIR_new_label_op(c->ctx, on_return),
                               MIR_new_reg_op(c->ctx, kind), MIR_new_int_op(c->ctx, SV_COMPLETION_RETURN)));

  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, kind),
                               jit_finally_slot(c, index, offsetof(sv_jit_finally_t, target))));
  for (int off = 0; off < c->func->code_len; off += sv_op_size[c->func->code[off]]) {
    if (c->func->code[off] != OP_UNWIND_JMP) continue;
    int region = c->try_at[off];
    if (region < 0 || jit_finally_of_region(c->func, region) != fin) continue;
    int target = off + sv_op_size[OP_UNWIND_JMP] + sv_get_i32(c->func->code + off + 1);
    if (target < 0 || target >= c->func->code_len || c->depth[target] != c->vs.sp) {
      c->ok = false;
      return;
    }
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_BEQ, MIR_new_label_op(c->ctx, label_for_offset(c->ctx, &c->lm, target)),
                                 MIR_new_reg_op(c->ctx, kind), MIR_new_int_op(c->ctx, target)));
  }
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, label_for_offset(c->ctx, &c->lm, after))));

  MIR_append_insn(c->ctx, c->jit_func, on_throw);
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, value),
                               jit_finally_slot(c, index, offsetof(sv_jit_finally_t, value))));
  jit_emit_throw_value(c, value);

  MIR_append_insn(c->ctx, c->jit_func, on_return);
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, value),
                               jit_finally_slot(c, index, offsetof(sv_jit_finally_t, value))));
  jit_emit_return_value(c, value);
}

void jit_emit_finally_landings(jit_compile_t *c) {
  for (int i = 0; i < c->finally_landing_count; i++) {
    const jit_finally_landing_t *landing = &c->finally_landings[i];
    MIR_append_insn(c->ctx, c->jit_func, landing->landing);
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_call_insn(c->ctx, 6,
                                      MIR_new_ref_op(c->ctx, c->helper1_proto),
                                      MIR_new_ref_op(c->ctx, c->imp_catch_value),
                                      MIR_new_reg_op(c->ctx, c->r_finally_value),
                                      MIR_new_reg_op(c->ctx, c->r_vm),
                                      MIR_new_reg_op(c->ctx, c->r_js),
                                      MIR_new_reg_op(c->ctx, landing->value)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, c->r_finally_kind),
                                 MIR_new_int_op(c->ctx, SV_COMPLETION_THROW)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, landing->handler)));
  }
}

void jit_emit_control(jit_compile_t *c) {
  switch (c->op) {
    case OP_AWAIT:
      jit_emit_await_exit(c, 1, 1);
      break;
    case OP_AWAIT_RETURN:
      jit_emit_await_exit(c, 1, 0);
      break;
    case OP_AWAIT_ITER_NEXT:
      jit_emit_await_exit(c, 3, 5);
      break;
    case OP_JMP:
    case OP_JMP8: {
      vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
      bool short_op = (c->op == OP_JMP8);
      int target = c->bc_off + c->sz + (short_op ? (int8_t)sv_get_i8(c->ip + 1) : sv_get_i32(c->ip + 1));
      MIR_label_t lbl = label_for_offset(c->ctx, &c->lm, target);
      if (target <= c->bc_off) jit_emit_interrupt_poll(c);
      if (c->cold_tier && target <= c->bc_off && c->promote_sites && c->promote_sites[c->bc_off])
        jit_emit_promote_check(c, lbl, c->promote_sites[c->bc_off] == 2 ? 1 : JIT_COLD_PROMOTE_CHECK_EVERY);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, lbl)));
      break;
    }

    case OP_JMP_NOT_NULLISH: {
      vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t cond = vstack_top(&c->vs);
      int target = c->bc_off + c->sz + sv_get_i32(c->ip + 1);
      MIR_label_t lbl = label_for_offset(c->ctx, &c->lm, target);
      MIR_label_t done = MIR_new_label(c->ctx);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, done),
                                   MIR_new_reg_op(c->ctx, cond),
                                   MIR_new_uint_op(c->ctx, mkval(kTypeNull, 0))));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BNE,
                                   MIR_new_label_op(c->ctx, lbl),
                                   MIR_new_reg_op(c->ctx, cond),
                                   MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0))));
      MIR_append_insn(c->ctx, c->jit_func, done);
      break;
    }

    case OP_JMP_FALSE_PEEK:
    case OP_JMP_TRUE_PEEK:
    case OP_JMP_FALSE:
    case OP_JMP_FALSE8:
    case OP_JMP_TRUE:
    case OP_JMP_TRUE8: {
      bool cond_known_bool = c->vs.known_bool && c->vs.sp > 0 &&
                             c->vs.known_bool[c->vs.sp - 1];
      bool is_peek = (c->op == OP_JMP_FALSE_PEEK || c->op == OP_JMP_TRUE_PEEK);
      bool fused = !is_peek && c->cmp_value && c->cmp_end == c->bc_off &&
                   c->vs.sp > 0 && c->vs.regs[c->vs.sp - 1] == c->cmp_value;
      c->cmp_value = 0;
      vstack_flush_to_boxed(&c->vs, c->ctx, c->jit_func, c->r_d_slot);
      MIR_reg_t cond = is_peek ? vstack_top(&c->vs) : vstack_pop(&c->vs);
      bool short_op = (c->op == OP_JMP_FALSE8 || c->op == OP_JMP_TRUE8);
      bool is_false_branch = (c->op == OP_JMP_FALSE || c->op == OP_JMP_FALSE8 || c->op == OP_JMP_FALSE_PEEK);
      int target = c->bc_off + c->sz + (short_op ? (int8_t)sv_get_i8(c->ip + 1) : sv_get_i32(c->ip + 1));
      MIR_label_t lbl = label_for_offset(c->ctx, &c->lm, target);
      if (fused) {
        jit_emit_fused_compare_branch(c, is_false_branch, lbl);
        break;
      }
      if (cond_known_bool) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_BEQ,
                                     MIR_new_label_op(c->ctx, lbl),
                                     MIR_new_reg_op(c->ctx, cond),
                                     MIR_new_uint_op(c->ctx, is_false_branch ? js_false : js_true)));
        break;
      }
      mir_emit_truthy_branch(c->ctx, c->jit_func, cond, c->r_bool, c->r_js,
          c->truthy_proto, c->imp_is_truthy, is_false_branch, lbl);
      break;
    }

    case OP_RETURN: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      jit_emit_return_value(c, vstack_pop(&c->vs));
      break;
    }

    case OP_RETURN_UNDEF: {
      const jit_try_entry_t *fin = jit_innermost_finally(c);
      if (fin) {
        jit_emit_finally_route(c, fin, SV_COMPLETION_RETURN,
                               MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0)));
        break;
      }
      jit_emit_close_frame_upvalues(c);
      if (c->func->is_derived_ctor) {
        // implicit ctor return: hand the (possibly super-rebound) this
        // back to the caller, which has no other channel to receive it
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_ret_insn(c->ctx, 1, MIR_new_reg_op(c->ctx, c->r_this_curr)));
        break;
      }
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_ret_insn(c->ctx, 1,
                                       MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0))));
      break;
    }

    case OP_NOP:
    case OP_HALT:
    case OP_LINE_NUM:
    case OP_COL_NUM:
    case OP_LABEL:
      break;
    case OP_TRY_PUSH: {
      if (c->jit_try_depth >= JIT_TRY_MAX || c->catch_sp_count >= JIT_TRY_MAX) {
        c->ok = false;
        break;
      }
      int32_t off = sv_get_i32(c->ip + 1);
      int catch_off = c->bc_off + c->sz + off;
      MIR_label_t catch_lbl = label_for_offset(c->ctx, &c->lm, catch_off);
      c->jit_try_stack[c->jit_try_depth].catch_label = catch_lbl;
      c->jit_try_stack[c->jit_try_depth].catch_bc_off = catch_off;
      c->jit_try_stack[c->jit_try_depth].saved_sp = c->vs.sp;
      c->jit_try_depth++;
      c->catch_sp_map[c->catch_sp_count].bc_off = catch_off;
      c->catch_sp_map[c->catch_sp_count].saved_sp = c->vs.sp;
      c->catch_sp_count++;
      break;
    }

    case OP_TRY_POP:
      if (c->jit_try_depth > 0) c->jit_try_depth--;
      break;

    case OP_TRY_PUSH_FINALLY:
    case OP_FINALLY_DISCARD:
      if (!c->try_at) c->ok = false;
      break;

    case OP_FINALLY:
      jit_emit_finally_enter(c);
      break;

    case OP_FINALLY_RET:
      jit_emit_finally_ret(c);
      break;

    case OP_UNWIND_JMP: {
      const jit_try_entry_t *h = c->jit_try_depth > 0 ? &c->jit_try_stack[c->jit_try_depth - 1] : NULL;
      if (!h || !h->finally_label) {
        c->ok = false;
        break;
      }
      int target = c->bc_off + c->sz + sv_get_i32(c->ip + 1);
      jit_emit_finally_route(c, h, SV_COMPLETION_JUMP, MIR_new_int_op(c->ctx, target));
      break;
    }

    case OP_THROW: {
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      jit_emit_throw_value(c, vstack_pop(&c->vs));
      break;
    }

    case OP_THROW_ERROR: {
      uint32_t atom_idx = sv_get_u32(c->ip + 1);
      uint8_t err_type = sv_get_u8(c->ip + 5);
      if (atom_idx >= (uint32_t)c->func->atom_count) {
        c->ok = false;
        break;
      }
      sv_atom_t *atom = &c->func->atoms[atom_idx];
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 8,
                                        MIR_new_ref_op(c->ctx, c->throw_error_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_throw_error),
                                        MIR_new_reg_op(c->ctx, c->r_err_tmp),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_js),
                                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)atom->str),
                                        MIR_new_uint_op(c->ctx, (uint64_t)atom->len),
                                        MIR_new_int_op(c->ctx, (int64_t)err_type)));
      if (c->jit_try_depth > 0) {
        jit_try_entry_t *h = &c->jit_try_stack[c->jit_try_depth - 1];
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->vs.regs[h->saved_sp]),
                                     MIR_new_reg_op(c->ctx, c->r_err_tmp)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP,
                                     MIR_new_label_op(c->ctx, h->catch_label)));
      } else {
        jit_emit_exit_ret(c, MIR_new_reg_op(c->ctx, c->r_err_tmp));
      }
      break;
    }

    case OP_CATCH: {
      int catch_saved_sp = -1;
      for (int i = 0; i < c->catch_sp_count; i++) {
        if (c->catch_sp_map[i].bc_off == c->bc_off) {
          catch_saved_sp = c->catch_sp_map[i].saved_sp;
          break;
        }
      }
      if (catch_saved_sp >= 0) {
        c->vs.sp = catch_saved_sp + 1;
        vstack_clear_value_info(&c->vs, catch_saved_sp);
        c->vs.slot_type[catch_saved_sp] = SLOT_BOXED;
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_call_insn(c->ctx, 6,
                                          MIR_new_ref_op(c->ctx, c->helper1_proto),
                                          MIR_new_ref_op(c->ctx, c->imp_catch_value),
                                          MIR_new_reg_op(c->ctx, c->vs.regs[catch_saved_sp]),
                                          MIR_new_reg_op(c->ctx, c->r_vm),
                                          MIR_new_reg_op(c->ctx, c->r_js),
                                          MIR_new_reg_op(c->ctx, c->vs.regs[catch_saved_sp])));
        jit_emit_interrupt_poll(c);
      } else {
        c->ok = false;
      }
      break;
    }

    case OP_NIP_CATCH: {
      if (c->vs.sp < 2) {
        c->ok = false;
        break;
      }
      vstack_ensure_boxed(&c->vs, c->vs.sp - 1, c->ctx, c->jit_func, c->r_d_slot);
      jit_value_info_t info = vstack_value_info(&c->vs, c->vs.sp - 1);
      MIR_reg_t a = c->vs.regs[c->vs.sp - 1];
      MIR_reg_t below = c->vs.regs[c->vs.sp - 2];
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, below),
                                   MIR_new_reg_op(c->ctx, a)));
      c->vs.sp--;
      c->vs.slot_type[c->vs.sp - 1] = SLOT_BOXED;
      vstack_set_value_info(&c->vs, c->vs.sp - 1, info);
      break;
    }

    default:
      __builtin_unreachable();
  }
}
