#include "internal.h"
#include "errors.h"
#include "gc/roots.h"
#include "modules/proxy.h"
#include "silver/call.h"
#include "silver/glue.h"

#include "ops/property.h"
#include "ops/coercion.h"

static __attribute__((noinline)) ant_value_t Super_ConstructorHome(ant_t *js, ant_value_t constructor) {
  sv_closure_t *closure = js_func_closure(constructor);
  ant_value_t home = Ant_Value_Undefined;
  
  bool cached = closure->func_obj && sv_construct_prototype_data(js, js_obj_ptr(closure->func_obj), closure->func, &home);
  return cached ? home : lkp_interned_val(js, constructor, js->intern.prototype);
}

static inline ant_value_t Super_Home(ant_t *js, sv_func_t *func, ant_value_t home) {
  bool constructor = vtype(home) == kTypeFunction && func && !func->is_static;
  return __builtin_expect(constructor, 0) ? Super_ConstructorHome(js, home) : home;
}

static ant_value_t Super_Base(ant_t *js, sv_func_t *func, ant_value_t home) {
  return js_get_proto(js, Super_Home(js, func, home));
}

ant_value_t Silver_Super_Get(
  ant_t *js, sv_func_t *func, ant_value_t receiver,
  ant_value_t home, ant_value_t prop, ant_value_t *roots
) {
  ant_value_t base = roots[0] = Super_Base(js, func, home);
  if (is_err(base)) return base;
  if (vtype(prop) == kTypeSymbol) {
    if (!is_object_type(base)) return sv_mk_nullish_read_error_by_key(js, NULL, base, prop);
    return js_get_sym_with_receiver(js, base, prop, receiver);
  }
  
  ant_value_t data;
  if (sv_get_elem_string(js, base, prop, &data)) return data;
  
  ant_value_t key = roots[1] = coerce_to_str(js, prop);
  if (is_err(key)) return key;
  if (!is_object_type(base)) return sv_mk_nullish_read_error_by_key(js, NULL, base, key);
  
  ant_offset_t klen;
  ant_offset_t koff = vstr(js, key, &klen);
  return js_getprop_super(js, base, receiver, (const char *)(uintptr_t)koff);
}

ant_value_t Silver_Super_Put(
  ant_t *js, sv_func_t *func, ant_value_t receiver, ant_value_t home,
  ant_value_t prop, ant_value_t val, ant_value_t *roots
) {
  ant_value_t base = roots[0] = Super_Base(js, func, home);
  if (is_err(base)) return base;
  if (!is_object_type(base)) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot set properties of %s", vtype(base) == kTypeNull ? "null" : "undefined");
  
  ant_value_t key = roots[1] = vtype(prop) == kTypeSymbol ? prop : coerce_to_str(js, prop);
  if (is_err(key)) return key;
  
  ant_value_t stored = js_set_with_receiver(js, base, key, val, receiver);
  
  if (is_err(stored)) return stored;
  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
  if (stored == Ant_Value_False && func && func->is_strict) return Ant_Error_CreateReadOnlyProperty(js, receiver, key);
  
  return val;
}

static ant_value_t Super_FieldBase(ant_t *js, ant_value_t home) {
  if (vtype(home) == kTypeObject) return ant_object_proto((ant_object_t *)vptr(home));
  return js_get_proto(js, home);
}

ant_value_t Silver_Super_GetField(ant_t *js, sv_func_t *func, ant_value_t receiver, ant_value_t home, sv_atom_t *atom, sv_ic_entry_t *ic) {
  home = Super_Home(js, func, home);
  ant_value_t base = Super_FieldBase(js, home);
  if (is_err(base)) return base;
  if (__builtin_expect(!is_object_type(base), 0)) return js_mkerr_typed(
    js, JS_ERR_TYPE, "Cannot read properties of %s (reading '%.*s')",
    vtype(base) == kTypeNull ? "null" : "undefined", (int)atom->len, atom->str
  );
  
  ant_value_t cached = Ant_Value_Undefined, getter = Ant_Value_Undefined;
  bool exotic_base = vtype(base) == kTypeObject && ((const ant_object_t *)vptr(base))->flags.is_exotic;
  if (ic && !exotic_base && sv_try_prop_get_ic_lookup(js, base, atom, ic, true, &cached, &getter))
    return vtype(getter) == kTypeUndefined ? cached : js_call_getter(js, receiver, getter);
  
  return js_getprop_super(js, base, receiver, atom->str);
}

