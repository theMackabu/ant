#include "jit_internal.h"
#include "../silver/ops/globals.h"

static MIR_reg_t mir_new_ic_reg(
    MIR_context_t ctx, MIR_item_t fn, const char *prefix, const char *suffix,
    int bc_off, int ic_idx) {
  char name[48];
  if (ic_idx < 0)
    snprintf(name, sizeof(name), "%s_%s_%d", prefix, suffix, bc_off);
  else
    snprintf(name, sizeof(name), "%s_%s_%d_%u", prefix, suffix, bc_off, (unsigned)ic_idx);
  return MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, name);
}

void mir_emit_value_to_objptr_or_jmp(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t v, MIR_reg_t out_ptr,
    MIR_reg_t r_tag, MIR_label_t slow) {
  MIR_label_t done = MIR_new_label(ctx);

  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_URSH,
      MIR_new_reg_op(ctx, r_tag), MIR_new_reg_op(ctx, v),
      MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  mir_emit_decode_ref(ctx, fn, out_ptr, v);
  const uint64_t object_tags[] = {NANBOX_TOBJ_TAG, NANBOX_TARR_TAG, NANBOX_TPROM_TAG};
  for (size_t i = 0; i < sizeof(object_tags) / sizeof(object_tags[0]); i++)
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BEQ,
        MIR_new_label_op(ctx, done), MIR_new_reg_op(ctx, r_tag),
        MIR_new_uint_op(ctx, object_tags[i])));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, r_tag),
      MIR_new_uint_op(ctx, NANBOX_TFUNC_TAG)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, out_ptr),
      MIR_new_mem_op(ctx, MIR_T_I64, offsetof(sv_closure_t, func_obj), out_ptr, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BEQ,
      MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, out_ptr), MIR_new_int_op(ctx, 0)));
  mir_emit_decode_ref(ctx, fn, out_ptr, out_ptr);
  MIR_append_insn(ctx, fn, done);
}

