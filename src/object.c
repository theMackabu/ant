#include <compat.h> // IWYU pragma: keep
#include <stdlib.h>
#include <string.h>

#include "cfunc.h"
#include "object.h"
#include "proto.h"
#include "protectors.h"
#include "gc/object_arenas.h"
#include "silver/engine.h"
#include "modules/proxy.h"

ant_object_t *js_obj_ptr(ant_value_t v) {
  if (!is_object_type(v)) return NULL;
  ant_value_t as_obj = js_as_obj(v);
  return (ant_object_t *)vptr(as_obj);
}

ant_value_t js_obj_from_ptr(ant_object_t *obj) {
  if (!obj) return Ant_Value_Undefined;
  return mkref(kTypeObject, obj);
}

void js_mark_constructor(ant_value_t value, bool is_constructor) {
  ant_object_t *obj = js_obj_ptr(value);
  if (obj) obj->flags.is_constructor = is_constructor ? 1u : 0u;
}

ant_value_t obj_extra_get(ant_object_t *obj, internal_slot_t slot) {
  ant_extra_slot_t *entry = ant_object_extra_slot(obj, (uint8_t)slot);
  return entry ? entry->value : Ant_Value_Undefined;
}

bool obj_extra_reserve(ant_object_t *obj, uint8_t capacity) {
  if (!obj) return false;
  ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
  ant_extra_slots_t *block = ant_object_extra_block(obj);

  if (block && capacity <= block->cap) return true;
  ant_extra_slots_t *next = realloc(block, sizeof(*next) + sizeof(next->entries[0]) * capacity);
  if (!next) return false;

  if (!block) next->count = 0;
  next->cap = capacity;
  if (sidecar) sidecar->extra_slots = next;
  else obj->extra_slots = next;
  
  return true;
}

static bool obj_extra_set(ant_object_t *obj, internal_slot_t slot, ant_value_t value) {
  if (!obj) return false;
  
  ant_extra_slot_t *entry = ant_object_extra_slot(obj, (uint8_t)slot);
  if (entry) {
    entry->value = value;
    return true;
  }

  uint8_t count = ant_object_extra_count(obj);
  if (count == UINT8_MAX) return false;
  if ((uint8_t)(count + 1) > ant_object_extra_capacity(obj) && !obj_extra_reserve(obj, (uint8_t)(count + 1))) return false;

  ant_extra_slots_t *block = ant_object_extra_block(obj);
  block->entries[count].slot = (uint8_t)slot;
  block->entries[count].value = value;
  block->count = (uint8_t)(count + 1);
  
  return true;
}

ant_value_t js_prop_load(ant_prop_loc_t loc) {
  if (!loc.obj || loc.slot >= loc.obj->prop_count) return Ant_Value_Undefined;
  return ant_object_prop_get_unchecked(loc.obj, loc.slot);
}

bool js_prop_store(ant_t *js, ant_prop_loc_t loc, ant_value_t value) {
  if (!loc.obj || loc.slot >= loc.obj->prop_count) return false;
  ant_object_prop_set_unchecked(loc.obj, loc.slot, value);
  gc_write_barrier(js, loc.obj, value);
  const ant_shape_prop_t *prop = ant_shape_prop_at(loc.obj->shape, loc.slot);
  if (prop && prop->type == ANT_SHAPE_KEY_STRING) 
    ant_property_mutation_invalidate(js, loc.obj, prop->key.interned);
  return true;
}

