#include "jit_internal.h"
void mir_emit_value_to_objptr_or_jmp(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t v, MIR_reg_t out_ptr,
    MIR_reg_t r_tag, MIR_label_t slow) {
  MIR_label_t is_obj = MIR_new_label(ctx);
  MIR_label_t is_func = MIR_new_label(ctx);
  MIR_label_t done = MIR_new_label(ctx);

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_URSH,
                               MIR_new_reg_op(ctx, r_tag),
                               MIR_new_reg_op(ctx, v),
                               MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, is_obj),
                               MIR_new_reg_op(ctx, r_tag),
                               MIR_new_uint_op(ctx, NANBOX_TOBJ_TAG)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, is_obj),
                               MIR_new_reg_op(ctx, r_tag),
                               MIR_new_uint_op(ctx, NANBOX_TARR_TAG)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, is_obj),
                               MIR_new_reg_op(ctx, r_tag),
                               MIR_new_uint_op(ctx, NANBOX_TPROM_TAG)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, is_func),
                               MIR_new_reg_op(ctx, r_tag),
                               MIR_new_uint_op(ctx, NANBOX_TFUNC_TAG)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP,
                               MIR_new_label_op(ctx, slow)));

  MIR_append_insn(ctx, fn, is_obj);
  mir_emit_decode_ref(ctx, fn, out_ptr, v);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP,
                               MIR_new_label_op(ctx, done)));

  MIR_append_insn(ctx, fn, is_func);
  mir_emit_decode_ref(ctx, fn, out_ptr, v);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, out_ptr),
                               MIR_new_mem_op(ctx, MIR_T_I64,
                                              (MIR_disp_t)offsetof(sv_closure_t, func_obj),
                                              out_ptr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, out_ptr),
                               MIR_new_int_op(ctx, 0)));
  mir_emit_decode_ref(ctx, fn, out_ptr, out_ptr);
  MIR_append_insn(ctx, fn, done);
}

void mir_emit_ic_obj_epoch_guard(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t ic, MIR_label_t slow,
    const char *prefix, int bc_off, uint16_t ic_idx) {
  char ptr_name[40], current_name[40];
  snprintf(ptr_name, sizeof(ptr_name), "%s_oep_%d_%u",
           prefix, bc_off, (unsigned)ic_idx);
  snprintf(current_name, sizeof(current_name), "%s_oec_%d_%u",
           prefix, bc_off, (unsigned)ic_idx);
  MIR_reg_t epoch_scratch = MIR_new_func_reg(
      ctx, fn->u.func, MIR_T_I64, ptr_name);
  MIR_reg_t current = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, current_name);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, epoch_scratch),
                               MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)&ant_ic_obj_epoch_counter)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, current),
                               MIR_new_mem_op(ctx, MIR_T_U32, 0, epoch_scratch, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, epoch_scratch),
                               MIR_new_mem_op(ctx, MIR_T_U32,
                                              (MIR_disp_t)offsetof(
                                                  sv_ic_entry_t, guard.comparison.object_epoch),
                                              ic, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, epoch_scratch),
                               MIR_new_reg_op(ctx, current)));
}

static void mir_emit_promise_protector_invalidation(
    MIR_context_t ctx, MIR_item_t fn, ant_t *js,
    sv_atom_t *atom, int bc_off, uint16_t ic_idx,
    MIR_reg_t r_js, MIR_reg_t obj_ptr) {
  bool invalidates_constructor = atom->str == js->intern.constructor;
  bool invalidates_then = atom->str == js->intern.then;
  bool invalidates_global_promise = atom->str == js->intern.promise;
  bool invalidates_promise_resolve = atom->str == js->intern.resolve;

  if (
      !invalidates_constructor && !invalidates_then &&
      !invalidates_global_promise && !invalidates_promise_resolve) return;

  size_t protector_offset;
  if (invalidates_constructor)
    protector_offset = offsetof(ant_t, promise_constructor_protector_invalid);
  else if (invalidates_then)
    protector_offset = offsetof(ant_t, promise_then_protector_invalid);
  else
    protector_offset =
        offsetof(ant_t, promise_resolve_lookup_protector_invalid);

  char state_name[48];
  snprintf(
      state_name, sizeof(state_name), "pf_ps_%d_%u",
      bc_off, (unsigned)ic_idx);
  MIR_reg_t promise_state = MIR_new_func_reg(
      ctx, fn->u.func, MIR_T_I64, state_name);
  MIR_label_t invalidate = MIR_new_label(ctx);
  MIR_label_t done = MIR_new_label(ctx);

  if (invalidates_global_promise || invalidates_promise_resolve) {
    ant_value_t holder = invalidates_global_promise
                             ? js->global
                             : js->sym.promise_ctor;
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, done),
                                 MIR_new_reg_op(ctx, obj_ptr),
                                 MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)js_obj_ptr(holder))));
  } else {
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, promise_state),
                                 MIR_new_mem_op(ctx, MIR_T_P,
                                                (MIR_disp_t)offsetof(ant_object_t, promise_state), obj_ptr, 0, 1)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, invalidate),
                                 MIR_new_reg_op(ctx, promise_state), MIR_new_int_op(ctx, 0)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, done),
                                 MIR_new_reg_op(ctx, obj_ptr),
                                 MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)js_obj_ptr(js->sym.promise_proto))));
  }
  MIR_append_insn(ctx, fn, invalidate);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_mem_op(ctx, MIR_T_U8,
                                              (MIR_disp_t)protector_offset, r_js, 0, 1),
                               MIR_new_int_op(ctx, 1)));
  if (invalidates_constructor)
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_T_U8,
                                                (MIR_disp_t)offsetof(ant_t, promise_species_protector_invalid),
                                                r_js, 0, 1),
                                 MIR_new_int_op(ctx, 1)));
  MIR_append_insn(ctx, fn, done);
}