void mir_emit_ic_obj_epoch_guard(
    MIR_context_t ctx, MIR_item_t fn,
    MIR_reg_t ic, MIR_label_t slow,
    const char *prefix, int bc_off, uint16_t ic_idx) {
  MIR_reg_t epoch_scratch = mir_new_ic_reg(ctx, fn, prefix, "oep", bc_off, ic_idx);
  MIR_reg_t current = mir_new_ic_reg(ctx, fn, prefix, "oec", bc_off, ic_idx);
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

  MIR_reg_t promise_state = mir_new_ic_reg(ctx, fn, "pf", "ps", bc_off, ic_idx);
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

static MIR_reg_t mir_emit_put_field_add_guard(
    MIR_context_t ctx, MIR_item_t fn, int bc_off, uint16_t ic_idx,
    MIR_reg_t ric, MIR_reg_t rce, MIR_reg_t optr, MIR_reg_t flags,
    MIR_reg_t shape, MIR_reg_t prop_count, MIR_reg_t idx, MIR_reg_t limit,
    MIR_label_t slow) {
  MIR_reg_t add_epoch = mir_new_ic_reg(ctx, fn, "pf", "add_epoch", bc_off, ic_idx);
  MIR_reg_t add_from = mir_new_ic_reg(ctx, fn, "pf", "add_from", bc_off, ic_idx);
  MIR_reg_t add_to = mir_new_ic_reg(ctx, fn, "pf", "add_to", bc_off, ic_idx);
  MIR_reg_t shape_refs = mir_new_ic_reg(ctx, fn, "pf", "shape_refs", bc_off, ic_idx);
  MIR_reg_t type_tag = mir_new_ic_reg(ctx, fn, "pf", "type_tag", bc_off, ic_idx);
  MIR_reg_t scratch = mir_new_ic_reg(ctx, fn, "pf", "add_scratch", bc_off, ic_idx);

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
  MIR_reg_t ric = mir_new_ic_reg(ctx, fn, "pf", "ric", bc_off, ic_idx);
  MIR_reg_t rice = mir_new_ic_reg(ctx, fn, "pf", "rice", bc_off, ic_idx);
  MIR_reg_t rce = mir_new_ic_reg(ctx, fn, "pf", "rce", bc_off, ic_idx);
  MIR_reg_t optr = mir_new_ic_reg(ctx, fn, "pf", "optr", bc_off, ic_idx);
  MIR_reg_t otag = mir_new_ic_reg(ctx, fn, "pf", "otag", bc_off, ic_idx);
  MIR_reg_t flags = mir_new_ic_reg(ctx, fn, "pf", "flags", bc_off, ic_idx);
  MIR_reg_t shape = mir_new_ic_reg(ctx, fn, "pf", "shape", bc_off, ic_idx);
  MIR_reg_t icshape = mir_new_ic_reg(ctx, fn, "pf", "icshape", bc_off, ic_idx);
  MIR_reg_t idx = mir_new_ic_reg(ctx, fn, "pf", "idx", bc_off, ic_idx);
  MIR_reg_t prop_count = mir_new_ic_reg(ctx, fn, "pf", "prop_count", bc_off, ic_idx);
  MIR_reg_t limit = mir_new_ic_reg(ctx, fn, "pf", "limit", bc_off, ic_idx);
  MIR_reg_t overflow = mir_new_ic_reg(ctx, fn, "pf", "overflow", bc_off, ic_idx);
  MIR_reg_t overflow_idx = mir_new_ic_reg(ctx, fn, "pf", "overflow_idx", bc_off, ic_idx);
  MIR_reg_t vtag = mir_new_ic_reg(ctx, fn, "pf", "vtag", bc_off, ic_idx);
  MIR_reg_t vptr = mir_new_ic_reg(ctx, fn, "pf", "vp", bc_off, ic_idx);
  MIR_reg_t rope_flags = mir_new_ic_reg(ctx, fn, "pf", "rf", bc_off, ic_idx);
  MIR_reg_t need_barrier = mir_new_ic_reg(ctx, fn, "pf", "need", bc_off, ic_idx);

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

static void mir_emit_get_field_kind_epoch_guard(
    MIR_context_t ctx, MIR_item_t fn, sv_ic_entry_t *ic,
    sv_get_field_ic_kind_t expected_kind,
    MIR_reg_t cache, MIR_reg_t kind_reg, MIR_reg_t tmp, MIR_reg_t expect,
    MIR_reg_t r_global_epoch, MIR_label_t slow) {
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, cache), MIR_new_uint_op(ctx, (uintptr_t)ic)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, kind_reg),
      MIR_new_mem_op(ctx, MIR_T_U8, offsetof(sv_ic_entry_t, get_kind), cache, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, kind_reg), MIR_new_int_op(ctx, expected_kind)));

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  if (expected_kind == SV_GF_IC_OWN) {
    uint32_t index = ic->cached_index;
    // Index and epoch are adjacent words: one aligned 64-bit load compares both
    // against (global_epoch << 32 | index).
    static_assert(offsetof(sv_ic_entry_t, epoch) == offsetof(sv_ic_entry_t, cached_index) + 4,
                  "field IC index and epoch must be adjacent");
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, tmp),
        MIR_new_mem_op(ctx, MIR_T_U64, offsetof(sv_ic_entry_t, cached_index), cache, 0, 1)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, expect), MIR_new_mem_op(ctx, MIR_T_U32, 0, r_global_epoch, 0, 1)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_LSH,
        MIR_new_reg_op(ctx, expect), MIR_new_reg_op(ctx, expect), MIR_new_int_op(ctx, 32)));
    if (index != 0)
      MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_OR,
          MIR_new_reg_op(ctx, expect), MIR_new_reg_op(ctx, expect), MIR_new_uint_op(ctx, index)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
        MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tmp), MIR_new_reg_op(ctx, expect)));
    return;
  }
