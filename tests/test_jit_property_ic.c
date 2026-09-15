// meson test -C build jit-property-ic
#include "../src/jit/jit_internal.h"
#include "gc/roots.h"
#include <assert.h>

// Inject registration failure only into this translation unit's IC helpers.
static int registrations_until_failure = -1;
static int registration_attempts;
static bool test_shape_ref_register(ant_t *js, ant_shape_t **slot) {
  registration_attempts++;
  // Invalid context and allocation failure share the registry's fail path.
  if (registrations_until_failure == 0) return sv_ic_shape_ref_register(NULL, slot);
  if (registrations_until_failure > 0) registrations_until_failure--;
  return sv_ic_shape_ref_register(js, slot);
}
#define sv_ic_shape_ref_register test_shape_ref_register
#include "../src/silver/ops/property.h"
#undef sv_ic_shape_ref_register

static void check_field_codegen(
    ant_shape_t *shape, ant_value_t proto, sv_get_field_ic_kind_t kind, bool specialized) {
  sv_ic_entry_t ic = {
    .cached_shape = shape,
    .cached_aux = SV_GF_IC_AUX_ACTIVE_BIT,
    .get_kind = kind,
    .guard.receiver_proto = proto,
  };
  sv_func_t callee = {.ic_slots = &ic, .ic_count = 1};
  sv_atom_t atom = {.str = "absent", .len = 6};
  MIR_context_t ctx = MIR_init();
  MIR_new_module(ctx, "property_ic");
  MIR_type_t ret_type = MIR_JSVAL;
  MIR_item_t fn = MIR_new_func(ctx, "missing", 1, &ret_type, 0);
  MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, "cage_base");
  MIR_reg_t obj = MIR_new_func_reg(ctx, fn->u.func, MIR_JSVAL, "obj");
  MIR_reg_t dst = MIR_new_func_reg(ctx, fn->u.func, MIR_JSVAL, "dst");
  MIR_reg_t epoch = MIR_new_func_reg(ctx, fn->u.func, MIR_T_I64, "epoch");
  MIR_label_t slow = MIR_new_label(ctx);
  assert(mir_emit_get_field_ic_fastpath(ctx, fn, NULL, &callee, 0, 0, &atom, obj, dst, slow, epoch));
  MIR_append_insn(ctx, fn, slow);
  MIR_append_insn(ctx, fn, MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, dst)));
  MIR_finish_func(ctx);
  MIR_finish_module(ctx);

  int guards = 0, prototype_loads = 0;
  for (MIR_insn_t insn = DLIST_HEAD(MIR_insn_t, fn->u.func->insns); insn;
       insn = DLIST_NEXT(MIR_insn_t, insn)) {
    if (insn->code == MIR_MOV && insn->ops[0].mode == MIR_OP_REG &&
        insn->ops[1].mode == MIR_OP_MEM) {
      const char *name = MIR_reg_name(ctx, insn->ops[0].u.reg, fn->u.func);
      if (strncmp(name, "gf_icp_", sizeof("gf_icp_") - 1) == 0) {
        assert(insn->ops[1].u.mem.disp == offsetof(sv_ic_entry_t, guard.receiver_proto));
        prototype_loads++;
      }
    }
    if (insn->code != MIR_BNE || insn->ops[1].mode != MIR_OP_REG) continue;
    const char *name = MIR_reg_name(ctx, insn->ops[1].u.reg, fn->u.func);
    assert(strncmp(name, "gf_own_", sizeof("gf_own_") - 1) != 0);
    if (strncmp(name, "gf_miss_kind_", sizeof("gf_miss_kind_") - 1) != 0) continue;
    assert(insn->ops[2].mode == MIR_OP_INT || insn->ops[2].mode == MIR_OP_UINT);
    uint64_t emitted_kind = insn->ops[2].mode == MIR_OP_INT ? (uint64_t)insn->ops[2].u.i : insn->ops[2].u.u;
    assert(emitted_kind == SV_GF_IC_MISSING);
    guards++;
  }
  assert(guards == (specialized ? 1 : 0));
  if (kind == SV_GF_IC_PROTOTYPE) assert(prototype_loads == 1);
  MIR_finish(ctx);
}

static void check_failed_fills(ant_t *js, ant_value_t object, ant_shape_t *old_shape) {
  registrations_until_failure = 0;
  const char *keys[] = {"target", "absent"};
  for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
    sv_atom_t atom = {.str = intern_string(keys[i], strlen(keys[i])), .len = (uint32_t)strlen(keys[i])};
    sv_ic_entry_t ic = {.cached_shape = old_shape, .cached_index = 123, .get_kind = SV_GF_IC_PROTOTYPE};
    sv_func_t callee = {.ic_slots = &ic, .ic_count = 1};
    uint8_t code[] = {OP_GET_FIELD, 0, 0, 0, 0, 0, 0};
    ant_value_t out = js_mkundef();
    int attempts = registration_attempts;
    assert(sv_try_prop_get_field_ic_no_effect(js, object, &atom, &callee, code, &out));
    assert(registration_attempts == attempts + 1);
    assert(ic.cached_shape == NULL && !(ic.shape_ref_mask & SV_IC_SHAPE_REF_CACHED));
    ant_value_t ignored;
    assert(!sv_ic_try_get_hit(&ic, object, js_obj_ptr(object), &atom, &ignored));
    if (i == 0) assert(js_getnum(out) == 41);
    else assert(vtype(out) == kTypeUndefined);
  }

  sv_atom_t atom = {.str = intern_string("target", 6), .len = 6};
  sv_ic_entry_t ic = {.cached_shape = old_shape, .cached_index = 123};
  int attempts = registration_attempts;
  assert(js_getnum(sv_put_field_cached(js, object, js_mknum(42), &atom, &ic)) == 42);
  assert(js_getnum(js_get(js, object, "target")) == 42);
  assert(registration_attempts == attempts + 1);
  assert(ic.cached_shape == NULL && !(ic.shape_ref_mask & SV_IC_SHAPE_REF_CACHED));

  // A first fill stays unusable even after the index and epoch are written.
  memset(&ic, 0, sizeof(ic));
  assert(js_getnum(sv_put_field_cached(js, object, js_mknum(43), &atom, &ic)) == 43);
  assert(ic.cached_shape == NULL && !(ic.shape_ref_mask & SV_IC_SHAPE_REF_CACHED));
}

