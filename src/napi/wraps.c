#include "napi_internal.h"

static bool Slot_ReadId(ant_value_t object, internal_slot_t slot, uint64_t *id) {
  if (!is_object_type(object)) return false;

  ant_value_t stored = js_get_slot(object, slot);
  if (vtype(stored) != kTypeNumber) return false;

  *id = (uint64_t)Ant_Value_AsNumberUnchecked(stored);
  return true;
}

static void Slot_WriteId(ant_value_t object, internal_slot_t slot, uint64_t id) {
  js_set_slot(object, slot, Ant_Value_FromNumber((double)id));
}

static napi_external_entry_t *External_Find(napi_env env, ant_value_t object) {
  uint64_t id = 0;
  napi_external_entry_t *entry = NULL;

  if (env && Slot_ReadId(object, SLOT_NAPI_EXTERNAL_ID, &id))
    HASH_FIND(hh, env->externals, &id, sizeof(id), entry);

  return entry;
}

static napi_wrap_entry_t *Wrap_Find(napi_env env, ant_value_t object) {
  uint64_t id = 0;
  napi_wrap_entry_t *entry = NULL;

  if (env && Slot_ReadId(object, SLOT_NAPI_WRAP_ID, &id))
    HASH_FIND(hh, env->wraps, &id, sizeof(id), entry);

  return entry;
}

static napi_wrap_entry_t *Wrap_FindOrCreate(napi_env env, ant_value_t object) {
  napi_wrap_entry_t *entry = Wrap_Find(env, object);
  if (entry) return entry;

  entry = calloc(1, sizeof(*entry));
  if (!entry) return NULL;

  entry->id = env->wrap_next_id++;
  HASH_ADD(hh, env->wraps, id, sizeof(entry->id), entry);
  Slot_WriteId(object, SLOT_NAPI_WRAP_ID, entry->id);

  return entry;
}

static void Wrap_ReleaseIfUnused(napi_env env, napi_wrap_entry_t *entry, ant_value_t object) {
  if (entry->has_wrap || entry->attached_finalize_cb || entry->has_type_tag) return;

  HASH_DEL(env->wraps, entry);
  free(entry);
  js_set_slot(object, SLOT_NAPI_WRAP_ID, Ant_Value_Undefined);
}

static napi_status Wrap_Finish(napi_env env, napi_value object, napi_ref *result) {
  if (result) return napi_create_reference(env, object, 0, result);
  return Napi_Ok(env);
}

bool Ant_Napi_IsExternal(ant_t *js, napi_value value) {
  return External_Find(js->napi_env, Napi_Decode(value)) != NULL;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_external(
  napi_env env, void *data, node_api_basic_finalize finalize_cb, void *finalize_hint,
  napi_value *result
) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  napi_external_entry_t *entry = calloc(1, sizeof(*entry));
  if (!entry) return Napi_OutOfMemory(env);

  entry->id = env->external_next_id++;
  entry->data = data;
  entry->finalize_cb = finalize_cb;
  entry->finalize_hint = finalize_hint;
  HASH_ADD(hh, env->externals, id, sizeof(entry->id), entry);

  ant_value_t object = Ant_Object_CreateBare(env->js);
  Slot_WriteId(object, SLOT_NAPI_EXTERNAL_ID, entry->id);

  *result = Ant_Napi_Pin(env, object);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_value_external(napi_env env, napi_value value, void **result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  napi_external_entry_t *entry = External_Find(env, Napi_Decode(value));
  if (!entry) return Ant_Napi_SetLastError(env, napi_invalid_arg, "not an external");

  *result = entry->data;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_wrap(
  napi_env env, napi_value js_object, void *native_object, node_api_basic_finalize finalize_cb,
  void *finalize_hint, napi_ref *result
) {
  ant_value_t object = Napi_Decode(js_object);
  if (!env || !env->js || !is_object_type(object)) return Napi_InvalidArg(env);

  napi_wrap_entry_t *entry = Wrap_FindOrCreate(env, object);
  if (!entry) return Napi_OutOfMemory(env);

  entry->native_object = native_object;
  entry->finalize_cb = finalize_cb;
  entry->finalize_hint = finalize_hint;
  entry->has_wrap = true;

  return Wrap_Finish(env, js_object, result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_unwrap(napi_env env, napi_value js_object, void **result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  napi_wrap_entry_t *entry = Wrap_Find(env, Napi_Decode(js_object));
  if (!entry || !entry->has_wrap) return Ant_Napi_SetLastError(env, napi_invalid_arg, "object not wrapped");

  *result = entry->native_object;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_remove_wrap(napi_env env, napi_value js_object, void **result) {
  if (!env || !env->js) return Napi_InvalidArg(env);

  ant_value_t object = Napi_Decode(js_object);
  napi_wrap_entry_t *entry = Wrap_Find(env, object);
  if (!entry || !entry->has_wrap) return Ant_Napi_SetLastError(env, napi_invalid_arg, "object not wrapped");

  if (result) *result = entry->native_object;
  entry->native_object = NULL;
  entry->finalize_cb = NULL;
  entry->finalize_hint = NULL;
  entry->has_wrap = false;

  Wrap_ReleaseIfUnused(env, entry, object);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_add_finalizer(
  napi_env env, napi_value js_object, void *finalize_data, node_api_basic_finalize finalize_cb,
  void *finalize_hint, napi_ref *result
) {
  ant_value_t object = Napi_Decode(js_object);
  if (!env || !env->js || !is_object_type(object)) return Napi_InvalidArg(env);

  napi_wrap_entry_t *entry = Wrap_FindOrCreate(env, object);
  if (!entry) return Napi_OutOfMemory(env);

  entry->attached_data = finalize_data;
  entry->attached_finalize_cb = finalize_cb;
  entry->attached_finalize_hint = finalize_hint;

  return Wrap_Finish(env, js_object, result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_type_tag_object(napi_env env, napi_value value, const napi_type_tag *type_tag) {
  ant_value_t object = Napi_Decode(value);
  if (!env || !env->js || !type_tag) return Napi_InvalidArg(env);
  if (!is_object_type(object)) return Napi_ObjectExpected(env);

  napi_wrap_entry_t *entry = Wrap_FindOrCreate(env, object);
  if (!entry) return Napi_OutOfMemory(env);
  if (entry->has_type_tag) return Ant_Napi_SetLastError(env, napi_invalid_arg, "object already has a type tag");

  entry->type_tag = *type_tag;
  entry->has_type_tag = true;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_check_object_type_tag(napi_env env, napi_value value, const napi_type_tag *type_tag, bool *result) {
  ant_value_t object = Napi_Decode(value);
  if (!env || !env->js || !type_tag || !result) return Napi_InvalidArg(env);
  if (!is_object_type(object)) return Napi_ObjectExpected(env);

  napi_wrap_entry_t *entry = Wrap_Find(env, object);
  *result = entry && entry->has_type_tag
    && entry->type_tag.lower == type_tag->lower
    && entry->type_tag.upper == type_tag->upper;
  return Napi_Ok(env);
}