static MIR_reg_t mir_new_put_field_reg(
    MIR_context_t ctx, MIR_item_t fn, const char *suffix,
    int bc_off, uint16_t ic_idx) {
  char name[48];
  snprintf(name, sizeof(name), "pf_%s_%d_%u", suffix,
           bc_off, (unsigned)ic_idx);
  return MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, name);
}

static MIR_reg_t mir_emit_put_field_add_guard(
    MIR_context_t ctx, MIR_item_t fn, int bc_off, uint16_t ic_idx,
    MIR_reg_t ric, MIR_reg_t rce, MIR_reg_t optr, MIR_reg_t flags,
    MIR_reg_t shape, MIR_reg_t prop_count, MIR_reg_t idx, MIR_reg_t limit,
    MIR_label_t slow) {
  MIR_reg_t add_epoch = mir_new_put_field_reg(
      ctx, fn, "add_epoch", bc_off, ic_idx);
  MIR_reg_t add_from = mir_new_put_field_reg(
      ctx, fn, "add_from", bc_off, ic_idx);
  MIR_reg_t add_to = mir_new_put_field_reg(
      ctx, fn, "add_to", bc_off, ic_idx);
  MIR_reg_t shape_refs = mir_new_put_field_reg(
      ctx, fn, "shape_refs", bc_off, ic_idx);
  MIR_reg_t type_tag = mir_new_put_field_reg(
      ctx, fn, "type_tag", bc_off, ic_idx);
  MIR_reg_t scratch = mir_new_put_field_reg(
      ctx, fn, "add_scratch", bc_off, ic_idx);

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, scratch),
                               MIR_new_reg_op(ctx, flags),
                               MIR_new_uint_op(ctx, ANT_OBJECT_FLAG_FROZEN | ANT_OBJECT_FLAG_SEALED)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, scratch), MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, scratch),
                               MIR_new_reg_op(ctx, flags), MIR_new_uint_op(ctx, ANT_OBJECT_FLAG_EXTENSIBLE)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, scratch), MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, type_tag),
                               MIR_new_mem_op(ctx, MIR_T_U8,
                                              (MIR_disp_t)offsetof(ant_object_t, type_tag), optr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, type_tag), MIR_new_uint_op(ctx, kTypeArray)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, shape_refs),
                               MIR_new_mem_op(ctx, MIR_T_U8,
                                              (MIR_disp_t)offsetof(sv_ic_entry_t, shape_ref_mask), ric, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, shape_refs),
                               MIR_new_reg_op(ctx, shape_refs),
                               MIR_new_uint_op(ctx, SV_IC_SHAPE_REF_ADD_FROM | SV_IC_SHAPE_REF_ADD_TO)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, shape_refs),
                               MIR_new_uint_op(ctx, SV_IC_SHAPE_REF_ADD_FROM | SV_IC_SHAPE_REF_ADD_TO)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, add_epoch),
                               MIR_new_mem_op(ctx, MIR_T_U32,
                                              (MIR_disp_t)offsetof(sv_ic_entry_t, guard.add.epoch), ric, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, add_epoch), MIR_new_reg_op(ctx, rce)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, add_from),
                               MIR_new_mem_op(ctx, MIR_T_P,
                                              (MIR_disp_t)offsetof(sv_ic_entry_t, guard.add.from_shape), ric, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, add_from), MIR_new_reg_op(ctx, shape)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, add_to),
                               MIR_new_mem_op(ctx, MIR_T_P,
                                              (MIR_disp_t)offsetof(sv_ic_entry_t, guard.add.to_shape), ric, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, add_to), MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, idx),
                               MIR_new_mem_op(ctx, MIR_T_U32,
                                              (MIR_disp_t)offsetof(sv_ic_entry_t, guard.add.slot), ric, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, idx), MIR_new_reg_op(ctx, prop_count)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, limit),
                               MIR_new_mem_op(ctx, MIR_T_U8,
                                              (MIR_disp_t)offsetof(ant_object_t, inobj_limit), optr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_UBGE, MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, idx), MIR_new_reg_op(ctx, limit)));

  return add_to;
}

