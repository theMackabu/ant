#include <compat.h> // IWYU pragma: keep
#include <stdlib.h>

#include "arrays.h"
#include "arguments.h"
#include "cfunc.h"
#include "silver/engine.h"

static constexpr uint32_t ANT_ARGUMENTS_NATIVE_TAG = Ant_NativeTag("ARGS");

static inline sv_frame_t *js_arguments_frame(ant_t *js, ant_arguments_state_t *state) {
  if (state->direct_frame) return state->direct_frame;
  return &js->vm->frames[state->frame_index];
}

ant_arguments_state_t *js_arguments_state(ant_value_t obj) {
  return (ant_arguments_state_t *)js_get_native(obj, ANT_ARGUMENTS_NATIVE_TAG);
}

static inline sv_upvalue_t *js_arguments_cell(ant_arguments_state_t *state, unsigned long idx) {
  if (!state || !state->cells || idx >= state->mapped_count || state->deleted[idx]) return NULL;
  return state->cells[idx];
}

static inline ant_value_t js_arguments_mapped_value(ant_t *js, ant_arguments_state_t *state, unsigned long idx, bool *mapped) {
  sv_upvalue_t *cell = js_arguments_cell(state, idx);
  
  *mapped = true;
  if (cell) return *cell->location;
  
  if (state && state->frame_index >= 0 && idx < state->mapped_count && !state->deleted[idx])
    return js_arguments_frame(js, state)->bp[idx];
  
  *mapped = false;
  return Ant_Value_Undefined;
}

static inline void js_arguments_write_mapped(ant_t *js, ant_arguments_state_t *state, unsigned long idx, ant_value_t value) {
  sv_upvalue_t *cell = js_arguments_cell(state, idx);
  if (cell) {
    *cell->location = value;
    gc_upvalue_write_barrier(js, cell, value);
  } else if (state && state->frame_index >= 0 && idx < state->mapped_count && !state->deleted[idx])
    js_arguments_frame(js, state)->bp[idx] = value;
}

static void js_arguments_link_cell(ant_t *js, ant_value_t obj, ant_arguments_state_t *state, size_t idx, sv_upvalue_t *uv) {
  if (!state || idx >= state->mapped_count || idx > UINT8_MAX || state->deleted[idx]) return;
  state->cells[idx] = uv;
  uv->maps_arguments = 1;
  uv->args_index = (uint8_t)idx;
  uv->args_owner = fixed_arena_slot_index(&js->obj_arena, js_obj_ptr(obj));
}

void js_arguments_link_upvalue(ant_t *js, sv_frame_t *frame, sv_upvalue_t *uv) {
  if (!frame->bp || uv->location < frame->bp) return;
  ant_value_t obj = frame->arguments_obj;
  js_arguments_link_cell(js, obj, js_arguments_state(obj), (size_t)(uv->location - frame->bp), uv);
}

void js_arguments_upvalue_written(ant_t *js, sv_upvalue_t *uv, ant_value_t value) {
  ant_object_t *owner = fixed_arena_slot_at(&js->obj_arena, uv->args_owner);
  
  if (!owner || owner->mark_epoch == ANT_GC_DEAD) return;
  if (!ant_object_is_arguments(owner)) return;

  ant_value_t obj = js_obj_from_ptr(owner);
  ant_arguments_state_t *state = js_arguments_state(obj);
  
  uint32_t idx = uv->args_index;
  if (!state || idx >= state->mapped_count || state->cells[idx] != uv || state->deleted[idx]) return;

  if (owner->u.array.data && idx < owner->u.array.len && idx < owner->u.array.cap) {
    owner->u.array.data[idx] = value;
    gc_write_barrier(js, owner, value);
    return;
  }
  
  state->in_setter = 1;
  arr_set(js, obj, (ant_offset_t)idx, value);
  state->in_setter = 0;
}

