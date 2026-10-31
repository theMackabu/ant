L_CONST: {
  sv_op_const(vm, func, ip);
  NEXT(OP_CONST);
}

L_CONST_I8: {
  sv_op_const_i8(vm, ip);
  NEXT(OP_CONST_I8);
}

L_CONST8: {
  sv_op_const8(vm, func, ip);
  NEXT(OP_CONST8);
}

L_UNDEF: {
  sv_op_undef(vm);
  NEXT(OP_UNDEF);
}

L_NULL: {
  sv_op_null(vm);
  NEXT(OP_NULL);
}

L_TRUE: {
  sv_op_true(vm);
  NEXT(OP_TRUE);
}

L_FALSE: {
  sv_op_false(vm);
  NEXT(OP_FALSE);
}

L_THIS: {
  sv_op_this(vm, frame);
  NEXT(OP_THIS);
}

L_GLOBAL: {
  sv_op_global(vm, js);
  NEXT(OP_GLOBAL);
}

L_OBJECT: {
  VM_CHECK(sv_op_object(vm, js, func, ip));
  NEXT(OP_OBJECT);
}

L_ARRAY: {
  VM_CHECK(sv_op_array(vm, js, ip));
  NEXT(OP_ARRAY);
}

L_SET_BRAND: {
  sv_op_set_brand(vm, ip);
  NEXT(OP_SET_BRAND);
}

L_REGEXP: {
  sv_op_regexp(vm, js);
  NEXT(OP_REGEXP);
}

L_CLOSURE: {
  VM_CHECK(sv_op_closure(vm, js, frame, func, ip));
  NEXT(OP_CLOSURE);
}

L_CLOSURE_EVAL: {
  VM_CHECK(sv_op_closure(vm, js, frame, func, ip));
  NEXT(OP_CLOSURE_EVAL);
}

L_CLOSURE_ARGS: {
  VM_CHECK(sv_op_closure_args(vm, js, frame, func, ip));
  NEXT(OP_CLOSURE_ARGS);
}

L_INIT_EVAL_ENV: {
  VM_CHECK(sv_eval_init_variable_env(vm, js, frame));
  NEXT(OP_INIT_EVAL_ENV);
}

L_PUT_EVAL_FUNCTION: {
  sv_atom_t *name = &func->atoms[sv_get_u32(ip + 1)];
  VM_CHECK(sv_eval_store_function(js, sv_frame_eval_env(js, frame),
    name->str, name->len, vm->stack[--vm->sp]));
  NEXT(OP_PUT_EVAL_FUNCTION);
}

L_INIT_GLOBAL_LEX: {
  sv_atom_t *name = &func->atoms[sv_get_u32(ip + 1)];
  ant_global_lexical_t *lex = sv_global_lexical(js, name->str);
  ant_value_t val = vm->stack[--vm->sp];
  VM_CHECK(lex ? sv_global_lexical_set(js, lex, val, true) : js_mkerr(js, "missing global lexical"));
  NEXT(OP_INIT_GLOBAL_LEX);
}

L_PRIVATE_TOKEN: {
  VM_CHECK(sv_op_private_token(vm, js, ip));
  NEXT(OP_PRIVATE_TOKEN);
}

L_POP: {
  sv_op_pop(vm);
  NEXT(OP_POP);
}

L_DUP: {
  sv_op_dup(vm);
  NEXT(OP_DUP);
}

L_DUP2: {
  sv_op_dup2(vm);
  NEXT(OP_DUP2);
}

L_SWAP: {
  sv_op_swap(vm);
  NEXT(OP_SWAP);
}

L_ROT3L: {
  sv_op_rot3l(vm);
  NEXT(OP_ROT3L);
}

L_ROT3R: {
  sv_op_rot3r(vm);
  NEXT(OP_ROT3R);
}

L_NIP: {
  sv_op_nip(vm);
  NEXT(OP_NIP);
}

L_NIP2: {
  sv_op_nip2(vm);
  NEXT(OP_NIP2);
}

L_INSERT2: {
  sv_op_insert2(vm);
  NEXT(OP_INSERT2);
}

L_INSERT3: {
  sv_op_insert3(vm);
  NEXT(OP_INSERT3);
}

L_SWAP_UNDER: {
  sv_op_swap_under(vm);
  NEXT(OP_SWAP_UNDER);
}

L_ROT4_UNDER: {
  sv_op_rot4_under(vm);
  NEXT(OP_ROT4_UNDER);
}

L_GET_LOCAL: {
  VM_CHECK(sv_op_get_local(vm, lp, js, frame, ip));
  NEXT(OP_GET_LOCAL);
}

L_PUT_LOCAL: {
  sv_op_put_local(vm, lp, frame, func, ip);
  NEXT(OP_PUT_LOCAL);
}

L_SET_LOCAL: {
  sv_op_set_local(vm, lp, frame, func, ip);
  NEXT(OP_SET_LOCAL);
}

L_GET_LOCAL8: {
  VM_CHECK(sv_op_get_local8(vm, lp, js, frame, ip));
  NEXT(OP_GET_LOCAL8);
}

L_PUT_LOCAL8: {
  sv_op_put_local8(vm, lp, frame, func, ip);
  NEXT(OP_PUT_LOCAL8);
}

L_SET_LOCAL8: {
  sv_op_set_local8(vm, lp, frame, func, ip);
  NEXT(OP_SET_LOCAL8);
}

L_SET_LOCAL_UNDEF: {
  sv_op_set_local_undef(frame, lp, ip);
  NEXT(OP_SET_LOCAL_UNDEF);
}

L_GET_LOCAL_CHK: {
  VM_CHECK(sv_op_get_local_chk(vm, lp, js, frame, func, ip));
  NEXT(OP_GET_LOCAL_CHK);
}

L_PUT_LOCAL_CHK: {
  VM_CHECK(sv_op_put_local_chk(vm, lp, js, frame, func, ip));
  NEXT(OP_PUT_LOCAL_CHK);
}

L_GET_SLOT_RAW: {
  VM_CHECK(sv_op_get_slot_raw(vm, js, frame, ip));
  NEXT(OP_GET_SLOT_RAW);
}

L_GET_ARG: {
  VM_CHECK(sv_op_get_arg(vm, js, frame, ip));
  NEXT(OP_GET_ARG);
}

L_PUT_ARG: {
  sv_op_put_arg(vm, js, frame, ip);
  NEXT(OP_PUT_ARG);
}

L_SET_ARG: {
  sv_op_set_arg(vm, js, frame, ip);
  NEXT(OP_SET_ARG);
}

L_REST: {
  sv_op_rest(vm, frame, js, ip);
  NEXT(OP_REST);
}

L_GET_UPVAL: {
  VM_CHECK(sv_op_get_upval(vm, frame, js, ip));
  NEXT(OP_GET_UPVAL);
}

L_GET_UPVAL_VAL: {
  sv_op_get_upval_val(vm, frame, ip);
  NEXT(OP_GET_UPVAL_VAL);
}

L_PUT_UPVAL: {
  VM_CHECK(sv_op_put_upval(vm, frame, ip));
  NEXT(OP_PUT_UPVAL);
}

L_SET_UPVAL: {
  VM_CHECK(sv_op_set_upval(vm, frame, ip));
  NEXT(OP_SET_UPVAL);
}

L_CLOSE_UPVAL: {
  VM_CHECK(sv_op_close_upval(vm, frame, ip));
  NEXT(OP_CLOSE_UPVAL);
}

L_GET_GLOBAL: {
  VM_CHECK(sv_op_get_global(vm, js, func, ip));
  NEXT(OP_GET_GLOBAL);
}

L_GET_GLOBAL_UNDEF: {
  VM_CHECK(sv_op_get_global_undef(vm, js, func, ip));
  NEXT(OP_GET_GLOBAL_UNDEF);
}

L_PUT_GLOBAL: {
  VM_CHECK(sv_op_put_global(vm, js, frame, func, ip));
  NEXT(OP_PUT_GLOBAL);
}

L_LOAD_STABLE_BUILTIN: {
  ant_value_t receiver;
  ant_value_t stable_func = sv_load_stable_builtin(
    js, (sv_stable_builtin_t)ip[1], &receiver);
  if (is_err(stable_func)) VM_THROW(stable_func);
  vm->stack[vm->sp++] = receiver;
  vm->stack[vm->sp++] = stable_func;
  NEXT(OP_LOAD_STABLE_BUILTIN);
}

L_GET_FIELD: {
  VM_CHECK(sv_op_get_field(vm, js, func, ip));
  NEXT(OP_GET_FIELD);
}

L_GET_FIELD2: {
  VM_CHECK(sv_op_get_field2(vm, js, func, ip));
  NEXT(OP_GET_FIELD2);
}

L_PUT_FIELD: {
  VM_CHECK(sv_op_put_field(vm, js, func, ip));
  NEXT(OP_PUT_FIELD);
}

L_GET_ELEM: {
  sv_tfb_record_get_elem(func, ip, vm->stack[vm->sp - 2], vm->stack[vm->sp - 1]);
  VM_CHECK(sv_op_get_elem(vm, js, func, ip));
  NEXT(OP_GET_ELEM);
}

L_GET_ELEM2: {
  VM_CHECK(sv_op_get_elem2(vm, js, func, ip));
  NEXT(OP_GET_ELEM2);
}

L_PUT_ELEM: {
  sv_tfb_record_dense_numeric_put(
    func, ip,
    vm->stack[vm->sp - 3], vm->stack[vm->sp - 2],
    vm->stack[vm->sp - 1]
  );
  VM_CHECK(sv_op_put_elem(vm, js));
  NEXT(OP_PUT_ELEM);
}

L_DEFINE_FIELD: {
  sv_op_define_field(vm, js, func, ip);
  NEXT(OP_DEFINE_FIELD);
}

L_DEFINE_SLOT: {
  sv_op_define_slot(vm, js, func, ip);
  NEXT(OP_DEFINE_SLOT);
}