static void mir_emit_put_field_value_guard(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t val, MIR_reg_t optr, MIR_reg_t flags,
    MIR_reg_t scratch, MIR_reg_t vptr, MIR_reg_t rope_flags,
    MIR_reg_t need_barrier, MIR_label_t slow, MIR_label_t store) {
  MIR_label_t classify_tag = MIR_new_label(ctx);
  MIR_label_t classify_string = MIR_new_label(ctx);
  MIR_label_t classify_ref = MIR_new_label(ctx);

  mir_load_imm(ctx, fn, need_barrier, 0);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_UBGT, MIR_new_label_op(ctx, classify_tag),
                               MIR_new_reg_op(ctx, val), MIR_new_uint_op(ctx, NANBOX_PREFIX)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, store)));
  MIR_append_insn(ctx, fn, classify_tag);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_URSH, MIR_new_reg_op(ctx, scratch),
                               MIR_new_reg_op(ctx, val), MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, classify_string),
                               MIR_new_reg_op(ctx, scratch), MIR_new_uint_op(ctx, JIT_STR_TAG)));
  const uint64_t ref_tags[] = {
      NANBOX_TFUNC_TAG, NANBOX_TOBJ_TAG, NANBOX_TARR_TAG, NANBOX_TPROM_TAG,
      (NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | (uint64_t)kTypeGenerator};
  for (size_t i = 0; i < sizeof(ref_tags) / sizeof(ref_tags[0]); i++)
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, classify_ref),
                                 MIR_new_reg_op(ctx, scratch), MIR_new_uint_op(ctx, ref_tags[i])));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, store)));

  MIR_append_insn(ctx, fn, classify_ref);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, scratch),
                               MIR_new_reg_op(ctx, flags), MIR_new_uint_op(ctx, ANT_OBJECT_FLAG_GENERATION)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, scratch), MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, store)));

  MIR_append_insn(ctx, fn, classify_string);
  mir_emit_decode_ref(ctx, fn, vptr, val);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, scratch),
                               MIR_new_reg_op(ctx, vptr), MIR_new_uint_op(ctx, STR_HEAP_TAG_MASK)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, store),
                               MIR_new_reg_op(ctx, scratch), MIR_new_uint_op(ctx, STR_HEAP_TAG_ROPE)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, vptr),
                               MIR_new_reg_op(ctx, vptr), MIR_new_uint_op(ctx, ~STR_HEAP_TAG_MASK)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, rope_flags),
                               MIR_new_mem_op(ctx, MIR_T_U16,
                                              (MIR_disp_t)offsetof(ant_rope_heap_t, flags), vptr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, rope_flags),
                               MIR_new_reg_op(ctx, rope_flags), MIR_new_uint_op(ctx, ANT_ROPE_FLAG_YOUNG)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, store),
                               MIR_new_reg_op(ctx, rope_flags), MIR_new_int_op(ctx, 0)));
  mir_load_imm(ctx, fn, need_barrier, 1);
  MIR_append_insn(ctx, fn, store);
}

