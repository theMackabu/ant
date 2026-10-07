#include "compile.h"

void jit_emit_exit_ret(jit_compile_t *c, MIR_op_t ret_op) {
  mir_emit_exit_ret(c->ctx, c->jit_func,
                    c->close_upval_proto, c->imp_close_upval,
                    c->adopt_open_upvalues_proto, c->imp_adopt_open_upvalues,
                    c->r_vm, c->r_slotbuf, c->r_lbuf, c->r_jit_open_upvalues,
                    c->has_captured_slots, c->captured_params, c->param_count,
                    c->has_captures, c->captured_locals, c->n_locals,
                    &c->reg_site_n, ret_op);
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

void mir_emit_drop_owner_code_once(
  MIR_context_t ctx, MIR_item_t fn, ant_t *js, const char *prefix, int site,
  uint8_t *mark, uint8_t mark_bit, bool count_snapshot_reset
) {
  sv_func_t *owner = jit_compile_owner;
  uint8_t *fired = owner ? code_arena_bump(js, sizeof(*fired)) : NULL;
  if (!fired) return;
  *fired = 0;

  char name[48];
  snprintf(name, sizeof(name), "%s%d_drop_cell", prefix, site);
  MIR_reg_t cell = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, name);
  snprintf(name, sizeof(name), "%s%d_drop_func", prefix, site);
  MIR_reg_t func = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, name);
  snprintf(name, sizeof(name), "%s%d_drop_tmp", prefix, site);
  MIR_reg_t tmp = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, name);
  MIR_label_t done = MIR_new_label(ctx);

#define STORE(type, disp, base, op) \
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_mem_op(ctx, type, (MIR_disp_t)(disp), base, 0, 1), op))
#define LOAD(type, disp, base) \
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, tmp), MIR_new_mem_op(ctx, type, (MIR_disp_t)(disp), base, 0, 1)))

  mir_load_imm(ctx, fn, cell, (uint64_t)(uintptr_t)fired);
  LOAD(MIR_T_U8, 0, cell);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, done),
      MIR_new_reg_op(ctx, tmp), MIR_new_int_op(ctx, 0)));
  STORE(MIR_T_U8, 0, cell, MIR_new_int_op(ctx, 1));

  if (mark) {
    mir_load_imm(ctx, fn, cell, (uint64_t)(uintptr_t)mark);
    LOAD(MIR_T_U8, 0, cell);
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_OR,
        MIR_new_reg_op(ctx, tmp), MIR_new_reg_op(ctx, tmp), MIR_new_int_op(ctx, mark_bit)));
    STORE(MIR_T_U8, 0, cell, MIR_new_reg_op(ctx, tmp));
  }

  mir_load_imm(ctx, fn, func, (uint64_t)(uintptr_t)owner);
  STORE(MIR_T_P, offsetof(sv_func_t, jit_code), func, MIR_new_int_op(ctx, 0));
  STORE(MIR_T_U32, offsetof(sv_func_t, back_edge_count), func, MIR_new_int_op(ctx, 0));
  STORE(MIR_T_U32, offsetof(sv_func_t, jit_compiled_tfb_ver), func, MIR_new_int_op(ctx, 0));
  STORE(MIR_T_U32, offsetof(sv_func_t, call_count), func, MIR_new_int_op(ctx, SV_JIT_THRESHOLD - SV_JIT_RECOMPILE_DELAY));
  
  if (count_snapshot_reset) {
    LOAD(MIR_T_U8, offsetof(sv_func_t, jit_snapshot_resets), func);
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_ADD,
        MIR_new_reg_op(ctx, tmp), MIR_new_reg_op(ctx, tmp), MIR_new_int_op(ctx, 1)));
    STORE(MIR_T_U8, offsetof(sv_func_t, jit_snapshot_resets), func, MIR_new_reg_op(ctx, tmp));
  }
#undef LOAD
#undef STORE

  MIR_append_insn(ctx, fn, done);
}

void mir_emit_builtin_call_watch(
  MIR_context_t ctx, MIR_item_t fn, ant_t *js, const char *prefix, int site,
  MIR_reg_t r_tmp, MIR_reg_t func, sv_func_t *feedback_func, int bc_off
) {
  uint8_t *type_feedback = sv_func_type_feedback(feedback_func);
  if (!type_feedback || !jit_compile_owner) return;

  MIR_label_t not_builtin = MIR_new_label(ctx);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_URSH,
    MIR_new_reg_op(ctx, r_tmp), MIR_new_reg_op(ctx, func),
    MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
    MIR_new_label_op(ctx, not_builtin), MIR_new_reg_op(ctx, r_tmp),
    MIR_new_uint_op(ctx, (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | kTypeBuiltin)));
  mir_emit_drop_owner_code_once(ctx, fn, js, prefix, site, &type_feedback[bc_off], SV_TFB_CALLED_BUILTIN, false);
  MIR_append_insn(ctx, fn, not_builtin);
}