L_GET_LENGTH: {
  VM_CHECK(sv_op_get_length(vm, js, func, ip));
  NEXT(OP_GET_LENGTH);
}

L_GET_FIELD_OPT: {
  VM_CHECK(sv_op_get_field_opt(vm, js, func, ip));
  NEXT(OP_GET_FIELD_OPT);
}

L_GET_ELEM_OPT: {
  VM_CHECK(sv_op_get_elem_opt(vm, js, func, ip));
  NEXT(OP_GET_ELEM_OPT);
}

L_GET_PRIVATE: {
  VM_CHECK(sv_op_get_private(vm, js));
  NEXT(OP_GET_PRIVATE);
}

L_GET_PRIVATE_OPT: {
  VM_CHECK(sv_op_get_private_opt(vm, js));
  NEXT(OP_GET_PRIVATE_OPT);
}

L_PUT_PRIVATE: {
  VM_CHECK(sv_op_put_private(vm, js));
  NEXT(OP_PUT_PRIVATE);
}

L_DEF_PRIVATE: {
  VM_CHECK(sv_op_def_private(vm, js, ip));
  NEXT(OP_DEF_PRIVATE);
}

L_HAS_PRIVATE: {
  VM_CHECK(sv_op_has_private(vm, js));
  NEXT(OP_HAS_PRIVATE);
}

L_GET_SUPER: {
  sv_op_get_super(vm, js);
  NEXT(OP_GET_SUPER);
}

L_GET_SUPER_VAL: {
  sv_op_get_super_val(vm, js, frame);
  NEXT(OP_GET_SUPER_VAL);
}

L_PUT_SUPER_VAL: {
  sv_op_put_super_val(vm, js);
  NEXT(OP_PUT_SUPER_VAL);
}

L_ADD: {
  ant_value_t r = vm->stack[vm->sp - 1], l = vm->stack[vm->sp - 2];
  sv_tfb_record2(func, ip, l, r);
  if (__builtin_expect(vtype(l) == kTypeNumber && vtype(r) == kTypeNumber, 1)) {
    vm->sp--; vm->stack[vm->sp - 1] = Ant_Value_FromNumber(Ant_Value_AsNumberUnchecked(l) + Ant_Value_AsNumberUnchecked(r)); NEXT(OP_ADD);
  }
  VM_CHECK(sv_op_add(vm, js)); NEXT(OP_ADD);
}

L_ADD_NUM: {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[vm->sp - 1];
  vm->stack[vm->sp - 1] = Ant_Value_FromNumber(Ant_Value_AsNumberUnchecked(l) + Ant_Value_AsNumberUnchecked(r));
  NEXT(OP_ADD_NUM);
}

L_SUB: {
  ant_value_t r = vm->stack[vm->sp - 1], l = vm->stack[vm->sp - 2];
  sv_tfb_record2(func, ip, l, r);
  if (__builtin_expect(vtype(l) == kTypeNumber && vtype(r) == kTypeNumber, 1)) {
    vm->sp--; vm->stack[vm->sp - 1] = Ant_Value_FromNumber(Ant_Value_AsNumberUnchecked(l) - Ant_Value_AsNumberUnchecked(r)); NEXT(OP_SUB);
  }
  VM_CHECK(sv_op_sub(vm, js)); NEXT(OP_SUB);
}

L_SUB_NUM: {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[vm->sp - 1];
  vm->stack[vm->sp - 1] = Ant_Value_FromNumber(Ant_Value_AsNumberUnchecked(l) - Ant_Value_AsNumberUnchecked(r));
  NEXT(OP_SUB_NUM);
}

L_MUL_NUM: {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[vm->sp - 1];
  vm->stack[vm->sp - 1] = Ant_Value_FromNumber(Ant_Value_AsNumberUnchecked(l) * Ant_Value_AsNumberUnchecked(r));
  NEXT(OP_MUL_NUM);
}

L_DIV_NUM: {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[vm->sp - 1];
  vm->stack[vm->sp - 1] = Ant_Value_FromNumber(Ant_Value_AsNumberUnchecked(l) / Ant_Value_AsNumberUnchecked(r));
  NEXT(OP_DIV_NUM);
}

L_MUL: {
  sv_tfb_record2(func, ip, vm->stack[vm->sp-2], vm->stack[vm->sp-1]);
  VM_CHECK(sv_op_mul(vm, js));
  NEXT(OP_MUL);
}

L_DIV: {
  sv_tfb_record2(func, ip, vm->stack[vm->sp-2], vm->stack[vm->sp-1]);
  VM_CHECK(sv_op_div(vm, js));
  NEXT(OP_DIV);
}

L_MOD: {
  sv_tfb_record2(func, ip, vm->stack[vm->sp-2], vm->stack[vm->sp-1]);
  VM_CHECK(sv_op_mod(vm, js));
  NEXT(OP_MOD);
}

L_NEG: {
  sv_tfb_record1(func, ip, vm->stack[vm->sp-1]);
  VM_CHECK(sv_op_neg(vm, js));
  NEXT(OP_NEG);
}

L_ADD_LOCAL: {
  sv_tfb_record2(func, ip, lp[sv_get_u8(ip+1)], vm->stack[vm->sp-1]);
  VM_CHECK(sv_op_add_local(vm, lp, js, func, ip));
  NEXT(OP_ADD_LOCAL);
}

L_STR_APPEND_LOCAL: {
  VM_CHECK(sv_op_str_append_local(vm, js, frame, func, ip));
  NEXT(OP_STR_APPEND_LOCAL);
}

L_STR_ALC_SNAPSHOT: {
  VM_CHECK(sv_op_str_append_local_snapshot(vm, js, frame, func, ip));
  NEXT(OP_STR_ALC_SNAPSHOT);
}

L_STR_FLUSH_LOCAL: {
  VM_CHECK(sv_op_str_flush_local(vm, js, frame, ip));
  NEXT(OP_STR_FLUSH_LOCAL);
}

L_EXP: {
  VM_CHECK(sv_op_exp(vm, js));
  NEXT(OP_EXP);
}

L_UPLUS: {
  VM_CHECK(sv_op_uplus(vm, js));
  NEXT(OP_UPLUS);
}

L_INC: {
  ant_value_t v = vm->stack[vm->sp - 1];
  if (__builtin_expect(vtype(v) == kTypeNumber, 1)) vm->stack[vm->sp - 1] = Ant_Value_FromNumber(Ant_Value_AsNumberUnchecked(v) + 1.0);
  else VM_CHECK(sv_op_update_local_slow(&vm->stack[vm->sp - 1], js, true));
  NEXT(OP_INC);
}

L_DEC: {
  ant_value_t v = vm->stack[vm->sp - 1];
  if (__builtin_expect(vtype(v) == kTypeNumber, 1)) vm->stack[vm->sp - 1] = Ant_Value_FromNumber(Ant_Value_AsNumberUnchecked(v) - 1.0);
  else VM_CHECK(sv_op_update_local_slow(&vm->stack[vm->sp - 1], js, false));
  NEXT(OP_DEC);
}

L_POST_INC: {
  VM_CHECK(sv_op_post_update(vm, js, true));
  NEXT(OP_POST_INC);
}

L_POST_DEC: {
  VM_CHECK(sv_op_post_update(vm, js, false));
  NEXT(OP_POST_DEC);
}

L_INC_LOCAL: {
  VM_CHECK(sv_op_update_local(lp, js, func, ip, true));
  NEXT(OP_INC_LOCAL);
}

L_DEC_LOCAL: {
  VM_CHECK(sv_op_update_local(lp, js, func, ip, false));
  NEXT(OP_DEC_LOCAL);
}

L_EQ: {
  ant_value_t l = vm->stack[vm->sp - 2];
  ant_value_t r = vm->stack[vm->sp - 1];
  sv_tfb_record2_spec(func, ip, l, r, false);
  VM_CHECK(sv_op_eq(vm, js)); NEXT(OP_EQ);
}

L_NE: {
  ant_value_t l = vm->stack[vm->sp - 2];
  ant_value_t r = vm->stack[vm->sp - 1];
  sv_tfb_record2_spec(func, ip, l, r, false);
  VM_CHECK(sv_op_ne(vm, js)); NEXT(OP_NE);
}

L_SEQ: {
  ant_value_t l = vm->stack[vm->sp - 2];
  ant_value_t r = vm->stack[vm->sp - 1];
  sv_tfb_record2_spec(func, ip, l, r, false);
  sv_op_seq(vm, js); NEXT(OP_SEQ);
}

L_SNE: {
  ant_value_t l = vm->stack[vm->sp - 2];
  ant_value_t r = vm->stack[vm->sp - 1];
  sv_tfb_record2_spec(func, ip, l, r, false);
  sv_op_sne(vm, js); NEXT(OP_SNE);
}

L_LT: {
  ant_value_t r = vm->stack[vm->sp - 1], l = vm->stack[vm->sp - 2];
  sv_tfb_record2(func, ip, l, r);
  if (__builtin_expect(vtype(l) == kTypeNumber && vtype(r) == kTypeNumber, 1)) {
    vm->sp--; vm->stack[vm->sp - 1] = mkval(kTypeBool, Ant_Value_AsNumberUnchecked(l) < Ant_Value_AsNumberUnchecked(r)); NEXT(OP_LT);
  }
  VM_CHECK(sv_op_lt(vm, js)); NEXT(OP_LT);
}

L_LE: {
  sv_tfb_record2(func, ip, vm->stack[vm->sp-2], vm->stack[vm->sp-1]);
  VM_CHECK(sv_op_le(vm, js));
  NEXT(OP_LE);
}

L_GT: {
  sv_tfb_record2(func, ip, vm->stack[vm->sp-2], vm->stack[vm->sp-1]);
  VM_CHECK(sv_op_gt(vm, js));
  NEXT(OP_GT);
}