static void mir_emit_put_field_barrier(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t r_js, MIR_reg_t optr, MIR_reg_t flags,
    MIR_reg_t need_barrier, MIR_label_t done,
    MIR_item_t remember_proto, MIR_item_t imp_remember) {
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, done),
                               MIR_new_reg_op(ctx, need_barrier), MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, flags),
                               MIR_new_reg_op(ctx, flags),
                               MIR_new_uint_op(ctx, ANT_OBJECT_FLAG_GENERATION | ANT_OBJECT_FLAG_REMEMBERED)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, done),
                               MIR_new_reg_op(ctx, flags), MIR_new_uint_op(ctx, ANT_OBJECT_FLAG_GENERATION)));
  MIR_append_insn(ctx, fn,
                  MIR_new_call_insn(ctx, 4,
                                    MIR_new_ref_op(ctx, remember_proto),
                                    MIR_new_ref_op(ctx, imp_remember),
                                    MIR_new_reg_op(ctx, r_js),
                                    MIR_new_reg_op(ctx, optr)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
}

bool mir_emit_put_field_ic_fastpath(
    MIR_context_t ctx, MIR_item_t fn, ant_t *js,
    sv_func_t *func, int bc_off, uint16_t ic_idx, sv_atom_t *atom,
    MIR_reg_t r_js, MIR_reg_t obj, MIR_reg_t val,
    MIR_label_t slow, MIR_reg_t r_global_epoch,
    MIR_item_t shape_transition_proto, MIR_item_t imp_shape_transition,
    MIR_item_t remember_proto, MIR_item_t imp_remember) {
  if (!func || !func->ic_slots || !atom) return false;
  if (ic_idx == UINT16_MAX || ic_idx >= func->ic_count) return false;
  if (
      atom->str == js->intern.prototype ||
      atom->str == js->intern.exec ||
      atom->str == js->intern.replace) return false;
  bool can_add = atom->len != sizeof("__proto__") - 1 ||
                 memcmp(atom->str, "__proto__", sizeof("__proto__") - 1) != 0;

  sv_ic_entry_t *ic = &func->ic_slots[ic_idx];
  MIR_reg_t ric = mir_new_put_field_reg(ctx, fn, "ric", bc_off, ic_idx);
  MIR_reg_t rice = mir_new_put_field_reg(ctx, fn, "rice", bc_off, ic_idx);
  MIR_reg_t rce = mir_new_put_field_reg(ctx, fn, "rce", bc_off, ic_idx);
  MIR_reg_t optr = mir_new_put_field_reg(ctx, fn, "optr", bc_off, ic_idx);
  MIR_reg_t otag = mir_new_put_field_reg(ctx, fn, "otag", bc_off, ic_idx);
  MIR_reg_t flags = mir_new_put_field_reg(ctx, fn, "flags", bc_off, ic_idx);
  MIR_reg_t shape = mir_new_put_field_reg(ctx, fn, "shape", bc_off, ic_idx);
  MIR_reg_t icshape = mir_new_put_field_reg(ctx, fn, "icshape", bc_off, ic_idx);
  MIR_reg_t idx = mir_new_put_field_reg(ctx, fn, "idx", bc_off, ic_idx);
  MIR_reg_t prop_count = mir_new_put_field_reg(
      ctx, fn, "prop_count", bc_off, ic_idx);
  MIR_reg_t limit = mir_new_put_field_reg(ctx, fn, "limit", bc_off, ic_idx);
  MIR_reg_t overflow = mir_new_put_field_reg(
      ctx, fn, "overflow", bc_off, ic_idx);
  MIR_reg_t overflow_idx = mir_new_put_field_reg(
      ctx, fn, "overflow_idx", bc_off, ic_idx);
  MIR_reg_t vtag = mir_new_put_field_reg(ctx, fn, "vtag", bc_off, ic_idx);
  MIR_reg_t vptr = mir_new_put_field_reg(ctx, fn, "vp", bc_off, ic_idx);
  MIR_reg_t rope_flags = mir_new_put_field_reg(ctx, fn, "rf", bc_off, ic_idx);
  MIR_reg_t need_barrier = mir_new_put_field_reg(
      ctx, fn, "need", bc_off, ic_idx);

  MIR_label_t try_add = MIR_new_label(ctx);
  MIR_label_t existing_store = MIR_new_label(ctx);
  MIR_label_t load_overflow = MIR_new_label(ctx);
  MIR_label_t existing_after_store = MIR_new_label(ctx);
  MIR_label_t add_store = MIR_new_label(ctx);
  MIR_label_t done = MIR_new_label(ctx);

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, ric),
                               MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)ic)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, rice),
                               MIR_new_mem_op(ctx, MIR_T_U32,
                                              (MIR_disp_t)offsetof(sv_ic_entry_t, epoch), ric, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, rce),
                               MIR_new_mem_op(ctx, MIR_T_U32, 0, r_global_epoch, 0, 1)));

  mir_emit_value_to_objptr_or_jmp(ctx, fn, obj, optr, otag, slow);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, flags),
                               MIR_new_mem_op(ctx, MIR_T_U16,
                                              (MIR_disp_t)offsetof(ant_object_t, flags), optr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, otag),
                               MIR_new_reg_op(ctx, flags), MIR_new_uint_op(ctx, ANT_OBJECT_FLAG_EXOTIC)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, otag), MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, shape),
                               MIR_new_mem_op(ctx, MIR_T_P,
                                              (MIR_disp_t)offsetof(ant_object_t, shape), optr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, prop_count),
                               MIR_new_mem_op(ctx, MIR_T_U32,
                                              (MIR_disp_t)offsetof(ant_object_t, prop_count), optr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, icshape),
                               MIR_new_mem_op(ctx, MIR_T_P,
                                              (MIR_disp_t)offsetof(sv_ic_entry_t, cached_shape), ric, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, try_add),
                               MIR_new_reg_op(ctx, shape), MIR_new_reg_op(ctx, icshape)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, try_add),
                               MIR_new_reg_op(ctx, rice), MIR_new_reg_op(ctx, rce)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, idx),
                               MIR_new_mem_op(ctx, MIR_T_U32,
                                              (MIR_disp_t)offsetof(sv_ic_entry_t, cached_index), ric, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_UBGE, MIR_new_label_op(ctx, try_add),
                               MIR_new_reg_op(ctx, idx), MIR_new_reg_op(ctx, prop_count)));
  mir_emit_put_field_value_guard(
      ctx, fn, val, optr, flags, vtag, vptr, rope_flags,
      need_barrier, slow, existing_store);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, limit),
                               MIR_new_mem_op(ctx, MIR_T_U8,
                                              (MIR_disp_t)offsetof(ant_object_t, inobj_limit), optr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_UBGE, MIR_new_label_op(ctx, load_overflow),
                               MIR_new_reg_op(ctx, idx), MIR_new_reg_op(ctx, limit)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_mem_op(ctx, MIR_JSVAL,
                                              (MIR_disp_t)offsetof(ant_object_t, inobj), optr, idx, 8),
                               MIR_new_reg_op(ctx, val)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, existing_after_store)));
  MIR_append_insn(ctx, fn, load_overflow);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, overflow),
                               MIR_new_mem_op(ctx, MIR_T_P,
                                              (MIR_disp_t)offsetof(ant_object_t, overflow_prop), optr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, overflow), MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_SUB, MIR_new_reg_op(ctx, overflow_idx),
                               MIR_new_reg_op(ctx, idx), MIR_new_reg_op(ctx, limit)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_mem_op(ctx, MIR_JSVAL, 0, overflow, overflow_idx, 8),
                               MIR_new_reg_op(ctx, val)));
  MIR_append_insn(ctx, fn, existing_after_store);
  mir_emit_put_field_barrier(
      ctx, fn, r_js, optr, flags, need_barrier, done,
      remember_proto, imp_remember);

  MIR_append_insn(ctx, fn, try_add);
  if (!can_add) {
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, slow)));
  } else {
    MIR_reg_t add_to = mir_emit_put_field_add_guard(
        ctx, fn, bc_off, ic_idx, ric, rce, optr, flags,
        shape, prop_count, idx, limit, slow);
    mir_emit_put_field_value_guard(
        ctx, fn, val, optr, flags, vtag, vptr, rope_flags,
        need_barrier, slow, add_store);
    MIR_append_insn(ctx, fn,
                    MIR_new_call_insn(ctx, 4,
                                      MIR_new_ref_op(ctx, shape_transition_proto),
                                      MIR_new_ref_op(ctx, imp_shape_transition),
                                      MIR_new_reg_op(ctx, optr),
                                      MIR_new_reg_op(ctx, add_to)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, prop_count),
                                 MIR_new_reg_op(ctx, idx), MIR_new_int_op(ctx, 1)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_T_U32,
                                                (MIR_disp_t)offsetof(ant_object_t, prop_count), optr, 0, 1),
                                 MIR_new_reg_op(ctx, prop_count)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_mem_op(ctx, MIR_JSVAL,
                                                (MIR_disp_t)offsetof(ant_object_t, inobj), optr, idx, 8),
                                 MIR_new_reg_op(ctx, val)));
    mir_emit_put_field_barrier(
        ctx, fn, r_js, optr, flags, need_barrier, done,
        remember_proto, imp_remember);
  }

  MIR_append_insn(ctx, fn, done);
  mir_emit_promise_protector_invalidation(
      ctx, fn, js, atom, bc_off, ic_idx, r_js, optr);
  return true;
}