bool js_obj_grow_prop_capacity(ant_t *js, ant_object_t *obj, uint32_t needed) {
  if (!obj) return false;
  
  uint32_t inobj_limit = ant_object_inobj_limit(obj);
  uint32_t old_count = obj->prop_count;
  if (needed <= old_count) return true;

  if (needed > inobj_limit) {
    uint32_t overflow_needed = needed - inobj_limit;
    size_t old_slots = obj->overflow_prop && old_count > inobj_limit ? ant_object_overflow_cap_for(old_count - inobj_limit) : 0;
    if (overflow_needed > old_slots) {
      if (overflow_needed > (1u << 31)) return false;
      size_t slots = ant_object_overflow_cap_for(overflow_needed);
      if (slots > SIZE_MAX / sizeof(ant_value_t)) return false;

      ant_value_t *next = realloc(obj->overflow_prop, sizeof(*next) * slots);
      if (!next) return false;
      
      gc_note_overflow_alloc(js, sizeof(*next) * (slots - old_slots));
      obj->overflow_prop = next;
    }
  } else if (obj->overflow_prop) {
    gc_note_overflow_free(js, ant_object_overflow_cap(obj) * sizeof(*obj->overflow_prop));
    ant_object_overflow_free(obj);
  }

  obj->prop_count = needed;
  ant_proto_watch_note_change(obj);
  
  for (uint32_t i = old_count; i < needed; i++) {
    if (i < inobj_limit) obj->inobj[i] = Ant_Value_Undefined;
    else obj->overflow_prop[i - inobj_limit] = Ant_Value_Undefined;
  }

  return true;
}

static ant_shape_t *object_proto_root(ant_t *js, ant_value_t proto, uint8_t inobj_limit) {
  if (vtype(proto) == kTypeUndefined) return ant_shape_new_with_inobj_limit(js, inobj_limit);
  if (!is_object_type(proto)) return ant_shape_root(js, &js->shapes.null_roots, proto, inobj_limit);
  
  ant_object_sidecar_t *sidecar = ant_object_ensure_sidecar(js_obj_ptr(js_as_obj(proto)));
  if (!sidecar) return NULL;
  if (!sidecar->shape_roots && !(sidecar->shape_roots = calloc(1, sizeof(*sidecar->shape_roots)))) return NULL;
  
  return ant_shape_root(js, sidecar->shape_roots, proto, inobj_limit);
}

static ant_shape_t *object_proto_root_cached(ant_t *js, ant_value_t proto) {
  uint32_t line = (uint32_t)(proto >> 6) & 7;
  if (js->shapes.cached_protos[line] == proto) return js->shapes.cached_roots[line];
  
  ant_object_sidecar_t *sidecar = ant_object_sidecar(js_obj_ptr(js_as_obj(proto)));
  ant_shape_t *root = sidecar && sidecar->shape_roots ? sidecar->shape_roots->roots[ANT_INOBJ_BASE_SLOTS] : NULL;
  if (!root && (root = object_proto_root(js, proto, (uint8_t)ANT_INOBJ_BASE_SLOTS))) ant_shape_release(root);
  
  if (root) {
    js->shapes.cached_protos[line] = proto;
    js->shapes.cached_roots[line] = root;
  }
  
  return root;
}

static ant_shape_t *object_proto_root_wide(ant_t *js, ant_value_t proto, uint8_t inobj_limit) {
  ant_object_sidecar_t *sidecar = ant_object_sidecar(js_obj_ptr(js_as_obj(proto)));
  ant_shape_t *root = sidecar && sidecar->shape_roots ? sidecar->shape_roots->roots[inobj_limit] : NULL;
  if (!root && (root = object_proto_root(js, proto, inobj_limit))) ant_shape_release(root);
  return root;
}

ant_object_t *obj_alloc_with_proto(ant_t *js, uint8_t type_tag, ant_value_t proto) {
  if (!is_object_type(proto)) return obj_alloc(js, type_tag, (uint8_t)ANT_INOBJ_BASE_SLOTS);
  ant_shape_t *root = object_proto_root_cached(js, proto);
  if (!root) return NULL;
  
  ant_shape_retain(root);
  return obj_alloc_shaped(js, type_tag, root);
}

ant_shape_t *ant_object_proto_root(ant_t *js, ant_value_t proto, uint8_t inobj_limit) {
  return object_proto_root(js, proto, inobj_limit);
}

void ant_object_set_proto(ant_t *js, ant_object_t *obj, ant_value_t proto) {
  ant_shape_t *shape = obj->shape;
  
  if (!shape || ant_shape_proto(shape) == proto) return;
  ant_shape_t *next = NULL;
  
  if (ant_shape_is_empty(shape) && !ant_shape_is_exotic(shape)) {
    uint8_t limit = ant_shape_get_inobj_limit(shape);
    if (limit == ANT_INOBJ_BASE_SLOTS && is_object_type(proto)) next = object_proto_root_cached(js, proto);
    else if ((next = object_proto_root(js, proto, limit))) ant_shape_release(next);
    if (next) ant_shape_retain(next);
  }
  
  if (!next) next = ant_shape_with_proto(shape, proto);
  Ant_Assert(next, "out of memory changing a prototype");
  
  ant_shape_release(shape);
  obj->shape = next;
}

