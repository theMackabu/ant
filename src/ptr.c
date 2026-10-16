#include "ptr.h"

void js_set_native_slow(ant_object_t *o, void *ptr, uint32_t tag) {
  ant_object_sidecar_t *sidecar = ant_object_ensure_sidecar(o);
  if (!sidecar) return;
  
  for (uint8_t i = 0; i < sidecar->native_count; i++) {
    if (sidecar->native_entries[i].tag != tag) continue;
    sidecar->native_entries[i].ptr = ptr;
    return;
  }

  if (sidecar->native_count >= sidecar->native_cap) {
    uint8_t next_cap = sidecar->native_cap ? (uint8_t)(sidecar->native_cap * 2) : 2;
    ant_native_entry_t *next = realloc(sidecar->native_entries, next_cap * sizeof(*next));
    
    if (!next) return;
    sidecar->native_entries = next;
    sidecar->native_cap = next_cap;
  }

  sidecar->native_entries[sidecar->native_count++] = (ant_native_entry_t){ ptr, tag };
}

bool ant_ptr_pair_put(ant_ptr_pair_map_t *map, uintptr_t key_a, uintptr_t key_b, ant_value_t value) {
  if (!map->entries || (map->len + 1) * 2 > map->mask + 1) {
    uint32_t cap = map->entries ? (map->mask + 1) * 2 : 16;
    ant_ptr_pair_entry_t *grown = calloc(cap, sizeof(*grown));
    if (!grown) return false;
    
    ant_ptr_pair_map_t next = { .entries = grown, .len = 0, .mask = cap - 1 };
    for (uint32_t i = 0; map->entries && i <= map->mask; i++) {
      ant_ptr_pair_entry_t *entry = &map->entries[i];
      if (entry->key_a) ant_ptr_pair_put(&next, entry->key_a, entry->key_b, entry->value);
    }
    
    free(map->entries);
    *map = next;
  }

  uint32_t i = ant_ptr_pair_hash(key_a, key_b);
  for (;; i++) {
    ant_ptr_pair_entry_t *entry = &map->entries[i & map->mask];
    if (entry->key_a && (entry->key_a != key_a || entry->key_b != key_b)) continue;
    if (!entry->key_a) map->len++;
    *entry = (ant_ptr_pair_entry_t){ .key_a = key_a, .key_b = key_b, .value = value };
    return true;
  }
}

bool ant_ptr_pair_remove(ant_ptr_pair_map_t *map, uintptr_t key_a, uintptr_t key_b) {
  if (!map->entries) return false;
  uint32_t i = ant_ptr_pair_hash(key_a, key_b) & map->mask;
  
  for (;; i = (i + 1) & map->mask) {
    ant_ptr_pair_entry_t *entry = &map->entries[i];
    if (!entry->key_a) return false;
    if (entry->key_a == key_a && entry->key_b == key_b) break;
  }

  for (uint32_t gap = i, j = (i + 1) & map->mask;; j = (j + 1) & map->mask) {
    ant_ptr_pair_entry_t *entry = &map->entries[j];
    if (!entry->key_a) {
      map->entries[gap] = (ant_ptr_pair_entry_t){0};
      break;
    }
    
    uint32_t home = ant_ptr_pair_hash(entry->key_a, entry->key_b) & map->mask;
    bool home_between = gap <= j ? (home > gap && home <= j) : (home > gap || home <= j);
    if (home_between) continue;
    
    map->entries[gap] = *entry;
    gap = j;
  }
  
  map->len--;
  return true;
}

void ant_ptr_pair_free(ant_ptr_pair_map_t *map) {
  free(map->entries);
  *map = (ant_ptr_pair_map_t){0};
}
