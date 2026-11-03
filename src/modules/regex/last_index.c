#include "regex_internal.h"

static const ant_shape_prop_t *regexp_lastindex_lookup_own_property(
  ant_t *js,
  ant_object_t *obj,
  uint32_t *out_slot
) {
  if (!obj || !obj->shape) return NULL;

  const char *key = intern_string(js, "lastIndex", 9);
  if (!key) return NULL;
  int32_t found = ant_shape_lookup_interned(obj->shape, key);
  if (found < 0 || (uint32_t)found >= obj->prop_count) return NULL;

  uint32_t slot = (uint32_t)found;
  if (out_slot) *out_slot = slot;
  return ant_shape_prop_at(obj->shape, slot);
}

bool regexp_lastindex_fast_location(
  compiled_regex_cache_entry_t *compiled,
  ant_value_t regexp,
  ant_object_t **out_obj,
  uint32_t *out_slot
) {
  if (!compiled || !compiled->lastindex_shape) return false;
  ant_object_t *obj = js_obj_ptr(regexp);
  if (
    !obj || obj->flags.is_exotic ||
    obj->shape != compiled->lastindex_shape ||
    compiled->lastindex_slot >= obj->prop_count
  ) return false;
  *out_obj = obj;
  *out_slot = compiled->lastindex_slot;
  return true;
}

void regexp_lastindex_cache_location(
  ant_t *js,
  compiled_regex_cache_entry_t *compiled,
  ant_value_t regexp
) {
  if (!compiled || is_proxy(regexp)) return;
  ant_object_t *obj = js_obj_ptr(regexp);
  if (!obj || obj->flags.is_exotic || !obj->shape) return;
  if (
    compiled->lastindex_shape == obj->shape &&
    compiled->lastindex_slot < obj->prop_count
  ) return;

  uint32_t slot;
  const ant_shape_prop_t *prop = regexp_lastindex_lookup_own_property(
    js, obj, &slot
  );
  if (
    !prop || prop->has_getter || prop->has_setter ||
    !(prop->attrs & ANT_PROP_ATTR_WRITABLE)
  ) return;

  if (compiled->lastindex_shape != obj->shape) {
    ant_shape_retain(obj->shape);
    ant_shape_release(compiled->lastindex_shape);
    compiled->lastindex_shape = obj->shape;
  }
  compiled->lastindex_slot = slot;
}

ant_value_t regexp_set_lastindex(
  ant_t *js,
  compiled_regex_cache_entry_t *compiled,
  ant_value_t regexp,
  ant_value_t value
) {
  ant_object_t *obj;
  uint32_t slot;
  if (regexp_lastindex_fast_location(
    compiled, regexp, &obj, &slot
  )) {
    ant_object_prop_set_unchecked(obj, slot, value);
    gc_write_barrier(js, obj, value);
    return value;
  }

  ant_object_t *receiver = js_obj_ptr(regexp);
  if (!is_proxy(regexp)) {
    const ant_shape_prop_t *prop = regexp_lastindex_lookup_own_property(js, receiver, NULL);
    if (
      prop && !prop->has_getter && !prop->has_setter &&
      !(prop->attrs & ANT_PROP_ATTR_WRITABLE)
    ) return Ant_Error_CreateReadOnlyProperty(js, regexp, js_mkstr(js, "lastIndex", 9));
  }
  return setprop_cstr(js, regexp, "lastIndex", 9, value);
}

static bool regexp_has_builtin_exec(ant_t *js, ant_value_t rx) {
  ant_prop_loc_t exec_loc = lkp_proto(js, rx, "exec", 4);
  if (
    !exec_loc.obj || !exec_loc.obj->shape ||
    exec_loc.obj->flags.is_exotic ||
    exec_loc.slot >= exec_loc.obj->prop_count
  ) return false;

  const ant_shape_prop_t *exec_prop = ant_shape_prop_at(exec_loc.obj->shape, exec_loc.slot);
  if (!exec_prop || exec_prop->has_getter || exec_prop->has_setter) return false;

  ant_value_t exec_fn = js_prop_load(exec_loc);
  return vtype(exec_fn) == kTypeBuiltin && js_cfunc_same_entrypoint(exec_fn, builtin_regexp_exec);
}