void js_obj_set_exotic(ant_object_t *obj, const ant_exotic_ops_t *ops) {
  obj->flags.is_exotic = 1;
  ant_shape_t *shape = obj->shape;
  if (!shape || (ant_shape_is_exotic(shape) && ant_shape_exotic_ops(shape) == ops)) return;

  ant_shape_t *next = ant_shape_with_exotic(shape, ops);
  Ant_Assert(next, "out of memory marking an exotic object");

  ant_shape_release(shape);
  obj->shape = next;
}

void js_obj_mark_exotic(ant_object_t *obj) {
  js_obj_set_exotic(obj, ant_object_exotic_ops(obj));
}

bool js_obj_ensure_unique_shape(ant_object_t *obj) {
  if (!obj || !obj->shape) return false;
  if (!ant_shape_is_shared(obj->shape)) return true;

  ant_shape_t *copy = ant_shape_clone(obj->shape);
  if (!copy) return false;

  ant_shape_release(obj->shape);
  obj->shape = copy;
  return true;
}

static void obj_overflow_fit(ant_object_t *obj, uint32_t old_count) {
  if (!obj->overflow_prop || obj->prop_count >= old_count) return;
  
  uint32_t limit = ant_object_inobj_limit(obj);
  uint32_t old_cap = old_count > limit ? ant_object_overflow_cap_for(old_count - limit) : 0;
  
  uint32_t new_cap = ant_object_overflow_cap(obj);
  if (new_cap == old_cap) return;

  gc_note_overflow_free(ant_shape_isolate(obj->shape), (old_cap - new_cap) * sizeof(*obj->overflow_prop));
  if (!new_cap) {
    ant_object_overflow_free(obj);
    return;
  }

  ant_value_t *next = realloc(obj->overflow_prop, new_cap * sizeof(*next));
  if (next) obj->overflow_prop = next;
}

void obj_delete_prop_slot(ant_object_t *obj, uint32_t slot) {
  if (!obj || slot >= obj->prop_count) return;
  uint32_t old_count = obj->prop_count;
  ant_object_prop_set_unchecked(obj, slot, Ant_Value_Undefined);

  uint32_t shape_count = ant_shape_count(obj->shape);
  if (!ant_shape_should_compact(obj->shape)) {
    obj->prop_count = shape_count;
    obj_overflow_fit(obj, old_count);
    return;
  }

  uint32_t dst = 0;
  for (uint32_t src = 0; src < shape_count; src++) {
    if (!ant_shape_prop_at(obj->shape, src)) continue;
    if (dst != src)
      ant_object_prop_set_unchecked(obj, dst, ant_object_prop_get_unchecked(obj, src));
    dst++;
  }
  
  for (uint32_t i = dst; i < obj->prop_count; i++)
    ant_object_prop_set_unchecked(obj, i, Ant_Value_Undefined);
  obj->prop_count = ant_shape_compact(obj->shape);
  obj_overflow_fit(obj, old_count);
}

static ant_exotic_ops_t *obj_ensure_exotic_ops(ant_object_t *obj) {
  if (!obj) return NULL;
  
  ant_exotic_ops_t *owned = ant_object_owned_exotic_ops(obj);
  if (owned) return owned;
  
  ant_object_sidecar_t *sidecar = ant_object_ensure_sidecar(obj);
  if (!sidecar) return NULL;
  
  ant_exotic_ops_t *ops = realloc(sidecar->owned_exotic_ops, sizeof(*ops));
  if (!ops) return NULL;
  
  const ant_exotic_ops_t *current = ant_object_exotic_ops(obj);
  *ops = current ? *current : (ant_exotic_ops_t){0};
  
  ops->keys = ANT_EXOTIC_KEYS_ANY;
  sidecar->owned_exotic_ops = ops;
  js_obj_set_exotic(obj, ops);
  
  return ops;
}