L_GE: {
  sv_tfb_record2(func, ip, vm->stack[vm->sp-2], vm->stack[vm->sp-1]);
  VM_CHECK(sv_op_ge(vm, js));
  NEXT(OP_GE);
}

L_INSTANCEOF: {
  VM_CHECK(sv_op_instanceof(vm, js, func, ip));
  NEXT(OP_INSTANCEOF);
}

L_IN: {
  VM_CHECK(sv_op_in(vm, js));
  NEXT(OP_IN);
}

L_IN_FIELD: {
  VM_CHECK(sv_op_in_field(vm, js, func, ip));
  NEXT(OP_IN_FIELD);
}

L_IS_NULLISH: {
  sv_op_is_nullish(vm);
  NEXT(OP_IS_NULLISH);
}

L_IS_UNDEF_OR_NULL: {
  sv_op_is_undef_or_null(vm);
  NEXT(OP_IS_UNDEF_OR_NULL);
}

L_BAND: {
  ant_value_t l = vm->stack[vm->sp - 2];
  ant_value_t r = vm->stack[vm->sp - 1];
  sv_tfb_record2_spec(func, ip, l, r, true);
  VM_CHECK(sv_op_band(vm, js)); NEXT(OP_BAND);
}

L_BOR: {
  ant_value_t l = vm->stack[vm->sp - 2];
  ant_value_t r = vm->stack[vm->sp - 1];
  sv_tfb_record2_spec(func, ip, l, r, true);
  VM_CHECK(sv_op_bor(vm, js)); NEXT(OP_BOR);
}

L_BXOR: {
  ant_value_t l = vm->stack[vm->sp - 2];
  ant_value_t r = vm->stack[vm->sp - 1];
  sv_tfb_record2_spec(func, ip, l, r, true);
  VM_CHECK(sv_op_bxor(vm, js)); NEXT(OP_BXOR);
}

L_SHL: {
  ant_value_t l = vm->stack[vm->sp - 2];
  ant_value_t r = vm->stack[vm->sp - 1];
  sv_tfb_record2_spec(func, ip, l, r, true);
  VM_CHECK(sv_op_shl(vm, js)); NEXT(OP_SHL);
}

L_SHR: {
  ant_value_t l = vm->stack[vm->sp - 2];
  ant_value_t r = vm->stack[vm->sp - 1];
  sv_tfb_record2_spec(func, ip, l, r, true);
  VM_CHECK(sv_op_shr(vm, js)); NEXT(OP_SHR);
}

L_USHR: {
  ant_value_t l = vm->stack[vm->sp - 2];
  ant_value_t r = vm->stack[vm->sp - 1];
  sv_tfb_record2_spec(func, ip, l, r, true);
  VM_CHECK(sv_op_ushr(vm, js)); NEXT(OP_USHR);
}

L_BNOT: {
  ant_value_t value = vm->stack[vm->sp - 1];
  sv_tfb_record1_word32_spec(func, ip, value);
  VM_CHECK(sv_op_bnot(vm, js));
  NEXT(OP_BNOT);
}

L_NOT: {
  sv_op_not(vm, js);
  NEXT(OP_NOT);
}

L_TYPEOF: {
  sv_op_typeof(vm, js);
  NEXT(OP_TYPEOF);
}

L_IS_PRIMITIVE_TYPE: {
  vm->stack[vm->sp - 1] = Ant_Value_FromBool(js_typeof_is(vm->stack[vm->sp - 1], ip[1]));
  NEXT(OP_IS_PRIMITIVE_TYPE);
}

L_VOID: {
  sv_op_void(vm);
  NEXT(OP_VOID);
}

L_DELETE: {
  VM_CHECK(sv_op_delete(vm, js));
  NEXT(OP_DELETE);
}

L_DELETE_VAR: {
  sv_op_delete_var(vm, js, func, ip);
  NEXT(OP_DELETE_VAR);
}

L_JMP: {
  uint8_t *prev = ip; ip = sv_op_jmp(ip);
  if (ip <= prev) {
    if (__builtin_expect(gc_safepoint(js), 0)) goto sv_interrupt;
    JIT_OSR_BACK_EDGE();
  }
  DISPATCH();
}

L_JMP_FALSE: {
  uint8_t *prev = ip;
  ant_value_t v = vm->stack[--vm->sp];
  if (__builtin_expect(vtype(v) == kTypeBool, 1)) {
    ip = vdata(v)
      ? ip + sv_op_size[OP_JMP_FALSE]
      : ip + sv_op_size[OP_JMP_FALSE] + sv_get_i32(ip + 1);
  } else {
    ip = js_truthy(js, v)
      ? ip + sv_op_size[OP_JMP_FALSE]
      : ip + sv_op_size[OP_JMP_FALSE] + sv_get_i32(ip + 1);
  }
  if (ip <= prev) JIT_OSR_BACK_EDGE();
  DISPATCH();
}

L_JMP_TRUE: {
  uint8_t *prev = ip;
  ant_value_t v = vm->stack[--vm->sp];
  if (__builtin_expect(vtype(v) == kTypeBool, 1)) {
    ip = vdata(v)
      ? ip + sv_op_size[OP_JMP_TRUE] + sv_get_i32(ip + 1)
      : ip + sv_op_size[OP_JMP_TRUE];
  } else {
    ip = js_truthy(js, v)
      ? ip + sv_op_size[OP_JMP_TRUE] + sv_get_i32(ip + 1)
      : ip + sv_op_size[OP_JMP_TRUE];
  }
  if (ip <= prev) JIT_OSR_BACK_EDGE();
  DISPATCH();
}

L_JMP_FALSE_PEEK: {
  uint8_t *prev = ip; ip = sv_op_jmp_false_peek(vm, js, ip);
  if (ip <= prev) JIT_OSR_BACK_EDGE();
  DISPATCH();
}

L_JMP_TRUE_PEEK: {
  uint8_t *prev = ip; ip = sv_op_jmp_true_peek(vm, js, ip);
  if (ip <= prev) JIT_OSR_BACK_EDGE();
  DISPATCH();
}

L_JMP_NOT_NULLISH: {
  uint8_t *prev = ip; ip = sv_op_jmp_not_nullish(vm, ip);
  if (ip <= prev) JIT_OSR_BACK_EDGE();
  DISPATCH();
}

L_JMP8: {
  uint8_t *prev = ip; ip = sv_op_jmp8(ip);
  if (ip <= prev) JIT_OSR_BACK_EDGE();
  DISPATCH();
}

L_JMP_FALSE8: {
  uint8_t *prev = ip; ip = sv_op_jmp_false8(vm, js, ip);
  if (ip <= prev) {
    JIT_OSR_BACK_EDGE();
  }
  DISPATCH();
}

L_JMP_TRUE8:  {
  uint8_t *prev = ip; ip = sv_op_jmp_true8(vm, js, ip);
  if (ip <= prev) {
    JIT_OSR_BACK_EDGE();
  }
  DISPATCH();
}

L_CALL: {
  uint16_t call_argc = sv_get_u16(ip + 1);
  ant_value_t *call_args = &vm->stack[vm->sp - call_argc];
  ant_value_t call_func = vm->stack[vm->sp - call_argc - 1];

  bool is_super_call =
    (vtype(frame->super_val) != kTypeUndefined && call_func == frame->super_val);
  ant_value_t call_this = is_super_call ? frame->this : Ant_Value_Undefined;

  if (!is_super_call && vtype(frame->new_target) == kTypeUndefined && vtype(call_func) == kTypeFunction) {
    sv_closure_t *closure = js_func_closure(call_func);
    if (closure->func != NULL) {
      if (closure->call_flags & SV_CALL_HAS_BOUND_ARGS)
        goto call_fallback;
      if (closure->func->is_async && !closure->func->is_generator && closure->func->realm == js->realm) {
        sv_async_record_await_call(func, ip, sv_op_size[OP_CALL], closure->func);
        ant_value_t enter = sv_async_inline_call(
          vm, js, closure, call_func, call_this, call_args, (int)call_argc,
          (int)call_argc + 1, ip + sv_op_size[OP_CALL]);
        if (is_err(enter)) VM_THROW(enter);
        VM_RELOAD_FRAME();
        ip = func->code;
        DISPATCH();
      }
      if (closure->func->is_async || closure->func->is_generator) goto call_fallback;
      if (__builtin_expect(closure->func->realm != js->realm, 0)) goto call_fallback;
      {
        ant_value_t jit_this = sv_closure_has_lexical_this(closure)
          ? closure->bound_this : Ant_Value_Undefined;
        ant_value_t jit_result = sv_try_direct_closure_jit(
          vm, js, func, ip, frame, closure, jit_this, call_args, (int)call_argc);
        if (jit_result != SV_JIT_RETRY_INTERP) {
          vm->sp -= call_argc + 1;
          if (is_err(jit_result)) VM_THROW(jit_result);
          vm->stack[vm->sp++] = jit_result;
          ip = frame->ip + sv_op_size[OP_CALL];
          DISPATCH();
        }
      }
      if (vm->fp + 1 >= vm->max_frames && !sv_vm_grow_frames(vm)) VM_THROW(Ant_Error_CreateStackOverflow(js));
      if (sv_closure_has_lexical_this(closure))
        call_this = closure->bound_this;
      frame = &vm->frames[vm->fp];
      frame->ip = ip + sv_op_size[OP_CALL];
      vm->sp -= call_argc + 1;
      vm->fp++;
      frame = &vm->frames[vm->fp];
      func = closure->func;
      frame->func = func;
      frame->callee = call_func;
      frame->this = call_this;
      frame->new_target = Ant_Value_Undefined;
      frame->super_val = closure->super_val;
      frame->prev_sp = vm->sp;
      frame->handler_base = (uint16_t)vm->handler_depth;
      frame->handler_top = (uint16_t)vm->handler_depth;
      frame->argc = call_argc;
      frame->completion.kind = SV_COMPLETION_NONE;
      frame->completion.value = Ant_Value_Undefined;
      frame->arguments_obj = Ant_Value_Undefined;
      frame->eval_env = sv_closure_eval_env(closure);
      ant_value_t *call_bp = NULL;
      ant_value_t *call_lp = NULL;
      ant_value_t call_stage_err = sv_stage_frame_args(
        vm, js, func, call_args, (int)call_argc, &call_bp, &call_lp
      );
      if (is_err(call_stage_err)) {
        vm->fp--;
        vm->sp += call_argc + 1;
        VM_THROW(call_stage_err);
      }
      frame->bp = call_bp;
      frame->lp = call_lp;
      frame->upvalues = closure->upvalues;
      frame->upvalue_count = closure->func->upvalue_count;
      frame->this = sv_normalize_this_for_frame(js, func, frame->this);
      lp = frame->lp;

      ip = func->code;
      DISPATCH();
    }
  }
  call_fallback:;
  frame->ip = ip;
  if (vtype(call_func) == kTypeBuiltin) sv_tfb_record_builtin_call(func, ip);
  ant_value_t super_this_c = call_this;
  ant_value_t call_result = sv_vm_call(
    vm, js, call_func, call_this, call_args, call_argc,
    is_super_call ? &super_this_c : NULL, is_super_call ? frame->new_target : Ant_Value_Undefined);
  VM_RELOAD_FRAME();
  vm->sp -= call_argc + 1;
  if (is_err(call_result)) VM_THROW(call_result);
  if (is_super_call)
    frame->this = is_object_type(call_result) ? call_result : super_this_c;
  vm->stack[vm->sp++] = call_result;
  NEXT(OP_CALL);
}

