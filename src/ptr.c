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