ant_object_t *obj_alloc(ant_t *js, uint8_t type_tag, uint8_t inobj_limit) {
  ant_shape_t *shape = ant_shape_new_with_inobj_limit(js, inobj_limit);
  return shape ? obj_alloc_shaped(js, type_tag, shape) : NULL;
}

ant_object_t *obj_alloc_shaped(ant_t *js, uint8_t type_tag, ant_shape_t *shape) {
  gc_alloc_check(js);
  
  uint8_t inobj_limit = ant_shape_get_inobj_limit(shape);
  ant_object_t *obj = ant_object_cell_alloc(js, inobj_limit);
  
  if (!obj && gc_reclaim_arena(js, ant_object_arena_for(js, inobj_limit))) 
    obj = ant_object_cell_alloc(js, inobj_limit);
  
  if (!obj) {
    ant_shape_release(shape);
    return NULL;
  }

  obj->ic_identity = 0;
  obj->type_tag = type_tag;
  
  obj->u.array.len = 0;
  obj->u.array.cap = 0;
  
  if (type_tag == kTypeArray) obj->u.array.data = NULL;
  else obj->u.data.value = Ant_Value_Undefined;
  
  obj->shape = shape;
  obj->overflow_prop = NULL;
  obj->prop_count = 0;
  
  static_assert(ANT_INOBJ_BASE_SLOTS == 4, "obj_alloc_shaped unrolls the base slots");
  const ant_value_t undef = Ant_Value_Undefined;
  
  obj->inobj[0] = undef;
  obj->inobj[1] = undef;
  obj->inobj[2] = undef;
  obj->inobj[3] = undef;
  
  for (uint32_t i = ANT_INOBJ_BASE_SLOTS; i < inobj_limit; i++)
    obj->inobj[i] = undef;
  
  obj->extra_slots = NULL;
  obj->finalizer = NULL;
  obj->native_ptr = NULL;
  obj->native_tag = 0;
  
  obj->mark_epoch = 0;
  obj->flags = (ant_object_flags_t){.extensible = 1};

  obj->next = js->objects;
  js->objects = obj;
  
  return obj;
}

static void js_native_ctor_prepare(ant_t *js, ant_value_t obj, uint8_t flags) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return;

  if (vtype(ant_object_proto(ptr)) == kTypeUndefined && is_object_type(js->sym.function_proto))
    ant_object_set_proto(js, ptr, js->sym.function_proto);

  if (flags & SV_CALL_IS_DEFAULT_CTOR) {
    ptr->flags.is_constructor = 1;
    return;
  }

  ant_value_t cfunc = obj_extra_get(ptr, SLOT_CFUNC);
  if (vtype(cfunc) != kTypeBuiltin || !ptr->shape || !js->intern.prototype) return;
  if (ant_shape_lookup_interned(ptr->shape, js->intern.prototype) < 0) return;

  ptr->flags.is_constructor = 1;
  if (ant_shape_lookup_interned(ptr->shape, js->intern.length) < 0)
    js_define_function_length(js, obj, (int)js_cfunc_length(cfunc));
}

ant_value_t js_obj_to_func_ex(ant_t *js, ant_value_t obj, uint8_t flags) {
  ant_value_t func_obj = (vtype(obj) == kTypeObject) ? obj : mkval(kTypeObject, vdata(obj));
  js_native_ctor_prepare(js, func_obj, flags);

  sv_closure_t *closure = js_closure_alloc(js);

  if (!closure) {
    ant_value_t error = Ant_Exception_Peek(js);
    if (!is_err(error)) error = is_err(js->exception_oom)
      ? js->exception_oom
      : js_mkerr_typed(js, JS_ERR_TYPE, "out of memory allocating function");

    Ant_Exception_Set(js, error);
    return error;
  }
  
  closure->func = NULL;
  closure->upvalues = NULL;
  closure->native.cfunc = 0;
  closure->native.fn = NULL;
  closure->func_obj = func_obj;
  closure->bound_this = Ant_Value_Undefined;
  closure->bound_argc = 0;
  closure->super_val = Ant_Value_Undefined;
  closure->call_flags = flags;

  if (flags & SV_CALL_HAS_BOUND_ARGS)
    closure->u.bound_argv = NULL;
  else {
    closure->u.pending.name = NULL;
    closure->u.pending.len = 0;
  }
  
  return mkref(kTypeFunction, closure);
}

