#include "compile.h"

static void jit_discard_setup_module(jit_compile_t *c) {
  for (int i = 0; i < c->lm.count; i++)
    MIR_append_insn(c->ctx, c->jit_func, c->lm.entries[i].label);
  MIR_finish_func(c->ctx);
  MIR_finish_module(c->ctx);
  MIR_remove_module(c->ctx, c->mod);
}

static void jit_emit_cold_entry_counter(jit_compile_t *c, const char *site) {
  if (!c->cold_tier) return;
  char fn_name[32], calls_name[32], hot_name[32], res_name[32];
  snprintf(fn_name, sizeof fn_name, "tier_fn_%s", site);
  snprintf(calls_name, sizeof calls_name, "tier_calls_%s", site);
  snprintf(hot_name, sizeof hot_name, "tier_hot_%s", site);
  snprintf(res_name, sizeof res_name, "tier_res_%s", site);

  MIR_label_t body = MIR_new_label(c->ctx);
  MIR_reg_t r_fn = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, fn_name);
  MIR_reg_t r_calls = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, calls_name);
  MIR_reg_t r_hot = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, hot_name);
  MIR_reg_t r_res = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, res_name);
  MIR_op_t call_count = MIR_new_mem_op(c->ctx, MIR_T_U32,
                                       (MIR_disp_t)offsetof(sv_func_t, call_count), r_fn, 0, 1);
  mir_load_imm(c->ctx, c->jit_func, r_fn, (uint64_t)(uintptr_t)c->func);
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, r_calls), call_count));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_ADD, MIR_new_reg_op(c->ctx, r_calls),
                               MIR_new_reg_op(c->ctx, r_calls), MIR_new_int_op(c->ctx, 1)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV, call_count, MIR_new_reg_op(c->ctx, r_calls)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_UBLE, MIR_new_label_op(c->ctx, body),
                               MIR_new_reg_op(c->ctx, r_calls),
                               MIR_new_int_op(c->ctx, SV_JIT_THRESHOLD)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_call_insn(c->ctx, 6,
                                    MIR_new_ref_op(c->ctx, c->tier_up_proto),
                                    MIR_new_ref_op(c->ctx, c->imp_tier_up),
                                    MIR_new_reg_op(c->ctx, r_hot),
                                    MIR_new_reg_op(c->ctx, c->r_js),
                                    MIR_new_reg_op(c->ctx, r_fn),
                                    MIR_new_reg_op(c->ctx, c->r_closure)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_BEQ, MIR_new_label_op(c->ctx, body),
                               MIR_new_reg_op(c->ctx, r_hot), MIR_new_int_op(c->ctx, 0)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_call_insn(c->ctx, 10,
                                    MIR_new_ref_op(c->ctx, c->self_proto),
                                    MIR_new_reg_op(c->ctx, r_hot),
                                    MIR_new_reg_op(c->ctx, r_res),
                                    MIR_new_reg_op(c->ctx, c->r_vm),
                                    MIR_new_reg_op(c->ctx, c->r_this),
                                    MIR_new_reg_op(c->ctx, c->r_new_target),
                                    MIR_new_reg_op(c->ctx, c->r_super_val),
                                    MIR_new_reg_op(c->ctx, c->r_args),
                                    MIR_new_reg_op(c->ctx, c->r_argc),
                                    MIR_new_reg_op(c->ctx, c->r_closure)));
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_ret_insn(c->ctx, 1, MIR_new_reg_op(c->ctx, r_res)));
  MIR_append_insn(c->ctx, c->jit_func, body);
}