static constexpr uint16_t SUPER_PUT_FILL_LIMIT = 16;

static inline ant_object_t *Super_ObjectOf(ant_value_t value) {
  if (vtype(value) == kTypeObject) return (ant_object_t *)vptr(value);
  if (vtype(value) != kTypeFunction) return NULL;
  ant_value_t object = js_func_closure(value)->func_obj;
  return vtype(object) == kTypeObject ? (ant_object_t *)vptr(object) : NULL;
}

static inline ant_value_t Super_CacheableParent(ant_value_t home) {
  const ant_object_t *owner = Super_ObjectOf(home);
  return owner ? ant_object_proto(owner) : Ant_Value_Undefined;
}

static void Super_DisablePut(ant_t *js, sv_ic_entry_t *ic) {
  sv_pf_poly_free(js, ic);
  ic->super_put_kind = SV_SUPER_PUT_IC_DISABLED;
  ic->epoch = js->ic.epoch;
}

static void Super_BindPut(
  ant_t *js, sv_ic_entry_t *ic, sv_super_put_ic_kind_t kind,
  ant_shape_t *guarded, ant_shape_t *base_shape, uint32_t slot, uint32_t holder_depth
) {
  sv_ic_set_cached_shape(js, ic, guarded);
  sv_ic_set_shape_ref(js, ic, &ic->guard.super_put.base_shape, SV_IC_SHAPE_REF_SUPER_BASE, base_shape);
  ic->cached_index = slot;
  ic->super_put_holder_depth = holder_depth;
  ic->super_put_kind = kind;
  ic->epoch = js->ic.epoch;
}

static bool Super_SetterPathIsPlain(const ant_object_t *base, uint32_t depth) {
  const ant_object_t *cur = base;
  for (uint32_t hop = 0; cur && hop < depth; hop++) cur = Super_ObjectOf(ant_object_proto(cur));
  return cur && cur->shape && !ant_shape_is_dictionary(cur->shape);
}

static bool Super_TryBindPut(
  ant_t *js, sv_ic_entry_t *ic, ant_value_t receiver, ant_value_t home,
  const sv_atom_t *atom, bool exhausted
) {
  ant_value_t parent = Super_CacheableParent(home);
  ant_object_t *base = Super_ObjectOf(parent);
  if (!base || !base->shape || ant_shape_is_dictionary(base->shape)) return false;
  
  sv_chain_key_t found = sv_proto_chain_find_key(parent, atom->str);
  if (found.state == SV_CHAIN_KEY_UNKNOWN) return false;
  
  if (found.state == SV_CHAIN_KEY_HELD) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(found.holder->shape, found.slot);
    if (prop && prop->has_setter && is_callable(prop->setter) && Super_SetterPathIsPlain(base, found.depth)) {
      sv_pf_poly_free(js, ic);
      Super_BindPut(js, ic, SV_SUPER_PUT_IC_SETTER, found.holder->shape, base->shape, found.slot, found.depth);
      return true;
    }
    if (found.depth != 0 || !sv_shape_prop_is_writable_data(prop, atom->str)) return false;
  }
  
  ant_object_t *own = Super_ObjectOf(receiver);
  if (!own) return false;
  if (own->flags.is_exotic || !own->shape || ant_shape_is_dictionary(own->shape)) return false;
  
  int32_t slot = ant_shape_lookup_interned(own->shape, atom->str);
  if (slot < 0 || (uint32_t)slot >= own->prop_count) return false;
  if (!sv_shape_prop_is_writable_data(ant_shape_prop_at(own->shape, (uint32_t)slot), atom->str)) return false;
  
  bool stores_data = ic->super_put_kind == SV_SUPER_PUT_IC_DATA || ic->super_put_kind == SV_SUPER_PUT_IC_DATA_ANY_SHAPE;
  bool same_base = stores_data && ic->guard.super_put.base_shape == base->shape;
  if (exhausted && (!same_base || ic->super_put_kind == SV_SUPER_PUT_IC_DATA_ANY_SHAPE)) return false;
  
  if (!same_base) {
    sv_pf_poly_free(js, ic);
    ic->shape_ref_mask &= (sv_ic_flags_t)~SV_IC_POLY_MEGA;
  } else if (ic->cached_shape && ic->cached_shape != own->shape)
    sv_pf_poly_keep(js, ic, ic->cached_shape, NULL, ic->cached_index, ic->epoch, 0, false);
  
  bool megamorphic = same_base && (exhausted || (ic->shape_ref_mask & SV_IC_POLY_MEGA)) && own->type_tag == kTypeObject;
  if (megamorphic) sv_pf_poly_free(js, ic);
  Super_BindPut(js, ic, megamorphic ? SV_SUPER_PUT_IC_DATA_ANY_SHAPE : SV_SUPER_PUT_IC_DATA, own->shape, base->shape, (uint32_t)slot, 0);
  
  return true;
}