L_CALL_CALL: {
  uint8_t cc_n1 = ip[1];
  uint8_t cc_n2 = ip[2];
  int cc_total = 1 + (int)cc_n1 + (int)cc_n2;
  ant_value_t *cc_base = &vm->stack[vm->sp - cc_total];
  frame->ip = ip;
  ant_value_t cc_result = sv_op_call_call(
    vm, js, cc_base[0], cc_base + 1, (int)cc_n1, cc_base + 1 + cc_n1, (int)cc_n2);
  VM_RELOAD_FRAME();
  vm->sp -= cc_total;
  if (is_err(cc_result)) VM_THROW(cc_result);
  vm->stack[vm->sp++] = cc_result;
  NEXT(OP_CALL_CALL);
}

L_CALL_CALL_SLOT: {
  uint16_t cc_slot = sv_get_u16(ip + 1);
  ant_value_t cc_x = vm->stack[vm->sp - 2];
  ant_value_t cc_arg1 = vm->stack[vm->sp - 1];
  frame->ip = ip;
  ant_value_t cc_result = sv_op_call_call_slot(
    vm, js, cc_x, cc_arg1, cc_slot);
  VM_RELOAD_FRAME();
  vm->sp -= 2;
  if (is_err(cc_result)) VM_THROW(cc_result);
  vm->stack[vm->sp++] = cc_result;
  NEXT(OP_CALL_CALL_SLOT);
}

L_CALL_METHOD: {
  uint16_t call_argc = sv_get_u16(ip + 1);
  ant_value_t *call_args = &vm->stack[vm->sp - call_argc];
  ant_value_t call_func = vm->stack[vm->sp - call_argc - 1];
  ant_value_t call_this = vm->stack[vm->sp - call_argc - 2];

  bool is_super_call =
    (vtype(frame->super_val) != kTypeUndefined && call_func == frame->super_val);

  if (!is_super_call && vtype(frame->new_target) == kTypeUndefined &&
      vtype(call_func) == kTypeFunction) {
    sv_closure_t *closure = js_func_closure(call_func);
    if (closure->func != NULL) {
      if (closure->call_flags & SV_CALL_HAS_BOUND_ARGS)
        goto call_method_fallback;
      if (closure->func->is_async && !closure->func->is_generator && closure->func->realm == js->realm) {
        sv_async_record_await_call(func, ip, sv_op_size[OP_CALL_METHOD], closure->func);
        ant_value_t enter = sv_async_inline_call(
          vm, js, closure, call_func, call_this, call_args, (int)call_argc,
          (int)call_argc + 2, ip + sv_op_size[OP_CALL_METHOD]);
        if (is_err(enter)) VM_THROW(enter);
        VM_RELOAD_FRAME();
        ip = func->code;
        DISPATCH();
      }
      if (closure->func->is_async || closure->func->is_generator) goto call_method_fallback;
      if (__builtin_expect(closure->func->realm != js->realm, 0)) goto call_method_fallback;
      {
        ant_value_t jit_this = sv_closure_has_lexical_this(closure)
          ? closure->bound_this : call_this;
        ant_value_t jit_result = sv_try_direct_closure_jit(
          vm, js, func, ip, frame, closure, jit_this, call_args, (int)call_argc);
        if (jit_result != SV_JIT_RETRY_INTERP) {
          vm->sp -= call_argc + 2;
          if (is_err(jit_result)) VM_THROW(jit_result);
          vm->stack[vm->sp++] = jit_result;
          ip = frame->ip + sv_op_size[OP_CALL_METHOD];
          DISPATCH();
        }
      }
      if (vm->fp + 1 >= vm->max_frames && !sv_vm_grow_frames(vm)) VM_THROW(Ant_Error_CreateStackOverflow(js));
      if (sv_closure_has_lexical_this(closure))
        call_this = closure->bound_this;
      frame = &vm->frames[vm->fp];
      frame->ip = ip + sv_op_size[OP_CALL_METHOD];
      vm->sp -= call_argc + 2;
      vm->fp++;
      frame = &vm->frames[vm->fp];
      func = closure->func;
      frame->func = func;
      frame->callee = call_func;
      frame->this = call_this;
      frame->new_target = Ant_Value_Undefined;
      frame->super_val = closure->super_val;
      frame->prev_sp = vm->sp;
      frame->handler_base = (uint16_t)vm->handler_depth;
      frame->handler_top = (uint16_t)vm->handler_depth;
      frame->argc = call_argc;
      frame->completion.kind = SV_COMPLETION_NONE;
      frame->completion.value = Ant_Value_Undefined;
      frame->arguments_obj = Ant_Value_Undefined;
      frame->eval_env = sv_closure_eval_env(closure);
      ant_value_t *call_bp = NULL;
      ant_value_t *call_lp = NULL;
      ant_value_t call_stage_err = sv_stage_frame_args(
        vm, js, func, call_args, (int)call_argc, &call_bp, &call_lp
      );
      if (is_err(call_stage_err)) {
        vm->fp--;
        vm->sp += call_argc + 2;
        VM_THROW(call_stage_err);
      }
      frame->bp = call_bp;
      frame->lp = call_lp;
      frame->upvalues = closure->upvalues;
      frame->upvalue_count = closure->func->upvalue_count;
      frame->this = sv_normalize_this_for_frame(js, func, frame->this);
      lp = frame->lp;
      ip = func->code;
      DISPATCH();
    }
  }
  call_method_fallback:;
  frame->ip = ip;
  if (vtype(call_func) == kTypeBuiltin) sv_tfb_record_builtin_call(func, ip);
  ant_value_t super_this_cm = call_this;
  ant_value_t call_result = sv_vm_call(
    vm, js, call_func, call_this, call_args, call_argc,
    is_super_call ? &super_this_cm : NULL, is_super_call ? frame->new_target : Ant_Value_Undefined);
  VM_RELOAD_FRAME();
  vm->sp -= call_argc + 2;
  if (is_err(call_result)) VM_THROW(call_result);
  if (is_super_call)
    frame->this = is_object_type(call_result) ? call_result : super_this_cm;
  vm->stack[vm->sp++] = call_result;
  NEXT(OP_CALL_METHOD);
}

L_CALL_SUPER: {
  VM_CHECK(sv_op_call_super(vm, js, frame, ip));
  NEXT(OP_CALL_SUPER);
}

L_CALL_CHAR_CODE_AT: {
  uint16_t call_argc = sv_get_u16(ip + 1);
  ant_value_t *call_args = &vm->stack[vm->sp - call_argc];
  ant_value_t target = vm->stack[vm->sp - call_argc - 1];
  ant_value_t receiver = vm->stack[vm->sp - call_argc - 2];
  frame->ip = ip;
  ant_value_t result = sv_op_call_char_code_at(vm, js, target, receiver, call_args, call_argc);
  VM_RELOAD_FRAME();
  vm->sp -= call_argc + 2;
  if (is_err(result)) VM_THROW(result);
  vm->stack[vm->sp++] = result;
  NEXT(OP_CALL_CHAR_CODE_AT);
}

L_CALL_ARRAY_INCLUDES: {
  uint16_t call_argc = sv_get_u16(ip + 1);
  ant_value_t *call_args = &vm->stack[vm->sp - call_argc];
  ant_value_t call_func = vm->stack[vm->sp - call_argc - 1];
  ant_value_t call_this = vm->stack[vm->sp - call_argc - 2];
  ant_value_t call_result;

  frame->ip = ip;
  if (js_is_array_includes_builtin(call_func)) {
    call_result = js_array_includes_call(js, call_this, call_args, call_argc);
  } else call_result = sv_vm_call(vm, js, call_func, call_this, call_args, call_argc, NULL, Ant_Value_Undefined);
  VM_RELOAD_FRAME();

  vm->sp -= call_argc + 2;
  if (is_err(call_result)) VM_THROW(call_result);
  vm->stack[vm->sp++] = call_result;

  NEXT(OP_CALL_ARRAY_INCLUDES);
}

