#include "compiler_internal.h"

void emit_lexical_new_target(sv_compiler_t *c) {
  if (c->new_target_local >= 0) {
    emit_get_local(c, c->new_target_local);
    return;
  }

  int upval = resolve_new_target_upvalue(c);
  if (upval >= 0) {
    emit_op(c, OP_GET_UPVAL);
    emit_u16(c, (uint16_t)upval);
    return;
  }

  emit_op(c, OP_UNDEF);
}

static void emit_slot_atom_op(sv_compiler_t *c, sv_op_t op, int local, const char *name, uint32_t len) {
  int ai = add_atom(c, name, len);
  emit_op(c, op);
  emit_u16(c, (uint16_t)(local - c->param_locals));
  emit_u32(c, (uint32_t)ai);
}

static void emit_get_var_with(
  sv_compiler_t *c, const char *name, uint32_t len,
  sv_op_t global_op, sv_op_t eval_op
) {
  bool is_super = is_ident_str(name, len, "super", 5);

  if (is_super && c->super_local >= 0) {
    c->super_referenced = true;
    emit_get_local(c, c->super_local);
    return;
  }
  
  if (is_super && c->is_arrow) {
  int super_upval = resolve_super_upvalue(c);
  if (super_upval != -1) {
    emit_op(c, OP_GET_UPVAL);
    emit_u16(c, (uint16_t)super_upval);
    return;
  }}

  int local = resolve_local(c, name, len);
  if (local != -1) {
    if (c->locals[local].depth == -1) {
      emit_op(c, OP_GET_ARG);
      emit_u16(c, (uint16_t)local);
    } else {
      int slot = local - c->param_locals;
      if (c->locals[local].is_tdz) {
        emit_slot_atom_op(c, OP_GET_LOCAL_CHK, local, name, len);
      } else if (slot <= 255) {
        emit_op(c, OP_GET_LOCAL8);
        emit(c, (uint8_t)slot);
      } else {
        emit_op(c, OP_GET_LOCAL);
        emit_u16(c, (uint16_t)slot);
      }
    }
    emit_import_binding_resolve(
      c,
      c->locals[local].binding.import_kind,
      c->locals[local].binding.import_name,
      c->locals[local].binding.import_name_len
    );
    return;
  }
  
  int upval = resolve_upvalue(c, name, len);
  if (upval != -1) {
    emit_op(c, c->upval_descs[upval].by_value ? OP_GET_UPVAL_VAL : OP_GET_UPVAL);
    emit_u16(c, (uint16_t)upval);
    sv_binding_meta_t *meta = &c->upval_bindings[upval];
    emit_import_binding_resolve(
      c,
      meta->import_kind,
      meta->import_name,
      meta->import_name_len
    );
    return;
  }
  
  if (is_ident_str(name, len, "arguments", 9)) {
    if (c->owns_eval_env || sv_compile_mode_is_eval(c->mode)) {
      emit_atom_op(c, eval_op, name, len);
      return;
    }
    if (has_implicit_arguments_obj(c)) {
      if (c->strict_args_local >= 0) {
        emit_get_local(c, c->strict_args_local);
      } else {
        emit_op(c, OP_SPECIAL_OBJ);
        emit(c, SV_SPECIAL_ARGUMENTS);
      }
      return;
    }
    if (c->is_arrow) {
      int args_upval = resolve_arguments_upvalue(c);
      if (args_upval != -1) {
        emit_op(c, OP_GET_UPVAL);
        emit_u16(c, (uint16_t)args_upval);
        return;
      }
    }
  }
  
  if (has_module_import_binding(c) && is_ident_str(name, len, "import", 6)) {
    emit_get_module_import_binding(c);
    return;
  }
  
  emit_atom_op(c, env_resolution_op(c, global_op, eval_op), name, len);
}

void emit_get_var_plain(sv_compiler_t *c, const char *name, uint32_t len) {
  emit_get_var_with(c, name, len, OP_GET_GLOBAL, OP_GET_EVAL_GLOBAL);
}

void emit_get_var_typeof(sv_compiler_t *c, const char *name, uint32_t len) {
  emit_get_var_with(c, name, len, OP_GET_GLOBAL_UNDEF, OP_GET_EVAL_GLOBAL_UNDEF);
}

void emit_get_var(sv_compiler_t *c, const char *name, uint32_t len) {
  sv_with_refs_t shadows = {0};
  sv_patch_list_t found = {0};
  
  with_shadows(c, name, len, &shadows);
  emit_with_tries(c, &shadows, OP_WITH_TRY_GET, name, len, 1, &found);
  emit_get_var_plain(c, name, len);
  patch_list_resolve(c, &found);
}

static void emit_export_dups(sv_compiler_t *c, const sv_export_name_t *exports) {
  for (const sv_export_name_t *e = exports; e; e = e->next)
    emit_op(c, OP_DUP);
}

static void emit_export_writes(sv_compiler_t *c, const sv_export_name_t *exports) {
  for (const sv_export_name_t *e = exports; e; e = e->next)
    emit_atom_op(c, OP_EXPORT, e->name, e->len);
}