bool mir_emit_get_field_ic_fastpath(
    MIR_context_t ctx,
    MIR_item_t fn,
    sv_func_t *func,
    int bc_off,
    uint16_t ic_idx,
    sv_atom_t *atom,
    MIR_reg_t obj,
    MIR_reg_t dst,
    MIR_label_t slow,
    MIR_reg_t r_global_epoch) {
  if (!func || !func->ic_slots || !atom) return false;
  if (ic_idx == UINT16_MAX || ic_idx >= func->ic_count) return false;
  if (is_length_key(atom->str, atom->len)) return false;

  sv_ic_entry_t *ic = &func->ic_slots[ic_idx];
  if (!sv_gf_ic_active(ic->cached_aux) && func->code_len > 1024) return false;
  char gf_ic_name[32], gf_ice_name[32];
  char gf_ot_name[32], gf_op_name[32], gf_os_name[32], gf_ics_name[32];
  char gf_h_name[32], gf_hs_name[32], gf_idx_name[32], gf_pc_name[32];
  char gf_ica_name[32], gf_il_name[32], gf_ovf_name[32], gf_ovi_name[32];
  char gf_io_name[32], gf_src_name[32], gf_op_proto_name[32], gf_ic_proto_name[32];
  snprintf(gf_ic_name, sizeof(gf_ic_name), "gf_ic_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_ice_name, sizeof(gf_ice_name), "gf_ice_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_ot_name, sizeof(gf_ot_name), "gf_ot_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_op_name, sizeof(gf_op_name), "gf_op_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_os_name, sizeof(gf_os_name), "gf_os_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_ics_name, sizeof(gf_ics_name), "gf_ics_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_h_name, sizeof(gf_h_name), "gf_h_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_hs_name, sizeof(gf_hs_name), "gf_hs_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_idx_name, sizeof(gf_idx_name), "gf_idx_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_pc_name, sizeof(gf_pc_name), "gf_pc_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_ica_name, sizeof(gf_ica_name), "gf_ica_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_il_name, sizeof(gf_il_name), "gf_il_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_ovf_name, sizeof(gf_ovf_name), "gf_ovf_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_ovi_name, sizeof(gf_ovi_name), "gf_ovi_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_io_name, sizeof(gf_io_name), "gf_io_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_src_name, sizeof(gf_src_name), "gf_src_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_op_proto_name, sizeof(gf_op_proto_name), "gf_opp_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_ic_proto_name, sizeof(gf_ic_proto_name), "gf_icp_%d_%u", bc_off, (unsigned)ic_idx);

  MIR_reg_t r_ic = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_ic_name);
  MIR_reg_t r_ic_epoch = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_ice_name);
  MIR_reg_t r_obj_tag = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_ot_name);
  MIR_reg_t r_obj_ptr = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_op_name);
  MIR_reg_t r_obj_shape = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_os_name);
  MIR_reg_t r_ic_shape = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_ics_name);
  MIR_reg_t r_holder = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_h_name);
  MIR_reg_t r_holder_shape = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_hs_name);
  MIR_reg_t r_ic_idx_val = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_idx_name);
  MIR_reg_t r_holder_prop_count = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_pc_name);
  MIR_reg_t r_ic_aux = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_ica_name);
  MIR_reg_t r_inobj_limit = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_il_name);
  MIR_reg_t r_overflow = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_ovf_name);
  MIR_reg_t r_overflow_idx = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_ovi_name);
  MIR_reg_t r_is_own = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_io_name);
  MIR_reg_t r_source = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_src_name);
  MIR_reg_t r_obj_proto = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_op_proto_name);
  MIR_reg_t r_ic_proto = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_ic_proto_name);
  char gf_pp_name[32], gf_pid_name[32], gf_ipid_name[32];
  snprintf(gf_pp_name, sizeof(gf_pp_name), "gf_pp_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_pid_name, sizeof(gf_pid_name), "gf_pid_%d_%u", bc_off, (unsigned)ic_idx);
  snprintf(gf_ipid_name, sizeof(gf_ipid_name), "gf_ipid_%d_%u", bc_off, (unsigned)ic_idx);
  MIR_reg_t r_proto_ptr = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_pp_name);
  MIR_reg_t r_proto_id = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_pid_name);
  MIR_reg_t r_ic_proto_id = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, gf_ipid_name);
  MIR_label_t load_overflow = MIR_new_label(ctx);
  MIR_label_t fast_done = MIR_new_label(ctx);
  MIR_label_t own_path = MIR_new_label(ctx);
  MIR_label_t do_read = MIR_new_label(ctx);

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_ic),
                               MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)ic)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_ic_aux),
                               MIR_new_mem_op(ctx, MIR_T_I64,
                                              (MIR_disp_t)offsetof(sv_ic_entry_t, cached_aux), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_AND,
                               MIR_new_reg_op(ctx, r_ic_aux),
                               MIR_new_reg_op(ctx, r_ic_aux),
                               MIR_new_uint_op(ctx, (uint64_t)SV_GF_IC_AUX_ACTIVE_BIT)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, r_ic_aux),
                               MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_ic_epoch),
                               MIR_new_mem_op(ctx, MIR_T_U32,
                                              (MIR_disp_t)offsetof(sv_ic_entry_t, epoch), r_ic, 0, 1)));
  {
    char ce_name[40];
    snprintf(ce_name, sizeof(ce_name), "gf_ce_%d_%u", bc_off, (unsigned)ic_idx);
    MIR_reg_t r_cur_epoch = MIR_new_func_reg(
        ctx, fn->u.func, MIR_T_I64, ce_name);
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_MOV,
                                 MIR_new_reg_op(ctx, r_cur_epoch),
                                 MIR_new_mem_op(ctx, MIR_T_U32, 0, r_global_epoch, 0, 1)));
    MIR_append_insn(ctx, fn,
                    MIR_new_insn(ctx, MIR_BNE,
                                 MIR_new_label_op(ctx, slow),
                                 MIR_new_reg_op(ctx, r_ic_epoch),
                                 MIR_new_reg_op(ctx, r_cur_epoch)));
  }

  mir_emit_value_to_objptr_or_jmp(
      ctx, fn, obj, r_obj_ptr, r_obj_tag, slow);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_obj_shape),
                               MIR_new_mem_op(ctx, MIR_T_P,
                                              (MIR_disp_t)offsetof(ant_object_t, shape), r_obj_ptr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_ic_shape),
                               MIR_new_mem_op(ctx, MIR_T_P,
                                              (MIR_disp_t)offsetof(sv_ic_entry_t, cached_shape), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, r_obj_shape),
                               MIR_new_reg_op(ctx, r_ic_shape)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_is_own),
                               MIR_new_mem_op(ctx, MIR_T_U8,
                                              (MIR_disp_t)offsetof(sv_ic_entry_t, cached_is_own), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, own_path),
                               MIR_new_reg_op(ctx, r_is_own),
                               MIR_new_int_op(ctx, 0)));

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_obj_proto),
                               MIR_new_mem_op(ctx, MIR_T_I64,
                                              (MIR_disp_t)offsetof(ant_object_t, proto), r_obj_ptr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_ic_proto),
                               MIR_new_mem_op(ctx, MIR_T_I64,
                                              (MIR_disp_t)offsetof(sv_ic_entry_t, guard.receiver_proto), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, r_obj_proto),
                               MIR_new_reg_op(ctx, r_ic_proto)));
  mir_emit_decode_ref(ctx, fn, r_proto_ptr, r_obj_proto);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_proto_id),
                               MIR_new_mem_op(ctx, MIR_T_U32,
                                              (MIR_disp_t)offsetof(ant_object_t, ic_identity), r_proto_ptr, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_ic_proto_id),
                               MIR_new_mem_op(ctx, MIR_T_U64,
                                              (MIR_disp_t)offsetof(sv_ic_entry_t, cached_aux), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_URSH,
                               MIR_new_reg_op(ctx, r_ic_proto_id),
                               MIR_new_reg_op(ctx, r_ic_proto_id),
                               MIR_new_uint_op(ctx, SV_GF_IC_PROTO_ID_SHIFT)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, r_proto_id),
                               MIR_new_reg_op(ctx, r_ic_proto_id)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_holder),
                               MIR_new_mem_op(ctx, MIR_T_P,
                                              (MIR_disp_t)offsetof(sv_ic_entry_t, cached_holder), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, r_holder),
                               MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_holder_shape),
                               MIR_new_mem_op(ctx, MIR_T_P,
                                              (MIR_disp_t)offsetof(ant_object_t, shape), r_holder, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, r_holder_shape),
                               MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_source),
                               MIR_new_reg_op(ctx, r_holder)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP,
                               MIR_new_label_op(ctx, do_read)));

  MIR_append_insn(ctx, fn, own_path);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_source),
                               MIR_new_reg_op(ctx, r_obj_ptr)));

  MIR_append_insn(ctx, fn, do_read);

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_ic_idx_val),
                               MIR_new_mem_op(ctx, MIR_T_U32,
                                              (MIR_disp_t)offsetof(sv_ic_entry_t, cached_index), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_holder_prop_count),
                               MIR_new_mem_op(ctx, MIR_T_U32,
                                              (MIR_disp_t)offsetof(ant_object_t, prop_count), r_source, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_UBGE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, r_ic_idx_val),
                               MIR_new_reg_op(ctx, r_holder_prop_count)));

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_inobj_limit),
                               MIR_new_mem_op(ctx, MIR_T_U8,
                                              (MIR_disp_t)offsetof(ant_object_t, inobj_limit), r_source, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_UBGE,
                               MIR_new_label_op(ctx, load_overflow),
                               MIR_new_reg_op(ctx, r_ic_idx_val),
                               MIR_new_reg_op(ctx, r_inobj_limit)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, dst),
                               MIR_new_mem_op(ctx, MIR_T_I64,
                                              (MIR_disp_t)offsetof(ant_object_t, inobj), r_source, r_ic_idx_val, 8)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP,
                               MIR_new_label_op(ctx, fast_done)));

  MIR_append_insn(ctx, fn, load_overflow);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_overflow),
                               MIR_new_mem_op(ctx, MIR_T_P,
                                              (MIR_disp_t)offsetof(ant_object_t, overflow_prop), r_source, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, r_overflow),
                               MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_SUB,
                               MIR_new_reg_op(ctx, r_overflow_idx),
                               MIR_new_reg_op(ctx, r_ic_idx_val),
                               MIR_new_reg_op(ctx, r_inobj_limit)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, dst),
                               MIR_new_mem_op(ctx, MIR_T_I64, 0, r_overflow, r_overflow_idx, 8)));
  MIR_append_insn(ctx, fn, fast_done);

  return true;
}