L_CALL_STRING_INTRINSIC: {
  ant_string_intrinsic_kind_t kind =
    (ant_string_intrinsic_kind_t)ip[1];
  uint16_t call_argc = sv_get_u16(ip + 2);
  ant_value_t *call_args = &vm->stack[vm->sp - call_argc];
  ant_value_t call_func = vm->stack[vm->sp - call_argc - 1];
  ant_value_t call_this = vm->stack[vm->sp - call_argc - 2];

  frame->ip = ip;
  ant_value_t call_result = sv_op_call_string_intrinsic(
    vm, js, kind, call_func, call_this, call_args, call_argc);
  VM_RELOAD_FRAME();

  vm->sp -= call_argc + 2;
  if (is_err(call_result)) VM_THROW(call_result);
  vm->stack[vm->sp++] = call_result;
  NEXT(OP_CALL_STRING_INTRINSIC);
}

L_CALL_MAP_TEMPLATE: {
  uint32_t desc_idx = sv_get_u32(ip + 1);
  const sv_map_template_desc_t *desc =
    sv_map_template_desc_at(func, desc_idx);

  if (!desc || vm->sp < desc->substitution_count + 2) VM_THROW(js_mkerr(js, "invalid map template descriptor"));

  int substitution_count = desc->substitution_count;
  int base = vm->sp - substitution_count - 2;
  ant_value_t call_this = vm->stack[base];
  ant_value_t call_func = vm->stack[base + 1];
  ant_value_t *substitutions = &vm->stack[base + 2];

  frame->ip = ip;
  ant_value_t call_result = sv_op_call_map_template(
    vm, js, call_func, call_this, substitutions, desc);
  VM_RELOAD_FRAME();

  vm->sp -= substitution_count + 2;
  if (is_err(call_result)) VM_THROW(call_result);
  vm->stack[vm->sp++] = call_result;
  NEXT(OP_CALL_MAP_TEMPLATE);
}

L_CALL_STABLE_BUILTIN: {
  sv_stable_builtin_t kind = (sv_stable_builtin_t)ip[1];
  uint16_t call_argc = sv_get_u16(ip + 2);
  ant_value_t *call_args = &vm->stack[vm->sp - call_argc];
  ant_value_t call_func = vm->stack[vm->sp - call_argc - 1];
  ant_value_t call_this = vm->stack[vm->sp - call_argc - 2];

  frame->ip = ip;
  ant_value_t call_result = sv_op_call_stable_builtin(
    vm, js, kind, call_func, call_this, call_args, call_argc);
  VM_RELOAD_FRAME();

  vm->sp -= call_argc + 2;
  if (is_err(call_result)) VM_THROW(call_result);
  vm->stack[vm->sp++] = call_result;
  NEXT(OP_CALL_STABLE_BUILTIN);
}

L_CALL_IS_PROTO: {
  ant_value_t call_arg  = vm->stack[vm->sp - 1];
  ant_value_t call_func = vm->stack[vm->sp - 2];
  ant_value_t call_this = vm->stack[vm->sp - 3];
  ant_value_t call_result;

  if (
    vtype(call_func) == kTypeBuiltin &&
    js_cfunc_same_entrypoint(call_func, builtin_object_isPrototypeOf)
  ) call_result = sv_isproto_ic_eval(js, call_this, call_arg, func, ip); else {
    ant_value_t call_args[1] = { call_arg };
    frame->ip = ip;
    call_result = sv_vm_call(vm, js, call_func, call_this, call_args, 1, NULL, Ant_Value_Undefined);
    VM_RELOAD_FRAME();
  }

  vm->sp -= 3;
  if (is_err(call_result)) VM_THROW(call_result);
  vm->stack[vm->sp++] = call_result;
  NEXT(OP_CALL_IS_PROTO);
}

L_RE_EXEC_TRUTHY: {
  ant_value_t call_arg  = vm->stack[vm->sp - 1];
  ant_value_t call_func = vm->stack[vm->sp - 2];
  ant_value_t call_this = vm->stack[vm->sp - 3];
  ant_value_t call_result;

  if (!regexp_exec_truthy_try_fast(js, call_func, call_this, call_arg, &call_result)) {
    ant_value_t call_args[1] = { call_arg }; frame->ip = ip;
    ant_value_t raw_result = sv_vm_call(vm, js, call_func, call_this, call_args, 1, NULL, Ant_Value_Undefined);
    VM_RELOAD_FRAME();
    if (is_err(raw_result)) call_result = raw_result;
    else call_result = mkval(kTypeBool, js_truthy(js, raw_result) ? 1 : 0);
  }

  vm->sp -= 3;
  if (is_err(call_result)) VM_THROW(call_result);
  vm->stack[vm->sp++] = call_result;
  NEXT(OP_RE_EXEC_TRUTHY);
}

L_RE_EXEC_DISCARD: {
  ant_value_t call_arg  = vm->stack[vm->sp - 1];
  ant_value_t call_func = vm->stack[vm->sp - 2];
  ant_value_t call_this = vm->stack[vm->sp - 3];

  frame->ip = ip;
  ant_value_t call_result = jit_helper_regexp_exec_truthy(
    vm, js, call_func, call_this, call_arg);
  VM_RELOAD_FRAME();

  vm->sp -= 3;
  if (is_err(call_result)) VM_THROW(call_result);
  NEXT(OP_RE_EXEC_DISCARD);
}

L_RE_LITERAL_EXEC: {
  ant_value_t call_arg = vm->stack[vm->sp - 1];
  ant_value_t flags = vm->stack[vm->sp - 2];
  ant_value_t pattern = vm->stack[vm->sp - 3];
  ant_value_t call_result = regexp_literal_exec_call(js, pattern, flags, call_arg);

  vm->sp -= 3;
  if (is_err(call_result)) VM_THROW(call_result);
  vm->stack[vm->sp++] = call_result;
  NEXT(OP_RE_LITERAL_EXEC);
}

L_STR_RE_LITERAL_REPLACE: {
  ant_value_t replacement = vm->stack[vm->sp - 1];
  ant_value_t flags = vm->stack[vm->sp - 2];
  ant_value_t pattern = vm->stack[vm->sp - 3];
  ant_value_t str = vm->stack[vm->sp - 4];
  ant_value_t call_result = regexp_literal_replace_call(js, str, pattern, flags, replacement);

  vm->sp -= 4;
  if (is_err(call_result)) VM_THROW(call_result);
  vm->stack[vm->sp++] = call_result;
  NEXT(OP_STR_RE_LITERAL_REPLACE);
}

L_TAIL_CALL: {
  tc_argc = sv_get_u16(ip + 1);
  ant_value_t call_func = vm->stack[vm->sp - tc_argc - 1];
  tc_this = Ant_Value_Undefined;

  if (vm->handler_depth == frame->handler_base &&
      vtype(frame->new_target) == kTypeUndefined &&
      vtype(call_func) == kTypeFunction) {
    sv_closure_t *closure = js_func_closure(call_func);
    if (closure->func != NULL) {
      if (!closure->func->is_async && !closure->func->is_generator &&
          !(closure->call_flags & (SV_CALL_HAS_BOUND_ARGS | SV_CALL_HAS_SUPER)) &&
          closure->func->realm == js->realm) {
        if (sv_closure_has_lexical_this(closure))
          tc_this = closure->bound_this;
        goto tail_call_inline;
      }
    }
  }
  if (vtype(call_func) == kTypeBuiltin) sv_tfb_record_builtin_call(func, ip);
  ant_value_t *call_args = &vm->stack[vm->sp - tc_argc];
  frame->ip = ip;
  ant_value_t call_result = sv_vm_call(
    vm, js, call_func, tc_this, call_args, tc_argc, NULL, Ant_Value_Undefined);
  VM_RELOAD_FRAME();
  vm->sp -= tc_argc + 1;
  if (is_err(call_result)) VM_THROW(call_result);
  vm->stack[vm->sp++] = call_result;
  goto L_RETURN;
}

L_TAIL_MAP_TEMPLATE: {
  uint32_t desc_idx = sv_get_u32(ip + 1);
  const sv_map_template_desc_t *desc =
    sv_map_template_desc_at(func, desc_idx);

  if (!desc || vm->sp < desc->substitution_count + 2) VM_THROW(js_mkerr(js, "invalid map template descriptor"));

  int substitution_count = desc->substitution_count;
  int base = vm->sp - substitution_count - 2;
  ant_value_t call_this = vm->stack[base];
  ant_value_t call_func = vm->stack[base + 1];
  ant_value_t *substitutions = &vm->stack[base + 2];

  frame->ip = ip;
  ant_value_t call_result = sv_map_template_try_fast(
    js, call_func, call_this, substitutions, desc);
  if (!sv_is_jit_bailout(call_result)) {
    vm->sp -= substitution_count + 2;
    if (is_err(call_result)) VM_THROW(call_result);
    vm->stack[vm->sp++] = call_result;
    goto L_RETURN;
  }

  ant_value_t key = sv_map_template_build_key(js, substitutions, desc);
  VM_RELOAD_FRAME();
  if (is_err(key)) VM_THROW(key);
  vm->stack[base + 2] = key;
  vm->sp -= substitution_count - 1;
  tc_argc = 1;
  goto tail_call_method_dispatch;
}

L_TAIL_CALL_METHOD:
  tc_argc = sv_get_u16(ip + 1);

tail_call_method_dispatch: {
  ant_value_t call_func = vm->stack[vm->sp - tc_argc - 1];
  tc_this = vm->stack[vm->sp - tc_argc - 2];

  if (vm->handler_depth == frame->handler_base &&
      vtype(frame->new_target) == kTypeUndefined &&
      vtype(call_func) == kTypeFunction) {
    sv_closure_t *closure = js_func_closure(call_func);
    if (closure->func != NULL) {
      if (!closure->func->is_async && !closure->func->is_generator &&
          !(closure->call_flags & (SV_CALL_HAS_BOUND_ARGS | SV_CALL_HAS_SUPER)) &&
          closure->func->realm == js->realm) {
        if (sv_closure_has_lexical_this(closure))
          tc_this = closure->bound_this;
        goto tail_call_inline;
      }
      if (!(closure->call_flags & SV_CALL_HAS_BOUND_ARGS) && sv_func_type_feedback(func))
        sv_tfb_record_call_target(func, (int)(ip - func->code), closure->func);
    }
  }
  if (vtype(call_func) == kTypeBuiltin) sv_tfb_record_builtin_call(func, ip);
  ant_value_t *call_args = &vm->stack[vm->sp - tc_argc];
  frame->ip = ip;
  ant_value_t call_result = sv_vm_call(
    vm, js, call_func, tc_this, call_args, tc_argc, NULL, Ant_Value_Undefined);
  VM_RELOAD_FRAME();
  vm->sp -= tc_argc + 2;
  if (is_err(call_result)) VM_THROW(call_result);
  vm->stack[vm->sp++] = call_result;
  goto L_RETURN;
}