void emit_export_writes_from_stack(sv_compiler_t *c, const sv_export_name_t *exports) {
  if (!exports) return;
  for (const sv_export_name_t *e = exports->next; e; e = e->next)
    emit_op(c, OP_DUP);
  emit_export_writes(c, exports);
}

void emit_local_export_writes(sv_compiler_t *c, int local) {
  if (local < 0) return;
  const sv_export_name_t *exports = c->locals[local].binding.exports;
  if (!exports) return;
  emit_get_local(c, local);
  emit_export_writes_from_stack(c, exports);
}

static void emit_set_var_plain(sv_compiler_t *c, const char *name, uint32_t len, bool keep) {
  int local = resolve_local(c, name, len);
  if (local != -1) {
    bool tdz = c->locals[local].is_tdz && c->locals[local].depth != -1;
    if (c->locals[local].is_const) {
      if (tdz) {
        emit_slot_atom_op(c, OP_GET_LOCAL_CHK, local, name, len);
        emit_op(c, OP_POP);
      }
      emit_const_assign_error(c, name, len);
      return;
    }
    set_local_inferred_type(c, local, SV_TI_UNKNOWN);
    const sv_export_name_t *exports = c->locals[local].binding.exports;
    emit_export_dups(c, exports);

    if (c->locals[local].depth == -1) {
      emit_op(c, keep ? OP_SET_ARG : OP_PUT_ARG);
      emit_u16(c, (uint16_t)local);
    } else if (tdz) {
      if (keep) emit_op(c, OP_DUP);
      emit_slot_atom_op(c, OP_PUT_LOCAL_CHK, local, name, len);
    } else {
      int slot = local - c->param_locals;
      sv_op_t op = keep
        ? (slot <= 255 ? OP_SET_LOCAL8 : OP_SET_LOCAL)
        : (slot <= 255 ? OP_PUT_LOCAL8 : OP_PUT_LOCAL);
      emit_op(c, op);
      if (slot <= 255) emit(c, (uint8_t)slot);
      else emit_u16(c, (uint16_t)slot);
    }
    emit_export_writes(c, exports);
    return;
  }
  int upval = resolve_upvalue(c, name, len);
  if (upval != -1) {
    Ant_Assert(!c->upval_descs[upval].by_value, "assignment to a parameter captured by value");
    if (c->upval_descs[upval].is_const) {
      if (c->upval_descs[upval].may_tdz) {
        emit_op(c, OP_GET_UPVAL);
        emit_u16(c, (uint16_t)upval);
        emit_op(c, OP_POP);
      }
      emit_const_assign_error(c, name, len);
      return;
    }
    const sv_export_name_t *exports = c->upval_bindings[upval].exports;
    emit_export_dups(c, exports);
    emit_op(c, keep ? OP_SET_UPVAL : OP_PUT_UPVAL);
    emit_u16(c, (uint16_t)upval);
    emit_export_writes(c, exports);
    return;
  }
  if (has_module_import_binding(c) && is_ident_str(name, len, "import", 6)) {
    emit_const_assign_error(c, name, len);
    return;
  }
  if (keep) emit_op(c, OP_DUP);
  emit_atom_op(
    c, env_resolution_op(c, OP_PUT_GLOBAL, OP_PUT_EVAL_GLOBAL),
    name, len);
}

void emit_set_var(sv_compiler_t *c, const char *name, uint32_t len, bool keep) {
  sv_with_refs_t shadows = {0};
  with_shadows(c, name, len, &shadows);
  if (shadows.count == 0) {
    emit_set_var_plain(c, name, len, keep);
    return;
  }

  if (keep) emit_op(c, OP_DUP);
  sv_patch_list_t stored = {0};
  emit_with_tries(c, &shadows, OP_WITH_TRY_PUT, name, len, 0, &stored);
  emit_set_var_plain(c, name, len, false);
  
  int done = emit_jump(c, OP_JMP);
  patch_list_resolve(c, &stored);
  emit_op(c, OP_POP);
  patch_jump(c, done);
}

void emit_put_local_typed(sv_compiler_t *c, int local_idx, uint8_t type) {
  int slot = local_idx - c->param_locals;
  if (slot <= 255) { emit_op(c, OP_PUT_LOCAL8); emit(c, (uint8_t)slot); }
  else { emit_op(c, OP_PUT_LOCAL); emit_u16(c, (uint16_t)slot); }
  set_local_inferred_type(c, local_idx, type);
}

void emit_put_local(sv_compiler_t *c, int local_idx) {
  emit_put_local_typed(c, local_idx, SV_TI_UNKNOWN);
}

void emit_set_completion_from_stack(sv_compiler_t *c) {
  if (has_completion_accumulator(c)) emit_put_local(c, c->completion_local);
  else emit_op(c, OP_POP);
}

void emit_set_completion_undefined(sv_compiler_t *c) {
  if (!has_completion_accumulator(c)) return;
  emit_op(c, OP_UNDEF);
  emit_put_local(c, c->completion_local);
}

void emit_get_local(sv_compiler_t *c, int local_idx) {
  int slot = local_idx - c->param_locals;
  if (slot <= 255) { emit_op(c, OP_GET_LOCAL8); emit(c, (uint8_t)slot); }
  else { emit_op(c, OP_GET_LOCAL); emit_u16(c, (uint16_t)slot); }
}