static __attribute__((noinline)) bool regexp_fast_get_builtin_exec(
  ant_t *js, ant_value_t rx, ant_object_t *obj, regexp_fast_t *fast
) {
  if (!obj->flags.regexp_brand || obj->flags.is_exotic || !regexp_has_builtin_exec(js, rx)) return false;

  compiled_regex_cache_entry_t *hint;
  uint8_t flags = regexp_flags_mask(js, rx, &hint);
  compiled_regex_cache_entry_t *compiled = regex_get_or_compile(js, rx, flags, hint);
  if (!compiled) return false;

  ant_object_t *li_obj;
  uint32_t li_slot;
  regexp_lastindex_cache_location(js, compiled, rx);
  if (
    !regexp_lastindex_fast_location(compiled, rx, &li_obj, &li_slot) ||
    vtype(ant_object_prop_get_unchecked(li_obj, li_slot)) != kTypeNumber
  ) return false;

  *fast = (regexp_fast_t){ .obj = li_obj, .compiled = compiled, .li_slot = li_slot, .flags = flags };
  return true;
}

bool regexp_fast_get(ant_t *js, ant_value_t rx, regexp_fast_tier_t tier, regexp_fast_t *fast) {
  ant_regex_state_t *state = js->regex_state;
  if (!state || vtype(rx) != kTypeObject) return false;

  ant_object_t *obj = js_obj_ptr(rx);
  if (
    js->regexp.protector_invalid ||
    obj->shape != state->instance_shape.shape ||
    ant_object_proto(obj) != js->builtins.regexp_proto ||
    !obj->flags.regexp_brand || obj->flags.is_exotic
  ) return tier == REGEXP_FAST_BUILTIN_EXEC && regexp_fast_get_builtin_exec(js, rx, obj, fast);

  if (vtype(ant_object_prop_get_unchecked(obj, REGEXP_LAST_INDEX_SLOT)) != kTypeNumber) return false;
  compiled_regex_cache_entry_t *compiled = js_get_native(rx, REGEXP_NATIVE_TAG);
  if (!compiled) compiled = regex_get_or_compile(js, rx, regexp_flags_mask(js, rx, NULL), NULL);
  if (!compiled) return false;

  *fast = (regexp_fast_t){
    .obj = obj, .compiled = compiled,
    .li_slot = REGEXP_LAST_INDEX_SLOT, .flags = compiled->flags_mask,
  };
  return true;
}

__attribute__((noinline)) ant_value_t regexp_read_lastindex_slow(
  ant_t *js, compiled_regex_cache_entry_t *compiled_hint, ant_value_t regexp, ant_value_t str_arg, double *out
) {
  ant_object_t *lastindex_obj;
  uint32_t lastindex_slot;
  ant_value_t li_val;
  if (compiled_hint) regexp_lastindex_cache_location(js, compiled_hint, regexp);
  if (regexp_lastindex_fast_location(compiled_hint, regexp, &lastindex_obj, &lastindex_slot))
    li_val = ant_object_prop_get_unchecked(lastindex_obj, lastindex_slot);
  else {
    ant_prop_loc_t lastindex_off = lkp(js, regexp, "lastIndex", 9);
    li_val = lastindex_off.obj ? js_prop_load(lastindex_off) : Ant_Value_Undefined;
  }
  if (vtype(li_val) == kTypeNumber) {
    *out = Ant_Value_AsNumberUnchecked(li_val);
    return Ant_Value_Undefined;
  }
  GC_ROOT_SAVE(li_mark, js);
  GC_ROOT_PIN(js, regexp);
  GC_ROOT_PIN(js, str_arg);
  size_t length = 0;
  ant_value_t status = js_to_length_slow(js, li_val, &length);
  GC_ROOT_RESTORE(js, li_mark);
  if (is_err(status)) return status;
  *out = (double)length;
  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : Ant_Value_Undefined;
}