tail_call_inline: {
  if (js_interrupt_pending(js)) goto sv_interrupt;
  ant_value_t call_func = vm->stack[vm->sp - tc_argc - 1];
  sv_closure_t *closure = js_func_closure(call_func);

  if (sv_func_type_feedback(func)) sv_tfb_record_call_target(func, (int)(ip - func->code), closure->func);
  if (++closure->func->call_count == SV_TFB_ALLOC_THRESHOLD) sv_tfb_ensure(closure->func);

  int arg_slots = (
    (int)tc_argc > closure->func->param_count)
    ? (int)tc_argc : closure->func->param_count;

  int need = arg_slots + closure->func->max_locals;
  int reserve = need + closure->func->max_stack;

  while (frame->prev_sp + reserve > vm->stack_size) if (!sv_vm_grow_stack(vm)) {
    VM_THROW(Ant_Error_CreateStackOverflow(js));
  }

  ant_value_t *call_args = &vm->stack[vm->sp - tc_argc];
  if (frame->bp && vm->open_upvalues) sv_close_upvalues_from_slot(vm, frame->bp);
  sv_drop_frame_runtime_state(js, frame);
  vm->sp = frame->prev_sp;

  ant_value_t *base = &vm->stack[vm->sp];
  memmove(base, call_args, (size_t)tc_argc * sizeof(ant_value_t));
  for (int i = (int)tc_argc; i < arg_slots; i++) base[i] = Ant_Value_Undefined;

  ant_value_t *new_lp = base + arg_slots;
  for (int i = 0; i < closure->func->max_locals; i++) new_lp[i] = Ant_Value_Undefined;

  vm->sp = frame->prev_sp + need;
  func = closure->func;
  frame->func = func;
  frame->callee = call_func;
  frame->this = tc_this;
  frame->new_target = Ant_Value_Undefined;
  frame->super_val = Ant_Value_Undefined;
  frame->argc = tc_argc;
  frame->handler_base = (uint16_t)vm->handler_depth;
  frame->handler_top = (uint16_t)vm->handler_depth;
  frame->completion.kind = SV_COMPLETION_NONE;
  frame->completion.value = Ant_Value_Undefined;
  frame->arguments_obj = Ant_Value_Undefined;
  frame->eval_env = sv_closure_eval_env(closure);
  frame->bp = base;
  frame->lp = new_lp;
  frame->upvalues = closure->upvalues;
  frame->upvalue_count = closure->func->upvalue_count;
  frame->this = sv_normalize_this_for_frame(js, func, frame->this);

  lp = frame->lp;
  ip = func->code;

  DISPATCH();
}

L_NEW: {
  uint16_t new_argc = sv_get_u16(ip + 1);
  ant_value_t new_func = vm->stack[vm->sp - new_argc - 2];
  sv_closure_t *new_closure = vtype(new_func) == kTypeFunction ? js_func_closure(new_func) : NULL;
  sv_func_t *new_callee = new_closure ? new_closure->func : NULL;
  if (new_closure && sv_func_type_feedback(func))
    sv_tfb_record_call_target(func, (int)(ip - func->code), new_callee);

  if (
    new_callee && !new_callee->jit_code && !(new_closure->call_flags & (
      SV_CALL_HAS_BOUND_ARGS | SV_CALL_HAS_SUPER | SV_CALL_IS_ARROW |
      SV_CALL_IS_DEFAULT_CTOR | SV_CALL_HAS_BOUND_THIS | SV_CALL_IS_UNCURRY
    )) &&
    !new_callee->is_async && !new_callee->is_generator && !new_callee->is_derived_ctor &&
    new_callee->realm == js->realm &&
    (new_callee->jit_compile_failed || new_callee->call_count < SV_JIT_THRESHOLD) &&
    js_is_constructor(new_func)
  ) {
    ant_value_t new_target = vm->stack[vm->sp - new_argc - 1];
    ant_value_t effective_new_target = new_target;
    ant_value_t proto = sv_prepare_construct_meta(js, new_func, new_target, &effective_new_target);

    if (is_err(proto)) {
      vm->sp -= new_argc + 2;
      VM_THROW(proto);
    }

    if (vm->fp + 1 >= vm->max_frames && !sv_vm_grow_frames(vm)) {
      vm->sp -= new_argc + 2;
      VM_THROW(Ant_Error_CreateStackOverflow(js));
    }

    GC_ROOT_SAVE(new_mark, js);
    GC_ROOT_PIN(js, proto);

    ant_value_t new_obj = js_mkobj_with_proto_slots(
      js, is_object_type(proto) ? proto :
      Ant_Value_Undefined, sv_construct_slots_of(new_func)
    );

    GC_ROOT_RESTORE(js, new_mark);
    uint32_t new_count = ++new_callee->call_count;

    if (__builtin_expect(new_count == SV_TFB_ALLOC_THRESHOLD, 0)) sv_tfb_ensure(new_callee);
    ant_value_t *new_args = &vm->stack[vm->sp - new_argc];

    frame = &vm->frames[vm->fp];
    frame->ip = ip + sv_op_size[OP_NEW];
    vm->sp -= new_argc + 2;
    vm->fp++;
    frame = &vm->frames[vm->fp];
    func = new_callee;
    frame->func = func;
    frame->callee = new_func;
    frame->this = new_obj;
    frame->new_target = effective_new_target;
    frame->super_val = Ant_Value_Undefined;
    frame->prev_sp = vm->sp;
    frame->handler_base = (uint16_t)vm->handler_depth;
    frame->handler_top = (uint16_t)vm->handler_depth;
    frame->argc = new_argc;
    frame->completion.kind = SV_COMPLETION_NONE;
    frame->completion.value = Ant_Value_Undefined;
    frame->arguments_obj = Ant_Value_Undefined;
    frame->eval_env = sv_closure_eval_env(new_closure);
    frame->upvalues = new_closure->upvalues;
    frame->upvalue_count = func->upvalue_count;
    ant_value_t *new_bp = NULL;
    ant_value_t *new_lp = NULL;
    ant_value_t new_stage_err = sv_stage_frame_args(vm, js, func, new_args, (int)new_argc, &new_bp, &new_lp);
    if (is_err(new_stage_err)) {
      vm->fp--;
      vm->sp += new_argc + 2;
      VM_THROW(new_stage_err);
    }
    frame->bp = new_bp;
    frame->lp = new_lp;
    lp = new_lp;
    ip = func->code;
    DISPATCH();
  }

  VM_CHECK(sv_op_new(vm, js, ip));
  NEXT(OP_NEW);
}

L_APPLY: {
  VM_CHECK(sv_op_apply(vm, js, ip));
  NEXT(OP_APPLY);
}

L_SUPER_APPLY: {
  VM_CHECK(sv_op_super_apply(vm, js, frame, ip));
  NEXT(OP_SUPER_APPLY);
}

L_NEW_APPLY: {
  VM_CHECK(sv_op_new_apply(vm, js, ip));
  NEXT(OP_NEW_APPLY);
}

L_CALL_SPREAD: {
  VM_CHECK(sv_op_call_spread(vm, js, ip));
  NEXT(OP_CALL_SPREAD);
}

L_SUPER_FORWARD: {
  VM_CHECK(sv_op_super_forward(vm, js, frame));
  NEXT(OP_SUPER_FORWARD);
}

L_EVAL: {
  VM_CHECK(sv_op_eval(vm, js, frame, ip));
  NEXT(OP_EVAL);
}

L_GET_EVAL_GLOBAL: {
  VM_CHECK(sv_op_get_eval_global(vm, js, frame, func, ip));
  NEXT(OP_GET_EVAL_GLOBAL);
}

L_GET_EVAL_GLOBAL_UNDEF: {
  VM_CHECK(sv_op_get_eval_global_undef(vm, js, frame, func, ip));
  NEXT(OP_GET_EVAL_GLOBAL_UNDEF);
}

L_PUT_EVAL_GLOBAL: {
  VM_CHECK(sv_op_put_eval_global(vm, js, frame, func, ip));
  NEXT(OP_PUT_EVAL_GLOBAL);
}

L_DELETE_EVAL_VAR: {
  sv_op_delete_eval_var(vm, js, frame, func, ip);
  NEXT(OP_DELETE_EVAL_VAR);
}

L_RETURN: RETURN_VALUE(vm->stack[vm->sp - 1]);
L_RETURN_UNDEF: RETURN_VALUE(Ant_Value_Undefined);

L_CHECK_CTOR: {
  VM_CHECK(sv_op_check_ctor(vm, js));
  NEXT(OP_CHECK_CTOR);
}

L_CHECK_CTOR_RET: {
  sv_op_check_ctor_ret(vm, frame);
  NEXT(OP_CHECK_CTOR_RET);
}

L_HALT: VM_LEAVE(sv_op_halt(vm));
L_THROW: VM_THROW(sv_op_throw(vm));
L_THROW_ERROR: VM_THROW(sv_op_throw_error(vm, js, func, ip));

L_TRY_PUSH: {
  VM_CHECK(sv_op_try_push(vm, js, ip, SV_HANDLER_TRY));
  NEXT(OP_TRY_PUSH);
}

L_TRY_PUSH_FINALLY: {
  VM_CHECK(sv_op_try_push(vm, js, ip, SV_HANDLER_TRY_FINALLY));
  NEXT(OP_TRY_PUSH_FINALLY);
}

