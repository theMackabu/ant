#include "internal.h"
#include "arrays.h"
#include "gc/roots.h"
#include "silver/code_unit.h"
#include "silver/literal_sites.h"
#include "ops/property.h"

static ant_value_t Literal_Build(ant_t *js, sv_func_t *func, const sv_literal_site_t *site);
static ant_value_t Literal_Clone(ant_t *js, ant_value_t boilerplate);

void Silver_Literal_InitElement(ant_t *js, ant_value_t arr, uint32_t index, ant_value_t value) {
  ant_object_t *ptr = (ant_object_t *)vptr(arr);
  if (ptr->flags.fast_array && ptr->u.array.data && index < ptr->u.array.len && index < ptr->u.array.cap) {
    ptr->u.array.data[index] = value;
    gc_write_barrier_elem(js, ptr, index, value);
  } else arr_set(js, arr, index, value);
}

static ant_value_t Literal_FromBlock(ant_t *js, const sv_literal_site_t *site) {
  return Ant_Array_CreateFromBlock(js, site->block, site->flags & SV_LITERAL_HAS_DYNAMIC);
}

static ant_value_t Literal_Value(ant_t *js, sv_func_t *func, ant_value_t value) {
  if (!sv_literal_is_child(value)) return value;
  return Literal_Build(js, func, &func->debug->literal_sites[(uint32_t)value]);
}

static ant_value_t Literal_BuildArray(ant_t *js, sv_func_t *func, const sv_literal_site_t *site) {
  if (site->block) return Literal_FromBlock(js, site);
  
  const ant_value_t *values = func->constants + site->start;
  ant_value_t arr = js_mkarr_dense_literal(js, values, site->count);
  if (is_err(arr) || !(site->flags & SV_LITERAL_HAS_CHILDREN)) return arr;
  
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, arr);
  
  for (uint32_t i = 0; i < site->count; i++) {
    if (!sv_literal_is_child(values[i])) continue;
    ant_value_t child = Literal_Value(js, func, values[i]);
    if (is_err(child)) {
      GC_ROOT_RESTORE(js, mark);
      return child;
    }
    Silver_Literal_InitElement(js, arr, i, child);
  }
  
  GC_ROOT_RESTORE(js, mark);
  return arr;
}

static void Literal_ShapeObject(ant_t *js, sv_func_t *func, const sv_literal_site_t *site, ant_object_t *ptr) {
  ant_shape_t *shape = ptr->shape;
  ant_shape_retain(shape);
  
  for (uint32_t i = 0; i < site->count; i++) {
    if (ant_shape_add_interned_tr(js, &shape, func->atoms[site->key_atoms[i]].str, ANT_PROP_ATTR_DEFAULT, NULL)) continue;
    ant_shape_release(shape);
    return;
  }
  
  ant_shape_release(ptr->shape);
  ptr->shape = shape;
  js_obj_ensure_prop_capacity(js, ptr, site->count);
}

static ant_value_t Literal_BuildObject(ant_t *js, sv_func_t *func, const sv_literal_site_t *site) {
  ant_value_t obj = Ant_Object_Create(js);
  if (is_err(obj)) return obj;
  if (site->count) Literal_ShapeObject(js, func, site, (ant_object_t *)vptr(obj));
  
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, obj);
  
  const ant_value_t *values = func->constants + site->start;
  for (uint32_t i = 0; i < site->count; i++) {
    ant_value_t value = Literal_Value(js, func, values[i]);
    if (is_err(value)) {
      GC_ROOT_RESTORE(js, mark);
      return value;
    }
    
    sv_atom_t *key = &func->atoms[site->key_atoms[i]];
    sv_define_slot(js, obj, value, key->str, key->len, i);
    
    if (Ant_Exception_Pending(js)) {
      GC_ROOT_RESTORE(js, mark);
      return Ant_Exception_Current(js);
    }
  }
  
  GC_ROOT_RESTORE(js, mark);
  return obj;
}