ant_value_t js_obj_to_func(ant_t *js, ant_value_t obj) {
  return js_obj_to_func_ex(js, obj, 0);
}

ant_value_t js_mktypedarray(void *data) {
  return mkval(kTypeTypedArray, ant_external_handle_intern(data));
}

void *js_gettypedarray(ant_value_t val) {
  if (vtype(val) != kTypeTypedArray) return NULL;
  return ant_external_handle_get(vdata(val));
}

ant_value_t js_get_slot(ant_value_t obj, internal_slot_t slot) { 
  return get_slot(js_as_obj(obj), slot); 
}

static ant_value_t mkobj_with_inobj_limit(ant_t *js, ant_offset_t parent, uint8_t inobj_limit) {
  ant_object_t *obj = obj_alloc(js, kTypeObject, inobj_limit);

  if (!obj) {
    ant_value_t failure = is_err(js->exception_oom)
      ? js->exception_oom : mkval(kTypeError, 0);
    Ant_Exception_Set(js, failure);
    return failure;
  }

  return mkref(kTypeObject, obj);
}

ant_value_t mkobj(ant_t *js, ant_offset_t parent) {
  return mkobj_with_inobj_limit(js, parent, (uint8_t)ANT_INOBJ_BASE_SLOTS);
}

ant_value_t js_mkobj_with_proto_slots(ant_t *js, ant_value_t proto, uint8_t inobj_limit) {
  if (!is_object_type(proto)) {
    ant_value_t obj = mkobj(js, 0);
    if (vtype(proto) != kTypeUndefined && vtype(obj) == kTypeObject) ant_object_set_proto(js, js_obj_ptr(obj), proto);
    return obj;
  }
  
  ant_object_t *obj = NULL;
  if (inobj_limit == ANT_INOBJ_BASE_SLOTS) obj = obj_alloc_with_proto(js, kTypeObject, proto);
  else {
    ant_shape_t *root = object_proto_root_wide(js, proto, inobj_limit);
    if (root) {
      ant_shape_retain(root);
      obj = obj_alloc_shaped(js, kTypeObject, root);
    }
  }
  if (!obj) {
    ant_value_t failure = is_err(js->exception_oom) ? js->exception_oom : mkval(kTypeError, 0);
    Ant_Exception_Set(js, failure);
    return failure;
  }
  
  return mkref(kTypeObject, obj);
}

ant_value_t js_mkobj_with_proto(ant_t *js, ant_value_t proto) {
  return js_mkobj_with_proto_slots(js, proto, ANT_INOBJ_BASE_SLOTS);
}

ant_value_t js_mkobj_from_template(ant_t *js, ant_value_t template) {
  if (vtype(template) != kTypeObject) return js_mkerr(js, "invalid object template");
  if (gc_alloc_due(js)) {
    GC_ROOT_SAVE(mark, js);
    GC_ROOT_PIN(js, template);
    gc_alloc_check(js);
    GC_ROOT_RESTORE(js, mark);
  }

  ant_object_t *source = js_obj_ptr(template);
  if (
    source->flags.is_exotic || source->finalizer || source->native_ptr ||
    source->extra_slots || ant_object_promise_state(source)
  ) return js_mkerr(js, "invalid object template state");

  ant_value_t *overflow = NULL;
  size_t overflow_bytes = source->overflow_prop ? (size_t)ant_object_overflow_cap(source) * sizeof(*overflow) : 0;
  
  if (overflow_bytes) {
    overflow = malloc(overflow_bytes);
    if (!overflow) return js_mkerr(js, "oom");
    memcpy(overflow, source->overflow_prop, overflow_bytes);
  }
  
  ant_object_t *target = ant_object_cell_alloc(js, ant_object_inobj_limit(source));
  if (!target) { free(overflow); return js_mkerr(js, "oom"); }
  if (overflow_bytes) gc_note_overflow_alloc(js, overflow_bytes);
  
  memcpy(target, source, ant_object_cell_size(ant_object_inobj_limit(source)));
  target->overflow_prop = overflow;
  ant_shape_retain(target->shape);
  target->mark_epoch = 0;
  target->ic_identity = 0;
  target->flags.generation = 0;
  target->flags.in_remember_set = 0;
  target->flags.gc_permanent = 0;
  target->next = js->objects;
  js->objects = target;
  
  return mkref(kTypeObject, target);
}