void mir_emit_builtin_call_fast(
  MIR_context_t ctx, MIR_item_t fn, const char *prefix, int site,
  MIR_reg_t r_js, MIR_reg_t r_tmp,
  MIR_item_t cfunc_proto, MIR_item_t native_finish_proto, MIR_item_t imp_native_finish,
  MIR_reg_t func, MIR_reg_t this_val, MIR_reg_t args, uint16_t argc,
  MIR_reg_t result, MIR_label_t generic, MIR_label_t done
) {
  char name[48];

  snprintf(name, sizeof(name), "%s%d_bi_fn", prefix, site);
  MIR_reg_t r_fn = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, name);
  snprintf(name, sizeof(name), "%s%d_bi_this", prefix, site);
  MIR_reg_t r_this = MIR_new_func_reg(ctx, fn->u.func, MIR_JSVAL, name);
  snprintf(name, sizeof(name), "%s%d_bi_saved", prefix, site);
  MIR_reg_t r_saved = MIR_new_func_reg(ctx, fn->u.func, MIR_JSVAL, name);

  MIR_label_t this_ok = MIR_new_label(ctx);
  MIR_label_t this_global = MIR_new_label(ctx);
  MIR_label_t finish = MIR_new_label(ctx);

  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_URSH,
    MIR_new_reg_op(ctx, r_tmp), MIR_new_reg_op(ctx, func),
    MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
    MIR_new_label_op(ctx, generic), MIR_new_reg_op(ctx, r_tmp),
    MIR_new_uint_op(ctx, (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | kTypeBuiltin)));

  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
    MIR_new_reg_op(ctx, r_tmp),
    MIR_new_mem_op(ctx, MIR_T_U32, offsetof(ant_t, vm_exec_depth), r_js, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BEQ,
    MIR_new_label_op(ctx, generic), MIR_new_reg_op(ctx, r_tmp), MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
    MIR_new_reg_op(ctx, r_tmp),
    MIR_new_mem_op(ctx, MIR_JSVAL, offsetof(ant_t, exception), r_js, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
    MIR_new_label_op(ctx, generic), MIR_new_reg_op(ctx, r_tmp),
    MIR_new_uint_op(ctx, js_mkundef())));

  mir_emit_decode_ref(ctx, fn, r_fn, func);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
    MIR_new_reg_op(ctx, r_fn),
    MIR_new_mem_op(ctx, MIR_T_P, offsetof(ant_cfunc_meta_t, fn), r_fn, 0, 1)));

  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
    MIR_new_reg_op(ctx, r_this), MIR_new_reg_op(ctx, this_val)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BEQ,
    MIR_new_label_op(ctx, this_global), MIR_new_reg_op(ctx, r_this),
    MIR_new_uint_op(ctx, js_mkundef())));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BEQ,
    MIR_new_label_op(ctx, this_global), MIR_new_reg_op(ctx, r_this),
    MIR_new_uint_op(ctx, js_mknull())));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, this_ok)));
  MIR_append_insn(ctx, fn, this_global);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
    MIR_new_reg_op(ctx, r_this),
    MIR_new_mem_op(ctx, MIR_JSVAL, offsetof(ant_t, global), r_js, 0, 1)));
  MIR_append_insn(ctx, fn, this_ok);

  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
    MIR_new_reg_op(ctx, r_saved),
    MIR_new_mem_op(ctx, MIR_JSVAL, offsetof(ant_t, this_val), r_js, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
    MIR_new_mem_op(ctx, MIR_JSVAL, offsetof(ant_t, this_val), r_js, 0, 1),
    MIR_new_reg_op(ctx, r_this)));

  MIR_append_insn(ctx, fn, MIR_new_call_insn(ctx, 7,
    MIR_new_ref_op(ctx, cfunc_proto), MIR_new_reg_op(ctx, r_fn),
    MIR_new_reg_op(ctx, result), MIR_new_reg_op(ctx, r_js),
    MIR_new_reg_op(ctx, args), MIR_new_int_op(ctx, (int64_t)argc),
    MIR_new_uint_op(ctx, js_mkundef())));

  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
    MIR_new_mem_op(ctx, MIR_JSVAL, offsetof(ant_t, this_val), r_js, 0, 1),
    MIR_new_reg_op(ctx, r_saved)));

  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_URSH,
    MIR_new_reg_op(ctx, r_tmp), MIR_new_reg_op(ctx, result),
    MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BEQ,
    MIR_new_label_op(ctx, finish), MIR_new_reg_op(ctx, r_tmp),
    MIR_new_uint_op(ctx, JIT_ERR_TAG)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
    MIR_new_reg_op(ctx, r_tmp),
    MIR_new_mem_op(ctx, MIR_JSVAL, offsetof(ant_t, exception), r_js, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BEQ,
    MIR_new_label_op(ctx, done), MIR_new_reg_op(ctx, r_tmp),
    MIR_new_uint_op(ctx, js_mkundef())));

  MIR_append_insn(ctx, fn, finish);
  MIR_append_insn(ctx, fn, MIR_new_call_insn(ctx, 5,
    MIR_new_ref_op(ctx, native_finish_proto), MIR_new_ref_op(ctx, imp_native_finish),
    MIR_new_reg_op(ctx, result), MIR_new_reg_op(ctx, r_js),
    MIR_new_reg_op(ctx, result)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
}

void jit_emit_builtin_call_fast(
  jit_compile_t *c, MIR_reg_t func, MIR_reg_t this_val,
  MIR_reg_t args, uint16_t argc, MIR_reg_t result,
  MIR_label_t generic, MIR_label_t done
) {
  mir_emit_builtin_call_fast(
    c->ctx, c->jit_func, "c", c->call_n++, c->r_js, c->r_bool,
    c->cfunc_proto, c->native_finish_proto, c->imp_native_finish,
    func, this_val, args, argc, result, generic, done
  );
}
