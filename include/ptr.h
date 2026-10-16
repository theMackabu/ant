#ifndef ANT_NATIVE_PTR_H
#define ANT_NATIVE_PTR_H

#include "types.h"
#include "internal.h" // IWYU pragma: keep
#include "hash.h"

#define Ant_NativeTag(s) ((uint32_t)(                                    \
  (uint32_t)(uint8_t)(s)[0] << 24 | (uint32_t)(uint8_t)(s)[1] << 16 |    \
  (uint32_t)(uint8_t)(s)[2] << 8  | (uint32_t)(uint8_t)(s)[3]))

void ant_ptr_pair_free(ant_ptr_pair_map_t *map);
void js_set_native_slow(ant_object_t *o, void *ptr, uint32_t tag);

bool ant_ptr_pair_remove(ant_ptr_pair_map_t *map, uintptr_t key_a, uintptr_t key_b);
bool ant_ptr_pair_put(ant_ptr_pair_map_t *map, uintptr_t key_a, uintptr_t key_b, ant_value_t value);

static inline void *js_get_native(ant_value_t obj, uint32_t tag) {
  ant_object_t *o = js_obj_ptr(obj);
  
  if (!o || !tag) return NULL;
  if (o->native_tag == tag) return o->native_ptr;

  ant_object_sidecar_t *sidecar = ant_object_sidecar(o);
  if (!sidecar) return NULL;
  
  for (uint8_t i = 0; i < sidecar->native_count; i++)
    if (sidecar->native_entries[i].tag == tag) return sidecar->native_entries[i].ptr;

  return NULL;
}

static inline void js_clear_native(ant_value_t obj, uint32_t tag) {
  ant_object_t *o = js_obj_ptr(obj);
  if (!o || !tag) return;

  ant_object_sidecar_t *sidecar = ant_object_sidecar(o);
  if (o->native_tag == tag) {
    if (sidecar && sidecar->native_count > 0) {
      sidecar->native_count--;
      o->native_ptr = sidecar->native_entries[sidecar->native_count].ptr;
      o->native_tag = sidecar->native_entries[sidecar->native_count].tag;
    } else {
      o->native_ptr = NULL;
      o->native_tag = 0;
    }
    
    return;
  }

  if (!sidecar) return;
  
  for (uint8_t i = 0; i < sidecar->native_count; i++) {
    if (sidecar->native_entries[i].tag != tag) continue;
    
    sidecar->native_count--;
    sidecar->native_entries[i] = sidecar->native_entries[sidecar->native_count];
    
    return;
  }
}

static inline void js_set_native(ant_value_t obj, void *ptr, uint32_t tag) {
  ant_object_t *o = js_obj_ptr(obj);
  if (!o || !tag) return;
  
  if (o->native_tag == 0 || o->native_tag == tag) {
    o->native_ptr = ptr;
    o->native_tag = tag;
    return;
  }

  js_set_native_slow(o, ptr, tag);
}

static inline bool js_check_native_tag(ant_value_t obj, uint32_t tag) {
  ant_object_t *o = js_obj_ptr(obj);
  
  if (!o || !tag) return false;
  if (o->native_tag == tag) return true;

  ant_object_sidecar_t *sidecar = ant_object_sidecar(o);
  if (!sidecar) return false;
  
  for (uint8_t i = 0; i < sidecar->native_count; i++)
    if (sidecar->native_entries[i].tag == tag) return true;

  return false;
}

static inline uint32_t ant_ptr_pair_hash(uintptr_t key_a, uintptr_t key_b) {
  return (uint32_t)ant_hash_mix(key_a ^ 0x9E3779B97F4A7C15ull, key_b ^ 0xC2B2AE3D27D4EB4Full);
}

static inline bool ant_ptr_pair_get(
  const ant_ptr_pair_map_t *map, uintptr_t key_a, uintptr_t key_b, ant_value_t *out
) {
  if (!map->entries) return false;
  
  for (uint32_t i = ant_ptr_pair_hash(key_a, key_b);; i++) {
    const ant_ptr_pair_entry_t *entry = &map->entries[i & map->mask];
    
    if (!entry->key_a) return false;
    if (entry->key_a == key_a && entry->key_b == key_b) { *out = entry->value; return true; }
  }
}

#endif