#endif
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, tmp),
      MIR_new_mem_op(ctx, MIR_T_U32, offsetof(sv_ic_entry_t, epoch), cache, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, expect), MIR_new_mem_op(ctx, MIR_T_U32, 0, r_global_epoch, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tmp), MIR_new_reg_op(ctx, expect)));
  if (expected_kind == SV_GF_IC_OWN) {
    uint32_t index = ic->cached_index;
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, tmp),
        MIR_new_mem_op(ctx, MIR_T_U32, offsetof(sv_ic_entry_t, cached_index), cache, 0, 1)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
        MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tmp), MIR_new_uint_op(ctx, index)));
  }
}

static bool mir_emit_get_field_own_slot_fastpath(
    MIR_context_t ctx,
    MIR_item_t fn,
    sv_ic_entry_t *ic,
    int bc_off,
    uint16_t ic_idx,
    MIR_reg_t obj,
    MIR_reg_t dst,
    MIR_label_t slow,
    MIR_reg_t r_global_epoch) {
  MIR_reg_t cache = mir_new_ic_reg(ctx, fn, "gf_own", "ic", bc_off, ic_idx);
  MIR_reg_t ptr = mir_new_ic_reg(ctx, fn, "gf_own", "obj", bc_off, ic_idx);
  MIR_reg_t tmp = mir_new_ic_reg(ctx, fn, "gf_own", "tmp", bc_off, ic_idx);
  MIR_reg_t expect = mir_new_ic_reg(ctx, fn, "gf_own", "expect", bc_off, ic_idx);
  uint32_t index = ic->cached_index;
  uint8_t inobj_limit = ant_shape_get_inobj_limit(ic->cached_shape);

  mir_emit_get_field_kind_epoch_guard(
      ctx, fn, ic, SV_GF_IC_OWN, cache, tmp, tmp, expect, r_global_epoch, slow);

  mir_emit_value_to_objptr_or_jmp(ctx, fn, obj, ptr, tmp, slow);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, tmp),
      MIR_new_mem_op(ctx, MIR_T_P, offsetof(ant_object_t, shape), ptr, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, expect),
      MIR_new_mem_op(ctx, MIR_T_P, offsetof(sv_ic_entry_t, cached_shape), cache, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tmp), MIR_new_reg_op(ctx, expect)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, tmp),
      MIR_new_mem_op(ctx, MIR_T_U32, offsetof(ant_object_t, prop_count), ptr, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_UBLE,
      MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tmp), MIR_new_uint_op(ctx, index)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, tmp),
      MIR_new_mem_op(ctx, MIR_T_U8, offsetof(ant_object_t, inobj_limit), ptr, 0, 1)));
  if (index < inobj_limit) {
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_UBLE,
        MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tmp), MIR_new_uint_op(ctx, index)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, dst), MIR_new_mem_op(ctx, MIR_T_I64,
            offsetof(ant_object_t, inobj) + index * sizeof(ant_value_t), ptr, 0, 1)));
  } else {
    // The overflow offset depends on the allocation's inline capacity as
    // well as the property index. Guard both before using a constant offset.
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
        MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tmp), MIR_new_uint_op(ctx, inobj_limit)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, tmp),
        MIR_new_mem_op(ctx, MIR_T_P, offsetof(ant_object_t, overflow_prop), ptr, 0, 1)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BEQ,
        MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tmp), MIR_new_int_op(ctx, 0)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, dst), MIR_new_mem_op(ctx, MIR_T_I64,
            (index - inobj_limit) * sizeof(ant_value_t), tmp, 0, 1)));
  }
  return true;
}