ant_value_t Ant_Object_Create(ant_t *js) {
  ant_value_t obj = mkobj(js, 0);
  js_set_proto_init(obj, js->sym.object_proto);
  return obj;
}

void set_slot(ant_value_t obj, internal_slot_t slot, ant_value_t val) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || slot < 0 || slot > SLOT_MAX) return;
  
  Ant_Assert(
    slot != SLOT_PROTO,
    "prototype written without its isolate (use js_set_slot_wb)"
  );
  
  if (slot == SLOT_DATA) {
    ptr->u.data.value = val;
    return;
  }
  
  if (obj_extra_set(ptr, slot, val)) {
    if (slot == SLOT_ARGUMENTS_OBJECT) ptr->flags.arguments_object = 0;
    if (slot == SLOT_REGEXP_FLAGS_STRING) ptr->flags.regexp_brand = vtype(val) == kTypeString;
  }
}

__attribute__((noinline, cold)) 
static void js_set_slot_proto_wb(ant_t *js, ant_object_t *ptr, ant_value_t val) {
  ant_proto_watch_note_change(ptr);
  ant_object_set_proto(js, ptr, val);
  gc_write_barrier(js, ptr, val);
  ant_ic_epoch_bump(js);
}

__attribute__((noinline, cold)) 
static ant_object_t *js_set_slot_function_holder(ant_value_t func) {
  ant_value_t obj = js_func_obj(func);
  return is_object_type(obj) && vdata(obj) ? (ant_object_t *)vptr(obj) : NULL;
}

__attribute__((always_inline)) 
static inline ant_object_t *js_set_slot_holder(ant_value_t obj) {
  if (__builtin_expect(vtype(obj) == kTypeFunction, 0)) return js_set_slot_function_holder(obj);
  return vdata(obj) ? (ant_object_t *)vptr(obj) : NULL;
}

__attribute__((always_inline)) 
static inline bool js_set_slot_store(ant_object_t *ptr, internal_slot_t slot, ant_value_t value) {
  if (slot == SLOT_DATA) {
    ptr->u.data.value = value;
    return true;
  }

  if (!obj_extra_set(ptr, slot, value)) return false;
  if (slot == SLOT_ARGUMENTS_OBJECT) ptr->flags.arguments_object = 0;
  if (slot == SLOT_REGEXP_FLAGS_STRING) ptr->flags.regexp_brand = vtype(value) == kTypeString;
  
  return true;
}

__attribute__((always_inline)) 
void js_set_slot(ant_value_t obj, internal_slot_t slot, ant_value_t value) {
  ant_object_t *ptr = js_set_slot_holder(obj);
  if (!ptr) return;
  Ant_Assert(slot != SLOT_PROTO, "prototype written without its isolate (use js_set_slot_wb)");
  js_set_slot_store(ptr, slot, value);
}

__attribute__((always_inline)) 
void js_set_slot_wb(ant_t *js, ant_value_t obj, internal_slot_t slot, ant_value_t value) {
  ant_object_t *ptr = js_set_slot_holder(obj);
  if (!ptr) return;

  if (slot == SLOT_PROTO) {
    js_set_slot_proto_wb(js, ptr, value);
    return;
  }

  if (js_set_slot_store(ptr, slot, value)) gc_write_barrier(js, ptr, value);
}

bool js_reserve_slots(ant_value_t obj, uint8_t capacity) {
  return obj_extra_reserve(js_obj_ptr(js_as_obj(obj)), capacity);
}