static void check_partial_transition(ant_t *js, ant_shape_t *from, ant_shape_t *to) {
  sv_ic_entry_t ic = {0};
  ic.guard.add.epoch = ant_ic_epoch_counter;
  registrations_until_failure = 1;
  int attempts = registration_attempts;
  sv_ic_set_add_transition(js, &ic, from, to, 1, ant_ic_epoch_counter);
  assert(registration_attempts == attempts + 2);
  assert(ic.guard.add.epoch == ant_ic_epoch_counter);
  assert(ic.guard.add.from_shape == from && ic.guard.add.to_shape == NULL);
  assert(ic.shape_ref_mask == SV_IC_SHAPE_REF_ADD_FROM);
  registrations_until_failure = -1;
  sv_ic_set_add_transition(js, &ic, from, to, 1, ant_ic_epoch_counter);
  assert(ic.guard.add.epoch == ant_ic_epoch_counter && ic.guard.add.slot == 1);
  assert(ic.guard.add.to_shape == to);
  // Registered slots must remain alive until the shape-root list is cleared.
  sv_ic_shape_refs_cleanup(js);
  assert(ic.guard.add.from_shape == NULL && ic.guard.add.to_shape == NULL);
}

static void check_shape_snapshot_invalidation(void) {
  const char *key = intern_string("snapshot", 8);
  const char *extra = intern_string("extra", 5);
  for (unsigned operation = 0; operation < 4; operation++) {
    ant_shape_t *root = ant_shape_new();
    ant_shape_t *shape = ant_shape_clone(root);
    ant_shape_release(root);
    uint32_t slot;
    assert(shape && ant_shape_add_interned(shape, key, ANT_PROP_ATTR_DEFAULT, &slot));
    const uint32_t *guard = ant_shape_jit_guard(shape);
    assert(guard && !*guard);
    // Appending preserves existing slot positions and storage capacity.
    assert(ant_shape_add_interned(shape, extra, ANT_PROP_ATTR_DEFAULT, &slot));
    assert(!*guard);
    if (operation == 0) assert(ant_shape_set_attrs_interned(shape, key, 0));
    if (operation == 1) assert(ant_shape_prop_mut_at(shape, 0));
    if (operation == 2) assert(ant_shape_remove_slot(shape, 0));
    if (operation == 3) assert(ant_shape_clear_accessor_slot(shape, 0));
    assert(*guard);
    // A fresh clone owns new metadata and may acquire its own snapshot.
    ant_shape_t *copy = ant_shape_clone(shape);
    assert(copy && !*ant_shape_jit_guard(copy));
    ant_shape_release(copy);
    ant_shape_release(shape);
  }
}

int main(void) {
  char stack_base;
  ant_t *js = ant_create();
  assert(js);
  js_setstackbase(js, &stack_base);
  check_shape_snapshot_invalidation();
  GC_ROOT_SAVE(roots, js);
  ant_value_t old = js_mkobj(js);
  GC_ROOT_PIN(js, old);
  ant_value_t object = js_mkobj(js);
  GC_ROOT_PIN(js, object);
  js_set_proto(js, object, js_mknull());
  js_set(js, old, "other", js_mknum(1));
  js_set(js, object, "padding", js_mknum(0));
  js_set(js, object, "target", js_mknum(41));
  ant_shape_t *shape = js_obj_ptr(object)->shape;
  ant_shape_t *old_shape = js_obj_ptr(old)->shape;
  assert(shape && old_shape && shape != old_shape);

  check_field_codegen(shape, old, SV_GF_IC_MISSING, true);
  check_field_codegen(shape, js_mknull(), SV_GF_IC_MISSING, true);
  check_field_codegen(shape, js_mknum(1), SV_GF_IC_MISSING, false);
  check_field_codegen(shape, js_mkundef(), SV_GF_IC_MISSING, false);
  check_field_codegen(NULL, js_mknull(), SV_GF_IC_MISSING, false);
  check_field_codegen(NULL, js_mknull(), SV_GF_IC_OWN, false);
  check_field_codegen(shape, old, SV_GF_IC_PROTOTYPE, false);
  puts("PASS missing handler requires an object/null prototype and guards the C enum value");
  check_failed_fills(js, object, old_shape);
  check_partial_transition(js, old_shape, shape);
  puts("PASS failed shape registration preserves reads/writes without publishing mismatched IC metadata");
  GC_ROOT_RESTORE(js, roots);
  js_destroy(js);
}