static bool mir_emit_get_field_missing_fastpath(
    MIR_context_t ctx,
    MIR_item_t fn,
    sv_ic_entry_t *ic,
    int bc_off,
    uint16_t ic_idx,
    MIR_reg_t obj,
    MIR_reg_t dst,
    MIR_label_t slow,
    MIR_reg_t r_global_epoch) {
  MIR_reg_t cache = mir_new_ic_reg(ctx, fn, "gf_miss", "ic", bc_off, ic_idx);
  MIR_reg_t kind = mir_new_ic_reg(ctx, fn, "gf_miss", "kind", bc_off, ic_idx);
  MIR_reg_t ptr = mir_new_ic_reg(ctx, fn, "gf_miss", "obj", bc_off, ic_idx);
  MIR_reg_t proto = mir_new_ic_reg(ctx, fn, "gf_miss", "proto", bc_off, ic_idx);
  MIR_reg_t tmp = mir_new_ic_reg(ctx, fn, "gf_miss", "tmp", bc_off, ic_idx);
  MIR_reg_t expect = mir_new_ic_reg(ctx, fn, "gf_miss", "expect", bc_off, ic_idx);

  mir_emit_get_field_kind_epoch_guard(
      ctx, fn, ic, SV_GF_IC_MISSING, cache, kind, tmp, expect, r_global_epoch, slow);

  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_URSH,
      MIR_new_reg_op(ctx, tmp), MIR_new_reg_op(ctx, obj), MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tmp), MIR_new_uint_op(ctx, NANBOX_TOBJ_TAG)));
  mir_emit_decode_ref(ctx, fn, ptr, obj);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, tmp),
      MIR_new_mem_op(ctx, MIR_T_U8, offsetof(ant_object_t, type_tag), ptr, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tmp), MIR_new_uint_op(ctx, kTypeObject)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, tmp),
      MIR_new_mem_op(ctx, MIR_T_U16, offsetof(ant_object_t, flags), ptr, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_AND,
      MIR_new_reg_op(ctx, tmp), MIR_new_reg_op(ctx, tmp), MIR_new_uint_op(ctx, ANT_OBJECT_FLAG_EXOTIC)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tmp), MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, tmp),
      MIR_new_mem_op(ctx, MIR_T_P, offsetof(ant_object_t, shape), ptr, 0, 1)));
  // This site's shape reference was registered before specialization.
  // Subsequent missing fills retain a non-null shape without allocating, so
  // equality also excludes receivers without a shape.
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, expect),
      MIR_new_mem_op(ctx, MIR_T_P, offsetof(sv_ic_entry_t, cached_shape), cache, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tmp), MIR_new_reg_op(ctx, expect)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, proto),
      MIR_new_mem_op(ctx, MIR_T_I64, offsetof(ant_object_t, proto), ptr, 0, 1)));
  // Specialize the prototype value just as the positive prototype handler
  // does. Decode only the receiver's live prototype; its identity must still
  // match the current IC, including after GC or cache replacement.
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, proto), MIR_new_uint_op(ctx, ic->guard.receiver_proto)));
  if (vtype(ic->guard.receiver_proto) == kTypeNull) {
    mir_load_imm(ctx, fn, dst, mkval(kTypeUndefined, 0));
    return true;
  }
  if (vtype(ic->guard.receiver_proto) == kTypeFunction)
    mir_emit_value_to_objptr_or_jmp(ctx, fn, proto, ptr, tmp, slow);
  else
    mir_emit_decode_ref(ctx, fn, ptr, proto);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, tmp),
      MIR_new_mem_op(ctx, MIR_T_U32, offsetof(ant_object_t, ic_identity), ptr, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, expect),
      MIR_new_mem_op(ctx, MIR_T_U64, offsetof(sv_ic_entry_t, cached_aux), cache, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_URSH,
      MIR_new_reg_op(ctx, expect), MIR_new_reg_op(ctx, expect), MIR_new_uint_op(ctx, SV_GF_IC_PROTO_ID_SHIFT)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, tmp), MIR_new_reg_op(ctx, expect)));

  mir_load_imm(ctx, fn, dst, mkval(kTypeUndefined, 0));
  return true;
}