ant_value_t get_slot(ant_value_t obj, internal_slot_t slot) {
  ant_object_t *ptr = js_obj_ptr(obj);
  
  if (!ptr || slot < 0 || slot > SLOT_MAX) return Ant_Value_Undefined;
  if (slot == SLOT_PROTO) return ant_object_proto(ptr);
  if (slot == SLOT_DATA) return ptr->u.data.value;
  if (slot == SLOT_ARGUMENTS_OBJECT && ptr->flags.arguments_object) return Ant_Value_True;
  
  return obj_extra_get(ptr, slot);
}

static __attribute__((noinline)) ant_value_t js_get_builtin_proto(ant_t *js, ant_value_t obj) {
  ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
  if (vtype(promoted) == kTypeFunction) return js_get_proto(js, promoted);
  return ant_realm_function_proto(js, ant_realm_of_builtin(js, obj));
}

ant_value_t js_get_proto(ant_t *js, ant_value_t obj) {
  uint8_t t = vtype(obj);

  if (__builtin_expect(t == kTypeBuiltin, 0)) return js_get_builtin_proto(js, obj);
  if (!is_object_type(obj)) return Ant_Value_Null;
  
  ant_value_t as_obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(as_obj);
  ant_value_t proto = ptr ? ant_object_proto(ptr) : Ant_Value_Undefined;
  
  if (is_object_type(proto) || vtype(proto) == kTypeNull) return proto;
  if (t != kTypeObject) return js_primitive_prototype(js, t);
  
  return Ant_Value_Null;
}

ant_value_t get_proto(ant_t *js, ant_value_t obj) {
  return js_get_proto(js, obj);
}

void js_set_proto(ant_t *js, ant_value_t obj, ant_value_t proto) {
  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_promote(js, obj);
    if (is_err(promoted)) return;
    obj = promoted;
  }
  if (!is_object_type(obj)) return;

  ant_value_t as_obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (!ptr) return;

  ant_proto_watch_note_change(ptr);
  ant_object_set_proto(js, ptr, proto);
  ant_ic_epoch_bump(js);
}

void js_set_proto_init(ant_value_t obj, ant_value_t proto) {
  if (!is_object_type(obj)) return;
  
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr) return;
  if (!ptr->shape) return;
  
  ant_t *js = ant_shape_isolate(ptr->shape);
  ant_object_set_proto(js, ptr, proto);
  gc_write_barrier(js, ptr, proto);
}

void set_proto(ant_t *js, ant_value_t obj, ant_value_t proto) {
  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_promote(js, obj);
    if (is_err(promoted)) return;
    obj = promoted;
  }
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  js_set_proto(js, obj, proto);
  if (ptr) gc_write_barrier(js, ptr, proto);
}

void js_set_proto_wb(ant_t *js, ant_value_t obj, ant_value_t proto) {
  set_proto(js, obj, proto);
}

void js_set_literal_proto(ant_t *js, ant_value_t obj, ant_value_t proto) {
  if (!js_literal_proto_ok(proto) || !is_object_type(obj)) return;
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr) return;
  ant_object_invalidate_guarded_absence(js, ptr);
  ant_object_set_proto(js, ptr, proto);
  gc_write_barrier(js, ptr, proto);
}

ant_value_t js_primitive_prototype(ant_t *js, uint8_t type) {
  switch (type) {
    case kTypeObject:       return js->sym.object_proto;
    case kTypeArray:       return js->sym.array_proto;
    case kTypeString:       return js->sym.string_proto;
    case kTypeNumber:       return js->sym.number_proto;
    case kTypeBool:      return js->sym.boolean_proto;
    case kTypeFunction:      return js->sym.function_proto;
    case kTypePromise:   return js->sym.promise_proto;
    case kTypeBigInt:    return js->sym.bigint_proto;
    case kTypeSymbol:    return js->sym.symbol_proto;
    case kTypeGenerator: return js->sym.generator_proto;
    default:          return Ant_Value_Null;
  }
}

bool js_obj_extra_reserve(ant_object_t *obj, uint8_t capacity) {
  return obj_extra_reserve(obj, capacity);
}