void gc_mark_arguments_cells(ant_t *js, ant_value_t obj) {
  ant_arguments_state_t *state = js_arguments_state(obj);
  if (state && state->cells) gc_mark_upvalue_cells(js, state->cells, state->mapped_count);
}

ant_value_t js_arguments_getter(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  ant_offset_t arr_len = get_array_length(js, obj);
  unsigned long idx = 0;
  
  if (!parse_array_index(key, key_len, arr_len, &idx)) return Ant_Value_Undefined;
  if ((ant_offset_t)idx >= arr_len) return Ant_Value_Undefined;

  bool mapped = false;
  ant_value_t value = js_arguments_mapped_value(js, js_arguments_state(obj), idx, &mapped);
  return mapped ? value : arr_get(js, obj, (ant_offset_t)idx);
}

bool js_arguments_setter(
  ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t value
) {
  unsigned long idx = 0;
  if (!parse_array_index(key, key_len, (ant_offset_t)-1, &idx)) return false;

  ant_arguments_state_t *state = js_arguments_state(obj);
  if (state && state->in_setter) return false;
  if (state) state->in_setter = 1;
  arr_set(js, obj, (ant_offset_t)idx, value);
  
  if (state) state->in_setter = 0;
  js_arguments_write_mapped(js, state, idx, value);
  return true;
}

ant_value_t define_abrupt(define_desc_fields_t *fields, ant_value_t err) {
  if (fields) fields->abrupt = true;
  return err;
}

void js_arguments_after_define(
  ant_t *js, ant_value_t obj, const char *key, 
  size_t key_len, const define_desc_fields_t *desc
) {
  unsigned long idx = 0;
  if (!parse_array_index(key, key_len, (ant_offset_t)-1, &idx)) return;
  
  ant_arguments_state_t *state = js_arguments_state(obj);
  if (!state || (uint32_t)idx >= state->mapped_count || state->deleted[idx]) return;

  if (desc->accessor) {
    state->deleted[idx] = 1;
    return;
  }

  if (desc->has_value) js_arguments_write_mapped(js, state, idx, desc->value);
  if (desc->has_writable && !desc->writable) state->deleted[idx] = 1;
}

void js_arguments_unmap_all(ant_t *js, ant_value_t obj) {
  ant_arguments_state_t *state = js_arguments_state(obj);
  if (!state) return;
  
  for (uint32_t i = 0; i < state->mapped_count; i++) {
    bool mapped = false;
    ant_value_t value = js_arguments_mapped_value(js, state, i, &mapped);
    if (!mapped) continue;
    
    state->in_setter = 1;
    arr_set(js, obj, (ant_offset_t)i, value);
    state->in_setter = 0;
    state->deleted[i] = 1;
  }
}

static bool js_arguments_deleter(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  unsigned long idx = 0;
  if (!parse_array_index(key, key_len, (ant_offset_t)-1, &idx)) return true;

  ant_arguments_state_t *state = js_arguments_state(obj);
  if (state && (uint32_t)idx < state->mapped_count) state->deleted[idx] = 1;
  return true;
}

static const ant_exotic_ops_t js_arguments_exotic_ops = {
  .getter = js_arguments_getter,
  .setter = js_arguments_setter,
  .deleter = js_arguments_deleter,
};

static void js_arguments_finalizer(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  ant_arguments_state_t *state = js_arguments_state(value);
  
  free(state);
  js_clear_native(value, ANT_ARGUMENTS_NATIVE_TAG);
}

static ant_value_t builtin_arguments_callee_thrower(ant_params_t) {
  return js_mkerr_typed(js, JS_ERR_TYPE,
    "'caller', 'callee', and 'arguments' properties may not be accessed "
    "on strict mode functions or the arguments objects for calls to them"
  );
}