static __attribute__((noinline)) void Super_FillPut(
  ant_t *js, sv_ic_entry_t *ic, ant_value_t receiver, ant_value_t home, const sv_atom_t *atom
) {
  if (ic->epoch != js->ic.epoch) {
    ic->shape_ref_mask &= (sv_ic_flags_t)~SV_IC_POLY_MEGA;
    ic->super_put_kind = SV_SUPER_PUT_IC_EMPTY;
    ic->slow_hits = 0;
    sv_pf_poly_free(js, ic);
  }
  
  bool exhausted = ic->slow_hits >= SUPER_PUT_FILL_LIMIT;
  if (!exhausted) ic->slow_hits++;
  if (!Super_TryBindPut(js, ic, receiver, home, atom, exhausted) && exhausted) Super_DisablePut(js, ic);
}

static __attribute__((noinline)) int Super_StorePoly(
  ant_t *js, sv_ic_entry_t *ic, ant_object_t *own, const sv_atom_t *atom, ant_value_t val
) {
  return sv_pf_poly_try_store(js, ic, own, atom, val);
}

static inline __attribute__((always_inline)) int Super_StoreCached(
  ant_t *js, sv_ic_entry_t *ic, ant_object_t *own, const sv_atom_t *atom, ant_value_t val
) {
  if (own->flags.is_exotic || !own->shape) return 0;
  uint32_t slot = ic->cached_index;
  
  if (ic->cached_shape != own->shape) {
    if (ic->super_put_kind != SV_SUPER_PUT_IC_DATA_ANY_SHAPE)
      return (ic->shape_ref_mask & SV_IC_HAS_PUT_POLY) ? Super_StorePoly(js, ic, own, atom, val) : 0;
    
    int32_t found = own->type_tag == kTypeObject ? ant_shape_lookup_interned(own->shape, atom->str) : -1;
    if (found < 0) return 0;
    slot = (uint32_t)found;
  }
  
  if (slot >= own->prop_count) return 0;
  if (!sv_shape_prop_is_writable_data(ant_shape_prop_at(own->shape, slot), atom->str)) return 0;
  
  ant_object_prop_set_unchecked(own, slot, val);
  gc_write_barrier_prop(js, own, val);
  ant_property_mutation_invalidate(js, own, atom->str);
  
  return 1;
}

static inline __attribute__((always_inline)) const ant_shape_prop_t *Super_CachedSetter(
  const sv_ic_entry_t *ic, const ant_object_t *base, bool functions
) {
  const ant_object_t *holder = base;
  for (uintptr_t hop = ic->super_put_holder_depth; holder && hop; hop--) {
    ant_value_t parent = ant_object_proto(holder);
    if (vtype(parent) == kTypeObject) holder = (const ant_object_t *)vptr(parent);
    else holder = functions ? Super_ObjectOf(parent) : NULL;
  }
  return holder && holder->shape == ic->cached_shape ? ant_shape_prop_at(holder->shape, ic->cached_index) : NULL;
}