bool jit_setup_frame(jit_compile_t *c) {
  char fname[128];
  snprintf(fname, sizeof(fname), "jit_%s_%p",
           c->func->debug->name ? c->func->debug->name : "anon", (void *)c->func);

  c->mod = MIR_new_module(c->ctx, fname);
  MIR_type_t ret_type = MIR_JSVAL;

  jit_setup_prototypes(c, ret_type);

  c->jit_func = MIR_new_func(c->ctx, fname,
                             1, &ret_type,
                             7,
                             MIR_T_I64, "vm",
                             MIR_JSVAL, "this_val",
                             MIR_JSVAL, "new_target",
                             MIR_JSVAL, "super_val",
                             MIR_T_P, "args",
                             MIR_T_I32, "argc",
                             MIR_T_P, "closure");

  c->lm = (jit_label_map_t){0};
  if (!scan_branch_targets(c->func, &c->lm, c->ctx)) {
    jit_discard_setup_module(c);
    c->func->jit_compile_failed = true;
    c->func->jit_compiling = false;
    return false;
  }

  c->r_vm = MIR_reg(c->ctx, "vm", c->jit_func->u.func);
  c->r_this = MIR_reg(c->ctx, "this_val", c->jit_func->u.func);
  c->r_new_target = MIR_reg(c->ctx, "new_target", c->jit_func->u.func);
  c->r_super_val = MIR_reg(c->ctx, "super_val", c->jit_func->u.func);
  c->r_args = MIR_reg(c->ctx, "args", c->jit_func->u.func);
  c->r_argc = MIR_reg(c->ctx, "argc", c->jit_func->u.func);
  c->r_closure = MIR_reg(c->ctx, "closure", c->jit_func->u.func);

  c->r_cage_base = MIR_new_func_reg(
      c->ctx, c->jit_func->u.func, MIR_T_I64, "cage_base");
  mir_load_imm(c->ctx, c->jit_func, c->r_cage_base, (uint64_t)ant_cage_base());

  c->r_this_curr = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, "this_curr");
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV,
                               MIR_new_reg_op(c->ctx, c->r_this_curr),
                               MIR_new_reg_op(c->ctx, c->r_this)));

  c->r_js = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "js_ptr");
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_MOV,
                               MIR_new_reg_op(c->ctx, c->r_js),
                               MIR_new_mem_op(c->ctx, MIR_T_I64, 0, c->r_vm, 0, 1)));

  {
    MIR_reg_t r_stk_probe = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "stk_probe");
    MIR_reg_t r_stk_floor = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "stk_floor");
    MIR_label_t no_overflow = MIR_new_label(c->ctx);
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_ALLOCA,
                                 MIR_new_reg_op(c->ctx, r_stk_probe),
                                 MIR_new_int_op(c->ctx, 8)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV,
                                 MIR_new_reg_op(c->ctx, r_stk_floor),
                                 MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                (MIR_disp_t)offsetof(ant_t, cstk.floor), c->r_js, 0, 1)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_UBGE,
                                 MIR_new_label_op(c->ctx, no_overflow),
                                 MIR_new_reg_op(c->ctx, r_stk_probe),
                                 MIR_new_reg_op(c->ctx, r_stk_floor)));
    MIR_reg_t r_ovf_err = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, "stk_err");
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_call_insn(c->ctx, 5,
                                      MIR_new_ref_op(c->ctx, c->stack_ovf_err_proto),
                                      MIR_new_ref_op(c->ctx, c->imp_stack_ovf_err),
                                      MIR_new_reg_op(c->ctx, r_ovf_err),
                                      MIR_new_reg_op(c->ctx, c->r_vm),
                                      MIR_new_reg_op(c->ctx, c->r_js)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_ret_insn(c->ctx, 1, MIR_new_reg_op(c->ctx, r_ovf_err)));
    MIR_append_insn(c->ctx, c->jit_func, no_overflow);
  }

  c->vs = (jit_vstack_t){0};
  c->vs.max = c->func->max_stack + JIT_VSTACK_SLACK;
  c->vs.regs = calloc((size_t)c->vs.max, sizeof(MIR_reg_t));
  c->vs.known_func = calloc((size_t)c->vs.max, sizeof(sv_func_t *));
  c->vs.d_regs = calloc((size_t)c->vs.max, sizeof(MIR_reg_t));
  c->vs.slot_type = calloc((size_t)c->vs.max, sizeof(uint8_t));
  c->vs.known_const = calloc((size_t)c->vs.max, sizeof(uint64_t));
  c->vs.has_const = calloc((size_t)c->vs.max, sizeof(bool));
  c->vs.known_bool = calloc((size_t)c->vs.max, sizeof(uint8_t));
  c->vs.integer_range = calloc((size_t)c->vs.max, sizeof(jit_integer_range_t));

  if (!c->vs.regs || !c->vs.known_func || !c->vs.d_regs || !c->vs.slot_type || !c->vs.known_const || !c->vs.has_const || !c->vs.known_bool || !c->vs.integer_range) {
    free(c->vs.regs);
    free(c->vs.known_func);
    free(c->vs.d_regs);
    free(c->vs.slot_type);
    free(c->vs.known_const);
    free(c->vs.has_const);
    free(c->vs.known_bool);
    free(c->vs.integer_range);

    jit_discard_setup_module(c);

    c->func->jit_compiling = false;
    return false;
  }

  for (int i = 0; i < c->vs.max; i++) {
    char rname[32];
    snprintf(rname, sizeof(rname), "s%d", i);
    c->vs.regs[i] = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, rname);
  }
  for (int i = 0; i < c->vs.max; i++) {
    char dname[32];
    snprintf(dname, sizeof(dname), "sd%d", i);
    c->vs.d_regs[i] = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, dname);
  }

  c->n_locals = c->func->max_locals;
  c->local_regs = NULL;
  c->local_d_regs = NULL;
  c->known_func_locals = NULL;
  c->known_type_locals = NULL;
  if (c->n_locals > 0) {
    c->local_regs = calloc((size_t)c->n_locals, sizeof(MIR_reg_t));
    c->local_d_regs = calloc((size_t)c->n_locals, sizeof(MIR_reg_t));
    c->known_func_locals = calloc((size_t)c->n_locals, sizeof(sv_func_t *));
    c->known_type_locals = calloc((size_t)c->n_locals, sizeof(uint8_t));

    if (!c->local_regs || !c->local_d_regs || !c->known_func_locals || !c->known_type_locals) {
      free(c->vs.regs);
      free(c->vs.known_func);
      free(c->vs.d_regs);
      free(c->vs.slot_type);
      free(c->vs.known_const);
      free(c->vs.has_const);
      free(c->vs.known_bool);
      free(c->vs.integer_range);
      free(c->local_regs);
      free(c->local_d_regs);
      free(c->known_func_locals);
      free(c->known_type_locals);

      jit_discard_setup_module(c);

      c->func->jit_compiling = false;
      return false;
    }

    sv_type_info_t *local_types = sv_func_local_types(c->func);
    if (local_types && c->func->local_type_count > 0) {
      int ncopy = c->func->local_type_count < c->n_locals ? c->func->local_type_count : c->n_locals;
      for (int i = 0; i < ncopy; i++)
        c->known_type_locals[i] = local_types[i].type;
    }
    if (c->func->local_type_feedback) {
      for (int i = 0; i < c->n_locals; i++) {
        uint8_t ltf = c->func->local_type_feedback[i];
        if (ltf && !(ltf & ~SV_TFB_NUM))
          c->known_type_locals[i] = SV_TI_NUM;
      }
    }
    for (int i = 0; i < c->n_locals; i++) {
      char rname[32], dname[32];
      snprintf(rname, sizeof(rname), "l%d", i);
      snprintf(dname, sizeof(dname), "ld%d", i);
      c->local_regs[i] = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, rname);
      c->local_d_regs[i] = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, dname);
      mir_load_imm(c->ctx, c->jit_func, c->local_regs[i],
                   mkval(kTypeUndefined, 0));
    }
  }

  c->r_tmp = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, "tmp");
  c->r_tmp2 = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, "tmp2");
  c->r_bool = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "bool_tmp");
  c->r_err_tmp = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, "err_tmp");
  mir_load_imm(c->ctx, c->jit_func, c->r_tmp2, 0);

  c->self_binding_guards = calloc(
      c->func->code_len > 0 ? (size_t)c->func->code_len : 1u, 1u);
  c->feat = jit_prescan_features(
      c->func, c->func->param_count + c->n_locals);
  if (jit_mark_self_binding_guards(
          c->js, c->func, c->hint_closure, c->self_binding_guards)) {
    c->feat.needs_bailout = true;
    c->feat.needs_args_buf = true;
  }

  c->r_d_slot = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "d_slot");
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_ALLOCA,
                               MIR_new_reg_op(c->ctx, c->r_d_slot),
                               MIR_new_uint_op(c->ctx, 8)));

  c->r_d_one = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_D, "d_one");
  if (c->feat.needs_inc_local) {
    union {
      double d;
      uint64_t u;
    } one = {1.0};
    mir_load_imm(c->ctx, c->jit_func, c->r_bool, one.u);
    mir_i64_to_d(c->ctx, c->jit_func, c->r_d_one, c->r_bool, c->r_d_slot);
  }

  if (c->cold_tier) c->feat.needs_args_buf = true;
  c->r_args_buf = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "args_buf");
  if (c->feat.needs_args_buf) {
    int scratch_slots = SV_JIT_ARGS_BUF_CAP;
    if (c->vs.max > scratch_slots) scratch_slots = c->vs.max;
    if (c->n_locals > scratch_slots) scratch_slots = c->n_locals;
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_ALLOCA,
                                 MIR_new_reg_op(c->ctx, c->r_args_buf),
                                 MIR_new_uint_op(c->ctx, (uint64_t)scratch_slots * sizeof(ant_value_t))));
  } else {
    mir_load_imm(c->ctx, c->jit_func, c->r_args_buf, 0);
  }

  c->inline_ext = (jit_inline_ext_t){
      .helper1_proto = c->helper1_proto,
      .imp_get_length_inline = c->imp_get_length_inline,
      .imp_get_elem_inline = c->imp_get_elem_inline,
      .put_field_proto = c->put_field_proto,
      .imp_put_field = c->imp_put_field,
      .shape_transition_proto = c->shape_transition_proto,
      .imp_shape_transition = c->imp_shape_transition,
      .remember_obj_proto = c->remember_obj_proto,
      .imp_remember_obj = c->imp_remember_obj,
      .call_proto = c->call_proto,
      .imp_call = c->imp_call,
      .call_method_proto = c->call_method_proto,
      .imp_call_method = c->imp_call_method,
      .stable_load_proto = c->stable_load_proto,
      .imp_load_stable_builtin = c->imp_load_stable_builtin,
      .stable_call_proto = c->stable_call_proto,
      .imp_call_stable_builtin = c->imp_call_stable_builtin,
      .imp_band = c->imp_band,
      .imp_bor = c->imp_bor,
      .imp_bxor = c->imp_bxor,
      .imp_shl = c->imp_shl,
      .imp_shr = c->imp_shr,
      .imp_ushr = c->imp_ushr,
      .self_proto = c->self_proto,
      .r_args_buf = c->r_args_buf,
  };

  c->r_call_out_this = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "call_out_this");
  MIR_append_insn(c->ctx, c->jit_func,
                  MIR_new_insn(c->ctx, MIR_ALLOCA,
                               MIR_new_reg_op(c->ctx, c->r_call_out_this),
                               MIR_new_uint_op(c->ctx, sizeof(ant_value_t))));

  c->normalize_sloppy_this =
      c->feat.needs_this && !c->func->is_strict && !c->func->is_arrow;
  c->r_this_root = 0;
  if (c->normalize_sloppy_this) {
    c->r_this_root =
        MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "this_root");
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_ALLOCA,
                                 MIR_new_reg_op(c->ctx, c->r_this_root),
                                 MIR_new_uint_op(c->ctx, sizeof(ant_value_t))));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV,
                                 MIR_new_mem_op(c->ctx, MIR_JSVAL, 0, c->r_this_root, 0, 1),
                                 MIR_new_reg_op(c->ctx, c->r_this_curr)));
  }

  c->r_iter_roots = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "iter_roots");
  c->r_iter_buf = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "iter_buf");
  if (c->feat.needs_iter_roots && c->vs.max > 0) {
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_ALLOCA,
                                 MIR_new_reg_op(c->ctx, c->r_iter_roots),
                                 MIR_new_uint_op(c->ctx, (uint64_t)c->vs.max * sizeof(ant_value_t))));
    for (int i = 0; i < c->vs.max; i++) {
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), c->r_iter_roots, 0, 1),
                                   MIR_new_uint_op(c->ctx, mkval(kTypeUndefined, 0))));
    }
  } else {
    mir_load_imm(c->ctx, c->jit_func, c->r_iter_roots, 0);
  }

  c->r_tco_args = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "tco_args");
  c->r_cond_d = 0, c->r_cond_nan = 0, c->r_cond_zd = 0, c->r_cond_zero = 0;
  c->needs_bailout = c->feat.needs_bailout;

  c->r_ic_epoch_val = 0;
  if (c->feat.needs_ic_epoch) {
    c->r_ic_epoch_val = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "ic_ep_ptr");
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV,
                                 MIR_new_reg_op(c->ctx, c->r_ic_epoch_val),
                                 MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)&ant_ic_epoch_counter)));
  }

  c->r_bailout_val = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, "bail_val");
  c->r_bailout_off = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "bail_off");
  c->r_bailout_sp = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "bail_sp");
  c->bailout_tramp = c->needs_bailout ? MIR_new_label(c->ctx) : NULL;

  c->param_count = c->func->param_count;
  c->writes_params = func_writes_params(c->func);
  c->captured_params = scan_captured_params(c->func);
  c->captured_locals = scan_captured_locals(c->func, c->n_locals);
  c->builder_target_slots = c->feat.builder_target_slots;
  c->has_captured_params = false;
  c->has_captures = false;
  if (c->captured_params) {
    for (int i = 0; i < c->param_count; i++)
      if (c->captured_params[i]) {
        c->has_captured_params = true;
        break;
      }
  }
  if (c->captured_locals) {
    for (int i = 0; i < c->n_locals; i++)
      if (c->captured_locals[i]) {
        c->has_captures = true;
        break;
      }
  }
  c->cached_index_key = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "cached_index_key");
  c->cached_index_value = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "cached_index_value");
  mir_load_imm(c->ctx, c->jit_func, c->cached_index_key, js_mkundef());
  mir_load_imm(c->ctx, c->jit_func, c->cached_index_value, 0);
  c->cached_element_object = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "cached_element_object");
  c->cached_element_key = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "cached_element_key");
  c->cached_element_value = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "cached_element_value");
  c->cached_element_valid = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "cached_element_valid");
  mir_load_imm(c->ctx, c->jit_func, c->cached_element_valid, 0);
  c->dnum_locals = NULL;
  if (c->n_locals > 0 && c->local_d_regs && c->known_type_locals) {
    c->dnum_locals = calloc((size_t)c->n_locals, 1);
    if (c->dnum_locals) {
      for (int i = 0; i < c->n_locals; i++)
        c->dnum_locals[i] = c->known_type_locals[i] == SV_TI_NUM &&
                            !(c->captured_locals && c->captured_locals[i]);
      uint8_t *dp = c->func->code, *de = c->func->code + c->func->code_len;
      while (dp < de) {
        sv_op_t dop = (sv_op_t)*dp;
        int dsz = sv_op_size[dop];
        if (dsz == 0) break;
        if (dop == OP_SET_LOCAL_UNDEF) {
          uint16_t di = sv_get_u16(dp + 1);
          if (di < (uint16_t)c->n_locals &&
              !jit_has_immediate_numeric_local_init(c->func, dp + dsz, de, di))
            c->dnum_locals[di] = 0;
        } else if ((sv_op_flags[dop] & SV_OPF_BUILDER_TARGET) != 0) {
          uint16_t ds = sv_get_u16(dp + 1);
          if (ds >= (uint16_t)c->param_count &&
              (int)ds - c->param_count < c->n_locals)
            c->dnum_locals[ds - c->param_count] = 0;
        }
        dp += dsz;
      }
    }
  }
  c->entry_integer_ranges = calloc((size_t)c->n_locals, sizeof(jit_integer_range_t));
  c->entry_integer_regs = calloc((size_t)c->n_locals, sizeof(MIR_reg_t));
  if (c->entry_integer_ranges && c->entry_integer_regs && c->dnum_locals) {
    jit_entry_integer_ranges(c->func, c->entry_integer_ranges, c->n_locals, c->param_count);
    for (int i = 0; i < c->n_locals; i++) {
      if (!c->dnum_locals[i]) continue;
      if (!c->entry_integer_ranges[i].known) continue;
      char name[48];
      snprintf(name, sizeof(name), "entry_integer_%d", i);
      c->entry_integer_regs[i] = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, name);
    }
  }
  c->params_in_slotbuf = c->writes_params || c->has_captured_params;
  c->has_captured_slots = c->params_in_slotbuf || c->has_captures;
  c->use_unified_slotbuf = c->has_captured_slots && c->has_captures;
  c->slotbuf_count = c->use_unified_slotbuf ? (c->param_count + c->n_locals) : c->param_count;

  c->r_param_init_argc = c->r_argc;
  if (c->writes_params && c->param_count > 0) {
    c->r_param_init_argc = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "param_init_argc");
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV,
                                 MIR_new_reg_op(c->ctx, c->r_param_init_argc),
                                 MIR_new_reg_op(c->ctx, c->r_argc)));

    MIR_disp_t osr_base = (MIR_disp_t)offsetof(struct sv_vm, jit_osr);
    MIR_label_t param_argc_done = MIR_new_label(c->ctx);
    MIR_label_t param_argc_enough = MIR_new_label(c->ctx);
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV,
                                 MIR_new_reg_op(c->ctx, c->r_bool),
                                 MIR_new_mem_op(c->ctx, MIR_T_U8,
                                                osr_base + (MIR_disp_t)offsetof(sv_jit_osr_t, active),
                                                c->r_vm, 0, 1)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_BEQ,
                                 MIR_new_label_op(c->ctx, param_argc_done),
                                 MIR_new_reg_op(c->ctx, c->r_bool),
                                 MIR_new_int_op(c->ctx, 0)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_UBGT,
                                 MIR_new_label_op(c->ctx, param_argc_enough),
                                 MIR_new_reg_op(c->ctx, c->r_param_init_argc),
                                 MIR_new_int_op(c->ctx, (int64_t)c->param_count - 1)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV,
                                 MIR_new_reg_op(c->ctx, c->r_param_init_argc),
                                 MIR_new_int_op(c->ctx, c->param_count)));
    MIR_append_insn(c->ctx, c->jit_func, param_argc_enough);
    MIR_append_insn(c->ctx, c->jit_func, param_argc_done);
  }

  c->r_slotbuf = c->r_tmp2;
  if (c->has_captured_slots && c->slotbuf_count > 0) {
    c->r_slotbuf = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "slotbuf");
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_ALLOCA,
                                 MIR_new_reg_op(c->ctx, c->r_slotbuf),
                                 MIR_new_uint_op(c->ctx, (uint64_t)c->slotbuf_count * sizeof(ant_value_t))));
    mir_emit_fill_param_slots_from_args(
        c->ctx, c->jit_func, c->r_slotbuf, c->r_args, c->r_param_init_argc,
        c->captured_params, c->param_count, c->writes_params);
  }

  c->needs_lbuf = c->needs_bailout || c->feat.needs_close_upval || c->has_captures || c->cold_tier;
  c->r_lbuf = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "lbuf");
  if (c->use_unified_slotbuf && c->n_locals > 0) {
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_ADD,
                                 MIR_new_reg_op(c->ctx, c->r_lbuf),
                                 MIR_new_reg_op(c->ctx, c->r_slotbuf),
                                 MIR_new_int_op(c->ctx, (int64_t)c->param_count * (int64_t)sizeof(ant_value_t))));
  } else if (c->needs_lbuf && c->n_locals > 0) {
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_ALLOCA,
                                 MIR_new_reg_op(c->ctx, c->r_lbuf),
                                 MIR_new_uint_op(c->ctx, (uint64_t)c->n_locals * sizeof(ant_value_t))));
  } else {
    mir_load_imm(c->ctx, c->jit_func, c->r_lbuf, 0);
  }

  c->use_jit_upvalue_list = c->has_captured_slots;
  c->r_jit_open_upvalues = 0;
  if (c->use_jit_upvalue_list) {
    c->r_jit_open_upvalues = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "jit_open_upvalues");
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_ALLOCA,
                                 MIR_new_reg_op(c->ctx, c->r_jit_open_upvalues),
                                 MIR_new_uint_op(c->ctx, sizeof(sv_upvalue_t *))));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV,
                                 MIR_new_mem_op(c->ctx, MIR_T_I64, 0, c->r_jit_open_upvalues, 0, 1),
                                 MIR_new_uint_op(c->ctx, 0)));
  }

  c->bailout_ctx = (jit_bailout_emit_t){
    .val = c->r_bailout_val,
    .off = c->r_bailout_off,
    .sp = c->r_bailout_sp,
    .tramp = c->bailout_tramp,
    .args_buf = c->r_args_buf,
    .vstack = &c->vs,
    .local_regs = c->local_regs,
    .local_d_regs = c->local_d_regs,
    .dnum_locals = c->dnum_locals,
    .n_locals = c->n_locals,
    .lbuf = c->r_lbuf,
    .d_slot = c->r_d_slot,
  };
  
  if (c->cold_tier) {
    c->promote_tramp = MIR_new_label(c->ctx);
    c->promote_ctx = c->bailout_ctx;
    c->promote_ctx.tramp = c->promote_tramp;

    c->promote_sites = calloc((size_t)c->func->code_len, 1);
    if (c->promote_sites) {
      uint8_t *code = c->func->code;
      int n = 0, cap = 16;
      int (*edges)[2] = malloc((size_t)cap * sizeof(*edges));
      for (int off = 0; edges && off < c->func->code_len; ) {
        uint8_t op = code[off];
        int sz = sv_op_size[op];
        if (sz == 0) break;
        uint16_t flags = sv_op_flags[op];
        int target = -1;
        if (flags & SV_OPF_JIT_BRANCH32) target = off + sz + sv_get_i32(code + off + 1);
        else if (flags & SV_OPF_JIT_BRANCH8) target = off + sz + (int8_t)sv_get_i8(code + off + 1);
        if (target >= 0 && target <= off) {
          if (n == cap) { cap *= 2; edges = realloc(edges, (size_t)cap * sizeof(*edges)); if (!edges) break; }
          edges[n][0] = target; edges[n][1] = off; n++;
        }
        off += sz;
      }
      for (int i = 0; edges && i < n; i++) {
        bool nested = false, has_inner = false;
        for (int j = 0; j < n; j++) {
          if (j == i) continue;
          if (edges[j][0] <= edges[i][0] && edges[j][1] >= edges[i][1]) nested = true;
          if (edges[i][0] <= edges[j][0] && edges[j][1] <= edges[i][1]) has_inner = true;
        }
        c->promote_sites[edges[i][1]] = nested ? 0 : has_inner ? 2 : 1;
      }
      free(edges);
    }
    c->r_promote = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_T_I64, "promote_countdown");
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, c->r_promote),
                                 MIR_new_int_op(c->ctx, 1)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_call_insn(c->ctx, 3,
                                      MIR_new_ref_op(c->ctx, c->promote_start_proto),
                                      MIR_new_ref_op(c->ctx, c->imp_promote_start),
                                      MIR_new_ref_op(c->ctx, c->cold_ns_item)));
  }

  if (c->feat.needs_tco_args) {
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_ALLOCA,
                                 MIR_new_reg_op(c->ctx, c->r_tco_args),
                                 MIR_new_uint_op(c->ctx, (uint64_t)SV_JIT_ARGS_BUF_CAP * sizeof(ant_value_t))));
  } else
    mir_load_imm(c->ctx, c->jit_func, c->r_tco_args, 0);
  c->self_tail_entry = MIR_new_label(c->ctx);
  MIR_append_insn(c->ctx, c->jit_func, c->self_tail_entry);

  memset(c->param_cache, 0, sizeof(c->param_cache));
  {
    uint8_t read_mask = 0;
    uint8_t *pscan = c->func->code, *pend = c->func->code + c->func->code_len;
    while (pscan < pend) {
      sv_op_t pop_ = (sv_op_t)*pscan;
      int psz = sv_op_size[pop_];
      if (psz == 0) break;
      if (pop_ == OP_GET_ARG) {
        uint16_t pidx = sv_get_u16(pscan + 1);
        if (pidx < JIT_PARAM_HOIST_CAP) read_mask |= (uint8_t)(1u << pidx);
      }
      pscan += psz;
    }
    for (int i = 0; i < c->param_count && i < JIT_PARAM_HOIST_CAP; i++) {
      if (!(read_mask & (1u << i))) continue;
      if (c->writes_params || (c->captured_params && c->captured_params[i])) continue;
      char prn[16];
      snprintf(prn, sizeof(prn), "parg%d", i);
      c->param_cache[i] = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, prn);
      MIR_label_t in_range = MIR_new_label(c->ctx);
      MIR_label_t done = MIR_new_label(c->ctx);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_UBGT,
                                   MIR_new_label_op(c->ctx, in_range),
                                   MIR_new_reg_op(c->ctx, c->r_argc),
                                   MIR_new_int_op(c->ctx, (int64_t)i)));
      mir_load_imm(c->ctx, c->jit_func, c->param_cache[i], mkval(kTypeUndefined, 0));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done)));
      MIR_append_insn(c->ctx, c->jit_func, in_range);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->param_cache[i]),
                                   MIR_new_mem_op(c->ctx, MIR_JSVAL,
                                                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)), c->r_args, 0, 1)));
      MIR_append_insn(c->ctx, c->jit_func, done);
    }
  }

  c->osr_map = (osr_entry_map_t){0};
  scan_osr_entries(c->func, &c->osr_map);
  if (c->osr_map.count > 0) {
    MIR_label_t normal_entry = MIR_new_label(c->ctx);

    MIR_disp_t osr_base = (MIR_disp_t)offsetof(struct sv_vm, jit_osr);

    MIR_reg_t r_osr_active = MIR_new_func_reg(c->ctx, c->jit_func->u.func,
                                              MIR_T_I64, "osr_active");
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV,
                                 MIR_new_reg_op(c->ctx, r_osr_active),
                                 MIR_new_mem_op(c->ctx, MIR_T_U8,
                                                osr_base + (MIR_disp_t)offsetof(sv_jit_osr_t, active),
                                                c->r_vm, 0, 1)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_BEQ,
                                 MIR_new_label_op(c->ctx, normal_entry),
                                 MIR_new_reg_op(c->ctx, r_osr_active),
                                 MIR_new_int_op(c->ctx, 0)));

    MIR_reg_t r_osr_locals = MIR_new_func_reg(c->ctx, c->jit_func->u.func,
                                              MIR_T_I64, "osr_locals");
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV,
                                 MIR_new_reg_op(c->ctx, r_osr_locals),
                                 MIR_new_mem_op(c->ctx, MIR_T_P,
                                                osr_base + (MIR_disp_t)offsetof(sv_jit_osr_t, locals),
                                                c->r_vm, 0, 1)));

    if (c->feat.needs_iter_roots && c->vs.max > 0) {
      MIR_reg_t r_osr_vstack = MIR_new_func_reg(
          c->ctx, c->jit_func->u.func, MIR_T_I64, "osr_vstack");
      MIR_reg_t r_osr_vstack_sp = MIR_new_func_reg(
          c->ctx, c->jit_func->u.func, MIR_T_I64, "osr_vstack_sp");
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, r_osr_vstack),
                                   MIR_new_mem_op(c->ctx, MIR_T_P,
                                                  osr_base + (MIR_disp_t)offsetof(sv_jit_osr_t, vstack),
                                                  c->r_vm, 0, 1)));
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, r_osr_vstack_sp),
                                   MIR_new_mem_op(c->ctx, MIR_T_I32,
                                                  osr_base + (MIR_disp_t)offsetof(sv_jit_osr_t, vstack_sp),
                                                  c->r_vm, 0, 1)));
      for (int i = 0; i < c->vs.max; i++) {
        MIR_label_t slot_done = MIR_new_label(c->ctx);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_UBLE,
                                     MIR_new_label_op(c->ctx, slot_done),
                                     MIR_new_reg_op(c->ctx, r_osr_vstack_sp),
                                     MIR_new_int_op(c->ctx, i)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->vs.regs[i]),
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                                                    r_osr_vstack, 0, 1)));
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                    (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                                                    c->r_iter_roots, 0, 1),
                                     MIR_new_reg_op(c->ctx, c->vs.regs[i])));
        MIR_append_insn(c->ctx, c->jit_func, slot_done);
      }
    }

    for (int i = 0; i < c->n_locals; i++)
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, c->local_regs[i]),
                                   MIR_new_mem_op(c->ctx, MIR_JSVAL,
                                                  (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                                                  r_osr_locals, 0, 1)));

    if (c->known_type_locals && c->local_d_regs) {
      MIR_label_t osr_types_ok = MIR_new_label(c->ctx);
      MIR_label_t osr_type_bail = MIR_new_label(c->ctx);
      bool osr_any_num = false;
      for (int i = 0; i < c->n_locals; i++)
        if (c->known_type_locals[i] == SV_TI_NUM) {
          osr_any_num = true;
          mir_emit_is_num_guard(c->ctx, c->jit_func, c->r_bool, c->local_regs[i], osr_type_bail);
          mir_i64_to_d(c->ctx, c->jit_func, c->local_d_regs[i], c->local_regs[i], c->r_d_slot);
          if (c->entry_integer_regs && c->entry_integer_regs[i]) {
            jit_integer_range_t range = c->entry_integer_ranges[i];
            MIR_reg_t integer = mir_emit_exact_integer_guard(
                c->ctx, c->jit_func, c->local_regs[i], c->local_d_regs[i], true, c->r_d_slot,
                (double)range.min, (double)range.max, osr_type_bail, -i - 1);
            MIR_append_insn(c->ctx, c->jit_func, MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, c->entry_integer_regs[i]), MIR_new_reg_op(c->ctx, integer)));
          }
        }
      if (osr_any_num) {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, osr_types_ok)));
        MIR_append_insn(c->ctx, c->jit_func, osr_type_bail);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_mem_op(c->ctx, MIR_T_U8,
                                                    osr_base + (MIR_disp_t)offsetof(sv_jit_osr_t, active),
                                                    c->r_vm, 0, 1),
                                     MIR_new_int_op(c->ctx, 0)));
        mir_load_imm(c->ctx, c->jit_func, c->r_bailout_val,
                     (uint64_t)SV_JIT_RETRY_INTERP);
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_ret_insn(c->ctx, 1, MIR_new_reg_op(c->ctx, c->r_bailout_val)));
        MIR_append_insn(c->ctx, c->jit_func, osr_types_ok);
      }
    }

    jit_emit_cold_entry_counter(c, "osr");
    if (c->has_captured_params && c->r_jit_open_upvalues) {
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_call_insn(c->ctx, 7,
                                        MIR_new_ref_op(c->ctx, c->take_open_upvalues_rebase_proto),
                                        MIR_new_ref_op(c->ctx, c->imp_take_open_upvalues_rebase),
                                        MIR_new_reg_op(c->ctx, c->r_vm),
                                        MIR_new_reg_op(c->ctx, c->r_jit_open_upvalues),
                                        MIR_new_reg_op(c->ctx, c->r_args),
                                        MIR_new_reg_op(c->ctx, c->r_slotbuf),
                                        MIR_new_int_op(c->ctx, c->param_count)));
    }

    if (c->has_captures) {
      MIR_reg_t r_osr_lp = MIR_new_func_reg(c->ctx, c->jit_func->u.func,
                                            MIR_T_I64, "osr_lp");
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_MOV,
                                   MIR_new_reg_op(c->ctx, r_osr_lp),
                                   MIR_new_mem_op(c->ctx, MIR_T_P,
                                                  osr_base + (MIR_disp_t)offsetof(sv_jit_osr_t, lp),
                                                  c->r_vm, 0, 1)));

      if (c->use_unified_slotbuf) {
        for (int i = 0; i < c->n_locals; i++)
          if (c->captured_locals && c->captured_locals[i])
            MIR_append_insn(c->ctx, c->jit_func,
                            MIR_new_insn(c->ctx, MIR_MOV,
                                         MIR_new_mem_op(c->ctx, MIR_T_I64,
                                                        (MIR_disp_t)(i * (int)sizeof(ant_value_t)),
                                                        c->r_lbuf, 0, 1),
                                         MIR_new_reg_op(c->ctx, c->local_regs[i])));
      } else {
        MIR_append_insn(c->ctx, c->jit_func,
                        MIR_new_insn(c->ctx, MIR_MOV,
                                     MIR_new_reg_op(c->ctx, c->r_lbuf),
                                     MIR_new_reg_op(c->ctx, r_osr_lp)));
      }

      if (c->r_jit_open_upvalues) {
        if (c->use_unified_slotbuf)
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_call_insn(c->ctx, 7,
                                            MIR_new_ref_op(c->ctx, c->take_open_upvalues_rebase_proto),
                                            MIR_new_ref_op(c->ctx, c->imp_take_open_upvalues_rebase),
                                            MIR_new_reg_op(c->ctx, c->r_vm),
                                            MIR_new_reg_op(c->ctx, c->r_jit_open_upvalues),
                                            MIR_new_reg_op(c->ctx, r_osr_lp),
                                            MIR_new_reg_op(c->ctx, c->r_lbuf),
                                            MIR_new_int_op(c->ctx, c->n_locals)));
        else
          MIR_append_insn(c->ctx, c->jit_func,
                          MIR_new_call_insn(c->ctx, 6,
                                            MIR_new_ref_op(c->ctx, c->take_open_upvalues_proto),
                                            MIR_new_ref_op(c->ctx, c->imp_take_open_upvalues),
                                            MIR_new_reg_op(c->ctx, c->r_vm),
                                            MIR_new_reg_op(c->ctx, c->r_jit_open_upvalues),
                                            MIR_new_reg_op(c->ctx, c->r_lbuf),
                                            MIR_new_int_op(c->ctx, c->n_locals)));
      }
    }

    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV,
                                 MIR_new_mem_op(c->ctx, MIR_T_U8,
                                                osr_base + (MIR_disp_t)offsetof(sv_jit_osr_t, active),
                                                c->r_vm, 0, 1),
                                 MIR_new_int_op(c->ctx, 0)));

    MIR_reg_t r_osr_off = MIR_new_func_reg(c->ctx, c->jit_func->u.func,
                                           MIR_T_I64, "osr_off");
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV,
                                 MIR_new_reg_op(c->ctx, r_osr_off),
                                 MIR_new_mem_op(c->ctx, MIR_T_I32,
                                                osr_base + (MIR_disp_t)offsetof(sv_jit_osr_t, bc_offset),
                                                c->r_vm, 0, 1)));

    for (int i = 0; i < c->osr_map.count; i++) {
      MIR_label_t target_lbl = label_for_offset(c->ctx, &c->lm, c->osr_map.offsets[i]);
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, target_lbl),
                                   MIR_new_reg_op(c->ctx, r_osr_off),
                                   MIR_new_int_op(c->ctx, c->osr_map.offsets[i])));
    }

    MIR_append_insn(c->ctx, c->jit_func, normal_entry);
  }
  jit_emit_cold_entry_counter(c, "call");

  memset(c->jit_try_stack, 0, sizeof(c->jit_try_stack));
  c->jit_try_depth = 0;

  memset(c->catch_sp_map, 0, sizeof(c->catch_sp_map));
  c->catch_sp_count = 0;

  c->r_result = MIR_new_func_reg(c->ctx, c->jit_func->u.func, MIR_JSVAL, "result");
  mir_load_imm(c->ctx, c->jit_func, c->r_result, mkval(kTypeUndefined, 0));

  c->ip = c->func->code;
  c->end = c->func->code + c->func->code_len;

  c->integer_locals = calloc((size_t)c->n_locals, sizeof(MIR_reg_t));
  c->integer_local_ranges = calloc((size_t)c->n_locals, sizeof(jit_integer_range_t));
  c->local_reg_limit = 0;
  for (int i = 0; i < c->n_locals; i++) {
    if (c->local_regs[i] > c->local_reg_limit) c->local_reg_limit = c->local_regs[i];
    if (c->local_d_regs[i] > c->local_reg_limit) c->local_reg_limit = c->local_d_regs[i];
  }
  c->local_by_reg = calloc((size_t)c->local_reg_limit + 1, sizeof(int));
  if (c->local_by_reg) {
    for (int i = 0; i < c->n_locals; i++) {
      c->local_by_reg[c->local_regs[i]] = i + 1;
      c->local_by_reg[c->local_d_regs[i]] = i + 1;
    }
  } else {
    free(c->integer_locals);
    c->integer_locals = NULL;
  }
  c->integer_local_site = 0;
  c->element_available = false;

  c->ok = true;
  c->call_n = 0;
  c->upval_n = 0;
  c->arith_n = 0;
  c->reg_site_n = 0;

  if (c->normalize_sloppy_this) {
    MIR_label_t normalize_global = MIR_new_label(c->ctx);
    MIR_label_t normalize_box = MIR_new_label(c->ctx);
    MIR_label_t normalize_done = MIR_new_label(c->ctx);
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_UBLE,
                                 MIR_new_label_op(c->ctx, normalize_box),
                                 MIR_new_reg_op(c->ctx, c->r_this_curr),
                                 MIR_new_uint_op(c->ctx, NANBOX_PREFIX)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_URSH,
                                 MIR_new_reg_op(c->ctx, c->r_bool),
                                 MIR_new_reg_op(c->ctx, c->r_this_curr),
                                 MIR_new_int_op(c->ctx, NANBOX_TYPE_SHIFT)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_BEQ,
                                 MIR_new_label_op(c->ctx, normalize_global),
                                 MIR_new_reg_op(c->ctx, c->r_bool),
                                 MIR_new_uint_op(c->ctx,
                                                 (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)kTypeUndefined)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_BEQ,
                                 MIR_new_label_op(c->ctx, normalize_global),
                                 MIR_new_reg_op(c->ctx, c->r_bool),
                                 MIR_new_uint_op(c->ctx,
                                                 (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)kTypeNull)));
    const uint8_t boxed_types[] = {
        kTypeString, kTypeBool, kTypeBigInt, kTypeSymbol};
    for (size_t i = 0; i < sizeof(boxed_types); i++) {
      MIR_append_insn(c->ctx, c->jit_func,
                      MIR_new_insn(c->ctx, MIR_BEQ,
                                   MIR_new_label_op(c->ctx, normalize_box),
                                   MIR_new_reg_op(c->ctx, c->r_bool),
                                   MIR_new_uint_op(c->ctx,
                                                   (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) |
                                                       (uint64_t)boxed_types[i])));
    }
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, normalize_done)));

    MIR_append_insn(c->ctx, c->jit_func, normalize_global);
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV,
                                 MIR_new_reg_op(c->ctx, c->r_this_curr),
                                 MIR_new_mem_op(c->ctx, MIR_JSVAL,
                                                (MIR_disp_t)offsetof(ant_t, global), c->r_js, 0, 1)));
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, normalize_done)));

    MIR_append_insn(c->ctx, c->jit_func, normalize_box);
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_call_insn(c->ctx, 5,
                                      MIR_new_ref_op(c->ctx, c->normalize_this_proto),
                                      MIR_new_ref_op(c->ctx, c->imp_normalize_this),
                                      MIR_new_reg_op(c->ctx, c->r_this_curr),
                                      MIR_new_reg_op(c->ctx, c->r_js),
                                      MIR_new_reg_op(c->ctx, c->r_this_curr)));
    jit_emit_throw_if_error(c, c->r_this_curr);
    MIR_append_insn(c->ctx, c->jit_func, normalize_done);
    MIR_append_insn(c->ctx, c->jit_func,
                    MIR_new_insn(c->ctx, MIR_MOV,
                                 MIR_new_mem_op(c->ctx, MIR_JSVAL, 0, c->r_this_root, 0, 1),
                                 MIR_new_reg_op(c->ctx, c->r_this_curr)));
  }

  return true;
}