static ant_value_t arguments_template(ant_t *js, bool is_strict, bool has_iterator) {
  ant_value_t *cached = is_strict
    ? (has_iterator ? &js->builtins.arguments_iter_template : &js->builtins.arguments_template)
    : (has_iterator ? &js->builtins.sloppy_arguments_iter_template : &js->builtins.sloppy_arguments_template);
    
  if (vtype(*cached) == kTypeObject) return *cached;

  GC_ROOT_SAVE(roots, js);
  ant_value_t seed = Ant_Object_CreateBare(js);
  
  if (is_err(seed)) return seed;
  GC_ROOT_PIN(js, seed);
  if (is_object_type(js->sym.object_proto)) js_set_proto_init(seed, js->sym.object_proto);
  
  setprop_cstr(js, seed, "length", 6, Ant_Value_FromNumber(0));
  js_set_descriptor(js, seed, "length", 6, JS_DESC_W | JS_DESC_C);
  
  if (is_strict) {
    static const ant_cfunc_meta_t thrower_meta = { builtin_arguments_callee_thrower, "", 0, 0 };
    ant_value_t thrower = js_mkfun_meta(&thrower_meta);
    js_set_accessor_desc(js, seed, "callee", 6, thrower, thrower, 0);
  } else {
    setprop_cstr(js, seed, "callee", 6, Ant_Value_Undefined);
    js_set_descriptor(js, seed, "callee", 6, JS_DESC_W | JS_DESC_C);
  }
  
  if (has_iterator) {
    js_set_sym(js, seed, js->sym.iterator_sym, Ant_Value_Undefined);
    js_set_sym_descriptor(js, seed, js->sym.iterator_sym, JS_DESC_W | JS_DESC_C);
  }
  
  ant_object_t *ptr = js_obj_ptr(seed);
  if (ptr->prop_count != (has_iterator ? 3u : 2u) || ant_object_inobj_limit(ptr) < ptr->prop_count) {
    GC_ROOT_RESTORE(js, roots);
    return js_mkerr(js, "oom");
  }
  
  Ant_Assert(
    ant_shape_lookup_interned(ptr->shape, js->intern.length) == ANT_ARGUMENTS_SLOT_LENGTH &&
    ant_shape_lookup_interned(ptr->shape, intern_string(js, "callee", 6)) == ANT_ARGUMENTS_SLOT_CALLEE,
    "arguments template slots out of order"
  );
  
  *cached = seed;
  GC_ROOT_RESTORE(js, roots);
  
  return seed;
}