static bool mir_emit_get_field_shape_snapshot(
    MIR_context_t ctx, MIR_item_t fn, ant_t *js, sv_ic_entry_t *ic,
    sv_atom_t *atom, int bc_off, uint16_t ic_idx, MIR_reg_t obj,
    MIR_reg_t dst, MIR_label_t miss) {
  ant_shape_t *shape = ic->cached_shape;
  const uint32_t *guard = ant_shape_jit_guard(shape);
  uint32_t index = ic->cached_index;
  const ant_shape_prop_t *prop = ant_shape_prop_at(shape, index);
  // Virtual string/index properties can override ordinary slot lookup.
  if (!js || !guard || *guard || !atom->len ||
      (atom->str[0] >= '0' && atom->str[0] <= '9') ||
      !prop || prop->type != ANT_SHAPE_KEY_STRING || prop->key.interned != atom->str ||
      prop->has_getter || prop->has_setter) return false;

  // IC entries are mutable; keep an independent reference until code teardown.
  // Retention also makes ordinary object metadata updates take the COW path.
  ant_shape_t **root = code_arena_bump(sizeof(*root));
  if (!root) return false;
  *root = NULL;
  if (!sv_ic_shape_ref_register(js, root)) return false;
  ant_shape_retain(shape);
  *root = shape;

  MIR_reg_t ptr = mir_new_ic_reg(ctx, fn, "gf_snapshot", "obj", bc_off, ic_idx);
  MIR_reg_t tmp = mir_new_ic_reg(ctx, fn, "gf_snapshot", "tmp", bc_off, ic_idx);
  MIR_reg_t expect = mir_new_ic_reg(ctx, fn, "gf_snapshot", "shape", bc_off, ic_idx);
  mir_emit_value_to_objptr_or_jmp(ctx, fn, obj, ptr, tmp, miss);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, expect), MIR_new_uint_op(ctx, (uintptr_t)shape)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, tmp),
      MIR_new_mem_op(ctx, MIR_T_P, offsetof(ant_object_t, shape), ptr, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, miss), MIR_new_reg_op(ctx, tmp), MIR_new_reg_op(ctx, expect)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, tmp), MIR_new_mem_op(ctx, MIR_T_U32,
          (const char *)guard - (const char *)shape, expect, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, miss), MIR_new_reg_op(ctx, tmp), MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, tmp),
      MIR_new_mem_op(ctx, MIR_T_U16, offsetof(ant_object_t, flags), ptr, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_AND,
      MIR_new_reg_op(ctx, tmp), MIR_new_reg_op(ctx, tmp), MIR_new_uint_op(ctx, ANT_OBJECT_FLAG_EXOTIC)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, miss), MIR_new_reg_op(ctx, tmp), MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, tmp),
      MIR_new_mem_op(ctx, MIR_T_U32, offsetof(ant_object_t, prop_count), ptr, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_UBLE,
      MIR_new_label_op(ctx, miss), MIR_new_reg_op(ctx, tmp), MIR_new_uint_op(ctx, index)));
  uint8_t inobj_limit = ant_shape_get_inobj_limit(shape);
  if (index < inobj_limit) {
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, dst), MIR_new_mem_op(ctx, MIR_JSVAL,
            offsetof(ant_object_t, inobj) + index * sizeof(ant_value_t), ptr, 0, 1)));
  } else {
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, tmp),
        MIR_new_mem_op(ctx, MIR_T_P, offsetof(ant_object_t, overflow_prop), ptr, 0, 1)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BEQ,
        MIR_new_label_op(ctx, miss), MIR_new_reg_op(ctx, tmp), MIR_new_int_op(ctx, 0)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, dst), MIR_new_mem_op(ctx, MIR_JSVAL,
            (index - inobj_limit) * sizeof(ant_value_t), tmp, 0, 1)));
  }
  return true;
}