bool js_is_constructor(ant_value_t value) {
  ant_value_t slow = value;
  ant_value_t fast = value;

  while (true) {
    uint8_t t = vtype(slow);
    if (t == kTypeFunction) {
      ant_object_t *obj = js_obj_ptr(js_func_obj(slow));
      return obj && obj->flags.is_constructor;
    }
    if (t == kTypeBuiltin) {
      const ant_cfunc_meta_t *meta = js_as_cfunc_meta(slow);
      return meta && (meta->flags & CFUNC_IS_CONSTRUCTOR) != 0;
    }
    if (t != kTypeObject) return false;

    ant_object_t *obj = js_obj_ptr(slow);
    if (!obj) return false;
    if (obj->flags.is_constructor) return true;
    if (!obj->flags.is_exotic) return false;

    ant_proxy_state_t *data = get_proxy_data(slow);
    if (!data) return false;
    slow = data->target;

    for (int i = 0; i < 2; i++) {
      if (vtype(fast) != kTypeObject) { fast = Ant_Value_Null; break; }
      ant_object_t *fobj = js_obj_ptr(fast);
      if (!fobj || !fobj->flags.is_exotic) { fast = Ant_Value_Null; break; }
      ant_proxy_state_t *fdata = get_proxy_data(fast);
      if (!fdata) { fast = Ant_Value_Null; break; }
      fast = fdata->target;
    }

    if (same_object_identity(slow, fast)) return false;
  }
}

void js_set_getter(ant_value_t obj, js_getter_fn getter) {
  if (!is_object_type(obj)) return;
  if (vtype(obj) != kTypeObject) obj = js_as_obj(obj);
  
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return;
  
  ant_proto_watch_note_change(ptr);
  js_obj_mark_exotic(ptr);
  ant_exotic_ops_t *ops = obj_ensure_exotic_ops(ptr);
  if (!ops) return;
  
  ops->getter = getter;
}

void js_set_exotic_ops(ant_value_t obj, const ant_exotic_ops_t *ops) {
  ant_object_t *ptr = is_object_type(obj) ? js_obj_ptr(js_as_obj(obj)) : NULL;
  if (!ptr) return;
  
  ant_object_sidecar_t *sidecar = ant_object_owned_exotic_ops(ptr) ? ant_object_sidecar(ptr) : NULL;
  if (sidecar) {
    free(sidecar->owned_exotic_ops);
    sidecar->owned_exotic_ops = NULL;
  }
  
  ant_proto_watch_note_change(ptr);
  js_obj_set_exotic(ptr, ops);
}

void js_set_definer(ant_value_t obj, js_definer_fn definer) {
  if (!is_object_type(obj)) return;
  if (vtype(obj) != kTypeObject) obj = js_as_obj(obj);
  
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return;
  
  ant_proto_watch_note_change(ptr);
  js_obj_mark_exotic(ptr);
  ant_exotic_ops_t *ops = obj_ensure_exotic_ops(ptr);
  if (!ops) return;
  
  ops->definer = definer;
}

void js_set_setter(ant_value_t obj, js_setter_fn setter) {
  if (!is_object_type(obj)) return;
  if (vtype(obj) != kTypeObject) obj = js_as_obj(obj);
  
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return;
  
  ant_proto_watch_note_change(ptr);
  js_obj_mark_exotic(ptr);
  ant_exotic_ops_t *ops = obj_ensure_exotic_ops(ptr);
  if (!ops) return;
  
  ops->setter = setter;
}

void js_set_deleter(ant_value_t obj, js_deleter_fn deleter) {
  if (!is_object_type(obj)) return;
  if (vtype(obj) != kTypeObject) obj = js_as_obj(obj);
  
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return;
  
  ant_proto_watch_note_change(ptr);
  js_obj_mark_exotic(ptr);
  ant_exotic_ops_t *ops = obj_ensure_exotic_ops(ptr);
  if (!ops) return;
  
  ops->deleter = deleter;
}

void js_set_finalizer(ant_value_t obj, js_finalizer_fn fn) {
  if (!is_object_type(obj)) return;
  if (vtype(obj) != kTypeObject) obj = js_as_obj(obj);
  
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return;
  
  ptr->finalizer = fn;
}