static __attribute__((noinline)) ant_value_t Super_PutFieldSlow(
  ant_t *js, sv_func_t *func, ant_value_t receiver, ant_value_t home,
  const sv_atom_t *atom, ant_value_t val, sv_ic_entry_t *fillable
) {
  ant_value_t base = Super_FieldBase(js, home);
  if (is_err(base)) return base;
  if (!is_object_type(base)) return js_mkerr_typed(
    js, JS_ERR_TYPE, "Cannot set properties of %s",
    vtype(base) == kTypeNull ? "null" : "undefined"
  );
  
  ant_value_t stored = js_set_with_receiver_interned(js, base, atom->str, atom->len, val, receiver);
  ant_value_t result = val;
  
  if (is_err(stored)) result = stored;
  else if (Ant_Exception_Pending(js)) result = Ant_Exception_Current(js);
  else if (stored == Ant_Value_False && func && func->is_strict) result = Ant_Error_CreateReadOnlyProperty(js, receiver, js_mkstr(js, atom->str, atom->len));
  else if (fillable && stored == Ant_Value_True) Super_FillPut(js, fillable, receiver, home, atom);
  
  return result;
}

static inline __attribute__((always_inline)) ant_value_t Super_PutFieldCachedOn(
  ant_t *js, sv_func_t *func, ant_value_t receiver, ant_value_t home, const sv_atom_t *atom,
  ant_value_t val, sv_ic_entry_t *ic, const ant_object_t *base, ant_object_t *own, bool functions
) {
  if (base->shape != ic->guard.super_put.base_shape) return Super_PutFieldSlow(js, func, receiver, home, atom, val, ic);
  
  if (ic->super_put_kind == SV_SUPER_PUT_IC_SETTER) {
    const ant_shape_prop_t *prop = Super_CachedSetter(ic, base, functions);
    if (!prop) return Super_PutFieldSlow(js, func, receiver, home, atom, val, ic);
    
    ant_value_t called = sv_vm_call(js->vm, js, prop->setter, receiver, &val, 1, NULL, Ant_Value_Undefined);
    if (is_err(called)) return called;
    return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : val;
  }
  
  if (!functions || own) {
    int stored = Super_StoreCached(js, ic, own, atom, val);
    if (stored) return stored < 0 ? js_mkerr(js, "oom") : val;
  }
  return Super_PutFieldSlow(js, func, receiver, home, atom, val, ic);
}

static __attribute__((noinline)) ant_value_t Super_PutFieldCachedAny(
  ant_t *js, sv_func_t *func, ant_value_t receiver, ant_value_t home,
  const sv_atom_t *atom, ant_value_t val, sv_ic_entry_t *ic
) {
  const ant_object_t *base = Super_ObjectOf(Super_CacheableParent(home));
  if (!base) return Super_PutFieldSlow(js, func, receiver, home, atom, val, ic);
  return Super_PutFieldCachedOn(js, func, receiver, home, atom, val, ic, base, Super_ObjectOf(receiver), true);
}

static __attribute__((noinline)) ant_value_t Super_PutFieldCachedObjectHome(
  ant_t *js, sv_func_t *func, ant_value_t receiver, ant_value_t home,
  const sv_atom_t *atom, ant_value_t val, sv_ic_entry_t *ic
) {
  ant_value_t parent = ant_object_proto((const ant_object_t *)vptr(home));
  if (vtype(parent) != kTypeObject || vtype(receiver) != kTypeObject) return Super_PutFieldCachedAny(js, func, receiver, home, atom, val, ic);
  
  return Super_PutFieldCachedOn(
    js, func, receiver, home, atom, val, ic,
    (const ant_object_t *)vptr(parent), (ant_object_t *)vptr(receiver), false
  );
}

ant_value_t Silver_Super_PutField(
  ant_t *js, sv_func_t *func, ant_value_t receiver,
  ant_value_t home, const sv_atom_t *atom, ant_value_t val, sv_ic_entry_t *ic
) {
  home = Super_Home(js, func, home);
  if (!ic) return Super_PutFieldSlow(js, func, receiver, home, atom, val, NULL);
  if (ic->epoch != js->ic.epoch || ic->super_put_kind == SV_SUPER_PUT_IC_EMPTY) return Super_PutFieldSlow(js, func, receiver, home, atom, val, ic);
  if (ic->super_put_kind == SV_SUPER_PUT_IC_DISABLED) return Super_PutFieldSlow(js, func, receiver, home, atom, val, NULL);
  if (vtype(home) != kTypeObject) return Super_PutFieldCachedAny(js, func, receiver, home, atom, val, ic);
  return Super_PutFieldCachedObjectHome(js, func, receiver, home, atom, val, ic);
}