bool mir_emit_get_global_ic_fastpath(
    MIR_context_t ctx, MIR_item_t fn,
    sv_func_t *func, int bc_off,
    MIR_reg_t r_js, MIR_reg_t dst,
    MIR_label_t slow, MIR_reg_t r_global_epoch,
    uint8_t *ip) {
  if (!func || !func->ic_slots || !ip) return false;
  sv_ic_entry_t *ic = sv_global_ic_slot_for_ip(func, ip);
  if (!ic) return false;

  char n_ic[32], n_e[32], n_ce[32], n_gv[32], n_gt[32], n_gp[32];
  char n_sh[32], n_ics[32], n_idx[32], n_pc[32], n_il[32], n_ov[32], n_oi[32];
  snprintf(n_ic, sizeof(n_ic), "gg_ic_%d", bc_off);
  snprintf(n_e, sizeof(n_e), "gg_e_%d", bc_off);
  snprintf(n_ce, sizeof(n_ce), "gg_ce_%d", bc_off);
  snprintf(n_gv, sizeof(n_gv), "gg_gv_%d", bc_off);
  snprintf(n_gt, sizeof(n_gt), "gg_gt_%d", bc_off);
  snprintf(n_gp, sizeof(n_gp), "gg_gp_%d", bc_off);
  snprintf(n_sh, sizeof(n_sh), "gg_sh_%d", bc_off);
  snprintf(n_ics, sizeof(n_ics), "gg_ics_%d", bc_off);
  snprintf(n_idx, sizeof(n_idx), "gg_idx_%d", bc_off);
  snprintf(n_pc, sizeof(n_pc), "gg_pc_%d", bc_off);
  snprintf(n_il, sizeof(n_il), "gg_il_%d", bc_off);
  snprintf(n_ov, sizeof(n_ov), "gg_ov_%d", bc_off);
  snprintf(n_oi, sizeof(n_oi), "gg_oi_%d", bc_off);

  MIR_reg_t r_ic = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_ic);
  MIR_reg_t r_e = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_e);
  MIR_reg_t r_ce = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_ce);
  MIR_reg_t r_gv = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_gv);
  MIR_reg_t r_gt = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_gt);
  MIR_reg_t r_gp = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_gp);
  MIR_reg_t r_sh = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_sh);
  MIR_reg_t r_ics = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_ics);
  MIR_reg_t r_idx = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_idx);
  MIR_reg_t r_pc = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_pc);
  MIR_reg_t r_il = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_il);
  MIR_reg_t r_ov = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_ov);
  MIR_reg_t r_oi = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, n_oi);

  MIR_label_t load_overflow = MIR_new_label(ctx);
  MIR_label_t fast_done = MIR_new_label(ctx);

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_ic),
                               MIR_new_uint_op(ctx, (uint64_t)(uintptr_t)ic)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_e),
                               MIR_new_mem_op(ctx, MIR_T_U32,
                                              (MIR_disp_t)offsetof(sv_ic_entry_t, epoch), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_ce),
                               MIR_new_mem_op(ctx, MIR_T_U32, 0, r_global_epoch, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, r_e),
                               MIR_new_reg_op(ctx, r_ce)));

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_gv),
                               MIR_new_mem_op(ctx, MIR_T_I64,
                                              (MIR_disp_t)offsetof(ant_t, global), r_js, 0, 1)));
  mir_emit_value_to_objptr_or_jmp(ctx, fn, r_gv, r_gp, r_gt, slow);

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_sh),
                               MIR_new_mem_op(ctx, MIR_T_P,
                                              (MIR_disp_t)offsetof(ant_object_t, shape), r_gp, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_ics),
                               MIR_new_mem_op(ctx, MIR_T_P,
                                              (MIR_disp_t)offsetof(sv_ic_entry_t, cached_shape), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, r_ics),
                               MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BNE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, r_sh),
                               MIR_new_reg_op(ctx, r_ics)));

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_idx),
                               MIR_new_mem_op(ctx, MIR_T_U32,
                                              (MIR_disp_t)offsetof(sv_ic_entry_t, cached_index), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_pc),
                               MIR_new_mem_op(ctx, MIR_T_U32,
                                              (MIR_disp_t)offsetof(ant_object_t, prop_count), r_gp, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_UBGE,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, r_idx),
                               MIR_new_reg_op(ctx, r_pc)));

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_il),
                               MIR_new_mem_op(ctx, MIR_T_U8,
                                              (MIR_disp_t)offsetof(ant_object_t, inobj_limit), r_gp, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_UBGE,
                               MIR_new_label_op(ctx, load_overflow),
                               MIR_new_reg_op(ctx, r_idx),
                               MIR_new_reg_op(ctx, r_il)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, dst),
                               MIR_new_mem_op(ctx, MIR_T_I64,
                                              (MIR_disp_t)offsetof(ant_object_t, inobj), r_gp, r_idx, 8)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_JMP,
                               MIR_new_label_op(ctx, fast_done)));

  MIR_append_insn(ctx, fn, load_overflow);
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_ov),
                               MIR_new_mem_op(ctx, MIR_T_P,
                                              (MIR_disp_t)offsetof(ant_object_t, overflow_prop), r_gp, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, slow),
                               MIR_new_reg_op(ctx, r_ov),
                               MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_SUB,
                               MIR_new_reg_op(ctx, r_oi),
                               MIR_new_reg_op(ctx, r_idx),
                               MIR_new_reg_op(ctx, r_il)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, dst),
                               MIR_new_mem_op(ctx, MIR_T_I64, 0, r_ov, r_oi, 8)));
  MIR_append_insn(ctx, fn, fast_done);

  return true;
}