L_TRY_POP: {
  sv_op_try_pop(vm);
  NEXT(OP_TRY_POP);
}

L_CATCH: {
  sv_op_catch(vm);
  sv_tfb_record1(func, ip, vm->stack[vm->sp - 1]);
  if (__builtin_expect(func->jit_code != NULL, 0)) {
    ant_value_t osr_r = sv_jit_try_catch_entry(vm, js, frame, func, (int)(ip - func->code));
    JIT_OSR_FINISH(osr_r);
  }
  NEXT(OP_CATCH);
}

L_FINALLY: {
  VM_CHECK(sv_op_finally(vm, js, ip));
  NEXT(OP_FINALLY);
}

L_FINALLY_DISCARD: {
  sv_op_finally_discard(vm);
  NEXT(OP_FINALLY_DISCARD);
}

L_NIP_CATCH: {
  sv_op_nip_catch(vm);
  NEXT(OP_NIP_CATCH);
}

L_UNWIND_JMP: {
  int32_t off = sv_get_i32(ip + 1);
  uint8_t *target = ip + sv_op_size[OP_UNWIND_JMP] + off;
  int n_fin = (int)sv_get_u8(ip + 5);
  int n_pop = (int)sv_get_u8(ip + 6);
  uint8_t *finally_ip = sv_vm_unwind_for_jump(vm, target, n_fin, n_pop);
  ip = finally_ip ? finally_ip : target;
  DISPATCH();
}

L_FINALLY_RET: {
  uint8_t *resume_ip = NULL;
  ant_value_t completion = Ant_Value_Undefined;
  sv_finally_ret_t action = sv_op_finally_ret(vm, js, &resume_ip, &completion);
  if (action == SV_FINALLY_RET_ERROR || action == SV_FINALLY_RET_THROW) VM_THROW(completion);
  if (action == SV_FINALLY_RET_RETURN) {
    sv_frame_reset_handlers(vm, frame);
    if (vm->open_upvalues && frame->bp) sv_close_upvalues_from_slot(vm, frame->bp);
    if (__builtin_expect(sv_async_inline_at(vm, vm->fp), 0))
      completion = sv_async_inline_settle(vm, js, completion, false, false);
    COMPLETE_FRAME(completion);
  }
  ip = resume_ip;
  DISPATCH();
}

L_USING_PUSH: {
  VM_CHECK(sv_op_using_push(vm, js, false));
  NEXT(OP_USING_PUSH);
}

L_USING_PUSH_ASYNC: {
  VM_CHECK(sv_op_using_push(vm, js, true));
  NEXT(OP_USING_PUSH_ASYNC);
}

L_DISPOSE_RESOURCE: {
  VM_CHECK(sv_op_dispose_resource(vm, js, false));
  NEXT(OP_DISPOSE_RESOURCE);
}

L_DISPOSE_RESOURCE_ASYNC: {
  VM_CHECK(sv_op_dispose_resource(vm, js, true));
  NEXT(OP_DISPOSE_RESOURCE_ASYNC);
}

L_USING_DISPOSE: {
  VM_CHECK(sv_op_using_dispose(vm, js, false, false));
  NEXT(OP_USING_DISPOSE);
}

L_USING_DISPOSE_ASYNC: {
  VM_CHECK(sv_op_using_dispose(vm, js, true, false));
  NEXT(OP_USING_DISPOSE_ASYNC);
}

L_USING_DISPOSE_SUPPRESSED: {
  VM_CHECK(sv_op_using_dispose(vm, js, false, true));
  NEXT(OP_USING_DISPOSE_SUPPRESSED);
}

L_USING_DISPOSE_ASYNC_SUPPRESSED: {
  VM_CHECK(sv_op_using_dispose(vm, js, true, true));
  NEXT(OP_USING_DISPOSE_ASYNC_SUPPRESSED);
}

L_FOR_IN: {
  VM_CHECK(sv_op_for_in(vm, js));
  NEXT(OP_FOR_IN);
}

L_FOR_OF: {
  VM_CHECK(sv_op_for_of(vm, js));
  sv_tfb_record_iteration(func, ip, vm->stack[vm->sp - 1]);
  NEXT(OP_FOR_OF);
}

L_FOR_AWAIT_OF: {
  VM_CHECK(sv_op_for_await_of(vm, js));
  NEXT(OP_FOR_AWAIT_OF);
}

L_ITER_NEXT: {
  VM_CHECK(sv_op_iter_next(vm, js));
  NEXT(OP_ITER_NEXT);
}

L_ITER_GET_VALUE: {
  sv_op_iter_get_value(vm, js);
  NEXT(OP_ITER_GET_VALUE);
}

L_ITER_CLOSE: {
  VM_CHECK(sv_op_iter_close(vm, js, sv_get_u8(ip + 1)));
  NEXT(OP_ITER_CLOSE);
}

L_ITER_CLOSE_ASYNC: {
  VM_CHECK(sv_op_iter_close_async(vm, js));
  NEXT(OP_ITER_CLOSE_ASYNC);
}

L_ITER_CLOSE_CHECK: {
  VM_CHECK(sv_op_iter_close_check(vm, js));
  NEXT(OP_ITER_CLOSE_CHECK);
}

L_ITER_CALL: {
  VM_CHECK(sv_op_iter_call(vm, js, ip));
  NEXT(OP_ITER_CALL);
}

L_AWAIT_ITER_NEXT:  {
  frame->ip = ip;
  bool inline_await = sv_async_inline_at(vm, vm->fp);
  if (inline_await) {
    ant_value_t ensured = sv_async_inline_ensure_coro(vm, js);
    if (is_err(ensured)) VM_THROW(ensured);
  }
  sv_await_result_t await_result = sv_op_await_iter_next(vm, js);
  if (await_result.state == SV_AWAIT_ERROR) VM_THROW(await_result.value);
  if (await_result.state == SV_AWAIT_SUSPENDED) {
    if (inline_await) {
      ant_value_t promise = sv_async_inline_settle(vm, js, Ant_Value_Undefined, false, true);
      if (vm->fp < entry_fp) VM_LEAVE(promise);
      VM_RELOAD_FRAME();
      ip = frame->ip;
      vm->stack[vm->sp++] = promise;
      DISPATCH();
    }
    VM_LEAVE(Ant_Value_Undefined);
  }
  NEXT(OP_AWAIT_ITER_NEXT);
}

L_DESTRUCTURE_INIT: {
  VM_CHECK(sv_op_destructure_init(vm, js));
  sv_tfb_record_iteration(func, ip, vm->stack[vm->sp - 1]);
  NEXT(OP_DESTRUCTURE_INIT);
}

L_DESTRUCTURE_NEXT: {
  VM_CHECK(sv_op_destructure_next(vm, js));
  NEXT(OP_DESTRUCTURE_NEXT);
}

L_DESTRUCTURE_REST: {
  VM_CHECK(sv_op_destructure_rest(vm, js));
  NEXT(OP_DESTRUCTURE_REST);
}

L_DESTRUCTURE_CLOSE: {
  VM_CHECK(sv_op_destructure_close(vm, js, sv_get_u8(ip + 1)));
  NEXT(OP_DESTRUCTURE_CLOSE);
}

L_AWAIT: {
  ant_value_t await_val = vm->stack[--vm->sp];
  frame->ip = ip + sv_op_size[OP_AWAIT];

  bool inline_await = sv_async_inline_at(vm, vm->fp);
  if (inline_await) {
    ant_value_t ensured = sv_async_inline_ensure_coro(vm, js);
    if (is_err(ensured)) VM_THROW(ensured);
  }
  int await_fp = inline_await ? vm->fp : entry_fp;
  vm->suspend.entry_fp = await_fp;
  vm->suspend.saved_fp = await_fp - 1;

  bool await_deferred = vm->async_inline.deferred;
  vm->async_inline.deferred = false;
  sv_await_result_t await_result = sv_await_value(vm, js, await_val, await_deferred);
  vm->suspend.entry_fp = -1;
  vm->suspend.saved_fp = -1;

  if (await_result.state == SV_AWAIT_SUSPENDED) {
    if (inline_await) {
      ant_value_t promise = sv_async_inline_settle(vm, js, Ant_Value_Undefined, false, true);
      if (vm->fp < entry_fp) VM_LEAVE(promise);
      VM_RELOAD_FRAME();
      ip = frame->ip;
      vm->stack[vm->sp++] = promise;
      DISPATCH();
    }
    VM_LEAVE(Ant_Value_Undefined);
  }

  if (await_result.state == SV_AWAIT_ERROR) VM_THROW(await_result.value);

  vm->stack[vm->sp++] = await_result.value;
  NEXT(OP_AWAIT);
}

L_AWAIT_RETURN: {
  ant_value_t await_val = vm->stack[--vm->sp];
  if (sv_async_inline_at(vm, vm->fp) && !vm->async_inline.records[vm->async_inline.len - 1].coro) {
    bool caller_awaits = sv_async_inline_next_op(vm, &vm->async_inline.records[vm->async_inline.len - 1]) == OP_AWAIT;
    ant_value_t tail_promise = caller_awaits
      ? sv_await_type_is_primitive(vtype(await_val)) ? await_val : js_promise_assimilate_awaitable(js, await_val)
      : js_promise_tail_await(js, await_val);
    if (is_err(tail_promise)) VM_THROW(tail_promise);
    vm->async_inline.deferred = caller_awaits;
    vm->async_inline.len--;
    COMPLETE_FRAME(tail_promise);
  }

  sv_await_result_t await_result = sv_await_tail(js, await_val);
  if (await_result.state == SV_AWAIT_ERROR) VM_THROW(await_result.value);

  bool tail_pending = await_result.state == SV_AWAIT_SUSPENDED;
  ant_value_t r = tail_pending ? Ant_Value_Undefined : await_result.value;

  bool tail_inline = sv_async_inline_at(vm, vm->fp);
  if (tail_inline) r = sv_async_inline_settle(vm, js, r, false, tail_pending);

  if (!tail_inline) VM_LEAVE(r);

  COMPLETE_FRAME(r);
}