ant_value_t js_create_arguments_object(
  ant_t *js, sv_vm_t *vm,
  ant_value_t callee, sv_frame_t *frame,
  int argc, int mapped_count, bool is_strict
) {
  GC_ROOT_SAVE(root_mark, js);

  uint32_t count = frame && frame->bp && argc > 0 ? (uint32_t)argc : 0;
  ant_value_t arr = mkarr_dense_literal_capacity(js, count ? frame->bp : NULL, count, true);
  
  if (is_err(arr)) {
    GC_ROOT_RESTORE(js, root_mark);
    return arr;
  } 
  
  GC_ROOT_PIN(js, arr);
  ant_object_t *target = js_obj_ptr(arr);
  
  target->flags.arguments_object = 1;
  if (is_object_type(js->sym.object_proto)) js_set_proto_init(arr, js->sym.object_proto);

  ant_value_t iter_fn = js->sym.array_values_fn;
  bool has_iterator =
    vtype(js->sym.iterator_sym) == kTypeSymbol &&
    (vtype(iter_fn) == kTypeFunction || vtype(iter_fn) == kTypeBuiltin);
  GC_ROOT_PIN(js, iter_fn);
  
  ant_value_t seed = arguments_template(js, is_strict, has_iterator);
  if (is_err(seed)) {
    GC_ROOT_RESTORE(js, root_mark);
    return seed;
  }
  
  ant_object_t *source = js_obj_ptr(seed);
  ant_shape_retain(source->shape);
  ant_shape_release(target->shape);
  
  target->shape = source->shape;
  target->inobj_limit = source->inobj_limit;
  target->prop_count = source->prop_count;
  
  ant_object_set_proto(js, target, target->proto);
  
  target->inobj[ANT_ARGUMENTS_SLOT_LENGTH] = Ant_Value_FromNumber((double)count);
  target->inobj[ANT_ARGUMENTS_SLOT_CALLEE] = is_strict ? source->inobj[ANT_ARGUMENTS_SLOT_CALLEE] : callee;
  
  gc_write_barrier(js, target, target->inobj[ANT_ARGUMENTS_SLOT_CALLEE]);
  
  if (has_iterator) {
    target->inobj[ANT_ARGUMENTS_SLOT_ITERATOR] = iter_fn;
    gc_write_barrier(js, target, iter_fn);
  }

  if (!is_strict && mapped_count > 0 && frame && vm) {
    size_t cells_off = (sizeof(ant_arguments_state_t) + (size_t)mapped_count + _Alignof(sv_upvalue_t *) - 1) & ~(_Alignof(sv_upvalue_t *) - 1);
    ant_arguments_state_t *state = calloc(1, cells_off + (size_t)mapped_count * sizeof(sv_upvalue_t *));
    
    if (!state) {
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr(js, "oom");
    }

    state->cells = (sv_upvalue_t **)((uint8_t *)state + cells_off);
    state->frame_index = (int)(frame - vm->frames);
    state->mapped_count = (uint32_t)mapped_count;
    
    js_set_native(arr, state, ANT_ARGUMENTS_NATIVE_TAG);
    target->finalizer = js_arguments_finalizer;
    
    js_obj_mark_exotic(target);
    target->exotic_ops = &js_arguments_exotic_ops;

    for (sv_upvalue_t *uv = vm->open_upvalues; uv; uv = uv->next) {
      if (uv->location < frame->bp) break;
      if (uv->location < frame->bp + mapped_count)
        js_arguments_link_cell(js, arr, state, (size_t)(uv->location - frame->bp), uv);
    }
  }

  GC_ROOT_RESTORE(js, root_mark);
  return arr;
}

void js_arguments_rebind_frame(ant_t *js, ant_value_t obj, int frame_index) {
  ant_arguments_state_t *state = js_arguments_state(obj);
  if (!state || state->frame_index < 0) return;
  state->frame_index = frame_index;
  state->direct_frame = NULL;
}

void js_arguments_bind_direct(ant_t *js, ant_value_t obj, sv_frame_t *frame) {
  ant_arguments_state_t *state = js_arguments_state(obj);
  if (!state || state->frame_index < 0 || !frame) return;
  state->direct_frame = frame;
}

void js_arguments_detach(ant_t *js, ant_value_t obj) {
  ant_arguments_state_t *state = js_arguments_state(obj);
  if (!state || state->frame_index < 0) return;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);

  sv_frame_t *frame = js_arguments_frame(js, state);
  ant_offset_t arr_len = get_array_length(js, obj);
  ant_offset_t limit = (ant_offset_t)state->mapped_count;
  if (arr_len < limit) limit = arr_len;

  for (ant_offset_t i = 0; i < limit; i++) {
    if (state->deleted[i]) continue;
    sv_upvalue_t *cell = js_arguments_cell(state, (unsigned long)i);
    state->in_setter = 1;
    arr_set(js, obj, i, cell ? *cell->location : frame->bp[i]);
    state->in_setter = 0;
  }

  state->frame_index = -1;
  state->direct_frame = NULL;
  GC_ROOT_RESTORE(js, root_mark);
}

void js_arguments_sync_slot(ant_t *js, ant_value_t obj, uint32_t idx, ant_value_t value) {
  ant_arguments_state_t *state = js_arguments_state(obj);
  if (!state || state->frame_index < 0 ||
      idx >= state->mapped_count || state->deleted[idx]) {
    return;
  }

  state->in_setter = 1;
  arr_set(js, obj, (ant_offset_t)idx, value);
  state->in_setter = 0;
}