bool mir_emit_get_field_ic_fastpath(
    MIR_context_t ctx,
    MIR_item_t fn,
    ant_t *js,
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
  if (ic->get_kind == SV_GF_IC_SYMBOL_DESCRIPTION ||
      ic->get_kind == SV_GF_IC_PRIMITIVE_SYMBOL_DESCRIPTION) return false;
  if (!sv_gf_ic_active(ic->cached_aux) && func->code_len > 1024) return false;
  if (sv_gf_ic_active(ic->cached_aux) && ic->get_kind == SV_GF_IC_OWN && ic->cached_shape) {
    MIR_label_t miss = MIR_new_label(ctx);
    MIR_label_t done = MIR_new_label(ctx);
    bool snapshot = mir_emit_get_field_shape_snapshot(
        ctx, fn, js, ic, atom, bc_off, ic_idx, obj, dst, miss);
    if (snapshot) {
      MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done)));
      MIR_append_insn(ctx, fn, miss);
    }
    mir_emit_get_field_own_slot_fastpath(
        ctx, fn, ic, bc_off, ic_idx, obj, dst, slow, r_global_epoch);
    if (snapshot) MIR_append_insn(ctx, fn, done);
    return true;
  }
  if (sv_gf_ic_active(ic->cached_aux) && ic->get_kind == SV_GF_IC_MISSING && ic->cached_shape &&
      (is_object_type(ic->guard.receiver_proto) || vtype(ic->guard.receiver_proto) == kTypeNull))
    return mir_emit_get_field_missing_fastpath(
        ctx, fn, ic, bc_off, ic_idx, obj, dst, slow, r_global_epoch);
  MIR_reg_t r_ic = mir_new_ic_reg(ctx, fn, "gf", "ic", bc_off, ic_idx);
  MIR_reg_t r_ic_epoch = mir_new_ic_reg(ctx, fn, "gf", "ice", bc_off, ic_idx);
  MIR_reg_t r_obj_tag = mir_new_ic_reg(ctx, fn, "gf", "ot", bc_off, ic_idx);
  MIR_reg_t r_obj_ptr = mir_new_ic_reg(ctx, fn, "gf", "op", bc_off, ic_idx);
  MIR_reg_t r_obj_shape = mir_new_ic_reg(ctx, fn, "gf", "os", bc_off, ic_idx);
  MIR_reg_t r_ic_shape = mir_new_ic_reg(ctx, fn, "gf", "ics", bc_off, ic_idx);
  MIR_reg_t r_holder = mir_new_ic_reg(ctx, fn, "gf", "h", bc_off, ic_idx);
  MIR_reg_t r_holder_shape = mir_new_ic_reg(ctx, fn, "gf", "hs", bc_off, ic_idx);
  MIR_reg_t r_ic_idx_val = mir_new_ic_reg(ctx, fn, "gf", "idx", bc_off, ic_idx);
  MIR_reg_t r_holder_prop_count = mir_new_ic_reg(ctx, fn, "gf", "pc", bc_off, ic_idx);
  MIR_reg_t r_ic_aux = mir_new_ic_reg(ctx, fn, "gf", "ica", bc_off, ic_idx);
  MIR_reg_t r_inobj_limit = mir_new_ic_reg(ctx, fn, "gf", "il", bc_off, ic_idx);
  MIR_reg_t r_overflow = mir_new_ic_reg(ctx, fn, "gf", "ovf", bc_off, ic_idx);
  MIR_reg_t r_overflow_idx = mir_new_ic_reg(ctx, fn, "gf", "ovi", bc_off, ic_idx);
  MIR_reg_t r_kind = mir_new_ic_reg(ctx, fn, "gf", "kind", bc_off, ic_idx);
  MIR_reg_t r_source = mir_new_ic_reg(ctx, fn, "gf", "src", bc_off, ic_idx);
  MIR_reg_t r_obj_proto = mir_new_ic_reg(ctx, fn, "gf", "opp", bc_off, ic_idx);
  MIR_reg_t r_ic_proto = mir_new_ic_reg(ctx, fn, "gf", "icp", bc_off, ic_idx);
  MIR_reg_t r_proto_ptr = mir_new_ic_reg(ctx, fn, "gf", "pp", bc_off, ic_idx);
  MIR_reg_t r_proto_id = mir_new_ic_reg(ctx, fn, "gf", "pid", bc_off, ic_idx);
  MIR_reg_t r_ic_proto_id = mir_new_ic_reg(ctx, fn, "gf", "ipid", bc_off, ic_idx);

  MIR_label_t load_overflow = MIR_new_label(ctx);
  MIR_label_t fast_done = MIR_new_label(ctx);
  MIR_label_t own_path = MIR_new_label(ctx);
  sv_jit_ctx_t *jc = js ? js->jit_ctx : NULL;
  bool learn_missing = jc && ctx == jc->ctx_hot &&
      (!ic->cached_shape || !sv_gf_ic_active(ic->cached_aux));
  MIR_label_t missing_path = learn_missing ? MIR_new_label(ctx) : slow;
  MIR_label_t missing_done = learn_missing ? MIR_new_label(ctx) : NULL;
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
    MIR_reg_t r_cur_epoch = mir_new_ic_reg(ctx, fn, "gf", "ce", bc_off, ic_idx);
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
                               MIR_new_reg_op(ctx, r_kind),
                               MIR_new_mem_op(ctx, MIR_T_U8,
                                              (MIR_disp_t)offsetof(sv_ic_entry_t, get_kind), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_BEQ,
                               MIR_new_label_op(ctx, own_path),
                               MIR_new_reg_op(ctx, r_kind),
                               MIR_new_int_op(ctx, SV_GF_IC_OWN)));
  // Preserve the positive-read branch count; dispatch other kinds out of line.
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, missing_path), MIR_new_reg_op(ctx, r_kind),
      MIR_new_int_op(ctx, SV_GF_IC_PROTOTYPE)));

  MIR_append_insn(ctx, fn,
                  MIR_new_insn(ctx, MIR_MOV,
                               MIR_new_reg_op(ctx, r_obj_proto),
                               MIR_new_mem_op(ctx, MIR_T_I64,
                                              (MIR_disp_t)offsetof(ant_object_t, proto), r_obj_ptr, 0, 1)));
  // The cache can learn a new receiver prototype without recompiling this
  // function. Follow its current guard, rather than pinning the first one.
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
      MIR_new_reg_op(ctx, r_ic_proto),
      MIR_new_mem_op(ctx, MIR_T_I64,
          offsetof(sv_ic_entry_t, guard.receiver_proto), r_ic, 0, 1)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, r_obj_proto),
      MIR_new_reg_op(ctx, r_ic_proto)));
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_URSH,
      MIR_new_reg_op(ctx, r_obj_tag), MIR_new_reg_op(ctx, r_obj_proto),
      MIR_new_uint_op(ctx, NANBOX_TYPE_SHIFT)));
  mir_emit_decode_ref(ctx, fn, r_proto_ptr, r_obj_proto);
  MIR_label_t proto_object = MIR_new_label(ctx);
  MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
      MIR_new_label_op(ctx, proto_object), MIR_new_reg_op(ctx, r_obj_tag),
      MIR_new_uint_op(ctx, NANBOX_TFUNC_TAG)));
  {
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, r_proto_ptr),
        MIR_new_mem_op(ctx, MIR_T_I64, offsetof(sv_closure_t, func_obj), r_proto_ptr, 0, 1)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BEQ,
        MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, r_proto_ptr), MIR_new_int_op(ctx, 0)));
    mir_emit_decode_ref(ctx, fn, r_proto_ptr, r_proto_ptr);
  }
  MIR_append_insn(ctx, fn, proto_object);
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

  if (learn_missing) {
    MIR_append_insn(ctx, fn, missing_path);
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
        MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, r_kind), MIR_new_int_op(ctx, SV_GF_IC_MISSING)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
        MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, r_obj_tag), MIR_new_uint_op(ctx, NANBOX_TOBJ_TAG)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BEQ,
        MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, r_obj_shape), MIR_new_int_op(ctx, 0)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, r_proto_id), MIR_new_mem_op(ctx, MIR_T_U8, offsetof(ant_object_t, type_tag), r_obj_ptr, 0, 1)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
        MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, r_proto_id), MIR_new_int_op(ctx, kTypeObject)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, r_proto_id), MIR_new_mem_op(ctx, MIR_T_U16, offsetof(ant_object_t, flags), r_obj_ptr, 0, 1)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_AND,
        MIR_new_reg_op(ctx, r_proto_id), MIR_new_reg_op(ctx, r_proto_id), MIR_new_uint_op(ctx, ANT_OBJECT_FLAG_EXOTIC)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
        MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, r_proto_id), MIR_new_int_op(ctx, 0)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, r_obj_proto), MIR_new_mem_op(ctx, MIR_JSVAL, offsetof(ant_object_t, proto), r_obj_ptr, 0, 1)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, r_ic_proto), MIR_new_mem_op(ctx, MIR_JSVAL, offsetof(sv_ic_entry_t, guard.receiver_proto), r_ic, 0, 1)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
        MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, r_obj_proto), MIR_new_reg_op(ctx, r_ic_proto)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BEQ,
        MIR_new_label_op(ctx, missing_done), MIR_new_reg_op(ctx, r_obj_proto), MIR_new_uint_op(ctx, js_mknull())));
    mir_emit_value_to_objptr_or_jmp(ctx, fn, r_obj_proto, r_proto_ptr, r_ic_proto_id, slow);
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, r_proto_id), MIR_new_mem_op(ctx, MIR_T_U32, offsetof(ant_object_t, ic_identity), r_proto_ptr, 0, 1)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_MOV,
        MIR_new_reg_op(ctx, r_ic_proto_id), MIR_new_mem_op(ctx, MIR_T_U64, offsetof(sv_ic_entry_t, cached_aux), r_ic, 0, 1)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_URSH,
        MIR_new_reg_op(ctx, r_ic_proto_id), MIR_new_reg_op(ctx, r_ic_proto_id), MIR_new_uint_op(ctx, SV_GF_IC_PROTO_ID_SHIFT)));
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_BNE,
        MIR_new_label_op(ctx, slow), MIR_new_reg_op(ctx, r_proto_id), MIR_new_reg_op(ctx, r_ic_proto_id)));
    MIR_append_insn(ctx, fn, missing_done);
    mir_load_imm(ctx, fn, dst, js_mkundef());
    MIR_append_insn(ctx, fn, MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, fast_done)));

  }

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

  MIR_reg_t r_ic = mir_new_ic_reg(ctx, fn, "gg", "ic", bc_off, -1);
  MIR_reg_t r_e = mir_new_ic_reg(ctx, fn, "gg", "e", bc_off, -1);
  MIR_reg_t r_ce = mir_new_ic_reg(ctx, fn, "gg", "ce", bc_off, -1);
  MIR_reg_t r_gv = mir_new_ic_reg(ctx, fn, "gg", "gv", bc_off, -1);
  MIR_reg_t r_gt = mir_new_ic_reg(ctx, fn, "gg", "gt", bc_off, -1);
  MIR_reg_t r_gp = mir_new_ic_reg(ctx, fn, "gg", "gp", bc_off, -1);
  MIR_reg_t r_sh = mir_new_ic_reg(ctx, fn, "gg", "sh", bc_off, -1);
  MIR_reg_t r_ics = mir_new_ic_reg(ctx, fn, "gg", "ics", bc_off, -1);
  MIR_reg_t r_idx = mir_new_ic_reg(ctx, fn, "gg", "idx", bc_off, -1);
  MIR_reg_t r_pc = mir_new_ic_reg(ctx, fn, "gg", "pc", bc_off, -1);
  MIR_reg_t r_il = mir_new_ic_reg(ctx, fn, "gg", "il", bc_off, -1);
  MIR_reg_t r_ov = mir_new_ic_reg(ctx, fn, "gg", "ov", bc_off, -1);
  MIR_reg_t r_oi = mir_new_ic_reg(ctx, fn, "gg", "oi", bc_off, -1);

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