L_ASYNC_YIELD: {
  ant_value_t yielded = vm->stack[vm->sp - 1];
  coroutine_t *coro = js->active_async_coro;
  if (!coro || !coro->is_generator || !sv_await_type_is_primitive(vtype(yielded))) goto L_AWAIT;
  vm->sp--;
  coro->yield_tick = true;
  SUSPEND_YIELD(ip + sv_op_size[OP_ASYNC_YIELD] + sv_op_size[OP_YIELD], yielded);
}

L_YIELD: {
  ant_value_t yielded = vm->stack[--vm->sp];
  coroutine_t *coro = js->active_async_coro;
  if (!coro || !coro->is_generator) VM_THROW(js_mkerr(js, "yield can only be used inside generator functions"));
  SUSPEND_YIELD(ip + sv_op_size[OP_YIELD], yielded);
}

L_YIELD_STAR_INIT: {
  VM_CHECK(sv_yield_star_init(vm, js, lp, sv_get_u16(ip + 1)));
  NEXT(OP_YIELD_STAR_INIT);
}

L_YIELD_STAR_INIT_ASYNC: {
  frame->ip = ip;
  ant_value_t is_async = sv_yield_star_init_async(vm, js, lp, sv_get_u16(ip + 1));
  if (is_err(is_async)) VM_THROW(is_async);
  if (is_async == Ant_Value_True) NEXT(OP_YIELD_STAR_INIT_ASYNC);
  ip += SV_YS_ASYNC_SYNC_SKIP;
  DISPATCH();
}

L_YIELD_STAR_ASYNC_CALL: {
  sv_resume_kind_t resume_kind = suspended_resume_kind;
  suspended_resume_kind = SV_RESUME_NEXT;
  VM_CHECK(sv_yield_star_async_call(vm, js, lp, sv_get_u16(ip + 1), resume_kind));
  NEXT(OP_YIELD_STAR_ASYNC_CALL);
}

L_YIELD_STAR_ASYNC_RESULT: {
  frame->ip = ip;
  sv_ys_step_t step = sv_yield_star_async_result(vm, js, lp, sv_get_u16(ip + 1));
  if (is_err(step.status)) VM_THROW(step.status);
  if (step.kind == SV_YS_STEP_SUSPEND) SUSPEND_YIELD(ip - SV_YS_ASYNC_RESUME_BACK, vm->stack[--vm->sp]);
  if (step.kind == SV_YS_STEP_RETURN) goto L_RETURN;
  ip += SV_YS_ASYNC_DONE_SKIP;
  DISPATCH();
}

L_YIELD_STAR_NEXT:
L_YIELD_STAR_THROW:
L_YIELD_STAR_RETURN: {
  coroutine_t *coro = js->active_async_coro;
  if (!coro || !coro->is_generator) VM_THROW(js_mkerr(js, "yield can only be used inside generator functions"));

  uint16_t base = sv_get_u16(ip + 1);
  ant_value_t resume_value = vm->stack[--vm->sp];
  ant_value_t yielded = Ant_Value_Undefined;

  bool done = false;
  bool ys_return = false;
  bool ys_raw = !func->is_async;
  ant_value_t ys_status;

  sv_resume_kind_t resume_kind = suspended_resume_kind;
  suspended_resume_kind = SV_RESUME_NEXT;

  frame->ip = ip;
  GC_ROOT_SAVE(ys_mark, js);
  GC_ROOT_PIN(js, resume_value);

  if (*ip == OP_YIELD_STAR_THROW || resume_kind == SV_RESUME_THROW)
    ys_status = sv_yield_star_throw(vm, js, lp, base, resume_value, &yielded, &done, &ys_raw);
  else if (*ip == OP_YIELD_STAR_RETURN || resume_kind == SV_RESUME_RETURN) {
    ys_return = true;
    ys_status = sv_yield_star_return(vm, js, lp, base, resume_value, &yielded, &done, &ys_raw);
  }
  else ys_status = sv_yield_star_next(vm, js, lp, base, resume_value, &yielded, &done, &ys_raw);

  GC_ROOT_RESTORE(js, ys_mark);
  VM_CHECK(ys_status);

  if (ys_return && done) {
    sv_yield_star_clear_state(js, lp, base);
    vm->stack[vm->sp++] = yielded;
    goto L_RETURN;
  }

  if (done) {
    sv_yield_star_clear_state(js, lp, base);
    vm->stack[vm->sp++] = yielded;
    NEXT(*ip);
  }

  coro->yield_raw = ys_raw;
  SUSPEND_YIELD(ip, yielded);
}

L_SPREAD: {
  VM_CHECK(sv_op_spread(vm, js));
  NEXT(OP_SPREAD);
}

L_DEFINE_METHOD: {
  sv_op_define_method(vm, js, func, ip);
  NEXT(OP_DEFINE_METHOD);
}

L_DEFINE_METHOD_COMP: {
  sv_op_define_method_comp(vm, js, ip);
  NEXT(OP_DEFINE_METHOD_COMP);
}

L_SET_NAME: {
  sv_op_set_name(vm, js, func, ip);
  NEXT(OP_SET_NAME);
}

L_SET_NAME_COMP: {
  sv_op_set_name_comp(vm, js);
  NEXT(OP_SET_NAME_COMP);
}

L_SET_PROTO: {
  sv_op_set_proto(vm, js);
  NEXT(OP_SET_PROTO);
}

L_SET_HOME_OBJ: {
  sv_op_set_home_obj(vm, js);
  NEXT(OP_SET_HOME_OBJ);
}

L_APPEND: {
  sv_op_append(vm, js);
  NEXT(OP_APPEND);
}

L_COPY_DATA_PROPS: {
  VM_CHECK(sv_op_copy_data_props(vm, js, ip));
  NEXT(OP_COPY_DATA_PROPS);
}

L_DEFINE_CLASS: {
  VM_CHECK(sv_op_define_class(vm, js, func, ip));
  NEXT(OP_DEFINE_CLASS);
}

L_DEFINE_CLASS_COMP: {
  VM_CHECK(sv_op_define_class_comp(vm, js, func, ip));
  NEXT(OP_DEFINE_CLASS_COMP);
}

L_TO_OBJECT: {
  VM_CHECK(sv_op_to_object(vm, js));
  NEXT(OP_TO_OBJECT);
}

L_TO_PROPKEY: {
  sv_op_to_propkey(vm, js);
  NEXT(OP_TO_PROPKEY);
}

L_TO_STRING: {
  VM_CHECK(sv_op_to_string(vm, js));
  NEXT(OP_TO_STRING);
}

L_TO_STRING_DEFER_NUMBER: {
  if (vtype(vm->stack[vm->sp - 1]) != kTypeNumber)
    VM_CHECK(sv_op_to_string(vm, js));
  NEXT(OP_TO_STRING_DEFER_NUMBER);
}

L_IS_UNDEF: {
  sv_op_is_undef(vm);
  NEXT(OP_IS_UNDEF);
}

L_IS_NULL: {
  sv_op_is_null(vm);
  NEXT(OP_IS_NULL);
}

L_IMPORT: {
  VM_CHECK(sv_op_import(vm, js, func));
  NEXT(OP_IMPORT);
}

L_IMPORT_SYNC: {
  VM_CHECK(sv_op_import_sync(vm, js));
  NEXT(OP_IMPORT_SYNC);
}

L_IMPORT_DEFAULT: {
  sv_op_import_default(vm, js);
  NEXT(OP_IMPORT_DEFAULT);
}

L_IMPORT_NAMED: {
  VM_CHECK(sv_op_import_named(vm, js, func, ip));
  NEXT(OP_IMPORT_NAMED);
}

L_EXPORT: {
  VM_CHECK(sv_op_export(vm, js, frame, func, ip));
  NEXT(OP_EXPORT);
}

L_EXPORT_ALL: {
  VM_CHECK(sv_op_export_all(vm, js));
  NEXT(OP_EXPORT_ALL);
}

#define WITH_TRY(op, fn) {                                              \
  bool taken = false;                                                   \
  VM_CHECK(fn(vm, js, func, ip, &taken));                               \
  if (taken) { ip += sv_op_size[op] + sv_get_i32(ip + 5); DISPATCH(); } \
  NEXT(op);                                                             \
}

L_WITH_TRY_GET:  WITH_TRY(OP_WITH_TRY_GET, sv_op_with_try_get)
L_WITH_TRY_CALL: WITH_TRY(OP_WITH_TRY_CALL, sv_op_with_try_call)
L_WITH_TRY_PUT:  WITH_TRY(OP_WITH_TRY_PUT, sv_op_with_try_put)
L_WITH_TRY_DEL:  WITH_TRY(OP_WITH_TRY_DEL, sv_op_with_try_del)
#undef WITH_TRY

L_SPECIAL_OBJ: {
  sv_op_special_obj(vm, js, frame, ip);
  NEXT(OP_SPECIAL_OBJ);
}

L_EMPTY: {
  vm->stack[vm->sp++] = T_EMPTY;
  NEXT(OP_EMPTY);
}

L_PUT_CONST: {
  uint32_t idx = sv_get_u32(ip + 1);
  ant_value_t cached = vm->stack[--vm->sp];
  VM_CHECK(sv_code_unit_retain_template(js, func, cached) ? Ant_Value_Undefined : js_mkerr(js, "oom"));
  func->constants[idx] = cached;
  NEXT(OP_PUT_CONST);
}

L_DEBUGGER: NEXT(OP_DEBUGGER);
L_NOP: NEXT(OP_NOP);

L_LABEL:
L_LINE_NUM:
L_COL_NUM:
L_INVALID: VM_THROW(js_mkerr(js, "invalid opcode %d", (int)*ip));