static ant_value_t Literal_Build(ant_t *js, sv_func_t *func, const sv_literal_site_t *site) {
  return site->kind == SV_LITERAL_OBJECT
    ? Literal_BuildObject(js, func, site)
    : Literal_BuildArray(js, func, site);
}

static ant_value_t Literal_CloneArray(ant_t *js, ant_value_t boilerplate) {
  ant_object_t *src = (ant_object_t *)vptr(boilerplate);
  if (ant_array_is_shared(src)) return Ant_Array_CreateFromBlock(js, gc_shared_elements_of(src->u.array.data), false);
  
  uint32_t count = src->u.array.len;
  ant_value_t arr = Ant_Array_CreateFromElements(js, src->u.array.data, count, count, src->flags.may_have_holes);
  if (is_err(arr)) return arr;
  
  ant_object_t *dst = (ant_object_t *)vptr(arr);
  GC_ROOT_SAVE(mark, js);
  bool pinned = false;
  
  for (uint32_t i = 0; i < count; i++) {
    ant_value_t value = src->u.array.data[i];
    if (!is_object_type(value)) continue;
    if (!pinned) {
      GC_ROOT_PIN(js, arr);
      pinned = true;
    }
    
    ant_value_t child = Literal_Clone(js, value);
    if (is_err(child)) {
      GC_ROOT_RESTORE(js, mark);
      return child;
    }
    
    dst->u.array.data[i] = child;
    gc_write_barrier_elem(js, dst, i, child);
  }
  
  GC_ROOT_RESTORE(js, mark);
  return arr;
}

static ant_value_t Literal_CloneObject(ant_t *js, ant_value_t boilerplate) {
  ant_value_t obj = js_mkobj_from_template(js, boilerplate);
  if (is_err(obj)) return obj;
  
  ant_object_t *src = (ant_object_t *)vptr(boilerplate);
  ant_object_t *dst = (ant_object_t *)vptr(obj);
  GC_ROOT_SAVE(mark, js);
  bool pinned = false;
  
  for (uint32_t i = 0; i < src->prop_count; i++) {
    ant_value_t value = ant_object_prop_get_unchecked(src, i);
    if (!is_object_type(value)) continue;
    if (!pinned) {
      GC_ROOT_PIN(js, obj);
      pinned = true;
    }
    
    ant_value_t child = Literal_Clone(js, value);
    if (is_err(child)) {
      GC_ROOT_RESTORE(js, mark);
      return child;
    }
    
    ant_object_prop_set_unchecked(dst, i, child);
    gc_write_barrier_prop(js, dst, child);
  }
  
  GC_ROOT_RESTORE(js, mark);
  return obj;
}

static ant_value_t Literal_Clone(ant_t *js, ant_value_t boilerplate) {
  return vtype(boilerplate) == kTypeArray
    ? Literal_CloneArray(js, boilerplate)
    : Literal_CloneObject(js, boilerplate);
}

ant_value_t Silver_Literal_Create(ant_t *js, sv_func_t *func, uint32_t index) {
  sv_literal_site_t *site = &func->debug->literal_sites[index];
  if (site->block) return Literal_FromBlock(js, site);
  
  if (__builtin_expect(vtype(site->boilerplate) == kTypeUndefined, 0)) {
    if (!site->built_once) {
      site->built_once = true;
      return Literal_Build(js, func, site);
    }
    
    ant_value_t boilerplate = Literal_Build(js, func, site);
    if (is_err(boilerplate)) return boilerplate;
    
    GC_ROOT_SAVE(mark, js);
    GC_ROOT_PIN(js, boilerplate);
    bool retained = sv_code_unit_retain_template(js, func, boilerplate);
    GC_ROOT_RESTORE(js, mark);
    
    if (!retained) return js_mkerr(js, "oom");
    site->boilerplate = boilerplate;
  }
  
  return Literal_Clone(js, site->boilerplate);
}
